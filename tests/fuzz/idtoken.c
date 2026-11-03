/* Harness: a provider's ID token checked against its key set
 * (oc_idtoken_verify), the input split into the token and the JWKS. Neither
 * may crash the reader, and claims read are strings. */

#include "fuzz.h"
#include "idtoken.h"

#include <stdlib.h>
#include <string.h>

#define H "idtoken"

int oc_fuzz_idtoken(const uint8_t *data, size_t len) {
    char *buf = malloc(len + 4);
    if (!buf) return 0;
    const char *p[2];
    size_t pl[2];
    oc_fuzz_split(data, len, 2, buf, p, pl);
    static oc_idt_claims c;
    if (oc_idtoken_verify(p[0], pl[0], p[1], pl[1], "https://accounts.google.com", "client-1", "n-1",
                          1800000000u, &c) == OC_IDT_OK &&
        (memchr(c.sub, '\0', sizeof c.sub) == NULL || memchr(c.email, '\0', sizeof c.email) == NULL))
        OC_FUZZ_BROKEN(H, "a claim that is not a string");
    free(buf);
    return 0;
}
