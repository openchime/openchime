/* HPKE Auth mode over X25519 — see e2e_hpke.h. The names follow RFC 9180 §4–§5
 * so the code can be read against it line by line. */

#include "e2e_hpke.h"

#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/ecp.h>
#include <mbedtls/entropy.h>
#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>

#define NSECRET 32u   /* DHKEM(X25519, HKDF-SHA256) */
#define NK      16u   /* AES-128-GCM */
#define NN      12u
#define NH      32u   /* HKDF-SHA256 */

static const uint8_t KEM_SUITE[]  = { 'K', 'E', 'M', 0x00, 0x20 };
static const uint8_t HPKE_SUITE[] = { 'H', 'P', 'K', 'E', 0x00, 0x20, 0x00, 0x01, 0x00, 0x01 };
#define MODE_AUTH 0x02

void oc_e2e_wipe(void *p, size_t n) { mbedtls_platform_zeroize(p, n); }

/* --- randomness ----------------------------------------------------------- */

typedef struct { mbedtls_entropy_context ent; mbedtls_ctr_drbg_context drbg; } rng;

static int rng_open(rng *r) {
    static const char pers[] = "openchime e2e";
    mbedtls_entropy_init(&r->ent);
    mbedtls_ctr_drbg_init(&r->drbg);
    return mbedtls_ctr_drbg_seed(&r->drbg, mbedtls_entropy_func, &r->ent,
                                 (const unsigned char *)pers, sizeof pers - 1) == 0 ? 0 : -1;
}

static void rng_close(rng *r) {
    mbedtls_ctr_drbg_free(&r->drbg);
    mbedtls_entropy_free(&r->ent);
}

int oc_e2e_random(void *buf, size_t n) {
    rng r;
    int rc = rng_open(&r);
    if (rc == 0) rc = mbedtls_ctr_drbg_random(&r.drbg, buf, n) == 0 ? 0 : -1;
    rng_close(&r);
    if (rc) oc_e2e_wipe(buf, n);
    return rc;
}

/* --- X25519 ---------------------------------------------------------------- */

/* out = X25519(sk, u), with u the base point when `u` is NULL. mbedtls_ecp_read_key
 * clamps the scalar as RFC 7748 does, and reading the point clears the top bit;
 * mbedtls_ecp_mul refuses the small-order points, whose shared secret would be
 * the all-zero value RFC 9180 §7.1.4 says to reject — checked again below all
 * the same, since that rule is the RFC's and not the library's. */
static int x25519(const uint8_t sk[OC_X25519_LEN], const uint8_t *u, uint8_t out[OC_X25519_LEN]) {
    mbedtls_ecp_keypair kp;
    mbedtls_ecp_point pt, res;
    rng r;
    int rc = -1;
    mbedtls_ecp_keypair_init(&kp);
    mbedtls_ecp_point_init(&pt);
    mbedtls_ecp_point_init(&res);
    if (rng_open(&r) != 0) goto done;
    if (mbedtls_ecp_read_key(MBEDTLS_ECP_DP_CURVE25519, &kp, sk, OC_X25519_LEN) != 0) goto done;
    if (u) {
        if (mbedtls_ecp_point_read_binary(&kp.MBEDTLS_PRIVATE(grp), &pt, u, OC_X25519_LEN) != 0) goto done;
    } else if (mbedtls_ecp_copy(&pt, &kp.MBEDTLS_PRIVATE(grp).G) != 0) {
        goto done;
    }
    if (mbedtls_ecp_mul(&kp.MBEDTLS_PRIVATE(grp), &res, &kp.MBEDTLS_PRIVATE(d), &pt,
                        mbedtls_ctr_drbg_random, &r.drbg) != 0) goto done;
    size_t olen = 0;
    if (mbedtls_ecp_point_write_binary(&kp.MBEDTLS_PRIVATE(grp), &res, MBEDTLS_ECP_PF_UNCOMPRESSED,
                                       &olen, out, OC_X25519_LEN) != 0 || olen != OC_X25519_LEN) goto done;
    uint8_t acc = 0;
    for (size_t i = 0; i < OC_X25519_LEN; i++) acc |= out[i];
    rc = acc ? 0 : -1;
done:
    if (rc) oc_e2e_wipe(out, OC_X25519_LEN);
    rng_close(&r);
    mbedtls_ecp_point_free(&res);
    mbedtls_ecp_point_free(&pt);
    mbedtls_ecp_keypair_free(&kp);
    return rc;
}

int oc_x25519_public(const uint8_t sk[OC_X25519_LEN], uint8_t pk[OC_X25519_LEN]) {
    return x25519(sk, NULL, pk);
}

int oc_x25519_keypair(uint8_t sk[OC_X25519_LEN], uint8_t pk[OC_X25519_LEN]) {
    if (oc_e2e_random(sk, OC_X25519_LEN) != 0) return -1;
    if (oc_x25519_public(sk, pk) != 0) { oc_e2e_wipe(sk, OC_X25519_LEN); return -1; }
    return 0;
}

/* --- labeled HKDF (RFC 9180 §4) ------------------------------------------- */

#define LABEL_MAX 256u   /* the longest labeled input here is well under this */

static int labeled_extract(const uint8_t *suite, size_t suite_len,
                           const uint8_t *salt, size_t salt_len, const char *label,
                           const uint8_t *ikm, size_t ikm_len, uint8_t prk[NH]) {
    uint8_t buf[LABEL_MAX];
    size_t ll = strlen(label), n = 0;
    if (7 + suite_len + ll + ikm_len > sizeof buf) return -1;
    memcpy(buf + n, "HPKE-v1", 7); n += 7;
    memcpy(buf + n, suite, suite_len); n += suite_len;
    memcpy(buf + n, label, ll); n += ll;
    if (ikm_len) memcpy(buf + n, ikm, ikm_len);
    n += ikm_len;
    int rc = mbedtls_hkdf_extract(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                  salt, salt_len, buf, n, prk) == 0 ? 0 : -1;
    oc_e2e_wipe(buf, sizeof buf);
    return rc;
}

static int labeled_expand(const uint8_t *suite, size_t suite_len, const uint8_t prk[NH],
                          const char *label, const uint8_t *info, size_t info_len,
                          uint8_t *out, size_t len) {
    uint8_t buf[LABEL_MAX];
    size_t ll = strlen(label), n = 0;
    if (2 + 7 + suite_len + ll + info_len > sizeof buf) return -1;
    buf[n++] = (uint8_t)(len >> 8);
    buf[n++] = (uint8_t)len;
    memcpy(buf + n, "HPKE-v1", 7); n += 7;
    memcpy(buf + n, suite, suite_len); n += suite_len;
    memcpy(buf + n, label, ll); n += ll;
    if (info_len) memcpy(buf + n, info, info_len);
    n += info_len;
    int rc = mbedtls_hkdf_expand(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                                 prk, NH, buf, n, out, len) == 0 ? 0 : -1;
    oc_e2e_wipe(buf, sizeof buf);
    return rc;
}

/* --- DHKEM (RFC 9180 §4.1) ------------------------------------------------- */

/* shared_secret = ExtractAndExpand(dh, enc ‖ pkR ‖ pkS) */
static int extract_and_expand(const uint8_t dh[2 * OC_X25519_LEN], const uint8_t enc[OC_X25519_LEN],
                              const uint8_t pk_r[OC_X25519_LEN], const uint8_t pk_s[OC_X25519_LEN],
                              uint8_t shared[NSECRET]) {
    uint8_t ctx[3 * OC_X25519_LEN], prk[NH];
    memcpy(ctx, enc, OC_X25519_LEN);
    memcpy(ctx + OC_X25519_LEN, pk_r, OC_X25519_LEN);
    memcpy(ctx + 2 * OC_X25519_LEN, pk_s, OC_X25519_LEN);
    int rc = labeled_extract(KEM_SUITE, sizeof KEM_SUITE, NULL, 0, "eae_prk",
                             dh, 2 * OC_X25519_LEN, prk);
    if (rc == 0) rc = labeled_expand(KEM_SUITE, sizeof KEM_SUITE, prk, "shared_secret",
                                     ctx, sizeof ctx, shared, NSECRET);
    oc_e2e_wipe(prk, sizeof prk);
    return rc;
}

/* --- key schedule (RFC 9180 §5.1), Auth mode: no PSK ---------------------- */

static int key_schedule(const uint8_t shared[NSECRET], const uint8_t *info, size_t info_len,
                        uint8_t key[NK], uint8_t nonce[NN]) {
    uint8_t ksc[1 + 2 * NH], secret[NH];
    int rc = -1;
    ksc[0] = MODE_AUTH;
    if (labeled_extract(HPKE_SUITE, sizeof HPKE_SUITE, NULL, 0, "psk_id_hash", NULL, 0, ksc + 1) != 0) goto done;
    if (labeled_extract(HPKE_SUITE, sizeof HPKE_SUITE, NULL, 0, "info_hash", info, info_len, ksc + 1 + NH) != 0) goto done;
    if (labeled_extract(HPKE_SUITE, sizeof HPKE_SUITE, shared, NSECRET, "secret", NULL, 0, secret) != 0) goto done;
    if (labeled_expand(HPKE_SUITE, sizeof HPKE_SUITE, secret, "key", ksc, sizeof ksc, key, NK) != 0) goto done;
    if (labeled_expand(HPKE_SUITE, sizeof HPKE_SUITE, secret, "base_nonce", ksc, sizeof ksc, nonce, NN) != 0) goto done;
    rc = 0;
done:
    oc_e2e_wipe(secret, sizeof secret);
    return rc;
}

/* --- seal / open ----------------------------------------------------------- */

int oc_hpke_seal_auth_with(const uint8_t sk_e[OC_X25519_LEN],
                           const uint8_t pk_r[OC_X25519_LEN], const uint8_t sk_s[OC_X25519_LEN],
                           const uint8_t *info, size_t info_len,
                           const uint8_t *aad, size_t aad_len,
                           const uint8_t *pt, size_t pt_len,
                           uint8_t enc[OC_X25519_LEN], uint8_t *ct) {
    uint8_t dh[2 * OC_X25519_LEN], pk_s[OC_X25519_LEN], shared[NSECRET], key[NK], nonce[NN];
    mbedtls_gcm_context gcm;
    int rc = -1;
    mbedtls_gcm_init(&gcm);
    /* AuthEncap: dh = DH(skE, pkR) ‖ DH(skS, pkR), enc = pkE. */
    if (oc_x25519_public(sk_e, enc) != 0) goto done;
    if (oc_x25519_public(sk_s, pk_s) != 0) goto done;
    if (x25519(sk_e, pk_r, dh) != 0) goto done;
    if (x25519(sk_s, pk_r, dh + OC_X25519_LEN) != 0) goto done;
    if (extract_and_expand(dh, enc, pk_r, pk_s, shared) != 0) goto done;
    if (key_schedule(shared, info, info_len, key, nonce) != 0) goto done;
    /* Single-shot: sequence 0, so the nonce is base_nonce unchanged. */
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, NK * 8) != 0) goto done;
    if (mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, pt_len, nonce, NN, aad, aad_len,
                                  pt, ct, OC_HPKE_TAG_LEN, ct + pt_len) != 0) goto done;
    rc = 0;
done:
    mbedtls_gcm_free(&gcm);
    oc_e2e_wipe(dh, sizeof dh);
    oc_e2e_wipe(shared, sizeof shared);
    oc_e2e_wipe(key, sizeof key);
    oc_e2e_wipe(nonce, sizeof nonce);
    return rc;
}

int oc_hpke_seal_auth(const uint8_t pk_r[OC_X25519_LEN], const uint8_t sk_s[OC_X25519_LEN],
                      const uint8_t *info, size_t info_len,
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *pt, size_t pt_len,
                      uint8_t enc[OC_X25519_LEN], uint8_t *ct) {
    uint8_t sk_e[OC_X25519_LEN];
    int rc = oc_e2e_random(sk_e, sizeof sk_e);
    if (rc == 0) rc = oc_hpke_seal_auth_with(sk_e, pk_r, sk_s, info, info_len, aad, aad_len,
                                             pt, pt_len, enc, ct);
    oc_e2e_wipe(sk_e, sizeof sk_e);
    return rc;
}

int oc_hpke_open_auth(const uint8_t enc[OC_X25519_LEN], const uint8_t sk_r[OC_X25519_LEN],
                      const uint8_t pk_s[OC_X25519_LEN],
                      const uint8_t *info, size_t info_len,
                      const uint8_t *aad, size_t aad_len,
                      const uint8_t *ct, size_t ct_len, uint8_t *pt) {
    uint8_t dh[2 * OC_X25519_LEN], pk_r[OC_X25519_LEN], shared[NSECRET], key[NK], nonce[NN];
    mbedtls_gcm_context gcm;
    int rc = -1;
    if (ct_len < OC_HPKE_TAG_LEN) return -1;
    size_t pt_len = ct_len - OC_HPKE_TAG_LEN;
    mbedtls_gcm_init(&gcm);
    /* AuthDecap: dh = DH(skR, pkE) ‖ DH(skR, pkS). */
    if (oc_x25519_public(sk_r, pk_r) != 0) goto done;
    if (x25519(sk_r, enc, dh) != 0) goto done;
    if (x25519(sk_r, pk_s, dh + OC_X25519_LEN) != 0) goto done;
    if (extract_and_expand(dh, enc, pk_r, pk_s, shared) != 0) goto done;
    if (key_schedule(shared, info, info_len, key, nonce) != 0) goto done;
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, NK * 8) != 0) goto done;
    /* mbedtls_gcm_auth_decrypt compares the tag in constant time and zeroes the
     * output on a mismatch. */
    if (mbedtls_gcm_auth_decrypt(&gcm, pt_len, nonce, NN, aad, aad_len,
                                 ct + pt_len, OC_HPKE_TAG_LEN, ct, pt) != 0) goto done;
    rc = 0;
done:
    if (rc && pt_len) oc_e2e_wipe(pt, pt_len);
    mbedtls_gcm_free(&gcm);
    oc_e2e_wipe(dh, sizeof dh);
    oc_e2e_wipe(shared, sizeof shared);
    oc_e2e_wipe(key, sizeof key);
    oc_e2e_wipe(nonce, sizeof nonce);
    return rc;
}
