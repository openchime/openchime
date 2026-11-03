/* Harness: oc_parse_frame, then every payload decoder over the parsed payload
 * and over the raw bytes. A decoder may refuse anything, but never crash, read
 * outside its input, or answer with something other than OK or MALFORMED; a
 * parsed payload's view stays inside the input. */

#include "fuzz.h"

#include <stdio.h>
#include <stdlib.h>

static void bad(const char *what) {
    fprintf(stderr, "fuzz frame: %s returned neither OK nor MALFORMED\n", what);
    abort();
}

int oc_fuzz_frame(const uint8_t *data, size_t len) {
    oc_header hdr;
    oc_rbuf p;
    oc_result fr = oc_parse_frame(data, len, &hdr, &p);
    if (fr != OC_OK && fr != OC_E_MALFORMED && fr != OC_E_TOO_LARGE) {
        fprintf(stderr, "fuzz frame: oc_parse_frame returned %d\n", (int)fr);
        abort();
    }
    if (fr == OC_OK) {
        if (p.len && (p.data < data || p.data + p.len > data + len)) {
            fprintf(stderr, "fuzz frame: the payload view leaves the input\n");
            abort();
        }
        oc_fuzz_decode_all(p.data, p.len, bad);
    }
    oc_fuzz_decode_all(data, len, bad);
    return 0;
}
