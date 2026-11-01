/* The coverage-guided fuzzing tier (docs/TESTING.md, "Fuzzing"). Each harness
 * is a function over one input, called by libFuzzer through tests/fuzz/main.c
 * and, for every checked-in corpus file and reproducer, by `make test`
 * (tests/test_fuzz_replay.c). A harness aborts on a broken contract, so the
 * fuzzer and the sanitizers report it the same way as a crash. */

#ifndef OC_FUZZ_H
#define OC_FUZZ_H

#include <stddef.h>
#include <stdint.h>

#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>

/* Called with the decoder's name when it returns neither OC_OK nor
 * OC_E_MALFORMED. */
typedef void (*oc_fuzz_bad_fn)(const char *what);

/* Every payload decoder over `buf`, each on its own reader. None may crash or
 * read past `len`. */
void oc_fuzz_decode_all(const uint8_t *buf, size_t len, oc_fuzz_bad_fn bad);

/* The harnesses: 0, or abort(). */
int oc_fuzz_frame(const uint8_t *data, size_t len);
int oc_fuzz_framebuf(const uint8_t *data, size_t len);
/* The pre-authentication parsers (docs/TESTING.md, "Fuzzing"). */
int oc_fuzz_http(const uint8_t *data, size_t len);
int oc_fuzz_ws(const uint8_t *data, size_t len);
int oc_fuzz_proxy(const uint8_t *data, size_t len);
int oc_fuzz_json(const uint8_t *data, size_t len);
int oc_fuzz_cbor(const uint8_t *data, size_t len);
int oc_fuzz_jwt(const uint8_t *data, size_t len);
int oc_fuzz_webauthn(const uint8_t *data, size_t len);
int oc_fuzz_idtoken(const uint8_t *data, size_t len);

/* An input as `n` parts, for a function that takes several: each part but the
 * last is prefixed by its length, a big-endian u16, cut short where the input
 * ends; the last is the rest. Each part is copied and NUL-terminated (into
 * `buf`, which must hold len + n bytes), since several take strings. */
static inline void oc_fuzz_split(const uint8_t *data, size_t len, size_t n,
                                 char *buf, const char **part, size_t *plen) {
    size_t at = 0, out = 0;
    for (size_t i = 0; i < n; i++) {
        size_t want;
        if (i + 1 < n) {
            want = at + 2 <= len ? ((size_t)data[at] << 8 | data[at + 1]) : 0;
            at = at + 2 <= len ? at + 2 : len;
        } else {
            want = len - at;
        }
        if (want > len - at) want = len - at;
        part[i] = buf + out;
        plen[i] = want;
        for (size_t k = 0; k < want; k++) buf[out + k] = (char)data[at + k];
        buf[out + want] = '\0';
        out += want + 1;
        at += want;
    }
}

/* Stop, saying which harness and why: what every contract check does. */
#define OC_FUZZ_BROKEN(harness, why) do { \
        fprintf(stderr, "fuzz %s: %s\n", (harness), (why)); abort(); } while (0)

#endif /* OC_FUZZ_H */
