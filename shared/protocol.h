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
    OC_MSG_SEND             = 0x0020, /* C->S */
    OC_MSG_SEND_ACK         = 0x0021, /* S->C */
    OC_MSG_BROADCAST        = 0x0022, /* S->C */
    OC_MSG_CLIENT_ACK       = 0x0023, /* C->S */
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

/* --- Frame payload structs ---------------------------------------------- */

typedef struct { uint16_t min_version; uint16_t max_version; oc_slice client_info; } oc_hello;
typedef struct { uint16_t chosen_version; uint64_t server_time; } oc_welcome;
typedef struct { uint16_t code; oc_slice message; } oc_reject;
typedef struct { oc_slice jwt; } oc_auth;
typedef struct { uint64_t user_id; uint64_t session_expiry; } oc_auth_ok;
typedef struct { uint64_t channel_id; uint8_t idem[OC_IDEM_SIZE]; oc_slice body; } oc_send;
typedef struct { uint8_t idem[OC_IDEM_SIZE]; uint64_t channel_id; uint64_t message_id; uint64_t server_time; } oc_send_ack;
typedef struct { uint64_t message_id; uint64_t channel_id; uint64_t author_id; uint64_t server_time; oc_slice body; } oc_broadcast;
typedef struct { uint64_t channel_id; uint64_t message_id; } oc_client_ack;
typedef struct { uint64_t channel_id; uint64_t after_message_id; } oc_cursor;
typedef struct { uint16_t count; const oc_cursor *cursors; } oc_backfill_request;
typedef struct { uint64_t high_water; } oc_backfill_done;
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
oc_result oc_encode_auth(oc_wbuf *w, uint16_t version, const oc_auth *m);
oc_result oc_encode_auth_ok(oc_wbuf *w, uint16_t version, const oc_auth_ok *m);
oc_result oc_encode_send(oc_wbuf *w, uint16_t version, const oc_send *m);
oc_result oc_encode_send_ack(oc_wbuf *w, uint16_t version, const oc_send_ack *m);
oc_result oc_encode_broadcast(oc_wbuf *w, uint16_t version, const oc_broadcast *m);
oc_result oc_encode_client_ack(oc_wbuf *w, uint16_t version, const oc_client_ack *m);
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
oc_result oc_decode_auth(oc_rbuf *p, oc_auth *m);
oc_result oc_decode_auth_ok(oc_rbuf *p, oc_auth_ok *m);
oc_result oc_decode_send(oc_rbuf *p, oc_send *m);
oc_result oc_decode_send_ack(oc_rbuf *p, oc_send_ack *m);
oc_result oc_decode_broadcast(oc_rbuf *p, oc_broadcast *m);
oc_result oc_decode_client_ack(oc_rbuf *p, oc_client_ack *m);
oc_result oc_decode_backfill_request(oc_rbuf *p, oc_cursor *cursors, uint16_t cap, uint16_t *out_count);
oc_result oc_decode_backfill_done(oc_rbuf *p, oc_backfill_done *m);
oc_result oc_decode_error(oc_rbuf *p, oc_error *m);

#endif /* OPENCHIME_PROTOCOL_H */
