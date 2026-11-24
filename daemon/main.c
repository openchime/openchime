/*
 * openchimed — daemon entry point.
 *
 * Wires the skeleton together (ARCH-5, ARCH-22): a DB-writer thread that owns
 * the SQLite connection and migrates on boot, and the epoll network loop that
 * terminates TLS (ARCH-10) and serves the binary protocol, the webhook endpoint
 * and the plaintext /healthz port for the orchestrator (ARCH-25). The loop completes
 * the handshake (PROTOCOL.md §3), runs the two-mode auth handshake — local
 * passwords + session reconnect today, OIDC to follow (AUTH.md) — and serves
 * the messaging vertical. Local accounts can be provisioned at boot via
 * OPENCHIME_DEMO_USERS.
 */

#include "audio.h"
#include "build_id.h"   /* OC_BUILD_ID */
#include "certs.h"
#include "config.h"
#include "sum_fetch.h"
#include "sum_load.h"
#include "sum_worker.h"
#include "dbwriter.h"
#include "enroll.h"
#include "invite_mail.h"
#include "webhooks_out.h"
#include "address_report.h"
#include "listen.h"
#include "migrate.h"
#include "netloop.h"
#include "export_render.h"   /* oc_export_codecs_builtin (ARCH-135) */
#include "callrec_render.h"  /* oc_callrec_codecs_builtin (CALLS.md §8) */
#include "oidcrp.h"
#include "push.h"
#include "relaykeys.h"
#include "unfurl.h"
#include "tls.h"
#include "totp.h"
#include "tts_data.h"   /* the beside-the-binary data lookup: speech data and the web client's wasm */
#include "voice_pack.h"  /* --voice-pack: the voice model's data file */
#include "recognizer_pack.h"  /* --recognizer-pack: the recognizer's data file */
#include "summary_pack.h"  /* --summary-pack: the summary model's data file */
#include "webapp.h"
/* The engines' headers come in whether or not the engines do: the data
 * manifests below name each model's version, and a daemon built with
 * neither engine writes them for the arm64 build, which cannot run here. */
#include "tts.h"
#include "stt.h"
#ifdef OC_TTS
#include "tts_render.h"
#endif
#ifdef OC_STT
#include "stt_render.h"
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* The exit status of a daemon refused a database a newer one wrote (SCHEMA.md
 * "Migrations"): distinct from 1, so what started it can tell a box that must
 * go back to its newer image, or to an older copy of its data, from one that
 * failed. 3 is the image's entrypoint's: a failed restore. */
#define OC_EXIT_DATA_TOO_NEW 4

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

/* Read a whole file into a malloc'd NUL-terminated buffer, or NULL. */
static char *read_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t rd = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[rd] = '\0';
    return buf;
}

/* Write `data` to `path`, owner-read/write only (key material). Returns 0/-1. */
static int write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    size_t len = strlen(data);
    size_t wr = fwrite(data, 1, len, f);
    if (fclose(f) != 0 || wr != len) return -1;
    chmod(path, S_IRUSR | S_IWUSR);
    return 0;
}

/* Provision local accounts from OPENCHIME_DEMO_USERS="user:pass[:role],..."
 * (AUTH.md §2 — the owner bootstrap / air-gapped account setup). Idempotent:
 * re-running never clobbers an existing password. role ∈ owner|admin|member
 * (default member). Runs before the loop serves traffic, so registering through
 * the writer thread cannot race a live result consumer. */
static void bootstrap_users(oc_dbwriter *db, const char *spec) {
    if (!spec || !*spec) return;
    char *dup = strdup(spec);
    if (!dup) return;
    char *save = NULL;
    for (char *ent = strtok_r(dup, ",", &save); ent; ent = strtok_r(NULL, ",", &save)) {
        char *pass = strchr(ent, ':');
        if (!pass) continue;
        *pass++ = '\0';
        uint8_t role = OC_ROLE_MEMBER;
        char *rs = strchr(pass, ':');
        if (rs) {
            *rs++ = '\0';
            if (strcmp(rs, "owner") == 0)      role = OC_ROLE_OWNER;
            else if (strcmp(rs, "admin") == 0) role = OC_ROLE_ADMIN;
        }
        if (*ent && *pass) {
            uint64_t uid = oc_dbwriter_register_local(db, ent, pass, role, 0);
            fprintf(stderr, "openchimed: bootstrap user '%s' -> id %llu%s\n",
                    ent, (unsigned long long)uid, uid ? "" : " (FAILED)");
        }
    }
    free(dup);
}

/* --- the OpenChime services, once the binding is active (ARCH-85) --------- */

/* What runs on the enrollment: the push emitter and the invitation mail report.
 * Both start once the binding is active -- before the loop when it already was,
 * or, on a managed box's first boot, from the claim thread the moment central
 * activates the binding, which happens only once the daemon is serving. */
typedef struct {
    oc_dbwriter    *db;
    const oc_config *cfg;
    char           *privkey, *audience;   /* owned */
    const char     *ticket;               /* a managed claim still to make, or NULL */
    pthread_t       claim;
    int             claiming;
    oc_address_report *addr;              /* the address report (AUTH.md §3.6) */
    oc_push        *push;
    oc_invite_mail *invite_mail;
    /* The CA-issued certificate (TLS.md, "Certificates"): its worker, the TLS
     * state it presents into, what the database kept, and whether the binding
     * central needs is active. */
    oc_tls_server  *tls;
    oc_certs       *certs;
    oc_tls_state    kept;
    int             active;
    /* The name a self-hosted box took from central's certificate as its address
     * (TLS.md, the `central` source), "" for none, and when that certificate
     * runs out. Set at start, by the certificate worker and by the address
     * report's round, so under its own lock. */
    pthread_mutex_t adopt_mu;
    char            adopted[256];
    uint64_t        adopted_until_ms;
} services;

static uint64_t wall_ms(void) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
}

/* The names a passkey may be made for (AUTH.md §8.6): only where a CA vouches
 * for the name -- a self-signed daemon is reached through the loopback tunnel,
 * where a passkey would bind to 127.0.0.1 -- the managed address, the names it
 * is certified for, and a name adopted from central. */
static void publish_passkey_names(const oc_config *cfg, const char *adopted) {
    if (cfg->tls_src.source == OC_TLS_SRC_SELF) return;
    char names[1024];
    const char *parts[3] = { cfg->workspace_address, cfg->tls_src.names, adopted };
    size_t o = 0;
    names[0] = '\0';
    for (int i = 0; i < 3; i++) {
        if (!parts[i] || !parts[i][0]) continue;
        int w = snprintf(names + o, sizeof names - o, "%s%s", o ? "," : "", parts[i]);
        if (w < 0 || (size_t)w >= sizeof names - o) break;
        o += (size_t)w;
    }
    oc_netloop_set_passkey_names(names);
}

/* Only a self-hosted box bound to central adopts a name: a managed box's address
 * is OPENCHIME_MANAGED_ADDRESS alone, and any other source's names are its own. */
static int adopts_names(const oc_config *cfg) {
    return cfg->deployment_mode != OC_DEPLOY_MANAGED && cfg->tls_src.source == OC_TLS_SRC_CENTRAL;
}

/* Take, or drop (names NULL), the name central's certificate gives: sent to
 * clients as where the workspace is now, linked from the landing page, and
 * among the passkey names (netloop.h). */
static void set_adopted(services *f, const char *names, uint64_t until_ms) {
    char name[256] = "";
    if (names && oc_certs_adopted_name(names, name, sizeof name) != 0) name[0] = '\0';
    pthread_mutex_lock(&f->adopt_mu);
    int changed = strcmp(name, f->adopted) != 0;
    snprintf(f->adopted, sizeof f->adopted, "%s", name);
    f->adopted_until_ms = name[0] ? until_ms : 0;
    if (changed) {
        oc_netloop_set_adopted_address(name[0] ? name : NULL);
        publish_passkey_names(f->cfg, name);
        if (name[0]) fprintf(stderr, "openchimed: this workspace's address is %s (from central)\n", name);
    }
    pthread_mutex_unlock(&f->adopt_mu);
}

/* After each round of the address report (every two hours): the adopted name
 * stands while central still lists it and its certificate has not run out. */
static void check_adopted(void *ctx) {
    services *f = ctx;
    char name[256];
    uint64_t until;
    pthread_mutex_lock(&f->adopt_mu);
    snprintf(name, sizeof name, "%s", f->adopted);
    until = f->adopted_until_ms;
    pthread_mutex_unlock(&f->adopt_mu);
    if (!name[0]) return;
    if (until && wall_ms() >= until) {
        fprintf(stderr, "openchimed: the certificate for %s ran out unrenewed; no longer using it as the address\n", name);
        set_adopted(f, NULL, 0);
        return;
    }
    char names[1024], err[256] = "";
    int status = 0;
    if (oc_central_names(f->cfg->enroll.url, f->audience, f->privkey, names, sizeof names, &status, err, sizeof err) != 0)
        status = 0;
    if (oc_certs_name_listed(status, names, name) == OC_NAME_DROP) {
        fprintf(stderr, "openchimed: central no longer lists %s; no longer using it as the address\n", name);
        set_adopted(f, NULL, 0);
    }
}

static void keep_account(void *ctx, const char *key_pem, const char *url) {
    services *f = ctx;
    oc_dbwriter_store_acme_account(f->db, f->cfg->tls_src.directory, key_pem, url);
}

static void keep_cert(void *ctx, const oc_cert_issued *c) {
    services *f = ctx;
    oc_dbwriter_store_tls_cert(f->db, f->cfg->tls_src.source == OC_TLS_SRC_ACME ? "acme" : "central",
                               c->names, c->chain_pem, c->key_pem, c->not_before_ms, c->not_after_ms);
    if (adopts_names(f->cfg)) set_adopted(f, c->names, c->not_after_ms);
}

/* What the certificate worker cannot fix alone, for owners and admins (REQ-263). */
static void raise_alert(void *ctx, const char *key, const char *message) {
    services *f = ctx;
    if (message) oc_dbwriter_alert(f->db, key, message);
    else         oc_dbwriter_alert_clear(f->db, key);
}

/* Start keeping the certificate CA-issued, for the source configured. ACME is
 * answered through the listener, so this waits for the daemon to be serving;
 * central needs an active binding. */
static void start_certs(services *f) {
    const oc_config *cfg = f->cfg;
    if (f->certs || !f->tls) return;
    oc_certs_opts o;
    memset(&o, 0, sizeof o);
    o.tls = f->tls;
    o.store_cert = keep_cert;
    o.alert = raise_alert;
    o.ctx = f;
    o.issued_ms = f->kept.issued_ms;
    o.not_after_ms = f->kept.not_after_ms;
    o.chain_pem = f->kept.not_after_ms ? f->kept.chain_pem : NULL;
    if (cfg->tls_src.source == OC_TLS_SRC_ACME) {
        o.source = OC_CERTS_ACME;
        o.directory = cfg->tls_src.directory;
        o.names = cfg->tls_src.names;
        o.email = cfg->tls_src.email;
        /* The kept account, only for the CA it belongs to. */
        if (f->kept.acme_directory && !strcmp(f->kept.acme_directory, cfg->tls_src.directory)) {
            o.account_key_pem = f->kept.acme_key_pem;
            o.account_url = f->kept.acme_url;
        }
        o.store_account = keep_account;
    } else if (cfg->tls_src.source == OC_TLS_SRC_CENTRAL) {
        if (!f->active || !f->privkey || !f->audience) return;
        o.source = OC_CERTS_CENTRAL;
        o.central_url = cfg->enroll.url;
        o.audience = f->audience;
        o.enroll_key_pem = f->privkey;
    } else {
        return;
    }
    f->certs = oc_certs_start(&o);
    if (!f->certs) fprintf(stderr, "openchimed: the certificate worker failed to start\n");
}

static void start_services(services *f) {
    const oc_config *cfg = f->cfg;
    /* The address proof (AUTH.md §3.6): central's check of the address this
     * workspace reports is answered with the enrollment key, once it is active. */
    oc_netloop_set_enrollment(f->audience, f->privkey);
    /* Push (ARCH-85): on with OPENCHIME_PUSH=on, at the connection's origin, or
     * at OPENCHIME_PUSH_URL. It signs with the enrollment key and delivers
     * offline mobile notifications; off unless this box turns push on. */
    if (cfg->push.url && *cfg->push.url) {
        f->push = oc_push_start(cfg->db_path, f->db, cfg->push.url, f->audience, f->privkey);
        if (f->push) {
            oc_netloop_set_push(f->push);
            fprintf(stderr, "openchimed: push emitter enabled (audience=%s)\n", f->audience);
        } else {
            fprintf(stderr, "openchimed: push emitter failed to start\n");
        }
    }
    /* Invitation mail (REQ-280): each invite bound to an address is reported to
     * central at the origin the box enrolled with. */
    if (cfg->invite_mail) {
        f->invite_mail = oc_invite_mail_start(cfg->enroll.url, f->audience, f->privkey);
        if (f->invite_mail) {
            oc_netloop_set_invite_mail(f->invite_mail);
            fprintf(stderr, "openchimed: invitation mail on (audience=%s)\n", f->audience);
        } else {
            fprintf(stderr, "openchimed: invitation mail failed to start\n");
        }
    }
}

/* Sleep up to `secs`, returning early (non-zero) once shutdown is asked for. */
static int nap(unsigned secs) {
    for (unsigned i = 0; i < secs * 10 && !g_stop; i++) usleep(100000);
    return g_stop != 0;
}

/* A managed box's claim (AUTH.md §8.7), made once the daemon is serving: central
 * reads an activated binding as a workspace that is up, so the claim waits until
 * that is true. Bounded in time -- central may still be coming up, or briefly
 * busy -- and a refused ticket is not retried, since it will be refused again. */
static void *claim_thread(void *arg) {
    services *f = arg;
    const oc_config *cfg = f->cfg;
    time_t deadline = time(NULL) + OC_CLAIM_WAIT_SECS;
    unsigned pause = 2;
    while (!g_stop) {
        oc_enroll_result er = oc_enroll_claim(cfg->enroll.url, f->audience, f->privkey,
                                              f->ticket);
        if (er == OC_ENROLL_ACTIVE) {
            oc_dbwriter_note_enrollment_active(f->db, f->privkey, f->audience);
            fprintf(stderr, "openchimed: binding claimed (audience=%s)\n", f->audience);
            start_services(f);
            f->active = 1;
            start_certs(f);
            break;
        }
        if (er == OC_ENROLL_FAILED) {
            fprintf(stderr, "openchimed: the enrollment ticket was refused; this box is "
                            "not bound and nobody can sign in through the relay\n");
            break;
        }
        if (time(NULL) >= deadline) {
            fprintf(stderr, "openchimed: could not reach central to claim the binding; "
                            "retrying on next boot\n");
            break;
        }
        if (nap(pause)) break;
        if (pause < 16) pause *= 2;
    }
    return NULL;
}

/* The address a self-hosted workspace answers at, reported to central once it
 * serves, at each start and every two hours after (address_report.h, AUTH.md
 * §3.6): its DNS name, or {} with none, when central proves the address the
 * report came from. A managed box reports none: its address is central's own. */
static void start_address_report(services *f) {
    const oc_config *cfg = f->cfg;
    char body[340];
    if (oc_enroll_address_body(cfg->workspace_address,
                               cfg->tls_src.source == OC_TLS_SRC_ACME ? cfg->tls_src.names : NULL,
                               cfg->proto_port, body, sizeof body) != 0) {
        fprintf(stderr, "openchimed: this server's name is not a usable DNS name; no address is reported to central\n");
        return;
    }
    f->addr = oc_address_report_start(cfg->enroll.url, f->audience, f->privkey, body, NULL,
                                      adopts_names(cfg) ? check_adopted : NULL, f);
    if (!f->addr) fprintf(stderr, "openchimed: could not start the address report\n");
}

/* The net loop's ready hook: the listener takes connections, so a certificate
 * can be validated through it, a claim made, and the address checked. */
static void on_serving(void *ctx) {
    services *f = ctx;
    start_certs(f);
    if (f->active && !f->addr && !(f->cfg->enroll.ticket && *f->cfg->enroll.ticket)) start_address_report(f);
    if (!f->ticket || f->claiming) return;
    if (pthread_create(&f->claim, NULL, claim_thread, f) == 0) f->claiming = 1;
    else fprintf(stderr, "openchimed: could not start the enrollment claim\n");
}

/* --- summaries (REQ-310, ARCH-116) ---------------------------------------------
 * Brought up on a thread of their own, after the daemon is serving: the first
 * start with summaries on fetches the model (about 1.3 GB), and every start
 * checks the CPU can run it and loads it before anyone is told summaries exist.
 * Anything that fails leaves summaries off, with a line saying why. */
typedef struct {
    pthread_t      thread;
    int            started;
    oc_dbwriter   *db;
    const char    *db_path;
    const oc_config *cfg;          /* the hosted model's API, name and key */
    oc_sum_worker *worker;
} sum_up;

static int sum_sink_store(void *ctx, const oc_sum_answer *a, oc_sum_new *nodes, int n) {
    return oc_dbwriter_sum_store(((sum_up *)ctx)->db, a, nodes, n);
}

static int sum_sink_take(void *ctx, int64_t row) {
    return oc_dbwriter_sum_take(((sum_up *)ctx)->db, row);
}

static int sum_net_bytes(uint64_t *bytes) {
    oc_netloop_stats st;
    oc_netloop_stats_get(&st);
    *bytes = st.bytes_read;
    return 0;
}

/* Summaries stay off: why, in the log, and the loop told, so what waited for
 * them is answered. */
static void *sum_off(const char *fmt, const char *why) {
    fprintf(stderr, fmt, why);
    oc_netloop_summary_failed();
    return NULL;
}

/* The worker over `e`, and the loop told summaries exist. */
static void *sum_start(sum_up *u, const oc_sum_engine *e, const char *what) {
    char err[300] = "";
    oc_sum_probe_set_net(sum_net_bytes);
    oc_sum_worker_cfg cfg;
    memset(&cfg, 0, sizeof cfg);
    cfg.db_path = u->db_path;
    cfg.engine = e;
    cfg.sink.store = sum_sink_store;
    cfg.sink.take = sum_sink_take;
    cfg.sink.ctx = u;
    cfg.background = 1;
    u->worker = oc_sum_worker_start(&cfg, err, sizeof err);
    if (!u->worker) return sum_off("openchimed: summaries are off: %s\n", err);
    oc_netloop_set_summary(u->worker);
    fprintf(stderr, "openchimed: summaries on (%s)\n", what);
    return NULL;
}

#ifdef OC_SUM
/* Asked between the conversion's tensors: waits while the machine is busy;
 * non-zero once the daemon is stopping. */
static int sum_convert_pause(void *ctx) {
    oc_sum_load *gate = ctx;
    while (!g_stop && oc_sum_load_busy(gate)) usleep(1000000);
    return g_stop != 0;
}
#endif

static void *sum_bring_up(void *arg) {
    sum_up *u = arg;
    char err[300] = "";
    const oc_sum_engine *e;
    char what[400];
    if (u->cfg->summary.mode == OC_SUMMARY_CLOUD) {
        /* A hosted model: it must answer before anyone is told summaries exist
         * (opening asks it a one-line prompt). */
        e = oc_sum_cloud_engine(u->cfg->summary.url, u->cfg->summary.model, u->cfg->summary.api_key);
        if (!e->open(e->ctx, err, sizeof err))
            return sum_off("openchimed: summaries are off: the hosted model did not answer: %s\n", err);
        snprintf(what, sizeof what, "hosted model %s at %s", u->cfg->summary.model, u->cfg->summary.url);
        return sum_start(u, e, what);
    }
#ifdef OC_SUM
    char dir[1024], path[1200];
    /* Fetching and converting the model is idle-time work, as summarizing is:
     * this thread at idle priority, the conversion paused while the machine
     * is busy. */
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sched_setscheduler(0, SCHED_IDLE, &sp);
    setpriority(PRIO_PROCESS, (id_t)syscall(SYS_gettid), 19);
    oc_sum_load gate;
    oc_sum_load_init(&gate, oc_sum_probe_system());
    oc_sum_model_dir(u->db_path, dir, sizeof dir);
    if (oc_sum_model_ensure(dir, &OC_SUM_SOURCE, (const volatile int *)&g_stop, sum_convert_pause, &gate, path,
                            sizeof path, err, sizeof err) != 0)
        return g_stop ? NULL : sum_off("openchimed: summaries are off: %s\n", err);
    e = oc_sum_local_engine(path, OC_SUM_MODEL_NAME, SUM_CTX_TOKENS);
    /* The model must load; the worker loads it again when first needed. */
    void *h = g_stop ? NULL : e->open(e->ctx, err, sizeof err);
    if (!h) {
        if (g_stop) return NULL;
        return sum_off("openchimed: summaries are off: the model did not load: %s\n", err);
    }
    e->close(h);
    snprintf(what, sizeof what, "local model %s", OC_SUM_MODEL_NAME);
    return sum_start(u, e, what);
#else
    (void)e;
    (void)what;
    return sum_off("openchimed: summaries are off: %s\n", "this daemon was built without the local model (SUM=0)");
#endif
}

int main(int argc, char **argv) {
    /* The only argument the daemon takes. Everything else is configuration, and
     * configuration comes from the environment (ARCH-26) — so this is a version
     * probe, not the beginning of a command-line interface. */
#ifdef OC_TTS
    /* Read-aloud's check by ear (tts.h): not configuration, a diagnostic that
     * renders one text with the voice model built into this binary. */
    if (argc == 5 && strcmp(argv[1], "--tts-say") == 0)
        return oc_tts_say(argv[2], argv[3], argv[4]);
#endif
#ifdef OC_STT
    /* Voice input's check (stt.h): recognize one recording with the model in
     * this binary. */
    if (argc == 3 && strcmp(argv[1], "--stt-hear") == 0)
        return oc_stt_hear(argv[2]);
#endif
    /* The data manifests (tts_data.h): the version each engine is compiled
     * for over the files beside the binary. Written by whichever build runs
     * on the packaging machine, engines or not, since the version is a
     * constant of the sources and the hashes are of the data. */
    if (argc == 3 && strcmp(argv[1], "--tts-manifest") == 0)
        return oc_tts_data_write_manifest_cli(argv[2], OC_TTS_MODEL_VERSION, OC_TTS_DATA_FILES);
    if (argc == 3 && strcmp(argv[1], "--stt-manifest") == 0)
        return oc_tts_data_write_manifest_cli(argv[2], OC_STT_MODEL_VERSION, OC_STT_DATA_FILES);
    /* The voice model's data file (voice_pack.h), from the model as it is
     * published: in every build, read-aloud or not, since it reads and writes
     * files and runs no model. */
    if (argc >= 2 && strcmp(argv[1], "--voice-pack") == 0) {
        if (argc != 5) { fprintf(stderr, "usage: openchimed --voice-pack MODEL.onnx VOICES.npz OUT\n"); return 2; }
        return oc_voice_pack_cli(argv[2], argv[3], argv[4]);
    }
    /* The recognizer's data file (recognizer_pack.h), from the model as it is
     * published, and the listing of its graphs: in every build, voice input or
     * not, for the same reason. */
    if (argc >= 2 && strcmp(argv[1], "--recognizer-pack") == 0) return oc_recognizer_pack_cli(argc, argv);
    /* The summary model's data file (summary_pack.h), from Qwen's published
     * files, quantised or (--full) not: in every build, summaries or not. */
    if (argc >= 2 && strcmp(argv[1], "--summary-pack") == 0) return oc_summary_pack_cli(argc, argv);
    /* The registry's views (config.h): the reference CONFIG.md carries, the
     * packaged environment file, and a check of this environment -- or of a
     * file, applied over it -- through the same loader the daemon starts with. */
    if (argc == 2 && strcmp(argv[1], "--config-reference") == 0) { oc_config_print_reference(stdout); return 0; }
    if (argc == 2 && strcmp(argv[1], "--config-env") == 0) { oc_config_print_env_template(stdout); return 0; }
    if (argc >= 2 && strcmp(argv[1], "--check-config") == 0) {
        char cfgerr[512];
        if (argc == 4 && strcmp(argv[2], "--env-file") == 0) {
            if (oc_config_apply_env_file(argv[3], cfgerr, sizeof cfgerr) != 0) {
                fprintf(stderr, "openchimed: %s\n", cfgerr);
                return 1;
            }
        } else if (argc != 2) {
            fprintf(stderr, "usage: openchimed --check-config [--env-file PATH]\n");
            return 2;
        }
        if (oc_config_load(cfgerr, sizeof cfgerr) != 0) { fprintf(stderr, "openchimed: config error: %s\n", cfgerr); return 1; }
        oc_config_report(stdout);
        printf("openchimed: the configuration is complete; the daemon would start in %s mode\n",
               oc_mode_name(oc_config_get()->mode));
        return 0;
    }
    /* Whether this daemon would run on the database at PATH, read-only and
     * writing nothing: the image's entrypoint asks before Litestream starts,
     * since Litestream exits 1 for any failure of the daemon it runs and the
     * refusal's own status would be lost (SCHEMA.md §1). No database
     * there is one this daemon would begin. */
    if (argc >= 2 && strcmp(argv[1], "--check-data") == 0) {
        if (argc != 3) { fprintf(stderr, "usage: openchimed --check-data PATH\n"); return 2; }
        if (access(argv[2], F_OK) != 0) return 0;
        sqlite3 *cdb = NULL;
        if (sqlite3_open_v2(argv[2], &cdb, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
            fprintf(stderr, "openchimed: %s: cannot open: %s\n", argv[2], cdb ? sqlite3_errmsg(cdb) : "out of memory");
            sqlite3_close(cdb);
            return 1;
        }
        int have = oc_schema_version(cdb), newest = OC_MIGRATIONS[OC_MIGRATIONS_COUNT - 1].version;
        sqlite3_close(cdb);
        if (have > newest) {
            fprintf(stderr, "openchimed: %s was written by a newer daemon (schema %d; this one knows up to %d); "
                            "run that one, or restore a copy this one wrote\n", argv[2], have, newest);
            return OC_EXIT_DATA_TOO_NEW;
        }
        return 0;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("openchimed %s (protocol %u)\n", OC_BUILD_ID, (unsigned)OC_PROTOCOL_VERSION);
            return 0;
        }
        fprintf(stderr, "openchimed: unknown argument '%s'\n", argv[i]);
        fprintf(stderr, "usage: openchimed [--version | --check-config [--env-file PATH] | --check-data PATH | --config-reference | --config-env]\n");
        fprintf(stderr, "configuration is read from the environment; see CONFIG.md\n");
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN); /* a peer vanishing mid-write must not kill us */

    fprintf(stderr, "openchimed: build %s (protocol %u)\n", OC_BUILD_ID, (unsigned)OC_PROTOCOL_VERSION);

    /* All daemon config is read once, here, into the process-global oc_config
     * (config.c). Subsystems read it via oc_config_get(); nothing else touches
     * the environment. A fatal misconfiguration aborts before we open anything. */
    char cfgerr[256];
    if (oc_config_load(cfgerr, sizeof cfgerr) != 0) {
        fprintf(stderr, "openchimed: config error: %s\n", cfgerr);
        return 1;
    }
    const oc_config *cfg = oc_config_get();
    if (cfg->mode == OC_MODE_DEVELOPMENT)
        fprintf(stderr, "openchimed: DEVELOPMENT MODE: the OPENCHIME_DEV_* inputs are read; for a box of your own, never a deployment\n");
    else if (cfg->mode == OC_MODE_DEMO)
        fprintf(stderr, "openchimed: demo mode: the public demo workspace\n");

#ifdef OC_TTS
    /* Read-aloud's engine: the voice model built into this binary (ARCH-111).
     * Whether it is used at all is the operator's, through OPENCHIME_TTS.
     * Necessarily after the config is loaded. */
    {
        const oc_tts_engine *tts_engine = oc_tts_engine_for(OC_TTS_LANG);
        if (!tts_engine && cfg->tts.enabled) {
            fprintf(stderr, "openchimed: no read-aloud voice for %s in this binary\n", OC_TTS_LANG);
            return 2;
        }
        /* The voice data is files beside the daemon, not bytes inside it, so it
         * can be absent -- not installed, or from another build. That is not a
         * reason to refuse to start: read-aloud is simply off, clients are told so
         * and show nothing of it, and the log says why. */
        char why[256] = "";
        if (tts_engine && cfg->tts.enabled && !oc_tts_data_ready(why, sizeof why)) {
            fprintf(stderr, "openchimed: read-aloud is off: %s\n", why);
            tts_engine = NULL;
        }
        oc_netloop_set_tts(tts_engine);
        oc_netloop_set_export(oc_export_codecs_builtin());   /* NULL unless built with EXPORT=1 */
    }
#endif
    /* The codecs a call's recording is made with (CALLS.md §8): NULL unless
     * built with CALLREC=1, and then nobody records a call here. */
    oc_netloop_set_callrec(oc_callrec_codecs_builtin());
#ifdef OC_STT
    /* Voice input's engine (ARCH-112): the recognizer built into this binary,
     * its data beside it in a directory of its own. Absent data turns voice input
     * off -- clients are told and show nothing -- and nothing else. */
    {
        const oc_stt_engine *stt_engine = cfg->stt.enabled ? oc_stt_engine_for(OC_STT_LANG) : NULL;
        char why[256] = "";
        if (stt_engine && !oc_stt_data_ready(why, sizeof why)) {
            fprintf(stderr, "openchimed: voice input is off: %s\n", why);
            stt_engine = NULL;
        }
        oc_netloop_set_stt(stt_engine);
    }
#endif
    const char *db_path = cfg->db_path, *cert_path = cfg->tls_cert, *key_path = cfg->tls_key;
    int health_port = cfg->health_port > 0 ? cfg->health_port : -1, proto_port = cfg->proto_port;
    fprintf(stderr, "openchimed: deployment=%s workspace=\"%s\"\n",
            oc_deploy_mode_name(cfg->deployment_mode),
            cfg->workspace_name && cfg->workspace_name[0] ? cfg->workspace_name : "(unset)");

    /* The data directory (CONFIG.md): made if absent, one level, as its
     * contents are; its parent is the deployment's. */
    if (mkdir(cfg->data_dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "openchimed: OPENCHIME_DATA_DIR=%s: cannot create: %s\n", cfg->data_dir, strerror(errno));
        return 1;
    }

    /* DB-writer thread: opens the connection and applies migrations before we
     * serve any traffic (ARCH-27). Fatal if it can't. */
    /* The identity (ARCH-66b) is a database of its own in identity/ beside the
     * data: replicated apart from it, so the data can be begun again or
     * restored while the certificate, the enrollment and the keys stay. */
    char identity_dir[1100], identity_path[1200];
    snprintf(identity_dir, sizeof identity_dir, "%s/identity", cfg->data_dir);
    snprintf(identity_path, sizeof identity_path, "%s/identity.db", identity_dir);
    if (mkdir(identity_dir, 0700) != 0 && errno != EEXIST) {
        fprintf(stderr, "openchimed: %s: cannot create: %s\n", identity_dir, strerror(errno));
        return 1;
    }
    oc_dbwriter_boot boot = { .identity_path = identity_path,
                              .factor_key_file = cfg->auth_local ? cfg->factor_key_file : NULL };
    oc_dbwriter *db = oc_dbwriter_start_boot(db_path, &boot);
    if (!db && boot.too_new) {
        fprintf(stderr, "openchimed: the database was written by a newer daemon; run that one, or restore a "
                        "copy this one wrote\n");
        return OC_EXIT_DATA_TOO_NEW;
    }
    if (!db) { fprintf(stderr, "openchimed: DB init failed\n"); return 1; }

    /* Registered-user cap (AUTH.md §2): 0/unset = unlimited
     * (self-hosted). */
    oc_dbwriter_set_max_users(db, cfg->max_users);

    /* A managed workspace's first boot opens with a welcome in #general: its
     * topic and description, nothing authored. Before the accounts below, which
     * would otherwise create the channel bare; a later boot finds it made. */
    /* A demo workspace (AUTH.md §8.12): its rules, and at first start its
     * sample content -- before the welcome, so #general says what the demo is. */
    if (cfg->mode == OC_MODE_DEMO) {
        oc_dbwriter_set_demo(db, 1);
        if (oc_dbwriter_seed_demo(db, cfg->workspace_name))
            fprintf(stderr, "openchimed: demo mode: the sample channels and messages written\n");
    }
    if (oc_dbwriter_welcome_general(db, (int)cfg->deployment_mode, cfg->workspace_name))
        fprintf(stderr, "openchimed: #general created with its welcome\n");

    /* Optionally provision local accounts before serving (AUTH.md §2). */
    bootstrap_users(db, cfg->dev.users);

    /* Enrollment, the connection to the OpenChime services (ARCH-84, AUTH.md
     * §3.6). Gated on OPENCHIME_ENROLL_URL — the
     * operator opting this box into central. On first boot: generate a
     * keypair + opaque audience, persist them, and print an enrollment code to
     * enter on central's enrollment page. Then (if pending) call out to central to prove
     * possession and activate. The daemon-generated audience feeds OIDC below. */
    char *enroll_privkey = NULL, *enroll_audience = NULL;
    int enroll_active = 0;
    const char *enroll_url = cfg->enroll.url;
    /* A managed box (AUTH.md §8.7): started with OPENCHIME_MANAGED_AUDIENCE and
     * a one-time ticket. Exactly one party mints an audience, so
     * this path generates a key and nothing else — and a stored audience that is
     * not the one it was started with stops the boot rather than being replaced.
     * The claim itself waits until the daemon is serving (on_serving). */
    const char *ticket = cfg->enroll.ticket;
    int managed_claim = enroll_url && *enroll_url && ticket && *ticket &&
                        cfg->oidc.audience && *cfg->oidc.audience;
    if (managed_claim) {
        if (oc_dbwriter_load_enrollment(db, &enroll_privkey, &enroll_audience, &enroll_active)) {
            if (strcmp(enroll_audience, cfg->oidc.audience) != 0) {
                fprintf(stderr, "openchimed: this box holds audience %s but was started with %s; "
                                "refusing to run as two workspaces\n",
                        enroll_audience, cfg->oidc.audience);
                oc_dbwriter_stop(db); return 1;
            }
        } else {
            char pk[1024], unused[128];
            if (oc_enroll_generate(pk, sizeof pk, unused, sizeof unused) != 0 ||
                !oc_dbwriter_store_enrollment(db, pk, cfg->oidc.audience, 0)) {
                fprintf(stderr, "openchimed: enrollment key generation failed\n");
                oc_dbwriter_stop(db); return 1;
            }
            enroll_privkey = strdup(pk);
            enroll_audience = strdup(cfg->oidc.audience);
        }
    } else if (enroll_url && *enroll_url) {
        if (!oc_dbwriter_load_enrollment(db, &enroll_privkey, &enroll_audience, &enroll_active)) {
            char pk[1024], aud[128], code[2048];
            if (oc_enroll_generate(pk, sizeof pk, aud, sizeof aud) == 0 &&
                oc_dbwriter_store_enrollment(db, pk, aud, 0)) {
                enroll_privkey = strdup(pk);
                enroll_audience = strdup(aud);
                if (oc_enroll_build_code(pk, aud, code, sizeof code) == 0) {
                    fprintf(stderr, "openchimed: enrollment code (enter it on central's "
                                    "enrollment page): %s\n", code);
                }
            } else {
                fprintf(stderr, "openchimed: enrollment key generation failed\n");
            }
        }
        /* Try to activate, once: the operator enters the code on central's enrollment page
         * and the next boot finds it claimed. */
        if (enroll_audience && enroll_privkey && !enroll_active) {
            oc_enroll_result er = oc_enroll_activate(enroll_url, enroll_audience, enroll_privkey);
            if (er == OC_ENROLL_ACTIVE) {
                oc_dbwriter_store_enrollment(db, enroll_privkey, enroll_audience, 1);
                enroll_active = 1;
                fprintf(stderr, "openchimed: enrollment activated (audience=%s)\n", enroll_audience);
            } else if (er == OC_ENROLL_PENDING) {
                fprintf(stderr, "openchimed: enrollment pending: enter the code on "
                                "central's enrollment page; activation retries on next boot\n");
            } else {
                fprintf(stderr, "openchimed: enrollment activation failed (central unreachable?)\n");
            }
        }
    }

    /* Which built-in sources are on (AUTH.md §8.8): OPENCHIME_AUTH, parsed by
     * the loader. */
    int want_local = cfg->auth_local, want_relay = cfg->auth_relay;
    oc_dbwriter_set_local_enabled(db, want_local);
    oc_dbwriter_set_local_mfa(db, cfg->local_mfa);
    /* Passkeys only where a CA vouches for the name (publish_passkey_names);
     * a name adopted from central joins them when it is taken, below. */
    publish_passkey_names(cfg, NULL);
    /* A demo's visitors sign in for a day (AUTH.md §8.12); everyone else for
     * the constant's thirty. */
    oc_dbwriter_set_session_policy(db, cfg->mode == OC_MODE_DEMO ? OC_DEMO_SESSION_TTL_MS : OC_SESSION_TTL_MS,
                                   OC_SESSION_IDLE_MS);

    /* The factor key (AUTH.md §8.6), settled as the store started: kept in the
     * identity, apart from the data, so a copy of the data alone gives away no
     * second step. If it cannot be had, the daemon serves on and no second step
     * can be passed -- refusing those sign-ins rather than passing them. */
    if (want_local) {
        if (boot.factor == 1) fprintf(stderr, "openchimed: made the factor key, kept in %s\n", identity_path);
        else if (boot.factor == 2) fprintf(stderr, "openchimed: took the factor key %s into %s\n",
                                           cfg->factor_key_file, identity_path);
        else if (boot.factor < 0) {
            oc_dbwriter_set_factor_key(db, NULL);
            fprintf(stderr, "openchimed: no factor key (%s): local accounts with a second step cannot sign in\n",
                    boot.factor_why);
        }
    }

    /* The relay source (AUTH.md §3): the central service this box is enrolled
     * with. Its issuer is the origin it enrolled at, its audience the
     * enrollment's, and its keys the ones it publishes at /oidc/jwks, read at
     * boot and daily (§3.3); a managed box may hold one pinned beneath them. */
    oc_relaykeys *relay_keys = NULL;
    if (want_relay) {
        char origin[256] = "";
        {
            const char *scheme = strstr(cfg->enroll.url, "://");
            const char *path = scheme ? strchr(scheme + 3, '/') : NULL;
            size_t n = path ? (size_t)(path - cfg->enroll.url) : strlen(cfg->enroll.url);
            if (scheme && n < sizeof origin) { memcpy(origin, cfg->enroll.url, n); origin[n] = '\0'; }
        }
        const char *aud = enroll_audience;
        if (!origin[0] || !aud) {
            fprintf(stderr, "openchimed: OPENCHIME_AUTH=relay: this box has no enrollment to sign in through\n");
            oc_dbwriter_stop(db); return 1;
        }
        if (oc_dbwriter_configure_oidc(db, origin, aud, cfg->oidc.pubkey, origin) != 0) {
            fprintf(stderr, "openchimed: OIDC configuration failed\n");
            oc_dbwriter_stop(db); return 1;
        }
        char why[256];
        if (oc_dbwriter_configure_join_rules(db, cfg->oidc.allow, why, sizeof why) != 0) {
            fprintf(stderr, "openchimed: OPENCHIME_JOIN: %s\n", why);
            oc_dbwriter_stop(db); return 1;
        }
        oc_dbwriter_set_email_link(db, 0);
        if (!cfg->oidc.allow || !cfg->oidc.allow[0])
            fprintf(stderr, "openchimed: OPENCHIME_JOIN is unset: nobody new may join "
                            "by signing in except through an invite\n");
        fprintf(stderr, "openchimed: OIDC mode (issuer=%s audience=%s)\n", origin, aud);
        char url[300];
        snprintf(url, sizeof url, "%s/oidc/jwks", origin);
        relay_keys = oc_relaykeys_start(url, db);
        if (!relay_keys) fprintf(stderr, "openchimed: the relay's published keys will not be read\n");
    }

    /* Direct connections (AUTH.md §8.5): the operator's own providers, each a
     * source. Who may join is the same rule set the relay's sign-ins answer to,
     * read here too when there is no relay. */
    oc_oidcrp *rp = NULL;
    if (cfg->n_connect > 0) {
        if (!want_relay) {
            char why[256];
            if (oc_dbwriter_configure_join_rules(db, cfg->oidc.allow, why, sizeof why) != 0) {
                fprintf(stderr, "openchimed: OPENCHIME_JOIN: %s\n", why);
                oc_dbwriter_stop(db); return 1;
            }
        }
        const char *iss[OC_OIDC_MAX_CONNECT];
        for (int i = 0; i < cfg->n_connect; i++) iss[i] = cfg->connect[i].issuer;
        oc_dbwriter_set_direct_issuers(db, iss, cfg->n_connect);
        rp = oc_oidcrp_start(cfg->connect, cfg->n_connect, db);
        if (!rp) { fprintf(stderr, "openchimed: cannot start: the sign-in provider worker\n"); oc_dbwriter_stop(db); return 1; }
        oc_netloop_set_direct(rp, cfg->connect, cfg->n_connect);
        for (int i = 0; i < cfg->n_connect; i++)
            fprintf(stderr, "openchimed: sign-in with %s (%s) as oidc-%d\n", cfg->connect[i].label,
                    cfg->connect[i].issuer, i + 1);
    }

    /* The OpenChime services on the enrollment (ARCH-85): started now when the
     * binding is already active, or by the claim once the daemon serves. A box
     * that is not enrolled has neither. */
    services svc;
    memset(&svc, 0, sizeof svc);
    pthread_mutex_init(&svc.adopt_mu, NULL);
    svc.db = db;
    svc.cfg = cfg;
    svc.privkey = enroll_privkey;
    svc.audience = enroll_audience;
    enroll_privkey = enroll_audience = NULL;   /* svc owns them now */
    svc.active = enroll_active;
    /* A certificate through central needs a binding to ask with. */
    if (cfg->tls_src.source == OC_TLS_SRC_CENTRAL && !(svc.privkey && svc.audience)) {
        fprintf(stderr, "openchimed: OPENCHIME_TLS=central needs an enrollment "
                        "(OPENCHIME_ENROLL_URL); refusing to start without one\n");
        oc_dbwriter_stop(db); return 1;
    }
    if (svc.privkey && svc.audience) {
        if (enroll_active) start_services(&svc);
        else if (managed_claim) svc.ticket = ticket;
    }
    if (cfg->invite_mail && !enroll_active && !svc.ticket)
        fprintf(stderr, "openchimed: OPENCHIME_INVITE_MAIL is on, but this workspace has no "
                        "active enrollment; invitations are shared by copying them\n");
    oc_netloop_set_ready(on_serving, &svc);

    /* Link unfurls (REQ-222, ARCH-105): always on, no switch. The worker
     * fetches previews off the hot path; its SSRF gate is what makes
     * user-supplied destinations safe to dial at all. */
    oc_unfurler *unfurler = oc_unfurler_start(db, 0);
    if (unfurler) oc_netloop_set_unfurler(unfurler);
    else fprintf(stderr, "openchimed: unfurl worker failed to start\n");

    /* First-run bootstrap (REQ-024, local mode only): if there is no owner yet
     * and none was provisioned via OPENCHIME_DEMO_USERS, mint a one-time owner
     * setup token and print it once: the operator redeems it on the sign-up
     * page to create the first owner, no pre-existing admin needed. */
    if (oc_dbwriter_auth_methods(db) & OC_AUTH_LOCAL) {
        uint8_t stok[OC_INVITE_TOKEN_LEN];
        if (oc_dbwriter_setup_invite(db, stok)) {
            fprintf(stderr, "openchimed: no owner yet: first-run setup token "
                            "(the invitation that creates the owner, on the sign-up page): ");
            for (size_t i = 0; i < OC_INVITE_TOKEN_LEN; i++) fprintf(stderr, "%02x", stok[i]);
            fprintf(stderr, "\n");
        }
    }

    /* TLS identity (ARCH-10). A persisted cert+key in the DB (ARCH-66b) is
     * restored to the cert/key files first, so a database moved or restored onto
     * a new box keeps the same fingerprint instead of generating a new,
     * pin-breaking one. If none is stored, the first-run generation below
     * creates one and we persist it. */
    char *stored_cert = NULL, *stored_key = NULL;
    /* The operator's own certificate (OPENCHIME_TLS=file) is theirs:
     * neither overwritten from the database nor kept in it. */
    int own_files = cfg->tls_src.source == OC_TLS_SRC_FILE;
    int had_identity = !own_files && oc_dbwriter_load_identity(db, &stored_cert, &stored_key);
    if (had_identity &&
        (write_file(cert_path, stored_cert) != 0 || write_file(key_path, stored_key) != 0)) {
        fprintf(stderr, "openchimed: warning: could not restore TLS identity to disk\n");
    }
    free(stored_cert); free(stored_key);

    /* Self-signed cert on first run, reused thereafter (ARCH-10). */
    oc_tls_server tls;
    if (oc_tls_server_init(&tls, cert_path, key_path) != 0) {
        fprintf(stderr, "openchimed: TLS init failed\n");
        oc_dbwriter_stop(db);
        return 1;
    }

    /* First run (nothing was persisted): capture the just-generated (or
     * pre-existing on-disk) identity into the DB so a later restore reloads it. */
    if (!had_identity && !own_files) {
        char *cert = read_file(cert_path), *key = read_file(key_path);
        if (cert && key && !oc_dbwriter_store_identity(db, cert, key))
            fprintf(stderr, "openchimed: warning: could not persist TLS identity\n");
        free(cert); free(key);
    }

    /* A CA-issued certificate kept from before (migration 0049) is presented at
     * once, if it is from the source now configured, for the names now
     * configured, and not run out; the worker renews it when due. Otherwise the
     * identity above is presented until the worker obtains one. */
    svc.tls = &tls;
    oc_dbwriter_load_tls_state(db, &svc.kept);
    {
        const oc_tls_state *k = &svc.kept;
        const char *want = cfg->tls_src.source == OC_TLS_SRC_ACME ? "acme"
                         : cfg->tls_src.source == OC_TLS_SRC_CENTRAL ? "central" : NULL;
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        uint64_t now = (uint64_t)ts.tv_sec * 1000u;
        int usable = want && k->source && !strcmp(k->source, want) && k->chain_pem && k->key_pem &&
                     k->not_after_ms > now &&
                     (cfg->tls_src.source != OC_TLS_SRC_ACME || (k->names && !strcmp(k->names, cfg->tls_src.names))) &&
                     /* Moved to a new address: the kept certificate does not name it,
                      * so central is asked for one that does (both names, during the move). */
                     (cfg->tls_src.source != OC_TLS_SRC_CENTRAL || !cfg->workspace_address[0] ||
                      oc_certs_names_include(k->names, cfg->workspace_address));
        if (usable && oc_tls_server_use(&tls, k->chain_pem, strlen(k->chain_pem), k->key_pem, strlen(k->key_pem)) == 0) {
            fprintf(stderr, "openchimed: TLS certificate for %s (kept)\n", k->names);
            if (adopts_names(cfg)) set_adopted(&svc, k->names, k->not_after_ms);
        } else {
            svc.kept.issued_ms = svc.kept.not_after_ms = 0;    /* nothing to renew: obtain one */
        }
    }

    /* /healthz and the landing page (ARCH-25): served by the loop's HTTP stack
     * on the I/O threads, only while it serves. */
    oc_netloop_set_health_port(health_port);

    /* Call media (REQ-150/151, ARCH-18/73): bind the relay's UDP socket here, so
     * the port to advertise is known, and hand it to the net loop, whose relay
     * runs on it. A socket that cannot be bound leaves calls refused. */
    int audio_udp = -1;
    {
        int uport = cfg->proto_port;   /* the same number as TLS, on UDP (AUDIO.md §4) */
        const char *op = "socket";
        audio_udp = oc_listen_bind(SOCK_DGRAM, uport, cfg->mode == OC_MODE_DEVELOPMENT && cfg->dev.loopback, &op);
        if (audio_udp >= 0) {
            struct sockaddr_storage ua; socklen_t sl = sizeof ua;
            getsockname(audio_udp, (struct sockaddr *)&ua, &sl);
            unsigned aport = ntohs(ua.ss_family == AF_INET6 ? ((struct sockaddr_in6 *)&ua)->sin6_port
                                                           : ((struct sockaddr_in *)&ua)->sin_port);
            oc_netloop_set_audio(audio_udp, (uint16_t)aport);
            fprintf(stderr, "openchimed: call relay on UDP :%u\n", aport);
        } else {
            fprintf(stderr, "openchimed: call relay %s on UDP :%d: %s; calls are off\n",
                    op, uport, strerror(errno));
        }
    }

    /* Summaries, when the operator turned them on (REQ-310): brought up beside
     * the loop, which serves meanwhile. */
    sum_up sumu;
    memset(&sumu, 0, sizeof sumu);
    sumu.db = db;
    sumu.db_path = db_path;
    sumu.cfg = cfg;
    if (cfg->summary.mode != OC_SUMMARY_OFF) {
        /* Offered from the start: a request made while the model loads waits
         * in the queue for it. A build without the local model has nothing to
         * bring up for it, and offers nothing. */
#ifndef OC_SUM
        if (cfg->summary.mode == OC_SUMMARY_CLOUD)
#endif
            oc_netloop_summary_starting();
        sumu.started = pthread_create(&sumu.thread, NULL, sum_bring_up, &sumu) == 0;
        if (!sumu.started) oc_netloop_summary_failed();
        if (!sumu.started) fprintf(stderr, "openchimed: summaries are off: cannot start their thread\n");
    }

    /* Serve the binary protocol until a shutdown signal. */
    {   /* The web client this daemon serves (webapp.h): beside the binary, as
         * the voice data is. Its absence is a note, not a fault. */
        char exe[1024] = "", werr[256] = "";
        oc_tts_exe_dir(exe, sizeof exe);
        if (oc_webapp_load(exe, werr, sizeof werr)) fprintf(stderr, "web client: serving /app/\n");
        else fprintf(stderr, "web client: not served (%s)\n", werr);
    }
    /* Outgoing webhooks' deliveries (docs/details/WEBHOOKS.md), off the loop:
     * only with the factor key, which their secrets are sealed under. */
    oc_webhooks_out *owh = NULL;
    {
        uint8_t fk[OC_FACTOR_KEY_LEN];
        if (oc_dbwriter_factor_key(db, fk) == 0) owh = oc_webhooks_out_start(fk, NULL, NULL, NULL);
        for (volatile uint8_t *z = fk; z < fk + sizeof fk; z++) *z = 0;
        if (!owh) fprintf(stderr, "openchimed: outgoing webhooks are off: no factor key, or no worker\n");
        oc_netloop_set_webhooks_out(owh);
    }
    int served = oc_netloop_run(proto_port, &tls, db, &g_stop);
    if (audio_udp >= 0) close(audio_udp);

    /* Tear down either way — a daemon that could not start still holds a
     * database handle, a TLS context and two worker threads. A claim still
     * trying sees the stop flag and ends; the emitters it may have started go
     * with the rest. */
    g_stop = 1;
    if (svc.claiming) pthread_join(svc.claim, NULL);
    oc_address_report_stop(svc.addr);
    oc_netloop_set_enrollment(NULL, NULL);
    oc_certs_stop(svc.certs);
    oc_tls_state_free(&svc.kept);
    pthread_mutex_destroy(&svc.adopt_mu);
    oc_netloop_set_push(NULL);
    oc_netloop_set_invite_mail(NULL);
    oc_netloop_set_webhooks_out(NULL);
    oc_webhooks_out_stop(owh);
    oc_push_stop(svc.push);
    oc_invite_mail_stop(svc.invite_mail);
    free(svc.privkey);
    free(svc.audience);
    oc_unfurler_stop(unfurler);
    oc_oidcrp_stop(rp);
    oc_relaykeys_stop(relay_keys);
    oc_tls_server_free(&tls);
    /* Summaries before the writer: the worker may be waiting on it. */
    oc_netloop_set_summary(NULL);
    if (sumu.started) pthread_join(sumu.thread, NULL);
    oc_sum_worker_stop(sumu.worker);
    oc_dbwriter_stop(db);

    /* THE EXIT CODE IS THE POINT. `oc_netloop_run` returning -1 means the
     * daemon never served, and saying so is what changes the behaviour of
     * whatever started it: under `Type=simple` a clean 0 is read as "ran and
     * finished", so `Restart=on-failure` does not fire and a daemon that cannot
     * open its blob store stops and stays stopped, with `systemctl status`
     * showing no error at all.
     *
     * The health check no longer hides it: `/healthz` is served by the loop, so
     * a daemon whose loop never started never answers healthy (ARCH-25). */
    if (served < 0) {
        fprintf(stderr, "openchimed: exiting: the server never started\n");
        return 1;
    }
    fprintf(stderr, "openchimed: shutdown complete\n");
    return 0;
}
