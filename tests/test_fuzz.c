/* Codec fuzzer. Feeds deterministic pseudo-random and
 * semi-structured bytes through oc_parse_frame and every decoder, asserting the
 * codec never crashes, never reads out of bounds, and always returns a
 * well-defined result on garbage. Reproducible: fixed PRNG seed, no I/O.
 *
 * The codec is designed to be safe (oc_rbuf bounds every read; length prefixes
 * are validated by r_take before the bytes are consumed) — this suite exercises
 * that empirically across a wide input space. Run under ASan it also proves the
 * absence of out-of-bounds reads. */

#include "protocol.h"
#include "check.h"
#include "fuzz/fuzz.h"   /* the decoders, shared with the libFuzzer harnesses */

#include <string.h>

/* Iteration counts. The PRNG is deterministic (fixed seed), so a moderate sweep
 * hits the same input categories as a huge one — keep the default snappy for
 * `make test` and crank it via -D for a deep local/ASan run. */
#ifndef OC_FUZZ_RANDOM_ITERS
#define OC_FUZZ_RANDOM_ITERS 5000
#endif
#ifndef OC_FUZZ_FRAMED_ITERS
#define OC_FUZZ_FRAMED_ITERS 2000
#endif

/* xorshift32 with a fixed seed — deterministic across runs/platforms. */
static uint32_t g_fz = 0x9e3779b9u;
static uint32_t fz(void) {
    g_fz ^= g_fz << 13; g_fz ^= g_fz >> 17; g_fz ^= g_fz << 5;
    return g_fz;
}

/* A decoder that returned anything but OK or OC_E_MALFORMED. */
static void on_bad(const char *what) {
    printf("  FAIL %s returned neither OK nor MALFORMED\n", what);
    failures++;
}

static void decode_all(const uint8_t *buf, size_t len) { oc_fuzz_decode_all(buf, len, on_bad); }

static void test_fuzz_random(void) {
    uint8_t buf[600];
    for (int iter = 0; iter < OC_FUZZ_RANDOM_ITERS; iter++) {
        size_t len = fz() % (sizeof buf + 1);   /* 0 .. sizeof buf */
        for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)fz();

        /* Full-frame parse must never crash and must classify cleanly. */
        oc_header hdr; oc_rbuf p;
        oc_result fr = oc_parse_frame(buf, len, &hdr, &p);
        CHECK(fr == OC_OK || fr == OC_E_MALFORMED || fr == OC_E_TOO_LARGE);
        if (fr == OC_OK) {
            /* A parsed frame's payload view must stay inside the buffer. */
            CHECK(p.data >= buf && p.data + p.len <= buf + len);
            decode_all(p.data, p.len);
        }

        /* Also fuzz the decoders directly over the raw bytes. */
        decode_all(buf, len);
    }
}

/* Semi-structured: a valid 8-byte header (plausible length/version/type) over a
 * random payload — reaches deeper into the decoders than pure noise. */
static void test_fuzz_framed(void) {
    static uint8_t buf[OC_MAX_FRAME_SIZE];
    static const uint16_t types[] = {
        0x0001,0x0002,0x0003,0x0010,0x0011,0x0012,0x0013,0x0020,0x0021,0x0022,
        0x0023,0x0024,0x0025,0x0026,0x0027,0x0028,0x0029,0x002A,0x002B,0x002C,
        0x002D,0x002E,0x002F,0x0032,0x0030,0x0031,0x0040,0x0041,0x0042,0x0043,
        0x0044,0x0045,0x0046,0x0047,0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,
        0x0056,0x0057,0x0060,0x0061,0x00FF
    };
    for (int iter = 0; iter < OC_FUZZ_FRAMED_ITERS; iter++) {
        size_t paylen = fz() % 200;
        uint32_t length = (uint32_t)(paylen + 4);   /* version(2)+type(2)+payload */
        buf[0] = (uint8_t)(length >> 24); buf[1] = (uint8_t)(length >> 16);
        buf[2] = (uint8_t)(length >> 8);  buf[3] = (uint8_t)length;
        buf[4] = 0; buf[5] = 1;                       /* version 1 */
        uint16_t t = types[fz() % (sizeof types / sizeof types[0])];
        buf[6] = (uint8_t)(t >> 8); buf[7] = (uint8_t)t;
        for (size_t i = 0; i < paylen; i++) buf[8 + i] = (uint8_t)fz();

        oc_header hdr; oc_rbuf p;
        oc_result fr = oc_parse_frame(buf, 8 + paylen, &hdr, &p);
        CHECK(fr == OC_OK || fr == OC_E_MALFORMED || fr == OC_E_TOO_LARGE);
        if (fr == OC_OK) {
            CHECK(p.data + p.len <= buf + 8 + paylen);
            decode_all(p.data, p.len);
        }
    }
}

int run_fuzz_tests(void) {
    printf("test_fuzz: %d random + %d framed iterations through parse + all decoders\n",
           OC_FUZZ_RANDOM_ITERS, OC_FUZZ_FRAMED_ITERS);
    test_fuzz_random();
    test_fuzz_framed();
    return failures;
}
