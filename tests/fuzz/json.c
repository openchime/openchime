/* Harness: the JSON reader the sign-in paths use (daemon/json.c). The input
 * tokenized, then every token read as each type is read at sign-in, and
 * unescaped whole. A string read is a string that fits; skipping a token
 * moves forward. */

#include "fuzz.h"
#include "json.h"

#include <string.h>

#define H "json"

int oc_fuzz_json(const uint8_t *data, size_t len) {
    static char out[512];
    long un = oc_json_unescape((const char *)data, len, out, sizeof out);
    if (un < -1 || un > (long)sizeof out) OC_FUZZ_BROKEN(H, "oc_json_unescape answered outside its range");
    oc_json d;
    if (oc_json_parse(&d, (const char *)data, len) != 0) return 0;
    for (int i = 0; i < d.n; i++) {
        if (oc_json_str(&d, i, out, sizeof out) == 0 && memchr(out, '\0', sizeof out) == NULL)
            OC_FUZZ_BROKEN(H, "a string read that is not terminated");
        uint64_t v;
        (void)oc_json_u64(&d, i, &v);
        (void)oc_json_true(&d, i);
        int nx = oc_json_skip(&d, i);
        if (nx <= i || nx > d.n) OC_FUZZ_BROKEN(H, "oc_json_skip did not move forward within the document");
        (void)oc_json_get(&d, i, "sub");
        (void)oc_json_get_str(&d, i, "email", out, sizeof out);
    }
    oc_json_free(&d);
    return 0;
}
