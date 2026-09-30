#include "localissuer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/platform_util.h>

#include "jwt.h"

struct oc_local_issuer {
    mbedtls_pk_context       key;
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
    char                     name[128];
    char                     kid[OC_JWT_THUMBPRINT_LEN + 1];
    char                     pub_pem[512];
};

static int drbg_seed(mbedtls_entropy_context *e, mbedtls_ctr_drbg_context *d) {
    static const char pers[] = "openchimed-local-issuer";
    mbedtls_entropy_init(e);
    mbedtls_ctr_drbg_init(d);
    return mbedtls_ctr_drbg_seed(d, mbedtls_entropy_func, e, (const unsigned char *)pers, sizeof pers - 1);
}

int oc_local_issuer_generate(char **key_pem, char **issuer) {
    *key_pem = *issuer = NULL;
    mbedtls_entropy_context e; mbedtls_ctr_drbg_context d;
    mbedtls_pk_context pk; mbedtls_pk_init(&pk);
    unsigned char buf[2048], id[16];
    int rc = -1;
    if (drbg_seed(&e, &d) ||
        mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) ||
        mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), mbedtls_ctr_drbg_random, &d) ||
        mbedtls_pk_write_key_pem(&pk, buf, sizeof buf) ||
        mbedtls_ctr_drbg_random(&d, id, sizeof id)) goto out;
    /* A name of its own, so a token from one workspace's issuer is never taken as
     * another's, even where their keys were somehow alike. */
    char name[128], idb[32];
    oc_base64url_encode(id, sizeof id, idb);
    snprintf(name, sizeof name, "openchime-local:%s", idb);
    *key_pem = strdup((char *)buf);
    *issuer = strdup(name);
    if (*key_pem && *issuer) rc = 0;
    else { free(*key_pem); free(*issuer); *key_pem = *issuer = NULL; }
out:
    mbedtls_platform_zeroize(buf, sizeof buf);
    mbedtls_pk_free(&pk);
    mbedtls_ctr_drbg_free(&d); mbedtls_entropy_free(&e);
    return rc;
}

oc_local_issuer *oc_local_issuer_open(const char *key_pem, const char *issuer) {
    if (!key_pem || !issuer || strlen(issuer) >= sizeof ((oc_local_issuer *)0)->name) return NULL;
    oc_local_issuer *li = calloc(1, sizeof *li);
    if (!li) return NULL;
    mbedtls_pk_init(&li->key);
    if (drbg_seed(&li->entropy, &li->drbg) ||
        mbedtls_pk_parse_key(&li->key, (const unsigned char *)key_pem, strlen(key_pem) + 1, NULL, 0,
                             mbedtls_ctr_drbg_random, &li->drbg) ||
        oc_jwk_thumbprint(&li->key, li->kid) != 0 ||
        mbedtls_pk_write_pubkey_pem(&li->key, (unsigned char *)li->pub_pem, sizeof li->pub_pem)) {
        oc_local_issuer_close(li);
        return NULL;
    }
    snprintf(li->name, sizeof li->name, "%s", issuer);
    return li;
}

void oc_local_issuer_close(oc_local_issuer *li) {
    if (!li) return;
    mbedtls_pk_free(&li->key);
    mbedtls_ctr_drbg_free(&li->drbg);
    mbedtls_entropy_free(&li->entropy);
    free(li);
}

const char *oc_local_issuer_name(const oc_local_issuer *li) { return li->name; }
const char *oc_local_issuer_pubkey_pem(const oc_local_issuer *li) { return li->pub_pem; }

char *oc_local_issuer_mint(oc_local_issuer *li, uint64_t uid, const char *nonce, uint64_t now_secs) {
    if (!li || !nonce || strlen(nonce) >= OC_JWT_MAX_SHORT) return NULL;
    unsigned char rid[16];
    char jti[32], head[160], body[512], head64[220], body64[700];
    if (mbedtls_ctr_drbg_random(&li->drbg, rid, sizeof rid)) return NULL;
    oc_base64url_encode(rid, sizeof rid, jti);
    snprintf(head, sizeof head, "{\"alg\":\"ES256\",\"typ\":\"JWT\",\"kid\":\"%s\"}", li->kid);
    int bl = snprintf(body, sizeof body,
                      "{\"iss\":\"%s\",\"aud\":\"%s\",\"sub\":\"local|%llu\",\"jti\":\"%s\",\"nonce\":\"%s\","
                      "\"iat\":%llu,\"nbf\":%llu,\"exp\":%llu}",
                      li->name, OC_LOCAL_AUDIENCE, (unsigned long long)uid, jti, nonce,
                      (unsigned long long)now_secs, (unsigned long long)now_secs,
                      (unsigned long long)(now_secs + OC_LOCAL_TOKEN_SECS));
    if (bl < 0 || (size_t)bl >= sizeof body) return NULL;
    oc_base64url_encode((const uint8_t *)head, strlen(head), head64);
    oc_base64url_encode((const uint8_t *)body, (size_t)bl, body64);
    size_t il = strlen(head64) + 1 + strlen(body64);
    char *out = malloc(il + 1 + OC_JWS_SIG64_LEN + 1);
    if (!out) return NULL;
    snprintf(out, il + 1, "%s.%s", head64, body64);
    char sig[OC_JWS_SIG64_LEN + 1];
    if (oc_jws_es256_sign(&li->key, mbedtls_ctr_drbg_random, &li->drbg, out, il, sig) != 0) {
        free(out);
        return NULL;
    }
    out[il] = '.';
    memcpy(out + il + 1, sig, strlen(sig) + 1);
    return out;
}

uint64_t oc_local_subject_uid(const char *sub) {
    if (!sub || strncmp(sub, "local|", 6) != 0) return 0;
    const char *p = sub + 6;
    if (*p < '1' || *p > '9') return 0;
    uint64_t v = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9' || v > (UINT64_MAX - 9) / 10) return 0;
        v = v * 10 + (uint64_t)(*p - '0');
    }
    return v;
}
