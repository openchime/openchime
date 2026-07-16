/*
 * OpenChime wire protocol v1 — frame codec implementation.
 * See protocol.h and docs/PROTOCOL.md.
 */

#include "protocol.h"

#include <string.h>

/* --- Buffers ------------------------------------------------------------ */

void oc_wbuf_init(oc_wbuf *w, uint8_t *data, size_t cap) {
    w->data = data;
    w->cap = cap;
    w->len = 0;
    w->overflow = 0;
}

void oc_rbuf_init(oc_rbuf *r, const uint8_t *data, size_t len) {
    r->data = data;
    r->len = len;
    r->pos = 0;
    r->underflow = 0;
}

oc_slice oc_slice_str(const char *s) {
    oc_slice v;
    v.ptr = (const uint8_t *)s;
    v.len = s ? strlen(s) : 0;
    return v;
}

/* Reserve `n` writable bytes, returning a pointer to them or NULL (and marking
 * overflow) if they don't fit. */
static uint8_t *w_reserve(oc_wbuf *w, size_t n) {
    if (w->overflow || n > w->cap - w->len) {
        w->overflow = 1;
        return NULL;
    }
    uint8_t *p = w->data + w->len;
    w->len += n;
    return p;
}

void oc_w_u8(oc_wbuf *w, uint8_t v) {
    uint8_t *p = w_reserve(w, 1);
    if (p) p[0] = v;
}

void oc_w_u16(oc_wbuf *w, uint16_t v) {
    uint8_t *p = w_reserve(w, 2);
    if (p) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
}

void oc_w_u32(oc_wbuf *w, uint32_t v) {
    uint8_t *p = w_reserve(w, 4);
    if (p) {
        p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
        p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
    }
}

void oc_w_u64(oc_wbuf *w, uint64_t v) {
    uint8_t *p = w_reserve(w, 8);
    if (p) {
        p[0] = (uint8_t)(v >> 56); p[1] = (uint8_t)(v >> 48);
        p[2] = (uint8_t)(v >> 40); p[3] = (uint8_t)(v >> 32);
        p[4] = (uint8_t)(v >> 24); p[5] = (uint8_t)(v >> 16);
        p[6] = (uint8_t)(v >> 8);  p[7] = (uint8_t)v;
    }
}

/* Write a length-prefixed byte run. `width` is 2 (str) or 4 (lstr/bytes). A
 * str whose length exceeds the prefix width overflows the frame. */
static void w_lenpref(oc_wbuf *w, oc_slice s, int width) {
    if (width == 2) {
        if (s.len > 0xFFFFu) { w->overflow = 1; return; }
        oc_w_u16(w, (uint16_t)s.len);
    } else {
        if (s.len > 0xFFFFFFFFu) { w->overflow = 1; return; }
        oc_w_u32(w, (uint32_t)s.len);
    }
    uint8_t *p = w_reserve(w, s.len);
    if (p && s.len) memcpy(p, s.ptr, s.len);
}

void oc_w_str(oc_wbuf *w, oc_slice s)   { w_lenpref(w, s, 2); }
void oc_w_lstr(oc_wbuf *w, oc_slice s)  { w_lenpref(w, s, 4); }
void oc_w_bytes(oc_wbuf *w, oc_slice s) { w_lenpref(w, s, 4); }

void oc_w_idem(oc_wbuf *w, const uint8_t idem[OC_IDEM_SIZE]) {
    uint8_t *p = w_reserve(w, OC_IDEM_SIZE);
    if (p) memcpy(p, idem, OC_IDEM_SIZE);
}

/* Consume `n` bytes, returning a pointer to them or NULL (marking underflow). */
static const uint8_t *r_take(oc_rbuf *r, size_t n) {
    if (r->underflow || n > r->len - r->pos) {
        r->underflow = 1;
        return NULL;
    }
    const uint8_t *p = r->data + r->pos;
    r->pos += n;
    return p;
}

uint8_t oc_r_u8(oc_rbuf *r) {
    const uint8_t *p = r_take(r, 1);
    return p ? p[0] : 0;
}

uint16_t oc_r_u16(oc_rbuf *r) {
    const uint8_t *p = r_take(r, 2);
    return p ? (uint16_t)(((uint16_t)p[0] << 8) | p[1]) : 0;
}

uint32_t oc_r_u32(oc_rbuf *r) {
    const uint8_t *p = r_take(r, 4);
    if (!p) return 0;
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
}

uint64_t oc_r_u64(oc_rbuf *r) {
    const uint8_t *p = r_take(r, 8);
    if (!p) return 0;
    return ((uint64_t)p[0] << 56) | ((uint64_t)p[1] << 48) |
           ((uint64_t)p[2] << 40) | ((uint64_t)p[3] << 32) |
           ((uint64_t)p[4] << 24) | ((uint64_t)p[5] << 16) |
           ((uint64_t)p[6] << 8)  | (uint64_t)p[7];
}

static oc_slice r_lenpref(oc_rbuf *r, int width) {
    oc_slice s = {NULL, 0};
    size_t n = (width == 2) ? oc_r_u16(r) : oc_r_u32(r);
    const uint8_t *p = r_take(r, n);
    if (p) { s.ptr = p; s.len = n; }
    return s;
}

oc_slice oc_r_str(oc_rbuf *r)   { return r_lenpref(r, 2); }
oc_slice oc_r_lstr(oc_rbuf *r)  { return r_lenpref(r, 4); }
oc_slice oc_r_bytes(oc_rbuf *r) { return r_lenpref(r, 4); }

void oc_r_idem(oc_rbuf *r, uint8_t idem[OC_IDEM_SIZE]) {
    const uint8_t *p = r_take(r, OC_IDEM_SIZE);
    if (p) memcpy(idem, p, OC_IDEM_SIZE);
    else   memset(idem, 0, OC_IDEM_SIZE);
}

/* --- Frame framing (PROTOCOL.md §2) ------------------------------------- */

/* Start a frame: reserve a placeholder length, then write version + msg_type.
 * Returns the byte offset of the length field for oc_frame_end to backpatch. */
static size_t oc_frame_begin(oc_wbuf *w, uint16_t version, uint16_t msg_type) {
    size_t off = w->len;
    oc_w_u32(w, 0); /* placeholder length */
    oc_w_u16(w, version);
    oc_w_u16(w, msg_type);
    return off;
}

/* Finish a frame: backpatch `length` = bytes after the length field, and
 * validate the total frame size against the wire limit. */
static oc_result oc_frame_end(oc_wbuf *w, size_t length_off) {
    if (w->overflow) return OC_E_OVERFLOW;
    size_t total = w->len - length_off;         /* 4 + version + msg_type + payload */
    size_t length = total - 4;                  /* value the length field carries */
    if (total > OC_MAX_FRAME_SIZE) return OC_E_TOO_LARGE;
    uint8_t *p = w->data + length_off;
    p[0] = (uint8_t)(length >> 24); p[1] = (uint8_t)(length >> 16);
    p[2] = (uint8_t)(length >> 8);  p[3] = (uint8_t)length;
    return OC_OK;
}

oc_result oc_parse_frame(const uint8_t *data, size_t len,
                         oc_header *hdr, oc_rbuf *payload) {
    if (len < OC_HEADER_SIZE) return OC_E_MALFORMED;
    uint32_t length = ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
                      ((uint32_t)data[2] << 8)  | (uint32_t)data[3];
    if (length < OC_LENGTH_MIN) return OC_E_MALFORMED;
    if ((uint64_t)length + 4u > OC_MAX_FRAME_SIZE) return OC_E_TOO_LARGE;
    if (len < (size_t)length + 4u) return OC_E_MALFORMED; /* incomplete frame */

    hdr->length = length;
    hdr->version  = (uint16_t)(((uint16_t)data[4] << 8) | data[5]);
    hdr->msg_type = (uint16_t)(((uint16_t)data[6] << 8) | data[7]);
    oc_rbuf_init(payload, data + OC_HEADER_SIZE, (size_t)length - 4u);
    return OC_OK;
}

/* A decode is well-formed only if it consumed the payload exactly with no
 * underflow — trailing bytes mean the frame doesn't match this v1 layout. */
static oc_result r_done(const oc_rbuf *r) {
    if (r->underflow) return OC_E_MALFORMED;
    if (r->pos != r->len) return OC_E_MALFORMED;
    return OC_OK;
}

/* --- Version negotiation (PROTOCOL.md §3.2) ----------------------------- */

oc_result oc_negotiate_version(uint16_t client_min, uint16_t client_max,
                               uint16_t server_min, uint16_t server_max,
                               uint16_t *chosen, uint16_t *reject_code) {
    if (client_max < server_min) { *reject_code = OC_ERR_VERSION_TOO_OLD; return OC_E_MALFORMED; }
    if (client_min > server_max) { *reject_code = OC_ERR_VERSION_TOO_NEW; return OC_E_MALFORMED; }
    *chosen = (server_max < client_max) ? server_max : client_max;
    return OC_OK;
}

/* --- Encoders ----------------------------------------------------------- */

/* Reject a body that exceeds the message-body cap before doing any writing. */
#define OC_CHECK_BODY(slice) do { if ((slice).len > OC_MAX_BODY_SIZE) return OC_E_BODY_TOO_LARGE; } while (0)

oc_result oc_encode_hello(oc_wbuf *w, const oc_hello *m) {
    size_t off = oc_frame_begin(w, OC_PROTOCOL_VERSION, OC_MSG_HELLO);
    oc_w_u16(w, m->min_version);
    oc_w_u16(w, m->max_version);
    oc_w_str(w, m->client_info);
    return oc_frame_end(w, off);
}

oc_result oc_encode_welcome(oc_wbuf *w, const oc_welcome *m) {
    size_t off = oc_frame_begin(w, OC_PROTOCOL_VERSION, OC_MSG_WELCOME);
    oc_w_u16(w, m->chosen_version);
    oc_w_u64(w, m->server_time);
    return oc_frame_end(w, off);
}

oc_result oc_encode_reject(oc_wbuf *w, const oc_reject *m) {
    size_t off = oc_frame_begin(w, OC_PROTOCOL_VERSION, OC_MSG_REJECT);
    oc_w_u16(w, m->code);
    oc_w_str(w, m->message);
    return oc_frame_end(w, off);
}

oc_result oc_encode_auth_challenge(oc_wbuf *w, uint16_t version, const oc_auth_challenge *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_AUTH_CHALLENGE);
    oc_w_u8(w, m->methods);
    oc_w_str(w, m->oidc_params);
    return oc_frame_end(w, off);
}

oc_result oc_encode_auth(oc_wbuf *w, uint16_t version, const oc_auth *m) {
    OC_CHECK_BODY(m->credential);
    size_t off = oc_frame_begin(w, version, OC_MSG_AUTH);
    oc_w_u8(w, m->method);
    oc_w_lstr(w, m->credential);
    return oc_frame_end(w, off);
}

oc_result oc_encode_auth_ok(oc_wbuf *w, uint16_t version, const oc_auth_ok *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_AUTH_OK);
    oc_w_u64(w, m->user_id);
    oc_w_u8(w, m->role);
    oc_w_u64(w, m->session_expiry);
    oc_w_bytes(w, m->session_token);
    return oc_frame_end(w, off);
}

oc_result oc_encode_logout(oc_wbuf *w, uint16_t version, const oc_logout *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LOGOUT);
    oc_w_u8(w, m->scope);
    oc_w_bytes(w, m->session_token);
    return oc_frame_end(w, off);
}

oc_result oc_encode_send(oc_wbuf *w, uint16_t version, const oc_send *m) {
    OC_CHECK_BODY(m->body);
    size_t off = oc_frame_begin(w, version, OC_MSG_SEND);
    oc_w_u64(w, m->channel_id);
    oc_w_idem(w, m->idem);
    oc_w_lstr(w, m->body);
    /* Optional trailing attachment-id list (REQ-140): written only when present,
     * so a message with no attachments matches the original layout exactly. */
    if (m->n_attach) {
        uint16_t n = m->n_attach > OC_MAX_ATTACH ? OC_MAX_ATTACH : m->n_attach;
        oc_w_u16(w, n);
        for (uint16_t i = 0; i < n; i++) oc_w_u64(w, m->attach_ids[i]);
    }
    return oc_frame_end(w, off);
}

oc_result oc_encode_send_ack(oc_wbuf *w, uint16_t version, const oc_send_ack *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SEND_ACK);
    oc_w_idem(w, m->idem);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->message_id);
    oc_w_u64(w, m->server_time);
    return oc_frame_end(w, off);
}

oc_result oc_encode_broadcast(oc_wbuf *w, uint16_t version, const oc_broadcast *m) {
    OC_CHECK_BODY(m->body);
    size_t off = oc_frame_begin(w, version, OC_MSG_BROADCAST);
    oc_w_u64(w, m->message_id);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->author_id);
    oc_w_u64(w, m->server_time);
    oc_w_lstr(w, m->body);
    /* Optional trailing block (REQ-140/170): the attachment list, then an
     * optional author-name override. Both are self-describing — present only
     * when non-empty — but because two optional fields share the tail, an
     * author name with no attachments still writes a zero attachment count so
     * the decoder knows a name follows. Absent entirely -> byte-identical to the
     * original layout. */
    if (m->n_attach || m->author_name.len) {
        uint16_t n = m->n_attach > OC_MAX_ATTACH ? OC_MAX_ATTACH : m->n_attach;
        oc_w_u16(w, n);
        for (uint16_t i = 0; i < n; i++) {
            oc_w_u64(w, m->attach[i].id);
            oc_w_str(w, m->attach[i].filename);
            oc_w_str(w, m->attach[i].mime);
            oc_w_u64(w, m->attach[i].size);
        }
        if (m->author_name.len) oc_w_str(w, m->author_name);
    }
    return oc_frame_end(w, off);
}

oc_result oc_encode_client_ack(oc_wbuf *w, uint16_t version, const oc_client_ack *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_CLIENT_ACK);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->message_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_edit(oc_wbuf *w, uint16_t version, const oc_edit *m) {
    OC_CHECK_BODY(m->body);
    size_t off = oc_frame_begin(w, version, OC_MSG_EDIT);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->message_id);
    oc_w_lstr(w, m->body);
    return oc_frame_end(w, off);
}

oc_result oc_encode_delete(oc_wbuf *w, uint16_t version, const oc_delete *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_DELETE);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->message_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_msg_edited(oc_wbuf *w, uint16_t version, const oc_msg_edited *m) {
    OC_CHECK_BODY(m->body);
    size_t off = oc_frame_begin(w, version, OC_MSG_MSG_EDITED);
    oc_w_u64(w, m->message_id);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->author_id);
    oc_w_u64(w, m->edited_at);
    oc_w_lstr(w, m->body);
    return oc_frame_end(w, off);
}

oc_result oc_encode_msg_deleted(oc_wbuf *w, uint16_t version, const oc_msg_deleted *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_MSG_DELETED);
    oc_w_u64(w, m->message_id);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->author_id);
    oc_w_u64(w, m->deleted_by);
    oc_w_u64(w, m->deleted_at);
    return oc_frame_end(w, off);
}

oc_result oc_encode_react(oc_wbuf *w, uint16_t version, const oc_react *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_REACT);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->message_id);
    oc_w_str(w, m->emoji);
    oc_w_u8(w, m->op);
    return oc_frame_end(w, off);
}

oc_result oc_encode_reaction_updated(oc_wbuf *w, uint16_t version, const oc_reaction_updated *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_REACTION_UPDATED);
    oc_w_u64(w, m->message_id);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->user_id);
    oc_w_str(w, m->emoji);
    oc_w_u8(w, m->op);
    oc_w_u64(w, m->count);
    return oc_frame_end(w, off);
}

oc_result oc_encode_list_reactions(oc_wbuf *w, uint16_t version, const oc_list_reactions *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_REACTIONS);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->message_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_reactions(oc_wbuf *w, uint16_t version, const oc_reactions *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_REACTIONS);
    oc_w_u64(w, m->message_id);
    oc_w_u16(w, m->count);
    for (uint16_t i = 0; i < m->count; i++) {
        oc_w_str(w, m->entries[i].emoji);
        oc_w_u64(w, m->entries[i].user_id);
    }
    return oc_frame_end(w, off);
}

oc_result oc_encode_send_reply(oc_wbuf *w, uint16_t version, const oc_send_reply *m) {
    OC_CHECK_BODY(m->body);
    size_t off = oc_frame_begin(w, version, OC_MSG_SEND_REPLY);
    oc_w_u64(w, m->channel_id);
    oc_w_idem(w, m->idem);
    oc_w_u64(w, m->parent_id);
    oc_w_lstr(w, m->body);
    /* Optional trailing attachment-id list (REQ-140), as on SEND. */
    if (m->n_attach) {
        uint16_t n = m->n_attach > OC_MAX_ATTACH ? OC_MAX_ATTACH : m->n_attach;
        oc_w_u16(w, n);
        for (uint16_t i = 0; i < n; i++) oc_w_u64(w, m->attach_ids[i]);
    }
    return oc_frame_end(w, off);
}

oc_result oc_encode_thread_reply(oc_wbuf *w, uint16_t version, const oc_thread_reply *m) {
    OC_CHECK_BODY(m->body);
    size_t off = oc_frame_begin(w, version, OC_MSG_THREAD_REPLY);
    oc_w_u64(w, m->message_id);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->parent_id);
    oc_w_u64(w, m->author_id);
    oc_w_u64(w, m->server_time);
    oc_w_u32(w, m->reply_count);
    oc_w_lstr(w, m->body);
    /* Optional trailing attachment metadata (REQ-140), as on BROADCAST. */
    if (m->n_attach) {
        uint16_t n = m->n_attach > OC_MAX_ATTACH ? OC_MAX_ATTACH : m->n_attach;
        oc_w_u16(w, n);
        for (uint16_t i = 0; i < n; i++) {
            oc_w_u64(w, m->attach[i].id);
            oc_w_str(w, m->attach[i].filename);
            oc_w_str(w, m->attach[i].mime);
            oc_w_u64(w, m->attach[i].size);
        }
    }
    return oc_frame_end(w, off);
}

oc_result oc_encode_list_thread(oc_wbuf *w, uint16_t version, const oc_list_thread *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_THREAD);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->parent_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_thread(oc_wbuf *w, uint16_t version, const oc_thread *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_THREAD);
    oc_w_u64(w, m->parent_id);
    oc_w_u32(w, m->count);
    oc_w_u8(w, m->truncated);   /* 1 if replies were capped */
    return oc_frame_end(w, off);
}

oc_result oc_encode_thread_meta(oc_wbuf *w, uint16_t version, const oc_thread_meta *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_THREAD_META);
    oc_w_u64(w, m->message_id);
    oc_w_u32(w, m->reply_count);
    oc_w_u64(w, m->last_reply_at);
    return oc_frame_end(w, off);
}

oc_result oc_encode_create_channel(oc_wbuf *w, uint16_t version, const oc_create_channel *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_CREATE_CHANNEL);
    oc_w_str(w, m->name);
    oc_w_u8(w, m->is_public);
    return oc_frame_end(w, off);
}

oc_result oc_encode_channel_info(oc_wbuf *w, uint16_t version, const oc_channel_info *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_CHANNEL_INFO);
    oc_w_u64(w, m->channel_id);
    oc_w_u8(w, m->kind);
    oc_w_str(w, m->name);
    oc_w_u8(w, m->is_public);
    oc_w_u8(w, m->joined);
    oc_w_u64(w, m->created_at);
    return oc_frame_end(w, off);
}

oc_result oc_encode_list_channels(oc_wbuf *w, uint16_t version) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_CHANNELS);
    return oc_frame_end(w, off);
}

oc_result oc_encode_channel_list(oc_wbuf *w, uint16_t version, const oc_channel_list *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_CHANNEL_LIST);
    oc_w_u16(w, m->count);
    for (uint16_t i = 0; i < m->count; i++) {
        oc_w_u64(w, m->entries[i].channel_id);
        oc_w_str(w, m->entries[i].name);
        oc_w_u8(w, m->entries[i].is_public);
        oc_w_u8(w, m->entries[i].joined);
        oc_w_u8(w, m->entries[i].kind);
    }
    return oc_frame_end(w, off);
}

oc_result oc_encode_open_dm(oc_wbuf *w, uint16_t version, const oc_open_dm *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_OPEN_DM);
    oc_w_u64(w, m->user_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_create_webhook(oc_wbuf *w, uint16_t version, const oc_create_webhook *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_CREATE_WEBHOOK);
    oc_w_u64(w, m->channel_id);
    oc_w_str(w, m->label);
    return oc_frame_end(w, off);
}

oc_result oc_encode_webhook_info(oc_wbuf *w, uint16_t version, const oc_webhook_info *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_WEBHOOK_INFO);
    oc_w_u64(w, m->webhook_id);
    oc_w_u64(w, m->channel_id);
    oc_w_bytes(w, m->token);
    return oc_frame_end(w, off);
}

oc_result oc_encode_list_webhooks(oc_wbuf *w, uint16_t version, const oc_list_webhooks *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_WEBHOOKS);
    oc_w_u64(w, m->channel_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_webhook_list(oc_wbuf *w, uint16_t version, const oc_webhook_list *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_WEBHOOK_LIST);
    oc_w_u16(w, m->count);
    for (uint16_t i = 0; i < m->count; i++) {
        oc_w_u64(w, m->entries[i].webhook_id);
        oc_w_u64(w, m->entries[i].channel_id);
        oc_w_str(w, m->entries[i].label);
        oc_w_u8(w, m->entries[i].disabled);
    }
    return oc_frame_end(w, off);
}

oc_result oc_encode_delete_webhook(oc_wbuf *w, uint16_t version, const oc_delete_webhook *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_DELETE_WEBHOOK);
    oc_w_u64(w, m->webhook_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_webhook_deleted(oc_wbuf *w, uint16_t version, const oc_webhook_deleted *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_WEBHOOK_DELETED);
    oc_w_u64(w, m->webhook_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_set_presence(oc_wbuf *w, uint16_t version, const oc_set_presence *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SET_PRESENCE);
    oc_w_u8(w, m->status);
    return oc_frame_end(w, off);
}

oc_result oc_encode_presence_update(oc_wbuf *w, uint16_t version, const oc_presence_update *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_PRESENCE_UPDATE);
    oc_w_u64(w, m->user_id);
    oc_w_u8(w, m->status);
    return oc_frame_end(w, off);
}

oc_result oc_encode_typing(oc_wbuf *w, uint16_t version, const oc_typing *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_TYPING);
    oc_w_u64(w, m->channel_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_typing_update(oc_wbuf *w, uint16_t version, const oc_typing_update *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_TYPING_UPDATE);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->user_id);
    return oc_frame_end(w, off);
}

/* --- Attachment transfer (REQ-140/141, ARCH-69) ------------------------- */

oc_result oc_encode_upload_begin(oc_wbuf *w, uint16_t version, const oc_upload_begin *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_UPLOAD_BEGIN);
    oc_w_u64(w, m->channel_id);
    oc_w_idem(w, m->idem);
    oc_w_str(w, m->filename);
    oc_w_str(w, m->mime);
    oc_w_u64(w, m->total_size);
    return oc_frame_end(w, off);
}

oc_result oc_encode_upload_ready(oc_wbuf *w, uint16_t version, const oc_upload_ready *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_UPLOAD_READY);
    oc_w_u64(w, m->attachment_id);
    oc_w_u32(w, m->chunk_size);
    oc_w_u32(w, m->window_bytes);
    return oc_frame_end(w, off);
}

oc_result oc_encode_upload_chunk(oc_wbuf *w, uint16_t version, const oc_upload_chunk *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_UPLOAD_CHUNK);
    oc_w_u64(w, m->attachment_id);
    oc_w_u32(w, m->seq);
    oc_w_bytes(w, m->data);
    return oc_frame_end(w, off);
}

oc_result oc_encode_upload_ack(oc_wbuf *w, uint16_t version, const oc_upload_ack *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_UPLOAD_ACK);
    oc_w_u64(w, m->attachment_id);
    oc_w_u32(w, m->acked_through);
    return oc_frame_end(w, off);
}

oc_result oc_encode_upload_end(oc_wbuf *w, uint16_t version, const oc_upload_end *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_UPLOAD_END);
    oc_w_u64(w, m->attachment_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_upload_ok(oc_wbuf *w, uint16_t version, const oc_upload_ok *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_UPLOAD_OK);
    oc_w_u64(w, m->attachment_id);
    oc_w_u64(w, m->size);
    oc_w_bytes(w, m->sha256);
    return oc_frame_end(w, off);
}

oc_result oc_encode_download_begin(oc_wbuf *w, uint16_t version, const oc_download_begin *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_DOWNLOAD_BEGIN);
    oc_w_u64(w, m->attachment_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_download_info(oc_wbuf *w, uint16_t version, const oc_download_info *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_DOWNLOAD_INFO);
    oc_w_u64(w, m->attachment_id);
    oc_w_str(w, m->filename);
    oc_w_str(w, m->mime);
    oc_w_u64(w, m->total_size);
    oc_w_bytes(w, m->sha256);
    return oc_frame_end(w, off);
}

oc_result oc_encode_download_chunk(oc_wbuf *w, uint16_t version, const oc_download_chunk *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_DOWNLOAD_CHUNK);
    oc_w_u64(w, m->attachment_id);
    oc_w_u32(w, m->seq);
    oc_w_bytes(w, m->data);
    return oc_frame_end(w, off);
}

oc_result oc_encode_download_end(oc_wbuf *w, uint16_t version, const oc_download_end *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_DOWNLOAD_END);
    oc_w_u64(w, m->attachment_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_transfer_cancel(oc_wbuf *w, uint16_t version, const oc_transfer_cancel *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_TRANSFER_CANCEL);
    oc_w_u64(w, m->attachment_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_join_channel(oc_wbuf *w, uint16_t version, const oc_channel_ref *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_JOIN_CHANNEL);
    oc_w_u64(w, m->channel_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_leave_channel(oc_wbuf *w, uint16_t version, const oc_channel_ref *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LEAVE_CHANNEL);
    oc_w_u64(w, m->channel_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_invite_to_channel(oc_wbuf *w, uint16_t version, const oc_channel_member_op *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_INVITE_TO_CHANNEL);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->user_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_remove_from_channel(oc_wbuf *w, uint16_t version, const oc_channel_member_op *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_REMOVE_FROM_CHANNEL);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->user_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_list_users(oc_wbuf *w, uint16_t version) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_USERS);
    return oc_frame_end(w, off);
}

oc_result oc_encode_user_list(oc_wbuf *w, uint16_t version, const oc_user_list *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_USER_LIST);
    oc_w_u16(w, m->count);
    for (uint16_t i = 0; i < m->count; i++) {
        oc_w_u64(w, m->entries[i].user_id);
        oc_w_u8(w, m->entries[i].role);
        oc_w_u8(w, m->entries[i].disabled);
        oc_w_str(w, m->entries[i].email);
        oc_w_str(w, m->entries[i].display_name);
    }
    return oc_frame_end(w, off);
}

oc_result oc_encode_set_role(oc_wbuf *w, uint16_t version, const oc_set_role *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SET_ROLE);
    oc_w_u64(w, m->user_id);
    oc_w_u8(w, m->role);
    return oc_frame_end(w, off);
}

oc_result oc_encode_invite_user(oc_wbuf *w, uint16_t version, const oc_invite_user *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_INVITE_USER);
    oc_w_u8(w, m->role);
    return oc_frame_end(w, off);
}

oc_result oc_encode_remove_user(oc_wbuf *w, uint16_t version, const oc_remove_user *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_REMOVE_USER);
    oc_w_u64(w, m->user_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_user_updated(oc_wbuf *w, uint16_t version, const oc_user_updated *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_USER_UPDATED);
    oc_w_u64(w, m->user_id);
    oc_w_u8(w, m->role);
    oc_w_u8(w, m->disabled);
    return oc_frame_end(w, off);
}

oc_result oc_encode_invite_created(oc_wbuf *w, uint16_t version, const oc_invite_created *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_INVITE_CREATED);
    oc_w_bytes(w, m->token);
    oc_w_u8(w, m->role);
    oc_w_u64(w, m->expires_at);
    return oc_frame_end(w, off);
}

oc_result oc_encode_redeem_invite(oc_wbuf *w, uint16_t version, const oc_redeem_invite *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_REDEEM_INVITE);
    oc_w_bytes(w, m->token);
    oc_w_str(w, m->username);
    oc_w_str(w, m->password);
    return oc_frame_end(w, off);
}

oc_result oc_encode_search(oc_wbuf *w, uint16_t version, const oc_search *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SEARCH);
    oc_w_str(w, m->query);
    oc_w_u16(w, m->limit);
    return oc_frame_end(w, off);
}

oc_result oc_encode_search_results(oc_wbuf *w, uint16_t version, const oc_search_results *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SEARCH_RESULTS);
    oc_w_u16(w, m->count);
    for (uint16_t i = 0; i < m->count; i++) {
        oc_w_u64(w, m->entries[i].message_id);
        oc_w_u64(w, m->entries[i].channel_id);
        oc_w_u64(w, m->entries[i].author_id);
        oc_w_u64(w, m->entries[i].server_time);
        oc_w_str(w, m->entries[i].snippet);
    }
    oc_w_u8(w, m->truncated);   /* 1 if more matches exist past the cap */
    return oc_frame_end(w, off);
}

oc_result oc_encode_backfill_request(oc_wbuf *w, uint16_t version, const oc_backfill_request *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_BACKFILL_REQUEST);
    oc_w_u16(w, m->count);
    for (uint16_t i = 0; i < m->count; i++) {
        oc_w_u64(w, m->cursors[i].channel_id);
        oc_w_u64(w, m->cursors[i].after_message_id);
    }
    return oc_frame_end(w, off);
}

oc_result oc_encode_backfill_done(oc_wbuf *w, uint16_t version, const oc_backfill_done *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_BACKFILL_DONE);
    oc_w_u64(w, m->high_water);
    oc_w_u8(w, m->more);   /* 1 if the replay hit the per-response cap */
    return oc_frame_end(w, off);
}

oc_result oc_encode_error(oc_wbuf *w, uint16_t version, const oc_error *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_ERROR);
    oc_w_u16(w, m->code);
    oc_w_u8(w, m->fatal);
    oc_w_bytes(w, m->context);
    oc_w_str(w, m->message);
    return oc_frame_end(w, off);
}

/* --- Decoders ----------------------------------------------------------- */

oc_result oc_decode_hello(oc_rbuf *p, oc_hello *m) {
    m->min_version = oc_r_u16(p);
    m->max_version = oc_r_u16(p);
    m->client_info = oc_r_str(p);
    return r_done(p);
}

oc_result oc_decode_welcome(oc_rbuf *p, oc_welcome *m) {
    m->chosen_version = oc_r_u16(p);
    m->server_time = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_reject(oc_rbuf *p, oc_reject *m) {
    m->code = oc_r_u16(p);
    m->message = oc_r_str(p);
    return r_done(p);
}

oc_result oc_decode_auth_challenge(oc_rbuf *p, oc_auth_challenge *m) {
    m->methods = oc_r_u8(p);
    m->oidc_params = oc_r_str(p);
    return r_done(p);
}

oc_result oc_decode_auth(oc_rbuf *p, oc_auth *m) {
    m->method = oc_r_u8(p);
    m->credential = oc_r_lstr(p);
    return r_done(p);
}

oc_result oc_decode_auth_ok(oc_rbuf *p, oc_auth_ok *m) {
    m->user_id = oc_r_u64(p);
    m->role = oc_r_u8(p);
    m->session_expiry = oc_r_u64(p);
    m->session_token = oc_r_bytes(p);
    return r_done(p);
}

oc_result oc_decode_logout(oc_rbuf *p, oc_logout *m) {
    m->scope = oc_r_u8(p);
    m->session_token = oc_r_bytes(p);
    return r_done(p);
}

oc_result oc_decode_send(oc_rbuf *p, oc_send *m) {
    m->channel_id = oc_r_u64(p);
    oc_r_idem(p, m->idem);
    m->body = oc_r_lstr(p);
    /* Optional trailing attachment-id list (REQ-140): present only if bytes
     * remain after the body. Absent -> zero attachments (original layout). */
    m->n_attach = 0;
    if (!p->underflow && p->pos < p->len) {
        uint16_t n = oc_r_u16(p);
        if (n > OC_MAX_ATTACH) return OC_E_MALFORMED;
        for (uint16_t i = 0; i < n && !p->underflow; i++) m->attach_ids[i] = oc_r_u64(p);
        m->n_attach = n;
    }
    return r_done(p);
}

oc_result oc_decode_send_ack(oc_rbuf *p, oc_send_ack *m) {
    oc_r_idem(p, m->idem);
    m->channel_id = oc_r_u64(p);
    m->message_id = oc_r_u64(p);
    m->server_time = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_broadcast(oc_rbuf *p, oc_broadcast *m) {
    m->message_id = oc_r_u64(p);
    m->channel_id = oc_r_u64(p);
    m->author_id = oc_r_u64(p);
    m->server_time = oc_r_u64(p);
    m->body = oc_r_lstr(p);
    /* Optional trailing block: attachment list, then an optional author name. */
    m->n_attach = 0;
    m->author_name = (oc_slice){ NULL, 0 };
    if (!p->underflow && p->pos < p->len) {
        uint16_t n = oc_r_u16(p);
        if (n > OC_MAX_ATTACH) return OC_E_MALFORMED;
        for (uint16_t i = 0; i < n && !p->underflow; i++) {
            m->attach[i].id = oc_r_u64(p);
            m->attach[i].filename = oc_r_str(p);
            m->attach[i].mime = oc_r_str(p);
            m->attach[i].size = oc_r_u64(p);
        }
        m->n_attach = n;
        if (!p->underflow && p->pos < p->len) m->author_name = oc_r_str(p);
    }
    return r_done(p);
}

oc_result oc_decode_client_ack(oc_rbuf *p, oc_client_ack *m) {
    m->channel_id = oc_r_u64(p);
    m->message_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_edit(oc_rbuf *p, oc_edit *m) {
    m->channel_id = oc_r_u64(p);
    m->message_id = oc_r_u64(p);
    m->body = oc_r_lstr(p);
    return r_done(p);
}

oc_result oc_decode_delete(oc_rbuf *p, oc_delete *m) {
    m->channel_id = oc_r_u64(p);
    m->message_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_msg_edited(oc_rbuf *p, oc_msg_edited *m) {
    m->message_id = oc_r_u64(p);
    m->channel_id = oc_r_u64(p);
    m->author_id = oc_r_u64(p);
    m->edited_at = oc_r_u64(p);
    m->body = oc_r_lstr(p);
    return r_done(p);
}

oc_result oc_decode_msg_deleted(oc_rbuf *p, oc_msg_deleted *m) {
    m->message_id = oc_r_u64(p);
    m->channel_id = oc_r_u64(p);
    m->author_id = oc_r_u64(p);
    m->deleted_by = oc_r_u64(p);
    m->deleted_at = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_react(oc_rbuf *p, oc_react *m) {
    m->channel_id = oc_r_u64(p);
    m->message_id = oc_r_u64(p);
    m->emoji = oc_r_str(p);
    m->op = oc_r_u8(p);
    return r_done(p);
}

oc_result oc_decode_reaction_updated(oc_rbuf *p, oc_reaction_updated *m) {
    m->message_id = oc_r_u64(p);
    m->channel_id = oc_r_u64(p);
    m->user_id = oc_r_u64(p);
    m->emoji = oc_r_str(p);
    m->op = oc_r_u8(p);
    m->count = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_list_reactions(oc_rbuf *p, oc_list_reactions *m) {
    m->channel_id = oc_r_u64(p);
    m->message_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_reactions(oc_rbuf *p, oc_reaction_entry *entries, uint16_t cap,
                              uint16_t *out_count, uint64_t *out_message_id) {
    *out_message_id = oc_r_u64(p);
    uint16_t count = oc_r_u16(p);
    *out_count = count;
    for (uint16_t i = 0; i < count && !p->underflow; i++) {
        oc_slice emoji = oc_r_str(p);
        uint64_t uid = oc_r_u64(p);
        if (i < cap) { entries[i].emoji = emoji; entries[i].user_id = uid; }
    }
    return r_done(p);
}

oc_result oc_decode_send_reply(oc_rbuf *p, oc_send_reply *m) {
    m->channel_id = oc_r_u64(p);
    oc_r_idem(p, m->idem);
    m->parent_id = oc_r_u64(p);
    m->body = oc_r_lstr(p);
    m->n_attach = 0;
    if (!p->underflow && p->pos < p->len) {
        uint16_t n = oc_r_u16(p);
        if (n > OC_MAX_ATTACH) return OC_E_MALFORMED;
        for (uint16_t i = 0; i < n && !p->underflow; i++) m->attach_ids[i] = oc_r_u64(p);
        m->n_attach = n;
    }
    return r_done(p);
}

oc_result oc_decode_thread_reply(oc_rbuf *p, oc_thread_reply *m) {
    m->message_id = oc_r_u64(p);
    m->channel_id = oc_r_u64(p);
    m->parent_id = oc_r_u64(p);
    m->author_id = oc_r_u64(p);
    m->server_time = oc_r_u64(p);
    m->reply_count = oc_r_u32(p);
    m->body = oc_r_lstr(p);
    m->n_attach = 0;
    if (!p->underflow && p->pos < p->len) {
        uint16_t n = oc_r_u16(p);
        if (n > OC_MAX_ATTACH) return OC_E_MALFORMED;
        for (uint16_t i = 0; i < n && !p->underflow; i++) {
            m->attach[i].id = oc_r_u64(p);
            m->attach[i].filename = oc_r_str(p);
            m->attach[i].mime = oc_r_str(p);
            m->attach[i].size = oc_r_u64(p);
        }
        m->n_attach = n;
    }
    return r_done(p);
}

oc_result oc_decode_list_thread(oc_rbuf *p, oc_list_thread *m) {
    m->channel_id = oc_r_u64(p);
    m->parent_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_thread(oc_rbuf *p, oc_thread *m) {
    m->parent_id = oc_r_u64(p);
    m->count = oc_r_u32(p);
    m->truncated = oc_r_u8(p);
    return r_done(p);
}

oc_result oc_decode_thread_meta(oc_rbuf *p, oc_thread_meta *m) {
    m->message_id = oc_r_u64(p);
    m->reply_count = oc_r_u32(p);
    m->last_reply_at = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_create_channel(oc_rbuf *p, oc_create_channel *m) {
    m->name = oc_r_str(p);
    m->is_public = oc_r_u8(p);
    return r_done(p);
}

oc_result oc_decode_channel_info(oc_rbuf *p, oc_channel_info *m) {
    m->channel_id = oc_r_u64(p);
    m->kind = oc_r_u8(p);
    m->name = oc_r_str(p);
    m->is_public = oc_r_u8(p);
    m->joined = oc_r_u8(p);
    m->created_at = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_list_channels(oc_rbuf *p) {
    return r_done(p);   /* empty payload */
}

oc_result oc_decode_channel_list(oc_rbuf *p, oc_channel_list_entry *entries,
                                 uint16_t cap, uint16_t *out_count) {
    uint16_t count = oc_r_u16(p);
    *out_count = count;
    for (uint16_t i = 0; i < count && !p->underflow; i++) {
        uint64_t id = oc_r_u64(p);
        oc_slice name = oc_r_str(p);
        uint8_t is_public = oc_r_u8(p);
        uint8_t joined = oc_r_u8(p);
        uint8_t kind = oc_r_u8(p);
        if (i < cap) {
            entries[i].channel_id = id;
            entries[i].name = name;
            entries[i].is_public = is_public;
            entries[i].joined = joined;
            entries[i].kind = kind;
        }
    }
    return r_done(p);
}

oc_result oc_decode_open_dm(oc_rbuf *p, oc_open_dm *m) {
    m->user_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_create_webhook(oc_rbuf *p, oc_create_webhook *m) {
    m->channel_id = oc_r_u64(p);
    m->label = oc_r_str(p);
    return r_done(p);
}

oc_result oc_decode_webhook_info(oc_rbuf *p, oc_webhook_info *m) {
    m->webhook_id = oc_r_u64(p);
    m->channel_id = oc_r_u64(p);
    m->token = oc_r_bytes(p);
    return r_done(p);
}

oc_result oc_decode_list_webhooks(oc_rbuf *p, oc_list_webhooks *m) {
    m->channel_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_webhook_list(oc_rbuf *p, oc_webhook_list_entry *entries,
                                 uint16_t cap, uint16_t *out_count) {
    uint16_t count = oc_r_u16(p);
    *out_count = count;
    for (uint16_t i = 0; i < count && !p->underflow; i++) {
        uint64_t wid = oc_r_u64(p);
        uint64_t cid = oc_r_u64(p);
        oc_slice label = oc_r_str(p);
        uint8_t disabled = oc_r_u8(p);
        if (i < cap) {
            entries[i].webhook_id = wid;
            entries[i].channel_id = cid;
            entries[i].label = label;
            entries[i].disabled = disabled;
        }
    }
    return r_done(p);
}

oc_result oc_decode_delete_webhook(oc_rbuf *p, oc_delete_webhook *m) {
    m->webhook_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_webhook_deleted(oc_rbuf *p, oc_webhook_deleted *m) {
    m->webhook_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_set_presence(oc_rbuf *p, oc_set_presence *m) {
    m->status = oc_r_u8(p);
    return r_done(p);
}

oc_result oc_decode_presence_update(oc_rbuf *p, oc_presence_update *m) {
    m->user_id = oc_r_u64(p);
    m->status = oc_r_u8(p);
    return r_done(p);
}

oc_result oc_decode_typing(oc_rbuf *p, oc_typing *m) {
    m->channel_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_typing_update(oc_rbuf *p, oc_typing_update *m) {
    m->channel_id = oc_r_u64(p);
    m->user_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_upload_begin(oc_rbuf *p, oc_upload_begin *m) {
    m->channel_id = oc_r_u64(p);
    oc_r_idem(p, m->idem);
    m->filename = oc_r_str(p);
    m->mime = oc_r_str(p);
    m->total_size = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_upload_ready(oc_rbuf *p, oc_upload_ready *m) {
    m->attachment_id = oc_r_u64(p);
    m->chunk_size = oc_r_u32(p);
    m->window_bytes = oc_r_u32(p);
    return r_done(p);
}

oc_result oc_decode_upload_chunk(oc_rbuf *p, oc_upload_chunk *m) {
    m->attachment_id = oc_r_u64(p);
    m->seq = oc_r_u32(p);
    m->data = oc_r_bytes(p);
    return r_done(p);
}

oc_result oc_decode_upload_ack(oc_rbuf *p, oc_upload_ack *m) {
    m->attachment_id = oc_r_u64(p);
    m->acked_through = oc_r_u32(p);
    return r_done(p);
}

oc_result oc_decode_upload_end(oc_rbuf *p, oc_upload_end *m) {
    m->attachment_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_upload_ok(oc_rbuf *p, oc_upload_ok *m) {
    m->attachment_id = oc_r_u64(p);
    m->size = oc_r_u64(p);
    m->sha256 = oc_r_bytes(p);
    return r_done(p);
}

oc_result oc_decode_download_begin(oc_rbuf *p, oc_download_begin *m) {
    m->attachment_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_download_info(oc_rbuf *p, oc_download_info *m) {
    m->attachment_id = oc_r_u64(p);
    m->filename = oc_r_str(p);
    m->mime = oc_r_str(p);
    m->total_size = oc_r_u64(p);
    m->sha256 = oc_r_bytes(p);
    return r_done(p);
}

oc_result oc_decode_download_chunk(oc_rbuf *p, oc_download_chunk *m) {
    m->attachment_id = oc_r_u64(p);
    m->seq = oc_r_u32(p);
    m->data = oc_r_bytes(p);
    return r_done(p);
}

oc_result oc_decode_download_end(oc_rbuf *p, oc_download_end *m) {
    m->attachment_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_transfer_cancel(oc_rbuf *p, oc_transfer_cancel *m) {
    m->attachment_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_join_channel(oc_rbuf *p, oc_channel_ref *m) {
    m->channel_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_leave_channel(oc_rbuf *p, oc_channel_ref *m) {
    m->channel_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_invite_to_channel(oc_rbuf *p, oc_channel_member_op *m) {
    m->channel_id = oc_r_u64(p);
    m->user_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_remove_from_channel(oc_rbuf *p, oc_channel_member_op *m) {
    m->channel_id = oc_r_u64(p);
    m->user_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_list_users(oc_rbuf *p) {
    return r_done(p);   /* empty payload */
}

oc_result oc_decode_user_list(oc_rbuf *p, oc_user_list_entry *entries,
                              uint16_t cap, uint16_t *out_count) {
    uint16_t count = oc_r_u16(p);
    *out_count = count;
    for (uint16_t i = 0; i < count && !p->underflow; i++) {
        uint64_t uid = oc_r_u64(p);
        uint8_t role = oc_r_u8(p);
        uint8_t disabled = oc_r_u8(p);
        oc_slice email = oc_r_str(p);
        oc_slice name = oc_r_str(p);
        if (i < cap) {
            entries[i].user_id = uid;
            entries[i].role = role;
            entries[i].disabled = disabled;
            entries[i].email = email;
            entries[i].display_name = name;
        }
    }
    return r_done(p);
}

oc_result oc_decode_set_role(oc_rbuf *p, oc_set_role *m) {
    m->user_id = oc_r_u64(p);
    m->role = oc_r_u8(p);
    return r_done(p);
}

oc_result oc_decode_invite_user(oc_rbuf *p, oc_invite_user *m) {
    m->role = oc_r_u8(p);
    return r_done(p);
}

oc_result oc_decode_remove_user(oc_rbuf *p, oc_remove_user *m) {
    m->user_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_user_updated(oc_rbuf *p, oc_user_updated *m) {
    m->user_id = oc_r_u64(p);
    m->role = oc_r_u8(p);
    m->disabled = oc_r_u8(p);
    return r_done(p);
}

oc_result oc_decode_invite_created(oc_rbuf *p, oc_invite_created *m) {
    m->token = oc_r_bytes(p);
    m->role = oc_r_u8(p);
    m->expires_at = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_redeem_invite(oc_rbuf *p, oc_redeem_invite *m) {
    m->token = oc_r_bytes(p);
    m->username = oc_r_str(p);
    m->password = oc_r_str(p);
    return r_done(p);
}

oc_result oc_decode_search(oc_rbuf *p, oc_search *m) {
    m->query = oc_r_str(p);
    m->limit = oc_r_u16(p);
    return r_done(p);
}

oc_result oc_decode_search_results(oc_rbuf *p, oc_search_result_entry *entries,
                                   uint16_t cap, uint16_t *out_count, uint8_t *out_truncated) {
    uint16_t count = oc_r_u16(p);
    *out_count = count;
    for (uint16_t i = 0; i < count && !p->underflow; i++) {
        uint64_t mid = oc_r_u64(p);
        uint64_t ch = oc_r_u64(p);
        uint64_t author = oc_r_u64(p);
        uint64_t ts = oc_r_u64(p);
        oc_slice snip = oc_r_str(p);
        if (i < cap) {
            entries[i].message_id = mid;
            entries[i].channel_id = ch;
            entries[i].author_id = author;
            entries[i].server_time = ts;
            entries[i].snippet = snip;
        }
    }
    uint8_t trunc = oc_r_u8(p);
    if (out_truncated) *out_truncated = trunc;
    return r_done(p);
}

oc_result oc_decode_backfill_request(oc_rbuf *p, oc_cursor *cursors,
                                     uint16_t cap, uint16_t *out_count) {
    uint16_t count = oc_r_u16(p);
    *out_count = count;
    for (uint16_t i = 0; i < count && !p->underflow; i++) {
        uint64_t ch = oc_r_u64(p);
        uint64_t after = oc_r_u64(p);
        if (i < cap) { cursors[i].channel_id = ch; cursors[i].after_message_id = after; }
    }
    return r_done(p);
}

oc_result oc_decode_backfill_done(oc_rbuf *p, oc_backfill_done *m) {
    m->high_water = oc_r_u64(p);
    m->more = oc_r_u8(p);
    return r_done(p);
}

oc_result oc_decode_error(oc_rbuf *p, oc_error *m) {
    m->code = oc_r_u16(p);
    m->fatal = oc_r_u8(p);
    m->context = oc_r_bytes(p);
    m->message = oc_r_str(p);
    return r_done(p);
}

/* --- Inner credential codec (AUTH.md §2; PROTOCOL.md §4.2) --------------- */

oc_result oc_encode_local_credential(oc_wbuf *w, oc_slice username, oc_slice password) {
    oc_w_str(w, username);
    oc_w_str(w, password);
    return w->overflow ? OC_E_OVERFLOW : OC_OK;
}

oc_result oc_parse_local_credential(oc_slice credential, oc_slice *username, oc_slice *password) {
    oc_rbuf r;
    oc_rbuf_init(&r, credential.ptr, credential.len);
    oc_slice u = oc_r_str(&r);
    oc_slice p = oc_r_str(&r);
    if (r_done(&r) != OC_OK) return OC_E_MALFORMED;
    if (username) *username = u;
    if (password) *password = p;
    return OC_OK;
}
