/* Harness: the central service's sign-in token (oc_jwt_verify) against a
 * fixed key, and the base64url decoding under it. A token signed by nobody
 * is never accepted; a decode fits its buffer. */

#include "fuzz.h"
#include "jwt.h"

#include <string.h>

#define H "jwt"

/* tests/test_jwt.c's vector key: a fuzzed token cannot carry its signature. */
static const char PEM[] =
    "-----BEGIN PUBLIC KEY-----\n"
    "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEDqHKx+q7OAQmDqj5y9NaFRRyMFoH\n"
    "tdCjHSfxAGUIA9+4UU024HTVfwxlAyEJ/AZuJbyJG5DrtbEcEXSVdX1U1A==\n"
    "-----END PUBLIC KEY-----\n";

int oc_fuzz_jwt(const uint8_t *data, size_t len) {
    static oc_jwt_claims c;
    if (oc_jwt_verify((const char *)data, len, PEM, sizeof PEM, "https://auth.openchime.io", "acme.example",
                      1800000000u, &c) == OC_JWT_OK)
        OC_FUZZ_BROKEN(H, "a token nobody signed was accepted");
    static uint8_t out[4096];
    long n = oc_base64url_decode((const char *)data, len, out, sizeof out);
    if (n < -1 || n > (long)sizeof out) OC_FUZZ_BROKEN(H, "a decode outside its buffer");
    return 0;
}
