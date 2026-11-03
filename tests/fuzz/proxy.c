/* Harness: the PROXY protocol v2 header (oc_proxy_v2_parse), the first bytes
 * of a connection from a trusted forwarder. Its answer is -1, 0, or a length
 * within the input; the address it writes is a string. */

#include "fuzz.h"
#include "proxyproto.h"

#include <string.h>

#define H "proxy"

int oc_fuzz_proxy(const uint8_t *data, size_t len) {
    char src[46];
    memset(src, 'x', sizeof src);
    long n = oc_proxy_v2_parse(data, len, src);
    if (n < -1 || n > (long)len) OC_FUZZ_BROKEN(H, "a length outside the input");
    if (n > 0 && memchr(src, '\0', sizeof src) == NULL) OC_FUZZ_BROKEN(H, "an address that is not a string");
    return 0;
}
