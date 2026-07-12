/*
 * openchimed — daemon entry point.
 *
 * Wires the skeleton together (ARCH-5, ARCH-22): a DB-writer thread that owns
 * the SQLite connection and migrates on boot, a lightweight /healthz HTTP
 * responder for the orchestrator (ARCH-25), and the epoll network loop that
 * terminates TLS (ARCH-10) and serves the binary protocol. AUTH and messaging
 * are not handled yet — the loop currently completes the handshake and answers
 * HELLO with WELCOME/REJECT (PROTOCOL.md §3).
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
