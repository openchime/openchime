/* Flexible black-box client for the local federated demo (scripts/demo-federated.sh).
 * Connects to a *running* daemon over TLS (TOFU-trusting, like e2e_client), logs in
 * with local credentials, and runs ONE command — enough to drive the cross-repo
 * flows by hand/script:
 *
 *   demo_client <host> <port> <user> <pass> token <apns|fcm> <device-token>
 *   demo_client <host> <port> <user> <pass> send  <channel_id> <text...>
 *
 * `token` registers a push device token (so a later SEND has someone to notify);
 * `send` posts a message (which, if the daemon is enrolled + OC_PUSH_URL is set,
 * drives the push emitter). Exits 0 on success. Not part of `make test`.
 */

#include "protocol.h"
#include "framebuf.h"
#include "tls.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define FAIL(msg) do { fprintf(stderr, "demo_client: FAIL %s\n", (msg)); return -1; } while (0)

typedef struct { int fd; oc_tls_client cli; oc_tls_conn conn; oc_framebuf fb; } client;

static oc_tls_status handshake_blocking(oc_tls_conn *c) {
    for (;;) {
        oc_tls_status st = oc_tls_handshake(c);
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
        return st;
    }
}

static int dial(const char *host, int port) {
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof addr);
    addr.sin_family = AF_INET;
    addr.sin_port = htons((uint16_t)port);
    if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) return -1;
    for (int i = 0; i < 100; i++) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (connect(fd, (struct sockaddr *)&addr, sizeof addr) == 0) return fd;
        close(fd);
        usleep(50000);
    }
    return -1;
}

static int client_open(client *c, const char *host, int port) {
    c->fd = dial(host, port);
    if (c->fd < 0) return -1;
    if (oc_tls_client_init(&c->cli, NULL) != 0) return -1;
    if (oc_tls_conn_init(&c->conn, &c->cli.conf, c->fd) != 0) return -1;
    if (handshake_blocking(&c->conn) != OC_TLS_OK) return -1;
    oc_framebuf_init(&c->fb);
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

static int read_frame(client *c, oc_header *hdr, oc_rbuf *payload) {
    for (;;) {
        const uint8_t *frame; size_t flen;
        int r = oc_framebuf_next(&c->fb, &frame, &flen);
        if (r == 1) {
            if (oc_parse_frame(frame, flen, hdr, payload) != OC_OK) return -1;
            if (hdr->msg_type == OC_MSG_PRESENCE_UPDATE || hdr->msg_type == OC_MSG_TYPING_UPDATE)
                continue;
            return 0;
        }
        if (r < 0) return -1;
        uint8_t buf[4096]; size_t n = 0;
        if (oc_tls_read(&c->conn, buf, sizeof buf, &n) != OC_TLS_OK) return -1;
        if (oc_framebuf_push(&c->fb, buf, n) != 0) return -1;
    }
}

static int do_handshake(client *c) {
    uint8_t buf[128]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    /* min = max = OC_PROTOCOL_VERSION, like every real peer (shared/protocol.h). A
     * literal here is the same bug that broke the e2e client on the last bump and CI
     * with it — the two other tools carried it too. */
    oc_hello h = { OC_PROTOCOL_VERSION, OC_PROTOCOL_VERSION, oc_slice_str("demo") };
    if (oc_encode_hello(&w, &h) != OC_OK || write_all(&c->conn, buf, w.len) != 0) return -1;
    oc_header hdr; oc_rbuf p;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_WELCOME) return -1;
    oc_welcome wel;
    if (oc_decode_welcome(&p, &wel) != OC_OK) return -1;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_AUTH_CHALLENGE) return -1;
    oc_auth_challenge ch;
    return oc_decode_auth_challenge(&p, &ch) == OC_OK ? 0 : -1;
}

static int do_auth(client *c, const char *user, const char *pass, uint64_t *uid) {
    uint8_t cbuf[256]; oc_wbuf cw; oc_wbuf_init(&cw, cbuf, sizeof cbuf);
    if (oc_encode_local_credential(&cw, oc_slice_str(user), oc_slice_str(pass)) != OC_OK) return -1;
    uint8_t buf[512]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_auth a = { OC_AUTH_LOCAL, { cbuf, cw.len } };
    if (oc_encode_auth(&w, OC_PROTOCOL_VERSION, &a) != 0 || write_all(&c->conn, buf, w.len) != 0) return -1;
    oc_header hdr; oc_rbuf p;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_AUTH_OK) return -1;
    oc_auth_ok ok;
    if (oc_decode_auth_ok(&p, &ok) != OC_OK) return -1;
    *uid = ok.user_id;
    return 0;
}

/* Authenticate with a central-issued ES256 JWT (OC_AUTH_OIDC) — the credential is
 * the raw token; the daemon verifies it against its pinned central key. */
static int do_auth_oidc(client *c, const char *jwt, uint64_t *uid) {
    uint8_t buf[8192]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_auth a = { OC_AUTH_OIDC, { (const uint8_t *)jwt, strlen(jwt) } };
    if (oc_encode_auth(&w, OC_PROTOCOL_VERSION, &a) != 0 || write_all(&c->conn, buf, w.len) != 0) return -1;
    oc_header hdr; oc_rbuf p;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_AUTH_OK) return -1;
    oc_auth_ok ok;
    if (oc_decode_auth_ok(&p, &ok) != OC_OK) return -1;
    *uid = ok.user_id;
    return 0;
}

static int cmd_token(client *c, const char *platform, const char *token) {
    uint8_t plat = (strcmp(platform, "fcm") == 0) ? OC_PUSH_FCM : OC_PUSH_APNS;
    uint8_t buf[1024]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_register_device_token rd = { plat, oc_slice_str(token) };
    if (oc_encode_register_device_token(&w, OC_PROTOCOL_VERSION, &rd) != OC_OK ||
        write_all(&c->conn, buf, w.len) != 0) FAIL("send register token");
    oc_header hdr; oc_rbuf p;
    if (read_frame(c, &hdr, &p) != 0 || hdr.msg_type != OC_MSG_DEVICE_TOKEN_ACK) FAIL("no ack");
    oc_device_token_ack ack;
    if (oc_decode_device_token_ack(&p, &ack) != OC_OK) FAIL("decode ack");
    if (!ack.ok) FAIL("token rejected");
    printf("demo_client: registered %s token '%s'\n", platform, token);
    return 0;
}

static int cmd_send(client *c, uint64_t channel_id, const char *text) {
    uint8_t idem[OC_IDEM_SIZE];
    for (size_t i = 0; i < sizeof idem; i++) idem[i] = (uint8_t)(text[i % (strlen(text) ? strlen(text) : 1)] ^ i);
    uint8_t buf[4096]; oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_send s = {0};
    s.channel_id = channel_id;
    memcpy(s.idem, idem, OC_IDEM_SIZE);
    s.body = oc_slice_str(text);
    if (oc_encode_send(&w, OC_PROTOCOL_VERSION, &s) != OC_OK || write_all(&c->conn, buf, w.len) != 0)
        FAIL("send");
    for (int i = 0; i < 3; i++) {
        oc_header hdr; oc_rbuf p;
        if (read_frame(c, &hdr, &p) != 0) FAIL("read after send");
        if (hdr.msg_type == OC_MSG_SEND_ACK) {
            oc_send_ack ack;
            if (oc_decode_send_ack(&p, &ack) != OC_OK) FAIL("decode ack");
            printf("demo_client: sent message %llu to channel %llu\n",
                   (unsigned long long)ack.message_id, (unsigned long long)channel_id);
            return 0;
        }
        if (hdr.msg_type == OC_MSG_ERROR) FAIL("send rejected");
    }
    FAIL("no send ack");
}

int main(int argc, char **argv) {
    if (argc < 6) {
        fprintf(stderr,
            "usage: %s <host> <port> <user> <pass> token <apns|fcm> <device-token>\n"
            "       %s <host> <port> <user> <pass> send  <channel_id> <text...>\n"
            "       %s <host> <port> --oidc <jwt>   whoami\n"
            "       %s <host> <port> --oidc <jwt>   send <channel_id> <text...>\n",
            argv[0], argv[0], argv[0], argv[0]);
        return 2;
    }
    const char *host = argv[1]; int port = atoi(argv[2]);
    int oidc = (strcmp(argv[3], "--oidc") == 0);
    const char *jwt = oidc ? argv[4] : NULL;
    const char *user = argv[3], *pass = argv[4];
    const char *cmd = argv[5];

    client c;
    if (client_open(&c, host, port) != 0) { fprintf(stderr, "demo_client: FAIL connect\n"); return 1; }
    uint64_t uid = 0;
    if (do_handshake(&c) != 0) { fprintf(stderr, "demo_client: FAIL handshake\n"); return 1; }
    if (oidc) {
        if (do_auth_oidc(&c, jwt, &uid) != 0) { fprintf(stderr, "demo_client: FAIL oidc auth\n"); return 1; }
    } else {
        if (do_auth(&c, user, pass, &uid) != 0) { fprintf(stderr, "demo_client: FAIL auth\n"); return 1; }
    }

    int rc;
    if (strcmp(cmd, "whoami") == 0) {
        printf("demo_client: authenticated as uid %llu\n", (unsigned long long)uid);
        rc = 0;
    } else if (strcmp(cmd, "token") == 0 && argc >= 8) {
        rc = cmd_token(&c, argv[6], argv[7]);
    } else if (strcmp(cmd, "send") == 0 && argc >= 8) {
        rc = cmd_send(&c, strtoull(argv[6], NULL, 10), argv[7]);
    } else {
        fprintf(stderr, "demo_client: unknown or malformed command\n");
        rc = -1;
    }
    return rc == 0 ? 0 : 1;
}
