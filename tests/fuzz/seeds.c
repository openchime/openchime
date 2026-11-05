/* build/fuzz/seeds <dir>: the harnesses' seed corpora (docs/TESTING.md,
 * "Fuzzing"). One frame per encoder, each over a zeroed message, so every
 * message type is in the corpus as the encoder writes it, into <dir>/frame/;
 * and into <dir>/framebuf/, streams of those frames under a few chunkings,
 * prefixed as tests/fuzz/framebuf.c reads them; and for each pre-authentication
 * parser, a few real inputs of its kind, into <dir>/<harness>/. The corpora checked in are this
 * output minimized (libFuzzer -merge=1); a new message type's encoder is added
 * here as well as to tests/fuzz/decoders.c.
 *
 * Generated in the encoders' order in shared/protocol.h; a typed encoder (one
 * that writes several message types) is called once per type. */

#include "protocol.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define V OC_PROTOCOL_VERSION

static char g_dir[512];
static uint8_t g_buf[OC_MAX_FRAME_SIZE];
static uint8_t g_stream[1u << 16];
static size_t g_stream_len;
static int g_written, g_failed;

static void put(const char *sub, const char *name, const uint8_t *a, size_t alen, const uint8_t *b, size_t blen) {
    char path[700];
    snprintf(path, sizeof path, "%s/%s/%s", g_dir, sub, name);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(a, 1, alen, f) != alen || (blen && fwrite(b, 1, blen, f) != blen)) {
        fprintf(stderr, "seeds: cannot write %s\n", path);
        g_failed++;
    }
    if (f) fclose(f);
}

/* One encoded frame: into frame/, and onto the stream framebuf/ gets. */
static void frame(const char *name, oc_result rc, const oc_wbuf *w) {
    if (rc != OC_OK) { fprintf(stderr, "seeds: %s did not encode (%d)\n", name, (int)rc); g_failed++; return; }
    put("frame", name, w->data, w->len, NULL, 0);
    if (g_stream_len + w->len <= sizeof g_stream) {
        memcpy(g_stream + g_stream_len, w->data, w->len);
        g_stream_len += w->len;
    }
    g_written++;
}
#define FRAME(name, call) do { oc_wbuf w; oc_wbuf_init(&w, g_buf, sizeof g_buf); \
        oc_result rc_ = (call); frame(name, rc_, &w); } while (0)

/* The stream in `nsizes` chunk sizes, each a big-endian u16, as framebuf.c reads them. */
static void stream(const char *name, const uint16_t *sizes, size_t nsizes, size_t from, size_t len) {
    uint8_t head[17];
    head[0] = (uint8_t)(nsizes - 1);
    for (size_t i = 0; i < nsizes; i++) { head[1 + i * 2] = (uint8_t)(sizes[i] >> 8); head[2 + i * 2] = (uint8_t)sizes[i]; }
    if (from + len > g_stream_len) len = g_stream_len > from ? g_stream_len - from : 0;
    put("framebuf", name, head, 1 + nsizes * 2, g_stream + from, len);
}

/* One seed for a harness other than frame's: `n` byte runs written whole. */
static void seed(const char *harness, const char *name, const void *a, size_t alen, const void *b, size_t blen) {
    char sub[600];
    snprintf(sub, sizeof sub, "%s/%s", g_dir, harness);
    mkdir(sub, 0755);
    put(harness, name, a, alen, b, blen);
}
static void seed_str(const char *harness, const char *name, const char *text) {
    seed(harness, name, text, strlen(text), NULL, 0);
}
/* Two parts, as oc_fuzz_split reads them: the first prefixed by its length. */
static void seed_two(const char *harness, const char *name, const void *a, size_t alen, const void *b, size_t blen) {
    static uint8_t buf[8192];
    if (alen + blen + 2 > sizeof buf) return;
    buf[0] = (uint8_t)(alen >> 8); buf[1] = (uint8_t)alen;
    memcpy(buf + 2, a, alen);
    seed(harness, name, buf, alen + 2, b, blen);
}

/* The pre-authentication parsers' seeds: what each sees on a good day. */
static void preauth_seeds(void) {
    seed_str("http", "get", "GET / HTTP/1.1\r\nHost: acme.example\r\nAccept-Encoding: gzip, br\r\n\r\n");
    seed_str("http", "conditional", "GET /app.js HTTP/1.1\r\nHost: acme.example\r\nIf-None-Match: \"abc\", W/\"d\"\r\n\r\n");
    seed_str("http", "webhook-json", "POST /hooks/abc HTTP/1.1\r\nHost: acme.example\r\nContent-Type: application/json\r\n"
                                     "Content-Length: 27\r\n\r\n{\"text\":\"deploy \\u00e9 done\"}");
    seed_str("http", "webhook-plain", "POST /hooks/abc HTTP/1.1\r\nContent-Type: text/plain\r\nContent-Length: 5\r\n\r\nhello");
    seed_str("http", "form", "POST /signin HTTP/1.1\r\nHost: acme.example\r\nOrigin: https://acme.example\r\n"
                             "Content-Type: application/x-www-form-urlencoded\r\nContent-Length: 19\r\n\r\nuser=ada&pass=x%20y");
    seed_str("http", "upgrade", "GET /ws HTTP/1.1\r\nHost: acme.example\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                                "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n"
                                "Sec-WebSocket-Protocol: openchime, x\r\n\r\n");

    static const uint8_t ws_small[] = { 0x82, 0x85, 1, 2, 3, 4, 'h' ^ 1, 'e' ^ 2, 'l' ^ 3, 'l' ^ 4, 'o' ^ 1 };
    static const uint8_t ws_ping[] = { 0x89, 0x80, 9, 9, 9, 9, 0x88, 0x80, 0, 0, 0, 0 };
    static uint8_t ws_mid[4 + 4 + 200], ws_big[10 + 4 + 300];
    ws_mid[0] = 0x02; ws_mid[1] = 0x80 | 126; ws_mid[2] = 0; ws_mid[3] = 200;
    ws_big[0] = 0x82; ws_big[1] = 0x80 | 127; ws_big[8] = 1; ws_big[9] = 44;
    seed("ws", "small", ws_small, sizeof ws_small, NULL, 0);
    seed("ws", "ping-close", ws_ping, sizeof ws_ping, NULL, 0);
    seed("ws", "fragment-16", ws_mid, sizeof ws_mid, ws_small, sizeof ws_small);
    seed("ws", "len-64", ws_big, sizeof ws_big, NULL, 0);

    static const uint8_t sig[12] = { 0x0D, 0x0A, 0x0D, 0x0A, 0x00, 0x0D, 0x0A, 0x51, 0x55, 0x49, 0x54, 0x0A };
    static uint8_t v4[16 + 12], v6[16 + 36], local[16], unix_[16 + 216];
    memcpy(v4, sig, 12);  v4[12] = 0x21; v4[13] = 0x11; v4[15] = 12;
    v4[16] = 203; v4[17] = 0; v4[18] = 113; v4[19] = 7; v4[20] = 10; v4[23] = 1; v4[24] = 0x1F; v4[25] = 0x90; v4[26] = 0x01; v4[27] = 0xBB;
    memcpy(v6, sig, 12);  v6[12] = 0x21; v6[13] = 0x21; v6[15] = 36; v6[16] = 0x20; v6[17] = 0x01; v6[18] = 0x0d; v6[19] = 0xb8; v6[31] = 1;
    memcpy(local, sig, 12); local[12] = 0x20;
    memcpy(unix_, sig, 12); unix_[12] = 0x21; unix_[13] = 0x31; unix_[14] = 0; unix_[15] = 216;
    seed("proxy", "tcp4", v4, sizeof v4, "\x16\x03\x01", 3);
    seed("proxy", "tcp6", v6, sizeof v6, NULL, 0);
    seed("proxy", "local", local, sizeof local, NULL, 0);
    seed("proxy", "unix", unix_, sizeof unix_, NULL, 0);

    static const char claims[] = "{\"iss\":\"https://accounts.google.com\",\"aud\":\"client-1\",\"sub\":\"1234\","
        "\"email\":\"ada@acme.example\",\"email_verified\":\"true\",\"name\":\"Ada \\u00c9\",\"nonce\":\"n-1\","
        "\"iat\":1799999990,\"exp\":1800000300,\"hd\":\"acme.example\",\"amr\":[\"pwd\",\"mfa\"]}";
    static const char jwks[] = "{\"keys\":[{\"kty\":\"EC\",\"crv\":\"P-256\",\"kid\":\"k1\",\"use\":\"sig\","
        "\"x\":\"DqHKx-q7OAQmDqj5y9NaFRRyMFoHtdCjHSfxAGUIA98\",\"y\":\"uFFNNuB01X8MZQMhCfwGbiW8iRuQ67WxHBF0lXV9VNQ\"},"
        "{\"kty\":\"RSA\",\"kid\":\"r1\",\"n\":\"sXchDaQebHnPiGvyDOAT4saGEUetSyo9MKLOoWFsueri23bOdgWp4Dy1Wl\",\"e\":\"AQAB\"}]}";
    seed_str("json", "claims", claims);
    seed_str("json", "jwks", jwks);
    seed_str("json", "nested", "[{\"a\":[1,2,{\"b\":null}],\"c\":true,\"d\":\"\\ud83d\\ude00\\n\\\"\"},-0.5e3,18446744073709551615]");

    /* A "none" attestation, and a P-256 COSE key, as a browser makes them. */
    static const uint8_t att[] = { 0xA3, 0x63, 'f', 'm', 't', 0x64, 'n', 'o', 'n', 'e', 0x67, 'a', 't', 't', 'S', 't', 'm', 't', 0xA0,
                                   0x68, 'a', 'u', 't', 'h', 'D', 'a', 't', 'a', 0x58, 0x25,
                                   1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32, 0x45, 0,0,0,1 };
    static const uint8_t cose[] = { 0xA5, 0x01, 0x02, 0x03, 0x26, 0x20, 0x01, 0x21, 0x58, 0x20,
                                    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32,
                                    0x22, 0x58, 0x20,
                                    1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31,32 };
    seed("cbor", "attestation", att, sizeof att, NULL, 0);
    seed("cbor", "cose-key", cose, sizeof cose, NULL, 0);
    static const uint8_t misc[] = { 0x9F, 0x01, 0x82, 0x02, 0x03, 0xFF, 0xC1, 0x1A, 0x5F, 0x00, 0x00, 0x00, 0xF9, 0x3C, 0x00, 0x3B, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    seed("cbor", "kinds", misc, sizeof misc, NULL, 0);

    seed_str("jwt", "es256", "eyJhbGciOiJFUzI1NiIsInR5cCI6IkpXVCIsImtpZCI6ImxRY2pzNVBNV3RmeDdfVi1kSEFEQTNldWhHLUppb3BiOTJDNWhQclRtWU0ifQ"
             ".eyJpc3MiOiJodHRwczovL2F1dGgub3BlbmNoaW1lLmlvIiwiYXVkIjoiYWNtZS5leGFtcGxlIiwic3ViIjoiZ29vZ2xlfDQyIiwiZXhwIjoxODAwMDAwMjAwLCJpYXQiOjE3OTk5OTk5OTB9"
             ".AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA");
    seed_str("jwt", "none", "eyJhbGciOiJub25lIn0.eyJzdWIiOiJ4In0.");

    static const char cdata[] = "{\"type\":\"webauthn.create\",\"challenge\":\"Y2hhbGxlbmdl\",\"origin\":\"https://acme.example\",\"crossOrigin\":false}";
    seed_two("webauthn", "register", cdata, strlen(cdata), att, sizeof att);
    /* A sign-in (an assertion): its client data, the authenticator data
     * (rpIdHash, flags with user presence, a counter), an ES256 signature's DER
     * shape, and the P-256 COSE key above; four parts, as oc_fuzz_split reads
     * them. */
    {
        static const char gdata[] = "{\"type\":\"webauthn.get\",\"challenge\":\"Y2hhbGxlbmdl\",\"origin\":\"https://acme.example\"}";
        static uint8_t authd[37], sig[72], buf[1024];
        for (int i = 0; i < 32; i++) authd[i] = (uint8_t)(i + 1);
        authd[32] = 0x05; authd[36] = 2;
        sig[0] = 0x30; sig[1] = 0x46; sig[2] = 0x02; sig[3] = 0x21; sig[4] = 0x00;
        for (int i = 5; i < 37; i++) sig[i] = (uint8_t)(i * 7);
        sig[37] = 0x02; sig[38] = 0x21; sig[39] = 0x00;
        for (int i = 40; i < 72; i++) sig[i] = (uint8_t)(i * 3);
        const void *part[3] = { gdata, authd, sig };
        size_t plen[3] = { strlen(gdata), sizeof authd, sizeof sig }, n = 0;
        for (int k = 0; k < 3; k++) {
            buf[n++] = (uint8_t)(plen[k] >> 8); buf[n++] = (uint8_t)plen[k];
            memcpy(buf + n, part[k], plen[k]); n += plen[k];
        }
        seed("webauthn", "assert", buf, n, cose, sizeof cose);
    }
    seed_two("idtoken", "token-jwks", "eyJhbGciOiJSUzI1NiIsImtpZCI6InIxIn0.eyJzdWIiOiIxMjM0In0.c2ln", 58, jwks, strlen(jwks));
}

int main(int argc, char **argv) {
    if (argc != 2) { fprintf(stderr, "usage: seeds <dir>\n"); return 2; }
    snprintf(g_dir, sizeof g_dir, "%s", argv[1]);
    char sub[600];
    mkdir(g_dir, 0755);
    snprintf(sub, sizeof sub, "%s/frame", g_dir);    if (mkdir(sub, 0755) && errno != EEXIST) return 1;
    snprintf(sub, sizeof sub, "%s/framebuf", g_dir); if (mkdir(sub, 0755) && errno != EEXIST) return 1;

    { static oc_hello m; FRAME("hello", oc_encode_hello(&w, &m)); }
    { static oc_welcome m; FRAME("welcome", oc_encode_welcome(&w, &m)); }
    { static oc_reject m; FRAME("reject", oc_encode_reject(&w, &m)); }
    { static oc_auth_challenge m; FRAME("auth_challenge", oc_encode_auth_challenge(&w, V, &m)); }
    { static oc_auth_begin m; FRAME("auth_begin", oc_encode_auth_begin(&w, V, &m)); }
    { static oc_auth_redirect m; FRAME("auth_redirect", oc_encode_auth_redirect(&w, V, &m)); }
    { static oc_auth_device_begin m; FRAME("auth_device_begin", oc_encode_auth_device_begin(&w, V, &m)); }
    { static oc_auth_device m; FRAME("auth_device", oc_encode_auth_device(&w, V, &m)); }
    { static oc_auth_device_poll m; FRAME("auth_device_poll", oc_encode_auth_device_poll(&w, V, &m)); }
    { static oc_auth_device_token m; FRAME("auth_device_token", oc_encode_auth_device_token(&w, V, &m)); }
    { static oc_auth_continue m; FRAME("auth_continue", oc_encode_auth_continue(&w, V, &m)); }
    { static oc_auth m; FRAME("auth", oc_encode_auth(&w, V, &m)); }
    { static oc_auth_ok m; FRAME("auth_ok", oc_encode_auth_ok(&w, V, &m)); }
    { static oc_workspace_info m; FRAME("workspace_info", oc_encode_workspace_info(&w, V, &m)); }
    { static oc_workspace_settings m; FRAME("workspace_settings", oc_encode_workspace_settings(&w, V, &m)); }
    { static oc_read_receipts m; FRAME("read_receipts", oc_encode_read_receipts(&w, V, &m)); }
    { static oc_workspace_address m; FRAME("workspace_address", oc_encode_workspace_address(&w, V, &m)); }
    { static oc_server_build m; FRAME("server_build", oc_encode_server_build(&w, V, &m)); }
    { static oc_logout m; FRAME("logout", oc_encode_logout(&w, V, &m)); }
    { static oc_send m; FRAME("send", oc_encode_send(&w, V, &m)); }
    { static oc_send_ack m; FRAME("send_ack", oc_encode_send_ack(&w, V, &m)); }
    { static oc_broadcast m; FRAME("broadcast", oc_encode_broadcast(&w, V, &m)); }
    { static oc_client_ack m; FRAME("client_ack", oc_encode_client_ack(&w, V, &m)); }
    { static oc_read_cursor m; FRAME("read_cursor", oc_encode_read_cursor(&w, V, &m)); }
    { static oc_edit m; FRAME("edit", oc_encode_edit(&w, V, &m)); }
    { static oc_delete m; FRAME("delete", oc_encode_delete(&w, V, &m)); }
    { static oc_msg_edited m; FRAME("msg_edited", oc_encode_msg_edited(&w, V, &m)); }
    { static oc_msg_deleted m; FRAME("msg_deleted", oc_encode_msg_deleted(&w, V, &m)); }
    { static oc_react m; FRAME("react", oc_encode_react(&w, V, &m)); }
    { static oc_reaction_updated m; FRAME("reaction_updated", oc_encode_reaction_updated(&w, V, &m)); }
    { static oc_list_reactions m; FRAME("list_reactions", oc_encode_list_reactions(&w, V, &m)); }
    { static oc_pin m; FRAME("pin", oc_encode_pin(&w, V, &m)); }
    { static oc_pin_updated m; FRAME("pin_updated", oc_encode_pin_updated(&w, V, &m)); }
    { static oc_unfurl m; FRAME("unfurl", oc_encode_unfurl(&w, V, &m)); }
    { static oc_forward m; FRAME("forward", oc_encode_forward(&w, V, &m)); }
    { static oc_action m; FRAME("action", oc_encode_action(&w, V, &m)); }
    { static oc_post_poll m; FRAME("post_poll", oc_encode_post_poll(&w, V, &m)); }
    { static oc_vote m; FRAME("vote", oc_encode_vote(&w, V, &m)); }
    { static oc_poll_msg m; FRAME("poll", oc_encode_poll(&w, V, &m)); }
    { static oc_list_poll_voters m; FRAME("list_poll_voters", oc_encode_list_poll_voters(&w, V, &m)); }
    { static oc_edit_poll m; FRAME("edit_poll", oc_encode_edit_poll(&w, V, &m)); }
    FRAME("poll_voters", oc_encode_poll_voters(&w, V, 0, NULL, 0));
    FRAME("deactivate_self", oc_encode_deactivate_self(&w, V));
    { static oc_get_profile m; FRAME("get_profile", oc_encode_get_profile(&w, V, &m)); }
    { static oc_set_collapsed m; FRAME("set_collapsed", oc_encode_set_collapsed(&w, V, &m)); }
    { static oc_set_collapsed m; FRAME("collapsed_updated", oc_encode_collapsed_updated(&w, V, &m)); }
    { static oc_summarize m; FRAME("summarize", oc_encode_summarize(&w, V, &m)); }
    { static oc_summary m; FRAME("summary", oc_encode_summary(&w, V, &m)); }
    { static oc_summary_queued m; FRAME("summary_queued", oc_encode_summary_queued(&w, V, &m)); }
    { static oc_summary_req m; FRAME("summary_req-summary_cancel", oc_encode_summary_req(&w, V, OC_MSG_SUMMARY_CANCEL, &m)); }
    { static oc_summary_req m; FRAME("summary_req-summary_detach", oc_encode_summary_req(&w, V, OC_MSG_SUMMARY_DETACH, &m)); }
    { static oc_summary_req m; FRAME("summary_req-summary_dismiss", oc_encode_summary_req(&w, V, OC_MSG_SUMMARY_DISMISS, &m)); }
    { static oc_summary_req m; FRAME("summary_req-summary_open", oc_encode_summary_req(&w, V, OC_MSG_SUMMARY_OPEN, &m)); }
    { static oc_summary_ready m; FRAME("summary_ready", oc_encode_summary_ready(&w, V, &m)); }
    { static oc_summary_open m; FRAME("summary_open", oc_encode_summary_open(&w, V, &m)); }
    { static oc_summary_dismiss m; FRAME("summary_dismiss", oc_encode_summary_dismiss(&w, V, &m)); }
    { static oc_attach_media_set m; FRAME("attach_media_set", oc_encode_attach_media_set(&w, V, &m)); }
    { static oc_attach_media_ok m; FRAME("attach_media_ok", oc_encode_attach_media_ok(&w, V, &m)); }
    { static oc_list_pins m; FRAME("list_pins", oc_encode_list_pins(&w, V, &m)); }
    { static oc_pinned_msg m; FRAME("pinned_msg", oc_encode_pinned_msg(&w, V, &m)); }
    { static oc_pins m; FRAME("pins", oc_encode_pins(&w, V, &m)); }
    { static oc_list_members m; FRAME("list_members", oc_encode_list_members(&w, V, &m)); }
    { static oc_member_entry m; FRAME("member_entry", oc_encode_member_entry(&w, V, &m)); }
    { static oc_members m; FRAME("members", oc_encode_members(&w, V, &m)); }
    { static oc_list_files m; FRAME("list_files", oc_encode_list_files(&w, V, &m)); }
    { static oc_file_entry m; FRAME("file_entry", oc_encode_file_entry(&w, V, &m)); }
    { static oc_files m; FRAME("files", oc_encode_files(&w, V, &m)); }
    { static oc_reactions m; FRAME("reactions", oc_encode_reactions(&w, V, &m)); }
    { static oc_send_reply m; FRAME("send_reply", oc_encode_send_reply(&w, V, &m)); }
    { static oc_thread_reply m; FRAME("thread_reply", oc_encode_thread_reply(&w, V, &m)); }
    { static oc_list_thread m; FRAME("list_thread", oc_encode_list_thread(&w, V, &m)); }
    { static oc_thread m; FRAME("thread", oc_encode_thread(&w, V, &m)); }
    { static oc_thread_meta m; FRAME("thread_meta", oc_encode_thread_meta(&w, V, &m)); }
    { static oc_create_channel m; FRAME("create_channel", oc_encode_create_channel(&w, V, &m)); }
    { static oc_update_channel m; FRAME("update_channel", oc_encode_update_channel(&w, V, &m)); }
    { static oc_save_item m; FRAME("save_item", oc_encode_save_item(&w, V, &m)); }
    { static oc_saved_updated m; FRAME("saved_updated", oc_encode_saved_updated(&w, V, &m)); }
    FRAME("list_saved", oc_encode_list_saved(&w, V));
    { static oc_saved_msg m; FRAME("saved_msg", oc_encode_saved_msg(&w, V, &m)); }
    { static oc_saved m; FRAME("saved", oc_encode_saved(&w, V, &m)); }
    FRAME("list_activity", oc_encode_list_activity(&w, V, 0));
    { static oc_activity_entry m; FRAME("activity_entry", oc_encode_activity_entry(&w, V, &m)); }
    { static oc_activity m; FRAME("activity", oc_encode_activity(&w, V, &m)); }
    { static oc_channel_info m; FRAME("channel_info", oc_encode_channel_info(&w, V, &m)); }
    { static oc_mention_unresolved m; FRAME("mention_unresolved", oc_encode_mention_unresolved(&w, V, &m)); }
    FRAME("list_channels", oc_encode_list_channels(&w, V));
    FRAME("storage_status_req", oc_encode_storage_status_req(&w, V));
    { static oc_channel_list m; FRAME("channel_list", oc_encode_channel_list(&w, V, &m)); }
    { static oc_channel_ref m; FRAME("join_channel", oc_encode_join_channel(&w, V, &m)); }
    { static oc_channel_ref m; FRAME("leave_channel", oc_encode_leave_channel(&w, V, &m)); }
    { static oc_channel_member_op m; FRAME("invite_to_channel", oc_encode_invite_to_channel(&w, V, &m)); }
    { static oc_channel_member_op m; FRAME("remove_from_channel", oc_encode_remove_from_channel(&w, V, &m)); }
    { static oc_open_dm m; FRAME("open_dm", oc_encode_open_dm(&w, V, &m)); }
    { static oc_create_webhook m; FRAME("create_webhook", oc_encode_create_webhook(&w, V, &m)); }
    { static oc_webhook_info m; FRAME("webhook_info", oc_encode_webhook_info(&w, V, &m)); }
    { static oc_list_webhooks m; FRAME("list_webhooks", oc_encode_list_webhooks(&w, V, &m)); }
    { static oc_webhook_list m; FRAME("webhook_list", oc_encode_webhook_list(&w, V, &m)); }
    { static oc_delete_webhook m; FRAME("delete_webhook", oc_encode_delete_webhook(&w, V, &m)); }
    { static oc_set_webhook_state m; FRAME("set_webhook_state", oc_encode_set_webhook_state(&w, V, &m)); }
    { static oc_rotate_webhook m; FRAME("rotate_webhook", oc_encode_rotate_webhook(&w, V, &m)); }
    { static oc_set_notify_default m; FRAME("set_notify_default", oc_encode_set_notify_default(&w, V, &m)); }
    { static oc_set_avatar m; FRAME("set_avatar", oc_encode_set_avatar(&w, V, &m)); }
    { static oc_open_group_dm m; FRAME("open_group_dm", oc_encode_open_group_dm(&w, V, &m)); }
    FRAME("list_groups", oc_encode_list_groups(&w, V));
    { static oc_create_group m; FRAME("create_group", oc_encode_create_group(&w, V, &m)); }
    { static oc_update_group m; FRAME("update_group", oc_encode_update_group(&w, V, &m)); }
    { static oc_group_ref m; FRAME("delete_group", oc_encode_delete_group(&w, V, &m)); }
    { static oc_group_members_op m; FRAME("group_members_op-group_add_members", oc_encode_group_members_op(&w, V, OC_MSG_GROUP_ADD_MEMBERS, &m)); }
    { static oc_group_members_op m; FRAME("group_members_op-group_remove_members", oc_encode_group_members_op(&w, V, OC_MSG_GROUP_REMOVE_MEMBERS, &m)); }
    { static oc_group_info m; FRAME("group_info", oc_encode_group_info(&w, V, &m)); }
    { static oc_group_ref m; FRAME("group_deleted", oc_encode_group_deleted(&w, V, &m)); }
    { static oc_groups_end m; FRAME("groups_end", oc_encode_groups_end(&w, V, &m)); }
    { static oc_channel_group_op m; FRAME("channel_group_op-channel_add_group", oc_encode_channel_group_op(&w, V, OC_MSG_CHANNEL_ADD_GROUP, &m)); }
    { static oc_channel_group_op m; FRAME("channel_group_op-channel_remove_group", oc_encode_channel_group_op(&w, V, OC_MSG_CHANNEL_REMOVE_GROUP, &m)); }
    { static oc_channel_groups m; FRAME("channel_groups", oc_encode_channel_groups(&w, V, &m)); }
    { static oc_channel_via_group m; FRAME("channel_via_group", oc_encode_channel_via_group(&w, V, &m)); }
    { static oc_mention_unresolved_more m; FRAME("mention_unresolved_more", oc_encode_mention_unresolved_more(&w, V, &m)); }
    { static oc_alerts_summary m; FRAME("alerts_summary", oc_encode_alerts_summary(&w, V, &m)); }
    { static oc_restore m; FRAME("restore", oc_encode_restore(&w, V, &m)); }
    { static oc_broadcast m; FRAME("msg_restored", oc_encode_msg_restored(&w, V, &m)); }
    FRAME("alerts_list", oc_encode_alerts_list(&w, V));
    { static oc_alerts m; FRAME("alerts", oc_encode_alerts(&w, V, &m)); }
    { static oc_alert_ack m; FRAME("alert_ack", oc_encode_alert_ack(&w, V, &m)); }
    { static oc_add_emoji m; FRAME("add_emoji", oc_encode_add_emoji(&w, V, &m)); }
    { static oc_delete_emoji m; FRAME("delete_emoji", oc_encode_delete_emoji(&w, V, &m)); }
    FRAME("list_emoji", oc_encode_list_emoji(&w, V));
    { static oc_emoji_list m; FRAME("emoji_list", oc_encode_emoji_list(&w, V, &m)); }
    FRAME("list_sessions", oc_encode_list_sessions(&w, V));
    FRAME("mark_all_read", oc_encode_mark_all_read(&w, V));
    { static oc_get_channel_description m; FRAME("get_channel_description", oc_encode_get_channel_description(&w, V, &m)); }
    { static oc_channel_description m; FRAME("channel_description", oc_encode_channel_description(&w, V, &m)); }
    { static oc_session_list m; FRAME("session_list", oc_encode_session_list(&w, V, &m)); }
    FRAME("list_file_channels", oc_encode_list_file_channels(&w, V));
    { static oc_file_channels m; FRAME("file_channels", oc_encode_file_channels(&w, V, &m)); }
    { static oc_set_status m; FRAME("set_status", oc_encode_set_status(&w, V, &m)); }
    { static oc_set_profile m; FRAME("set_profile", oc_encode_set_profile(&w, V, &m)); }
    { static oc_profile_info m; FRAME("profile_info", oc_encode_profile_info(&w, V, &m)); }
    { static oc_set_mute m; FRAME("set_mute", oc_encode_set_mute(&w, V, &m)); }
    { static oc_set_read_cursor m; FRAME("set_read_cursor", oc_encode_set_read_cursor(&w, V, &m)); }
    FRAME("list_invites", oc_encode_list_invites(&w, V));
    { static oc_invite_list m; FRAME("invite_list", oc_encode_invite_list(&w, V, &m)); }
    { static oc_revoke_invite m; FRAME("revoke_invite", oc_encode_revoke_invite(&w, V, &m)); }
    { static oc_revoke_invite m; FRAME("invite_revoked", oc_encode_invite_revoked(&w, V, &m)); }
    { static oc_webhook_deleted m; FRAME("webhook_deleted", oc_encode_webhook_deleted(&w, V, &m)); }
    { static oc_set_presence m; FRAME("set_presence", oc_encode_set_presence(&w, V, &m)); }
    { static oc_presence_update m; FRAME("presence_update", oc_encode_presence_update(&w, V, &m)); }
    { static oc_typing m; FRAME("typing", oc_encode_typing(&w, V, &m)); }
    { static oc_typing_update m; FRAME("typing_update", oc_encode_typing_update(&w, V, &m)); }
    { static oc_set_notify_pref m; FRAME("set_notify_pref", oc_encode_set_notify_pref(&w, V, &m)); }
    { static oc_set_tz_offset m; FRAME("set_tz_offset", oc_encode_set_tz_offset(&w, V, &m)); }
    { static oc_set_snooze m; FRAME("set_snooze", oc_encode_set_snooze(&w, V, &m)); }
    { static oc_snooze m; FRAME("snooze", oc_encode_snooze(&w, V, &m)); }
    { static oc_schedule m; FRAME("set_schedule", oc_encode_set_schedule(&w, V, &m)); }
    { static oc_schedule m; FRAME("schedule", oc_encode_schedule(&w, V, &m)); }
    { static oc_set_keywords m; FRAME("set_keywords", oc_encode_set_keywords(&w, V, &m)); }
    { static oc_set_priority m; FRAME("set_priority", oc_encode_set_priority(&w, V, &m)); }
    { static oc_alert_prefs m; FRAME("alert_prefs", oc_encode_alert_prefs(&w, V, &m)); }
    { static oc_list_threads m; FRAME("list_threads", oc_encode_list_threads(&w, V, &m)); }
    { static oc_thread_summary m; FRAME("thread_summary", oc_encode_thread_summary(&w, V, &m)); }
    { static oc_threads m; FRAME("threads", oc_encode_threads(&w, V, &m)); }
    { static oc_set_thread_follow m; FRAME("set_thread_follow", oc_encode_set_thread_follow(&w, V, &m)); }
    { static oc_mark_thread_read m; FRAME("mark_thread_read", oc_encode_mark_thread_read(&w, V, &m)); }
    { static oc_register_device_token m; FRAME("register_device_token", oc_encode_register_device_token(&w, V, &m)); }
    FRAME("unregister_device_token", oc_encode_unregister_device_token(&w, V, oc_slice_str("token")));
    { static oc_device_token_ack m; FRAME("device_token_ack", oc_encode_device_token_ack(&w, V, &m)); }
    FRAME("list_notify_prefs", oc_encode_list_notify_prefs(&w, V));
    { static oc_notify_prefs m; FRAME("notify_prefs", oc_encode_notify_prefs(&w, V, &m)); }
    { static oc_schedule_message m; FRAME("schedule_message", oc_encode_schedule_message(&w, V, &m)); }
    FRAME("list_scheduled", oc_encode_list_scheduled(&w, V));
    { static oc_cancel_scheduled m; FRAME("cancel_scheduled", oc_encode_cancel_scheduled(&w, V, &m)); }
    { static oc_update_scheduled m; FRAME("update_scheduled", oc_encode_update_scheduled(&w, V, &m)); }
    { static oc_scheduled m; FRAME("scheduled", oc_encode_scheduled(&w, V, &m)); }
    { static oc_scheduled_list m; FRAME("scheduled_list", oc_encode_scheduled_list(&w, V, &m)); }
    { static oc_set_draft m; FRAME("set_draft", oc_encode_set_draft(&w, V, &m)); }
    FRAME("list_drafts", oc_encode_list_drafts(&w, V));
    { static oc_draft m; FRAME("draft", oc_encode_draft(&w, V, &m)); }
    { static oc_drafts m; FRAME("drafts", oc_encode_drafts(&w, V, &m)); }
    { static oc_set_client_setting m; FRAME("set_client_setting", oc_encode_set_client_setting(&w, V, &m)); }
    { static oc_list_client_settings m; FRAME("list_client_settings", oc_encode_list_client_settings(&w, V, &m)); }
    { static oc_audit_query m; FRAME("audit_query", oc_encode_audit_query(&w, V, &m)); }
    { static oc_audit_page m; FRAME("audit_page", oc_encode_audit_page(&w, V, &m)); }
    { static oc_storage_status m; FRAME("storage_status", oc_encode_storage_status(&w, V, &m)); }
    { static oc_client_settings m; FRAME("client_settings", oc_encode_client_settings(&w, V, &m)); }
    { static oc_call_join m; FRAME("call_join", oc_encode_call_join(&w, V, &m)); }
    { static oc_call_leave m; FRAME("call_leave", oc_encode_call_leave(&w, V, &m)); }
    { static oc_call_joined m; FRAME("call_joined", oc_encode_call_joined(&w, V, &m)); }
    { static oc_call_roster m; FRAME("call_roster", oc_encode_call_roster(&w, V, &m)); }
    { static oc_call_invite m; FRAME("call_invite", oc_encode_call_invite(&w, V, &m)); }
    { static oc_call_decline m; FRAME("call_decline", oc_encode_call_decline(&w, V, &m)); }
    { static oc_call_end m; FRAME("call_end", oc_encode_call_end(&w, V, &m)); }
    { static oc_call_state m; FRAME("call_state", oc_encode_call_state(&w, V, &m)); }
    { static oc_call_key m; FRAME("call_key", oc_encode_call_key(&w, V, &m)); }
    { static oc_call_key_for m; FRAME("call_key_for", oc_encode_call_key_for(&w, V, &m)); }
    { static oc_pdm_open m; FRAME("pdm_open", oc_encode_pdm_open(&w, V, &m)); }
    { static oc_pdm_opened m; FRAME("pdm_opened", oc_encode_pdm_opened(&w, V, &m)); }
    { static oc_pdm_invite m; FRAME("pdm_invite", oc_encode_pdm_invite(&w, V, &m)); }
    { static oc_pdm_hs m; FRAME("pdm_hs-pdm_hs", oc_encode_pdm_hs(&w, V, OC_MSG_PDM_HS, &m)); }
    { static oc_pdm_hs m; FRAME("pdm_hs-pdm_hs_for", oc_encode_pdm_hs(&w, V, OC_MSG_PDM_HS_FOR, &m)); }
    { static oc_pdm_msg m; FRAME("pdm_msg-pdm_msg", oc_encode_pdm_msg(&w, V, OC_MSG_PDM_MSG, &m)); }
    { static oc_pdm_msg m; FRAME("pdm_msg-pdm_send", oc_encode_pdm_msg(&w, V, OC_MSG_PDM_SEND, &m)); }
    { static oc_pdm_end m; FRAME("pdm_end", oc_encode_pdm_end(&w, V, &m)); }
    { static oc_call_share m; FRAME("call_share", oc_encode_call_share(&w, V, &m)); }
    { static oc_call_media_pkt m; FRAME("call_media_up", oc_encode_call_media_up(&w, V, &m)); }
    { static oc_call_media_pkt m; FRAME("call_media_down", oc_encode_call_media_down(&w, V, &m)); }
    { static oc_upload_begin m; FRAME("upload_begin", oc_encode_upload_begin(&w, V, &m)); }
    { static oc_upload_ready m; FRAME("upload_ready", oc_encode_upload_ready(&w, V, &m)); }
    { static oc_upload_chunk m; FRAME("upload_chunk", oc_encode_upload_chunk(&w, V, &m)); }
    { static oc_upload_ack m; FRAME("upload_ack", oc_encode_upload_ack(&w, V, &m)); }
    { static oc_upload_end m; FRAME("upload_end", oc_encode_upload_end(&w, V, &m)); }
    { static oc_upload_ok m; FRAME("upload_ok", oc_encode_upload_ok(&w, V, &m)); }
    { static oc_tts_info m; FRAME("tts_info", oc_encode_tts_info(&w, V, &m)); }
    { static oc_audio_get m; FRAME("audio_get", oc_encode_audio_get(&w, V, &m)); }
    { static oc_audio_info m; FRAME("audio_info", oc_encode_audio_info(&w, V, &m)); }
    { static oc_audio_chunk m; FRAME("audio_chunk", oc_encode_audio_chunk(&w, V, &m)); }
    { static oc_audio_end m; FRAME("audio_end", oc_encode_audio_end(&w, V, &m)); }
    { static oc_capabilities m; FRAME("capabilities", oc_encode_capabilities(&w, V, &m)); }
    { static oc_voice_preview_get m; FRAME("voice_preview_get", oc_encode_voice_preview_get(&w, V, &m)); }
    { static oc_stt_info m; FRAME("stt_info", oc_encode_stt_info(&w, V, &m)); }
    { static oc_stt_begin m; FRAME("stt_begin", oc_encode_stt_begin(&w, V, &m)); }
    { static oc_stt_chunk m; FRAME("stt_chunk", oc_encode_stt_chunk(&w, V, &m)); }
    { static oc_stt_end m; FRAME("stt_end", oc_encode_stt_end(&w, V, &m)); }
    { static oc_stt_text m; FRAME("stt_text", oc_encode_stt_text(&w, V, &m)); }
    { static oc_download_begin m; FRAME("download_begin", oc_encode_download_begin(&w, V, &m)); }
    { static oc_download_info m; FRAME("download_info", oc_encode_download_info(&w, V, &m)); }
    { static oc_download_chunk m; FRAME("download_chunk", oc_encode_download_chunk(&w, V, &m)); }
    { static oc_download_end m; FRAME("download_end", oc_encode_download_end(&w, V, &m)); }
    { static oc_transfer_cancel m; FRAME("transfer_cancel", oc_encode_transfer_cancel(&w, V, &m)); }
    FRAME("list_users", oc_encode_list_users(&w, V));
    { static oc_user_list m; FRAME("user_list", oc_encode_user_list(&w, V, &m)); }
    { static oc_set_role m; FRAME("set_role", oc_encode_set_role(&w, V, &m)); }
    { static oc_invite_user m; FRAME("invite_user", oc_encode_invite_user(&w, V, &m)); }
    { static oc_remove_user m; FRAME("remove_user", oc_encode_remove_user(&w, V, &m)); }
    { static oc_reset_credential m; FRAME("reset_credential", oc_encode_reset_credential(&w, V, &m)); }
    { static oc_credential_reset m; FRAME("credential_reset", oc_encode_credential_reset(&w, V, &m)); }
    { static oc_enable_user m; FRAME("enable_user", oc_encode_enable_user(&w, V, &m)); }
    { static oc_revoke_session m; FRAME("revoke_session", oc_encode_revoke_session(&w, V, &m)); }
    { static oc_set_email m; FRAME("set_email", oc_encode_set_email(&w, V, &m)); }
    { static oc_user_updated m; FRAME("user_updated", oc_encode_user_updated(&w, V, &m)); }
    { static oc_invite_created m; FRAME("invite_created", oc_encode_invite_created(&w, V, &m)); }
    { static oc_set_display_name m; FRAME("set_display_name", oc_encode_set_display_name(&w, V, &m)); }
    { static oc_profile_updated m; FRAME("profile_updated", oc_encode_profile_updated(&w, V, &m)); }
    { static oc_search m; FRAME("search", oc_encode_search(&w, V, &m)); }
    { static oc_search_results m; FRAME("search_results", oc_encode_search_results(&w, V, &m)); }
    { static oc_backfill_request m; FRAME("backfill_request", oc_encode_backfill_request(&w, V, &m)); }
    { static oc_history_request m; FRAME("history_request", oc_encode_history_request(&w, V, &m)); }
    { static oc_history_around m; FRAME("history_around", oc_encode_history_around(&w, V, &m)); }
    { static oc_backfill_done m; FRAME("backfill_done", oc_encode_backfill_done(&w, V, &m)); }
    { static oc_error m; FRAME("error", oc_encode_error(&w, V, &m)); }

    static const uint16_t one[] = { 1 }, small[] = { 3, 7, 64 }, whole[] = { 16384 }, mixed[] = { 9, 4096, 2, 300 };
    stream("bytewise", one, 1, 0, 600);
    stream("small", small, 3, 0, 4000);
    stream("whole", whole, 1, 0, g_stream_len);
    stream("mixed", mixed, 4, 200, 8000);
    preauth_seeds();
    printf("seeds: %d frames, 4 streams, into %s\n", g_written, g_dir);
    return g_failed ? 1 : 0;
}
