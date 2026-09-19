/* SFrame, suite AES_128_GCM_SHA256_128 — see e2e_sframe.h. Names follow RFC 9605
 * §4.3–§4.4. */

#include "e2e_sframe.h"

#include <string.h>

#include <mbedtls/gcm.h>
#include <mbedtls/hkdf.h>
#include <mbedtls/md.h>
#include <mbedtls/platform_util.h>

#define NK 16u
#define NN 12u
#define NH 32u

/* --- header (§4.3) --------------------------------------------------------- */

static size_t min_bytes(uint64_t v) {
    size_t n = 1;
    while (n < 8 && (v >> (8 * n))) n++;
    return n;
}

size_t oc_sframe_header_encode(uint64_t kid, uint64_t ctr, uint8_t *out) {
    size_t n = 1;
    uint8_t cfg = 0;
    if (kid < 8) {
        cfg |= (uint8_t)(kid << 4);
    } else {
        size_t kl = min_bytes(kid);
        cfg |= (uint8_t)(0x80 | ((kl - 1) << 4));
        for (size_t i = 0; i < kl; i++) out[n++] = (uint8_t)(kid >> (8 * (kl - 1 - i)));
    }
    if (ctr < 8) {
        cfg |= (uint8_t)ctr;
    } else {
        size_t cl = min_bytes(ctr);
        cfg |= (uint8_t)(0x08 | (cl - 1));
        for (size_t i = 0; i < cl; i++) out[n++] = (uint8_t)(ctr >> (8 * (cl - 1 - i)));
    }
    out[0] = cfg;
    return n;
}

/* Read a field that follows the config byte: `l` bytes, big-endian. It must need
 * all of them and be at least 8, or it would have been written shorter. */
static int read_ext(const uint8_t *in, size_t len, size_t *off, size_t l, uint64_t *v) {
    if (len - *off < l) return -1;
    uint64_t x = 0;
    for (size_t i = 0; i < l; i++) x = (x << 8) | in[*off + i];
    if (x < 8 || min_bytes(x) != l) return -1;
    *off += l;
    *v = x;
    return 0;
}

int oc_sframe_header_decode(const uint8_t *in, size_t len,
                            uint64_t *kid, uint64_t *ctr, size_t *hdr_len) {
    if (len < 1) return -1;
    uint8_t cfg = in[0];
    size_t off = 1;
    if (cfg & 0x80) { if (read_ext(in, len, &off, (size_t)((cfg >> 4) & 7) + 1, kid) != 0) return -1; }
    else *kid = (cfg >> 4) & 7;
    if (cfg & 0x08) { if (read_ext(in, len, &off, (size_t)(cfg & 7) + 1, ctr) != 0) return -1; }
    else *ctr = cfg & 7;
    *hdr_len = off;
    return 0;
}

/* --- keys (§4.4.2) --------------------------------------------------------- */

int oc_sframe_key_init(oc_sframe_key *k, uint64_t kid, const uint8_t *base_key, size_t base_len) {
    const mbedtls_md_info_t *md = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    /* The longer label: "SFrame 1.0 Secret salt " (23) + KID (8) + suite (2). */
    uint8_t secret[NH], label[23 + 8 + 2];
    int rc = -1;
    memset(k, 0, sizeof *k);
    /* sframe_secret = HKDF-Extract("", base_key) */
    if (mbedtls_hkdf_extract(md, NULL, 0, base_key, base_len, secret) != 0) goto done;
    /* label = "SFrame 1.0 Secret key " ‖ KID(u64 BE) ‖ cipher_suite(u16 BE), and
     * the same with "salt" */
    static const char kl[] = "SFrame 1.0 Secret key ";
    static const char sl[] = "SFrame 1.0 Secret salt ";
    for (int which = 0; which < 2; which++) {
        const char *s = which ? sl : kl;
        size_t n = strlen(s);
        memcpy(label, s, n);
        for (int i = 0; i < 8; i++) label[n++] = (uint8_t)(kid >> (56 - 8 * i));
        label[n++] = (uint8_t)(OC_SFRAME_SUITE >> 8);
        label[n++] = (uint8_t)OC_SFRAME_SUITE;
        if (mbedtls_hkdf_expand(md, secret, NH, label, n,
                                which ? k->salt : k->key, which ? NN : NK) != 0) goto done;
    }
    k->kid = kid;
    k->ready = 1;
    rc = 0;
done:
    mbedtls_platform_zeroize(secret, sizeof secret);
    if (rc) oc_sframe_key_wipe(k);
    return rc;
}

void oc_sframe_key_wipe(oc_sframe_key *k) {
    mbedtls_platform_zeroize(k, sizeof *k);
}

/* --- encrypt / decrypt (§4.4.3–§4.4.4) ------------------------------------- */

static void make_nonce(const oc_sframe_key *k, uint64_t ctr, uint8_t nonce[NN]) {
    memcpy(nonce, k->salt, NN);
    for (int i = 0; i < 8; i++) nonce[NN - 1 - i] ^= (uint8_t)(ctr >> (8 * i));
}

/* The AAD is header ‖ metadata; both are short, so it is assembled on the stack. */
#define AAD_MAX 128u

int oc_sframe_encrypt(const oc_sframe_key *k, uint64_t ctr,
                      const uint8_t *meta, size_t meta_len,
                      const uint8_t *pt, size_t pt_len,
                      uint8_t *out, size_t cap, size_t *out_len) {
    uint8_t hdr[OC_SFRAME_HDR_MAX], aad[AAD_MAX], nonce[NN];
    if (!k->ready) return -1;
    size_t hl = oc_sframe_header_encode(k->kid, ctr, hdr);
    if (hl + meta_len > sizeof aad || cap < hl + pt_len + OC_SFRAME_TAG_LEN) return -1;
    memcpy(aad, hdr, hl);
    if (meta_len) memcpy(aad + hl, meta, meta_len);
    make_nonce(k, ctr, nonce);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = -1;
    memcpy(out, hdr, hl);
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, k->key, NK * 8) == 0 &&
        mbedtls_gcm_crypt_and_tag(&gcm, MBEDTLS_GCM_ENCRYPT, pt_len, nonce, NN, aad, hl + meta_len,
                                  pt, out + hl, OC_SFRAME_TAG_LEN, out + hl + pt_len) == 0) {
        *out_len = hl + pt_len + OC_SFRAME_TAG_LEN;
        rc = 0;
    }
    mbedtls_gcm_free(&gcm);
    return rc;
}

int oc_sframe_decrypt(const oc_sframe_key *k,
                      const uint8_t *meta, size_t meta_len,
                      const uint8_t *in, size_t len,
                      uint8_t *pt, size_t cap, size_t *pt_len, uint64_t *ctr) {
    uint8_t aad[AAD_MAX], nonce[NN];
    uint64_t kid, c;
    size_t hl;
    if (!k->ready || oc_sframe_header_decode(in, len, &kid, &c, &hl) != 0) return -1;
    if (kid != k->kid || len < hl + OC_SFRAME_TAG_LEN || hl + meta_len > sizeof aad) return -1;
    size_t n = len - hl - OC_SFRAME_TAG_LEN;
    if (cap < n) return -1;
    memcpy(aad, in, hl);
    if (meta_len) memcpy(aad + hl, meta, meta_len);
    make_nonce(k, c, nonce);
    mbedtls_gcm_context gcm;
    mbedtls_gcm_init(&gcm);
    int rc = -1;
    if (mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, k->key, NK * 8) == 0 &&
        mbedtls_gcm_auth_decrypt(&gcm, n, nonce, NN, aad, hl + meta_len,
                                 in + hl + n, OC_SFRAME_TAG_LEN, in + hl, pt) == 0) {
        *pt_len = n;
        *ctr = c;
        rc = 0;
    }
    mbedtls_gcm_free(&gcm);
    return rc;
}

/* --- replay window --------------------------------------------------------- */

static int bit(const oc_sframe_replay *w, uint64_t ctr) {
    uint64_t i = ctr % OC_SFRAME_REPLAY_WINDOW;
    return (int)((w->seen[i / 64] >> (i % 64)) & 1u);
}

int oc_sframe_replay_ok(const oc_sframe_replay *w, uint64_t ctr) {
    if (!w->any || ctr > w->top) return 1;
    if (w->top - ctr >= OC_SFRAME_REPLAY_WINDOW) return 0;
    return !bit(w, ctr);
}

void oc_sframe_replay_mark(oc_sframe_replay *w, uint64_t ctr) {
    if (!w->any) {
        memset(w->seen, 0, sizeof w->seen);
        w->top = ctr;
        w->any = 1;
    } else if (ctr > w->top) {
        /* Slide: clear the slots the window moves over, which now stand for
         * counters that have not arrived. */
        uint64_t step = ctr - w->top;
        if (step >= OC_SFRAME_REPLAY_WINDOW) memset(w->seen, 0, sizeof w->seen);
        else for (uint64_t c = w->top + 1; c <= ctr; c++) {
            uint64_t i = c % OC_SFRAME_REPLAY_WINDOW;
            w->seen[i / 64] &= ~((uint64_t)1 << (i % 64));
        }
        w->top = ctr;
    } else if (w->top - ctr >= OC_SFRAME_REPLAY_WINDOW) {
        return;
    }
    uint64_t i = ctr % OC_SFRAME_REPLAY_WINDOW;
    w->seen[i / 64] |= (uint64_t)1 << (i % 64);
}
