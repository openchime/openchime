/*
 * OpenChime incremental frame reassembler. See framebuf.h and protocol.h.
 */

#include "framebuf.h"
#include "protocol.h"

#include <stdlib.h>
#include <string.h>

int oc_framebuf_init(oc_framebuf *fb) {
    fb->cap = OC_MAX_FRAME_SIZE + OC_READ_CHUNK;
    fb->buf = malloc(fb->cap);
    fb->len = 0;
    fb->pos = 0;
    return fb->buf ? 0 : -1;
}

void oc_framebuf_free(oc_framebuf *fb) {
    free(fb->buf);
    fb->buf = NULL;
    fb->cap = fb->len = fb->pos = 0;
}

/* Drop already-yielded bytes, sliding the remainder to the front. */
static void compact(oc_framebuf *fb) {
    if (fb->pos == 0) return;
    size_t rem = fb->len - fb->pos;
    if (rem) memmove(fb->buf, fb->buf + fb->pos, rem);
    fb->len = rem;
    fb->pos = 0;
}

int oc_framebuf_push(oc_framebuf *fb, const uint8_t *data, size_t len) {
    compact(fb);
    if (len > fb->cap - fb->len) return -1;
    memcpy(fb->buf + fb->len, data, len);
    fb->len += len;
    return 0;
}

int oc_framebuf_next(oc_framebuf *fb, const uint8_t **frame, size_t *flen) {
    size_t avail = fb->len - fb->pos;
    if (avail < OC_HEADER_SIZE) return 0;

    const uint8_t *p = fb->buf + fb->pos;
    uint32_t length = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
                      ((uint32_t)p[2] << 8)  | (uint32_t)p[3];
    if (length < OC_LENGTH_MIN) return OC_E_MALFORMED;
    uint64_t total = (uint64_t)length + 4u;
    if (total > OC_MAX_FRAME_SIZE) return OC_E_TOO_LARGE;
    if (avail < total) return 0; /* frame not fully arrived */

    *frame = p;
    *flen = (size_t)total;
    fb->pos += (size_t)total;
    return 1;
}
