/* A minimal CBOR reader -- see cbor.h. */

#include "cbor.h"

#include <string.h>

int oc_cbor_head(oc_cbor *c, oc_cbor_major *major, uint64_t *arg) {
    if (!c->p || c->p >= c->end) return -1;
    uint8_t b = *c->p++;
    *major = (oc_cbor_major)(b >> 5);
    uint8_t ai = b & 31;
    if (ai < 24) { *arg = ai; return 0; }
    if (ai > 27) return -1;                      /* indefinite or reserved */
    size_t n = (size_t)1 << (ai - 24);
    if ((size_t)(c->end - c->p) < n) return -1;
    uint64_t v = 0;
    for (size_t i = 0; i < n; i++) v = (v << 8) | *c->p++;
    *arg = v;
    return 0;
}

static int skip_depth(oc_cbor *c, int depth) {
    if (depth > OC_CBOR_MAX_DEPTH) return -1;
    oc_cbor_major m; uint64_t a;
    if (oc_cbor_head(c, &m, &a) != 0) return -1;
    switch (m) {
    case OC_CBOR_UINT: case OC_CBOR_NINT: case OC_CBOR_SIMPLE: return 0;
    case OC_CBOR_BYTES: case OC_CBOR_TEXT:
        if ((uint64_t)(c->end - c->p) < a) return -1;
        c->p += a;
        return 0;
    case OC_CBOR_ARRAY:
        for (uint64_t i = 0; i < a; i++) if (skip_depth(c, depth + 1) != 0) return -1;
        return 0;
    case OC_CBOR_MAP:
        for (uint64_t i = 0; i < a; i++)
            if (skip_depth(c, depth + 1) != 0 || skip_depth(c, depth + 1) != 0) return -1;
        return 0;
    case OC_CBOR_TAG:
        return skip_depth(c, depth + 1);
    }
    return -1;
}

int oc_cbor_skip(oc_cbor *c) { return skip_depth(c, 0); }

int oc_cbor_string(oc_cbor *c, oc_cbor_major want, const uint8_t **ptr, size_t *len) {
    oc_cbor_major m; uint64_t a;
    if (oc_cbor_head(c, &m, &a) != 0 || m != want || (uint64_t)(c->end - c->p) < a) return -1;
    *ptr = c->p; *len = (size_t)a;
    c->p += a;
    return 0;
}

int oc_cbor_int(oc_cbor *c, int64_t *v) {
    oc_cbor_major m; uint64_t a;
    if (oc_cbor_head(c, &m, &a) != 0 || a > INT64_MAX) return -1;
    if (m == OC_CBOR_UINT) { *v = (int64_t)a; return 0; }
    if (m == OC_CBOR_NINT) { *v = -1 - (int64_t)a; return 0; }
    return -1;
}

int oc_cbor_map_get(oc_cbor c, const char *key, int64_t ikey, oc_cbor *out) {
    oc_cbor_major m; uint64_t n;
    if (oc_cbor_head(&c, &m, &n) != 0 || m != OC_CBOR_MAP) return -1;
    for (uint64_t i = 0; i < n; i++) {
        oc_cbor k = c;
        int hit = 0;
        if (key) {
            const uint8_t *s; size_t l;
            oc_cbor t = c;
            if (oc_cbor_string(&t, OC_CBOR_TEXT, &s, &l) == 0) hit = l == strlen(key) && memcmp(s, key, l) == 0;
        } else {
            int64_t v;
            oc_cbor t = c;
            if (oc_cbor_int(&t, &v) == 0) hit = v == ikey;
        }
        if (oc_cbor_skip(&k) != 0) return -1;     /* past the key */
        if (hit) { *out = k; return 1; }
        if (oc_cbor_skip(&k) != 0) return -1;     /* past its value */
        c = k;
    }
    return 0;
}
