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
 * OC_BOOTSTRAP_USERS.
 */

#include "audio.h"
#include "certs.h"
#include "config.h"
#include "dbwriter.h"
#include "enroll.h"
#include "invite_mail.h"
#include "listen.h"
#include "netloop.h"
#include "oidcrp.h"
#include "push.h"
#include "relaykeys.h"
#include "unfurl.h"
#include "tls.h"
#include "totp.h"
#ifdef OC_TTS
#include "tts.h"
#include "tts_render.h"
#endif
#ifdef OC_STT
#include "stt.h"
#include "stt_render.h"
#endif

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

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

/* Provision local accounts from OPENCHIME_BOOTSTRAP_USERS="user:pass[:role],..."
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

/* --- the federated services, once the binding is active (ARCH-85) --------- */

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
    oc_push        *push;
    oc_invite_mail *invite_mail;
    /* The CA-issued certificate (TLS.md, "Certificates"): its worker, the TLS
     * state it presents into, what the database kept, and whether the binding
     * central needs is active. */
    oc_tls_server  *tls;
    oc_certs       *certs;
    oc_tls_state    kept;
    int             active;
} fed_services;

static void keep_account(void *ctx, const char *key_pem, const char *url) {
    fed_services *f = ctx;
    oc_dbwriter_store_acme_account(f->db, f->cfg->tls_src.directory, key_pem, url);
}

static void keep_cert(void *ctx, const oc_cert_issued *c) {
    fed_services *f = ctx;
    oc_dbwriter_store_tls_cert(f->db, f->cfg->tls_src.source == OC_TLS_SRC_ACME ? "acme" : "central",
                               c->names, c->chain_pem, c->key_pem, c->not_before_ms, c->not_after_ms);
}

/* What the certificate worker cannot fix alone, for owners and admins (REQ-263). */
static void raise_alert(void *ctx, const char *key, const char *message) {
    fed_services *f = ctx;
    if (message) oc_dbwriter_alert(f->db, key, message);
    else         oc_dbwriter_alert_clear(f->db, key);
}

/* Start keeping the certificate CA-issued, for the source configured. ACME is
 * answered through the listener, so this waits for the daemon to be serving;
 * central needs an active binding. */
static void start_certs(fed_services *f) {
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

static void start_federated(fed_services *f) {
    const oc_config *cfg = f->cfg;
    /* Push (ARCH-85): only with OC_PUSH_URL pointing at the control-plane push
     * gateway. It signs with the enrollment key and delivers offline mobile
     * notifications; absent in self-hosted stand-alone. */
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
    fed_services *f = arg;
    const oc_config *cfg = f->cfg;
    int wait_secs = cfg->enroll.wait_secs > 0 ? cfg->enroll.wait_secs : 120;
    time_t deadline = time(NULL) + wait_secs;
    unsigned pause = 2;
    while (!g_stop) {
        oc_enroll_result er = oc_enroll_claim(cfg->enroll.url, f->audience, f->privkey,
                                              f->ticket);
        if (er == OC_ENROLL_ACTIVE) {
            oc_dbwriter_note_enrollment_active(f->db, f->privkey, f->audience);
            fprintf(stderr, "openchimed: binding claimed (audience=%s)\n", f->audience);
            start_federated(f);
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

/* The net loop's ready hook: the listener takes connections, so a certificate
 * can be validated through it, and a claim made. */
static void on_serving(void *ctx) {
    fed_services *f = ctx;
    start_certs(f);
    if (!f->ticket || f->claiming) return;
    if (pthread_create(&f->claim, NULL, claim_thread, f) == 0) f->claiming = 1;
    else fprintf(stderr, "openchimed: could not start the enrollment claim\n");
}

/* Stamped by the build (-DOC_VERSION=...). A source build that sets nothing
 * reports "dev", which is the honest answer: only a release build carries a
 * release number, and an operator comparing a running process against an
 * installed package needs to be able to tell the two apart. */
#ifndef OC_VERSION
#define OC_VERSION "dev"
#endif

int main(int argc, char **argv) {
    /* The only argument the daemon takes. Everything else is configuration, and
     * configuration comes from the environment (ARCH-26) — so this is a version
     * probe, not the beginning of a command-line interface. */
#ifdef OC_TTS
    /* Read-aloud's check by ear (tts.h): not configuration, a diagnostic that
     * renders one text with the voice model built into this binary. */
    if (argc == 5 && strcmp(argv[1], "--tts-say") == 0)
        return oc_tts_say(argv[2], argv[3], argv[4]);
    if (argc == 3 && strcmp(argv[1], "--tts-manifest") == 0)
        return oc_tts_manifest(argv[2]);
#endif
#ifdef OC_STT
    /* Voice input's check (stt.h): recognize one recording with the model in
     * this binary. */
    if (argc == 3 && strcmp(argv[1], "--stt-hear") == 0)
        return oc_stt_hear(argv[2]);
    if (argc == 3 && strcmp(argv[1], "--stt-manifest") == 0)
        return oc_stt_manifest(argv[2]);
#endif
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--version") == 0 || strcmp(argv[i], "-V") == 0) {
            printf("openchimed %s (protocol %u)\n", OC_VERSION,
                   (unsigned)OC_PROTOCOL_VERSION);
            return 0;
        }
        fprintf(stderr, "openchimed: unknown argument '%s'\n", argv[i]);
        fprintf(stderr, "usage: openchimed [--version]\n");
        fprintf(stderr, "configuration is read from the environment; see CONFIG.md\n");
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN); /* a peer vanishing mid-write must not kill us */

    fprintf(stderr, "openchimed: version %s (protocol %u)\n", OC_VERSION,
            (unsigned)OC_PROTOCOL_VERSION);

    /* All daemon config is read once, here, into the process-global oc_config
     * (config.c). Subsystems read it via oc_config_get(); nothing else touches
     * the environment. A fatal misconfiguration aborts before we open anything. */
    char cfgerr[256];
    if (oc_config_load(cfgerr, sizeof cfgerr) != 0) {
        fprintf(stderr, "openchimed: config error: %s\n", cfgerr);
        return 1;
    }
    const oc_config *cfg = oc_config_get();

#ifdef OC_TTS
    /* Read-aloud's engine: the voice model built into this binary, chosen by the
     * language asked for (ARCH-111). Whether it is used at all is the operator's,
     * through OPENCHIME_TTS. A language this binary cannot speak is refused here
     * rather than fallen back from: starting anyway would read every message in a
     * language nobody asked for. Necessarily after the config is loaded. */
    {
        const oc_tts_engine *tts_engine = oc_tts_engine_for(cfg->tts.lang);
        if (!tts_engine && cfg->tts.enabled) {
            fprintf(stderr, "openchimed: no read-aloud voice for OPENCHIME_TTS_LANG=%s "
                            "(this binary speaks %s)\n", cfg->tts.lang, OC_TTS_LANG);
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
    }
#endif
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
    int health_port = cfg->health_port, proto_port = cfg->proto_port;
    fprintf(stderr, "openchimed: deployment=%s workspace=\"%s\"\n",
            oc_deploy_mode_name(cfg->deployment_mode),
            cfg->workspace_name && cfg->workspace_name[0] ? cfg->workspace_name : "(unset)");

    /* DB-writer thread: opens the connection and applies migrations before we
     * serve any traffic (ARCH-27). Fatal if it can't. */
    oc_dbwriter *db = oc_dbwriter_start(db_path);
    if (!db) { fprintf(stderr, "openchimed: DB init failed\n"); return 1; }
    if (oc_dbwriter_password_frames(db))
        fprintf(stderr, "openchimed: WARNING: OPENCHIME_TEST_PASSWORD_AUTH is set -- passwords are "
                        "accepted outside the sign-in pages. A test setting; never set it in production.\n");

    /* Registered-user cap (CP-7, hosted plan CP-4): 0/unset = unlimited
     * (self-hosted). Injected into managed-box config at provision time. */
    oc_dbwriter_set_max_users(db, cfg->max_users);

    /* A managed workspace's first boot opens with a welcome in #general: its
     * topic and description, nothing authored. Before the accounts below, which
     * would otherwise create the channel bare; a later boot finds it made. */
    if (oc_dbwriter_welcome_general(db, (int)cfg->deployment_mode, cfg->workspace_name))
        fprintf(stderr, "openchimed: #general created with its welcome\n");

    /* Optionally provision local accounts before serving (AUTH.md §2). */
    bootstrap_users(db, cfg->bootstrap_users);

    /* Federated enrollment (CP-8, AUTH.md §3.6). Gated on OC_ENROLL_URL — the
     * operator opting this box into the control plane. On first boot: generate a
     * keypair + opaque audience, persist them, and print an enrollment code to
     * courier into the console. Then (if pending) call out to central to prove
     * possession and activate. The daemon-generated audience feeds OIDC below. */
    char *enroll_privkey = NULL, *enroll_audience = NULL;
    int enroll_active = 0;
    const char *enroll_url = cfg->enroll.url;
    /* A managed box (AUTH.md §8.7): central minted the audience and started this
     * box with it and a one-time ticket. Exactly one party mints an audience, so
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
                    fprintf(stderr, "openchimed: enrollment code (courier into your "
                                    "control-plane console): %s\n", code);
                    /* Also write it to the enrollment code file (if set) so
                     * orchestration — e.g. the local federated demo's enroll-init —
                     * can reserve it without scraping the log. */
                    const char *cf = cfg->enroll.code_file;
                    if (cf && *cf) {
                        FILE *f = fopen(cf, "w");
                        if (f) { fprintf(f, "%s\n", code); fclose(f); }
                    }
                }
            } else {
                fprintf(stderr, "openchimed: enrollment key generation failed\n");
            }
        }
        /* Try to activate. If the operator hasn't reserved the code yet (PENDING),
         * retry every 3s for up to OC_ENROLL_WAIT_SECS before serving — so a box
         * can come up already-Active (and with push enabled) once the reserve
         * lands, no restart needed. Default 0 = one attempt (retry next boot). */
        int wait_secs = cfg->enroll.wait_secs;
        if (enroll_audience && enroll_privkey && !enroll_active) {
            time_t deadline = time(NULL) + wait_secs;
            for (;;) {
                oc_enroll_result er = oc_enroll_activate(enroll_url, enroll_audience,
                                                         enroll_privkey);
                if (er == OC_ENROLL_ACTIVE) {
                    oc_dbwriter_store_enrollment(db, enroll_privkey, enroll_audience, 1);
                    enroll_active = 1;
                    fprintf(stderr, "openchimed: enrollment activated (audience=%s)\n", enroll_audience);
                    break;
                }
                if (time(NULL) >= deadline) {
                    if (er == OC_ENROLL_PENDING)
                        fprintf(stderr, "openchimed: enrollment pending — reserve the code in the "
                                        "console; activation retries on next boot\n");
                    else
                        fprintf(stderr, "openchimed: enrollment activation failed (central unreachable?)\n");
                    break;
                }
                sleep(3);   /* waiting for the operator to reserve the code */
            }
        }
    }

    /* Which built-in sources are on (AUTH.md §8.8): OPENCHIME_AUTH_MODE is a list
     * — `local`, `relay`, or `local,relay` — and `oidc` reads as `relay`. A name
     * it does not know stops the boot: a typo must not quietly leave a workspace
     * with no way in, or with one its operator did not mean. */
    int want_local = 0, want_relay = 0;
    {
        const char *p = cfg->auth_mode;
        while (*p) {
            const char *e = strchr(p, ',');
            if (!e) e = p + strlen(p);
            const char *s = p;
            while (s < e && (*s == ' ' || *s == '\t')) s++;
            const char *t = e;
            while (t > s && (t[-1] == ' ' || t[-1] == '\t')) t--;
            size_t n = (size_t)(t - s);
            if (n == 5 && strncmp(s, "local", 5) == 0) want_local = 1;
            else if ((n == 5 && strncmp(s, "relay", 5) == 0) ||
                     (n == 4 && strncmp(s, "oidc", 4) == 0)) want_relay = 1;
            else if (n != 0) {
                fprintf(stderr, "openchimed: OPENCHIME_AUTH_MODE: unknown source \"%.*s\"\n",
                        (int)n, s);
                oc_dbwriter_stop(db); return 1;
            }
            p = *e ? e + 1 : e;
        }
        if (!want_local && !want_relay) {
            fprintf(stderr, "openchimed: OPENCHIME_AUTH_MODE names no source\n");
            oc_dbwriter_stop(db); return 1;
        }
    }
    oc_dbwriter_set_local_enabled(db, want_local);
    oc_dbwriter_set_local_mfa(db, cfg->local_mfa);
    /* Passkeys only where a CA vouches for the name (AUTH.md §8.6): a
     * self-signed daemon is reached through the loopback tunnel, where a
     * passkey would bind to 127.0.0.1. */
    if (cfg->tls_src.source != OC_TLS_SRC_SELF) {
        char names[1024];
        snprintf(names, sizeof names, "%s%s%s",
                 cfg->workspace_address ? cfg->workspace_address : "",
                 cfg->workspace_address && cfg->workspace_address[0] && cfg->tls_src.names && cfg->tls_src.names[0] ? "," : "",
                 cfg->tls_src.names ? cfg->tls_src.names : "");
        oc_netloop_set_passkey_names(names);
    }
    oc_dbwriter_set_session_policy(db, (uint64_t)cfg->session_days * 86400000ull,
                                   (uint64_t)cfg->session_idle_days * 86400000ull);

    /* The factor key (AUTH.md §8.6), kept outside the database: without it a
     * stolen database gives away no second step. Made at first start; if it
     * cannot be had, the daemon serves on and no second step can be passed --
     * refusing those sign-ins rather than passing them without it. */
    if (want_local) {
        uint8_t fkey[OC_FACTOR_KEY_LEN];
        char why[256];
        int got = oc_factor_key_load(cfg->factor_key_file, fkey, why, sizeof why);
        if (got >= 0) {
            oc_dbwriter_set_factor_key(db, fkey);
            if (got == 1) fprintf(stderr, "openchimed: made the factor key %s -- back it up with the database\n",
                                  cfg->factor_key_file);
        } else {
            oc_dbwriter_set_factor_key(db, NULL);
            fprintf(stderr, "openchimed: no factor key (%s): local accounts with a second step cannot sign in\n", why);
        }
        memset(fkey, 0, sizeof fkey);
    }

    /* The relay source (AUTH.md §3): pin central's ES256 keys + issuer/audience.
     * An enrolled box (CP-8) uses its daemon-generated audience automatically, and
     * the relay is at the origin it enrolled with (§8.3). */
    oc_relaykeys *relay_keys = NULL;
    if (want_relay) {
        const char *iss = cfg->oidc.issuer;
        const char *aud = enroll_audience ? enroll_audience : cfg->oidc.audience;
        const char *pem = cfg->oidc.pubkey;   /* resolved in oc_config_load; owned by config */
        if (!iss || !aud || !pem) {
            fprintf(stderr, "openchimed: OIDC mode needs OPENCHIME_OIDC_ISSUER, "
                            "OPENCHIME_OIDC_AUDIENCE, and OPENCHIME_OIDC_PUBKEY[_FILE]\n");
            oc_dbwriter_stop(db); return 1;
        }
        char origin[256] = "";
        if (cfg->enroll.url) {
            const char *scheme = strstr(cfg->enroll.url, "://");
            const char *path = scheme ? strchr(scheme + 3, '/') : NULL;
            size_t n = path ? (size_t)(path - cfg->enroll.url) : strlen(cfg->enroll.url);
            if (scheme && n < sizeof origin) { memcpy(origin, cfg->enroll.url, n); origin[n] = '\0'; }
        }
        if (oc_dbwriter_configure_oidc(db, iss, aud, pem, origin) != 0) {
            fprintf(stderr, "openchimed: OIDC configuration failed\n");
            oc_dbwriter_stop(db); return 1;
        }
        char why[256];
        if (oc_dbwriter_configure_join_rules(db, cfg->oidc.allow, why, sizeof why) != 0) {
            fprintf(stderr, "openchimed: OPENCHIME_OIDC_ALLOW: %s\n", why);
            oc_dbwriter_stop(db); return 1;
        }
        oc_dbwriter_set_email_link(db, cfg->oidc.email_link_any);
        if (!cfg->oidc.allow || !cfg->oidc.allow[0])
            fprintf(stderr, "openchimed: OPENCHIME_OIDC_ALLOW is empty: nobody new may join "
                            "by OIDC except through an invite\n");
        fprintf(stderr, "openchimed: OIDC mode (issuer=%s audience=%s)\n", iss, aud);
        /* Enrolled with central: its published keys too, so a rotation reaches
         * this box without its operator (AUTH.md §3.3). */
        if (origin[0]) {
            char url[300];
            snprintf(url, sizeof url, "%s/oidc/jwks", origin);
            relay_keys = oc_relaykeys_start(url, db);
            if (!relay_keys) fprintf(stderr, "openchimed: the relay's published keys will not be read\n");
        }
    }

    /* Direct connections (AUTH.md §8.5): the operator's own providers, each a
     * source. Who may join is the same rule set the relay's sign-ins answer to,
     * read here too when there is no relay. */
    oc_oidcrp *rp = NULL;
    if (cfg->n_connect > 0) {
        if (!want_relay) {
            char why[256];
            if (oc_dbwriter_configure_join_rules(db, cfg->oidc.allow, why, sizeof why) != 0) {
                fprintf(stderr, "openchimed: OPENCHIME_OIDC_ALLOW: %s\n", why);
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

    /* The federated services on the enrollment (ARCH-85): started now when the
     * binding is already active, or by the claim once the daemon serves. A box
     * that is not enrolled has neither. */
    fed_services fed;
    memset(&fed, 0, sizeof fed);
    fed.db = db;
    fed.cfg = cfg;
    fed.privkey = enroll_privkey;
    fed.audience = enroll_audience;
    enroll_privkey = enroll_audience = NULL;   /* fed owns them now */
    fed.active = enroll_active;
    /* A certificate through central needs a binding to ask with. */
    if (cfg->tls_src.source == OC_TLS_SRC_CENTRAL && !(fed.privkey && fed.audience)) {
        fprintf(stderr, "openchimed: OPENCHIME_TLS_SOURCE=central needs an enrollment "
                        "(OPENCHIME_ENROLL_URL); refusing to start without one\n");
        oc_dbwriter_stop(db); return 1;
    }
    if (fed.privkey && fed.audience) {
        if (enroll_active) start_federated(&fed);
        else if (managed_claim) fed.ticket = ticket;
    }
    if (cfg->invite_mail && !enroll_active && !fed.ticket)
        fprintf(stderr, "openchimed: OPENCHIME_INVITE_MAIL is on, but this workspace has no "
                        "active enrollment; invitations are shared by copying them\n");
    oc_netloop_set_ready(on_serving, &fed);

    /* Link unfurls (REQ-222, ARCH-105): always on, no switch. The worker
     * fetches previews off the hot path; its SSRF gate is what makes
     * user-supplied destinations safe to dial at all. */
    oc_unfurler *unfurler = oc_unfurler_start(db, cfg->unfurl.allow_private);
    if (unfurler) oc_netloop_set_unfurler(unfurler);
    else fprintf(stderr, "openchimed: unfurl worker failed to start\n");

    /* First-run bootstrap (REQ-024, local mode only): if there is no owner yet
     * and none was provisioned via OC_BOOTSTRAP_USERS, mint a one-time owner
     * setup token and print it once — the operator redeems it (REDEEM_INVITE)
     * to create the first owner, no pre-existing admin needed. */
    if (oc_dbwriter_auth_methods(db) & OC_AUTH_LOCAL) {
        uint8_t stok[OC_INVITE_TOKEN_LEN];
        if (oc_dbwriter_setup_invite(db, stok)) {
            fprintf(stderr, "openchimed: no owner yet — first-run setup token "
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
    /* The operator's own certificate (OPENCHIME_TLS_SOURCE=file) is theirs:
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
    fed.tls = &tls;
    oc_dbwriter_load_tls_state(db, &fed.kept);
    {
        const oc_tls_state *k = &fed.kept;
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
        } else {
            fed.kept.issued_ms = fed.kept.not_after_ms = 0;    /* nothing to renew: obtain one */
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
        int uport = cfg->audio_port;   /* 0 = ephemeral */
        const char *op = "socket";
        audio_udp = oc_listen_bind(SOCK_DGRAM, uport, &op);
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

    /* Serve the binary protocol until a shutdown signal. */
    int served = oc_netloop_run(proto_port, &tls, db, &g_stop);
    if (audio_udp >= 0) close(audio_udp);

    /* Tear down either way — a daemon that could not start still holds a
     * database handle, a TLS context and two worker threads. A claim still
     * trying sees the stop flag and ends; the emitters it may have started go
     * with the rest. */
    g_stop = 1;
    if (fed.claiming) pthread_join(fed.claim, NULL);
    oc_certs_stop(fed.certs);
    oc_tls_state_free(&fed.kept);
    oc_netloop_set_push(NULL);
    oc_netloop_set_invite_mail(NULL);
    oc_push_stop(fed.push);
    oc_invite_mail_stop(fed.invite_mail);
    free(fed.privkey);
    free(fed.audience);
    oc_unfurler_stop(unfurler);
    oc_oidcrp_stop(rp);
    oc_relaykeys_stop(relay_keys);
    oc_tls_server_free(&tls);
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
