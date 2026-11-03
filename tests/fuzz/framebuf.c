/* Harness: shared/framebuf.c's reassembly. The input's first bytes choose the
 * chunk sizes it is pushed in, the rest is the stream; after each push every
 * complete frame is drained. A frame yielded must lie inside the buffer and
 * agree with its own length prefix. */

#include "fuzz.h"
#include "framebuf.h"

#include <stdio.h>
#include <stdlib.h>

static void broken(const char *why) {
    fprintf(stderr, "fuzz framebuf: %s\n", why);
    abort();
}

int oc_fuzz_framebuf(const uint8_t *data, size_t len) {
    if (len < 2) return 0;
    /* Up to eight chunk sizes, each 1..OC_READ_CHUNK, cycled. */
    size_t nsizes = (size_t)(data[0] % 8) + 1;
    if (len < 1 + nsizes * 2) return 0;
    size_t sizes[8];
    for (size_t i = 0; i < nsizes; i++) {
        size_t s = ((size_t)data[1 + i * 2] << 8 | data[2 + i * 2]) % OC_READ_CHUNK;
        sizes[i] = s ? s : 1;
    }
    const uint8_t *in = data + 1 + nsizes * 2;
    size_t left = len - (1 + nsizes * 2);

    oc_framebuf fb;
    if (oc_framebuf_init(&fb) != 0) return 0;
    for (size_t k = 0; left; k++) {
        size_t n = sizes[k % nsizes] < left ? sizes[k % nsizes] : left;
        if (oc_framebuf_push(&fb, in, n) != 0) broken("a push of at most OC_READ_CHUNK after a drain overflowed");
        in += n;
        left -= n;
        for (;;) {
            const uint8_t *frame;
            size_t flen;
            int r = oc_framebuf_next(&fb, &frame, &flen);
            if (r <= 0) {
                if (r < 0) left = 0;          /* a frame that can never be: the stream is over */
                break;
            }
            if (frame < fb.buf || flen < 4 || frame + flen > fb.buf + fb.len) broken("a frame outside the buffer");
            uint32_t prefix = (uint32_t)frame[0] << 24 | (uint32_t)frame[1] << 16 |
                              (uint32_t)frame[2] << 8 | frame[3];
            if ((size_t)prefix + 4 != flen) broken("a frame that disagrees with its length prefix");
        }
    }
    oc_framebuf_free(&fb);
    return 0;
}
