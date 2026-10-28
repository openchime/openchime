/* Tests for the private DM's cryptography (shared/noise_xx.c, shared/ratchet.c;
 * PRIVATE-DM.md): the handshake against the Noise test vectors published with
 * cacophony (public domain), then what the vectors do not cover, and the Double
 * Ratchet's properties: it has no published vectors, so every rule it promises
 * is asserted on its own. */
#include "check.h"
#include "e2e_hpke.h"
#include "noise_xx.h"
#include "ratchet.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static size_t unhex(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    for (; s[0] && s[1] && n < cap; s += 2) {
        unsigned v = 0;
        for (int i = 0; i < 2; i++) {
            char c = s[i];
            v = v * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10);
        }
        out[n++] = (uint8_t)v;
    }
    return n;
}

/* cacophony vectors/cacophony.txt, protocol_name Noise_XX_25519_AESGCM_SHA256.
 * Messages alternate, the initiator first; the last three are transport messages. */
static const char *V_PROLOGUE = "4a6f686e2047616c74";
static const char *V_INIT_S   = "e61ef9919cde45dd5f82166404bd08e38bceb5dfdfded0a34c8df7ed542214d1";
static const char *V_INIT_E   = "893e28b9dc6ca8d611ab664754b8ceb7bac5117349a4439a6b0569da977c464a";
static const char *V_RESP_S   = "4a3acbfdb163dec651dfa3194dece676d437029c62a408b4c5ea9114246e4893";
static const char *V_RESP_E   = "bbdb4cdbd309f1a1f2e1456967fe288cadd6f712d65dc7b7793d5e63da6b375b";
static const char *V_HASH     = "1b7aefb1125762aa21a252890d00af54519638b76437444538f9a52f21e2e0dc";
static const struct { const char *payload, *ciphertext; } V_MSGS[] = {
    { "4c756477696720766f6e204d69736573",
      "ca35def5ae56cec33dc2036731ab14896bc4c75dbb07a61f879f8e3afa4c79444c756477696720766f6e204d69736573" },
    { "4d757272617920526f746862617264",
      "95ebc60d2b1fa672c1f46a8aa265ef51bfe38e7ccb39ec5be34069f144808843757117acceb05bd7a45733bc22015c97"
      "a9d0cbaf41b80446d5988ff5127235d76b79eade70f473d6a4ef521fdcbeda5340d01e028ba793fc059f2724a83af05f"
      "12dda0448a7621a926b379a92477fd" },
    { "462e20412e20486179656b",
      "c90f1cf77eba4e50edb038991565e36c9758943a989229b6051244dc4fbecb6946744b401af2ee1a5881b65fbb87fd07"
      "cb6a328ececc9ce6ce84c399dc332d4fd521fa4bb7f467ce909395" },
    { "4361726c204d656e676572", "bc3fa77f6aca3e8466d7dc6bea10013e88a6a29add5132b461806c" },
    { "4a65616e2d426170746973746520536179",
      "250b01074cdfe0df2ecf8ccbf1737b15a2ddb5b52fd9a396604e9c793cee3b3bb9" },
    { "457567656e2042f6686d20766f6e2042617765726b",
      "449d4d433b3cdc3d02bf6fc881774b9df54366ebcffb9689bb13f14709822cd7ef42bcdb4d" },
};

static void test_noise_vectors(void) {
    uint8_t pro[16], is[32], ie[32], rs_sk[32], re[32], hh[32], buf[512], got[512], pt[512];
    size_t pro_len = unhex(V_PROLOGUE, pro, sizeof pro);
    unhex(V_INIT_S, is, 32); unhex(V_INIT_E, ie, 32); unhex(V_RESP_S, rs_sk, 32); unhex(V_RESP_E, re, 32);
    unhex(V_HASH, hh, 32);

    oc_noise_hs ini, rsp;
    CHECK(oc_noise_xx_init_with(&ini, 1, pro, pro_len, is, ie) == 0);
    CHECK(oc_noise_xx_init_with(&rsp, 0, pro, pro_len, rs_sk, re) == 0);

    for (int i = 0; i < 3; i++) {
        oc_noise_hs *w = (i % 2 == 0) ? &ini : &rsp, *r = (i % 2 == 0) ? &rsp : &ini;
        size_t plen = unhex(V_MSGS[i].payload, pt, sizeof pt), want_len = unhex(V_MSGS[i].ciphertext, buf, sizeof buf);
        size_t out_len = 0, back_len = 0;
        CHECK(oc_noise_xx_write(w, pt, plen, got, sizeof got, &out_len) == 0);
        CHECK(out_len == want_len && memcmp(got, buf, want_len) == 0);
        uint8_t back[512];
        CHECK(oc_noise_xx_read(r, got, out_len, back, sizeof back, &back_len) == 0);
        CHECK(back_len == plen && memcmp(back, pt, plen) == 0);
    }

    oc_noise_result a, b;
    CHECK(oc_noise_xx_split(&ini, &a) == 0);
    CHECK(oc_noise_xx_split(&rsp, &b) == 0);
    CHECK(memcmp(a.hh, hh, 32) == 0 && memcmp(b.hh, hh, 32) == 0);
    /* Each knows the other's static key; the ratchet starts from the same root and
     * the responder's ephemeral. */
    uint8_t pub[32];
    CHECK(oc_x25519_public(rs_sk, pub) == 0 && memcmp(a.rs, pub, 32) == 0);
    CHECK(oc_x25519_public(is, pub) == 0 && memcmp(b.rs, pub, 32) == 0);
    CHECK(memcmp(a.rk, b.rk, 32) == 0);
    CHECK(oc_x25519_public(re, pub) == 0 && memcmp(a.bob_pk, pub, 32) == 0 && memcmp(b.bob_sk, re, 32) == 0);

    for (int i = 3; i < 6; i++) {
        oc_noise_cipher *w = (i % 2 == 0) ? &a.send : &b.send, *r = (i % 2 == 0) ? &b.recv : &a.recv;
        size_t plen = unhex(V_MSGS[i].payload, pt, sizeof pt), want_len = unhex(V_MSGS[i].ciphertext, buf, sizeof buf);
        CHECK(oc_noise_encrypt(w, NULL, 0, pt, plen, got) == 0);
        CHECK(plen + OC_NOISE_TAGLEN == want_len && memcmp(got, buf, want_len) == 0);
        uint8_t back[512];
        CHECK(oc_noise_decrypt(r, NULL, 0, got, want_len, back) == 0 && memcmp(back, pt, plen) == 0);
    }
}

/* A handshake between two fresh devices with random ephemerals; `tamper` alters
 * one byte of the given message (1..3) on its way. Returns 0 when both sides
 * finished. */
static int handshake(const uint8_t *pro_a, size_t la, const uint8_t *pro_b, size_t lb,
                     const uint8_t s_a[32], const uint8_t s_b[32], int tamper,
                     oc_noise_result *ra, oc_noise_result *rb) {
    oc_noise_hs a, b;
    uint8_t msg[256], pl[64];
    size_t n, pn;
    int ok = 0;
    if (oc_noise_xx_init(&a, 1, pro_a, la, s_a) != 0 || oc_noise_xx_init(&b, 0, pro_b, lb, s_b) != 0) return -1;
    for (int i = 1; i <= 3; i++) {
        oc_noise_hs *w = (i % 2) ? &a : &b, *r = (i % 2) ? &b : &a;
        if (oc_noise_xx_write(w, (const uint8_t *)"hi", 2, msg, sizeof msg, &n) != 0) { ok = -1; break; }
        if (tamper == i) msg[n - 1] ^= 0x01;
        if (oc_noise_xx_read(r, msg, n, pl, sizeof pl, &pn) != 0) { ok = -1; break; }
    }
    if (ok == 0) ok = (oc_noise_xx_split(&a, ra) == 0 && oc_noise_xx_split(&b, rb) == 0) ? 0 : -1;
    oc_noise_xx_wipe(&a); oc_noise_xx_wipe(&b);
    return ok;
}

static void test_noise_rules(void) {
    uint8_t sa[32], pa[32], sb[32], pb[32];
    CHECK(oc_x25519_keypair(sa, pa) == 0 && oc_x25519_keypair(sb, pb) == 0);
    oc_noise_result ra, rb;
    const uint8_t pro[] = "openchime pdm test";

    CHECK(handshake(pro, sizeof pro, pro, sizeof pro, sa, sb, 0, &ra, &rb) == 0);
    CHECK(memcmp(ra.rs, pb, 32) == 0 && memcmp(rb.rs, pa, 32) == 0);
    CHECK(memcmp(ra.hh, rb.hh, 32) == 0 && memcmp(ra.rk, rb.rk, 32) == 0);

    /* Every altered message, and a prologue the two sides disagree on (another
     * workspace, other users, another DM), fails the handshake. */
    for (int t = 1; t <= 3; t++) CHECK(handshake(pro, sizeof pro, pro, sizeof pro, sa, sb, t, &ra, &rb) != 0);
    const uint8_t other[] = "openchime pdm tesT";
    CHECK(handshake(pro, sizeof pro, other, sizeof other, sa, sb, 0, &ra, &rb) != 0);

    /* Out of turn: the responder cannot write first, nor the initiator read first. */
    oc_noise_hs a, b;
    uint8_t m[256], p[64];
    size_t n, pn;
    CHECK(oc_noise_xx_init(&b, 0, pro, sizeof pro, sb) == 0);
    CHECK(oc_noise_xx_write(&b, NULL, 0, m, sizeof m, &n) != 0);
    CHECK(oc_noise_xx_init(&a, 1, pro, sizeof pro, sa) == 0);
    CHECK(oc_noise_xx_read(&a, m, 32, p, sizeof p, &pn) != 0);
    /* Not split before the third message. */
    oc_noise_result r;
    CHECK(oc_noise_xx_split(&a, &r) != 0);
    oc_noise_xx_wipe(&a); oc_noise_xx_wipe(&b);
}

/* --- the ratchet ------------------------------------------------------------ */

typedef struct { uint8_t h[OC_RATCHET_HEADERLEN]; uint8_t ct[96]; size_t len; } wire;

static void ratchet_pair(oc_ratchet **alice, oc_ratchet **bob) {
    uint8_t sa[32], pa[32], sb[32], pb[32];
    oc_noise_result ra, rb;
    const uint8_t pro[] = "openchime pdm ratchet test";
    *alice = calloc(1, sizeof **alice);
    *bob = calloc(1, sizeof **bob);
    CHECK(oc_x25519_keypair(sa, pa) == 0 && oc_x25519_keypair(sb, pb) == 0);
    CHECK(handshake(pro, sizeof pro, pro, sizeof pro, sa, sb, 0, &ra, &rb) == 0);
    CHECK(oc_ratchet_init_alice(*alice, ra.rk, ra.bob_pk, ra.hh) == 0);
    CHECK(oc_ratchet_init_bob(*bob, rb.rk, rb.bob_sk, rb.bob_pk, rb.hh) == 0);
}

static int send(oc_ratchet *r, const char *text, wire *w) {
    w->len = strlen(text) + OC_RATCHET_TAGLEN;
    return oc_ratchet_encrypt(r, (const uint8_t *)text, strlen(text), w->h, w->ct);
}

static int recv_is(oc_ratchet *r, const wire *w, const char *text) {
    uint8_t pt[96];
    if (oc_ratchet_decrypt(r, w->h, w->ct, w->len, pt) != 0) return 0;
    return w->len - OC_RATCHET_TAGLEN == strlen(text) && memcmp(pt, text, strlen(text)) == 0;
}

static void test_ratchet(void) {
    oc_ratchet *alice, *bob;
    ratchet_pair(&alice, &bob);
    wire w[8];

    /* Bob cannot speak before Alice; then both directions, several turns. */
    CHECK(send(bob, "too early", &w[0]) != 0);
    CHECK(send(alice, "hello bob", &w[0]) == 0 && recv_is(bob, &w[0], "hello bob"));
    CHECK(send(bob, "hello alice", &w[1]) == 0 && recv_is(alice, &w[1], "hello alice"));
    CHECK(send(alice, "how are you", &w[2]) == 0 && recv_is(bob, &w[2], "how are you"));

    /* Every message under its own key: the same text twice is two ciphertexts. */
    CHECK(send(alice, "same", &w[3]) == 0 && send(alice, "same", &w[4]) == 0);
    CHECK(memcmp(w[3].ct, w[4].ct, w[3].len) != 0);
    /* A new ratchet key at every change of speaker: Alice's next turn shows a new one. */
    uint8_t first_turn[32];
    memcpy(first_turn, w[0].h, 32);
    CHECK(memcmp(first_turn, w[3].h, 32) != 0);

    /* Out of order: the later one first, then the earlier, from its kept key. */
    CHECK(recv_is(bob, &w[4], "same") && recv_is(bob, &w[3], "same"));

    /* A replay, and a key once used, open nothing. */
    CHECK(!recv_is(bob, &w[3], "same") && !recv_is(bob, &w[4], "same") && !recv_is(bob, &w[2], "how are you"));

    /* An altered body or header opens nothing and leaves the state as it was. */
    CHECK(send(alice, "untouched", &w[5]) == 0);
    wire bad = w[5];
    bad.ct[0] ^= 1;
    CHECK(!recv_is(bob, &bad, "untouched"));
    bad = w[5]; bad.h[39] ^= 1;
    CHECK(!recv_is(bob, &bad, "untouched"));
    CHECK(recv_is(bob, &w[5], "untouched"));

    /* Another session's message opens nothing. */
    oc_ratchet *carol, *dave;
    ratchet_pair(&carol, &dave);
    CHECK(send(carol, "wrong session", &w[6]) == 0 && !recv_is(bob, &w[6], "wrong session"));

    oc_ratchet_wipe(alice); oc_ratchet_wipe(bob); oc_ratchet_wipe(carol); oc_ratchet_wipe(dave);
    free(alice); free(bob); free(carol); free(dave);
}

static void test_ratchet_skip_limit(void) {
    oc_ratchet *alice, *bob;
    ratchet_pair(&alice, &bob);
    wire w, last;
    /* OC_RATCHET_MAX_SKIP messages may be missing; one more and the message is refused. */
    for (unsigned i = 0; i < OC_RATCHET_MAX_SKIP; i++) CHECK(send(alice, "lost", &w) == 0);
    CHECK(send(alice, "arrives", &last) == 0 && recv_is(bob, &last, "arrives"));

    oc_ratchet *a2, *b2;
    ratchet_pair(&a2, &b2);
    for (unsigned i = 0; i < OC_RATCHET_MAX_SKIP + 1; i++) CHECK(send(a2, "lost", &w) == 0);
    CHECK(send(a2, "too far", &last) == 0 && !recv_is(b2, &last, "too far"));

    oc_ratchet_wipe(alice); oc_ratchet_wipe(bob); oc_ratchet_wipe(a2); oc_ratchet_wipe(b2);
    free(alice); free(bob); free(a2); free(b2);
}

/* Wiping leaves nothing behind. */
static void test_wipe(void) {
    oc_ratchet *alice, *bob;
    ratchet_pair(&alice, &bob);
    oc_ratchet_wipe(alice);
    const uint8_t *p = (const uint8_t *)alice;
    uint8_t acc = 0;
    for (size_t i = 0; i < sizeof *alice; i++) acc |= p[i];
    CHECK(acc == 0);
    oc_ratchet_wipe(bob);
    free(alice); free(bob);
}

int run_pdm_crypto_tests(void) {
    test_noise_vectors();
    test_noise_rules();
    test_ratchet();
    test_ratchet_skip_limit();
    test_wipe();
    return failures;
}
