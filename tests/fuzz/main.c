/* The libFuzzer entry point for one harness, chosen when it is built:
 * -DOC_FUZZ_TARGET=oc_fuzz_<harness> (mk/tests.mk, build/fuzz/%). */

#include "fuzz.h"

#ifndef OC_FUZZ_TARGET
#error "build with -DOC_FUZZ_TARGET=oc_fuzz_<harness>"
#endif

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    return OC_FUZZ_TARGET(data, size);
}
