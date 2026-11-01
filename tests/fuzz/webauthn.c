/* Harness: a passkey's registration and its sign-in (daemon/webauthn.c), the
 * input split into what the browser sends. A credential's id and key fit
 * their bounds; an answer signed by nobody is never accepted. */

#include "fuzz.h"
#include "webauthn.h"

#include <stdlib.h>

#define H "webauthn"

int oc_fuzz_webauthn(const uint8_t *data, size_t len) {
    char *buf = malloc(len + 8);
    if (!buf) return 0;
    const char *p[5];
    size_t pl[5];
    oc_fuzz_split(data, len, 2, buf, p, pl);
    oc_wa_cred cred;
    if (oc_webauthn_register((const uint8_t *)p[0], pl[0], (const uint8_t *)p[1], pl[1],
                             "Y2hhbGxlbmdl", "https://acme.example", "acme.example", &cred) == OC_WA_OK &&
        (cred.cred_len > OC_WA_CRED_MAX || cred.cose_len > OC_WA_COSE_MAX))
        OC_FUZZ_BROKEN(H, "a credential over its bounds");
    oc_fuzz_split(data, len, 4, buf, p, pl);
    uint32_t count = 0;
    if (oc_webauthn_assert((const uint8_t *)p[0], pl[0], (const uint8_t *)p[1], pl[1], (const uint8_t *)p[2], pl[2],
                           (const uint8_t *)p[3], pl[3], 0, "Y2hhbGxlbmdl", "https://acme.example", "acme.example",
                           &count) == OC_WA_OK && pl[3] == 0)
        OC_FUZZ_BROKEN(H, "a sign-in accepted without a key");
    free(buf);
    return 0;
}
