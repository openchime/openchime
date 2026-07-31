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

oc_result oc_encode_workspace_info(oc_wbuf *w, uint16_t version, const oc_workspace_info *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_WORKSPACE_INFO);
    oc_w_u8(w, m->deployment_mode);
    oc_w_u32(w, m->max_users);
    oc_w_str(w, m->workspace_name);        /* u16 length + bytes */
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
            oc_w_u8(w, m->attach[i].reclaimed);
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

oc_result oc_encode_read_cursor(oc_wbuf *w, uint16_t version, const oc_read_cursor *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_READ_CURSOR);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->user_id);
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

oc_result oc_encode_pin(oc_wbuf *w, uint16_t version, const oc_pin *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_PIN);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->message_id);
    oc_w_u8(w, m->op);
    return oc_frame_end(w, off);
}

oc_result oc_encode_pin_updated(oc_wbuf *w, uint16_t version, const oc_pin_updated *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_PIN_UPDATED);
    oc_w_u64(w, m->message_id);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->user_id);
    oc_w_u8(w, m->op);
    oc_w_u64(w, m->pinned_at);
    return oc_frame_end(w, off);
}

oc_result oc_encode_list_pins(oc_wbuf *w, uint16_t version, const oc_list_pins *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_PINS);
    oc_w_u64(w, m->channel_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_pinned_msg(oc_wbuf *w, uint16_t version, const oc_pinned_msg *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_PINNED_MSG);
    oc_w_u64(w, m->message_id);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->author_id);
    oc_w_u64(w, m->server_time);
    oc_w_u64(w, m->pinned_by);
    oc_w_u64(w, m->pinned_at);
    oc_w_str(w, m->body);
    oc_w_str(w, m->attach_name);
    return oc_frame_end(w, off);
}

oc_result oc_encode_pins(oc_wbuf *w, uint16_t version, const oc_pins *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_PINS);
    oc_w_u64(w, m->channel_id);
    oc_w_u32(w, m->count);
    return oc_frame_end(w, off);
}

oc_result oc_encode_list_members(oc_wbuf *w, uint16_t version, const oc_list_members *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_MEMBERS);
    oc_w_u64(w, m->channel_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_member_entry(oc_wbuf *w, uint16_t version, const oc_member_entry *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_MEMBER_ENTRY);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->user_id);
    oc_w_u8(w, m->role);
    oc_w_u64(w, m->joined_at);
    return oc_frame_end(w, off);
}

oc_result oc_encode_mention_unresolved(oc_wbuf *w, uint16_t version, const oc_mention_unresolved *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_MENTION_UNRESOLVED);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->message_id);
    oc_w_u8(w, m->can_add);
    oc_w_u8(w, m->is_private);
    uint16_t n = m->count > OC_UNRESOLVED_MAX ? OC_UNRESOLVED_MAX : m->count;
    oc_w_u16(w, n);
    for (uint16_t i = 0; i < n; i++) {
        oc_w_u64(w, m->who[i].user_id);
        oc_w_str(w, oc_slice_str(m->who[i].name));
    }
    return oc_frame_end(w, off);
}

oc_result oc_encode_members(oc_wbuf *w, uint16_t version, const oc_members *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_MEMBERS);
    oc_w_u64(w, m->channel_id);
    oc_w_u32(w, m->count);
    return oc_frame_end(w, off);
}

oc_result oc_encode_list_files(oc_wbuf *w, uint16_t version, const oc_list_files *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_FILES);
    oc_w_u64(w, m->channel_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_file_entry(oc_wbuf *w, uint16_t version, const oc_file_entry *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_FILE_ENTRY);
    oc_w_u64(w, m->attachment_id);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->message_id);
    oc_w_u64(w, m->uploader_id);
    oc_w_u64(w, m->size);
    oc_w_u64(w, m->created_at);
    oc_w_u8(w, m->reclaimed);
    oc_w_str(w, m->filename);
    oc_w_str(w, m->mime);
    return oc_frame_end(w, off);
}

oc_result oc_encode_files(oc_wbuf *w, uint16_t version, const oc_files *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_FILES);
    oc_w_u64(w, m->channel_id);
    oc_w_u32(w, m->count);
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
            oc_w_u8(w, m->attach[i].reclaimed);
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

oc_result oc_encode_update_channel(oc_wbuf *w, uint16_t version, const oc_update_channel *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_UPDATE_CHANNEL);
    oc_w_u64(w, m->channel_id);
    oc_w_u8(w, m->op);
    oc_w_str(w, m->value);
    return oc_frame_end(w, off);
}

oc_result oc_encode_save_item(oc_wbuf *w, uint16_t version, const oc_save_item *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SAVE_ITEM);
    oc_w_u64(w, m->message_id); oc_w_u8(w, m->op);
    return oc_frame_end(w, off);
}
oc_result oc_encode_saved_updated(oc_wbuf *w, uint16_t version, const oc_saved_updated *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SAVED_UPDATED);
    oc_w_u64(w, m->message_id); oc_w_u8(w, m->op); oc_w_u64(w, m->saved_at);
    return oc_frame_end(w, off);
}
oc_result oc_encode_list_saved(oc_wbuf *w, uint16_t version) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_SAVED);
    return oc_frame_end(w, off);
}
oc_result oc_encode_saved_msg(oc_wbuf *w, uint16_t version, const oc_saved_msg *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SAVED_MSG);
    oc_w_u64(w, m->message_id); oc_w_u64(w, m->channel_id); oc_w_u64(w, m->author_id);
    oc_w_u64(w, m->server_time); oc_w_u64(w, m->saved_at);
    oc_w_str(w, m->body); oc_w_str(w, m->attach_name);
    return oc_frame_end(w, off);
}
oc_result oc_encode_saved(oc_wbuf *w, uint16_t version, const oc_saved *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SAVED);
    oc_w_u32(w, m->count);
    return oc_frame_end(w, off);
}
oc_result oc_encode_list_activity(oc_wbuf *w, uint16_t version) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_ACTIVITY);
    return oc_frame_end(w, off);
}
oc_result oc_encode_activity_entry(oc_wbuf *w, uint16_t version, const oc_activity_entry *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_ACTIVITY_ENTRY);
    oc_w_u8(w, m->kind); oc_w_u64(w, m->message_id); oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->actor_id); oc_w_u64(w, m->at); oc_w_str(w, m->text);
    return oc_frame_end(w, off);
}
oc_result oc_encode_activity(oc_wbuf *w, uint16_t version, const oc_activity *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_ACTIVITY);
    oc_w_u32(w, m->count); oc_w_u64(w, m->seen_at);
    return oc_frame_end(w, off);
}

oc_result oc_encode_history_around(oc_wbuf *w, uint16_t version, const oc_history_around *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_HISTORY_AROUND);
    oc_w_u64(w, m->channel_id); oc_w_u64(w, m->message_id); oc_w_u16(w, m->limit);
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
    oc_w_u64(w, m->peer_id);      /* 0 when not a DM; no longer optional (ARCH-93) */
    oc_w_str(w, m->topic);
    oc_w_u8(w, m->archived);
    /* A group DM's participants (REQ-056). Zero for everything else, so the field
     * costs two bytes on frames that do not need it. */
    {
        uint16_t np = m->n_peers > OC_MAX_GROUP_DM + 1 ? (uint16_t)(OC_MAX_GROUP_DM + 1) : m->n_peers;
        oc_w_u16(w, np);
        for (uint16_t i = 0; i < np; i++) oc_w_u64(w, m->peers[i]);
    }
    return oc_frame_end(w, off);
}

oc_result oc_encode_list_channels(oc_wbuf *w, uint16_t version) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_CHANNELS);
    return oc_frame_end(w, off);
}

oc_result oc_encode_storage_status_req(oc_wbuf *w, uint16_t version) {
    size_t off = oc_frame_begin(w, version, OC_MSG_STORAGE_STATUS_REQ);
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
        oc_w_u64(w, m->entries[i].last_message_at);
        oc_w_u32(w, m->entries[i].unread);
        oc_w_u64(w, m->entries[i].peer_id);
        oc_w_str(w, m->entries[i].topic);
        oc_w_u8(w, m->entries[i].archived);
        oc_w_u64(w, m->entries[i].created_at);
        oc_w_str(w, m->entries[i].preview);
        oc_w_u64(w, m->entries[i].preview_author);
        {
            uint16_t np = m->entries[i].n_peers > OC_MAX_GROUP_DM + 1
                        ? (uint16_t)(OC_MAX_GROUP_DM + 1) : m->entries[i].n_peers;
            oc_w_u16(w, np);
            for (uint16_t k = 0; k < np; k++) oc_w_u64(w, m->entries[i].peers[k]);
        }
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

/* --- Notification preferences (REQ-130/131) ----------------------------- */

oc_result oc_encode_set_notify_pref(oc_wbuf *w, uint16_t version, const oc_set_notify_pref *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SET_NOTIFY_PREF);
    oc_w_u64(w, m->channel_id);
    oc_w_u8(w, m->level);
    return oc_frame_end(w, off);
}

oc_result oc_encode_set_dnd(oc_wbuf *w, uint16_t version, const oc_set_dnd *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SET_DND);
    oc_w_u8(w, m->enabled);
    oc_w_u16(w, m->start_min);
    oc_w_u16(w, m->end_min);
    return oc_frame_end(w, off);
}

oc_result oc_encode_list_notify_prefs(oc_wbuf *w, uint16_t version) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_NOTIFY_PREFS);
    return oc_frame_end(w, off);
}

oc_result oc_encode_notify_prefs(oc_wbuf *w, uint16_t version, const oc_notify_prefs *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_NOTIFY_PREFS);
    oc_w_u8(w, m->dnd_enabled);
    oc_w_u16(w, m->dnd_start_min);
    oc_w_u16(w, m->dnd_end_min);
    oc_w_u8(w, m->notify_default);      /* REQ-134 */
    oc_w_u16(w, m->count);
    for (uint16_t i = 0; i < m->count; i++) {
        oc_w_u64(w, m->entries[i].channel_id);
        oc_w_u8(w, m->entries[i].level);
        oc_w_u8(w, m->entries[i].muted);
    }
    return oc_frame_end(w, off);
}

oc_result oc_decode_set_notify_pref(oc_rbuf *p, oc_set_notify_pref *m) {
    m->channel_id = oc_r_u64(p);
    m->level = oc_r_u8(p);
    return r_done(p);
}

oc_result oc_decode_set_dnd(oc_rbuf *p, oc_set_dnd *m) {
    m->enabled = oc_r_u8(p);
    m->start_min = oc_r_u16(p);
    m->end_min = oc_r_u16(p);
    return r_done(p);
}

oc_result oc_encode_register_device_token(oc_wbuf *w, uint16_t version, const oc_register_device_token *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_REGISTER_DEVICE_TOKEN);
    oc_w_u8(w, m->platform);
    oc_w_str(w, m->token);
    return oc_frame_end(w, off);
}

oc_result oc_decode_register_device_token(oc_rbuf *p, oc_register_device_token *m) {
    m->platform = oc_r_u8(p);
    m->token = oc_r_str(p);
    return r_done(p);
}

oc_result oc_encode_unregister_device_token(oc_wbuf *w, uint16_t version, oc_slice token) {
    size_t off = oc_frame_begin(w, version, OC_MSG_UNREGISTER_DEVICE_TOKEN);
    oc_w_str(w, token);
    return oc_frame_end(w, off);
}

oc_result oc_decode_unregister_device_token(oc_rbuf *p, oc_slice *token) {
    *token = oc_r_str(p);
    return r_done(p);
}

oc_result oc_encode_device_token_ack(oc_wbuf *w, uint16_t version, const oc_device_token_ack *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_DEVICE_TOKEN_ACK);
    oc_w_u8(w, m->ok);
    oc_w_u16(w, m->code);
    return oc_frame_end(w, off);
}

oc_result oc_decode_device_token_ack(oc_rbuf *p, oc_device_token_ack *m) {
    m->ok = oc_r_u8(p);
    m->code = oc_r_u16(p);
    return r_done(p);
}

oc_result oc_decode_list_notify_prefs(oc_rbuf *p) {
    return r_done(p);   /* empty payload */
}

oc_result oc_decode_notify_prefs(oc_rbuf *p, oc_notify_pref_entry *entries, uint16_t cap,
                                 uint16_t *out_count, oc_set_dnd *dnd_out,
                                 uint8_t *out_default) {
    dnd_out->enabled = oc_r_u8(p);
    dnd_out->start_min = oc_r_u16(p);
    dnd_out->end_min = oc_r_u16(p);
    uint8_t dflt = oc_r_u8(p);                 /* REQ-134 */
    if (out_default) *out_default = dflt;
    uint16_t count = oc_r_u16(p);
    *out_count = count;
    for (uint16_t i = 0; i < count && !p->underflow; i++) {
        uint64_t cid = oc_r_u64(p);
        uint8_t level = oc_r_u8(p);
        uint8_t muted = oc_r_u8(p);
        if (i < cap) { entries[i].channel_id = cid; entries[i].level = level;
                       entries[i].muted = muted; }
    }
    return r_done(p);
}

/* --- Synced client settings bucket -------------------------------------- */

oc_result oc_encode_set_client_setting(oc_wbuf *w, uint16_t version, const oc_set_client_setting *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SET_CLIENT_SETTING);
    oc_w_str(w, m->client_type);
    oc_w_str(w, m->key);
    oc_w_str(w, m->value);
    return oc_frame_end(w, off);
}

oc_result oc_encode_list_client_settings(oc_wbuf *w, uint16_t version, const oc_list_client_settings *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_CLIENT_SETTINGS);
    oc_w_str(w, m->client_type);
    return oc_frame_end(w, off);
}

/* Storage usage report (REQ-214). Fixed-width fields only, so it needs no
 * length prefixes and stays trivially forward-compatible: a later version can
 * append fields and an older client simply stops reading early. */
oc_result oc_encode_audit_query(oc_wbuf *w, uint16_t version, const oc_audit_query *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_AUDIT_QUERY);
    oc_w_u64(w, m->before_ms);
    oc_w_u16(w, m->limit);
    return oc_frame_end(w, off);
}

oc_result oc_decode_audit_query(oc_rbuf *r, oc_audit_query *m) {
    m->before_ms = oc_r_u64(r);
    m->limit = oc_r_u16(r);
    return r_done(r);
}

oc_result oc_encode_audit_page(oc_wbuf *w, uint16_t version, const oc_audit_page *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_AUDIT_PAGE);
    oc_w_u16(w, m->count);
    for (uint16_t i = 0; i < m->count; i++) {
        const oc_audit_entry *e = &m->entries[i];
        oc_w_u64(w, e->at_ms);
        oc_w_u64(w, e->actor_id);
        oc_w_u64(w, e->target_id);
        oc_w_u8(w, e->family);
        oc_w_u8(w, e->outcome);
        oc_w_str(w, e->actor_name);
        oc_w_str(w, e->action);
        oc_w_str(w, e->target);
        oc_w_str(w, e->detail);
    }
    return oc_frame_end(w, off);
}

oc_result oc_decode_audit_page(oc_rbuf *r, oc_audit_entry *out, uint16_t cap, uint16_t *n) {
    uint16_t count = oc_r_u16(r);
    if (count > cap) count = cap;          /* clamp: never write past the caller's array */
    for (uint16_t i = 0; i < count; i++) {
        out[i].at_ms     = oc_r_u64(r);
        out[i].actor_id  = oc_r_u64(r);
        out[i].target_id = oc_r_u64(r);
        out[i].family    = oc_r_u8(r);
        out[i].outcome   = oc_r_u8(r);
        out[i].actor_name = oc_r_str(r);
        out[i].action     = oc_r_str(r);
        out[i].target     = oc_r_str(r);
        out[i].detail     = oc_r_str(r);
    }
    *n = count;
    return r_done(r);
}

oc_result oc_encode_storage_status(oc_wbuf *w, uint16_t version, const oc_storage_status *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_STORAGE_STATUS);
    oc_w_u64(w, m->total_bytes);
    oc_w_u64(w, m->avail_bytes);
    oc_w_u64(w, m->attach_bytes);
    oc_w_u64(w, m->attach_count);
    oc_w_u64(w, m->reclaimed_orphan);
    oc_w_u64(w, m->reclaimed_expired);
    oc_w_u64(w, m->reclaimed_evicted);
    oc_w_u64(w, m->last_reclaim_ms);
    oc_w_u64(w, m->max_age_days);
    oc_w_u64(w, m->reserve_bytes);
    oc_w_u8(w, m->evict_enabled);
    oc_w_u8(w, m->under_pressure);
    return oc_frame_end(w, off);
}

oc_result oc_decode_storage_status(oc_rbuf *r, oc_storage_status *m) {
    m->total_bytes      = oc_r_u64(r);
    m->avail_bytes      = oc_r_u64(r);
    m->attach_bytes     = oc_r_u64(r);
    m->attach_count     = oc_r_u64(r);
    m->reclaimed_orphan = oc_r_u64(r);
    m->reclaimed_expired= oc_r_u64(r);
    m->reclaimed_evicted= oc_r_u64(r);
    m->last_reclaim_ms  = oc_r_u64(r);
    m->max_age_days     = oc_r_u64(r);
    m->reserve_bytes    = oc_r_u64(r);
    m->evict_enabled    = oc_r_u8(r);
    m->under_pressure   = oc_r_u8(r);
    return r_done(r);
}

oc_result oc_encode_client_settings(oc_wbuf *w, uint16_t version, const oc_client_settings *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_CLIENT_SETTINGS);
    oc_w_str(w, m->client_type);
    oc_w_u16(w, m->count);
    for (uint16_t i = 0; i < m->count; i++) {
        oc_w_str(w, m->entries[i].key);
        oc_w_str(w, m->entries[i].value);
    }
    return oc_frame_end(w, off);
}

oc_result oc_decode_set_client_setting(oc_rbuf *p, oc_set_client_setting *m) {
    m->client_type = oc_r_str(p);
    m->key = oc_r_str(p);
    m->value = oc_r_str(p);
    return r_done(p);
}

oc_result oc_decode_list_client_settings(oc_rbuf *p, oc_list_client_settings *m) {
    m->client_type = oc_r_str(p);
    return r_done(p);
}

oc_result oc_decode_client_settings(oc_rbuf *p, oc_client_settings *m,
                                    oc_client_setting_entry *entries, uint16_t cap) {
    m->client_type = oc_r_str(p);
    uint16_t count = oc_r_u16(p);
    m->count = count;
    m->entries = entries;
    for (uint16_t i = 0; i < count && !p->underflow; i++) {
        oc_slice k = oc_r_str(p);
        oc_slice v = oc_r_str(p);
        if (i < cap) { entries[i].key = k; entries[i].value = v; }
    }
    if (m->count > cap) m->count = cap;
    return r_done(p);
}

/* --- Audio call signaling (REQ-150) ------------------------------------- */

oc_result oc_encode_call_join(oc_wbuf *w, uint16_t version, const oc_call_join *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_CALL_JOIN);
    oc_w_u64(w, m->channel_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_call_leave(oc_wbuf *w, uint16_t version, const oc_call_leave *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_CALL_LEAVE);
    oc_w_u64(w, m->channel_id);
    return oc_frame_end(w, off);
}

oc_result oc_encode_call_joined(oc_wbuf *w, uint16_t version, const oc_call_joined *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_CALL_JOINED);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->call_id);
    oc_w_u16(w, m->udp_port);
    oc_w_bytes(w, m->token);
    oc_w_u16(w, m->count);
    for (uint16_t i = 0; i < m->count; i++) oc_w_u64(w, m->participants[i]);
    return oc_frame_end(w, off);
}

oc_result oc_encode_call_roster(oc_wbuf *w, uint16_t version, const oc_call_roster *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_CALL_ROSTER);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->call_id);
    oc_w_u16(w, m->count);
    for (uint16_t i = 0; i < m->count; i++) oc_w_u64(w, m->participants[i]);
    return oc_frame_end(w, off);
}

oc_result oc_decode_call_join(oc_rbuf *p, oc_call_join *m) {
    m->channel_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_call_leave(oc_rbuf *p, oc_call_leave *m) {
    m->channel_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_call_joined(oc_rbuf *p, oc_call_joined *m, uint64_t *parts, uint16_t cap) {
    m->channel_id = oc_r_u64(p);
    m->call_id = oc_r_u64(p);
    m->udp_port = oc_r_u16(p);
    m->token = oc_r_bytes(p);
    uint16_t count = oc_r_u16(p);
    m->count = count;
    m->participants = parts;
    for (uint16_t i = 0; i < count && !p->underflow; i++) {
        uint64_t u = oc_r_u64(p);
        if (i < cap) parts[i] = u;
    }
    return r_done(p);
}

oc_result oc_decode_call_roster(oc_rbuf *p, oc_call_roster *m, uint64_t *parts, uint16_t cap) {
    m->channel_id = oc_r_u64(p);
    m->call_id = oc_r_u64(p);
    uint16_t count = oc_r_u16(p);
    m->count = count;
    m->participants = parts;
    for (uint16_t i = 0; i < count && !p->underflow; i++) {
        uint64_t u = oc_r_u64(p);
        if (i < cap) parts[i] = u;
    }
    return r_done(p);
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
        oc_w_u64(w, m->entries[i].avatar_id);      /* WIN-47; 0 = none */
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

oc_result oc_encode_set_display_name(oc_wbuf *w, uint16_t version, const oc_set_display_name *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SET_DISPLAY_NAME);
    oc_w_str(w, m->name);
    return oc_frame_end(w, off);
}

oc_result oc_encode_change_password(oc_wbuf *w, uint16_t version, const oc_change_password *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_CHANGE_PASSWORD);
    oc_w_str(w, m->old_password);
    oc_w_str(w, m->new_password);
    return oc_frame_end(w, off);
}

oc_result oc_encode_profile_updated(oc_wbuf *w, uint16_t version, const oc_profile_updated *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_PROFILE_UPDATED);
    oc_w_u64(w, m->user_id);
    oc_w_str(w, m->display_name);
    return oc_frame_end(w, off);
}

oc_result oc_encode_search(oc_wbuf *w, uint16_t version, const oc_search *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SEARCH);
    oc_w_str(w, m->query);
    oc_w_u16(w, m->limit);
    /* WIN-38/39 fields. A layout change, hence protocol 3. */
    oc_w_u64(w, m->before_id);
    oc_w_str(w, m->from_name);
    oc_w_str(w, m->in_channel);
    oc_w_u8(w, m->has_mask);
    oc_w_u64(w, m->after_ms);
    oc_w_u64(w, m->before_ms);
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

oc_result oc_encode_history_request(oc_wbuf *w, uint16_t version, const oc_history_request *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_HISTORY_REQUEST);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->before_message_id);
    oc_w_u16(w, m->limit);
    return oc_frame_end(w, off);
}

oc_result oc_decode_history_request(oc_rbuf *p, oc_history_request *m) {
    m->channel_id        = oc_r_u64(p);
    m->before_message_id = oc_r_u64(p);
    m->limit             = oc_r_u16(p);
    return r_done(p);
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

oc_result oc_decode_workspace_info(oc_rbuf *p, oc_workspace_info *m) {
    m->deployment_mode = oc_r_u8(p);
    m->max_users = oc_r_u32(p);
    m->workspace_name = oc_r_str(p);
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
            m->attach[i].reclaimed = oc_r_u8(p);
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

oc_result oc_decode_read_cursor(oc_rbuf *p, oc_read_cursor *m) {
    m->channel_id = oc_r_u64(p);
    m->user_id = oc_r_u64(p);
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

oc_result oc_decode_pin(oc_rbuf *p, oc_pin *m) {
    m->channel_id = oc_r_u64(p);
    m->message_id = oc_r_u64(p);
    m->op = oc_r_u8(p);
    return r_done(p);
}

oc_result oc_decode_pin_updated(oc_rbuf *p, oc_pin_updated *m) {
    m->message_id = oc_r_u64(p);
    m->channel_id = oc_r_u64(p);
    m->user_id = oc_r_u64(p);
    m->op = oc_r_u8(p);
    m->pinned_at = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_list_pins(oc_rbuf *p, oc_list_pins *m) {
    m->channel_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_pinned_msg(oc_rbuf *p, oc_pinned_msg *m) {
    m->message_id  = oc_r_u64(p);
    m->channel_id  = oc_r_u64(p);
    m->author_id   = oc_r_u64(p);
    m->server_time = oc_r_u64(p);
    m->pinned_by   = oc_r_u64(p);
    m->pinned_at   = oc_r_u64(p);
    m->body        = oc_r_str(p);
    m->attach_name = oc_r_str(p);
    return r_done(p);
}

oc_result oc_decode_pins(oc_rbuf *p, oc_pins *m) {
    m->channel_id = oc_r_u64(p);
    m->count = oc_r_u32(p);
    return r_done(p);
}

oc_result oc_decode_list_members(oc_rbuf *p, oc_list_members *m) {
    m->channel_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_member_entry(oc_rbuf *p, oc_member_entry *m) {
    m->channel_id = oc_r_u64(p);
    m->user_id    = oc_r_u64(p);
    m->role       = oc_r_u8(p);
    m->joined_at  = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_mention_unresolved(oc_rbuf *p, oc_mention_unresolved *m) {
    memset(m, 0, sizeof *m);
    m->channel_id = oc_r_u64(p);
    m->message_id = oc_r_u64(p);
    m->can_add    = oc_r_u8(p);
    m->is_private = oc_r_u8(p);
    uint16_t n    = oc_r_u16(p);
    /* Read every entry the sender wrote even past our cap, or the remaining
     * bytes are misread as the next frame — the parser must consume exactly what
     * was framed regardless of what we can store. */
    for (uint16_t i = 0; i < n; i++) {
        uint64_t uid = oc_r_u64(p);
        oc_slice nm  = oc_r_str(p);
        if (i >= OC_UNRESOLVED_MAX) continue;
        m->who[i].user_id = uid;
        size_t len = nm.len < OC_MAX_DISPLAY_NAME ? nm.len : OC_MAX_DISPLAY_NAME;
        if (nm.ptr && len) memcpy(m->who[i].name, nm.ptr, len);
        m->who[i].name[len] = '\0';
        m->count = (uint16_t)(i + 1);
    }
    return r_done(p);
}

oc_result oc_decode_members(oc_rbuf *p, oc_members *m) {
    m->channel_id = oc_r_u64(p);
    m->count      = oc_r_u32(p);
    return r_done(p);
}

oc_result oc_decode_list_files(oc_rbuf *p, oc_list_files *m) {
    m->channel_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_file_entry(oc_rbuf *p, oc_file_entry *m) {
    m->attachment_id = oc_r_u64(p);
    m->channel_id    = oc_r_u64(p);
    m->message_id    = oc_r_u64(p);
    m->uploader_id   = oc_r_u64(p);
    m->size          = oc_r_u64(p);
    m->created_at    = oc_r_u64(p);
    m->reclaimed     = oc_r_u8(p);
    m->filename      = oc_r_str(p);
    m->mime          = oc_r_str(p);
    return r_done(p);
}

oc_result oc_decode_files(oc_rbuf *p, oc_files *m) {
    m->channel_id = oc_r_u64(p);
    m->count      = oc_r_u32(p);
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
            m->attach[i].reclaimed = oc_r_u8(p);
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

oc_result oc_decode_update_channel(oc_rbuf *p, oc_update_channel *m) {
    m->channel_id = oc_r_u64(p);
    m->op         = oc_r_u8(p);
    m->value      = oc_r_str(p);
    return r_done(p);
}

oc_result oc_decode_save_item(oc_rbuf *p, oc_save_item *m) {
    m->message_id = oc_r_u64(p); m->op = oc_r_u8(p);
    return r_done(p);
}
oc_result oc_decode_saved_updated(oc_rbuf *p, oc_saved_updated *m) {
    m->message_id = oc_r_u64(p); m->op = oc_r_u8(p); m->saved_at = oc_r_u64(p);
    return r_done(p);
}
oc_result oc_decode_saved_msg(oc_rbuf *p, oc_saved_msg *m) {
    m->message_id = oc_r_u64(p); m->channel_id = oc_r_u64(p); m->author_id = oc_r_u64(p);
    m->server_time = oc_r_u64(p); m->saved_at = oc_r_u64(p);
    m->body = oc_r_str(p); m->attach_name = oc_r_str(p);
    return r_done(p);
}
oc_result oc_decode_saved(oc_rbuf *p, oc_saved *m) {
    m->count = oc_r_u32(p);
    return r_done(p);
}
oc_result oc_decode_activity_entry(oc_rbuf *p, oc_activity_entry *m) {
    m->kind = oc_r_u8(p); m->message_id = oc_r_u64(p); m->channel_id = oc_r_u64(p);
    m->actor_id = oc_r_u64(p); m->at = oc_r_u64(p); m->text = oc_r_str(p);
    return r_done(p);
}
oc_result oc_decode_activity(oc_rbuf *p, oc_activity *m) {
    m->count = oc_r_u32(p); m->seen_at = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_decode_history_around(oc_rbuf *p, oc_history_around *m) {
    m->channel_id = oc_r_u64(p); m->message_id = oc_r_u64(p); m->limit = oc_r_u16(p);
    return r_done(p);
}

oc_result oc_decode_channel_info(oc_rbuf *p, oc_channel_info *m) {
    m->channel_id = oc_r_u64(p);
    m->kind = oc_r_u8(p);
    m->name = oc_r_str(p);
    m->is_public = oc_r_u8(p);
    m->joined = oc_r_u8(p);
    m->created_at = oc_r_u64(p);
    m->peer_id  = oc_r_u64(p);
    m->topic    = oc_r_str(p);
    m->archived = oc_r_u8(p);
    m->n_peers  = oc_r_u16(p);
    if (m->n_peers > OC_MAX_GROUP_DM + 1) return OC_E_MALFORMED;
    for (uint16_t i = 0; i < m->n_peers; i++) m->peers[i] = oc_r_u64(p);
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
        uint64_t last_at = oc_r_u64(p);
        uint32_t unread = oc_r_u32(p);
        uint64_t peer = oc_r_u64(p);
        oc_slice topic = oc_r_str(p);
        uint8_t archived = oc_r_u8(p);
        uint64_t created = oc_r_u64(p);
        oc_slice prev = oc_r_str(p);
        uint64_t prev_a = oc_r_u64(p);
        uint16_t np = oc_r_u16(p);
        if (np > OC_MAX_GROUP_DM + 1) return OC_E_MALFORMED;
        uint64_t peers[OC_MAX_GROUP_DM + 1];
        for (uint16_t k = 0; k < np; k++) peers[k] = oc_r_u64(p);
        if (i < cap) {
            entries[i].n_peers = np;
            for (uint16_t k = 0; k < np; k++) entries[i].peers[k] = peers[k];
            entries[i].channel_id = id;
            entries[i].name = name;
            entries[i].is_public = is_public;
            entries[i].joined = joined;
            entries[i].kind = kind;
            entries[i].last_message_at = last_at;
            entries[i].unread = unread;
            entries[i].peer_id = peer;
            entries[i].topic = topic;
            entries[i].archived = archived;
            entries[i].created_at = created;
            entries[i].preview = prev;
            entries[i].preview_author = prev_a;
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
        uint64_t avatar = oc_r_u64(p);
        if (i < cap) {
            entries[i].user_id = uid;
            entries[i].role = role;
            entries[i].disabled = disabled;
            entries[i].email = email;
            entries[i].display_name = name;
            entries[i].avatar_id = avatar;
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

oc_result oc_decode_set_display_name(oc_rbuf *p, oc_set_display_name *m) {
    m->name = oc_r_str(p);
    return r_done(p);
}

oc_result oc_decode_change_password(oc_rbuf *p, oc_change_password *m) {
    m->old_password = oc_r_str(p);
    m->new_password = oc_r_str(p);
    return r_done(p);
}

oc_result oc_decode_profile_updated(oc_rbuf *p, oc_profile_updated *m) {
    m->user_id = oc_r_u64(p);
    m->display_name = oc_r_str(p);
    return r_done(p);
}

oc_result oc_decode_search(oc_rbuf *p, oc_search *m) {
    m->query      = oc_r_str(p);
    m->limit      = oc_r_u16(p);
    m->before_id  = oc_r_u64(p);
    m->from_name  = oc_r_str(p);
    m->in_channel = oc_r_str(p);
    m->has_mask   = oc_r_u8(p);
    m->after_ms   = oc_r_u64(p);
    m->before_ms  = oc_r_u64(p);
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

/* ---- invite management (REQ-026, WIN-46) + webhook lifecycle (WIN-48) ------- */

oc_result oc_encode_set_webhook_state(oc_wbuf *w, uint16_t version, const oc_set_webhook_state *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SET_WEBHOOK_STATE);
    oc_w_u64(w, m->webhook_id);
    oc_w_u8(w, m->disabled);
    return oc_frame_end(w, off);
}

oc_result oc_decode_set_webhook_state(oc_rbuf *p, oc_set_webhook_state *m) {
    m->webhook_id = oc_r_u64(p);
    m->disabled   = oc_r_u8(p);
    return r_done(p);
}

oc_result oc_encode_rotate_webhook(oc_wbuf *w, uint16_t version, const oc_rotate_webhook *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_ROTATE_WEBHOOK);
    oc_w_u64(w, m->webhook_id);
    return oc_frame_end(w, off);
}

oc_result oc_decode_rotate_webhook(oc_rbuf *p, oc_rotate_webhook *m) {
    m->webhook_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_encode_list_invites(oc_wbuf *w, uint16_t version) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_INVITES);
    return oc_frame_end(w, off);
}

/* The token is NOT in this frame and must never be: only its SHA-256 is stored, and
 * a list any admin can pull is not a place to hand back credentials. */
oc_result oc_encode_invite_list(oc_wbuf *w, uint16_t version, const oc_invite_list *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_INVITE_LIST);
    oc_w_u16(w, m->count);
    for (uint16_t i = 0; i < m->count; i++) {
        oc_w_u64(w, m->entries[i].invite_id);
        oc_w_u8(w, m->entries[i].role);
        oc_w_u64(w, m->entries[i].created_at);
        oc_w_u64(w, m->entries[i].expires_at);
        oc_w_u64(w, m->entries[i].created_by);
    }
    return oc_frame_end(w, off);
}

oc_result oc_decode_invite_list(oc_rbuf *p, oc_invite_entry *entries, uint16_t cap,
                               uint16_t *out_count) {
    uint16_t count = oc_r_u16(p);
    if (out_count) *out_count = count < cap ? count : cap;
    for (uint16_t i = 0; i < count && !p->underflow; i++) {
        oc_invite_entry e;
        e.invite_id  = oc_r_u64(p);
        e.role       = oc_r_u8(p);
        e.created_at = oc_r_u64(p);
        e.expires_at = oc_r_u64(p);
        e.created_by = oc_r_u64(p);
        if (i < cap) entries[i] = e;   /* drain the rest so the frame still validates */
    }
    return r_done(p);
}

oc_result oc_encode_revoke_invite(oc_wbuf *w, uint16_t version, const oc_revoke_invite *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_REVOKE_INVITE);
    oc_w_u64(w, m->invite_id);
    return oc_frame_end(w, off);
}

oc_result oc_decode_revoke_invite(oc_rbuf *p, oc_revoke_invite *m) {
    m->invite_id = oc_r_u64(p);
    return r_done(p);
}

/* The S->C ack. Same single field as the request, but its own opcode — the first
 * attempt re-tagged the request frame in place, which wrote over the VERSION field
 * because msg_type sits at offset 6, not 2. A frame is cheaper than that class of
 * bug. */
oc_result oc_encode_invite_revoked(oc_wbuf *w, uint16_t version, const oc_revoke_invite *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_INVITE_REVOKED);
    oc_w_u64(w, m->invite_id);
    return oc_frame_end(w, off);
}

oc_result oc_decode_invite_revoked(oc_rbuf *p, oc_revoke_invite *m) {
    m->invite_id = oc_r_u64(p);
    return r_done(p);
}

/* Mute (REQ-137, WIN-40) and mark-unread (REQ-235, WIN-52). */

oc_result oc_encode_set_mute(oc_wbuf *w, uint16_t version, const oc_set_mute *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SET_MUTE);
    oc_w_u64(w, m->channel_id);
    oc_w_u8(w, m->muted);
    return oc_frame_end(w, off);
}

oc_result oc_decode_set_mute(oc_rbuf *p, oc_set_mute *m) {
    m->channel_id = oc_r_u64(p);
    m->muted      = oc_r_u8(p);
    return r_done(p);
}

oc_result oc_encode_set_read_cursor(oc_wbuf *w, uint16_t version, const oc_set_read_cursor *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SET_READ_CURSOR);
    oc_w_u64(w, m->channel_id);
    oc_w_u64(w, m->message_id);
    return oc_frame_end(w, off);
}

oc_result oc_decode_set_read_cursor(oc_rbuf *p, oc_set_read_cursor *m) {
    m->channel_id = oc_r_u64(p);
    m->message_id = oc_r_u64(p);
    return r_done(p);
}

/* Custom status (REQ-241/122, WIN-53) and profile fields (REQ-240, WIN-47). */

oc_result oc_encode_set_status(oc_wbuf *w, uint16_t version, const oc_set_status *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SET_STATUS);
    oc_w_str(w, m->emoji);
    oc_w_str(w, m->text);
    oc_w_u64(w, m->expires_at);
    return oc_frame_end(w, off);
}

oc_result oc_decode_set_status(oc_rbuf *p, oc_set_status *m) {
    m->emoji      = oc_r_str(p);
    m->text       = oc_r_str(p);
    m->expires_at = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_encode_set_profile(oc_wbuf *w, uint16_t version, const oc_set_profile *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SET_PROFILE);
    oc_w_str(w, m->title);
    oc_w_str(w, m->timezone);
    return oc_frame_end(w, off);
}

oc_result oc_decode_set_profile(oc_rbuf *p, oc_set_profile *m) {
    m->title    = oc_r_str(p);
    m->timezone = oc_r_str(p);
    return r_done(p);
}

/* One frame per PERSON, not per field: a roster wants everything it shows about
 * somebody in a single message, and it doubles as the push when any of it changes. */
oc_result oc_encode_profile_info(oc_wbuf *w, uint16_t version, const oc_profile_info *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_PROFILE_INFO);
    oc_w_u64(w, m->user_id);
    oc_w_str(w, m->display_name);
    oc_w_str(w, m->email);
    oc_w_str(w, m->status_emoji);
    oc_w_str(w, m->status_text);
    oc_w_u64(w, m->status_expires);
    oc_w_str(w, m->title);
    oc_w_str(w, m->timezone);
    oc_w_u64(w, m->avatar_id);
    oc_w_u8(w, m->role);
    return oc_frame_end(w, off);
}

oc_result oc_decode_profile_info(oc_rbuf *p, oc_profile_info *m) {
    m->user_id        = oc_r_u64(p);
    m->display_name   = oc_r_str(p);
    m->email          = oc_r_str(p);
    m->status_emoji   = oc_r_str(p);
    m->status_text    = oc_r_str(p);
    m->status_expires = oc_r_u64(p);
    m->title          = oc_r_str(p);
    m->timezone       = oc_r_str(p);
    m->avatar_id      = oc_r_u64(p);
    m->role           = oc_r_u8(p);
    return r_done(p);
}

/* Which channels hold files (WIN-82). */

oc_result oc_encode_list_file_channels(oc_wbuf *w, uint16_t version) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_FILE_CHANNELS);
    return oc_frame_end(w, off);
}

oc_result oc_encode_file_channels(oc_wbuf *w, uint16_t version, const oc_file_channels *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_FILE_CHANNELS);
    oc_w_u16(w, m->count);
    for (uint16_t i = 0; i < m->count; i++) {
        oc_w_u64(w, m->entries[i].channel_id);
        oc_w_u32(w, m->entries[i].count);
    }
    return oc_frame_end(w, off);
}

oc_result oc_decode_file_channels(oc_rbuf *p, oc_file_channel_entry *entries, uint16_t cap,
                                  uint16_t *out_count) {
    uint16_t count = oc_r_u16(p);
    if (out_count) *out_count = count < cap ? count : cap;
    for (uint16_t i = 0; i < count && !p->underflow; i++) {
        uint64_t cid = oc_r_u64(p);
        uint32_t n   = oc_r_u32(p);
        if (i < cap) { entries[i].channel_id = cid; entries[i].count = n; }
    }
    return r_done(p);
}

/* The caller's active sessions (REQ-182). Tokens are never listed: only their hashes
 * are stored, and a list is not a place to hand credentials back. */

oc_result oc_encode_list_sessions(oc_wbuf *w, uint16_t version) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_SESSIONS);
    return oc_frame_end(w, off);
}

oc_result oc_encode_session_list(oc_wbuf *w, uint16_t version, const oc_session_list *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SESSION_LIST);
    oc_w_u16(w, m->count);
    for (uint16_t i = 0; i < m->count; i++) {
        oc_w_u64(w, m->entries[i].session_id);
        oc_w_u64(w, m->entries[i].created_at);
        oc_w_u64(w, m->entries[i].last_seen);
        oc_w_u64(w, m->entries[i].expires_at);
        oc_w_u8(w, m->entries[i].current);
        oc_w_str(w, m->entries[i].device_label);
    }
    return oc_frame_end(w, off);
}

oc_result oc_decode_session_list(oc_rbuf *p, oc_session_entry *entries, uint16_t cap,
                                 uint16_t *out_count) {
    uint16_t count = oc_r_u16(p);
    if (out_count) *out_count = count < cap ? count : cap;
    for (uint16_t i = 0; i < count && !p->underflow; i++) {
        oc_session_entry e;
        e.session_id   = oc_r_u64(p);
        e.created_at   = oc_r_u64(p);
        e.last_seen    = oc_r_u64(p);
        e.expires_at   = oc_r_u64(p);
        e.current      = oc_r_u8(p);
        e.device_label = oc_r_str(p);
        if (i < cap) entries[i] = e;
    }
    return r_done(p);
}

/* The global notification default (REQ-134). */
oc_result oc_encode_set_notify_default(oc_wbuf *w, uint16_t version, const oc_set_notify_default *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SET_NOTIFY_DEFAULT);
    oc_w_u8(w, m->level);
    return oc_frame_end(w, off);
}

oc_result oc_decode_set_notify_default(oc_rbuf *p, oc_set_notify_default *m) {
    m->level = oc_r_u8(p);
    return r_done(p);
}

/* The avatar (WIN-47). */
oc_result oc_encode_set_avatar(oc_wbuf *w, uint16_t version, const oc_set_avatar *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_SET_AVATAR);
    oc_w_u64(w, m->attachment_id);
    return oc_frame_end(w, off);
}

oc_result oc_decode_set_avatar(oc_rbuf *p, oc_set_avatar *m) {
    m->attachment_id = oc_r_u64(p);
    return r_done(p);
}

/* A group DM (REQ-056). */
oc_result oc_encode_open_group_dm(oc_wbuf *w, uint16_t version, const oc_open_group_dm *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_OPEN_GROUP_DM);
    uint16_t n = m->count > OC_MAX_GROUP_DM ? (uint16_t)OC_MAX_GROUP_DM : m->count;
    oc_w_u16(w, n);
    for (uint16_t i = 0; i < n; i++) oc_w_u64(w, m->user_ids[i]);
    return oc_frame_end(w, off);
}

oc_result oc_decode_open_group_dm(oc_rbuf *p, oc_open_group_dm *m) {
    memset(m, 0, sizeof *m);
    uint16_t n = oc_r_u16(p);
    /* A count beyond the cap is not clamped silently: the ids would be read as
     * whatever followed, and a wrong participant set is worse than a refusal. */
    if (n > OC_MAX_GROUP_DM) return OC_E_MALFORMED;
    m->count = n;
    for (uint16_t i = 0; i < n; i++) m->user_ids[i] = oc_r_u64(p);
    return r_done(p);
}

/* Custom emoji (REQ-072). */
oc_result oc_encode_add_emoji(oc_wbuf *w, uint16_t version, const oc_add_emoji *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_ADD_EMOJI);
    oc_w_str(w, m->name);
    oc_w_u64(w, m->attachment_id);
    return oc_frame_end(w, off);
}

oc_result oc_decode_add_emoji(oc_rbuf *p, oc_add_emoji *m) {
    m->name = oc_r_str(p);
    m->attachment_id = oc_r_u64(p);
    return r_done(p);
}

oc_result oc_encode_delete_emoji(oc_wbuf *w, uint16_t version, const oc_delete_emoji *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_DELETE_EMOJI);
    oc_w_str(w, m->name);
    return oc_frame_end(w, off);
}

oc_result oc_decode_delete_emoji(oc_rbuf *p, oc_delete_emoji *m) {
    m->name = oc_r_str(p);
    return r_done(p);
}

oc_result oc_encode_list_emoji(oc_wbuf *w, uint16_t version) {
    size_t off = oc_frame_begin(w, version, OC_MSG_LIST_EMOJI);
    return oc_frame_end(w, off);
}

oc_result oc_decode_list_emoji(oc_rbuf *p) { return r_done(p); }

oc_result oc_encode_emoji_list(oc_wbuf *w, uint16_t version, const oc_emoji_list *m) {
    size_t off = oc_frame_begin(w, version, OC_MSG_EMOJI_LIST);
    oc_w_u16(w, m->count);
    for (uint16_t i = 0; i < m->count; i++) {
        oc_w_str(w, m->entries[i].name);
        oc_w_u64(w, m->entries[i].attachment_id);
        oc_w_u64(w, m->entries[i].created_by);
    }
    return oc_frame_end(w, off);
}

oc_result oc_decode_emoji_list(oc_rbuf *p, oc_emoji_entry *entries, uint16_t cap,
                               uint16_t *out_count) {
    uint16_t count = oc_r_u16(p);
    *out_count = count;
    for (uint16_t i = 0; i < count && !p->underflow; i++) {
        oc_slice nm = oc_r_str(p);
        uint64_t aid = oc_r_u64(p);
        uint64_t by = oc_r_u64(p);
        if (i < cap) {
            entries[i].name = nm;
            entries[i].attachment_id = aid;
            entries[i].created_by = by;
        }
    }
    return r_done(p);
}
