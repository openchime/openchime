/* ES256 JWT verification — see jwt.h. jsmn reads the claims; mbedTLS does the
 * SHA-256 + ECDSA-P256 signature check against the pinned key. */

#include "jwt.h"

#include "jsmn.h"   /* vendored; this TU carries the implementation */

#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#include <stdio.h>
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

/* A relay token carries a dozen claims and a provider may add more; 64 tokens
 * is five claims short of a real one. */
#define JWT_MAX_TOKENS 160

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

typedef struct {
    const char *js;
    jsmntok_t   toks[JWT_MAX_TOKENS];
    int         ntok;
} jdoc;

static int jdoc_parse(jdoc *d, const char *js, size_t jlen) {
    jsmn_parser P;
    jsmn_init(&P);
    d->js = js;
    d->ntok = jsmn_parse(&P, js, jlen, d->toks, JWT_MAX_TOKENS);
    return d->ntok >= 1 && d->toks[0].type == JSMN_OBJECT;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static int hex4(const char *p, uint32_t *out) {
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        int h = hex_val(p[i]);
        if (h < 0) return 0;
        v = (v << 4) | (uint32_t)h;
    }
    *out = v;
    return 1;
}

/* JSON-unescape src[0..n) into dst. Returns the byte count, or -1 on a bad
 * escape, an embedded NUL, or a result that does not fit `cap` with its NUL:
 * a claim is never truncated, because a truncated subject is somebody else's. */
static long json_unescape(const char *src, size_t n, char *dst, size_t cap) {
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        uint32_t cp;
        char c = src[i];
        if (c != '\\') {
            cp = (unsigned char)c;
            if (cp == 0) return -1;
            if (o + 1 >= cap) return -1;
            dst[o++] = c;
            continue;
        }
        if (++i >= n) return -1;
        switch (src[i]) {
        case '"': cp = '"'; break;   case '\\': cp = '\\'; break;
        case '/': cp = '/'; break;   case 'b': cp = '\b'; break;
        case 'f': cp = '\f'; break;  case 'n': cp = '\n'; break;
        case 'r': cp = '\r'; break;  case 't': cp = '\t'; break;
        case 'u':
            if (i + 4 >= n) return -1;
            if (!hex4(src + i + 1, &cp)) return -1;
            i += 4;
            if (cp >= 0xD800 && cp <= 0xDBFF) {          /* surrogate pair */
                uint32_t lo;
                if (i + 6 >= n || src[i + 1] != '\\' || src[i + 2] != 'u') return -1;
                if (!hex4(src + i + 3, &lo) || lo < 0xDC00 || lo > 0xDFFF) return -1;
                cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                i += 6;
            } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
                return -1;
            }
            break;
        default: return -1;
        }
        if (cp == 0) return -1;
        uint8_t u[4]; size_t ul;
        if (cp < 0x80)        { u[0] = (uint8_t)cp; ul = 1; }
        else if (cp < 0x800)  { u[0] = (uint8_t)(0xC0 | (cp >> 6));
                                u[1] = (uint8_t)(0x80 | (cp & 0x3F)); ul = 2; }
        else if (cp < 0x10000){ u[0] = (uint8_t)(0xE0 | (cp >> 12));
                                u[1] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                                u[2] = (uint8_t)(0x80 | (cp & 0x3F)); ul = 3; }
        else                  { u[0] = (uint8_t)(0xF0 | (cp >> 18));
                                u[1] = (uint8_t)(0x80 | ((cp >> 12) & 0x3F));
                                u[2] = (uint8_t)(0x80 | ((cp >> 6) & 0x3F));
                                u[3] = (uint8_t)(0x80 | (cp & 0x3F)); ul = 4; }
        if (o + ul >= cap) return -1;
        memcpy(dst + o, u, ul);
        o += ul;
    }
    dst[o] = '\0';
    return (long)o;
}

/* The string value of top-level `key`, unescaped into dst. 1 found, 0 absent
 * (dst is ""), -1 present but not a string, malformed, or too long for `cap`. */
static int jdoc_str(const jdoc *d, const char *key, char *dst, size_t cap) {
    dst[0] = '\0';
    int v = find_value(d->js, d->toks, d->ntok, key);
    if (v < 0) return 0;
    if (d->toks[v].type != JSMN_STRING) return -1;
    size_t len = (size_t)(d->toks[v].end - d->toks[v].start);
    if (json_unescape(d->js + d->toks[v].start, len, dst, cap) < 0) { dst[0] = '\0'; return -1; }
    return 1;
}

/* Integer seconds. 1 found, 0 absent, -1 present but not a plain integer. */
static int jdoc_u64(const jdoc *d, const char *key, uint64_t *out) {
    int v = find_value(d->js, d->toks, d->ntok, key);
    if (v < 0) return 0;
    if (d->toks[v].type != JSMN_PRIMITIVE) return -1;
    int start = d->toks[v].start, end = d->toks[v].end;
    if (end - start < 1 || end - start > 18) return -1;
    uint64_t acc = 0;
    for (int i = start; i < end; i++) {
        char c = d->js[i];
        if (c < '0' || c > '9') return -1;
        acc = acc * 10u + (uint64_t)(c - '0');
    }
    *out = acc;
    return 1;
}

/* 1 only for the JSON literal true: "true", 1 and "yes" are not it. */
static int jdoc_true(const jdoc *d, const char *key) {
    int v = find_value(d->js, d->toks, d->ntok, key);
    if (v < 0 || d->toks[v].type != JSMN_PRIMITIVE) return 0;
    return d->toks[v].end - d->toks[v].start == 4 &&
           strncmp(d->js + d->toks[v].start, "true", 4) == 0;
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

/* --- the pinned key set, chosen by thumbprint ---------------------------- */

static const char PEM_BEGIN[] = "-----BEGIN PUBLIC KEY-----";
static const char PEM_END[]   = "-----END PUBLIC KEY-----";

static const char B64URL[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static size_t b64url_encode(const uint8_t *in, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        size_t rem = n - i;
        uint32_t v = (uint32_t)in[i] << 16;
        if (rem > 1) v |= (uint32_t)in[i + 1] << 8;
        if (rem > 2) v |= (uint32_t)in[i + 2];
        out[o++] = B64URL[(v >> 18) & 63];
        out[o++] = B64URL[(v >> 12) & 63];
        if (rem > 1) out[o++] = B64URL[(v >> 6) & 63];
        if (rem > 2) out[o++] = B64URL[v & 63];
    }
    out[o] = '\0';
    return o;
}

/* RFC 7638 over a parsed key: SHA-256 of the canonical JWK
 * {"crv":"P-256","kty":"EC","x":"..","y":".."}. The point comes off the end of
 * the SubjectPublicKeyInfo, whose last 65 bytes for P-256 are 04 || X || Y. */
static int thumbprint_of(mbedtls_pk_context *pk, char out[OC_JWT_THUMBPRINT_LEN + 1]) {
    if (mbedtls_pk_get_type(pk) != MBEDTLS_PK_ECKEY || mbedtls_pk_get_bitlen(pk) != 256)
        return -1;
    unsigned char der[160];
    int n = mbedtls_pk_write_pubkey_der(pk, der, sizeof der);   /* written at the END */
    if (n < 65) return -1;
    const unsigned char *pt = der + sizeof der - 65;
    if (pt[0] != 0x04) return -1;
    char x[44], y[44], jwk[160];
    b64url_encode(pt + 1, 32, x);
    b64url_encode(pt + 33, 32, y);
    int jl = snprintf(jwk, sizeof jwk,
                      "{\"crv\":\"P-256\",\"kty\":\"EC\",\"x\":\"%s\",\"y\":\"%s\"}", x, y);
    if (jl <= 0 || (size_t)jl >= sizeof jwk) return -1;
    uint8_t hash[32];
    if (mbedtls_sha256((const unsigned char *)jwk, (size_t)jl, hash, 0) != 0) return -1;
    return b64url_encode(hash, sizeof hash, out) == OC_JWT_THUMBPRINT_LEN ? 0 : -1;
}

int oc_jwt_key_thumbprint(const char *pubkey_pem, size_t pem_len,
                          char out[OC_JWT_THUMBPRINT_LEN + 1]) {
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = -1;
    if (pubkey_pem &&
        mbedtls_pk_parse_public_key(&pk, (const unsigned char *)pubkey_pem, pem_len) == 0)
        rc = thumbprint_of(&pk, out);
    mbedtls_pk_free(&pk);
    return rc;
}

/* Parse the pinned key whose thumbprint is `kid` into `pk`. 0 if found. A block
 * that does not parse, or is not P-256, is skipped rather than fatal: one bad
 * key in the set must not lock out the good one beside it. */
static int pick_key(const char *pems, const char *kid, mbedtls_pk_context *pk) {
    const char *p = pems;
    while ((p = strstr(p, PEM_BEGIN)) != NULL) {
        const char *e = strstr(p, PEM_END);
        if (!e) break;
        e += sizeof PEM_END - 1;
        size_t len = (size_t)(e - p);
        char one[1024];
        if (len + 2 <= sizeof one) {
            memcpy(one, p, len);
            one[len] = '\n';
            one[len + 1] = '\0';
            char tp[OC_JWT_THUMBPRINT_LEN + 1];
            mbedtls_pk_init(pk);
            if (mbedtls_pk_parse_public_key(pk, (const unsigned char *)one, len + 2) == 0 &&
                thumbprint_of(pk, tp) == 0 && strcmp(tp, kid) == 0)
                return 0;
            mbedtls_pk_free(pk);
        }
        p = e;
    }
    return -1;
}

int oc_jwt_nonce_matches(const char *nonce, const uint8_t *verifier, size_t verifier_len) {
    if (!nonce || !verifier || verifier_len == 0) return 0;
    uint8_t hash[32];
    char want[OC_JWT_THUMBPRINT_LEN + 1];
    if (mbedtls_sha256(verifier, verifier_len, hash, 0) != 0) return 0;
    b64url_encode(hash, sizeof hash, want);
    /* Constant time over the fixed length: the nonce is public, but there is no
     * reason to hand out a byte-at-a-time oracle on principle. */
    if (strlen(nonce) != OC_JWT_THUMBPRINT_LEN) return 0;
    unsigned diff = 0;
    for (size_t i = 0; i < OC_JWT_THUMBPRINT_LEN; i++)
        diff |= (unsigned)(nonce[i] ^ want[i]);
    return diff == 0;
}

/* --- verify ------------------------------------------------------------- */

#define OC_JWT_LEEWAY_SECS 60u   /* small clock-skew tolerance */

oc_jwt_result oc_jwt_verify(const char *token, size_t tlen,
                            const char *pubkeys_pem, size_t pem_len,
                            const char *want_iss, const char *want_aud,
                            uint64_t now_secs, oc_jwt_claims *out) {
    if (!token || !out || !pubkeys_pem || pem_len == 0) return OC_JWT_E_INTERNAL;
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

    jdoc doc;

    /* Header: pin alg == ES256, and read which pinned key signed it. */
    uint8_t hdr[512];
    long hn = oc_base64url_decode(token, h_len, hdr, sizeof hdr);
    if (hn < 0) return OC_JWT_E_FORMAT;
    char alg[16], kid[OC_JWT_MAX_SHORT];
    if (!jdoc_parse(&doc, (const char *)hdr, (size_t)hn)) return OC_JWT_E_FORMAT;
    if (jdoc_str(&doc, "alg", alg, sizeof alg) != 1) return OC_JWT_E_FORMAT;
    if (strcmp(alg, "ES256") != 0) return OC_JWT_E_ALG;
    if (jdoc_str(&doc, "kid", kid, sizeof kid) != 1) return OC_JWT_E_KEY;

    /* Signature: 64 raw bytes -> DER. */
    uint8_t sig[80];
    long sn = oc_base64url_decode(sig_b64, sig_b64_len, sig, sizeof sig);
    if (sn != 64) return OC_JWT_E_FORMAT;
    uint8_t der[80];
    size_t der_len = raw_ecdsa_to_der(sig, der, sizeof der);
    if (der_len == 0) return OC_JWT_E_INTERNAL;

    /* Hash the signing input and verify against the pinned key the kid names. */
    uint8_t hash[32];
    if (mbedtls_sha256((const unsigned char *)token, signing_len, hash, 0) != 0)
        return OC_JWT_E_INTERNAL;

    (void)pem_len;   /* the set is NUL-terminated; the length is the contract's */
    mbedtls_pk_context pk;
    if (pick_key(pubkeys_pem, kid, &pk) != 0) return OC_JWT_E_KEY;
    int bad = mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof hash, der, der_len);
    mbedtls_pk_free(&pk);
    if (bad != 0) return OC_JWT_E_SIGNATURE;

    /* Payload: read only after the signature is trusted. */
    uint8_t pay[8192];
    long pn = oc_base64url_decode(dot1 + 1, p_len, pay, sizeof pay);
    if (pn < 0) return OC_JWT_E_FORMAT;
    if (!jdoc_parse(&doc, (const char *)pay, (size_t)pn)) return OC_JWT_E_FORMAT;

    /* Required strings must be present; every string must fit and unescape. */
    if (jdoc_str(&doc, "iss",   out->iss,   sizeof out->iss)   != 1) return OC_JWT_E_CLAIMS;
    if (jdoc_str(&doc, "aud",   out->aud,   sizeof out->aud)   != 1) return OC_JWT_E_CLAIMS;
    if (jdoc_str(&doc, "sub",   out->sub,   sizeof out->sub)   != 1) return OC_JWT_E_CLAIMS;
    if (jdoc_str(&doc, "nonce", out->nonce, sizeof out->nonce) != 1) return OC_JWT_E_CLAIMS;
    if (jdoc_str(&doc, "jti",   out->jti,   sizeof out->jti)   != 1) return OC_JWT_E_CLAIMS;
    if (jdoc_str(&doc, "email",  out->email,  sizeof out->email)  < 0) return OC_JWT_E_CLAIMS;
    if (jdoc_str(&doc, "name",   out->name,   sizeof out->name)   < 0) return OC_JWT_E_CLAIMS;
    if (jdoc_str(&doc, "idp",    out->idp,    sizeof out->idp)    < 0) return OC_JWT_E_CLAIMS;
    if (jdoc_str(&doc, "tenant", out->tenant, sizeof out->tenant) < 0) return OC_JWT_E_CLAIMS;
    if (!out->sub[0] || !out->jti[0] || !out->nonce[0]) return OC_JWT_E_CLAIMS;
    out->email_verified = jdoc_true(&doc, "email_verified");
    if (jdoc_u64(&doc, "exp", &out->exp) != 1) return OC_JWT_E_CLAIMS;
    if (jdoc_u64(&doc, "iat", &out->iat) != 1) return OC_JWT_E_CLAIMS;
    if (jdoc_u64(&doc, "nbf", &out->nbf) < 0) return OC_JWT_E_CLAIMS;

    if (want_iss && strcmp(out->iss, want_iss) != 0) return OC_JWT_E_CLAIMS;
    if (want_aud && strcmp(out->aud, want_aud) != 0) return OC_JWT_E_CLAIMS;
    /* A short life is part of the contract, and what bounds the caller's jti set. */
    if (out->exp < out->iat || out->exp - out->iat > OC_JWT_MAX_LIFETIME_SECS)
        return OC_JWT_E_CLAIMS;
    if (now_secs > out->exp + OC_JWT_LEEWAY_SECS) return OC_JWT_E_EXPIRED;
    if (out->iat > now_secs + OC_JWT_LEEWAY_SECS) return OC_JWT_E_EXPIRED;
    if (out->nbf > now_secs + OC_JWT_LEEWAY_SECS) return OC_JWT_E_EXPIRED;

    return OC_JWT_OK;
}
