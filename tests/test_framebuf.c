/* Unit tests for the incremental frame reassembler (framebuf.c), the ARCH-9
 * two-phase read discipline over a boundary-less stream. Includes the code
 * under test directly (openblocks convention); pure, no sockets. */

#include "framebuf.h"
#include "protocol.h"
#include "check.h"

#include <string.h>

/* Encode a HELLO with the given range into `out`; return its byte length. */
static size_t make_hello(uint8_t *out, size_t cap, uint16_t mn, uint16_t mx) {
    oc_wbuf w;
    oc_wbuf_init(&w, out, cap);
    oc_hello h = { mn, mx, oc_slice_str("client/x") };
    oc_encode_hello(&w, &h);
    return w.len;
}

/* A frame arriving one byte at a time yields nothing until the final byte. */
static void test_byte_at_a_time(void) {
    uint8_t frame[128];
    size_t flen = make_hello(frame, sizeof frame, 1, 2);

    oc_framebuf fb;
    CHECK(oc_framebuf_init(&fb) == 0);

    const uint8_t *out; size_t olen;
    for (size_t i = 0; i < flen; i++) {
        CHECK(oc_framebuf_push(&fb, &frame[i], 1) == 0);
        int r = oc_framebuf_next(&fb, &out, &olen);
        if (i + 1 < flen) CHECK(r == 0);          /* not complete yet */
        else              CHECK(r == 1);          /* last byte completes it */
    }
    CHECK(olen == flen);

    /* And it decodes back to the original. */
    oc_header hdr; oc_rbuf p;
    CHECK(oc_parse_frame(out, olen, &hdr, &p) == OC_OK);
    CHECK(hdr.msg_type == OC_MSG_HELLO);
    oc_hello h;
    CHECK(oc_decode_hello(&p, &h) == OC_OK);
    CHECK(h.min_version == 1 && h.max_version == 2);

    CHECK(oc_framebuf_next(&fb, &out, &olen) == 0); /* nothing left */
    oc_framebuf_free(&fb);
}

/* Two frames in a single push are yielded in order, then exhausted. */
static void test_two_at_once(void) {
    uint8_t a[128], b[128];
    size_t alen = make_hello(a, sizeof a, 1, 1);
    size_t blen = make_hello(b, sizeof b, 2, 3);
    uint8_t both[256];
    memcpy(both, a, alen);
    memcpy(both + alen, b, blen);

    oc_framebuf fb;
    oc_framebuf_init(&fb);
    CHECK(oc_framebuf_push(&fb, both, alen + blen) == 0);

    const uint8_t *out; size_t olen;
    CHECK(oc_framebuf_next(&fb, &out, &olen) == 1 && olen == alen);
    CHECK(oc_framebuf_next(&fb, &out, &olen) == 1 && olen == blen);
    CHECK(oc_framebuf_next(&fb, &out, &olen) == 0);
    oc_framebuf_free(&fb);
}

/* A frame split across two pushes reassembles across the compaction boundary. */
static void test_split_push(void) {
    uint8_t frame[128];
    size_t flen = make_hello(frame, sizeof frame, 1, 1);

    oc_framebuf fb;
    oc_framebuf_init(&fb);
    const uint8_t *out; size_t olen;

    size_t cut = flen / 2;
    CHECK(oc_framebuf_push(&fb, frame, cut) == 0);
    CHECK(oc_framebuf_next(&fb, &out, &olen) == 0);
    CHECK(oc_framebuf_push(&fb, frame + cut, flen - cut) == 0);
    CHECK(oc_framebuf_next(&fb, &out, &olen) == 1 && olen == flen);
    oc_framebuf_free(&fb);
}

/* Bad length fields are surfaced as errors, not silently buffered forever. */
static void test_bad_length(void) {
    oc_framebuf fb;
    const uint8_t *out; size_t olen;

    /* length below the 4-byte minimum. */
    oc_framebuf_init(&fb);
    uint8_t small[8] = {0,0,0,3, 0,1, 0,1};
    oc_framebuf_push(&fb, small, sizeof small);
    CHECK(oc_framebuf_next(&fb, &out, &olen) == OC_E_MALFORMED);
    oc_framebuf_free(&fb);

    /* length implying a frame past the max size. */
    oc_framebuf_init(&fb);
    uint32_t huge = OC_MAX_FRAME_SIZE;               /* 4 + huge > max */
    uint8_t big[8] = { (uint8_t)(huge>>24),(uint8_t)(huge>>16),
                       (uint8_t)(huge>>8),(uint8_t)huge, 0,1, 0,1 };
    oc_framebuf_push(&fb, big, sizeof big);
    CHECK(oc_framebuf_next(&fb, &out, &olen) == OC_E_TOO_LARGE);
    oc_framebuf_free(&fb);
}

int run_framebuf_tests(void) {
    printf("test_framebuf: byte-at-a-time, two-at-once, split push, bad length\n");
    test_byte_at_a_time();
    test_two_at_once();
    test_split_push();
    test_bad_length();
    return failures;
}
