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

/* The only version this codec speaks. Both sides send it as min AND max, so a
 * mismatched pair is rejected at the handshake with VERSION_TOO_OLD/TOO_NEW
 * rather than discovering the disagreement halfway through a frame.
 *
 * **Bump this whenever a frame's LAYOUT changes** — a new field, a reordering, a
 * field that stops being optional — not only when a frame is added. Adding a
 * frame is safe (an old peer never sends or expects it); changing a layout is
 * not, because both sides still claim the same number while disagreeing about
 * what it means. That is not hypothetical: version 1 was left alone while
 * CHANNEL_INFO and CHANNEL_LIST both grew fields, and the result was a client
 * and daemon that connected happily and then dropped the link on a decode
 * failure, reported as "connection lost — reconnecting" with nothing pointing at
 * the real cause.
 *
 * v2 (2026-07-29): CHANNEL_INFO gained topic/archived and made peer_id
 * unconditional; CHANNEL_LIST gained topic/archived/created_at/preview/
 * preview_author. Shipping client and daemon together (ARCH-61) means there is
 * no compatibility window to preserve — only a mismatch to detect loudly. */
#define OC_PROTOCOL_VERSION 2u

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
/* Attachment transfer (REQ-140, ARCH-69). A chunk's data must leave room for the
 * frame header + the chunk's fixed fields under OC_MAX_FRAME_SIZE. */
#define OC_ATTACH_CHUNK_SIZE   65024u /* 63.5 KiB data per UPLOAD/DOWNLOAD chunk  */
#define OC_MAX_ATTACHMENT_SIZE (100ull * 1024 * 1024) /* 100 MiB default cap      */

/* --- Message types (PROTOCOL.md §9) ------------------------------------- */

typedef enum {
    OC_MSG_HELLO            = 0x0001, /* C->S, frozen@v1 */
    OC_MSG_WELCOME          = 0x0002, /* S->C, frozen@v1 */
    OC_MSG_REJECT           = 0x0003, /* S->C, frozen@v1, fatal */
    OC_MSG_AUTH             = 0x0010, /* C->S */
    OC_MSG_AUTH_OK          = 0x0011, /* S->C */
    OC_MSG_AUTH_CHALLENGE   = 0x0012, /* S->C */
    OC_MSG_LOGOUT           = 0x0013, /* C->S */
    OC_MSG_WORKSPACE_INFO   = 0x0014, /* S->C, pushed after AUTH_OK */
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
    OC_MSG_READ_CURSOR      = 0x0033, /* S->C, a member's read cursor advanced (REQ-090 seen-by) */
    OC_MSG_HISTORY_REQUEST  = 0x0034, /* C->S, page BACKWARDS through a channel (§6.3) */
    OC_MSG_PIN              = 0x0035, /* C->S (REQ-230), pin/unpin a message */
    OC_MSG_PIN_UPDATED      = 0x0036, /* S->C, pin fan-out to every channel member */
    OC_MSG_LIST_PINS        = 0x0037, /* C->S, a channel's pinned messages */
    OC_MSG_PINNED_MSG       = 0x0038, /* S->C, one pinned message (streamed, body included) */
    OC_MSG_PINS             = 0x0039, /* S->C, terminator of a LIST_PINS response */
    OC_MSG_LIST_MEMBERS     = 0x003A, /* C->S (REQ-031), a channel's members */
    OC_MSG_MEMBER_ENTRY     = 0x003B, /* S->C, one channel member (streamed) */
    OC_MSG_MEMBERS          = 0x003C, /* S->C, terminator of a LIST_MEMBERS response */
    OC_MSG_LIST_FILES       = 0x003D, /* C->S (REQ-143), files in a channel (0 = everywhere) */
    OC_MSG_FILE_ENTRY       = 0x003E, /* S->C, one shared file (streamed) */
    OC_MSG_FILES            = 0x003F, /* S->C, terminator of a LIST_FILES response */
    OC_MSG_CREATE_CHANNEL     = 0x0050, /* C->S (REQ-050) */
    OC_MSG_CHANNEL_INFO       = 0x0051, /* S->C, ack for create/join/leave/invite/remove */
    OC_MSG_LIST_CHANNELS      = 0x0052, /* C->S */
    OC_MSG_CHANNEL_LIST       = 0x0053, /* S->C */
    OC_MSG_JOIN_CHANNEL       = 0x0054, /* C->S */
    OC_MSG_LEAVE_CHANNEL      = 0x0055, /* C->S */
    OC_MSG_INVITE_TO_CHANNEL  = 0x0056, /* C->S (REQ-033, channel-level) */
    OC_MSG_REMOVE_FROM_CHANNEL= 0x0057, /* C->S (REQ-033, channel-level) */
    OC_MSG_OPEN_DM            = 0x0058, /* C->S, open/get a 1:1 DM (REQ-050) */
    OC_MSG_CREATE_WEBHOOK     = 0x0059, /* C->S, mint an incoming-webhook token (REQ-170) */
    OC_MSG_WEBHOOK_INFO       = 0x005A, /* S->C, the minted webhook id + token (shown once) */
    OC_MSG_LIST_WEBHOOKS      = 0x005B, /* C->S, list a channel's webhooks (REQ-170) */
    OC_MSG_WEBHOOK_LIST       = 0x005C, /* S->C, the webhook list (no tokens) */
    OC_MSG_DELETE_WEBHOOK     = 0x005D, /* C->S, remove a webhook */
    OC_MSG_WEBHOOK_DELETED    = 0x005E, /* S->C, ack for DELETE_WEBHOOK */
    OC_MSG_UPDATE_CHANNEL     = 0x005F, /* C->S (REQ-034/035/036), mutate a channel */
    /* Saved items + activity feed (REQ-231/139, ARCH-95). 0x0060-0x0061 are
     * taken; this family starts at the next free run. */
    OC_MSG_SAVE_ITEM        = 0x0062, /* C->S, save/unsave a message (private) */
    OC_MSG_SAVED_UPDATED    = 0x0063, /* S->C, ack to the actor only */
    OC_MSG_LIST_SAVED       = 0x0064, /* C->S, my saved items */
    OC_MSG_SAVED_MSG        = 0x0065, /* S->C, one saved message (streamed) */
    OC_MSG_SAVED            = 0x0066, /* S->C, terminator */
    OC_MSG_LIST_ACTIVITY    = 0x0067, /* C->S (REQ-139), what involved me */
    OC_MSG_ACTIVITY_ENTRY   = 0x0068, /* S->C, one activity item (streamed) */
    OC_MSG_ACTIVITY         = 0x0069, /* S->C, terminator + the seen watermark */
    OC_MSG_SET_PRESENCE     = 0x0070, /* C->S, set own presence (REQ-120) */
    OC_MSG_PRESENCE_UPDATE  = 0x0071, /* S->C, a user's presence changed */
    OC_MSG_TYPING           = 0x0072, /* C->S, "I am typing" in a channel (REQ-121) */
    OC_MSG_TYPING_UPDATE    = 0x0073, /* S->C, relay of a typing signal */
    OC_MSG_UPLOAD_BEGIN     = 0x0080, /* C->S, declare an attachment upload (REQ-140) */
    OC_MSG_UPLOAD_READY     = 0x0081, /* S->C, id + chunk size + in-flight window */
    OC_MSG_UPLOAD_CHUNK     = 0x0082, /* C->S, one upload chunk */
    OC_MSG_UPLOAD_ACK       = 0x0083, /* S->C, window advance */
    OC_MSG_UPLOAD_END       = 0x0084, /* C->S, finish the upload */
    OC_MSG_UPLOAD_OK        = 0x0085, /* S->C, upload finalized (id + size + sha256) */
    OC_MSG_DOWNLOAD_BEGIN   = 0x0086, /* C->S, request an attachment (REQ-141) */
    OC_MSG_DOWNLOAD_INFO    = 0x0087, /* S->C, metadata preceding the bytes */
    OC_MSG_DOWNLOAD_CHUNK   = 0x0088, /* S->C, one download chunk */
    OC_MSG_DOWNLOAD_END     = 0x0089, /* S->C, all bytes sent */
    OC_MSG_TRANSFER_CANCEL  = 0x008A, /* C->S, abort an in-progress transfer */
    OC_MSG_SET_NOTIFY_PREF  = 0x0090, /* C->S, set a channel's notification level (REQ-130) */
    OC_MSG_SET_DND          = 0x0091, /* C->S, set the do-not-disturb window (REQ-131) */
    OC_MSG_LIST_NOTIFY_PREFS= 0x0092, /* C->S, request all notification settings */
    OC_MSG_NOTIFY_PREFS     = 0x0093, /* S->C, DND + per-channel levels (also a sync push) */
    OC_MSG_SET_CLIENT_SETTING  = 0x0094, /* C->S, upsert a synced client setting (empty value = delete) */
    OC_MSG_LIST_CLIENT_SETTINGS= 0x0095, /* C->S, request a client_type bucket's settings */
    OC_MSG_CLIENT_SETTINGS     = 0x0096, /* S->C, a bucket snapshot (also a device-sync push) */
    OC_MSG_STORAGE_STATUS_REQ  = 0x0097, /* C->S, owner/admin: ask for storage usage (REQ-214) */
    OC_MSG_STORAGE_STATUS      = 0x0098, /* S->C, usage + policy + what maintenance reclaimed */
    OC_MSG_AUDIT_QUERY         = 0x0099, /* C->S, owner/admin: page the audit log (REQ-251) */
    OC_MSG_AUDIT_PAGE          = 0x009A, /* S->C, a page of entries, newest first */
    OC_MSG_CALL_JOIN        = 0x00A0, /* C->S, join a channel's audio call (REQ-150) */
    OC_MSG_CALL_LEAVE       = 0x00A1, /* C->S, leave the call */
    OC_MSG_CALL_JOINED      = 0x00A2, /* S->C, to the joiner: call id + UDP endpoint/token + roster */
    OC_MSG_CALL_ROSTER      = 0x00A3, /* S->C, to participants: roster changed */
    OC_MSG_REGISTER_DEVICE_TOKEN   = 0x00B0, /* C->S, register a mobile push token (REQ-132) */
    OC_MSG_UNREGISTER_DEVICE_TOKEN = 0x00B1, /* C->S, drop a push token (logout / token change) */
    OC_MSG_DEVICE_TOKEN_ACK        = 0x00B2, /* S->C, register/unregister acknowledged */
    OC_MSG_LIST_USERS       = 0x0040, /* C->S, tenant user enumeration */
    OC_MSG_USER_LIST        = 0x0041, /* S->C */
    OC_MSG_SET_ROLE         = 0x0042, /* C->S (ARCH-60, REQ-030) */
    OC_MSG_INVITE_USER      = 0x0043, /* C->S (REQ-033, tenant-level; owner/admin) */
    OC_MSG_REMOVE_USER      = 0x0044, /* C->S (REQ-033, tenant-level; owner/admin) */
    OC_MSG_USER_UPDATED     = 0x0045, /* S->C, ack/push for SET_ROLE + REMOVE_USER */
    OC_MSG_INVITE_CREATED   = 0x0046, /* S->C, the minted invite token */
    OC_MSG_REDEEM_INVITE    = 0x0047, /* C->S, pre-auth account creation */
    OC_MSG_SET_DISPLAY_NAME = 0x0048, /* C->S, change your own display name (REQ-020) */
    OC_MSG_CHANGE_PASSWORD  = 0x0049, /* C->S, change your own local password (verify old) */
    OC_MSG_PROFILE_UPDATED  = 0x004A, /* S->C, a user's display name changed (also the self ack) */
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
    OC_ERR_USER_LIMIT          = 2004, /* workspace at its registered-user cap (OPENCHIME_MAX_USERS) */
    OC_ERR_BODY_TOO_LARGE      = 3001,
    OC_ERR_NOT_A_MEMBER        = 3002,
    OC_ERR_UNKNOWN_CHANNEL     = 3003,
    OC_ERR_SEND_RATE_LIMITED   = 3004,
    OC_ERR_FORBIDDEN           = 3005, /* actor may not perform the action (role, or not the author) */
    OC_ERR_LAST_OWNER          = 3006, /* would remove/demote the last owner (REQ-030) */
    OC_ERR_UNKNOWN_MESSAGE     = 3007, /* no such message in the channel (edit/delete) */
    OC_ERR_INVALID_CHANNEL     = 3008, /* bad channel name on CREATE_CHANNEL (empty/too long) */
    OC_ERR_INVALID_REACTION    = 3009, /* empty/oversized emoji on REACT */
    OC_ERR_ATTACHMENT_TOO_LARGE = 3010, /* upload exceeds MAX_ATTACHMENT_SIZE (REQ-140) */
    OC_ERR_UNKNOWN_ATTACHMENT  = 3011, /* no such attachment, or not finalized (REQ-141) */
    OC_ERR_STORAGE_FULL        = 3012, /* upload refused: below the DB reserve (REQ-216) */
    OC_ERR_ATTACHMENT_GONE     = 3013, /* reclaimed by age or storage pressure (REQ-215/217) */
    OC_ERR_TRANSFER_PROTOCOL   = 3015, /* out-of-order/oversized chunk or bad transfer state */
    OC_ERR_UNKNOWN_WEBHOOK     = 3016, /* no such (or disabled) incoming webhook token (REQ-170) */
    OC_ERR_CHANNEL_EXISTS      = 3017, /* a channel of that name already exists (names are unique, case-insensitively) */
    OC_ERR_TOO_MANY_PINS       = 3018, /* channel already holds OC_MAX_PINS pins (REQ-230) */
    OC_ERR_CHANNEL_ARCHIVED    = 3019, /* channel is archived: read-only (REQ-035) */
    OC_ERR_INVALID_DEVICE_TOKEN = 3014, /* empty token or unknown platform on REGISTER_DEVICE_TOKEN */
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

/* Audio calls (REQ-150-152): one call per channel, small participant sets. */
#define OC_MAX_CALL_PARTICIPANTS 32u

/* Per-channel notification level (REQ-130). */
#define OC_NOTIFY_ALL      0u
#define OC_NOTIFY_MENTIONS 1u
#define OC_NOTIFY_NONE     2u
#define OC_MAX_NOTIFY_PREFS 1024u   /* cap on a NOTIFY_PREFS list */

/* Synced client settings (the daemon-side settings bucket). */
#define OC_MAX_CLIENT_SETTINGS 128u /* cap on a CLIENT_SETTINGS snapshot */
#define OC_CLIENT_TYPE_MAX     32u  /* client_type string cap (bytes) */
#define OC_SETTING_KEY_MAX     64u  /* setting key string cap (bytes) */
#define OC_SETTING_VALUE_MAX   512u /* setting value string cap (bytes) */

/* Channel kind (SCHEMA.md channels.kind) and the name cap for CREATE_CHANNEL. */
#define OC_CHANNEL_KIND    0u   /* a named channel */
#define OC_CHANNEL_KIND_DM 1u   /* a direct-message conversation (reserved) */
#define OC_MAX_CHANNEL_NAME 64u /* channel-name length cap (bytes) */
#define OC_MAX_DISPLAY_NAME 48u /* display-name length cap (bytes), self-service rename */

/* Account-creation invite token (AUTH.md §2); like a session token, only its
 * SHA-256 is stored. Presented in REDEEM_INVITE to set a password. */
#define OC_INVITE_TOKEN_LEN 32u

/* Emoji reaction op (REQ-070) and the emoji length cap (bytes; fits multi-
 * codepoint sequences like flags/ZWJ). */
#define OC_REACT_REMOVE 0u
#define OC_REACT_ADD    1u
#define OC_MAX_EMOJI    32u

/* Saved-item op (REQ-231) and activity kinds (REQ-139), ARCH-95. */
#define OC_SAVE_REMOVE 0u
#define OC_SAVE_ADD    1u
#define OC_ACT_MENTION  0u   /* someone named me */
#define OC_ACT_REACTION 1u   /* someone reacted to something I wrote */
#define OC_ACT_REPLY    2u   /* someone replied in a thread I started */
#define OC_MAX_SAVED    200u
#define OC_MAX_ACTIVITY 200u

/* Channel mutation ops (REQ-034/035/036, ARCH-93). One frame carries all four
 * because they all mutate one row and all fan out the same CHANNEL_INFO — the
 * op IS the difference. `value` is the new topic or name; unused for archive. */
#define OC_CHUP_TOPIC     0u   /* any member */
#define OC_CHUP_RENAME    1u   /* owner/admin */
#define OC_CHUP_ARCHIVE   2u   /* owner/admin */
#define OC_CHUP_UNARCHIVE 3u   /* owner/admin */
#define OC_MAX_TOPIC      250u /* bytes; Slack's cap, and a topic is one header line */
#define OC_MAX_PREVIEW    120u /* bytes of last-message preview in CHANNEL_LIST */

/* Pin op (REQ-230, ARCH-90) and the per-channel cap. A pin belongs to the
 * channel, not to the pinner: pinning an already-pinned message is a no-op
 * rather than a second pin. The cap bounds both the list frame and the work of
 * opening the pins view; Slack's is the same number. */
#define OC_PIN_REMOVE 0u
#define OC_PIN_ADD    1u
#define OC_MAX_PINS   100u

/* Response caps for the two channel-details listings (REQ-031, REQ-143). Both
 * are "show me this channel" views, not paged datasets: a bound keeps one frame
 * burst bounded, and a client that wants an older file has search. */
#define OC_MAX_MEMBER_LIST 500u
#define OC_MAX_FILE_LIST   200u

/* LOGOUT scope (PROTOCOL.md §4; REQ-182). */
#define OC_LOGOUT_THIS 0u   /* revoke just the presented session token */
#define OC_LOGOUT_ALL  1u   /* revoke every session of the authenticated user */

/* --- Frame payload structs ---------------------------------------------- */

/* Attachment linkage (REQ-140, ARCH-69). A message may reference up to
 * OC_MAX_ATTACH uploaded attachments. On the wire the list is an *optional
 * trailing field* on SEND/BROADCAST: it is written only when non-empty, and the
 * decoder reads it only if bytes remain after the base fields — so a message
 * with no attachments is byte-identical to the pre-attachment format and no
 * protocol-version bump is needed (client and daemon share this codec). SEND
 * carries just the ids to link; BROADCAST carries each linked attachment's
 * metadata so every reader can render/fetch it. */
#define OC_MAX_ATTACH 16u
/* One attachment's metadata as carried on a message (REQ-140). `reclaimed` is
 * set when the bytes have been removed by age or storage pressure (REQ-215/217)
 * while the row survives as a tombstone — so a client can render "no longer
 * available" in place, instead of offering a download that will fail. */
typedef struct {
    uint64_t id;
    oc_slice filename;
    oc_slice mime;
    uint64_t size;
    uint8_t  reclaimed;
} oc_attach_entry;

typedef struct { uint16_t min_version; uint16_t max_version; oc_slice client_info; } oc_hello;
typedef struct { uint16_t chosen_version; uint64_t server_time; } oc_welcome;
typedef struct { uint16_t code; oc_slice message; } oc_reject;
typedef struct { uint8_t methods; oc_slice oidc_params; } oc_auth_challenge;
typedef struct { uint8_t method; oc_slice credential; } oc_auth;
typedef struct { uint64_t user_id; uint8_t role; uint64_t session_expiry; oc_slice session_token; } oc_auth_ok;
typedef struct { uint8_t scope; oc_slice session_token; } oc_logout;
/* Pushed after AUTH_OK: infra facts about this workspace, from the daemon's
 * static config. deployment_mode ∈ {0 standalone,1 federated,2 managed};
 * workspace_name may be empty (client falls back to the host subdomain). */
typedef struct { uint8_t deployment_mode; uint32_t max_users; oc_slice workspace_name; } oc_workspace_info;
typedef struct { uint64_t channel_id; uint8_t idem[OC_IDEM_SIZE]; oc_slice body;
                 uint16_t n_attach; uint64_t attach_ids[OC_MAX_ATTACH]; } oc_send;
typedef struct { uint8_t idem[OC_IDEM_SIZE]; uint64_t channel_id; uint64_t message_id; uint64_t server_time; } oc_send_ack;
typedef struct { uint64_t message_id; uint64_t channel_id; uint64_t author_id; uint64_t server_time; oc_slice body;
                 uint16_t n_attach; oc_attach_entry attach[OC_MAX_ATTACH];
                 oc_slice author_name; } oc_broadcast;  /* override name (webhooks); empty = use author_id */
typedef struct { uint64_t channel_id; uint64_t message_id; } oc_client_ack;
/* A member's read cursor in a channel advanced (REQ-090 seen-by). */
typedef struct { uint64_t channel_id; uint64_t user_id; uint64_t message_id; } oc_read_cursor;
typedef struct { uint64_t channel_id; uint64_t message_id; oc_slice body; } oc_edit;
typedef struct { uint64_t channel_id; uint64_t message_id; } oc_delete;
typedef struct { uint64_t message_id; uint64_t channel_id; uint64_t author_id; uint64_t edited_at; oc_slice body; } oc_msg_edited;
typedef struct { uint64_t message_id; uint64_t channel_id; uint64_t author_id; uint64_t deleted_by; uint64_t deleted_at; } oc_msg_deleted;
typedef struct { uint64_t channel_id; uint64_t message_id; oc_slice emoji; uint8_t op; } oc_react;
typedef struct { uint64_t message_id; uint64_t channel_id; uint64_t user_id; oc_slice emoji; uint8_t op; uint64_t count; } oc_reaction_updated;
typedef struct { uint64_t channel_id; uint64_t message_id; } oc_list_reactions;
typedef struct { oc_slice emoji; uint64_t user_id; } oc_reaction_entry;
typedef struct { uint64_t message_id; uint16_t count; const oc_reaction_entry *entries; } oc_reactions;
/* Pins (REQ-230). PIN carries the op; PIN_UPDATED is the fan-out every member
 * receives. LIST_PINS streams PINNED_MSG frames — each self-framed, so a full
 * body is fine — terminated by PINS, exactly as LIST_THREAD does, because a
 * pinned message is often scrolled out of a client's loaded history. */
typedef struct { uint64_t channel_id; uint64_t message_id; uint8_t op; } oc_pin;
typedef struct { uint64_t message_id; uint64_t channel_id; uint64_t user_id; uint8_t op; uint64_t pinned_at; } oc_pin_updated;
typedef struct { uint64_t channel_id; } oc_list_pins;
typedef struct { uint64_t message_id; uint64_t channel_id; uint64_t author_id; uint64_t server_time;
                 uint64_t pinned_by; uint64_t pinned_at; oc_slice body;
                 /* The first attachment's filename, or empty. An attachment-only
                  * message has no body at all, so without this a pinned file
                  * rendered as a blank row in the pins list. */
                 oc_slice attach_name; } oc_pinned_msg;
typedef struct { uint64_t channel_id; uint32_t count; } oc_pins;

/* A channel's members (REQ-031) and its shared files (REQ-143, ARCH-91). Both
 * follow the LIST_PINS shape — stream the entries, then a terminator — because
 * both carry variable-length text and both are lists a client holds nothing on
 * disk to reconstruct (ARCH-88). */
typedef struct { uint64_t channel_id; } oc_list_members;
typedef struct { uint64_t channel_id; uint64_t user_id; uint8_t role; uint64_t joined_at; } oc_member_entry;
typedef struct { uint64_t channel_id; uint32_t count; } oc_members;

/* channel_id 0 means "every channel I can read" — the same query with a wider
 * WHERE, which is what makes a workspace-wide files view free. */
typedef struct { uint64_t channel_id; } oc_list_files;
typedef struct {
    uint64_t attachment_id, channel_id, message_id, uploader_id;
    uint64_t size, created_at;
    uint8_t  reclaimed;          /* bytes gone (REQ-215/217); row kept, no download */
    oc_slice filename, mime;
} oc_file_entry;
typedef struct { uint64_t channel_id; uint32_t count; } oc_files;

typedef struct { uint64_t channel_id; uint8_t idem[OC_IDEM_SIZE]; uint64_t parent_id; oc_slice body;
                 uint16_t n_attach; uint64_t attach_ids[OC_MAX_ATTACH]; } oc_send_reply;
typedef struct { uint64_t message_id; uint64_t channel_id; uint64_t parent_id; uint64_t author_id; uint64_t server_time; uint32_t reply_count; oc_slice body;
                 uint16_t n_attach; oc_attach_entry attach[OC_MAX_ATTACH]; } oc_thread_reply;
typedef struct { uint64_t channel_id; uint64_t parent_id; } oc_list_thread;
/* THREAD is the terminator of a LIST_THREAD response: the daemon streams the
 * replies as THREAD_REPLY frames (each self-framed, so a 64KB body is fine),
 * then this frame closes the stream — mirroring BACKFILL_DONE (§6.2). */
typedef struct { uint64_t parent_id; uint32_t count; uint8_t truncated; } oc_thread;
typedef struct { uint64_t message_id; uint32_t reply_count; uint64_t last_reply_at; } oc_thread_meta;
typedef struct { oc_slice name; uint8_t is_public; } oc_create_channel;
typedef struct { uint64_t channel_id; uint8_t op; oc_slice value; } oc_update_channel;

/* Saved items (REQ-231) — private, so SAVED_UPDATED goes to the actor only. */
typedef struct { uint64_t message_id; uint8_t op; } oc_save_item;
typedef struct { uint64_t message_id; uint8_t op; uint64_t saved_at; } oc_saved_updated;
typedef struct { uint64_t message_id; uint64_t channel_id; uint64_t author_id;
                 uint64_t server_time; uint64_t saved_at; oc_slice body;
                 oc_slice attach_name; } oc_saved_msg;
typedef struct { uint32_t count; } oc_saved;

/* Activity (REQ-139). `actor_id` is who did the thing; `text` is the message
 * body for a mention or reply, and the emoji for a reaction. */
typedef struct { uint8_t kind; uint64_t message_id; uint64_t channel_id;
                 uint64_t actor_id; uint64_t at; oc_slice text; } oc_activity_entry;
typedef struct { uint32_t count; uint64_t seen_at; } oc_activity;
/* CHANNEL_INFO is the channel-state frame: the ack for create/join/leave/
 * invite/remove, and (ARCH-93) the fan-out when a topic/name/archive changes.
 * `peer_id` used to be an "optional trailing" field written only for DMs; that
 * trick does not survive a SECOND optional field, so as of REQ-034/035/036 the
 * layout is fixed and peer_id is always written (0 when not a DM). */
typedef struct { uint64_t channel_id; uint8_t kind; oc_slice name; uint8_t is_public; uint8_t joined; uint64_t created_at;
                 uint64_t peer_id; oc_slice topic; uint8_t archived; } oc_channel_info;
typedef struct { uint64_t channel_id; } oc_channel_ref;                       /* JOIN / LEAVE */
typedef struct { uint64_t channel_id; uint64_t user_id; } oc_channel_member_op; /* INVITE / REMOVE */
/* CHANNEL_LIST entry. `last_message_at`/`unread` were added 2026-07-27 so a
 * client that keeps no local cache (ARCH-88) can order and badge the sidebar on
 * first paint rather than only after backfill. This amends the v1 layout in
 * place rather than riding a trailing block, because the entries are packed
 * sequentially and r_done() rejects leftover bytes — and client and daemon ship
 * from this one file (ARCH-61), so they change together. */
typedef struct { uint64_t channel_id; oc_slice name; uint8_t is_public; uint8_t joined; uint8_t kind;
                 uint64_t last_message_at; uint32_t unread; uint64_t peer_id;
                 oc_slice topic; uint8_t archived; uint64_t created_at;
                 /* The newest top-level message, for a scannable list: a client
                  * that caches nothing (ARCH-88) has no other way to show one. */
                 oc_slice preview; uint64_t preview_author; } oc_channel_list_entry;
typedef struct { uint64_t user_id; } oc_open_dm;
/* Incoming-webhook management (REQ-170). CREATE_WEBHOOK asks for a token scoped
 * to a channel; WEBHOOK_INFO returns the id + the raw 32-byte token (shown once,
 * the client hex-encodes it into the POST URL). */
typedef struct { uint64_t channel_id; oc_slice label; } oc_create_webhook;
typedef struct { uint64_t webhook_id; uint64_t channel_id; oc_slice token; } oc_webhook_info;
typedef struct { uint64_t channel_id; } oc_list_webhooks;
typedef struct { uint64_t webhook_id; uint64_t channel_id; oc_slice label; uint8_t disabled; } oc_webhook_list_entry;
typedef struct { uint16_t count; const oc_webhook_list_entry *entries; } oc_webhook_list;  /* tokens never listed */
typedef struct { uint64_t webhook_id; } oc_delete_webhook;
typedef struct { uint64_t webhook_id; } oc_webhook_deleted;
typedef struct { uint8_t status; } oc_set_presence;
typedef struct { uint64_t user_id; uint8_t status; } oc_presence_update;
typedef struct { uint64_t channel_id; } oc_typing;
typedef struct { uint64_t channel_id; uint64_t user_id; } oc_typing_update;
/* Notification preferences (REQ-130/131). */
typedef struct { uint64_t channel_id; uint8_t level; } oc_set_notify_pref;
typedef struct { uint8_t enabled; uint16_t start_min; uint16_t end_min; } oc_set_dnd;

/* Push device-token registration (REQ-132). platform: OC_PUSH_APNS / OC_PUSH_FCM. */
#define OC_PUSH_APNS        0u
#define OC_PUSH_FCM         1u
#define OC_DEVICE_TOKEN_MAX 512u
typedef struct { uint8_t platform; oc_slice token; } oc_register_device_token;
typedef struct { uint8_t ok; uint16_t code; } oc_device_token_ack;
typedef struct { uint64_t channel_id; uint8_t level; } oc_notify_pref_entry;
typedef struct { uint8_t dnd_enabled; uint16_t dnd_start_min; uint16_t dnd_end_min;
                 uint16_t count; const oc_notify_pref_entry *entries; } oc_notify_prefs;
/* Synced client settings bucket (the daemon-side layer of the client config). */
typedef struct { oc_slice client_type; oc_slice key; oc_slice value; } oc_set_client_setting;
typedef struct { oc_slice client_type; } oc_list_client_settings;
typedef struct { oc_slice key; oc_slice value; } oc_client_setting_entry;
/* Storage usage + policy + reclamation history (REQ-214/215). Sent only to an
 * owner/admin. `attach_bytes`/`attach_count` cover live attachments; the
 * reclaimed_* counts are cumulative and come from the attachments table's
 * reclaim_reason, which is what makes eviction auditable after the fact. */
typedef struct {
    uint64_t total_bytes;
    uint64_t avail_bytes;
    uint64_t attach_bytes;
    uint64_t attach_count;
    uint64_t reclaimed_orphan;
    uint64_t reclaimed_expired;
    uint64_t reclaimed_evicted;
    uint64_t last_reclaim_ms;   /* 0 if nothing has ever been reclaimed */
    uint64_t max_age_days;      /* 0 = keep forever */
    uint64_t reserve_bytes;
    uint8_t  evict_enabled;
    uint8_t  under_pressure;
} oc_storage_status;

/* Audit log paging (REQ-251). `before_ms` pages backwards from a timestamp
 * rather than by offset, so a boundary stays stable as new entries arrive;
 * 0 asks for the newest page. */
#define OC_AUDIT_PAGE_MAX 200u
typedef struct { uint64_t before_ms; uint16_t limit; } oc_audit_query;

typedef struct {
    uint64_t at_ms;
    uint64_t actor_id;
    uint64_t target_id;
    oc_slice actor_name;
    oc_slice action;
    oc_slice target;
    oc_slice detail;      /* never the secret involved (ARCH-79) */
    uint8_t  family;      /* 1 admin, 2 account, 3 security, 4 moderation */
    uint8_t  outcome;     /* 1 ok, 0 denied/failed */
} oc_audit_entry;

typedef struct {
    uint16_t count;
    const oc_audit_entry *entries;
} oc_audit_page;

typedef struct { oc_slice client_type; uint16_t count;
                 const oc_client_setting_entry *entries; } oc_client_settings;
/* Audio call signaling (REQ-150). CALL_JOINED carries the joiner's private UDP
 * endpoint + bearer token (empty until the sidecar milestone) plus the roster;
 * CALL_ROSTER carries just the participant list, pushed on any change. */
typedef struct { uint64_t channel_id; } oc_call_join;
typedef struct { uint64_t channel_id; } oc_call_leave;
typedef struct { uint64_t channel_id; uint64_t call_id; uint16_t udp_port; oc_slice token;
                 uint16_t count; const uint64_t *participants; } oc_call_joined;
typedef struct { uint64_t channel_id; uint64_t call_id;
                 uint16_t count; const uint64_t *participants; } oc_call_roster;
/* Attachment transfer (REQ-140/141, ARCH-69). `data` chunks are zero-copy views.
 * sha256 is a 32-byte digest carried as `bytes`. */
typedef struct { uint64_t channel_id; uint8_t idem[OC_IDEM_SIZE]; oc_slice filename; oc_slice mime; uint64_t total_size; } oc_upload_begin;
typedef struct { uint64_t attachment_id; uint32_t chunk_size; uint32_t window_bytes; } oc_upload_ready;
typedef struct { uint64_t attachment_id; uint32_t seq; oc_slice data; } oc_upload_chunk;
typedef struct { uint64_t attachment_id; uint32_t acked_through; } oc_upload_ack;
typedef struct { uint64_t attachment_id; } oc_upload_end;
typedef struct { uint64_t attachment_id; uint64_t size; oc_slice sha256; } oc_upload_ok;
typedef struct { uint64_t attachment_id; } oc_download_begin;
typedef struct { uint64_t attachment_id; oc_slice filename; oc_slice mime; uint64_t total_size; oc_slice sha256; } oc_download_info;
typedef struct { uint64_t attachment_id; uint32_t seq; oc_slice data; } oc_download_chunk;
typedef struct { uint64_t attachment_id; } oc_download_end;
typedef struct { uint64_t attachment_id; } oc_transfer_cancel;
typedef struct { uint16_t count; const oc_channel_list_entry *entries; } oc_channel_list;
typedef struct { uint64_t user_id; uint8_t role; uint8_t disabled; oc_slice email; oc_slice display_name; } oc_user_list_entry;
typedef struct { uint16_t count; const oc_user_list_entry *entries; } oc_user_list;
typedef struct { uint64_t user_id; uint8_t role; } oc_set_role;
typedef struct { uint8_t role; } oc_invite_user;
typedef struct { uint64_t user_id; } oc_remove_user;
typedef struct { uint64_t user_id; uint8_t role; uint8_t disabled; } oc_user_updated;
typedef struct { oc_slice token; uint8_t role; uint64_t expires_at; } oc_invite_created;
typedef struct { oc_slice token; oc_slice username; oc_slice password; } oc_redeem_invite;
/* Self-service profile (REQ-020). */
typedef struct { oc_slice name; } oc_set_display_name;
typedef struct { oc_slice old_password; oc_slice new_password; } oc_change_password;
typedef struct { uint64_t user_id; oc_slice display_name; } oc_profile_updated;
typedef struct { oc_slice query; uint16_t limit; } oc_search;
typedef struct { uint64_t message_id; uint64_t channel_id; uint64_t author_id; uint64_t server_time; oc_slice snippet; } oc_search_result_entry;
typedef struct { uint16_t count; const oc_search_result_entry *entries; uint8_t truncated; } oc_search_results;
typedef struct { uint64_t channel_id; uint64_t after_message_id; } oc_cursor;
typedef struct { uint16_t count; const oc_cursor *cursors; } oc_backfill_request;
/* Page backwards: the newest `limit` messages STRICTLY OLDER than
 * `before_message_id`. BACKFILL_REQUEST's cursor only points forward, so
 * scrolling into older history needed its own request rather than a new field
 * on an existing frame. `before_message_id` of 0 means "from the newest". */
typedef struct { uint64_t channel_id; uint64_t before_message_id; uint16_t limit; } oc_history_request;
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
oc_result oc_encode_workspace_info(oc_wbuf *w, uint16_t version, const oc_workspace_info *m);
oc_result oc_encode_logout(oc_wbuf *w, uint16_t version, const oc_logout *m);
oc_result oc_encode_send(oc_wbuf *w, uint16_t version, const oc_send *m);
oc_result oc_encode_send_ack(oc_wbuf *w, uint16_t version, const oc_send_ack *m);
oc_result oc_encode_broadcast(oc_wbuf *w, uint16_t version, const oc_broadcast *m);
oc_result oc_encode_client_ack(oc_wbuf *w, uint16_t version, const oc_client_ack *m);
oc_result oc_encode_read_cursor(oc_wbuf *w, uint16_t version, const oc_read_cursor *m);
oc_result oc_encode_edit(oc_wbuf *w, uint16_t version, const oc_edit *m);
oc_result oc_encode_delete(oc_wbuf *w, uint16_t version, const oc_delete *m);
oc_result oc_encode_msg_edited(oc_wbuf *w, uint16_t version, const oc_msg_edited *m);
oc_result oc_encode_msg_deleted(oc_wbuf *w, uint16_t version, const oc_msg_deleted *m);
oc_result oc_encode_react(oc_wbuf *w, uint16_t version, const oc_react *m);
oc_result oc_encode_reaction_updated(oc_wbuf *w, uint16_t version, const oc_reaction_updated *m);
oc_result oc_encode_list_reactions(oc_wbuf *w, uint16_t version, const oc_list_reactions *m);
oc_result oc_encode_pin(oc_wbuf *w, uint16_t version, const oc_pin *m);
oc_result oc_encode_pin_updated(oc_wbuf *w, uint16_t version, const oc_pin_updated *m);
oc_result oc_encode_list_pins(oc_wbuf *w, uint16_t version, const oc_list_pins *m);
oc_result oc_encode_pinned_msg(oc_wbuf *w, uint16_t version, const oc_pinned_msg *m);
oc_result oc_encode_pins(oc_wbuf *w, uint16_t version, const oc_pins *m);
oc_result oc_encode_list_members(oc_wbuf *w, uint16_t version, const oc_list_members *m);
oc_result oc_encode_member_entry(oc_wbuf *w, uint16_t version, const oc_member_entry *m);
oc_result oc_encode_members(oc_wbuf *w, uint16_t version, const oc_members *m);
oc_result oc_encode_list_files(oc_wbuf *w, uint16_t version, const oc_list_files *m);
oc_result oc_encode_file_entry(oc_wbuf *w, uint16_t version, const oc_file_entry *m);
oc_result oc_encode_files(oc_wbuf *w, uint16_t version, const oc_files *m);
oc_result oc_encode_reactions(oc_wbuf *w, uint16_t version, const oc_reactions *m);
oc_result oc_encode_send_reply(oc_wbuf *w, uint16_t version, const oc_send_reply *m);
oc_result oc_encode_thread_reply(oc_wbuf *w, uint16_t version, const oc_thread_reply *m);
oc_result oc_encode_list_thread(oc_wbuf *w, uint16_t version, const oc_list_thread *m);
oc_result oc_encode_thread(oc_wbuf *w, uint16_t version, const oc_thread *m);
oc_result oc_encode_thread_meta(oc_wbuf *w, uint16_t version, const oc_thread_meta *m);
oc_result oc_encode_create_channel(oc_wbuf *w, uint16_t version, const oc_create_channel *m);
oc_result oc_encode_update_channel(oc_wbuf *w, uint16_t version, const oc_update_channel *m);
oc_result oc_encode_save_item(oc_wbuf *w, uint16_t version, const oc_save_item *m);
oc_result oc_encode_saved_updated(oc_wbuf *w, uint16_t version, const oc_saved_updated *m);
oc_result oc_encode_list_saved(oc_wbuf *w, uint16_t version);
oc_result oc_encode_saved_msg(oc_wbuf *w, uint16_t version, const oc_saved_msg *m);
oc_result oc_encode_saved(oc_wbuf *w, uint16_t version, const oc_saved *m);
oc_result oc_encode_list_activity(oc_wbuf *w, uint16_t version);
oc_result oc_encode_activity_entry(oc_wbuf *w, uint16_t version, const oc_activity_entry *m);
oc_result oc_encode_activity(oc_wbuf *w, uint16_t version, const oc_activity *m);
oc_result oc_encode_channel_info(oc_wbuf *w, uint16_t version, const oc_channel_info *m);
oc_result oc_encode_list_channels(oc_wbuf *w, uint16_t version);
/* Bodyless request for the storage report (REQ-214); owner/admin only. */
oc_result oc_encode_storage_status_req(oc_wbuf *w, uint16_t version);
oc_result oc_encode_channel_list(oc_wbuf *w, uint16_t version, const oc_channel_list *m);
oc_result oc_encode_join_channel(oc_wbuf *w, uint16_t version, const oc_channel_ref *m);
oc_result oc_encode_leave_channel(oc_wbuf *w, uint16_t version, const oc_channel_ref *m);
oc_result oc_encode_invite_to_channel(oc_wbuf *w, uint16_t version, const oc_channel_member_op *m);
oc_result oc_encode_remove_from_channel(oc_wbuf *w, uint16_t version, const oc_channel_member_op *m);
oc_result oc_encode_open_dm(oc_wbuf *w, uint16_t version, const oc_open_dm *m);
oc_result oc_encode_create_webhook(oc_wbuf *w, uint16_t version, const oc_create_webhook *m);
oc_result oc_encode_webhook_info(oc_wbuf *w, uint16_t version, const oc_webhook_info *m);
oc_result oc_encode_list_webhooks(oc_wbuf *w, uint16_t version, const oc_list_webhooks *m);
oc_result oc_encode_webhook_list(oc_wbuf *w, uint16_t version, const oc_webhook_list *m);
oc_result oc_encode_delete_webhook(oc_wbuf *w, uint16_t version, const oc_delete_webhook *m);
oc_result oc_encode_webhook_deleted(oc_wbuf *w, uint16_t version, const oc_webhook_deleted *m);
oc_result oc_encode_set_presence(oc_wbuf *w, uint16_t version, const oc_set_presence *m);
oc_result oc_encode_presence_update(oc_wbuf *w, uint16_t version, const oc_presence_update *m);
oc_result oc_encode_typing(oc_wbuf *w, uint16_t version, const oc_typing *m);
oc_result oc_encode_typing_update(oc_wbuf *w, uint16_t version, const oc_typing_update *m);
oc_result oc_encode_set_notify_pref(oc_wbuf *w, uint16_t version, const oc_set_notify_pref *m);
oc_result oc_encode_set_dnd(oc_wbuf *w, uint16_t version, const oc_set_dnd *m);
oc_result oc_encode_register_device_token(oc_wbuf *w, uint16_t version, const oc_register_device_token *m);
oc_result oc_encode_unregister_device_token(oc_wbuf *w, uint16_t version, oc_slice token);
oc_result oc_encode_device_token_ack(oc_wbuf *w, uint16_t version, const oc_device_token_ack *m);
oc_result oc_encode_list_notify_prefs(oc_wbuf *w, uint16_t version);
oc_result oc_encode_notify_prefs(oc_wbuf *w, uint16_t version, const oc_notify_prefs *m);
oc_result oc_encode_set_client_setting(oc_wbuf *w, uint16_t version, const oc_set_client_setting *m);
oc_result oc_encode_list_client_settings(oc_wbuf *w, uint16_t version, const oc_list_client_settings *m);
oc_result oc_encode_audit_query(oc_wbuf *w, uint16_t ver, const oc_audit_query *m);
oc_result oc_decode_audit_query(oc_rbuf *r, oc_audit_query *m);
oc_result oc_encode_audit_page(oc_wbuf *w, uint16_t ver, const oc_audit_page *m);
/* Decodes into `out` (caller-provided, `cap` entries); slices point into `r`. */
oc_result oc_decode_audit_page(oc_rbuf *r, oc_audit_entry *out, uint16_t cap, uint16_t *n);

oc_result oc_encode_storage_status(oc_wbuf *w, uint16_t ver, const oc_storage_status *m);
oc_result oc_decode_storage_status(oc_rbuf *r, oc_storage_status *m);

oc_result oc_encode_client_settings(oc_wbuf *w, uint16_t version, const oc_client_settings *m);
oc_result oc_encode_call_join(oc_wbuf *w, uint16_t version, const oc_call_join *m);
oc_result oc_encode_call_leave(oc_wbuf *w, uint16_t version, const oc_call_leave *m);
oc_result oc_encode_call_joined(oc_wbuf *w, uint16_t version, const oc_call_joined *m);
oc_result oc_encode_call_roster(oc_wbuf *w, uint16_t version, const oc_call_roster *m);
oc_result oc_encode_upload_begin(oc_wbuf *w, uint16_t version, const oc_upload_begin *m);
oc_result oc_encode_upload_ready(oc_wbuf *w, uint16_t version, const oc_upload_ready *m);
oc_result oc_encode_upload_chunk(oc_wbuf *w, uint16_t version, const oc_upload_chunk *m);
oc_result oc_encode_upload_ack(oc_wbuf *w, uint16_t version, const oc_upload_ack *m);
oc_result oc_encode_upload_end(oc_wbuf *w, uint16_t version, const oc_upload_end *m);
oc_result oc_encode_upload_ok(oc_wbuf *w, uint16_t version, const oc_upload_ok *m);
oc_result oc_encode_download_begin(oc_wbuf *w, uint16_t version, const oc_download_begin *m);
oc_result oc_encode_download_info(oc_wbuf *w, uint16_t version, const oc_download_info *m);
oc_result oc_encode_download_chunk(oc_wbuf *w, uint16_t version, const oc_download_chunk *m);
oc_result oc_encode_download_end(oc_wbuf *w, uint16_t version, const oc_download_end *m);
oc_result oc_encode_transfer_cancel(oc_wbuf *w, uint16_t version, const oc_transfer_cancel *m);
oc_result oc_encode_list_users(oc_wbuf *w, uint16_t version);
oc_result oc_encode_user_list(oc_wbuf *w, uint16_t version, const oc_user_list *m);
oc_result oc_encode_set_role(oc_wbuf *w, uint16_t version, const oc_set_role *m);
oc_result oc_encode_invite_user(oc_wbuf *w, uint16_t version, const oc_invite_user *m);
oc_result oc_encode_remove_user(oc_wbuf *w, uint16_t version, const oc_remove_user *m);
oc_result oc_encode_user_updated(oc_wbuf *w, uint16_t version, const oc_user_updated *m);
oc_result oc_encode_invite_created(oc_wbuf *w, uint16_t version, const oc_invite_created *m);
oc_result oc_encode_redeem_invite(oc_wbuf *w, uint16_t version, const oc_redeem_invite *m);
oc_result oc_encode_set_display_name(oc_wbuf *w, uint16_t version, const oc_set_display_name *m);
oc_result oc_encode_change_password(oc_wbuf *w, uint16_t version, const oc_change_password *m);
oc_result oc_encode_profile_updated(oc_wbuf *w, uint16_t version, const oc_profile_updated *m);
oc_result oc_encode_search(oc_wbuf *w, uint16_t version, const oc_search *m);
oc_result oc_encode_search_results(oc_wbuf *w, uint16_t version, const oc_search_results *m);
oc_result oc_encode_backfill_request(oc_wbuf *w, uint16_t version, const oc_backfill_request *m);
oc_result oc_encode_history_request(oc_wbuf *w, uint16_t version, const oc_history_request *m);
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
oc_result oc_decode_workspace_info(oc_rbuf *p, oc_workspace_info *m);
oc_result oc_decode_logout(oc_rbuf *p, oc_logout *m);
oc_result oc_decode_send(oc_rbuf *p, oc_send *m);
oc_result oc_decode_send_ack(oc_rbuf *p, oc_send_ack *m);
oc_result oc_decode_broadcast(oc_rbuf *p, oc_broadcast *m);
oc_result oc_decode_client_ack(oc_rbuf *p, oc_client_ack *m);
oc_result oc_decode_read_cursor(oc_rbuf *p, oc_read_cursor *m);
oc_result oc_decode_edit(oc_rbuf *p, oc_edit *m);
oc_result oc_decode_delete(oc_rbuf *p, oc_delete *m);
oc_result oc_decode_msg_edited(oc_rbuf *p, oc_msg_edited *m);
oc_result oc_decode_msg_deleted(oc_rbuf *p, oc_msg_deleted *m);
oc_result oc_decode_react(oc_rbuf *p, oc_react *m);
oc_result oc_decode_reaction_updated(oc_rbuf *p, oc_reaction_updated *m);
oc_result oc_decode_list_reactions(oc_rbuf *p, oc_list_reactions *m);
oc_result oc_decode_pin(oc_rbuf *p, oc_pin *m);
oc_result oc_decode_pin_updated(oc_rbuf *p, oc_pin_updated *m);
oc_result oc_decode_list_pins(oc_rbuf *p, oc_list_pins *m);
oc_result oc_decode_pinned_msg(oc_rbuf *p, oc_pinned_msg *m);
oc_result oc_decode_pins(oc_rbuf *p, oc_pins *m);
oc_result oc_decode_list_members(oc_rbuf *p, oc_list_members *m);
oc_result oc_decode_member_entry(oc_rbuf *p, oc_member_entry *m);
oc_result oc_decode_members(oc_rbuf *p, oc_members *m);
oc_result oc_decode_list_files(oc_rbuf *p, oc_list_files *m);
oc_result oc_decode_file_entry(oc_rbuf *p, oc_file_entry *m);
oc_result oc_decode_files(oc_rbuf *p, oc_files *m);
oc_result oc_decode_reactions(oc_rbuf *p, oc_reaction_entry *entries, uint16_t cap, uint16_t *out_count, uint64_t *out_message_id);
oc_result oc_decode_send_reply(oc_rbuf *p, oc_send_reply *m);
oc_result oc_decode_thread_reply(oc_rbuf *p, oc_thread_reply *m);
oc_result oc_decode_list_thread(oc_rbuf *p, oc_list_thread *m);
oc_result oc_decode_thread(oc_rbuf *p, oc_thread *m);
oc_result oc_decode_thread_meta(oc_rbuf *p, oc_thread_meta *m);
oc_result oc_decode_create_channel(oc_rbuf *p, oc_create_channel *m);
oc_result oc_decode_update_channel(oc_rbuf *p, oc_update_channel *m);
oc_result oc_decode_save_item(oc_rbuf *p, oc_save_item *m);
oc_result oc_decode_saved_updated(oc_rbuf *p, oc_saved_updated *m);
oc_result oc_decode_saved_msg(oc_rbuf *p, oc_saved_msg *m);
oc_result oc_decode_saved(oc_rbuf *p, oc_saved *m);
oc_result oc_decode_activity_entry(oc_rbuf *p, oc_activity_entry *m);
oc_result oc_decode_activity(oc_rbuf *p, oc_activity *m);
oc_result oc_decode_channel_info(oc_rbuf *p, oc_channel_info *m);
oc_result oc_decode_list_channels(oc_rbuf *p);
oc_result oc_decode_channel_list(oc_rbuf *p, oc_channel_list_entry *entries, uint16_t cap, uint16_t *out_count);
oc_result oc_decode_join_channel(oc_rbuf *p, oc_channel_ref *m);
oc_result oc_decode_leave_channel(oc_rbuf *p, oc_channel_ref *m);
oc_result oc_decode_invite_to_channel(oc_rbuf *p, oc_channel_member_op *m);
oc_result oc_decode_remove_from_channel(oc_rbuf *p, oc_channel_member_op *m);
oc_result oc_decode_open_dm(oc_rbuf *p, oc_open_dm *m);
oc_result oc_decode_create_webhook(oc_rbuf *p, oc_create_webhook *m);
oc_result oc_decode_webhook_info(oc_rbuf *p, oc_webhook_info *m);
oc_result oc_decode_list_webhooks(oc_rbuf *p, oc_list_webhooks *m);
oc_result oc_decode_webhook_list(oc_rbuf *p, oc_webhook_list_entry *entries, uint16_t cap, uint16_t *out_count);
oc_result oc_decode_delete_webhook(oc_rbuf *p, oc_delete_webhook *m);
oc_result oc_decode_webhook_deleted(oc_rbuf *p, oc_webhook_deleted *m);
oc_result oc_decode_set_presence(oc_rbuf *p, oc_set_presence *m);
oc_result oc_decode_presence_update(oc_rbuf *p, oc_presence_update *m);
oc_result oc_decode_typing(oc_rbuf *p, oc_typing *m);
oc_result oc_decode_typing_update(oc_rbuf *p, oc_typing_update *m);
oc_result oc_decode_set_notify_pref(oc_rbuf *p, oc_set_notify_pref *m);
oc_result oc_decode_set_dnd(oc_rbuf *p, oc_set_dnd *m);
oc_result oc_decode_register_device_token(oc_rbuf *p, oc_register_device_token *m);
oc_result oc_decode_unregister_device_token(oc_rbuf *p, oc_slice *token);
oc_result oc_decode_device_token_ack(oc_rbuf *p, oc_device_token_ack *m);
oc_result oc_decode_list_notify_prefs(oc_rbuf *p);
oc_result oc_decode_notify_prefs(oc_rbuf *p, oc_notify_pref_entry *entries, uint16_t cap,
                                 uint16_t *out_count, oc_set_dnd *dnd_out);
oc_result oc_decode_set_client_setting(oc_rbuf *p, oc_set_client_setting *m);
oc_result oc_decode_list_client_settings(oc_rbuf *p, oc_list_client_settings *m);
/* CLIENT_SETTINGS decodes the entries into a caller buffer; client_type stays a
 * view into the frame. */
oc_result oc_decode_client_settings(oc_rbuf *p, oc_client_settings *m,
                                    oc_client_setting_entry *entries, uint16_t cap);
oc_result oc_decode_call_join(oc_rbuf *p, oc_call_join *m);
oc_result oc_decode_call_leave(oc_rbuf *p, oc_call_leave *m);
/* CALL_JOINED/CALL_ROSTER decode the participant ids into a caller buffer. */
oc_result oc_decode_call_joined(oc_rbuf *p, oc_call_joined *m, uint64_t *parts, uint16_t cap);
oc_result oc_decode_call_roster(oc_rbuf *p, oc_call_roster *m, uint64_t *parts, uint16_t cap);
oc_result oc_decode_upload_begin(oc_rbuf *p, oc_upload_begin *m);
oc_result oc_decode_upload_ready(oc_rbuf *p, oc_upload_ready *m);
oc_result oc_decode_upload_chunk(oc_rbuf *p, oc_upload_chunk *m);
oc_result oc_decode_upload_ack(oc_rbuf *p, oc_upload_ack *m);
oc_result oc_decode_upload_end(oc_rbuf *p, oc_upload_end *m);
oc_result oc_decode_upload_ok(oc_rbuf *p, oc_upload_ok *m);
oc_result oc_decode_download_begin(oc_rbuf *p, oc_download_begin *m);
oc_result oc_decode_download_info(oc_rbuf *p, oc_download_info *m);
oc_result oc_decode_download_chunk(oc_rbuf *p, oc_download_chunk *m);
oc_result oc_decode_download_end(oc_rbuf *p, oc_download_end *m);
oc_result oc_decode_transfer_cancel(oc_rbuf *p, oc_transfer_cancel *m);
oc_result oc_decode_list_users(oc_rbuf *p);
oc_result oc_decode_user_list(oc_rbuf *p, oc_user_list_entry *entries, uint16_t cap, uint16_t *out_count);
oc_result oc_decode_set_role(oc_rbuf *p, oc_set_role *m);
oc_result oc_decode_invite_user(oc_rbuf *p, oc_invite_user *m);
oc_result oc_decode_remove_user(oc_rbuf *p, oc_remove_user *m);
oc_result oc_decode_user_updated(oc_rbuf *p, oc_user_updated *m);
oc_result oc_decode_invite_created(oc_rbuf *p, oc_invite_created *m);
oc_result oc_decode_redeem_invite(oc_rbuf *p, oc_redeem_invite *m);
oc_result oc_decode_set_display_name(oc_rbuf *p, oc_set_display_name *m);
oc_result oc_decode_change_password(oc_rbuf *p, oc_change_password *m);
oc_result oc_decode_profile_updated(oc_rbuf *p, oc_profile_updated *m);
oc_result oc_decode_search(oc_rbuf *p, oc_search *m);
oc_result oc_decode_search_results(oc_rbuf *p, oc_search_result_entry *entries, uint16_t cap, uint16_t *out_count, uint8_t *out_truncated);
oc_result oc_decode_history_request(oc_rbuf *p, oc_history_request *m);
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
