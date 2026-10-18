/* The Double Ratchet: see ratchet.h. Function names follow the Signal
 * specification (§3) so the code reads against it. */

#include "ratchet.h"

#include "e2e_hpke.h"

#include <stdlib.h>
#include <string.h>

#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>

static const char INFO_RK[]  = "OpenChime private DM ratchet";
static const char INFO_MSG[] = "OpenChime private DM message";

void oc_ratchet_wipe(oc_ratchet *r) { oc_e2e_wipe(r, sizeof *r); }

/* --- KDFs (§5.2) ----------------------------------------------------------- */

/* KDF_RK: HKDF-SHA256 with the root key as salt and the DH output as input. */
static int kdf_rk(uint8_t rk[OC_RATCHET_KEYLEN], const uint8_t dh_out[OC_RATCHET_KEYLEN],
                  uint8_t ck[OC_RATCHET_KEYLEN]) {
    uint8_t okm[2 * OC_RATCHET_KEYLEN];
    int rc = mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), rk, OC_RATCHET_KEYLEN,
                          dh_out, OC_RATCHET_KEYLEN, (const uint8_t *)INFO_RK, sizeof INFO_RK - 1,
                          okm, sizeof okm) == 0 ? 0 : -1;
    if (rc == 0) {
        memcpy(rk, okm, OC_RATCHET_KEYLEN);
        memcpy(ck, okm + OC_RATCHET_KEYLEN, OC_RATCHET_KEYLEN);
    }
    oc_e2e_wipe(okm, sizeof okm);
    return rc;
}

/* KDF_CK: the message key is HMAC(ck, 0x01), the next chain key HMAC(ck, 0x02). */
static int kdf_ck(uint8_t ck[OC_RATCHET_KEYLEN], uint8_t mk[OC_RATCHET_KEYLEN]) {
    static const uint8_t one = 0x01, two = 0x02;
    const mbedtls_md_info_t *sha = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    uint8_t next[OC_RATCHET_KEYLEN];
    int rc = mbedtls_md_hmac(sha, ck, OC_RATCHET_KEYLEN, &one, 1, mk) == 0
          && mbedtls_md_hmac(sha, ck, OC_RATCHET_KEYLEN, &two, 1, next) == 0 ? 0 : -1;
    if (rc == 0) memcpy(ck, next, OC_RATCHET_KEYLEN);
    oc_e2e_wipe(next, sizeof next);
    return rc;
}

/* --- one message: AES-256-GCM under a key and nonce derived from mk ---------- */

static int seal(int encrypt, const uint8_t mk[OC_RATCHET_KEYLEN],
                const uint8_t ad[OC_RATCHET_KEYLEN], const uint8_t header[OC_RATCHET_HEADERLEN],
                const uint8_t *in, size_t in_len, uint8_t *out) {
    uint8_t okm[32 + 12], aad[OC_RATCHET_KEYLEN + OC_RATCHET_HEADERLEN];
    mbedtls_gcm_context g;
    int rc;
    if (mbedtls_hkdf(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), NULL, 0, mk, OC_RATCHET_KEYLEN,
                     (const uint8_t *)INFO_MSG, sizeof INFO_MSG - 1, okm, sizeof okm) != 0) return -1;
    memcpy(aad, ad, OC_RATCHET_KEYLEN);
    memcpy(aad + OC_RATCHET_KEYLEN, header, OC_RATCHET_HEADERLEN);
    mbedtls_gcm_init(&g);
    rc = mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES, okm, 256);
    if (rc == 0 && encrypt) {
        rc = mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, in_len, okm + 32, 12, aad, sizeof aad,
                                       in, out, OC_RATCHET_TAGLEN, out + in_len);
    } else if (rc == 0) {
        rc = in_len < OC_RATCHET_TAGLEN ? -1
           : mbedtls_gcm_auth_decrypt(&g, in_len - OC_RATCHET_TAGLEN, okm + 32, 12, aad, sizeof aad,
                                      in + in_len - OC_RATCHET_TAGLEN, OC_RATCHET_TAGLEN, in, out);
        if (rc != 0 && in_len >= OC_RATCHET_TAGLEN) oc_e2e_wipe(out, in_len - OC_RATCHET_TAGLEN);
    }
    mbedtls_gcm_free(&g);
    oc_e2e_wipe(okm, sizeof okm);
    return rc == 0 ? 0 : -1;
}

/* --- header ---------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }

static void header_of(const oc_ratchet *r, uint8_t h[OC_RATCHET_HEADERLEN]) {
    memcpy(h, r->dhs_pk, OC_RATCHET_KEYLEN);
    put32(h + 32, r->pn);
    put32(h + 36, r->ns);
}

/* --- initialisation (§3.3) ------------------------------------------------- */

int oc_ratchet_init_alice(oc_ratchet *r, const uint8_t rk[OC_RATCHET_KEYLEN],
                          const uint8_t bob_pk[OC_RATCHET_KEYLEN], const uint8_t ad[OC_RATCHET_KEYLEN]) {
    uint8_t dh_out[OC_RATCHET_KEYLEN];
    memset(r, 0, sizeof *r);
    memcpy(r->rk, rk, OC_RATCHET_KEYLEN);
    memcpy(r->ad, ad, OC_RATCHET_KEYLEN);
    memcpy(r->dhr, bob_pk, OC_RATCHET_KEYLEN);
    r->has_dhr = 1;
    int rc = oc_x25519_keypair(r->dhs_sk, r->dhs_pk) == 0
          && oc_x25519(r->dhs_sk, r->dhr, dh_out) == 0
          && kdf_rk(r->rk, dh_out, r->cks) == 0 ? 0 : -1;
    oc_e2e_wipe(dh_out, sizeof dh_out);
    if (rc != 0) { oc_ratchet_wipe(r); return -1; }
    r->has_cks = 1;
    return 0;
}

int oc_ratchet_init_bob(oc_ratchet *r, const uint8_t rk[OC_RATCHET_KEYLEN],
                        const uint8_t bob_sk[OC_RATCHET_KEYLEN], const uint8_t bob_pk[OC_RATCHET_KEYLEN],
                        const uint8_t ad[OC_RATCHET_KEYLEN]) {
    memset(r, 0, sizeof *r);
    memcpy(r->rk, rk, OC_RATCHET_KEYLEN);
    memcpy(r->ad, ad, OC_RATCHET_KEYLEN);
    memcpy(r->dhs_sk, bob_sk, OC_RATCHET_KEYLEN);
    memcpy(r->dhs_pk, bob_pk, OC_RATCHET_KEYLEN);
    return 0;
}

/* --- encrypt (§3.4) -------------------------------------------------------- */

int oc_ratchet_encrypt(oc_ratchet *r, const uint8_t *pt, size_t pt_len,
                       uint8_t header[OC_RATCHET_HEADERLEN], uint8_t *ct) {
    uint8_t mk[OC_RATCHET_KEYLEN];
    if (!r->has_cks || r->ns == UINT32_MAX) return -1;
    if (kdf_ck(r->cks, mk) != 0) return -1;
    header_of(r, header);
    int rc = seal(1, mk, r->ad, header, pt, pt_len, ct);
    oc_e2e_wipe(mk, sizeof mk);
    if (rc == 0) r->ns++;
    return rc;
}

/* --- decrypt (§3.5) -------------------------------------------------------- */

static int skipped_count(const oc_ratchet *r) {
    int n = 0;
    for (unsigned i = 0; i < OC_RATCHET_MAX_SKIP; i++) n += r->skipped[i].used;
    return n;
}

static int skip_message_keys(oc_ratchet *r, uint32_t until) {
    if (!r->has_ckr) return 0;
    if (until < r->nr) return 0;
    if ((uint64_t)until - r->nr + (uint64_t)skipped_count(r) > OC_RATCHET_MAX_SKIP) return -1;
    while (r->nr < until) {
        unsigned i;
        for (i = 0; i < OC_RATCHET_MAX_SKIP && r->skipped[i].used; i++) {}
        if (i == OC_RATCHET_MAX_SKIP) return -1;
        if (kdf_ck(r->ckr, r->skipped[i].mk) != 0) return -1;
        memcpy(r->skipped[i].dh, r->dhr, OC_RATCHET_KEYLEN);
        r->skipped[i].n = r->nr;
        r->skipped[i].used = 1;
        r->nr++;
    }
    return 0;
}

static int dh_ratchet(oc_ratchet *r, const uint8_t their_pk[OC_RATCHET_KEYLEN]) {
    uint8_t dh_out[OC_RATCHET_KEYLEN];
    int rc;
    r->pn = r->ns;
    r->ns = r->nr = 0;
    memcpy(r->dhr, their_pk, OC_RATCHET_KEYLEN);
    r->has_dhr = 1;
    rc = oc_x25519(r->dhs_sk, r->dhr, dh_out) == 0 && kdf_rk(r->rk, dh_out, r->ckr) == 0
      && oc_x25519_keypair(r->dhs_sk, r->dhs_pk) == 0
      && oc_x25519(r->dhs_sk, r->dhr, dh_out) == 0 && kdf_rk(r->rk, dh_out, r->cks) == 0 ? 0 : -1;
    oc_e2e_wipe(dh_out, sizeof dh_out);
    if (rc == 0) r->has_ckr = r->has_cks = 1;
    return rc;
}

static int decrypt_into(oc_ratchet *r, const uint8_t header[OC_RATCHET_HEADERLEN],
                        const uint8_t *ct, size_t ct_len, uint8_t *pt) {
    const uint8_t *their_pk = header;
    uint32_t pn = get32(header + 32), n = get32(header + 36);
    uint8_t mk[OC_RATCHET_KEYLEN];
    int rc;

    /* A key kept for a message that arrived out of order: used once, then wiped. */
    for (unsigned i = 0; i < OC_RATCHET_MAX_SKIP; i++) {
        oc_ratchet_skipped *s = &r->skipped[i];
        if (s->used && s->n == n && memcmp(s->dh, their_pk, OC_RATCHET_KEYLEN) == 0) {
            rc = seal(0, s->mk, r->ad, header, ct, ct_len, pt);
            if (rc == 0) oc_e2e_wipe(s, sizeof *s);
            return rc;
        }
    }

    if (!r->has_dhr || memcmp(their_pk, r->dhr, OC_RATCHET_KEYLEN) != 0) {
        if (skip_message_keys(r, pn) != 0) return -1;
        if (dh_ratchet(r, their_pk) != 0) return -1;
    }
    if (!r->has_ckr || n < r->nr) return -1;          /* a replay, or a key already used */
    if (skip_message_keys(r, n) != 0) return -1;
    if (kdf_ck(r->ckr, mk) != 0) return -1;
    r->nr++;
    rc = seal(0, mk, r->ad, header, ct, ct_len, pt);
    oc_e2e_wipe(mk, sizeof mk);
    return rc;
}

int oc_ratchet_decrypt(oc_ratchet *r, const uint8_t header[OC_RATCHET_HEADERLEN],
                       const uint8_t *ct, size_t ct_len, uint8_t *pt) {
    /* Work on a copy and keep it only if the message opened: a forged or replayed
     * message must not advance anything (§3.5's note on trial decryption). */
    oc_ratchet *next = malloc(sizeof *next);
    if (!next) return -1;
    memcpy(next, r, sizeof *next);
    int rc = decrypt_into(next, header, ct, ct_len, pt);
    if (rc == 0) memcpy(r, next, sizeof *r);
    else if (ct_len >= OC_RATCHET_TAGLEN) oc_e2e_wipe(pt, ct_len - OC_RATCHET_TAGLEN);
    oc_ratchet_wipe(next);
    free(next);
    return rc;
}
