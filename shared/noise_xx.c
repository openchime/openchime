/* Noise_XX_25519_AESGCM_SHA256: see noise_xx.h. Section numbers are those of the
 * Noise Protocol Framework, revision 34. */

#include "noise_xx.h"

#include "e2e_hpke.h"

#include <string.h>

#include <mbedtls/gcm.h>
#include <mbedtls/md.h>

static const char PROTOCOL_NAME[] = "Noise_XX_25519_AESGCM_SHA256";

/* --- the hash and HKDF of §4.3 -------------------------------------------- */

static int hash2(const uint8_t *a, size_t al, const uint8_t *b, size_t bl, uint8_t out[OC_NOISE_HASHLEN]) {
    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    int rc = mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 0) == 0
          && mbedtls_md_starts(&md) == 0
          && mbedtls_md_update(&md, a, al) == 0
          && (bl == 0 || mbedtls_md_update(&md, b, bl) == 0)
          && mbedtls_md_finish(&md, out) == 0 ? 0 : -1;
    mbedtls_md_free(&md);
    return rc;
}

static int hmac(const uint8_t key[OC_NOISE_HASHLEN], const uint8_t *a, size_t al,
                const uint8_t *b, size_t bl, uint8_t out[OC_NOISE_HASHLEN]) {
    mbedtls_md_context_t md;
    mbedtls_md_init(&md);
    int rc = mbedtls_md_setup(&md, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1) == 0
          && mbedtls_md_hmac_starts(&md, key, OC_NOISE_HASHLEN) == 0
          && (al == 0 || mbedtls_md_hmac_update(&md, a, al) == 0)
          && (bl == 0 || mbedtls_md_hmac_update(&md, b, bl) == 0)
          && mbedtls_md_hmac_finish(&md, out) == 0 ? 0 : -1;
    mbedtls_md_free(&md);
    return rc;
}

/* HKDF(chaining_key, input_key_material, 2): the two outputs Noise uses. */
static int hkdf2(const uint8_t ck[OC_NOISE_HASHLEN], const uint8_t *ikm, size_t ikm_len,
                 uint8_t o1[OC_NOISE_HASHLEN], uint8_t o2[OC_NOISE_HASHLEN]) {
    uint8_t temp[OC_NOISE_HASHLEN];
    static const uint8_t one = 0x01, two = 0x02;
    int rc = hmac(ck, ikm, ikm_len, NULL, 0, temp) == 0
          && hmac(temp, &one, 1, NULL, 0, o1) == 0
          && hmac(temp, o1, OC_NOISE_HASHLEN, &two, 1, o2) == 0 ? 0 : -1;
    oc_e2e_wipe(temp, sizeof temp);
    return rc;
}

/* --- CipherState (§5.1): AES-256-GCM, nonce = 32 zero bits || n big-endian -- */

static void nonce_of(uint64_t n, uint8_t iv[12]) {
    memset(iv, 0, 4);
    for (int i = 0; i < 8; i++) iv[4 + i] = (uint8_t)(n >> (56 - 8 * i));
}

static int gcm(int encrypt, const uint8_t k[32], uint64_t n, const uint8_t *ad, size_t ad_len,
               const uint8_t *in, size_t in_len, uint8_t *out) {
    mbedtls_gcm_context g;
    uint8_t iv[12];
    int rc;
    nonce_of(n, iv);
    mbedtls_gcm_init(&g);
    rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, k, 256);
    if (rc == 0 && encrypt) {
        rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, in_len, iv, sizeof iv, ad, ad_len,
                                       in, out, OC_NOISE_TAGLEN, out + in_len);
    } else if (rc == 0) {
        if (in_len < OC_NOISE_TAGLEN) rc = -1;
        else rc = mbedtls_gcm_auth_decrypt(&g, in_len - OC_NOISE_TAGLEN, iv, sizeof iv, ad, ad_len,
                                           in + in_len - OC_NOISE_TAGLEN, OC_NOISE_TAGLEN, in, out);
        if (rc != 0 && in_len >= OC_NOISE_TAGLEN) oc_e2e_wipe(out, in_len - OC_NOISE_TAGLEN);
    }
    mbedtls_gcm_free(&g);
    return rc == 0 ? 0 : -1;
}

int oc_noise_encrypt(oc_noise_cipher *c, const uint8_t *ad, size_t ad_len,
                     const uint8_t *pt, size_t pt_len, uint8_t *out) {
    if (!c->has_k || c->n == UINT64_MAX) return -1;   /* 2^64-1 is reserved (§5.1) */
    if (gcm(1, c->k, c->n, ad, ad_len, pt, pt_len, out) != 0) return -1;
    c->n++;
    return 0;
}

int oc_noise_decrypt(oc_noise_cipher *c, const uint8_t *ad, size_t ad_len,
                     const uint8_t *ct, size_t ct_len, uint8_t *out) {
    if (!c->has_k || c->n == UINT64_MAX) return -1;
    if (gcm(0, c->k, c->n, ad, ad_len, ct, ct_len, out) != 0) return -1;
    c->n++;
    return 0;
}

/* --- SymmetricState (§5.2) ------------------------------------------------- */

static int mix_hash(oc_noise_hs *hs, const uint8_t *data, size_t len) {
    return hash2(hs->h, sizeof hs->h, data, len, hs->h);
}

static int mix_key(oc_noise_hs *hs, const uint8_t *ikm, size_t len) {
    uint8_t temp_k[OC_NOISE_HASHLEN];
    if (hkdf2(hs->ck, ikm, len, hs->ck, temp_k) != 0) return -1;
    memcpy(hs->cs.k, temp_k, 32);
    hs->cs.n = 0;
    hs->cs.has_k = 1;
    oc_e2e_wipe(temp_k, sizeof temp_k);
    return 0;
}

/* EncryptAndHash: `out` holds len (+ tag once there is a key); returns bytes written or -1. */
static long encrypt_and_hash(oc_noise_hs *hs, const uint8_t *pt, size_t len, uint8_t *out, size_t cap) {
    size_t n = len + (hs->cs.has_k ? OC_NOISE_TAGLEN : 0);
    if (n > cap) return -1;
    if (hs->cs.has_k) {
        if (oc_noise_encrypt(&hs->cs, hs->h, sizeof hs->h, pt, len, out) != 0) return -1;
    } else if (len) {
        memmove(out, pt, len);
    }
    if (mix_hash(hs, out, n) != 0) return -1;
    return (long)n;
}

/* DecryptAndHash: reads `len` bytes of `in`; returns the plaintext length or -1. */
static long decrypt_and_hash(oc_noise_hs *hs, const uint8_t *in, size_t len, uint8_t *out, size_t cap) {
    uint8_t h_before[OC_NOISE_HASHLEN];
    long n;
    memcpy(h_before, hs->h, sizeof h_before);
    if (hs->cs.has_k) {
        if (len < OC_NOISE_TAGLEN || len - OC_NOISE_TAGLEN > cap) return -1;
        if (oc_noise_decrypt(&hs->cs, h_before, sizeof h_before, in, len, out) != 0) return -1;
        n = (long)(len - OC_NOISE_TAGLEN);
    } else {
        if (len > cap) return -1;
        if (len) memmove(out, in, len);
        n = (long)len;
    }
    if (mix_hash(hs, in, len) != 0) return -1;
    return n;
}

/* --- HandshakeState (§5.3) ------------------------------------------------- */

void oc_noise_xx_wipe(oc_noise_hs *hs) { oc_e2e_wipe(hs, sizeof *hs); }

static int init_common(oc_noise_hs *hs, int initiator, const uint8_t *prologue, size_t prologue_len,
                       const uint8_t s_sk[OC_NOISE_DHLEN]) {
    memset(hs, 0, sizeof *hs);
    hs->initiator = initiator ? 1 : 0;
    /* The name is 28 bytes, no longer than HASHLEN: it is h, padded with zeros. */
    memcpy(hs->h, PROTOCOL_NAME, sizeof PROTOCOL_NAME - 1);
    memcpy(hs->ck, hs->h, sizeof hs->ck);
    memcpy(hs->s_sk, s_sk, OC_NOISE_DHLEN);
    if (oc_x25519_public(hs->s_sk, hs->s_pk) != 0) return -1;
    return mix_hash(hs, prologue, prologue_len);
}

int oc_noise_xx_init(oc_noise_hs *hs, int initiator, const uint8_t *prologue, size_t prologue_len,
                     const uint8_t s_sk[OC_NOISE_DHLEN]) {
    if (init_common(hs, initiator, prologue, prologue_len, s_sk) != 0) { oc_noise_xx_wipe(hs); return -1; }
    return 0;
}

int oc_noise_xx_init_with(oc_noise_hs *hs, int initiator, const uint8_t *prologue, size_t prologue_len,
                          const uint8_t s_sk[OC_NOISE_DHLEN], const uint8_t e_sk[OC_NOISE_DHLEN]) {
    if (init_common(hs, initiator, prologue, prologue_len, s_sk) != 0) { oc_noise_xx_wipe(hs); return -1; }
    memcpy(hs->e_sk, e_sk, OC_NOISE_DHLEN);
    hs->has_e_fixed = 1;
    return 0;
}

static int make_e(oc_noise_hs *hs) {
    if (hs->has_e_fixed) return oc_x25519_public(hs->e_sk, hs->e_pk);
    return oc_x25519_keypair(hs->e_sk, hs->e_pk);
}

static int dh_mix(oc_noise_hs *hs, const uint8_t sk[OC_NOISE_DHLEN], const uint8_t pk[OC_NOISE_DHLEN]) {
    uint8_t ss[OC_NOISE_DHLEN];
    int rc = oc_x25519(sk, pk, ss) == 0 && mix_key(hs, ss, sizeof ss) == 0 ? 0 : -1;
    oc_e2e_wipe(ss, sizeof ss);
    return rc;
}

/* Whose turn it is: the initiator writes messages 1 and 3, the responder 2. */
static int my_turn(const oc_noise_hs *hs) {
    return hs->step < 3 && ((hs->step % 2 == 0) == (hs->initiator == 1));
}

int oc_noise_xx_write(oc_noise_hs *hs, const uint8_t *payload, size_t payload_len,
                      uint8_t *out, size_t cap, size_t *out_len) {
    size_t n = 0;
    long w;
    if (!my_turn(hs)) return -1;
    if (hs->step == 0) {                                   /* -> e */
        if (cap < OC_NOISE_DHLEN || make_e(hs) != 0) return -1;
        memcpy(out, hs->e_pk, OC_NOISE_DHLEN); n += OC_NOISE_DHLEN;
        if (mix_hash(hs, hs->e_pk, OC_NOISE_DHLEN) != 0) return -1;
    } else if (hs->step == 1) {                            /* <- e, ee, s, es */
        if (cap < OC_NOISE_DHLEN || make_e(hs) != 0) return -1;
        memcpy(out, hs->e_pk, OC_NOISE_DHLEN); n += OC_NOISE_DHLEN;
        if (mix_hash(hs, hs->e_pk, OC_NOISE_DHLEN) != 0) return -1;
        if (dh_mix(hs, hs->e_sk, hs->re) != 0) return -1;
        if ((w = encrypt_and_hash(hs, hs->s_pk, OC_NOISE_DHLEN, out + n, cap - n)) < 0) return -1;
        n += (size_t)w;
        if (dh_mix(hs, hs->s_sk, hs->re) != 0) return -1;
    } else {                                               /* -> s, se */
        if ((w = encrypt_and_hash(hs, hs->s_pk, OC_NOISE_DHLEN, out, cap)) < 0) return -1;
        n += (size_t)w;
        if (dh_mix(hs, hs->s_sk, hs->re) != 0) return -1;
    }
    if ((w = encrypt_and_hash(hs, payload, payload_len, out + n, cap - n)) < 0) return -1;
    n += (size_t)w;
    if (n > OC_NOISE_MAXMSG) return -1;
    *out_len = n;
    hs->step++;
    return 0;
}

int oc_noise_xx_read(oc_noise_hs *hs, const uint8_t *msg, size_t msg_len,
                     uint8_t *payload, size_t cap, size_t *payload_len) {
    size_t off = 0, slen;
    long r;
    uint8_t s_ct[OC_NOISE_DHLEN + OC_NOISE_TAGLEN];
    if (hs->step >= 3 || my_turn(hs) || msg_len > OC_NOISE_MAXMSG) return -1;
    if (hs->step == 0) {                                   /* -> e (responder reads) */
        if (msg_len < OC_NOISE_DHLEN) return -1;
        memcpy(hs->re, msg, OC_NOISE_DHLEN); off = OC_NOISE_DHLEN;
        if (mix_hash(hs, hs->re, OC_NOISE_DHLEN) != 0) return -1;
    } else if (hs->step == 1) {                            /* <- e, ee, s, es (initiator reads) */
        slen = OC_NOISE_DHLEN + OC_NOISE_TAGLEN;
        if (msg_len < OC_NOISE_DHLEN + slen) return -1;
        memcpy(hs->re, msg, OC_NOISE_DHLEN); off = OC_NOISE_DHLEN;
        if (mix_hash(hs, hs->re, OC_NOISE_DHLEN) != 0) return -1;
        if (dh_mix(hs, hs->e_sk, hs->re) != 0) return -1;
        if (decrypt_and_hash(hs, msg + off, slen, s_ct, sizeof s_ct) != (long)OC_NOISE_DHLEN) return -1;
        memcpy(hs->rs, s_ct, OC_NOISE_DHLEN); off += slen;
        if (dh_mix(hs, hs->e_sk, hs->rs) != 0) return -1;
    } else {                                               /* -> s, se (responder reads) */
        slen = OC_NOISE_DHLEN + OC_NOISE_TAGLEN;
        if (msg_len < slen) return -1;
        if (decrypt_and_hash(hs, msg, slen, s_ct, sizeof s_ct) != (long)OC_NOISE_DHLEN) return -1;
        memcpy(hs->rs, s_ct, OC_NOISE_DHLEN); off = slen;
        if (dh_mix(hs, hs->e_sk, hs->rs) != 0) return -1;
    }
    oc_e2e_wipe(s_ct, sizeof s_ct);
    if ((r = decrypt_and_hash(hs, msg + off, msg_len - off, payload, cap)) < 0) return -1;
    *payload_len = (size_t)r;
    hs->step++;
    return 0;
}

int oc_noise_xx_split(oc_noise_hs *hs, oc_noise_result *out) {
    uint8_t k1[OC_NOISE_HASHLEN], k2[OC_NOISE_HASHLEN], temp[OC_NOISE_HASHLEN];
    static const uint8_t three = 0x03;
    if (hs->step != 3) return -1;
    memset(out, 0, sizeof *out);
    if (hkdf2(hs->ck, NULL, 0, k1, k2) != 0) return -1;
    /* HKDF's third output, for the ratchet: HMAC(temp, out2 || 0x03). */
    if (hmac(hs->ck, NULL, 0, NULL, 0, temp) != 0 || hmac(temp, k2, sizeof k2, &three, 1, out->rk) != 0) {
        oc_e2e_wipe(k1, sizeof k1); oc_e2e_wipe(k2, sizeof k2); oc_e2e_wipe(temp, sizeof temp);
        return -1;
    }
    /* c1 carries the initiator's messages, c2 the responder's. */
    memcpy(hs->initiator ? out->send.k : out->recv.k, k1, 32);
    memcpy(hs->initiator ? out->recv.k : out->send.k, k2, 32);
    out->send.has_k = out->recv.has_k = 1;
    memcpy(out->rs, hs->rs, OC_NOISE_DHLEN);
    memcpy(out->hh, hs->h, OC_NOISE_HASHLEN);
    if (hs->initiator) {
        memcpy(out->bob_pk, hs->re, OC_NOISE_DHLEN);
    } else {
        memcpy(out->bob_sk, hs->e_sk, OC_NOISE_DHLEN);
        memcpy(out->bob_pk, hs->e_pk, OC_NOISE_DHLEN);
    }
    oc_e2e_wipe(k1, sizeof k1); oc_e2e_wipe(k2, sizeof k2); oc_e2e_wipe(temp, sizeof temp);
    oc_noise_xx_wipe(hs);
    return 0;
}
