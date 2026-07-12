/* Integration test for the event loop (netloop.c): a TLS client completes the
 * handshake, sends HELLO, and gets a WELCOME back; a bad version gets REJECT.
 * Runs the non-blocking epoll server in a thread and drives it with a blocking
 * TLS client. Includes the code under test directly; links mbedTLS + pthread. */

#include "netloop.c"
#include "framebuf.c"
#include "protocol.c"
#include "tls.c"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

struct loop_arg {
    int                       port;
    oc_tls_server            *srv;
    volatile sig_atomic_t     stop;
};

static void *loop_thread(void *p) {
    struct loop_arg *a = (struct loop_arg *)p;
    oc_netloop_run(a->port, a->srv, &a->stop);
    return NULL;
}

static oc_tls_status handshake_blocking(oc_tls_conn *c) {
    for (;;) {
        oc_tls_status st = oc_tls_handshake(c);
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
        return st;
    }
}

/* Connect (with retry until the loop is listening) and TLS-handshake, pinning
 * the server fingerprint. Returns the connected fd via *out_fd. */
static int connect_client(int port, const uint8_t *pin, oc_tls_client *cli,
                          oc_tls_conn *c, int *out_fd) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons((uint16_t)port);

    int fd = -1;
    for (int i = 0; i < 100; i++) {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) break;
        close(fd); fd = -1;
        usleep(20000); /* 20ms; loop may not be listening yet */
    }
    if (fd < 0) return -1;

    if (oc_tls_client_init(cli, pin) != 0) { close(fd); return -1; }
    if (oc_tls_conn_init(c, &cli->conf, fd) != 0) { close(fd); return -1; }
    if (handshake_blocking(c) != OC_TLS_OK) { close(fd); return -1; }
    *out_fd = fd;
    return 0;
}

static int write_all(oc_tls_conn *c, const uint8_t *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        size_t n = 0;
        if (oc_tls_write(c, buf + sent, len - sent, &n) != OC_TLS_OK) return -1;
        sent += n;
    }
    return 0;
}

/* Read exactly one protocol frame from the (blocking) client connection. */
static int read_frame(oc_tls_conn *c, oc_framebuf *fb, oc_header *hdr, oc_rbuf *payload) {
    for (;;) {
        const uint8_t *frame; size_t flen;
        int r = oc_framebuf_next(fb, &frame, &flen);
        if (r == 1) return oc_parse_frame(frame, flen, hdr, payload) == OC_OK ? 0 : -1;
        if (r < 0) return -1;
        uint8_t buf[2048]; size_t n = 0;
        if (oc_tls_read(c, buf, sizeof buf, &n) != OC_TLS_OK) return -1;
        if (oc_framebuf_push(fb, buf, n) != 0) return -1;
    }
}

static void test_hello_welcome_and_reject(void) {
    oc_tls_server srv;
    CHECK(oc_tls_server_init(&srv, NULL, NULL) == 0);
    uint8_t pin[OC_TLS_FINGERPRINT_LEN];
    CHECK(oc_tls_server_fingerprint(&srv, pin) == 0);

    struct loop_arg arg;
    arg.port = 18000 + (int)(getpid() % 2000);
    arg.srv = &srv;
    arg.stop = 0;
    pthread_t th;
    CHECK(pthread_create(&th, NULL, loop_thread, &arg) == 0);

    /* Case 1: a supported HELLO gets a WELCOME with the chosen version. */
    {
        oc_tls_client cli; oc_tls_conn c; int fd = -1;
        CHECK(connect_client(arg.port, pin, &cli, &c, &fd) == 0);

        uint8_t buf[128];
        oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        oc_hello h = { 1, 1, oc_slice_str("itest") };
        CHECK(oc_encode_hello(&w, &h) == OC_OK);
        CHECK(write_all(&c, buf, w.len) == 0);

        oc_framebuf fb; oc_framebuf_init(&fb);
        oc_header hdr; oc_rbuf payload;
        CHECK(read_frame(&c, &fb, &hdr, &payload) == 0);
        CHECK(hdr.msg_type == OC_MSG_WELCOME);
        oc_welcome wel;
        CHECK(oc_decode_welcome(&payload, &wel) == OC_OK);
        CHECK(wel.chosen_version == OC_PROTOCOL_VERSION);

        oc_framebuf_free(&fb);
        oc_tls_conn_free(&c); oc_tls_client_free(&cli); close(fd);
    }

    /* Case 2: a too-new HELLO gets a REJECT with VERSION_TOO_NEW. */
    {
        oc_tls_client cli; oc_tls_conn c; int fd = -1;
        CHECK(connect_client(arg.port, pin, &cli, &c, &fd) == 0);

        uint8_t buf[128];
        oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
        oc_hello h = { 5, 5, oc_slice_str("itest") };
        CHECK(oc_encode_hello(&w, &h) == OC_OK);
        CHECK(write_all(&c, buf, w.len) == 0);

        oc_framebuf fb; oc_framebuf_init(&fb);
        oc_header hdr; oc_rbuf payload;
        CHECK(read_frame(&c, &fb, &hdr, &payload) == 0);
        CHECK(hdr.msg_type == OC_MSG_REJECT);
        oc_reject rej;
        CHECK(oc_decode_reject(&payload, &rej) == OC_OK);
        CHECK(rej.code == OC_ERR_VERSION_TOO_NEW);

        oc_framebuf_free(&fb);
        oc_tls_conn_free(&c); oc_tls_client_free(&cli); close(fd);
    }

    arg.stop = 1;
    pthread_join(th, NULL);
    oc_tls_server_free(&srv);
}

int main(void) {
    printf("itest_netloop: TLS handshake, HELLO->WELCOME, version REJECT\n");
    test_hello_welcome_and_reject();
    if (failures == 0) { printf("OK: all checks passed\n"); return 0; }
    printf("FAILED: %d check(s)\n", failures);
    return 1;
}
