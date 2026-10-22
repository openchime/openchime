/* `signin <host> <port> <user> <password>`: sign in through the daemon's page
 * (tests/testsignin.h) and print the token and the verifier a client presents
 * on AUTH, space-separated, on one line. For a shell script that drives a
 * client which has no browser of its own.
 *
 * `signin present <host> <port> <token> <verifier>`: the other half, for a
 * script that played the browser itself (scripts/web/pages.sh): HELLO, then
 * AUTH{oidc, "local", token, verifier}, and the user id AUTH_OK names, or
 * the refusal's code. Exit 1 with the reason on stderr either way. */
#include "check.h"        /* oc_test_deadline, oc_test_pause: waits on the monotonic clock */
#include "testsignin.h"

#include "framebuf.h"
#include "protocol.h"
#include "tls.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int dial(const char *host, int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return -1;
    /* Until the daemon listens, or five seconds on the monotonic clock. */
    uint64_t end = oc_test_deadline(5000);
    do {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd >= 0 && connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) return fd;
        if (fd >= 0) close(fd);
    } while (oc_test_pause(end));
    return -1;
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

/* The next frame of type `want`, or an ERROR or REJECT; the daemon's unbidden
 * pushes are skipped. */
static int read_until(oc_tls_conn *c, oc_framebuf *fb, uint16_t want, oc_header *hdr, oc_rbuf *p) {
    for (int i = 0; i < 32; i++) {
        for (;;) {
            const uint8_t *frame; size_t flen;
            int r = oc_framebuf_next(fb, &frame, &flen);
            if (r == 1) {
                if (oc_parse_frame(frame, flen, hdr, p) != OC_OK) return -1;
                break;
            }
            if (r < 0) return -1;
            uint8_t buf[4096]; size_t n = 0;
            if (oc_tls_read(c, buf, sizeof buf, &n) != OC_TLS_OK) return -1;
            if (oc_framebuf_push(fb, buf, n) != 0) return -1;
        }
        if (hdr->msg_type == want || hdr->msg_type == OC_MSG_ERROR || hdr->msg_type == OC_MSG_REJECT) return 0;
    }
    return -1;
}

static int present(const char *host, int port, const char *tok, const char *ver) {
    int fd = dial(host, port);
    if (fd < 0) { fprintf(stderr, "signin: cannot connect to %s:%d\n", host, port); return 1; }
    oc_tls_client cli; oc_tls_conn conn; oc_framebuf fb;
    if (oc_tls_client_init(&cli, NULL) != 0 || oc_tls_conn_init(&conn, &cli.conf, fd) != 0) return 1;
    oc_tls_status st;
    do st = oc_tls_handshake(&conn); while (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE);
    if (st != OC_TLS_OK) { fprintf(stderr, "signin: TLS handshake failed\n"); return 1; }
    oc_framebuf_init(&fb);
    uint8_t buf[8192]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_hello h = { OC_PROTOCOL_VERSION, OC_PROTOCOL_VERSION, oc_slice_str("signin") };
    if (oc_encode_hello(&w, &h) != OC_OK || write_all(&conn, buf, w.len) != 0) return 1;
    oc_header hdr; oc_rbuf p;
    if (read_until(&conn, &fb, OC_MSG_WELCOME, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_WELCOME) {
        fprintf(stderr, "signin: no WELCOME\n"); return 1;
    }
    if (read_until(&conn, &fb, OC_MSG_AUTH_CHALLENGE, &hdr, &p) != 0) { fprintf(stderr, "signin: no AUTH_CHALLENGE\n"); return 1; }
    oc_wbuf_init(&w, buf, sizeof buf);
    oc_auth a = { OC_AUTH_OIDC, oc_slice_str("local"), oc_slice_str(tok), oc_slice_str(ver), { NULL, 0 } };
    if (oc_encode_auth(&w, OC_PROTOCOL_VERSION, &a) != OC_OK || write_all(&conn, buf, w.len) != 0) return 1;
    if (read_until(&conn, &fb, OC_MSG_AUTH_OK, &hdr, &p) != 0) { fprintf(stderr, "signin: no answer to AUTH\n"); return 1; }
    int rc = 1;
    if (hdr.msg_type == OC_MSG_AUTH_OK) {
        oc_auth_ok ok;
        if (oc_decode_auth_ok(&p, &ok) == OC_OK) { printf("%llu\n", (unsigned long long)ok.user_id); rc = 0; }
    } else {
        oc_error e;
        if (oc_decode_error(&p, &e) == OC_OK)
            fprintf(stderr, "signin: refused: %u %.*s\n", e.code, (int)e.message.len, (const char *)e.message.ptr);
        else fprintf(stderr, "signin: refused\n");
    }
    oc_framebuf_free(&fb); oc_tls_conn_free(&conn); oc_tls_client_free(&cli); close(fd);
    return rc;
}

int main(int argc, char **argv) {
    if (argc == 6 && strcmp(argv[1], "present") == 0) return present(argv[2], atoi(argv[3]), argv[4], argv[5]);
    if (argc != 5) {
        fprintf(stderr, "usage: signin <host> <port> <user> <password>\n"
                        "       signin present <host> <port> <token> <verifier>\n");
        return 2;
    }
    char tok[OC_TEST_TOKEN_CAP], ver[OC_TEST_VERIFIER_CAP], why[256];
    if (oc_test_signin(argv[1], atoi(argv[2]), NULL, argv[3], argv[4],
                       tok, sizeof tok, ver, sizeof ver, why, sizeof why) != 0) {
        fprintf(stderr, "signin: %s\n", why);
        return 1;
    }
    printf("%s %s\n", tok, ver);
    return 0;
}
