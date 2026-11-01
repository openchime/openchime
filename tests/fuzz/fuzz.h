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

/* Called with the decoder's name when it returns neither OC_OK nor
 * OC_E_MALFORMED. */
typedef void (*oc_fuzz_bad_fn)(const char *what);

/* Every payload decoder over `buf`, each on its own reader. None may crash or
 * read past `len`. */
void oc_fuzz_decode_all(const uint8_t *buf, size_t len, oc_fuzz_bad_fn bad);

/* The harnesses: 0, or abort(). */
int oc_fuzz_frame(const uint8_t *data, size_t len);
int oc_fuzz_framebuf(const uint8_t *data, size_t len);

#endif /* OC_FUZZ_H */
