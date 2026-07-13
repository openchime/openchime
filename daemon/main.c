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

#include "dbwriter.h"
#include "netloop.h"
#include "tls.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
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

/* --- /healthz (ARCH-25) ------------------------------------------------- */

static const char HEALTH_RESPONSE[] =
    "HTTP/1.1 200 OK\r\n"
    "Content-Type: text/plain\r\n"
    "Content-Length: 2\r\n"
    "Connection: close\r\n"
    "\r\n"
    "OK";

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
        (void)n;
        ssize_t w = write(fd, HEALTH_RESPONSE, sizeof HEALTH_RESPONSE - 1);
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

    /* Optionally provision local accounts before serving (AUTH.md §2). */
    bootstrap_users(db, getenv("OC_BOOTSTRAP_USERS"));

    /* OIDC mode (AUTH.md §3): pin central's ES256 key + issuer/audience. When
     * set, AUTH_CHALLENGE advertises oidc+session instead of local+session. */
    if (strcmp(env_or("OC_AUTH_MODE", "local"), "oidc") == 0) {
        const char *iss = getenv("OC_OIDC_ISSUER");
        const char *aud = getenv("OC_OIDC_AUDIENCE");
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

    /* Self-signed cert on first run, reused thereafter (ARCH-10). */
    oc_tls_server tls;
    if (oc_tls_server_init(&tls, cert_path, key_path) != 0) {
        fprintf(stderr, "openchimed: TLS init failed\n");
        oc_dbwriter_stop(db);
        return 1;
    }

    pthread_t hz;
    pthread_create(&hz, NULL, health_thread, &health_port);
    pthread_detach(hz);

    /* Serve the binary protocol until a shutdown signal. */
    oc_netloop_run(proto_port, &tls, db, &g_stop);

    oc_tls_server_free(&tls);
    oc_dbwriter_stop(db);
    fprintf(stderr, "openchimed: shutdown complete\n");
    return 0;
}
