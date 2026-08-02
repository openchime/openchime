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
        /* Frozen at 1, asserted as the literal rather than via the constant:
         * the point is that this value does NOT track OC_PROTOCOL_VERSION, and
         * an assertion written in terms of the constant would follow it. */
        CHECK(h.version == 1);
        CHECK(OC_HANDSHAKE_VERSION == 1);
        oc_hello out;
        CHECK(oc_decode_hello(&p, &out) == OC_OK);
        CHECK(out.min_version == 1 && out.max_version == 3);
        CHECK(slice_eq_str(out.client_info, "openchime-desktop/0.1 linux"));
    }
    {
        oc_welcome in = { 2, 1751200000000ull };
        ROUNDTRIP(oc_encode_welcome(&w, &in), OC_MSG_WELCOME, h, p);
        CHECK(h.version == 1);   /* frozen, and independent of chosen_version */
        oc_welcome out;
        CHECK(oc_decode_welcome(&p, &out) == OC_OK);
        CHECK(out.chosen_version == 2 && out.server_time == 1751200000000ull);
    }
    {
        oc_reject in = { OC_ERR_VERSION_TOO_OLD, oc_slice_str("please update") };
        ROUNDTRIP(oc_encode_reject(&w, &in), OC_MSG_REJECT, h, p);
        /* REJECT matters most: it is the frame sent TO a peer whose version we
         * have just refused, so it is the one that must be decodable by a peer
         * that agrees with us about nothing else. */
        CHECK(h.version == 1);
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
        /* WORKSPACE_INFO — deployment mode + user cap + (optional) name. */
        oc_workspace_info in = { 2 /* managed */, 500, oc_slice_str("Acme HQ") };
        ROUNDTRIP(oc_encode_workspace_info(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_WORKSPACE_INFO, h, p);
        oc_workspace_info out;
        CHECK(oc_decode_workspace_info(&p, &out) == OC_OK);
        CHECK(out.deployment_mode == 2 && out.max_users == 500);
        CHECK(slice_eq_str(out.workspace_name, "Acme HQ"));
    }
    {
        /* WORKSPACE_INFO — empty name (client derives from the host subdomain). */
        oc_workspace_info in = { 0, 0, oc_slice_str("") };
        ROUNDTRIP(oc_encode_workspace_info(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_WORKSPACE_INFO, h, p);
        oc_workspace_info out;
        CHECK(oc_decode_workspace_info(&p, &out) == OC_OK);
        CHECK(out.deployment_mode == 0 && out.max_users == 0);
        CHECK(slice_eq_str(out.workspace_name, ""));
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
        oc_send in = {0}; in.channel_id = 7; memcpy(in.idem, idem, OC_IDEM_SIZE);
        in.body = oc_slice_str("hello channel");
        ROUNDTRIP(oc_encode_send(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SEND, h, p);
        oc_send out = {0};
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
        oc_broadcast in = { 1001, 7, 42, 1751200500000ull, oc_slice_str("hello channel"), 0, {{0}}, {0} };
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
    {
        oc_edit in = { 7, 1001, oc_slice_str("edited body") };
        ROUNDTRIP(oc_encode_edit(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_EDIT, h, p);
        oc_edit out;
        CHECK(oc_decode_edit(&p, &out) == OC_OK);
        CHECK(out.channel_id == 7 && out.message_id == 1001);
        CHECK(slice_eq_str(out.body, "edited body"));
    }
    {
        oc_delete in = { 7, 1001 };
        ROUNDTRIP(oc_encode_delete(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_DELETE, h, p);
        oc_delete out;
        CHECK(oc_decode_delete(&p, &out) == OC_OK);
        CHECK(out.channel_id == 7 && out.message_id == 1001);
    }
    {
        oc_msg_edited in = { 1001, 7, 42, 1751200500000ull, oc_slice_str("edited body") };
        ROUNDTRIP(oc_encode_msg_edited(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_MSG_EDITED, h, p);
        oc_msg_edited out;
        CHECK(oc_decode_msg_edited(&p, &out) == OC_OK);
        CHECK(out.message_id == 1001 && out.channel_id == 7 && out.author_id == 42);
        CHECK(out.edited_at == 1751200500000ull);
        CHECK(slice_eq_str(out.body, "edited body"));
    }
    {
        oc_msg_deleted in = { 1001, 7, 42, 9, 1751200500000ull };
        ROUNDTRIP(oc_encode_msg_deleted(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_MSG_DELETED, h, p);
        oc_msg_deleted out;
        CHECK(oc_decode_msg_deleted(&p, &out) == OC_OK);
        CHECK(out.message_id == 1001 && out.channel_id == 7 && out.author_id == 42);
        CHECK(out.deleted_by == 9 && out.deleted_at == 1751200500000ull);
    }
}

static void test_thread_frames(void) {
    uint8_t idem[OC_IDEM_SIZE];
    for (int i = 0; i < (int)OC_IDEM_SIZE; i++) idem[i] = (uint8_t)(0xC0 + i);
    {
        oc_send_reply in = {0}; in.channel_id = 7; memcpy(in.idem, idem, OC_IDEM_SIZE);
        in.parent_id = 500; in.body = oc_slice_str("a reply");
        in.n_attach = 1; in.attach_ids[0] = 321;   /* REQ-140 thread attachment */
        ROUNDTRIP(oc_encode_send_reply(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SEND_REPLY, h, p);
        oc_send_reply out = {0};
        CHECK(oc_decode_send_reply(&p, &out) == OC_OK);
        CHECK(out.channel_id == 7 && out.parent_id == 500);
        CHECK(memcmp(out.idem, idem, OC_IDEM_SIZE) == 0);
        CHECK(slice_eq_str(out.body, "a reply"));
        CHECK(out.n_attach == 1 && out.attach_ids[0] == 321);
    }
    {
        oc_thread_reply in = { 1002, 7, 500, 42, 1751200500000ull, 3, oc_slice_str("a reply"), 0, {{0}} };
        in.n_attach = 1; in.attach[0].id = 321; in.attach[0].filename = oc_slice_str("t.pdf");
        in.attach[0].mime = oc_slice_str("application/pdf"); in.attach[0].size = 88;
        ROUNDTRIP(oc_encode_thread_reply(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_THREAD_REPLY, h, p);
        oc_thread_reply out = {0};
        CHECK(oc_decode_thread_reply(&p, &out) == OC_OK);
        CHECK(out.message_id == 1002 && out.channel_id == 7 && out.parent_id == 500);
        CHECK(out.author_id == 42 && out.server_time == 1751200500000ull && out.reply_count == 3);
        CHECK(slice_eq_str(out.body, "a reply"));
        CHECK(out.n_attach == 1 && out.attach[0].id == 321 && out.attach[0].size == 88);
        CHECK(slice_eq_str(out.attach[0].filename, "t.pdf"));
    }
    {
        oc_list_thread in = { 7, 500 };
        ROUNDTRIP(oc_encode_list_thread(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_LIST_THREAD, h, p);
        oc_list_thread out;
        CHECK(oc_decode_list_thread(&p, &out) == OC_OK);
        CHECK(out.channel_id == 7 && out.parent_id == 500);
    }
    {
        oc_thread in = { 500, 3, 1 };
        ROUNDTRIP(oc_encode_thread(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_THREAD, h, p);
        oc_thread out;
        CHECK(oc_decode_thread(&p, &out) == OC_OK);
        CHECK(out.truncated == 1);
        CHECK(out.parent_id == 500 && out.count == 3);
    }
    {
        oc_thread_meta in = { 500, 3, 1751200500000ull };
        ROUNDTRIP(oc_encode_thread_meta(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_THREAD_META, h, p);
        oc_thread_meta out;
        CHECK(oc_decode_thread_meta(&p, &out) == OC_OK);
        CHECK(out.message_id == 500 && out.reply_count == 3 && out.last_reply_at == 1751200500000ull);
    }
}

static void test_reaction_frames(void) {
    {
        oc_react in = { 7, 1001, oc_slice_str("\xF0\x9F\x91\x8D"), OC_REACT_ADD };
        ROUNDTRIP(oc_encode_react(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_REACT, h, p);
        oc_react out;
        CHECK(oc_decode_react(&p, &out) == OC_OK);
        CHECK(out.channel_id == 7 && out.message_id == 1001 && out.op == OC_REACT_ADD);
        CHECK(slice_eq_str(out.emoji, "\xF0\x9F\x91\x8D"));
    }
    {
        oc_reaction_updated in = { 1001, 7, 42, oc_slice_str(":tada:"), OC_REACT_ADD, 3 };
        ROUNDTRIP(oc_encode_reaction_updated(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_REACTION_UPDATED, h, p);
        oc_reaction_updated out;
        CHECK(oc_decode_reaction_updated(&p, &out) == OC_OK);
        CHECK(out.message_id == 1001 && out.channel_id == 7 && out.user_id == 42);
        CHECK(slice_eq_str(out.emoji, ":tada:") && out.op == OC_REACT_ADD && out.count == 3);
    }
    {
        oc_list_reactions in = { 7, 1001 };
        ROUNDTRIP(oc_encode_list_reactions(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_LIST_REACTIONS, h, p);
        oc_list_reactions out;
        CHECK(oc_decode_list_reactions(&p, &out) == OC_OK);
        CHECK(out.channel_id == 7 && out.message_id == 1001);
    }
    {
        oc_reaction_entry ents[2] = {
            { oc_slice_str(":+1:"),   5 },
            { oc_slice_str(":tada:"), 9 },
        };
        oc_reactions in = { 1001, 2, ents };
        ROUNDTRIP(oc_encode_reactions(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_REACTIONS, h, p);
        oc_reaction_entry out[2]; uint16_t n = 0; uint64_t mid = 0;
        CHECK(oc_decode_reactions(&p, out, 2, &n, &mid) == OC_OK);
        CHECK(mid == 1001 && n == 2);
        CHECK(slice_eq_str(out[0].emoji, ":+1:") && out[0].user_id == 5);
        CHECK(slice_eq_str(out[1].emoji, ":tada:") && out[1].user_id == 9);
    }
}

static void test_channel_frames(void) {
    {
        oc_create_channel in = { oc_slice_str("engineering"), 0 };
        ROUNDTRIP(oc_encode_create_channel(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_CREATE_CHANNEL, h, p);
        oc_create_channel out;
        CHECK(oc_decode_create_channel(&p, &out) == OC_OK);
        CHECK(slice_eq_str(out.name, "engineering") && out.is_public == 0);
    }
    {
        oc_channel_info in = { 5, OC_CHANNEL_KIND, oc_slice_str("engineering"), 0, 1, 1751200500000ull, 0 };
        ROUNDTRIP(oc_encode_channel_info(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_CHANNEL_INFO, h, p);
        oc_channel_info out;
        CHECK(oc_decode_channel_info(&p, &out) == OC_OK);
        CHECK(out.channel_id == 5 && out.kind == OC_CHANNEL_KIND);
        CHECK(slice_eq_str(out.name, "engineering"));
        CHECK(out.is_public == 0 && out.joined == 1 && out.created_at == 1751200500000ull);
    }
    {
        oc_wbuf w; oc_wbuf_init(&w, frame, sizeof frame);
        CHECK(oc_encode_list_channels(&w, OC_PROTOCOL_VERSION) == OC_OK);
        oc_header h; oc_rbuf p;
        CHECK(oc_parse_frame(frame, w.len, &h, &p) == OC_OK);
        CHECK(h.msg_type == OC_MSG_LIST_CHANNELS);
        CHECK(oc_decode_list_channels(&p) == OC_OK);
    }
    {
        oc_channel_list_entry ents[2] = {
            { 1, oc_slice_str("general"),   1, 1, OC_CHANNEL_KIND },
            { 9, oc_slice_str(""),          0, 1, OC_CHANNEL_KIND_DM },
        };
        oc_channel_list in = { 2, ents };
        ROUNDTRIP(oc_encode_channel_list(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_CHANNEL_LIST, h, p);
        oc_channel_list_entry out[2]; uint16_t n = 0;
        CHECK(oc_decode_channel_list(&p, out, 2, &n) == OC_OK);
        CHECK(n == 2);
        CHECK(out[0].channel_id == 1 && slice_eq_str(out[0].name, "general") && out[0].kind == OC_CHANNEL_KIND);
        CHECK(out[1].channel_id == 9 && out[1].joined == 1 && out[1].kind == OC_CHANNEL_KIND_DM);
    }
    {
        oc_open_dm in = { 42 };
        ROUNDTRIP(oc_encode_open_dm(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_OPEN_DM, h, p);
        oc_open_dm out;
        CHECK(oc_decode_open_dm(&p, &out) == OC_OK);
        CHECK(out.user_id == 42);
    }
    {
        oc_channel_ref in = { 5 };
        ROUNDTRIP(oc_encode_join_channel(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_JOIN_CHANNEL, h, p);
        oc_channel_ref out;
        CHECK(oc_decode_join_channel(&p, &out) == OC_OK);
        CHECK(out.channel_id == 5);
    }
    {
        oc_channel_ref in = { 5 };
        ROUNDTRIP(oc_encode_leave_channel(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_LEAVE_CHANNEL, h, p);
        oc_channel_ref out;
        CHECK(oc_decode_leave_channel(&p, &out) == OC_OK);
        CHECK(out.channel_id == 5);
    }
    {
        oc_channel_member_op in = { 5, 42 };
        ROUNDTRIP(oc_encode_invite_to_channel(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_INVITE_TO_CHANNEL, h, p);
        oc_channel_member_op out;
        CHECK(oc_decode_invite_to_channel(&p, &out) == OC_OK);
        CHECK(out.channel_id == 5 && out.user_id == 42);
    }
    {
        oc_channel_member_op in = { 5, 42 };
        ROUNDTRIP(oc_encode_remove_from_channel(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_REMOVE_FROM_CHANNEL, h, p);
        oc_channel_member_op out;
        CHECK(oc_decode_remove_from_channel(&p, &out) == OC_OK);
        CHECK(out.channel_id == 5 && out.user_id == 42);
    }
}

static void test_admin_frames(void) {
    {
        oc_wbuf w; oc_wbuf_init(&w, frame, sizeof frame);
        CHECK(oc_encode_list_users(&w, OC_PROTOCOL_VERSION) == OC_OK);
        oc_header h; oc_rbuf p;
        CHECK(oc_parse_frame(frame, w.len, &h, &p) == OC_OK);
        CHECK(h.msg_type == OC_MSG_LIST_USERS);
        CHECK(oc_decode_list_users(&p) == OC_OK);
    }
    {
        oc_user_list_entry ents[2] = {
            { 1, OC_ROLE_OWNER,  0, oc_slice_str("a@x.io"), oc_slice_str("Alice") },
            { 2, OC_ROLE_MEMBER, 1, oc_slice_str(""),       oc_slice_str("Bob") },
        };
        oc_user_list in = { 2, ents };
        ROUNDTRIP(oc_encode_user_list(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_USER_LIST, h, p);
        oc_user_list_entry out[2]; uint16_t n = 0;
        CHECK(oc_decode_user_list(&p, out, 2, &n) == OC_OK);
        CHECK(n == 2);
        CHECK(out[0].user_id == 1 && out[0].role == OC_ROLE_OWNER && out[0].disabled == 0);
        CHECK(slice_eq_str(out[0].email, "a@x.io") && slice_eq_str(out[0].display_name, "Alice"));
        CHECK(out[1].user_id == 2 && out[1].disabled == 1 && slice_eq_str(out[1].display_name, "Bob"));
    }
    {
        oc_set_role in = { 7, OC_ROLE_ADMIN };
        ROUNDTRIP(oc_encode_set_role(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SET_ROLE, h, p);
        oc_set_role out;
        CHECK(oc_decode_set_role(&p, &out) == OC_OK);
        CHECK(out.user_id == 7 && out.role == OC_ROLE_ADMIN);
    }
    {
        oc_invite_user in = { OC_ROLE_ADMIN };
        ROUNDTRIP(oc_encode_invite_user(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_INVITE_USER, h, p);
        oc_invite_user out;
        CHECK(oc_decode_invite_user(&p, &out) == OC_OK);
        CHECK(out.role == OC_ROLE_ADMIN);
    }
    {
        oc_remove_user in = { 42 };
        ROUNDTRIP(oc_encode_remove_user(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_REMOVE_USER, h, p);
        oc_remove_user out;
        CHECK(oc_decode_remove_user(&p, &out) == OC_OK);
        CHECK(out.user_id == 42);
    }
    {
        oc_user_updated in = { 42, OC_ROLE_MEMBER, 1 };
        ROUNDTRIP(oc_encode_user_updated(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_USER_UPDATED, h, p);
        oc_user_updated out;
        CHECK(oc_decode_user_updated(&p, &out) == OC_OK);
        CHECK(out.user_id == 42 && out.role == OC_ROLE_MEMBER && out.disabled == 1);
    }
    {
        uint8_t tok[OC_INVITE_TOKEN_LEN];
        for (int i = 0; i < (int)OC_INVITE_TOKEN_LEN; i++) tok[i] = (uint8_t)(i + 3);
        oc_invite_created in = { { tok, OC_INVITE_TOKEN_LEN }, OC_ROLE_MEMBER, 1751200500000ull };
        ROUNDTRIP(oc_encode_invite_created(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_INVITE_CREATED, h, p);
        oc_invite_created out;
        CHECK(oc_decode_invite_created(&p, &out) == OC_OK);
        CHECK(out.token.len == OC_INVITE_TOKEN_LEN && out.token.ptr[0] == 3);
        CHECK(out.role == OC_ROLE_MEMBER && out.expires_at == 1751200500000ull);
    }
    {
        uint8_t tok[OC_INVITE_TOKEN_LEN]; memset(tok, 0x9A, sizeof tok);
        oc_redeem_invite in = { { tok, OC_INVITE_TOKEN_LEN }, oc_slice_str("newbie"), oc_slice_str("hunter2") };
        ROUNDTRIP(oc_encode_redeem_invite(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_REDEEM_INVITE, h, p);
        oc_redeem_invite out;
        CHECK(oc_decode_redeem_invite(&p, &out) == OC_OK);
        CHECK(out.token.len == OC_INVITE_TOKEN_LEN && out.token.ptr[0] == 0x9A);
        CHECK(slice_eq_str(out.username, "newbie") && slice_eq_str(out.password, "hunter2"));
    }
}

static void test_search_frames(void) {
    {
        oc_search in = { oc_slice_str("deploy pipeline"), 25 };
        ROUNDTRIP(oc_encode_search(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SEARCH, h, p);
        oc_search out;
        CHECK(oc_decode_search(&p, &out) == OC_OK);
        CHECK(slice_eq_str(out.query, "deploy pipeline") && out.limit == 25);
    }
    {
        oc_search_result_entry ents[2] = {
            { 1001, 7, 42, 1751200500000ull, oc_slice_str("... the deploy ...") },
            { 990,  7, 43, 1751200400000ull, oc_slice_str("deploy again") },
        };
        oc_search_results in = { 2, ents, 1 };
        ROUNDTRIP(oc_encode_search_results(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SEARCH_RESULTS, h, p);
        oc_search_result_entry out[2]; uint16_t n = 0; uint8_t trunc = 0;
        CHECK(oc_decode_search_results(&p, out, 2, &n, &trunc) == OC_OK);
        CHECK(n == 2 && trunc == 1);
        CHECK(out[0].message_id == 1001 && out[0].channel_id == 7 && out[0].author_id == 42);
        CHECK(out[0].server_time == 1751200500000ull && slice_eq_str(out[0].snippet, "... the deploy ..."));
        CHECK(out[1].message_id == 990 && slice_eq_str(out[1].snippet, "deploy again"));
    }
}

static void test_presence_frames(void) {
    {
        oc_set_presence in = { OC_PRESENCE_AWAY };
        ROUNDTRIP(oc_encode_set_presence(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SET_PRESENCE, h, p);
        oc_set_presence out;
        CHECK(oc_decode_set_presence(&p, &out) == OC_OK && out.status == OC_PRESENCE_AWAY);
    }
    {
        oc_presence_update in = { 42, OC_PRESENCE_ONLINE, 0 };
        ROUNDTRIP(oc_encode_presence_update(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_PRESENCE_UPDATE, h, p);
        oc_presence_update out;
        CHECK(oc_decode_presence_update(&p, &out) == OC_OK);
        CHECK(out.user_id == 42 && out.status == OC_PRESENCE_ONLINE && out.dnd == 0);
    }
    {   /* The do-not-disturb FACT rides beside presence (REQ-122/278) — and it
         * is a fact, never an instant: there is nowhere in this frame to put a
         * time, which is the design rather than an omission. */
        oc_presence_update in = { 42, OC_PRESENCE_AWAY, 1 };
        ROUNDTRIP(oc_encode_presence_update(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_PRESENCE_UPDATE, h, p);
        oc_presence_update out;
        CHECK(oc_decode_presence_update(&p, &out) == OC_OK);
        CHECK(out.user_id == 42 && out.status == OC_PRESENCE_AWAY && out.dnd == 1);
    }
    {   /* Pausing (REQ-278): minutes from now on the way in, an absolute instant
         * on the way back, and 0 in either direction means "not paused". */
        oc_set_snooze in = { 30 };
        ROUNDTRIP(oc_encode_set_snooze(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SET_SNOOZE, h, p);
        oc_set_snooze out;
        CHECK(oc_decode_set_snooze(&p, &out) == OC_OK && out.minutes == 30);
    }
    {
        oc_set_snooze in = { 0 };
        ROUNDTRIP(oc_encode_set_snooze(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SET_SNOOZE, h, p);
        oc_set_snooze out;
        CHECK(oc_decode_set_snooze(&p, &out) == OC_OK && out.minutes == 0);
    }
    {
        oc_snooze in = { 1785620669000ull };
        ROUNDTRIP(oc_encode_snooze(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SNOOZE, h, p);
        oc_snooze out;
        CHECK(oc_decode_snooze(&p, &out) == OC_OK && out.until_ms == 1785620669000ull);
    }
    {
        oc_typing in = { 7 };
        ROUNDTRIP(oc_encode_typing(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_TYPING, h, p);
        oc_typing out;
        CHECK(oc_decode_typing(&p, &out) == OC_OK && out.channel_id == 7);
    }
    {
        oc_typing_update in = { 7, 42 };
        ROUNDTRIP(oc_encode_typing_update(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_TYPING_UPDATE, h, p);
        oc_typing_update out;
        CHECK(oc_decode_typing_update(&p, &out) == OC_OK);
        CHECK(out.channel_id == 7 && out.user_id == 42);
    }
}

static void test_notify_frames(void) {
    {
        oc_set_notify_pref in = { 7, OC_NOTIFY_MENTIONS };
        ROUNDTRIP(oc_encode_set_notify_pref(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SET_NOTIFY_PREF, h, p);
        oc_set_notify_pref out;
        CHECK(oc_decode_set_notify_pref(&p, &out) == OC_OK && out.channel_id == 7 && out.level == OC_NOTIFY_MENTIONS);
    }
    {   /* The SCHEDULE (REQ-136), which replaced the single window. Its range is
         * the ALLOWED hours, and the offset rides with it because a per-weekday
         * window without one is a window on the wrong day for half the world. */
        oc_schedule_day days_in[3] = { { 1, 1, 540, 1020 }, { 3, 0, 0, 0 }, { 6, 1, 600, 720 } };
        oc_schedule in = { OC_DND_CUSTOM, -480, 480, 1320, 3, days_in };
        ROUNDTRIP(oc_encode_set_schedule(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SET_SCHEDULE, h, p);
        oc_schedule out; oc_schedule_day days[OC_SCHEDULE_DAYS];
        CHECK(oc_decode_schedule(&p, &out, days) == OC_OK);
        CHECK(out.mode == OC_DND_CUSTOM && out.tz_offset_min == -480);
        CHECK(out.start_min == 480 && out.end_min == 1320 && out.count == 3);
        CHECK(days[0].weekday == 1 && days[0].start_min == 540 && days[0].end_min == 1020);
        CHECK(days[1].weekday == 3 && days[1].enabled == 0);
        CHECK(days[2].weekday == 6 && days[2].enabled == 1 && days[2].end_min == 720);
    }
    {   /* A NEGATIVE offset is the common case west of Greenwich, so it is the
         * one worth pinning: it travels as two's complement in a u16. */
        oc_schedule in = { OC_DND_EVERY_DAY, -330, 480, 1320, 0, NULL };
        ROUNDTRIP(oc_encode_schedule(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SCHEDULE, h, p);
        oc_schedule out; oc_schedule_day days[OC_SCHEDULE_DAYS];
        CHECK(oc_decode_schedule(&p, &out, days) == OC_OK);
        CHECK(out.tz_offset_min == -330 && out.count == 0 && out.mode == OC_DND_EVERY_DAY);
    }
    {   /* Keywords and priority people (REQ-135), both wholesale replacements. */
        oc_slice terms_in[2] = { oc_slice_str("deploy"), oc_slice_str("release train") };
        oc_set_keywords in = { 2, terms_in };
        ROUNDTRIP(oc_encode_set_keywords(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SET_KEYWORDS, h, p);
        oc_slice out[8]; uint8_t n = 0;
        CHECK(oc_decode_set_keywords(&p, out, 8, &n) == OC_OK && n == 2);
        CHECK(out[0].len == 6 && memcmp(out[0].ptr, "deploy", 6) == 0);
        CHECK(out[1].len == 13 && memcmp(out[1].ptr, "release train", 13) == 0);
    }
    {
        uint64_t people_in[2] = { 7, 9 };
        oc_set_priority in = { 2, people_in };
        ROUNDTRIP(oc_encode_set_priority(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SET_PRIORITY, h, p);
        uint64_t out[8]; uint8_t n = 0;
        CHECK(oc_decode_set_priority(&p, out, 8, &n) == OC_OK && n == 2);
        CHECK(out[0] == 7 && out[1] == 9);
    }
    {
        oc_slice terms_in[1] = { oc_slice_str("outage") };
        uint64_t people_in[1] = { 42 };
        oc_alert_prefs in = { 1, terms_in, 1, people_in };
        ROUNDTRIP(oc_encode_alert_prefs(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_ALERT_PREFS, h, p);
        oc_slice t[4]; uint8_t nt = 0; uint64_t pe[4]; uint8_t np = 0;
        CHECK(oc_decode_alert_prefs(&p, t, 4, &nt, pe, 4, &np) == OC_OK);
        CHECK(nt == 1 && np == 1 && pe[0] == 42);
        CHECK(t[0].len == 6 && memcmp(t[0].ptr, "outage", 6) == 0);
    }
    {
        ROUNDTRIP(oc_encode_list_notify_prefs(&w, OC_PROTOCOL_VERSION), OC_MSG_LIST_NOTIFY_PREFS, h, p);
        CHECK(oc_decode_list_notify_prefs(&p) == OC_OK);
    }
    {
        /* REQ-056: a group DM's participant list, and the cap being a REFUSAL rather
         * than a silent clamp — clamping would read the ids from whatever followed
         * and open a conversation with the wrong people in it. */
        oc_open_group_dm in; memset(&in, 0, sizeof in);
        in.count = 3; in.user_ids[0] = 7; in.user_ids[1] = 9; in.user_ids[2] = 11;
        ROUNDTRIP(oc_encode_open_group_dm(&w, OC_PROTOCOL_VERSION, &in),
                  OC_MSG_OPEN_GROUP_DM, h, p);
        oc_open_group_dm out;
        CHECK(oc_decode_open_group_dm(&p, &out) == OC_OK && out.count == 3);
        CHECK(out.user_ids[0] == 7 && out.user_ids[1] == 9 && out.user_ids[2] == 11);
    }
    {
        /* The same frame with its COUNT overwritten past the cap. Patched rather than
         * hand-built because oc_frame_begin/end are internal to protocol.c — and the
         * point is the decoder, not the encoder. */
        oc_open_group_dm in; memset(&in, 0, sizeof in);
        in.count = 1; in.user_ids[0] = 5;
        oc_wbuf w2; oc_wbuf_init(&w2, frame, sizeof frame);
        CHECK(oc_encode_open_group_dm(&w2, OC_PROTOCOL_VERSION, &in) == OC_OK);
        oc_header h2; oc_rbuf p2;
        CHECK(oc_parse_frame(frame, w2.len, &h2, &p2) == OC_OK);
        /* The count is the first payload field; find it by decoding once, then move
         * the read cursor back is not possible — so patch the buffer and re-parse. */
        /* The payload starts after the 8-byte header (length 4, version 2, type 2);
         * the count is its first field, big-endian. */
        frame[8] = 0; frame[9] = 99;
        oc_header h3; oc_rbuf p3;
        CHECK(oc_parse_frame(frame, w2.len, &h3, &p3) == OC_OK);
        oc_open_group_dm bad;
        CHECK(oc_decode_open_group_dm(&p3, &bad) != OC_OK);
    }
    {
        oc_set_notify_default in = { OC_NOTIFY_NONE };            /* REQ-134 */
        ROUNDTRIP(oc_encode_set_notify_default(&w, OC_PROTOCOL_VERSION, &in),
                  OC_MSG_SET_NOTIFY_DEFAULT, h, p);
        oc_set_notify_default out;
        CHECK(oc_decode_set_notify_default(&p, &out) == OC_OK && out.level == OC_NOTIFY_NONE);
    }
    {
        oc_notify_pref_entry ents[2] = { { 7, OC_NOTIFY_MENTIONS }, { 9, OC_NOTIFY_NONE } };
        oc_notify_prefs in;
        in.notify_default = OC_NOTIFY_MENTIONS;   /* REQ-134 */
        in.count = 2; in.entries = ents;
        ROUNDTRIP(oc_encode_notify_prefs(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_NOTIFY_PREFS, h, p);
        oc_notify_pref_entry got[4]; uint16_t n = 0; uint8_t dflt = 0xFF;
        CHECK(oc_decode_notify_prefs(&p, got, 4, &n, &dflt) == OC_OK && n == 2);
        CHECK(dflt == OC_NOTIFY_MENTIONS);
        CHECK(got[0].channel_id == 7 && got[0].level == OC_NOTIFY_MENTIONS);
        CHECK(got[1].channel_id == 9 && got[1].level == OC_NOTIFY_NONE);
    }
}

/* Drafts (REQ-223, ARCH-101). The empty body is the DELETE form and has to
 * survive the round trip as an empty string rather than a missing field, and a
 * body at the cap has to survive intact — a draft silently truncated on the
 * wire is worse than one refused. */
static void test_draft_frames(void) {
    {
        oc_set_draft in = { 7, 0, oc_slice_str(""), oc_slice_str("half a thought") };
        ROUNDTRIP(oc_encode_set_draft(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SET_DRAFT, h, p);
        oc_set_draft out;
        CHECK(oc_decode_set_draft(&p, &out) == OC_OK);
        CHECK(out.channel_id == 7 && out.thread_root == 0);
        CHECK(out.body.len == 14 && memcmp(out.body.ptr, "half a thought", 14) == 0);
    }
    {   /* the delete form */
        oc_set_draft in = { 7, 0, oc_slice_str(""), oc_slice_str("") };
        ROUNDTRIP(oc_encode_set_draft(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SET_DRAFT, h, p);
        oc_set_draft out;
        CHECK(oc_decode_set_draft(&p, &out) == OC_OK);
        CHECK(out.channel_id == 7 && out.body.len == 0);
    }
    {   /* thread_root carried, though no client writes one yet (ARCH-101) */
        oc_set_draft in = { 7, 4242, oc_slice_str(""), oc_slice_str("x") };
        ROUNDTRIP(oc_encode_set_draft(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SET_DRAFT, h, p);
        oc_set_draft out;
        CHECK(oc_decode_set_draft(&p, &out) == OC_OK);
        CHECK(out.thread_root == 4242);
    }
    {
        ROUNDTRIP(oc_encode_list_drafts(&w, OC_PROTOCOL_VERSION), OC_MSG_LIST_DRAFTS, h, p);
        (void)p;
    }
    {   /* a body at the cap, byte for byte */
        static char big[OC_DRAFT_BODY_MAX];
        memset(big, 'x', sizeof big);
        oc_slice bs = { (const uint8_t *)big, sizeof big };
        oc_draft in = { 5, 9, 0, 1234567890123ULL, oc_slice_str(""), bs };
        ROUNDTRIP(oc_encode_draft(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_DRAFT, h, p);
        oc_draft out;
        CHECK(oc_decode_draft(&p, &out) == OC_OK);
        CHECK(out.channel_id == 9 && out.updated_ms == 1234567890123ULL);
        CHECK(out.body.len == OC_DRAFT_BODY_MAX);
        CHECK(memcmp(out.body.ptr, big, OC_DRAFT_BODY_MAX) == 0);
    }
    {   /* UNADDRESSED (REQ-229): no channel, a recipient list instead, and an
         * id that is the only thing telling two of them apart. */
        oc_set_draft in = { 0, 0, oc_slice_str("2,3"), oc_slice_str("to nobody yet") };
        ROUNDTRIP(oc_encode_set_draft(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SET_DRAFT, h, p);
        oc_set_draft out;
        CHECK(oc_decode_set_draft(&p, &out) == OC_OK);
        CHECK(out.channel_id == 0);
        CHECK(out.recipients.len == 3 && memcmp(out.recipients.ptr, "2,3", 3) == 0);
    }
    {
        oc_drafts in = { 3 };
        ROUNDTRIP(oc_encode_drafts(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_DRAFTS, h, p);
        oc_drafts out;
        CHECK(oc_decode_drafts(&p, &out) == OC_OK && out.count == 3);
    }
}

static void test_client_settings_frames(void) {
    {
        oc_set_client_setting in = { oc_slice_str("tui"), oc_slice_str("mouse"), oc_slice_str("1") };
        ROUNDTRIP(oc_encode_set_client_setting(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SET_CLIENT_SETTING, h, p);
        oc_set_client_setting out;
        CHECK(oc_decode_set_client_setting(&p, &out) == OC_OK);
        CHECK(out.client_type.len == 3 && memcmp(out.client_type.ptr, "tui", 3) == 0);
        CHECK(out.key.len == 5 && memcmp(out.key.ptr, "mouse", 5) == 0);
        CHECK(out.value.len == 1 && out.value.ptr[0] == '1');
    }
    {
        oc_list_client_settings in = { oc_slice_str("tui") };
        ROUNDTRIP(oc_encode_list_client_settings(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_LIST_CLIENT_SETTINGS, h, p);
        oc_list_client_settings out;
        CHECK(oc_decode_list_client_settings(&p, &out) == OC_OK);
        CHECK(out.client_type.len == 3 && memcmp(out.client_type.ptr, "tui", 3) == 0);
    }
    {
        oc_client_setting_entry ents[2] = {
            { oc_slice_str("mouse"), oc_slice_str("1") },
            { oc_slice_str("time_24h"), oc_slice_str("0") },
        };
        oc_client_settings in = { oc_slice_str("tui"), 2, ents };
        ROUNDTRIP(oc_encode_client_settings(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_CLIENT_SETTINGS, h, p);
        oc_client_setting_entry got[4]; oc_client_settings out;
        CHECK(oc_decode_client_settings(&p, &out, got, 4) == OC_OK && out.count == 2);
        CHECK(out.client_type.len == 3 && memcmp(out.client_type.ptr, "tui", 3) == 0);
        CHECK(got[0].key.len == 5 && memcmp(got[0].key.ptr, "mouse", 5) == 0);
        CHECK(got[0].value.len == 1 && got[0].value.ptr[0] == '1');
        CHECK(got[1].key.len == 8 && memcmp(got[1].key.ptr, "time_24h", 8) == 0);
    }
}

static void test_read_cursor_frames(void) {
    oc_read_cursor in = { 7, 42, 100 };
    ROUNDTRIP(oc_encode_read_cursor(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_READ_CURSOR, h, p);
    oc_read_cursor out;
    CHECK(oc_decode_read_cursor(&p, &out) == OC_OK);
    CHECK(out.channel_id == 7 && out.user_id == 42 && out.message_id == 100);
}

static void test_profile_frames(void) {
    {
        oc_set_display_name in = { oc_slice_str("Dana Q") };
        ROUNDTRIP(oc_encode_set_display_name(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SET_DISPLAY_NAME, h, p);
        oc_set_display_name out;
        CHECK(oc_decode_set_display_name(&p, &out) == OC_OK);
        CHECK(out.name.len == 6 && memcmp(out.name.ptr, "Dana Q", 6) == 0);
    }
    {
        oc_change_password in = { oc_slice_str("old-secret"), oc_slice_str("new-secret") };
        ROUNDTRIP(oc_encode_change_password(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_CHANGE_PASSWORD, h, p);
        oc_change_password out;
        CHECK(oc_decode_change_password(&p, &out) == OC_OK);
        CHECK(out.old_password.len == 10 && memcmp(out.old_password.ptr, "old-secret", 10) == 0);
        CHECK(out.new_password.len == 10 && memcmp(out.new_password.ptr, "new-secret", 10) == 0);
    }
    {
        oc_profile_updated in = { 42, oc_slice_str("Dana Q") };
        ROUNDTRIP(oc_encode_profile_updated(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_PROFILE_UPDATED, h, p);
        oc_profile_updated out;
        CHECK(oc_decode_profile_updated(&p, &out) == OC_OK && out.user_id == 42);
        CHECK(out.display_name.len == 6 && memcmp(out.display_name.ptr, "Dana Q", 6) == 0);
    }
}

static void test_call_frames(void) {
    {
        oc_call_join in = { 7 };
        ROUNDTRIP(oc_encode_call_join(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_CALL_JOIN, h, p);
        oc_call_join out; CHECK(oc_decode_call_join(&p, &out) == OC_OK && out.channel_id == 7);
    }
    {
        oc_call_leave in = { 7 };
        ROUNDTRIP(oc_encode_call_leave(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_CALL_LEAVE, h, p);
        oc_call_leave out; CHECK(oc_decode_call_leave(&p, &out) == OC_OK && out.channel_id == 7);
    }
    {
        uint64_t parts[3] = { 10, 20, 30 };
        uint8_t tok[16]; for (int i = 0; i < 16; i++) tok[i] = (uint8_t)(i + 1);
        oc_call_joined in = { 7, 7, 41234, { tok, 16 }, 3, parts };
        ROUNDTRIP(oc_encode_call_joined(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_CALL_JOINED, h, p);
        oc_call_joined out; uint64_t got[8];
        CHECK(oc_decode_call_joined(&p, &out, got, 8) == OC_OK);
        CHECK(out.channel_id == 7 && out.call_id == 7 && out.udp_port == 41234);
        CHECK(out.token.len == 16 && out.count == 3);
        CHECK(got[0] == 10 && got[1] == 20 && got[2] == 30);
    }
    {
        uint64_t parts[2] = { 10, 20 };
        oc_call_roster in = { 7, 7, 2, parts };
        ROUNDTRIP(oc_encode_call_roster(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_CALL_ROSTER, h, p);
        oc_call_roster out; uint64_t got[8];
        CHECK(oc_decode_call_roster(&p, &out, got, 8) == OC_OK && out.count == 2);
        CHECK(got[0] == 10 && got[1] == 20);
    }
}

static void test_webhook_frames(void) {
    {
        oc_create_webhook in = { 9, oc_slice_str("github") };
        ROUNDTRIP(oc_encode_create_webhook(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_CREATE_WEBHOOK, h, p);
        oc_create_webhook out;
        CHECK(oc_decode_create_webhook(&p, &out) == OC_OK);
        CHECK(out.channel_id == 9 && slice_eq_str(out.label, "github"));
    }
    {
        uint8_t tok[OC_SESSION_TOKEN_LEN];
        for (int i = 0; i < (int)OC_SESSION_TOKEN_LEN; i++) tok[i] = (uint8_t)(i * 7 + 1);
        oc_webhook_info in = { 5, 9, { tok, sizeof tok } };
        ROUNDTRIP(oc_encode_webhook_info(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_WEBHOOK_INFO, h, p);
        oc_webhook_info out;
        CHECK(oc_decode_webhook_info(&p, &out) == OC_OK);
        CHECK(out.webhook_id == 5 && out.channel_id == 9 && out.token.len == OC_SESSION_TOKEN_LEN);
        CHECK(memcmp(out.token.ptr, tok, sizeof tok) == 0);
    }
    {
        oc_list_webhooks in = { 9 };
        ROUNDTRIP(oc_encode_list_webhooks(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_LIST_WEBHOOKS, h, p);
        oc_list_webhooks out;
        CHECK(oc_decode_list_webhooks(&p, &out) == OC_OK && out.channel_id == 9);
    }
    {
        oc_webhook_list_entry ents[2] = { { 1, 9, oc_slice_str("ci"), 0 },
                                          { 2, 9, oc_slice_str("bot"), 1 } };
        oc_webhook_list in = { 2, ents };
        ROUNDTRIP(oc_encode_webhook_list(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_WEBHOOK_LIST, h, p);
        oc_webhook_list_entry got[4]; uint16_t n = 0;
        CHECK(oc_decode_webhook_list(&p, got, 4, &n) == OC_OK && n == 2);
        CHECK(got[0].webhook_id == 1 && slice_eq_str(got[0].label, "ci") && got[0].disabled == 0);
        CHECK(got[1].webhook_id == 2 && got[1].disabled == 1);
    }
    {
        oc_delete_webhook in = { 5 };
        ROUNDTRIP(oc_encode_delete_webhook(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_DELETE_WEBHOOK, h, p);
        oc_delete_webhook out;
        CHECK(oc_decode_delete_webhook(&p, &out) == OC_OK && out.webhook_id == 5);
    }
    {
        oc_webhook_deleted in = { 5 };
        ROUNDTRIP(oc_encode_webhook_deleted(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_WEBHOOK_DELETED, h, p);
        oc_webhook_deleted out;
        CHECK(oc_decode_webhook_deleted(&p, &out) == OC_OK && out.webhook_id == 5);
    }
}

static void test_attachment_frames(void) {
    uint8_t idem[OC_IDEM_SIZE];
    for (int i = 0; i < (int)OC_IDEM_SIZE; i++) idem[i] = (uint8_t)(0x30 + i);
    uint8_t digest[32];
    for (int i = 0; i < 32; i++) digest[i] = (uint8_t)(0xC0 + i);
    {
        oc_upload_begin in; in.channel_id = 5; memcpy(in.idem, idem, OC_IDEM_SIZE);
        in.filename = oc_slice_str("report.pdf"); in.mime = oc_slice_str("application/pdf");
        in.total_size = 1048576;
        ROUNDTRIP(oc_encode_upload_begin(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_UPLOAD_BEGIN, h, p);
        oc_upload_begin out;
        CHECK(oc_decode_upload_begin(&p, &out) == OC_OK);
        CHECK(out.channel_id == 5 && memcmp(out.idem, idem, OC_IDEM_SIZE) == 0);
        CHECK(slice_eq_str(out.filename, "report.pdf") && slice_eq_str(out.mime, "application/pdf"));
        CHECK(out.total_size == 1048576);
    }
    {
        oc_upload_ready in = { 99, OC_ATTACH_CHUNK_SIZE, 262144 };
        ROUNDTRIP(oc_encode_upload_ready(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_UPLOAD_READY, h, p);
        oc_upload_ready out;
        CHECK(oc_decode_upload_ready(&p, &out) == OC_OK);
        CHECK(out.attachment_id == 99 && out.chunk_size == OC_ATTACH_CHUNK_SIZE && out.window_bytes == 262144);
    }
    {
        oc_upload_chunk in; in.attachment_id = 99; in.seq = 3;
        in.data = (oc_slice){ (const uint8_t *)"blobbytes", 9 };
        ROUNDTRIP(oc_encode_upload_chunk(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_UPLOAD_CHUNK, h, p);
        oc_upload_chunk out;
        CHECK(oc_decode_upload_chunk(&p, &out) == OC_OK);
        CHECK(out.attachment_id == 99 && out.seq == 3 && out.data.len == 9);
        CHECK(memcmp(out.data.ptr, "blobbytes", 9) == 0);
    }
    {
        oc_upload_ack in = { 99, 4 };
        ROUNDTRIP(oc_encode_upload_ack(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_UPLOAD_ACK, h, p);
        oc_upload_ack out;
        CHECK(oc_decode_upload_ack(&p, &out) == OC_OK && out.attachment_id == 99 && out.acked_through == 4);
    }
    {
        oc_upload_end in = { 99 };
        ROUNDTRIP(oc_encode_upload_end(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_UPLOAD_END, h, p);
        oc_upload_end out;
        CHECK(oc_decode_upload_end(&p, &out) == OC_OK && out.attachment_id == 99);
    }
    {
        oc_upload_ok in; in.attachment_id = 99; in.size = 1048576;
        in.sha256 = (oc_slice){ digest, 32 };
        ROUNDTRIP(oc_encode_upload_ok(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_UPLOAD_OK, h, p);
        oc_upload_ok out;
        CHECK(oc_decode_upload_ok(&p, &out) == OC_OK);
        CHECK(out.attachment_id == 99 && out.size == 1048576 && out.sha256.len == 32);
        CHECK(memcmp(out.sha256.ptr, digest, 32) == 0);
    }
    {
        oc_download_begin in = { 99 };
        ROUNDTRIP(oc_encode_download_begin(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_DOWNLOAD_BEGIN, h, p);
        oc_download_begin out;
        CHECK(oc_decode_download_begin(&p, &out) == OC_OK && out.attachment_id == 99);
    }
    {
        oc_download_info in; in.attachment_id = 99;
        in.filename = oc_slice_str("report.pdf"); in.mime = oc_slice_str("application/pdf");
        in.total_size = 1048576; in.sha256 = (oc_slice){ digest, 32 };
        ROUNDTRIP(oc_encode_download_info(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_DOWNLOAD_INFO, h, p);
        oc_download_info out;
        CHECK(oc_decode_download_info(&p, &out) == OC_OK);
        CHECK(out.attachment_id == 99 && out.total_size == 1048576);
        CHECK(slice_eq_str(out.filename, "report.pdf") && out.sha256.len == 32);
    }
    {
        oc_download_chunk in; in.attachment_id = 99; in.seq = 0;
        in.data = (oc_slice){ (const uint8_t *)"xyz", 3 };
        ROUNDTRIP(oc_encode_download_chunk(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_DOWNLOAD_CHUNK, h, p);
        oc_download_chunk out;
        CHECK(oc_decode_download_chunk(&p, &out) == OC_OK);
        CHECK(out.attachment_id == 99 && out.seq == 0 && out.data.len == 3);
    }
    {
        oc_download_end in = { 99 };
        ROUNDTRIP(oc_encode_download_end(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_DOWNLOAD_END, h, p);
        oc_download_end out;
        CHECK(oc_decode_download_end(&p, &out) == OC_OK && out.attachment_id == 99);
    }
    {
        oc_transfer_cancel in = { 99 };
        ROUNDTRIP(oc_encode_transfer_cancel(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_TRANSFER_CANCEL, h, p);
        oc_transfer_cancel out;
        CHECK(oc_decode_transfer_cancel(&p, &out) == OC_OK && out.attachment_id == 99);
    }
    /* SEND with a trailing attachment-id list (REQ-140 linking). */
    {
        oc_send in = {0}; in.channel_id = 3; memset(in.idem, 0x11, OC_IDEM_SIZE);
        in.body = oc_slice_str("see file");
        in.n_attach = 2; in.attach_ids[0] = 100; in.attach_ids[1] = 200;
        ROUNDTRIP(oc_encode_send(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SEND, h, p);
        oc_send out = {0};
        CHECK(oc_decode_send(&p, &out) == OC_OK);
        CHECK(out.channel_id == 3 && out.n_attach == 2);
        CHECK(out.attach_ids[0] == 100 && out.attach_ids[1] == 200);
        CHECK(slice_eq_str(out.body, "see file"));
    }
    /* Zero attachments -> the list is omitted; decode yields n_attach 0. */
    {
        oc_send in = {0}; in.channel_id = 3; in.body = oc_slice_str("plain");
        ROUNDTRIP(oc_encode_send(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_SEND, h, p);
        oc_send out = {0};
        CHECK(oc_decode_send(&p, &out) == OC_OK && out.n_attach == 0);
        CHECK(slice_eq_str(out.body, "plain"));
    }
    /* BROADCAST carrying attachment metadata AND a display-name override. */
    {
        oc_broadcast in = { 5, 3, 42, 999, oc_slice_str("here"), 0, {{0}}, {0} };
        in.n_attach = 1;
        in.attach[0].id = 77; in.attach[0].filename = oc_slice_str("a.png");
        in.attach[0].mime = oc_slice_str("image/png"); in.attach[0].size = 4096;
        in.author_name = oc_slice_str("GitHub CI");
        ROUNDTRIP(oc_encode_broadcast(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_BROADCAST, h, p);
        oc_broadcast out;
        CHECK(oc_decode_broadcast(&p, &out) == OC_OK);
        CHECK(out.n_attach == 1 && out.attach[0].id == 77 && out.attach[0].size == 4096);
        CHECK(slice_eq_str(out.attach[0].filename, "a.png"));
        CHECK(slice_eq_str(out.author_name, "GitHub CI"));
    }
    /* Author name with NO attachments: a zero attachment count precedes the name
     * (REQ-170 display-name override, ARCH-71). */
    {
        oc_broadcast in = { 6, 3, 42, 999, oc_slice_str("hi"), 0, {{0}}, {0} };
        in.author_name = oc_slice_str("Zapier");
        ROUNDTRIP(oc_encode_broadcast(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_BROADCAST, h, p);
        oc_broadcast out;
        CHECK(oc_decode_broadcast(&p, &out) == OC_OK);
        CHECK(out.n_attach == 0 && slice_eq_str(out.author_name, "Zapier"));
        CHECK(slice_eq_str(out.body, "hi"));
    }
    /* No attachments and no name -> byte-identical to the original layout. */
    {
        oc_broadcast in = { 7, 3, 42, 999, oc_slice_str("plain"), 0, {{0}}, {0} };
        ROUNDTRIP(oc_encode_broadcast(&w, OC_PROTOCOL_VERSION, &in), OC_MSG_BROADCAST, h, p);
        oc_broadcast out;
        CHECK(oc_decode_broadcast(&p, &out) == OC_OK);
        CHECK(out.n_attach == 0 && out.author_name.len == 0 && slice_eq_str(out.body, "plain"));
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
        oc_backfill_done in = { 4242, 1 };
        ROUNDTRIP(oc_encode_backfill_done(&w, OC_PROTOCOL_VERSION, &in),
                  OC_MSG_BACKFILL_DONE, h, p);
        oc_backfill_done out;
        CHECK(oc_decode_backfill_done(&p, &out) == OC_OK);
        CHECK(out.more == 1);
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
    oc_send in = {0}; in.channel_id = 1; memset(in.idem, 0, OC_IDEM_SIZE);
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


/* Storage status (REQ-214): a fixed-width report, so the round-trip is the
 * whole contract. */
static void test_storage_status_frames(void) {
    uint8_t buf[256];
    oc_wbuf w; oc_wbuf_init(&w, buf, sizeof buf);
    oc_storage_status in = { 1000, 200, 50, 7, 3, 2, 1, 12345, 30, 256, 1, 1 };
    CHECK(oc_encode_storage_status(&w, OC_PROTOCOL_VERSION, &in) == OC_OK);

    oc_header h; oc_rbuf r;
    CHECK(oc_parse_frame(buf, w.len, &h, &r) == OC_OK);
    CHECK(h.msg_type == OC_MSG_STORAGE_STATUS);
    oc_storage_status out;
    CHECK(oc_decode_storage_status(&r, &out) == OC_OK);
    CHECK(out.total_bytes == 1000 && out.avail_bytes == 200);
    CHECK(out.attach_bytes == 50 && out.attach_count == 7);
    CHECK(out.reclaimed_orphan == 3 && out.reclaimed_expired == 2 && out.reclaimed_evicted == 1);
    CHECK(out.last_reclaim_ms == 12345);
    CHECK(out.max_age_days == 30 && out.reserve_bytes == 256);
    CHECK(out.evict_enabled == 1 && out.under_pressure == 1);

    /* The request carries no body. */
    oc_wbuf_init(&w, buf, sizeof buf);
    CHECK(oc_encode_storage_status_req(&w, OC_PROTOCOL_VERSION) == OC_OK);
    CHECK(oc_parse_frame(buf, w.len, &h, &r) == OC_OK);
    CHECK(h.msg_type == OC_MSG_STORAGE_STATUS_REQ);
}

/* Every reason code is distinct.
 *
 * This exists because they were not. INVALID_MESSAGE (REQ-224) was added in
 * 2026-08 reusing 3014, which REGISTER_DEVICE_TOKEN's INVALID_DEVICE_TOKEN had
 * held since 2026-07 -- so one number meant two things and a client reading the
 * ERROR frame could not tell which. Nothing caught it because nothing looked.
 *
 * Listed explicitly rather than derived: an enum cannot be enumerated in C, and
 * a test that walks a range would pass for a code nobody remembered to add. A
 * new reason code belongs in this array, and the compiler says nothing if it is
 * missing -- but the next duplicate does not reach the wire.
 */
static void test_reason_codes_unique(void) {
    static const struct { const char *name; int code; } codes[] = {
        { "MALFORMED_FRAME",        OC_ERR_MALFORMED_FRAME },
        { "VERSION_TOO_OLD",        OC_ERR_VERSION_TOO_OLD },
        { "VERSION_TOO_NEW",        OC_ERR_VERSION_TOO_NEW },
        { "UNEXPECTED_MSG_TYPE",    OC_ERR_UNEXPECTED_MSG_TYPE },
        { "VERSION_MISMATCH",       OC_ERR_VERSION_MISMATCH },
        { "FRAME_TOO_LARGE",        OC_ERR_FRAME_TOO_LARGE },
        { "AUTH_INVALID_TOKEN",     OC_ERR_AUTH_INVALID_TOKEN },
        { "AUTH_REQUIRED",          OC_ERR_AUTH_REQUIRED },
        { "AUTH_RATE_LIMITED",      OC_ERR_AUTH_RATE_LIMITED },
        { "USER_LIMIT",             OC_ERR_USER_LIMIT },
        { "UNKNOWN_CHANNEL",        OC_ERR_UNKNOWN_CHANNEL },
        { "NOT_A_MEMBER",           OC_ERR_NOT_A_MEMBER },
        { "BODY_TOO_LARGE",         OC_ERR_BODY_TOO_LARGE },
        { "SEND_RATE_LIMITED",      OC_ERR_SEND_RATE_LIMITED },
        { "FORBIDDEN",              OC_ERR_FORBIDDEN },
        { "LAST_OWNER",             OC_ERR_LAST_OWNER },
        { "UNKNOWN_MESSAGE",        OC_ERR_UNKNOWN_MESSAGE },
        { "INVALID_CHANNEL",        OC_ERR_INVALID_CHANNEL },
        { "INVALID_REACTION",       OC_ERR_INVALID_REACTION },
        { "ATTACHMENT_TOO_LARGE",   OC_ERR_ATTACHMENT_TOO_LARGE },
        { "UNKNOWN_ATTACHMENT",     OC_ERR_UNKNOWN_ATTACHMENT },
        { "STORAGE_FULL",           OC_ERR_STORAGE_FULL },
        { "ATTACHMENT_GONE",        OC_ERR_ATTACHMENT_GONE },
        { "INVALID_DEVICE_TOKEN",   OC_ERR_INVALID_DEVICE_TOKEN },
        { "TRANSFER_PROTOCOL",      OC_ERR_TRANSFER_PROTOCOL },
        { "UNKNOWN_WEBHOOK",        OC_ERR_UNKNOWN_WEBHOOK },
        { "CHANNEL_EXISTS",         OC_ERR_CHANNEL_EXISTS },
        { "TOO_MANY_PINS",          OC_ERR_TOO_MANY_PINS },
        { "CHANNEL_ARCHIVED",       OC_ERR_CHANNEL_ARCHIVED },
        { "INVALID_MESSAGE",        OC_ERR_INVALID_MESSAGE },
        { "INTERNAL",               OC_ERR_INTERNAL },
    };
    const size_t n = sizeof codes / sizeof codes[0];
    for (size_t i = 0; i < n; i++) {
        for (size_t j = i + 1; j < n; j++) {
            if (codes[i].code == codes[j].code)
                printf("  FAIL reason code %d is both %s and %s\n",
                       codes[i].code, codes[i].name, codes[j].name);
            CHECK(codes[i].code != codes[j].code);
        }
    }
    /* The specific collision this test was written for. */
    CHECK(OC_ERR_INVALID_MESSAGE != OC_ERR_INVALID_DEVICE_TOKEN);
}

int run_protocol_tests(void) {
    printf("test_protocol: primitives, handshake, auth, messaging, backfill,\n");
    printf("               error, size limits, malformed frames, version negotiation\n");
    test_reason_codes_unique();
    test_primitives();
    test_handshake_frames();
    test_auth_frames();
    test_messaging_frames();
    test_reaction_frames();
    test_thread_frames();
    test_channel_frames();
    test_admin_frames();
    test_search_frames();
    test_presence_frames();
    test_attachment_frames();
    test_webhook_frames();
    test_notify_frames();
    test_draft_frames();
    test_client_settings_frames();
    test_read_cursor_frames();
    test_storage_status_frames();
    test_profile_frames();
    test_call_frames();
    test_backfill_and_error();
    test_size_limits();
    test_malformed();
    test_version_negotiation();
    return failures;
}
