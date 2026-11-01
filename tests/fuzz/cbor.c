/* Harness: the CBOR reader passkeys use (daemon/cbor.c). Items read off the
 * input one after another, each as every type it might be, and as a map by a
 * text and an integer key. A string's bytes lie inside the input, and a
 * reader never moves past its end. */

#include "fuzz.h"
#include "cbor.h"

#define H "cbor"

static void within(const uint8_t *data, size_t len, const oc_cbor *c) {
    if (c->p < data || c->p > data + len || c->end != data + len) OC_FUZZ_BROKEN(H, "a reader outside the input");
}

int oc_fuzz_cbor(const uint8_t *data, size_t len) {
    oc_cbor c = { data, data + len };
    for (int items = 0; items < 64 && c.p < c.end; items++) {
        oc_cbor at = c, v;
        oc_cbor_major m;
        uint64_t arg;
        if (oc_cbor_head(&at, &m, &arg) != 0) break;
        within(data, len, &at);
        at = c;
        const uint8_t *s;
        size_t sl;
        if (oc_cbor_string(&at, OC_CBOR_BYTES, &s, &sl) == 0 && (s < data || s + sl > data + len))
            OC_FUZZ_BROKEN(H, "a byte string outside the input");
        at = c;
        if (oc_cbor_string(&at, OC_CBOR_TEXT, &s, &sl) == 0 && (s < data || s + sl > data + len))
            OC_FUZZ_BROKEN(H, "a text string outside the input");
        at = c;
        int64_t iv;
        (void)oc_cbor_int(&at, &iv);
        int r = oc_cbor_map_get(c, "fmt", 0, &v);
        if (r < -1 || r > 1) OC_FUZZ_BROKEN(H, "oc_cbor_map_get answered outside -1..1");
        if (r == 1) within(data, len, &v);
        r = oc_cbor_map_get(c, NULL, -2, &v);
        if (r == 1) within(data, len, &v);
        if (oc_cbor_skip(&c) != 0) break;
        within(data, len, &c);
    }
    return 0;
}
