/* A provider's ID token -- see idtoken.h. */

#include "idtoken.h"
#include "json.h"
#include "jwt.h"   /* oc_base64url_decode */

#include <mbedtls/ecdsa.h>
#include <mbedtls/pk.h>
#include <mbedtls/rsa.h>
#include <mbedtls/sha256.h>

#include <stdlib.h>
#include <string.h>

typedef enum { ALG_RS256, ALG_PS256, ES256_ } alg_t;

/* A base64url segment into a fresh buffer (len in *n), or NULL. */
static uint8_t *seg(const char *s, size_t len, size_t *n) {
    uint8_t *b = malloc(len + 4);
    if (!b) return NULL;
    long l = oc_base64url_decode(s, len, b, len + 4);
    if (l < 0) { free(b); return NULL; }
    *n = (size_t)l;
    b[l] = 0;
    return b;
}

/* A base64url member of a JWK into `out` (at most `cap`). The length, or -1. */
static long jwk_bytes(const oc_json *d, int obj, const char *key, uint8_t *out, size_t cap) {
    char tmp[1100];
    if (oc_json_get_str(d, obj, key, tmp, sizeof tmp) != 0) return -1;
    return oc_base64url_decode(tmp, strlen(tmp), out, cap);
}

/* The key in the set the header asks for, as an mbedTLS key. */
static oc_idt_result jwk_key(const char *jwks, size_t jl, const char *kid, alg_t alg, mbedtls_pk_context *pk) {
    oc_json d;
    if (oc_json_parse(&d, jwks, jl) != 0) return OC_IDT_KEY;
    int keys = oc_json_get(&d, 0, "keys");
    oc_idt_result r = OC_IDT_KEY;
    if (keys < 0 || d.t[keys].type != JSMN_ARRAY) { oc_json_free(&d); return r; }
    const char *want_kty = alg == ES256_ ? "EC" : "RSA";
    int i = keys + 1, chosen = -1, of_type = 0, only = -1;
    for (int k = 0; k < d.t[keys].size && i < d.n; k++) {
        char kty[8], kk[256], use[8];
        if (d.t[i].type == JSMN_OBJECT && oc_json_get_str(&d, i, "kty", kty, sizeof kty) == 0 &&
            strcmp(kty, want_kty) == 0 &&
            (oc_json_get_str(&d, i, "use", use, sizeof use) != 0 || strcmp(use, "sig") == 0)) {
            of_type++;
            only = i;
            if (kid[0] && oc_json_get_str(&d, i, "kid", kk, sizeof kk) == 0 && strcmp(kk, kid) == 0) chosen = i;
        }
        i = oc_json_skip(&d, i);
    }
    if (chosen < 0 && !kid[0] && of_type == 1) chosen = only;   /* no kid: the one key there is */
    if (chosen >= 0) {
        if (alg == ES256_) {
            char crv[16];
            uint8_t pt[65];
            static const uint8_t SPKI[] = { 0x30, 0x59, 0x30, 0x13, 0x06, 0x07, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x02,
                                            0x01, 0x06, 0x08, 0x2a, 0x86, 0x48, 0xce, 0x3d, 0x03, 0x01, 0x07, 0x03,
                                            0x42, 0x00, 0x04 };
            uint8_t der[sizeof SPKI + 64];
            if (oc_json_get_str(&d, chosen, "crv", crv, sizeof crv) == 0 && strcmp(crv, "P-256") == 0 &&
                jwk_bytes(&d, chosen, "x", pt, 32) == 32 && jwk_bytes(&d, chosen, "y", pt + 32, 32) == 32) {
                memcpy(der, SPKI, sizeof SPKI);
                memcpy(der + sizeof SPKI, pt, 64);
                if (mbedtls_pk_parse_public_key(pk, der, sizeof der) == 0) r = OC_IDT_OK;
            }
        } else {
            uint8_t n[512], e[8];
            long nl = jwk_bytes(&d, chosen, "n", n, sizeof n), el = jwk_bytes(&d, chosen, "e", e, sizeof e);
            if (nl >= 256 && el > 0 && mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA)) == 0) {
                mbedtls_rsa_context *rsa = mbedtls_pk_rsa(*pk);
                if (mbedtls_rsa_import_raw(rsa, n, (size_t)nl, NULL, 0, NULL, 0, NULL, 0, e, (size_t)el) == 0 &&
                    mbedtls_rsa_complete(rsa) == 0 && mbedtls_rsa_check_pubkey(rsa) == 0) r = OC_IDT_OK;
            }
        }
    }
    oc_json_free(&d);
    return r;
}

/* JWS ES256's raw r||s as the DER mbedTLS checks. Its length, or 0. */
static size_t es_der(const uint8_t rs[64], uint8_t *der, size_t cap) {
    uint8_t buf[72];
    size_t o = 0;
    for (int h = 0; h < 2; h++) {
        const uint8_t *v = rs + 32 * h;
        size_t skip = 0;
        while (skip < 31 && v[skip] == 0) skip++;
        size_t len = 32 - skip, pad = (v[skip] & 0x80) ? 1 : 0;
        buf[o++] = 0x02;
        buf[o++] = (uint8_t)(len + pad);
        if (pad) buf[o++] = 0;
        memcpy(buf + o, v + skip, len);
        o += len;
    }
    if (o + 2 > cap) return 0;
    der[0] = 0x30; der[1] = (uint8_t)o;
    memcpy(der + 2, buf, o);
    return o + 2;
}

oc_idt_result oc_idtoken_verify(const char *token, size_t tlen, const char *jwks, size_t jwks_len,
                                const char *issuer, const char *client_id, const char *nonce,
                                uint64_t now_s, oc_idt_claims *out) {
    memset(out, 0, sizeof *out);
    if (!token || !jwks || !issuer || !client_id || !nonce) return OC_IDT_FORMAT;
    const char *d1 = memchr(token, '.', tlen);
    const char *d2 = d1 ? memchr(d1 + 1, '.', (size_t)(token + tlen - d1 - 1)) : NULL;
    if (!d1 || !d2 || memchr(d2 + 1, '.', (size_t)(token + tlen - d2 - 1))) return OC_IDT_FORMAT;
    size_t hl = 0, pl = 0, sl = 0;
    uint8_t *h = seg(token, (size_t)(d1 - token), &hl);
    uint8_t *p = seg(d1 + 1, (size_t)(d2 - d1 - 1), &pl);
    uint8_t *s = seg(d2 + 1, (size_t)(token + tlen - d2 - 1), &sl);
    oc_idt_result r = OC_IDT_FORMAT;
    oc_json hd = { 0 }, pd = { 0 };
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    if (!h || !p || !s || oc_json_parse(&hd, (const char *)h, hl) != 0 || hd.t[0].type != JSMN_OBJECT) goto done;
    char algs[16], kid[256] = "";
    if (oc_json_get_str(&hd, 0, "alg", algs, sizeof algs) != 0) goto done;
    alg_t alg;
    if (!strcmp(algs, "RS256")) alg = ALG_RS256;
    else if (!strcmp(algs, "PS256")) alg = ALG_PS256;
    else if (!strcmp(algs, "ES256")) alg = ES256_;
    else { r = OC_IDT_ALG; goto done; }
    if (oc_json_get(&hd, 0, "kid") >= 0 && oc_json_get_str(&hd, 0, "kid", kid, sizeof kid) != 0) goto done;
    if ((r = jwk_key(jwks, jwks_len, kid, alg, &pk)) != OC_IDT_OK) goto done;

    /* The signature, over the header and payload as sent. */
    uint8_t hash[32];
    if (mbedtls_sha256((const unsigned char *)token, (size_t)(d2 - token), hash, 0) != 0) { r = OC_IDT_FORMAT; goto done; }
    int ok = 0;
    if (alg == ES256_) {
        uint8_t der[80];
        size_t dl = sl == 64 ? es_der(s, der, sizeof der) : 0;
        ok = dl && mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof hash, der, dl) == 0;
    } else if (alg == ALG_RS256) {
        ok = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof hash, s, sl) == 0;
    } else {
        mbedtls_pk_rsassa_pss_options o = { MBEDTLS_MD_SHA256, MBEDTLS_RSA_SALT_LEN_ANY };
        ok = mbedtls_pk_verify_ext(MBEDTLS_PK_RSASSA_PSS, &o, &pk, MBEDTLS_MD_SHA256, hash, sizeof hash, s, sl) == 0;
    }
    if (!ok) { r = OC_IDT_SIGNATURE; goto done; }

    /* The claims. */
    r = OC_IDT_FORMAT;
    if (oc_json_parse(&pd, (const char *)p, pl) != 0 || pd.t[0].type != JSMN_OBJECT) goto done;
    char iss[OC_IDT_FIELD], azp[OC_IDT_FIELD] = "", tnonce[OC_IDT_FIELD];
    r = OC_IDT_CLAIMS;
    if (oc_json_get_str(&pd, 0, "iss", iss, sizeof iss) != 0 || strcmp(iss, issuer) != 0) goto done;
    int aud = oc_json_get(&pd, 0, "aud"), auds = 0, mine = 0;
    char a[OC_IDT_FIELD];
    if (aud >= 0 && pd.t[aud].type == JSMN_STRING) {
        auds = 1;
        mine = oc_json_str(&pd, aud, a, sizeof a) == 0 && strcmp(a, client_id) == 0;
    } else if (aud >= 0 && pd.t[aud].type == JSMN_ARRAY) {
        int i = aud + 1;
        for (int k = 0; k < pd.t[aud].size && i < pd.n; k++) {
            auds++;
            if (oc_json_str(&pd, i, a, sizeof a) == 0 && strcmp(a, client_id) == 0) mine = 1;
            i = oc_json_skip(&pd, i);
        }
    }
    if (!mine) goto done;
    if (oc_json_get(&pd, 0, "azp") >= 0) {
        if (oc_json_get_str(&pd, 0, "azp", azp, sizeof azp) != 0 || strcmp(azp, client_id) != 0) goto done;
    } else if (auds > 1) goto done;   /* several audiences: the party it was for must say */
    if (oc_json_get_str(&pd, 0, "nonce", tnonce, sizeof tnonce) != 0 || strcmp(tnonce, nonce) != 0) goto done;
    if (oc_json_get_str(&pd, 0, "sub", out->sub, sizeof out->sub) != 0 || !out->sub[0]) goto done;
    /* Optional strings: absent is empty, but present and unreadable is refused. */
    static const char *const OPT[] = { "oid", "email", "name", "hd", "tid" };
    char *dst[] = { out->oid, out->email, out->name, out->hd, out->tid };
    for (int k = 0; k < 5; k++)
        if (oc_json_get(&pd, 0, OPT[k]) >= 0 && oc_json_get_str(&pd, 0, OPT[k], dst[k], OC_IDT_FIELD) != 0) goto done;
    out->email_verified = oc_json_true(&pd, oc_json_get(&pd, 0, "email_verified"));
    out->xms_edov = oc_json_true(&pd, oc_json_get(&pd, 0, "xms_edov"));
    uint64_t exp = 0, nbf = 0, iat = 0;
    if (oc_json_u64(&pd, oc_json_get(&pd, 0, "exp"), &exp) != 0) goto done;
    r = OC_IDT_TIME;
    if (now_s >= exp + OC_IDT_LEEWAY_S) goto done;
    if (oc_json_get(&pd, 0, "nbf") >= 0 && (oc_json_u64(&pd, oc_json_get(&pd, 0, "nbf"), &nbf) != 0 ||
                                            nbf > now_s + OC_IDT_LEEWAY_S)) goto done;
    if (oc_json_get(&pd, 0, "iat") >= 0 && (oc_json_u64(&pd, oc_json_get(&pd, 0, "iat"), &iat) != 0 ||
                                            iat > now_s + OC_IDT_LEEWAY_S)) goto done;
    out->exp = exp;
    r = OC_IDT_OK;
done:
    if (r != OC_IDT_OK) memset(out, 0, sizeof *out);
    oc_json_free(&hd);
    oc_json_free(&pd);
    mbedtls_pk_free(&pk);
    free(h); free(p); free(s);
    return r;
}
