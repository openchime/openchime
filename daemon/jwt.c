/* ES256 JWT verification — see jwt.h. jsmn reads the claims; mbedTLS does the
 * SHA-256 + ECDSA-P256 signature check against the pinned key. */

#include "jwt.h"

#include "jsmn.h"   /* vendored; this TU carries the implementation */

#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include <string.h>

/* --- base64url ---------------------------------------------------------- */

static int b64url_val(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

long oc_base64url_decode(const char *in, size_t inlen, uint8_t *out, size_t out_cap) {
    size_t o = 0;
    uint32_t buf = 0;
    int bits = 0;
    for (size_t i = 0; i < inlen; i++) {
        int v = b64url_val(in[i]);
        if (v < 0) return -1;
        buf = (buf << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (o >= out_cap) return -1;
            out[o++] = (uint8_t)((buf >> bits) & 0xFFu);
        }
    }
    return (long)o;
}

/* --- Minimal JSON claim reads (flat top-level object) ------------------- */

#define JWT_MAX_TOKENS 64

static int tok_eq(const char *js, const jsmntok_t *t, const char *s) {
    int len = t->end - t->start;
    return t->type == JSMN_STRING && len == (int)strlen(s) &&
           strncmp(js + t->start, s, (size_t)len) == 0;
}

/* Token count spanned by the value subtree starting at index i. */
static int skip_tok(const jsmntok_t *t, int i, int ntok) {
    if (i >= ntok) return i;
    if (t[i].type == JSMN_STRING || t[i].type == JSMN_PRIMITIVE) return i + 1;
    int j = i + 1;
    if (t[i].type == JSMN_OBJECT) {
        for (int c = 0; c < t[i].size; c++) {
            j = skip_tok(t, j, ntok);   /* key   */
            j = skip_tok(t, j, ntok);   /* value */
        }
    } else if (t[i].type == JSMN_ARRAY) {
        for (int c = 0; c < t[i].size; c++) j = skip_tok(t, j, ntok);
    }
    return j;
}

/* Index of the value token for top-level key `key`, or -1 (subtree-aware). */
static int find_value(const char *js, const jsmntok_t *t, int ntok, const char *key) {
    if (ntok < 1 || t[0].type != JSMN_OBJECT) return -1;
    int i = 1;
    for (int c = 0; c < t[0].size && i + 1 < ntok; c++) {
        int vidx = i + 1;
        if (tok_eq(js, &t[i], key)) return vidx;
        i = skip_tok(t, vidx, ntok);
    }
    return -1;
}

static int parse_json(const char *js, size_t jlen, jsmntok_t *toks) {
    jsmn_parser P;
    jsmn_init(&P);
    return jsmn_parse(&P, js, jlen, toks, JWT_MAX_TOKENS);
}

/* Copy the string value of top-level `key` into dst (raw bytes, not
 * un-escaped — the claims we match on are plain ASCII). 1 if found. */
static int json_get_str(const char *js, size_t jlen, const char *key,
                        char *dst, size_t cap) {
    jsmntok_t toks[JWT_MAX_TOKENS];
    int n = parse_json(js, jlen, toks);
    if (n < 0) return 0;
    int v = find_value(js, toks, n, key);
    if (v < 0 || toks[v].type != JSMN_STRING) return 0;
    size_t len = (size_t)(toks[v].end - toks[v].start);
    if (len >= cap) len = cap - 1;
    memcpy(dst, js + toks[v].start, len);
    dst[len] = '\0';
    return 1;
}

static int json_get_u64(const char *js, size_t jlen, const char *key, uint64_t *out) {
    jsmntok_t toks[JWT_MAX_TOKENS];
    int n = parse_json(js, jlen, toks);
    if (n < 0) return 0;
    int v = find_value(js, toks, n, key);
    if (v < 0 || toks[v].type != JSMN_PRIMITIVE) return 0;
    uint64_t acc = 0;
    for (int i = toks[v].start; i < toks[v].end; i++) {
        char c = js[i];
        if (c < '0' || c > '9') return 0;   /* integer seconds only */
        acc = acc * 10u + (uint64_t)(c - '0');
    }
    *out = acc;
    return 1;
}

/* --- raw ECDSA (r||s) -> ASN.1 DER SEQUENCE{INTEGER,INTEGER} ------------- */

static size_t enc_int(uint8_t *out, const uint8_t *v, size_t vlen) {
    size_t i = 0;
    while (i < vlen - 1 && v[i] == 0) i++;    /* strip leading zero bytes */
    size_t n = vlen - i;
    int pad = (v[i] & 0x80) ? 1 : 0;          /* keep INTEGER non-negative */
    out[0] = 0x02;
    out[1] = (uint8_t)(n + (size_t)pad);
    size_t o = 2;
    if (pad) out[o++] = 0x00;
    memcpy(out + o, v + i, n);
    return o + n;
}

/* 64-byte raw signature -> DER. Returns DER length (<= 72), or 0 on overflow. */
static size_t raw_ecdsa_to_der(const uint8_t sig[64], uint8_t *der, size_t cap) {
    uint8_t rint[40], sint[40];
    size_t rl = enc_int(rint, sig, 32);
    size_t sl = enc_int(sint, sig + 32, 32);
    size_t body = rl + sl;
    if (body > 127 || body + 2 > cap) return 0;   /* fits single-byte length */
    der[0] = 0x30;
    der[1] = (uint8_t)body;
    memcpy(der + 2, rint, rl);
    memcpy(der + 2 + rl, sint, sl);
    return body + 2;
}

/* --- verify ------------------------------------------------------------- */

#define OC_JWT_LEEWAY_SECS 60u   /* small clock-skew tolerance on exp */

oc_jwt_result oc_jwt_verify(const char *token, size_t tlen,
                            const char *pubkey_pem, size_t pem_len,
                            const char *want_iss, const char *want_aud,
                            uint64_t now_secs, oc_jwt_claims *out) {
    if (!token || !out) return OC_JWT_E_INTERNAL;
    memset(out, 0, sizeof *out);

    /* Split "header.payload.signature". */
    const char *dot1 = memchr(token, '.', tlen);
    if (!dot1) return OC_JWT_E_FORMAT;
    size_t h_len = (size_t)(dot1 - token);
    size_t rest = tlen - h_len - 1;
    const char *dot2 = memchr(dot1 + 1, '.', rest);
    if (!dot2) return OC_JWT_E_FORMAT;
    size_t p_len = (size_t)(dot2 - (dot1 + 1));
    const char *sig_b64 = dot2 + 1;
    size_t sig_b64_len = tlen - (size_t)(sig_b64 - token);
    size_t signing_len = (size_t)(dot2 - token);   /* "header.payload" */

    /* Header: pin alg == ES256. */
    uint8_t hdr[512];
    long hn = oc_base64url_decode(token, h_len, hdr, sizeof hdr);
    if (hn < 0) return OC_JWT_E_FORMAT;
    char alg[16];
    if (!json_get_str((const char *)hdr, (size_t)hn, "alg", alg, sizeof alg))
        return OC_JWT_E_FORMAT;
    if (strcmp(alg, "ES256") != 0) return OC_JWT_E_ALG;

    /* Payload: extract claims. */
    uint8_t pay[4096];
    long pn = oc_base64url_decode(dot1 + 1, p_len, pay, sizeof pay);
    if (pn < 0) return OC_JWT_E_FORMAT;
    const char *pj = (const char *)pay;
    size_t pjl = (size_t)pn;
    json_get_str(pj, pjl, "sub",   out->sub,   sizeof out->sub);
    json_get_str(pj, pjl, "email", out->email, sizeof out->email);
    json_get_str(pj, pjl, "name",  out->name,  sizeof out->name);
    json_get_str(pj, pjl, "iss",   out->iss,   sizeof out->iss);
    json_get_str(pj, pjl, "aud",   out->aud,   sizeof out->aud);
    json_get_u64(pj, pjl, "exp", &out->exp);
    json_get_u64(pj, pjl, "iat", &out->iat);

    /* Signature: 64 raw bytes -> DER. */
    uint8_t sig[80];
    long sn = oc_base64url_decode(sig_b64, sig_b64_len, sig, sizeof sig);
    if (sn != 64) return OC_JWT_E_FORMAT;
    uint8_t der[80];
    size_t der_len = raw_ecdsa_to_der(sig, der, sizeof der);
    if (der_len == 0) return OC_JWT_E_INTERNAL;

    /* Hash the signing input and verify against the pinned key. */
    uint8_t hash[32];
    if (mbedtls_sha256((const unsigned char *)token, signing_len, hash, 0) != 0)
        return OC_JWT_E_INTERNAL;

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    oc_jwt_result rc = OC_JWT_OK;
    if (mbedtls_pk_parse_public_key(&pk, (const unsigned char *)pubkey_pem, pem_len) != 0) {
        rc = OC_JWT_E_KEY;
    } else if (mbedtls_pk_get_type(&pk) != MBEDTLS_PK_ECKEY ||
               mbedtls_pk_get_bitlen(&pk) != 256) {
        rc = OC_JWT_E_KEY;   /* pin the curve size alongside alg=ES256 */
    } else if (mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof hash,
                                 der, der_len) != 0) {
        rc = OC_JWT_E_SIGNATURE;
    }
    mbedtls_pk_free(&pk);
    if (rc != OC_JWT_OK) return rc;

    /* Claim checks (after the signature is trusted). */
    if (want_iss && strcmp(out->iss, want_iss) != 0) return OC_JWT_E_CLAIMS;
    if (want_aud && strcmp(out->aud, want_aud) != 0) return OC_JWT_E_CLAIMS;
    if (out->exp != 0 && now_secs > out->exp + OC_JWT_LEEWAY_SECS) return OC_JWT_E_EXPIRED;

    return OC_JWT_OK;
}
