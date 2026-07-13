/* Unit tests for the OpenChime v1 frame codec (docs/PROTOCOL.md, TESTING.md
 * §2.2). Like the openblocks convention, the code under test is #included
 * directly so file-static helpers are reachable, and a single CHECK macro
 * counts failures. Built and run by `make test`; non-zero exit means failure. */

#include "protocol.h"
#include "check.h"

#include <string.h>

/* Scratch buffers sized for a full frame plus a max body. */
static uint8_t frame[OC_MAX_FRAME_SIZE];
static uint8_t big[OC_MAX_BODY_SIZE + 8];

static int slice_eq_str(oc_slice s, const char *str) {
    size_t n = strlen(str);
    return s.len == n && (n == 0 || memcmp(s.ptr, str, n) == 0);
}

/* --- Primitive encodings (§7) ------------------------------------------- */
static void test_primitives(void) {
    oc_wbuf w;
    oc_wbuf_init(&w, frame, sizeof frame);
    oc_w_u8(&w, 0x12);
    oc_w_u16(&w, 0x1234);
    oc_w_u32(&w, 0x12345678u);
    oc_w_u64(&w, 0x1122334455667788ull);
    oc_w_str(&w, oc_slice_str("hi"));
    oc_w_str(&w, oc_slice_str(""));            /* empty string is legal */
    oc_w_lstr(&w, oc_slice_str("longer body"));
    uint8_t idem[OC_IDEM_SIZE];
    for (int i = 0; i < (int)OC_IDEM_SIZE; i++) idem[i] = (uint8_t)(i + 1);
    oc_w_idem(&w, idem);
    CHECK(!w.overflow);

    /* Big-endian on the wire: first byte of the u16 is the high byte. */
    CHECK(frame[0] == 0x12);
    CHECK(frame[1] == 0x12 && frame[2] == 0x34);

    oc_rbuf r;
    oc_rbuf_init(&r, frame, w.len);
    CHECK(oc_r_u8(&r) == 0x12);
    CHECK(oc_r_u16(&r) == 0x1234);
    CHECK(oc_r_u32(&r) == 0x12345678u);
    CHECK(oc_r_u64(&r) == 0x1122334455667788ull);
    CHECK(slice_eq_str(oc_r_str(&r), "hi"));
    CHECK(slice_eq_str(oc_r_str(&r), ""));
    CHECK(slice_eq_str(oc_r_lstr(&r), "longer body"));
    uint8_t got[OC_IDEM_SIZE];
    oc_r_idem(&r, got);
    CHECK(memcmp(got, idem, OC_IDEM_SIZE) == 0);
    CHECK(!r.underflow && r.pos == r.len);

    /* A str longer than its u16 prefix can hold overflows the frame. */
    oc_wbuf w2;
    oc_wbuf_init(&w2, frame, sizeof frame);
    oc_slice toolong = { frame, 70000 };       /* ptr never read: length check fires first */
    oc_w_str(&w2, toolong);
    CHECK(w2.overflow);
}

/* --- Full-frame round trips (§9) ---------------------------------------- */

/* Encode with `enc`, parse the frame, assert header, hand payload to caller. */
#define ROUNDTRIP(enc, expect_type, hdrvar, payloadvar)                      \
    oc_wbuf w; oc_wbuf_init(&w, frame, sizeof frame);                        \
    CHECK((enc) == OC_OK);                                                   \
    oc_header hdrvar; oc_rbuf payloadvar;                                     \
    CHECK(oc_parse_frame(frame, w.len, &hdrvar, &payloadvar) == OC_OK);      \
    CHECK(hdrvar.msg_type == (expect_type));                                 \
    CHECK((size_t)hdrvar.length + 4u == w.len)

static void test_handshake_frames(void) {
    {
        oc_hello in = { 1, 3, oc_slice_str("openchime-desktop/0.1 linux") };
        ROUNDTRIP(oc_encode_hello(&w, &in), OC_MSG_HELLO, h, p);
        CHECK(h.version == OC_PROTOCOL_VERSION); /* HELLO is frozen at v1 */
        oc_hello out;
        CHECK(oc_decode_hello(&p, &out) == OC_OK);
        CHECK(out.min_version == 1 && out.max_version == 3);
        CHECK(slice_eq_str(out.client_info, "openchime-desktop/0.1 linux"));
    }
    {
        oc_welcome in = { 2, 1751200000000ull };
        ROUNDTRIP(oc_encode_welcome(&w, &in), OC_MSG_WELCOME, h, p);
        oc_welcome out;
        CHECK(oc_decode_welcome(&p, &out) == OC_OK);
        CHECK(out.chosen_version == 2 && out.server_time == 1751200000000ull);
    }
    {
        oc_reject in = { OC_ERR_VERSION_TOO_OLD, oc_slice_str("please update") };
        ROUNDTRIP(oc_encode_reject(&w, &in), OC_MSG_REJECT, h, p);
        oc_reject out;
        CHECK(oc_decode_reject(&p, &out) == OC_OK);
        CHECK(out.code == OC_ERR_VERSION_TOO_OLD);
        CHECK(slice_eq_str(out.message, "please update"));
    }
}

static void test_auth_frames(void) {
    {
        /* AUTH_CHALLENGE — the server advertises its enabled methods (a bitset)
         * plus any OIDC authorize params. */
        oc_auth_challenge in = { OC_AUTH_LOCAL | OC_AUTH_OIDC, oc_slice_str("issuer=https://c") };
        ROUNDTRIP(oc_encode_auth_challenge(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_AUTH_CHALLENGE, h, p);
        oc_auth_challenge out;
        CHECK(oc_decode_auth_challenge(&p, &out) == OC_OK);
        CHECK(out.methods == (OC_AUTH_LOCAL | OC_AUTH_OIDC));
        CHECK(slice_eq_str(out.oidc_params, "issuer=https://c"));
    }
    {
        /* AUTH — method-discriminated credential (here a local password). */
        oc_auth in = { OC_AUTH_LOCAL, oc_slice_str("alice:hunter2") };
        ROUNDTRIP(oc_encode_auth(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_AUTH, h, p);
        oc_auth out;
        CHECK(oc_decode_auth(&p, &out) == OC_OK);
        CHECK(out.method == OC_AUTH_LOCAL);
        CHECK(slice_eq_str(out.credential, "alice:hunter2"));
    }
    {
        /* AUTH_OK — carries the tenant role and a fresh 32-byte session token. */
        uint8_t token[OC_SESSION_TOKEN_LEN];
        memset(token, 0xA5, sizeof token);
        oc_auth_ok in = { 42, OC_ROLE_OWNER, 1751200999000ull,
                          { token, sizeof token } };
        ROUNDTRIP(oc_encode_auth_ok(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_AUTH_OK, h, p);
        oc_auth_ok out;
        CHECK(oc_decode_auth_ok(&p, &out) == OC_OK);
        CHECK(out.user_id == 42 && out.role == OC_ROLE_OWNER);
        CHECK(out.session_expiry == 1751200999000ull);
        CHECK(out.session_token.len == OC_SESSION_TOKEN_LEN);
        CHECK(memcmp(out.session_token.ptr, token, sizeof token) == 0);
    }
    {
        /* LOGOUT — scope + the session token to revoke. */
        uint8_t token[OC_SESSION_TOKEN_LEN];
        memset(token, 0x3C, sizeof token);
        oc_logout in = { OC_LOGOUT_THIS, { token, sizeof token } };
        ROUNDTRIP(oc_encode_logout(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_LOGOUT, h, p);
        oc_logout out;
        CHECK(oc_decode_logout(&p, &out) == OC_OK);
        CHECK(out.scope == OC_LOGOUT_THIS);
        CHECK(out.session_token.len == OC_SESSION_TOKEN_LEN);
        CHECK(memcmp(out.session_token.ptr, token, sizeof token) == 0);
    }
}

static void test_messaging_frames(void) {
    uint8_t idem[OC_IDEM_SIZE];
    for (int i = 0; i < (int)OC_IDEM_SIZE; i++) idem[i] = (uint8_t)(0xA0 + i);
    {
        oc_send in; in.channel_id = 7; memcpy(in.idem, idem, OC_IDEM_SIZE);
        in.body = oc_slice_str("hello channel");
        ROUNDTRIP(oc_encode_send(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SEND, h, p);
        oc_send out;
        CHECK(oc_decode_send(&p, &out) == OC_OK);
        CHECK(out.channel_id == 7);
        CHECK(memcmp(out.idem, idem, OC_IDEM_SIZE) == 0);
        CHECK(slice_eq_str(out.body, "hello channel"));
    }
    {
        oc_send_ack in; memcpy(in.idem, idem, OC_IDEM_SIZE);
        in.channel_id = 7; in.message_id = 1001; in.server_time = 1751200500000ull;
        ROUNDTRIP(oc_encode_send_ack(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SEND_ACK, h, p);
        oc_send_ack out;
        CHECK(oc_decode_send_ack(&p, &out) == OC_OK);
        CHECK(memcmp(out.idem, idem, OC_IDEM_SIZE) == 0);
        CHECK(out.channel_id == 7 && out.message_id == 1001);
        CHECK(out.server_time == 1751200500000ull);
    }
    {
        oc_broadcast in = { 1001, 7, 42, 1751200500000ull, oc_slice_str("hello channel") };
        ROUNDTRIP(oc_encode_broadcast(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_BROADCAST, h, p);
        oc_broadcast out;
        CHECK(oc_decode_broadcast(&p, &out) == OC_OK);
        CHECK(out.message_id == 1001 && out.channel_id == 7 && out.author_id == 42);
        CHECK(slice_eq_str(out.body, "hello channel"));
    }
    {
        oc_client_ack in = { 7, 1001 };
        ROUNDTRIP(oc_encode_client_ack(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_CLIENT_ACK, h, p);
        oc_client_ack out;
        CHECK(oc_decode_client_ack(&p, &out) == OC_OK);
        CHECK(out.channel_id == 7 && out.message_id == 1001);
    }
}

static void test_backfill_and_error(void) {
    {
        oc_cursor cursors[3] = { {7, 1000}, {8, 0}, {9, 512} };
        oc_backfill_request in = { 3, cursors };
        ROUNDTRIP(oc_encode_backfill_request(&w, OC_PROTOCOL_VERSION, &in),
                  OC_MSG_BACKFILL_REQUEST, h, p);
        oc_cursor out[3]; uint16_t n = 0;
        CHECK(oc_decode_backfill_request(&p, out, 3, &n) == OC_OK);
        CHECK(n == 3);
        CHECK(out[0].channel_id == 7 && out[0].after_message_id == 1000);
        CHECK(out[2].channel_id == 9 && out[2].after_message_id == 512);
    }
    {   /* count == 0 (a fresh client bootstrapping) */
        oc_backfill_request in = { 0, NULL };
        ROUNDTRIP(oc_encode_backfill_request(&w, OC_PROTOCOL_VERSION, &in),
                  OC_MSG_BACKFILL_REQUEST, h, p);
        oc_cursor out[1]; uint16_t n = 99;
        CHECK(oc_decode_backfill_request(&p, out, 1, &n) == OC_OK);
        CHECK(n == 0);
    }
    {   /* cap smaller than wire count: still well-formed, count reported */
        oc_cursor cursors[3] = { {1, 1}, {2, 2}, {3, 3} };
        oc_backfill_request in = { 3, cursors };
        ROUNDTRIP(oc_encode_backfill_request(&w, OC_PROTOCOL_VERSION, &in),
                  OC_MSG_BACKFILL_REQUEST, h, p);
        oc_cursor out[2]; uint16_t n = 0;
        CHECK(oc_decode_backfill_request(&p, out, 2, &n) == OC_OK);
        CHECK(n == 3);                       /* wire count exceeds our capacity */
        CHECK(out[0].channel_id == 1 && out[1].channel_id == 2);
    }
    {
        oc_backfill_done in = { 4242 };
        ROUNDTRIP(oc_encode_backfill_done(&w, OC_PROTOCOL_VERSION, &in),
                  OC_MSG_BACKFILL_DONE, h, p);
        oc_backfill_done out;
        CHECK(oc_decode_backfill_done(&p, &out) == OC_OK);
        CHECK(out.high_water == 4242);
    }
    {
        uint8_t idem[OC_IDEM_SIZE]; memset(idem, 0x5A, sizeof idem);
        oc_error in;
        in.code = OC_ERR_SEND_RATE_LIMITED; in.fatal = 0;
        in.context.ptr = idem; in.context.len = OC_IDEM_SIZE;
        in.message = oc_slice_str("slow down");
        ROUNDTRIP(oc_encode_error(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_ERROR, h, p);
        oc_error out;
        CHECK(oc_decode_error(&p, &out) == OC_OK);
        CHECK(out.code == OC_ERR_SEND_RATE_LIMITED && out.fatal == 0);
        CHECK(out.context.len == OC_IDEM_SIZE && out.context.ptr[0] == 0x5A);
        CHECK(slice_eq_str(out.message, "slow down"));
    }
}

/* --- Size limits (§2.1) ------------------------------------------------- */
static void test_size_limits(void) {
    memset(big, 'x', sizeof big);

    /* A body at exactly MAX_BODY_SIZE fits within a frame (headroom reconciles
     * REQ-054's 64KB body with the 65KB frame cap). */
    oc_send in; in.channel_id = 1; memset(in.idem, 0, OC_IDEM_SIZE);
    in.body.ptr = big; in.body.len = OC_MAX_BODY_SIZE;
    oc_wbuf w; oc_wbuf_init(&w, frame, sizeof frame);
    CHECK(oc_encode_send(&w, OC_PROTOCOL_VERSION, &in) == OC_OK);
    CHECK(w.len <= OC_MAX_FRAME_SIZE);

    /* One byte over the body cap is rejected before any writing. */
    in.body.len = OC_MAX_BODY_SIZE + 1;
    oc_wbuf w2; oc_wbuf_init(&w2, frame, sizeof frame);
    CHECK(oc_encode_send(&w2, OC_PROTOCOL_VERSION, &in) == OC_E_BODY_TOO_LARGE);

    /* A writer with too small a buffer reports overflow, not a scribble. */
    uint8_t tiny[4];
    oc_wbuf w3; oc_wbuf_init(&w3, tiny, sizeof tiny);
    oc_welcome wel = { 1, 0 };
    CHECK(oc_encode_welcome(&w3, &wel) == OC_E_OVERFLOW);
}

/* --- Malformed frames --------------------------------------------------- */
static void test_malformed(void) {
    oc_header h; oc_rbuf p;

    /* Fewer than 8 bytes: no header. */
    uint8_t tiny[4] = {0};
    CHECK(oc_parse_frame(tiny, sizeof tiny, &h, &p) == OC_E_MALFORMED);

    /* length below the 4-byte minimum. */
    uint8_t bad_len[8] = {0,0,0,3, 0,1, 0,2};
    CHECK(oc_parse_frame(bad_len, sizeof bad_len, &h, &p) == OC_E_MALFORMED);

    /* length claiming a frame past MAX_FRAME_SIZE. */
    uint32_t huge = OC_MAX_FRAME_SIZE; /* 4 + huge > MAX_FRAME_SIZE */
    uint8_t big_len[8];
    big_len[0] = (uint8_t)(huge >> 24); big_len[1] = (uint8_t)(huge >> 16);
    big_len[2] = (uint8_t)(huge >> 8);  big_len[3] = (uint8_t)huge;
    big_len[4] = 0; big_len[5] = 1; big_len[6] = 0; big_len[7] = 2;
    CHECK(oc_parse_frame(big_len, sizeof big_len, &h, &p) == OC_E_TOO_LARGE);

    /* Header says length N but the buffer is short (incomplete frame). */
    uint8_t incomplete[8] = {0,0,0,100, 0,1, 0,0x22};
    CHECK(oc_parse_frame(incomplete, sizeof incomplete, &h, &p) == OC_E_MALFORMED);

    /* A str length prefix that overruns the payload -> decode underflow. */
    uint8_t overrun[6] = {0,1, 0,1, 0,255}; /* min_ver, max_ver, str len=255, no bytes */
    oc_rbuf pr; oc_rbuf_init(&pr, overrun, sizeof overrun);
    oc_hello hello;
    CHECK(oc_decode_hello(&pr, &hello) == OC_E_MALFORMED);

    /* Trailing bytes after a complete payload -> not this v1 layout. */
    uint8_t trailing[11] = {0,2, 0,0,0,0,0,0,0,0, 0xFF}; /* welcome payload + 1 extra */
    oc_rbuf tr; oc_rbuf_init(&tr, trailing, sizeof trailing);
    oc_welcome wel;
    CHECK(oc_decode_welcome(&tr, &wel) == OC_E_MALFORMED);
}

/* --- Version negotiation (§3.2) ----------------------------------------- */
static void test_version_negotiation(void) {
    uint16_t chosen = 0, code = 0;

    /* Overlapping ranges pick min(server_max, client_max). */
    CHECK(oc_negotiate_version(1, 3, 1, 2, &chosen, &code) == OC_OK);
    CHECK(chosen == 2);
    CHECK(oc_negotiate_version(1, 2, 1, 5, &chosen, &code) == OC_OK);
    CHECK(chosen == 2);
    CHECK(oc_negotiate_version(2, 2, 1, 2, &chosen, &code) == OC_OK);
    CHECK(chosen == 2);

    /* Client too old: its max is below the server's minimum. */
    CHECK(oc_negotiate_version(1, 1, 2, 3, &chosen, &code) == OC_E_MALFORMED);
    CHECK(code == OC_ERR_VERSION_TOO_OLD);

    /* Client too new: its min is above the server's maximum. */
    CHECK(oc_negotiate_version(4, 5, 1, 3, &chosen, &code) == OC_E_MALFORMED);
    CHECK(code == OC_ERR_VERSION_TOO_NEW);
}

int run_protocol_tests(void) {
    printf("test_protocol: primitives, handshake, auth, messaging, backfill,\n");
    printf("               error, size limits, malformed frames, version negotiation\n");
    test_primitives();
    test_handshake_frames();
    test_auth_frames();
    test_messaging_frames();
    test_backfill_and_error();
    test_size_limits();
    test_malformed();
    test_version_negotiation();
    return failures;
}
