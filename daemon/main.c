/*
 * openchimed — daemon entry point.
 *
 * Wires the skeleton together (ARCH-5, ARCH-22): a DB-writer thread that owns
 * the SQLite connection and migrates on boot, a lightweight /healthz HTTP
 * responder for the orchestrator (ARCH-25), and the epoll network loop that
 * terminates TLS (ARCH-10) and serves the binary protocol. The loop completes
 * the handshake (PROTOCOL.md §3), runs the two-mode auth handshake — local
 * passwords + session reconnect today, OIDC to follow (AUTH.md) — and serves
 * the messaging vertical. Local accounts can be provisioned at boot via
 * OC_BOOTSTRAP_USERS.
 */

#include "audio.h"
#include "dbwriter.h"
#include "enroll.h"
#include "netloop.h"
#include "push.h"
#include "tls.h"

#include <arpa/inet.h>
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

static const char *env_or(const char *name, const char *dflt) {
    const char *v = getenv(name);
    return v ? v : dflt;
}

static int env_port(const char *name, int dflt) {
    const char *v = getenv(name);
    return v ? atoi(v) : dflt;
}

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

/* The pinned central public key (AUTH.md §3.4): OC_OIDC_PUBKEY_FILE (a path) or
 * OC_OIDC_PUBKEY (inline PEM). Caller frees. */
static char *load_oidc_pubkey(void) {
    const char *path = getenv("OC_OIDC_PUBKEY_FILE");
    if (path) return read_file(path);
    const char *inl = getenv("OC_OIDC_PUBKEY");
    return inl ? strdup(inl) : NULL;
}

/* Provision local accounts from OC_BOOTSTRAP_USERS="user:pass[:role],..."
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

/* --- /healthz (ARCH-25) + the landing page -------------------------------- */

static const char HEALTH_RESPONSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "Connection: close\r\n"
    "\r\n"
    "OK";

/* A workspace address is a perfectly natural thing to paste into a browser, so
 * every non-/healthz path answers with this instead of a bare 404 / connection
 * reset: enough to confirm "yes, an OpenChime workspace lives here, you need a
 * client", and nothing more. It deliberately leaks no workspace identity, user
 * list, or version — an unauthenticated stranger learns only that the daemon is
 * running. Static and self-contained (no external fetches). */
static const char LANDING_BODY[] =
    "<!DOCTYPE html>\n"
    "<html lang=\"en\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">\n"
    "<title>OpenChime</title>\n"
    "<style>\n"
    "body{background:#14161a;color:#d8dee9;font:16px/1.6 ui-sans-serif,system-ui,"
    "-apple-system,Segoe UI,Roboto,sans-serif;display:flex;align-items:center;"
    "justify-content:center;min-height:100vh;margin:0}\n"
    "main{max-width:32rem;padding:2rem;text-align:center}\n"
    "h1{font-size:1.5rem;font-weight:600;margin:0 0 .5rem}\n"
    "p{margin:.5rem 0;color:#9aa5b1}\n"
    "a{color:#88c0d0}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<main>\n"
    "<h1>OpenChime</h1>\n"
    "<p>An OpenChime workspace is running here.</p>\n"
    "<p>This address speaks the OpenChime protocol, not the web &mdash; "
    "open it in an OpenChime client to sign in.</p>\n"
    "<p><a href=\"https://github.com/danheskett/openchime\">openchime</a></p>\n"
    "</main>\n"
    "</body>\n"
    "</html>\n";

/* Is the request line's path exactly `want` (ignoring any ?query)? `req` is the
 * raw request as read; a malformed or truncated request line matches nothing. */
static int path_is(const char *req, size_t len, const char *want) {
    const char *sp = memchr(req, ' ', len);            /* end of the method */
    if (!sp) return 0;
    const char *p = sp + 1;
    size_t rest = len - (size_t)(p - req);
    size_t plen = 0;
    while (plen < rest && p[plen] != ' ' && p[plen] != '?' &&
           p[plen] != '\r' && p[plen] != '\n') plen++;
    size_t wlen = strlen(want);
    return plen == wlen && memcmp(p, want, wlen) == 0;
}

static void *health_thread(void *arg) {
    int port = *(int *)arg;
    int listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (listen_fd < 0) { perror("openchimed: healthz socket"); return NULL; }
    int yes = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof yes);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons((uint16_t)port);
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof addr) < 0 ||
        listen(listen_fd, 16) < 0) {
        perror("openchimed: healthz bind/listen");
        close(listen_fd);
        return NULL;
    }
    fprintf(stderr, "openchimed: healthz listening on :%d\n", port);

    for (;;) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) continue;
        char buf[512];
        ssize_t n = read(fd, buf, sizeof buf);
        size_t got = (n > 0) ? (size_t)n : 0;
        ssize_t w;
        if (path_is(buf, got, "/healthz")) {
            w = write(fd, HEALTH_RESPONSE, sizeof HEALTH_RESPONSE - 1);
        } else {
            /* Everything else — including `/` and any unknown path — gets the
             * landing page, so a browser never sees a 404 or a raw reset. */
            char head[192];
            int hl = snprintf(head, sizeof head,
                              "HTTP/1.1 200 OK\r\n"
                              "Content-Type: text/html; charset=utf-8\r\n"
                              "Content-Length: %zu\r\n"
                              "Connection: close\r\n"
                              "\r\n",
                              sizeof LANDING_BODY - 1);
            w = write(fd, head, (size_t)hl);
            if (w > 0) w = write(fd, LANDING_BODY, sizeof LANDING_BODY - 1);
        }
        (void)w;
        close(fd);
    }
    return NULL;
}

int main(void) {
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN); /* a peer vanishing mid-write must not kill us */

    const char *db_path   = env_or("OPENCHIME_DB_PATH", "/data/openchime.db");
    const char *cert_path = env_or("OPENCHIME_TLS_CERT", "/data/cert.pem");
    const char *key_path  = env_or("OPENCHIME_TLS_KEY",  "/data/key.pem");
    int health_port = env_port("OPENCHIME_HEALTH_PORT", 8080);
    int proto_port  = env_port("OPENCHIME_PROTO_PORT", 8443);

    /* DB-writer thread: opens the connection and applies migrations before we
     * serve any traffic (ARCH-27). Fatal if it can't. */
    oc_dbwriter *db = oc_dbwriter_start(db_path);
    if (!db) { fprintf(stderr, "openchimed: DB init failed\n"); return 1; }

    /* Registered-user cap (CP-7, hosted plan CP-4): OPENCHIME_MAX_USERS; 0/unset =
     * unlimited (self-hosted). Written into hosted-box config at provision time. */
    oc_dbwriter_set_max_users(db, atoi(env_or("OPENCHIME_MAX_USERS", "0")));

    /* Optionally provision local accounts before serving (AUTH.md §2). */
    bootstrap_users(db, getenv("OC_BOOTSTRAP_USERS"));

    /* Federated enrollment (CP-8, AUTH.md §3.6). Gated on OC_ENROLL_URL — the
     * operator opting this box into the control plane. On first boot: generate a
     * keypair + opaque audience, persist them, and print an enrollment code to
     * courier into the console. Then (if pending) call out to central to prove
     * possession and activate. The daemon-generated audience feeds OIDC below. */
    char *enroll_privkey = NULL, *enroll_audience = NULL;
    int enroll_active = 0;
    const char *enroll_url = getenv("OC_ENROLL_URL");
    if (enroll_url && *enroll_url) {
        if (!oc_dbwriter_load_enrollment(db, &enroll_privkey, &enroll_audience, &enroll_active)) {
            char pk[1024], aud[128], code[2048];
            if (oc_enroll_generate(pk, sizeof pk, aud, sizeof aud) == 0 &&
                oc_dbwriter_store_enrollment(db, pk, aud, 0)) {
                enroll_privkey = strdup(pk);
                enroll_audience = strdup(aud);
                if (oc_enroll_build_code(pk, aud, code, sizeof code) == 0) {
                    fprintf(stderr, "openchimed: enrollment code (courier into your "
                                    "control-plane console): %s\n", code);
                    /* Also write it to OC_ENROLL_CODE_FILE (if set) so orchestration
                     * — e.g. the local federated demo's enroll-init — can reserve it
                     * without scraping the log. */
                    const char *cf = getenv("OC_ENROLL_CODE_FILE");
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
        int wait_secs = atoi(env_or("OC_ENROLL_WAIT_SECS", "0"));
        if (enroll_audience && enroll_privkey && !enroll_active) {
            time_t deadline = time(NULL) + wait_secs;
            for (;;) {
                oc_enroll_result er = oc_enroll_activate(enroll_url, getenv("OC_ENROLL_CA_BUNDLE"),
                                                         enroll_audience, enroll_privkey);
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

    /* OIDC mode (AUTH.md §3): pin central's ES256 key + issuer/audience. When
     * set, AUTH_CHALLENGE advertises oidc+session instead of local+session. An
     * enrolled box (CP-8) uses its daemon-generated audience automatically. */
    if (strcmp(env_or("OC_AUTH_MODE", "local"), "oidc") == 0) {
        const char *iss = getenv("OC_OIDC_ISSUER");
        const char *aud = enroll_audience ? enroll_audience : getenv("OC_OIDC_AUDIENCE");
        char *pem = load_oidc_pubkey();
        if (!iss || !aud || !pem) {
            fprintf(stderr, "openchimed: OIDC mode needs OC_OIDC_ISSUER, "
                            "OC_OIDC_AUDIENCE, and OC_OIDC_PUBKEY[_FILE]\n");
            free(pem); oc_dbwriter_stop(db); return 1;
        }
        if (oc_dbwriter_configure_oidc(db, iss, aud, pem, env_or("OC_OIDC_PARAMS", "")) != 0) {
            fprintf(stderr, "openchimed: OIDC configuration failed\n");
            free(pem); oc_dbwriter_stop(db); return 1;
        }
        free(pem);
        fprintf(stderr, "openchimed: OIDC mode (issuer=%s audience=%s)\n", iss, aud);
    }

    /* Outbound push emitter (ARCH-85). Only when the box is enrolled (holds an
     * active audience + key) and OC_PUSH_URL points at the control-plane push
     * gateway. It signs with the enrollment key and delivers offline mobile
     * notifications; absent in self-hosted stand-alone. */
    oc_push *push = NULL;
    const char *push_url = getenv("OC_PUSH_URL");
    if (enroll_active && enroll_audience && enroll_privkey && push_url && *push_url) {
        push = oc_push_start(db_path, db, push_url, getenv("OC_PUSH_CA_BUNDLE"),
                             enroll_audience, enroll_privkey);
        if (push) {
            oc_netloop_set_push(push);
            fprintf(stderr, "openchimed: push emitter enabled (audience=%s)\n", enroll_audience);
        } else {
            fprintf(stderr, "openchimed: push emitter failed to start\n");
        }
    }

    free(enroll_privkey);
    free(enroll_audience);

    /* First-run bootstrap (REQ-024, local mode only): if there is no owner yet
     * and none was provisioned via OC_BOOTSTRAP_USERS, mint a one-time owner
     * setup token and print it once — the operator redeems it (REDEEM_INVITE)
     * to create the first owner, no pre-existing admin needed. */
    if (oc_dbwriter_auth_methods(db) & OC_AUTH_LOCAL) {
        uint8_t stok[OC_INVITE_TOKEN_LEN];
        if (oc_dbwriter_setup_invite(db, stok)) {
            fprintf(stderr, "openchimed: no owner yet — first-run setup token "
                            "(redeem to create the owner): ");
            for (size_t i = 0; i < OC_INVITE_TOKEN_LEN; i++) fprintf(stderr, "%02x", stok[i]);
            fprintf(stderr, "\n");
        }
    }

    /* TLS identity (ARCH-10). A persisted cert+key in the DB (ARCH-66b) is
     * restored to the cert/key files first, so a database moved or restored onto
     * a new box keeps the same TOFU fingerprint instead of generating a new,
     * pin-breaking one. If none is stored, the first-run generation below
     * creates one and we persist it. */
    char *stored_cert = NULL, *stored_key = NULL;
    int had_identity = oc_dbwriter_load_identity(db, &stored_cert, &stored_key);
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
    if (!had_identity) {
        char *cert = read_file(cert_path), *key = read_file(key_path);
        if (cert && key && !oc_dbwriter_store_identity(db, cert, key))
            fprintf(stderr, "openchimed: warning: could not persist TLS identity\n");
        free(cert); free(key);
    }

    pthread_t hz;
    pthread_create(&hz, NULL, health_thread, &health_port);
    pthread_detach(hz);

    /* Fork the audio relay sidecar (REQ-150/151, ARCH-28/31): bind its UDP
     * socket here (so we learn the port to advertise), pair a Unix socket for
     * IPC, and fork. The child runs the relay and exits when this daemon closes
     * its IPC end (EOF); a fork failure just disables media (calls still form). */
    {
        int uport = env_port("OPENCHIME_AUDIO_PORT", 0);   /* 0 = ephemeral */
        int udp = socket(AF_INET, SOCK_DGRAM, 0);
        struct sockaddr_in ua; memset(&ua, 0, sizeof ua);
        ua.sin_family = AF_INET; ua.sin_addr.s_addr = htonl(INADDR_ANY);
        ua.sin_port = htons((uint16_t)uport);
        int sv[2];
        if (udp >= 0 && bind(udp, (struct sockaddr *)&ua, sizeof ua) == 0 &&
            socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            socklen_t sl = sizeof ua; getsockname(udp, (struct sockaddr *)&ua, &sl);
            uint16_t audio_port = ntohs(ua.sin_port);
            pid_t pid = fork();
            if (pid == 0) {                 /* sidecar */
                close(sv[0]);
                _exit(oc_audio_sidecar_run(sv[1], udp, &g_stop));
            }
            close(sv[1]); close(udp);       /* the sidecar owns these now */
            if (pid > 0) {
                oc_netloop_set_audio(sv[0], audio_port);
                fprintf(stderr, "openchimed: audio sidecar pid %d, UDP :%u\n", (int)pid, audio_port);
            } else { close(sv[0]); }
        } else if (udp >= 0) { close(udp); }
    }

    /* Serve the binary protocol until a shutdown signal. */
    oc_netloop_run(proto_port, &tls, db, &g_stop);

    oc_push_stop(push);
    oc_tls_server_free(&tls);
    oc_dbwriter_stop(db);
    fprintf(stderr, "openchimed: shutdown complete\n");
    return 0;
}
