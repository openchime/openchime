/*
 * OpenChime incremental frame reassembler (ARCH-9, ARCH-22).
 *
 * TCP delivers a boundary-less byte stream; TLS records fragment it further.
 * This buffer accumulates bytes as they arrive (from oc_tls_read) and yields
 * complete protocol frames one at a time, applying the two-phase length
 * discipline and the size limits from PROTOCOL.md §2. It is pure logic — no
 * sockets — so it is unit-tested directly (docs/TESTING.md §2.2).
 */

#ifndef OPENCHIME_FRAMEBUF_H
#define OPENCHIME_FRAMEBUF_H

#include <stddef.h>
#include <stdint.h>

/* Max bytes handed to oc_framebuf_push in one call (a single TLS read). The
 * buffer is sized so a maximal partial frame plus one read always fits, so a
 * caller that drains frames after each push never overflows. */
#define OC_READ_CHUNK 16384u

typedef struct {
    uint8_t *buf;
    size_t   cap;   /* OC_MAX_FRAME_SIZE + OC_READ_CHUNK */
    size_t   len;   /* bytes written into buf */
    size_t   pos;   /* bytes already yielded (consumed) */
} oc_framebuf;

/* Allocate the backing buffer. Returns 0 on success, -1 on allocation failure. */
int  oc_framebuf_init(oc_framebuf *fb);
void oc_framebuf_free(oc_framebuf *fb);

/* Append `len` freshly-read bytes. Returns 0, or -1 if the bytes would overflow
 * the buffer (only possible if the caller pushes more than OC_READ_CHUNK
 * without draining). */
int  oc_framebuf_push(oc_framebuf *fb, const uint8_t *data, size_t len);

/* Yield the next complete frame. Returns 1 with frame/flen set to a view valid
 * until the next oc_framebuf_push; 0 if more bytes are needed; or a negative
 * oc_result (OC_E_TOO_LARGE / OC_E_MALFORMED) for a frame that can never be
 * valid, after which the connection should be dropped. */
int  oc_framebuf_next(oc_framebuf *fb, const uint8_t **frame, size_t *flen);

#endif /* OPENCHIME_FRAMEBUF_H */
