/*
 * OpenChime wire protocol v1 — frame codec.
 *
 * Pure encode/decode of the core-messaging-path frames defined in
 * docs/PROTOCOL.md. No sockets, no threads, no SQLite: this is the leaf the
 * event loop and DB writer are built on top of, and it is the first thing to
 * be exhaustively unit-tested (docs/TESTING.md §2.2).
 *
 * Encoding is explicit and field-by-field (ARCH-7); nothing is memcpy'd over a
 * struct. All integers are big-endian on the wire, converted at the edges
 * (ARCH-9). Decoders return zero-copy views (oc_slice) into the caller's frame
 * buffer, so that buffer must outlive any use of the decoded strings.
 */

#ifndef OPENCHIME_PROTOCOL_H
#define OPENCHIME_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

/* --- Constants (PROTOCOL.md §2.1, §7) ----------------------------------- */

#define OC_PROTOCOL_VERSION 1u     /* the only version this codec speaks */

/* Transport conventions (see PROTOCOL.md §1). The binary protocol shares TLS
 * port 443 with the future HTTP/webhook surface, demultiplexed by ALPN: a
 * client offers OC_ALPN_PROTO, and the daemon routes that to the binary
 * handler (anything else is HTTP). Clients dial OC_DEFAULT_PORT unless a SRV
 * record or .well-known metadata overrides the port. */
#define OC_ALPN_PROTO   "oc/1"     /* ALPN id for the binary protocol */
#define OC_DEFAULT_PORT 443        /* default client connect port */
#define OC_HEADER_SIZE      8u     /* length(4) + version(2) + msg_type(2) */
#define OC_LENGTH_MIN       4u     /* version(2) + msg_type(2), empty payload */
#define OC_MAX_FRAME_SIZE   66560u /* 65 KiB, total on wire (4 + length) */
#define OC_MAX_BODY_SIZE    65536u /* 64 KiB, message body cap (REQ-054) */
#define OC_IDEM_SIZE        16u    /* 128-bit idempotency token, fixed width */

/* --- Message types (PROTOCOL.md §9) ------------------------------------- */

typedef enum {
    OC_MSG_HELLO            = 0x0001, /* C->S, frozen@v1 */
    OC_MSG_WELCOME          = 0x0002, /* S->C, frozen@v1 */
    OC_MSG_REJECT           = 0x0003, /* S->C, frozen@v1, fatal */
    OC_MSG_AUTH             = 0x0010, /* C->S */
    OC_MSG_AUTH_OK          = 0x0011, /* S->C */
    OC_MSG_AUTH_CHALLENGE   = 0x0012, /* S->C */
    OC_MSG_LOGOUT           = 0x0013, /* C->S */
    OC_MSG_SEND             = 0x0020, /* C->S */
    OC_MSG_SEND_ACK         = 0x0021, /* S->C */
    OC_MSG_BROADCAST        = 0x0022, /* S->C */
    OC_MSG_CLIENT_ACK       = 0x0023, /* C->S */
    OC_MSG_EDIT             = 0x0024, /* C->S (REQ-051) */
    OC_MSG_DELETE           = 0x0025, /* C->S (REQ-052) */
    OC_MSG_MSG_EDITED       = 0x0026, /* S->C, edit fan-out */
    OC_MSG_MSG_DELETED      = 0x0027, /* S->C, tombstone fan-out */
    OC_MSG_REACT            = 0x0028, /* C->S (REQ-070) */
    OC_MSG_REACTION_UPDATED = 0x0029, /* S->C, reaction fan-out */
    OC_MSG_LIST_REACTIONS   = 0x002A, /* C->S (REQ-071) */
    OC_MSG_REACTIONS        = 0x002B, /* S->C */
    OC_MSG_SEND_REPLY       = 0x002C, /* C->S (REQ-060), a threaded reply */
    OC_MSG_THREAD_REPLY     = 0x002D, /* S->C, reply fan-out (not in main scroll) */
    OC_MSG_LIST_THREAD      = 0x002E, /* C->S, open a thread */
    OC_MSG_THREAD           = 0x002F, /* S->C, a thread's replies */
    OC_MSG_THREAD_META      = 0x0032, /* S->C, a parent's reply count (backfill) */
    OC_MSG_CREATE_CHANNEL     = 0x0050, /* C->S (REQ-050) */
    OC_MSG_CHANNEL_INFO       = 0x0051, /* S->C, ack for create/join/leave/invite/remove */
    OC_MSG_LIST_CHANNELS      = 0x0052, /* C->S */
    OC_MSG_CHANNEL_LIST       = 0x0053, /* S->C */
    OC_MSG_JOIN_CHANNEL       = 0x0054, /* C->S */
    OC_MSG_LEAVE_CHANNEL      = 0x0055, /* C->S */
    OC_MSG_INVITE_TO_CHANNEL  = 0x0056, /* C->S (REQ-033, channel-level) */
    OC_MSG_REMOVE_FROM_CHANNEL= 0x0057, /* C->S (REQ-033, channel-level) */
    OC_MSG_OPEN_DM            = 0x0058, /* C->S, open/get a 1:1 DM (REQ-050) */
    OC_MSG_SET_PRESENCE     = 0x0070, /* C->S, set own presence (REQ-120) */
    OC_MSG_PRESENCE_UPDATE  = 0x0071, /* S->C, a user's presence changed */
    OC_MSG_TYPING           = 0x0072, /* C->S, "I am typing" in a channel (REQ-121) */
    OC_MSG_TYPING_UPDATE    = 0x0073, /* S->C, relay of a typing signal */
    OC_MSG_LIST_USERS       = 0x0040, /* C->S, tenant user enumeration */
    OC_MSG_USER_LIST        = 0x0041, /* S->C */
    OC_MSG_SET_ROLE         = 0x0042, /* C->S (ARCH-60, REQ-030) */
    OC_MSG_INVITE_USER      = 0x0043, /* C->S (REQ-033, tenant-level; owner/admin) */
    OC_MSG_REMOVE_USER      = 0x0044, /* C->S (REQ-033, tenant-level; owner/admin) */
    OC_MSG_USER_UPDATED     = 0x0045, /* S->C, ack/push for SET_ROLE + REMOVE_USER */
    OC_MSG_INVITE_CREATED   = 0x0046, /* S->C, the minted invite token */
    OC_MSG_REDEEM_INVITE    = 0x0047, /* C->S, pre-auth account creation */
    OC_MSG_SEARCH           = 0x0060, /* C->S (REQ-080) */
    OC_MSG_SEARCH_RESULTS   = 0x0061, /* S->C */
    OC_MSG_BACKFILL_REQUEST = 0x0030, /* C->S */
    OC_MSG_BACKFILL_DONE    = 0x0031, /* S->C */
    OC_MSG_ERROR            = 0x00FF  /* S->C */
} oc_msg_type;

/* --- Reason codes (PROTOCOL.md §8.2) ------------------------------------ */

typedef enum {
    OC_ERR_VERSION_TOO_OLD     = 1001,
    OC_ERR_VERSION_TOO_NEW     = 1002,
    OC_ERR_MALFORMED_FRAME     = 1003,
    OC_ERR_FRAME_TOO_LARGE     = 1004,
    OC_ERR_UNEXPECTED_MSG_TYPE = 1005,
    OC_ERR_AUTH_REQUIRED       = 2001,
    OC_ERR_AUTH_INVALID_TOKEN  = 2002,
    OC_ERR_AUTH_RATE_LIMITED   = 2003,
    OC_ERR_BODY_TOO_LARGE      = 3001,
    OC_ERR_NOT_A_MEMBER        = 3002,
    OC_ERR_UNKNOWN_CHANNEL     = 3003,
    OC_ERR_SEND_RATE_LIMITED   = 3004,
    OC_ERR_FORBIDDEN           = 3005, /* actor may not perform the action (role, or not the author) */
    OC_ERR_LAST_OWNER          = 3006, /* would remove/demote the last owner (REQ-030) */
    OC_ERR_UNKNOWN_MESSAGE     = 3007, /* no such message in the channel (edit/delete) */
    OC_ERR_INVALID_CHANNEL     = 3008, /* bad channel name on CREATE_CHANNEL (empty/too long) */
    OC_ERR_INVALID_REACTION    = 3009, /* empty/oversized emoji on REACT */
    OC_ERR_INTERNAL            = 9001
} oc_reason_code;

/* --- Codec result codes ------------------------------------------------- */

typedef enum {
    OC_OK               =  0,
    OC_E_OVERFLOW       = -1, /* writer ran out of buffer space */
    OC_E_MALFORMED      = -2, /* reader underflow / bad length / short frame */
    OC_E_TOO_LARGE      = -3, /* frame exceeds OC_MAX_FRAME_SIZE */
    OC_E_BODY_TOO_LARGE = -4  /* body exceeds OC_MAX_BODY_SIZE */
} oc_result;

/* --- Buffers and primitive encodings (PROTOCOL.md §7) ------------------- */

/* A zero-copy view into a frame buffer, returned by string/bytes decoders. */
typedef struct {
    const uint8_t *ptr;
    size_t         len;
} oc_slice;

/* Write cursor over a caller-provided buffer. Once `overflow` is set, further
 * writes are no-ops and the frame is unusable. */
typedef struct {
    uint8_t *data;
    size_t   cap;
    size_t   len;
    int      overflow;
} oc_wbuf;

/* Read cursor over a caller-provided buffer. Once `underflow` is set, further
 * reads are no-ops and yield zeroed values. */
typedef struct {
    const uint8_t *data;
    size_t         len;
    size_t         pos;
    int            underflow;
} oc_rbuf;

void oc_wbuf_init(oc_wbuf *w, uint8_t *data, size_t cap);
void oc_rbuf_init(oc_rbuf *r, const uint8_t *data, size_t len);

/* Convenience: view a NUL-terminated C string as a slice (length = strlen). */
oc_slice oc_slice_str(const char *s);

/* Primitive writers. Each is a no-op once the buffer has overflowed. */
void oc_w_u8(oc_wbuf *w, uint8_t v);
void oc_w_u16(oc_wbuf *w, uint16_t v);
void oc_w_u32(oc_wbuf *w, uint32_t v);
void oc_w_u64(oc_wbuf *w, uint64_t v);
void oc_w_str(oc_wbuf *w, oc_slice s);   /* u16 length + bytes; >65535 overflows */
void oc_w_lstr(oc_wbuf *w, oc_slice s);  /* u32 length + bytes */
void oc_w_bytes(oc_wbuf *w, oc_slice s); /* u32 length + bytes */
void oc_w_idem(oc_wbuf *w, const uint8_t idem[OC_IDEM_SIZE]);

/* Primitive readers. Each is a no-op once the buffer has underflowed. */
uint8_t  oc_r_u8(oc_rbuf *r);
uint16_t oc_r_u16(oc_rbuf *r);
uint32_t oc_r_u32(oc_rbuf *r);
uint64_t oc_r_u64(oc_rbuf *r);
oc_slice oc_r_str(oc_rbuf *r);
oc_slice oc_r_lstr(oc_rbuf *r);
oc_slice oc_r_bytes(oc_rbuf *r);
void     oc_r_idem(oc_rbuf *r, uint8_t idem[OC_IDEM_SIZE]);

/* --- Frame header (PROTOCOL.md §2) -------------------------------------- */

typedef struct {
    uint32_t length;
    uint16_t version;
    uint16_t msg_type;
} oc_header;

/* Parse and validate a complete frame. `data`/`len` must span at least one
 * full frame (4 + length bytes). On OC_OK, `hdr` is filled and `payload` is a
 * reader positioned at the first payload byte, bounded to the payload length.
 * Returns OC_E_MALFORMED (short frame / length below minimum) or
 * OC_E_TOO_LARGE (length beyond OC_MAX_FRAME_SIZE). */
oc_result oc_parse_frame(const uint8_t *data, size_t len,
                         oc_header *hdr, oc_rbuf *payload);

/* --- Version negotiation (PROTOCOL.md §3.2) ----------------------------- */

/* Given the client's advertised [min,max] and the server's supported [min,max],
 * compute the chosen version (OC_OK, *chosen set) or the REJECT reason
 * (OC_E_MALFORMED, *reject_code set to OC_ERR_VERSION_TOO_OLD/TOO_NEW). */
oc_result oc_negotiate_version(uint16_t client_min, uint16_t client_max,
                               uint16_t server_min, uint16_t server_max,
                               uint16_t *chosen, uint16_t *reject_code);

/* --- Auth methods and roles (PROTOCOL.md §4, AUTH.md) ------------------- */

/* AUTH `method` discriminator; also the bitset in AUTH_CHALLENGE.methods. */
#define OC_AUTH_LOCAL   0x01u   /* username + password (ARCH-59) */
#define OC_AUTH_OIDC    0x02u   /* central-issued ES256 JWT (ARCH-56/57) */
#define OC_AUTH_SESSION 0x04u   /* a prior session token (reconnect, ARCH-58) */

/* AUTH_OK.role (ARCH-60). */
#define OC_ROLE_MEMBER  0u
#define OC_ROLE_ADMIN   1u
#define OC_ROLE_OWNER   2u

#define OC_SESSION_TOKEN_LEN 32u  /* random session token; only its hash is stored */

/* Presence status (REQ-120). offline is implicit (no connection); a client sets
 * online/away. */
#define OC_PRESENCE_OFFLINE 0u
#define OC_PRESENCE_ONLINE  1u
#define OC_PRESENCE_AWAY    2u

/* Channel kind (SCHEMA.md channels.kind) and the name cap for CREATE_CHANNEL. */
#define OC_CHANNEL_KIND    0u   /* a named channel */
#define OC_CHANNEL_KIND_DM 1u   /* a direct-message conversation (reserved) */
#define OC_MAX_CHANNEL_NAME 64u /* channel-name length cap (bytes) */

/* Account-creation invite token (AUTH.md §2); like a session token, only its
 * SHA-256 is stored. Presented in REDEEM_INVITE to set a password. */
#define OC_INVITE_TOKEN_LEN 32u

/* Emoji reaction op (REQ-070) and the emoji length cap (bytes; fits multi-
 * codepoint sequences like flags/ZWJ). */
#define OC_REACT_REMOVE 0u
#define OC_REACT_ADD    1u
#define OC_MAX_EMOJI    32u

/* LOGOUT scope (PROTOCOL.md §4; REQ-182). */
#define OC_LOGOUT_THIS 0u   /* revoke just the presented session token */
#define OC_LOGOUT_ALL  1u   /* revoke every session of the authenticated user */

/* --- Frame payload structs ---------------------------------------------- */

typedef struct { uint16_t min_version; uint16_t max_version; oc_slice client_info; } oc_hello;
typedef struct { uint16_t chosen_version; uint64_t server_time; } oc_welcome;
typedef struct { uint16_t code; oc_slice message; } oc_reject;
typedef struct { uint8_t methods; oc_slice oidc_params; } oc_auth_challenge;
typedef struct { uint8_t method; oc_slice credential; } oc_auth;
typedef struct { uint64_t user_id; uint8_t role; uint64_t session_expiry; oc_slice session_token; } oc_auth_ok;
typedef struct { uint8_t scope; oc_slice session_token; } oc_logout;
typedef struct { uint64_t channel_id; uint8_t idem[OC_IDEM_SIZE]; oc_slice body; } oc_send;
typedef struct { uint8_t idem[OC_IDEM_SIZE]; uint64_t channel_id; uint64_t message_id; uint64_t server_time; } oc_send_ack;
typedef struct { uint64_t message_id; uint64_t channel_id; uint64_t author_id; uint64_t server_time; oc_slice body; } oc_broadcast;
typedef struct { uint64_t channel_id; uint64_t message_id; } oc_client_ack;
typedef struct { uint64_t channel_id; uint64_t message_id; oc_slice body; } oc_edit;
typedef struct { uint64_t channel_id; uint64_t message_id; } oc_delete;
typedef struct { uint64_t message_id; uint64_t channel_id; uint64_t author_id; uint64_t edited_at; oc_slice body; } oc_msg_edited;
typedef struct { uint64_t message_id; uint64_t channel_id; uint64_t author_id; uint64_t deleted_by; uint64_t deleted_at; } oc_msg_deleted;
typedef struct { uint64_t channel_id; uint64_t message_id; oc_slice emoji; uint8_t op; } oc_react;
typedef struct { uint64_t message_id; uint64_t channel_id; uint64_t user_id; oc_slice emoji; uint8_t op; uint64_t count; } oc_reaction_updated;
typedef struct { uint64_t channel_id; uint64_t message_id; } oc_list_reactions;
typedef struct { oc_slice emoji; uint64_t user_id; } oc_reaction_entry;
typedef struct { uint64_t message_id; uint16_t count; const oc_reaction_entry *entries; } oc_reactions;
typedef struct { uint64_t channel_id; uint8_t idem[OC_IDEM_SIZE]; uint64_t parent_id; oc_slice body; } oc_send_reply;
typedef struct { uint64_t message_id; uint64_t channel_id; uint64_t parent_id; uint64_t author_id; uint64_t server_time; uint32_t reply_count; oc_slice body; } oc_thread_reply;
typedef struct { uint64_t channel_id; uint64_t parent_id; } oc_list_thread;
/* THREAD is the terminator of a LIST_THREAD response: the daemon streams the
 * replies as THREAD_REPLY frames (each self-framed, so a 64KB body is fine),
 * then this frame closes the stream — mirroring BACKFILL_DONE (§6.2). */
typedef struct { uint64_t parent_id; uint32_t count; uint8_t truncated; } oc_thread;
typedef struct { uint64_t message_id; uint32_t reply_count; uint64_t last_reply_at; } oc_thread_meta;
typedef struct { oc_slice name; uint8_t is_public; } oc_create_channel;
typedef struct { uint64_t channel_id; uint8_t kind; oc_slice name; uint8_t is_public; uint8_t joined; uint64_t created_at; } oc_channel_info;
typedef struct { uint64_t channel_id; } oc_channel_ref;                       /* JOIN / LEAVE */
typedef struct { uint64_t channel_id; uint64_t user_id; } oc_channel_member_op; /* INVITE / REMOVE */
typedef struct { uint64_t channel_id; oc_slice name; uint8_t is_public; uint8_t joined; uint8_t kind; } oc_channel_list_entry;
typedef struct { uint64_t user_id; } oc_open_dm;
typedef struct { uint8_t status; } oc_set_presence;
typedef struct { uint64_t user_id; uint8_t status; } oc_presence_update;
typedef struct { uint64_t channel_id; } oc_typing;
typedef struct { uint64_t channel_id; uint64_t user_id; } oc_typing_update;
typedef struct { uint16_t count; const oc_channel_list_entry *entries; } oc_channel_list;
typedef struct { uint64_t user_id; uint8_t role; uint8_t disabled; oc_slice email; oc_slice display_name; } oc_user_list_entry;
typedef struct { uint16_t count; const oc_user_list_entry *entries; } oc_user_list;
typedef struct { uint64_t user_id; uint8_t role; } oc_set_role;
typedef struct { uint8_t role; } oc_invite_user;
typedef struct { uint64_t user_id; } oc_remove_user;
typedef struct { uint64_t user_id; uint8_t role; uint8_t disabled; } oc_user_updated;
typedef struct { oc_slice token; uint8_t role; uint64_t expires_at; } oc_invite_created;
typedef struct { oc_slice token; oc_slice username; oc_slice password; } oc_redeem_invite;
typedef struct { oc_slice query; uint16_t limit; } oc_search;
typedef struct { uint64_t message_id; uint64_t channel_id; uint64_t author_id; uint64_t server_time; oc_slice snippet; } oc_search_result_entry;
typedef struct { uint16_t count; const oc_search_result_entry *entries; uint8_t truncated; } oc_search_results;
typedef struct { uint64_t channel_id; uint64_t after_message_id; } oc_cursor;
typedef struct { uint16_t count; const oc_cursor *cursors; } oc_backfill_request;
typedef struct { uint64_t high_water; uint8_t more; } oc_backfill_done;
typedef struct { uint16_t code; uint8_t fatal; oc_slice context; oc_slice message; } oc_error;

/* --- Frame encoders ----------------------------------------------------- */
/*
 * Each writes a complete frame (header + payload) into `w`. Frozen handshake
 * frames force version 1; the rest take the negotiated `version`. Returns
 * OC_OK, OC_E_OVERFLOW (buffer too small), OC_E_TOO_LARGE (frame over the
 * limit), or OC_E_BODY_TOO_LARGE (body over OC_MAX_BODY_SIZE).
 */
oc_result oc_encode_hello(oc_wbuf *w, const oc_hello *m);
oc_result oc_encode_welcome(oc_wbuf *w, const oc_welcome *m);
oc_result oc_encode_reject(oc_wbuf *w, const oc_reject *m);
oc_result oc_encode_auth_challenge(oc_wbuf *w, uint16_t version, const oc_auth_challenge *m);
oc_result oc_encode_auth(oc_wbuf *w, uint16_t version, const oc_auth *m);
oc_result oc_encode_auth_ok(oc_wbuf *w, uint16_t version, const oc_auth_ok *m);
oc_result oc_encode_logout(oc_wbuf *w, uint16_t version, const oc_logout *m);
oc_result oc_encode_send(oc_wbuf *w, uint16_t version, const oc_send *m);
oc_result oc_encode_send_ack(oc_wbuf *w, uint16_t version, const oc_send_ack *m);
oc_result oc_encode_broadcast(oc_wbuf *w, uint16_t version, const oc_broadcast *m);
oc_result oc_encode_client_ack(oc_wbuf *w, uint16_t version, const oc_client_ack *m);
oc_result oc_encode_edit(oc_wbuf *w, uint16_t version, const oc_edit *m);
oc_result oc_encode_delete(oc_wbuf *w, uint16_t version, const oc_delete *m);
oc_result oc_encode_msg_edited(oc_wbuf *w, uint16_t version, const oc_msg_edited *m);
oc_result oc_encode_msg_deleted(oc_wbuf *w, uint16_t version, const oc_msg_deleted *m);
oc_result oc_encode_react(oc_wbuf *w, uint16_t version, const oc_react *m);
oc_result oc_encode_reaction_updated(oc_wbuf *w, uint16_t version, const oc_reaction_updated *m);
oc_result oc_encode_list_reactions(oc_wbuf *w, uint16_t version, const oc_list_reactions *m);
oc_result oc_encode_reactions(oc_wbuf *w, uint16_t version, const oc_reactions *m);
oc_result oc_encode_send_reply(oc_wbuf *w, uint16_t version, const oc_send_reply *m);
oc_result oc_encode_thread_reply(oc_wbuf *w, uint16_t version, const oc_thread_reply *m);
oc_result oc_encode_list_thread(oc_wbuf *w, uint16_t version, const oc_list_thread *m);
oc_result oc_encode_thread(oc_wbuf *w, uint16_t version, const oc_thread *m);
oc_result oc_encode_thread_meta(oc_wbuf *w, uint16_t version, const oc_thread_meta *m);
oc_result oc_encode_create_channel(oc_wbuf *w, uint16_t version, const oc_create_channel *m);
oc_result oc_encode_channel_info(oc_wbuf *w, uint16_t version, const oc_channel_info *m);
oc_result oc_encode_list_channels(oc_wbuf *w, uint16_t version);
oc_result oc_encode_channel_list(oc_wbuf *w, uint16_t version, const oc_channel_list *m);
oc_result oc_encode_join_channel(oc_wbuf *w, uint16_t version, const oc_channel_ref *m);
oc_result oc_encode_leave_channel(oc_wbuf *w, uint16_t version, const oc_channel_ref *m);
oc_result oc_encode_invite_to_channel(oc_wbuf *w, uint16_t version, const oc_channel_member_op *m);
oc_result oc_encode_remove_from_channel(oc_wbuf *w, uint16_t version, const oc_channel_member_op *m);
oc_result oc_encode_open_dm(oc_wbuf *w, uint16_t version, const oc_open_dm *m);
oc_result oc_encode_set_presence(oc_wbuf *w, uint16_t version, const oc_set_presence *m);
oc_result oc_encode_presence_update(oc_wbuf *w, uint16_t version, const oc_presence_update *m);
oc_result oc_encode_typing(oc_wbuf *w, uint16_t version, const oc_typing *m);
oc_result oc_encode_typing_update(oc_wbuf *w, uint16_t version, const oc_typing_update *m);
oc_result oc_encode_list_users(oc_wbuf *w, uint16_t version);
oc_result oc_encode_user_list(oc_wbuf *w, uint16_t version, const oc_user_list *m);
oc_result oc_encode_set_role(oc_wbuf *w, uint16_t version, const oc_set_role *m);
oc_result oc_encode_invite_user(oc_wbuf *w, uint16_t version, const oc_invite_user *m);
oc_result oc_encode_remove_user(oc_wbuf *w, uint16_t version, const oc_remove_user *m);
oc_result oc_encode_user_updated(oc_wbuf *w, uint16_t version, const oc_user_updated *m);
oc_result oc_encode_invite_created(oc_wbuf *w, uint16_t version, const oc_invite_created *m);
oc_result oc_encode_redeem_invite(oc_wbuf *w, uint16_t version, const oc_redeem_invite *m);
oc_result oc_encode_search(oc_wbuf *w, uint16_t version, const oc_search *m);
oc_result oc_encode_search_results(oc_wbuf *w, uint16_t version, const oc_search_results *m);
oc_result oc_encode_backfill_request(oc_wbuf *w, uint16_t version, const oc_backfill_request *m);
oc_result oc_encode_backfill_done(oc_wbuf *w, uint16_t version, const oc_backfill_done *m);
oc_result oc_encode_error(oc_wbuf *w, uint16_t version, const oc_error *m);

/* --- Frame decoders ----------------------------------------------------- */
/*
 * Each reads a payload (positioned by oc_parse_frame) into the out struct.
 * Returns OC_OK or OC_E_MALFORMED (payload underflow / trailing-length
 * mismatch). Backfill-request decode copies up to `cap` cursors into `cursors`
 * and reports the wire count in `*out_count` (which may exceed `cap`, meaning
 * the caller's array was too small).
 */
oc_result oc_decode_hello(oc_rbuf *p, oc_hello *m);
oc_result oc_decode_welcome(oc_rbuf *p, oc_welcome *m);
oc_result oc_decode_reject(oc_rbuf *p, oc_reject *m);
oc_result oc_decode_auth_challenge(oc_rbuf *p, oc_auth_challenge *m);
oc_result oc_decode_auth(oc_rbuf *p, oc_auth *m);
oc_result oc_decode_auth_ok(oc_rbuf *p, oc_auth_ok *m);
oc_result oc_decode_logout(oc_rbuf *p, oc_logout *m);
oc_result oc_decode_send(oc_rbuf *p, oc_send *m);
oc_result oc_decode_send_ack(oc_rbuf *p, oc_send_ack *m);
oc_result oc_decode_broadcast(oc_rbuf *p, oc_broadcast *m);
oc_result oc_decode_client_ack(oc_rbuf *p, oc_client_ack *m);
oc_result oc_decode_edit(oc_rbuf *p, oc_edit *m);
oc_result oc_decode_delete(oc_rbuf *p, oc_delete *m);
oc_result oc_decode_msg_edited(oc_rbuf *p, oc_msg_edited *m);
oc_result oc_decode_msg_deleted(oc_rbuf *p, oc_msg_deleted *m);
oc_result oc_decode_react(oc_rbuf *p, oc_react *m);
oc_result oc_decode_reaction_updated(oc_rbuf *p, oc_reaction_updated *m);
oc_result oc_decode_list_reactions(oc_rbuf *p, oc_list_reactions *m);
oc_result oc_decode_reactions(oc_rbuf *p, oc_reaction_entry *entries, uint16_t cap, uint16_t *out_count, uint64_t *out_message_id);
oc_result oc_decode_send_reply(oc_rbuf *p, oc_send_reply *m);
oc_result oc_decode_thread_reply(oc_rbuf *p, oc_thread_reply *m);
oc_result oc_decode_list_thread(oc_rbuf *p, oc_list_thread *m);
oc_result oc_decode_thread(oc_rbuf *p, oc_thread *m);
oc_result oc_decode_thread_meta(oc_rbuf *p, oc_thread_meta *m);
oc_result oc_decode_create_channel(oc_rbuf *p, oc_create_channel *m);
oc_result oc_decode_channel_info(oc_rbuf *p, oc_channel_info *m);
oc_result oc_decode_list_channels(oc_rbuf *p);
oc_result oc_decode_channel_list(oc_rbuf *p, oc_channel_list_entry *entries, uint16_t cap, uint16_t *out_count);
oc_result oc_decode_join_channel(oc_rbuf *p, oc_channel_ref *m);
oc_result oc_decode_leave_channel(oc_rbuf *p, oc_channel_ref *m);
oc_result oc_decode_invite_to_channel(oc_rbuf *p, oc_channel_member_op *m);
oc_result oc_decode_remove_from_channel(oc_rbuf *p, oc_channel_member_op *m);
oc_result oc_decode_open_dm(oc_rbuf *p, oc_open_dm *m);
oc_result oc_decode_set_presence(oc_rbuf *p, oc_set_presence *m);
oc_result oc_decode_presence_update(oc_rbuf *p, oc_presence_update *m);
oc_result oc_decode_typing(oc_rbuf *p, oc_typing *m);
oc_result oc_decode_typing_update(oc_rbuf *p, oc_typing_update *m);
oc_result oc_decode_list_users(oc_rbuf *p);
oc_result oc_decode_user_list(oc_rbuf *p, oc_user_list_entry *entries, uint16_t cap, uint16_t *out_count);
oc_result oc_decode_set_role(oc_rbuf *p, oc_set_role *m);
oc_result oc_decode_invite_user(oc_rbuf *p, oc_invite_user *m);
oc_result oc_decode_remove_user(oc_rbuf *p, oc_remove_user *m);
oc_result oc_decode_user_updated(oc_rbuf *p, oc_user_updated *m);
oc_result oc_decode_invite_created(oc_rbuf *p, oc_invite_created *m);
oc_result oc_decode_redeem_invite(oc_rbuf *p, oc_redeem_invite *m);
oc_result oc_decode_search(oc_rbuf *p, oc_search *m);
oc_result oc_decode_search_results(oc_rbuf *p, oc_search_result_entry *entries, uint16_t cap, uint16_t *out_count, uint8_t *out_truncated);
oc_result oc_decode_backfill_request(oc_rbuf *p, oc_cursor *cursors, uint16_t cap, uint16_t *out_count);
oc_result oc_decode_backfill_done(oc_rbuf *p, oc_backfill_done *m);
oc_result oc_decode_error(oc_rbuf *p, oc_error *m);

/* --- Inner credential codec (AUTH.md §2; PROTOCOL.md §4.2) --------------- */
/* For method=local the AUTH `credential` payload is itself `str username`
 * followed by `str password`. These pack/parse that inner payload, kept here
 * so the client and daemon share one definition of the format. Encode writes
 * into `w` (whose bytes then become the credential slice); parse reads a
 * credential slice, rejecting trailing garbage with OC_E_MALFORMED. */
oc_result oc_encode_local_credential(oc_wbuf *w, oc_slice username, oc_slice password);
oc_result oc_parse_local_credential(oc_slice credential, oc_slice *username, oc_slice *password);

#endif /* OPENCHIME_PROTOCOL_H */
