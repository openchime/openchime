/* Codec fuzzer (robustness backlog #5). Feeds deterministic pseudo-random and
 * semi-structured bytes through oc_parse_frame and every decoder, asserting the
 * codec never crashes, never reads out of bounds, and always returns a
 * well-defined result on garbage. Reproducible: fixed PRNG seed, no I/O.
 *
 * The codec is designed to be safe (oc_rbuf bounds every read; length prefixes
 * are validated by r_take before the bytes are consumed) — this suite exercises
 * that empirically across a wide input space. Run under ASan it also proves the
 * absence of out-of-bounds reads. */

#include "protocol.h"
#include "check.h"

#include <string.h>

/* Iteration counts. The PRNG is deterministic (fixed seed), so a moderate sweep
 * hits the same input categories as a huge one — keep the default snappy for
 * `make test` and crank it via -D for a deep local/ASan run. */
#ifndef OC_FUZZ_RANDOM_ITERS
#define OC_FUZZ_RANDOM_ITERS 30000
#endif
#ifndef OC_FUZZ_FRAMED_ITERS
#define OC_FUZZ_FRAMED_ITERS 15000
#endif

/* xorshift32 with a fixed seed — deterministic across runs/platforms. */
static uint32_t g_fz = 0x9e3779b9u;
static uint32_t fz(void) {
    g_fz ^= g_fz << 13; g_fz ^= g_fz >> 17; g_fz ^= g_fz << 5;
    return g_fz;
}

/* Run every payload decoder over `buf` (each on its own reader). None may crash
 * or read past `len`; each must return OK or OC_E_MALFORMED. */
static void decode_all(const uint8_t *buf, size_t len) {
    oc_rbuf p;
    oc_result rc;
    (void)rc;

    #define D(call) do { oc_rbuf_init(&p, buf, len); rc = (call); \
        CHECK(rc == OC_OK || rc == OC_E_MALFORMED); } while (0)

    oc_hello hello; D(oc_decode_hello(&p, &hello));
    oc_welcome wel; D(oc_decode_welcome(&p, &wel));
    oc_reject rej; D(oc_decode_reject(&p, &rej));
    oc_auth_challenge ac; D(oc_decode_auth_challenge(&p, &ac));
    oc_auth au; D(oc_decode_auth(&p, &au));
    oc_auth_ok aok; D(oc_decode_auth_ok(&p, &aok));
    oc_logout lo; D(oc_decode_logout(&p, &lo));
    oc_send s = {0}; D(oc_decode_send(&p, &s));
    oc_send_ack sa; D(oc_decode_send_ack(&p, &sa));
    oc_broadcast b; D(oc_decode_broadcast(&p, &b));
    oc_client_ack ca; D(oc_decode_client_ack(&p, &ca));
    oc_edit ed; D(oc_decode_edit(&p, &ed));
    oc_delete de; D(oc_decode_delete(&p, &de));
    oc_msg_edited me; D(oc_decode_msg_edited(&p, &me));
    oc_msg_deleted md; D(oc_decode_msg_deleted(&p, &md));
    oc_react rc2; D(oc_decode_react(&p, &rc2));
    oc_reaction_updated ru; D(oc_decode_reaction_updated(&p, &ru));
    oc_list_reactions lr; D(oc_decode_list_reactions(&p, &lr));
    oc_send_reply sr; D(oc_decode_send_reply(&p, &sr));
    oc_thread_reply tr; D(oc_decode_thread_reply(&p, &tr));
    oc_list_thread lt; D(oc_decode_list_thread(&p, &lt));
    oc_thread th; D(oc_decode_thread(&p, &th));
    oc_thread_meta tm; D(oc_decode_thread_meta(&p, &tm));
    oc_create_channel cc; D(oc_decode_create_channel(&p, &cc));
    oc_channel_info ci; D(oc_decode_channel_info(&p, &ci));
    oc_channel_ref cr; D(oc_decode_join_channel(&p, &cr));
    oc_channel_member_op cm; D(oc_decode_invite_to_channel(&p, &cm));
    oc_set_role srr; D(oc_decode_set_role(&p, &srr));
    oc_invite_user iu; D(oc_decode_invite_user(&p, &iu));
    oc_remove_user rmu; D(oc_decode_remove_user(&p, &rmu));
    oc_user_updated uu; D(oc_decode_user_updated(&p, &uu));
    oc_invite_created ic; D(oc_decode_invite_created(&p, &ic));
    oc_redeem_invite ri; D(oc_decode_redeem_invite(&p, &ri));
    oc_backfill_done bd; D(oc_decode_backfill_done(&p, &bd));
    oc_error er; D(oc_decode_error(&p, &er));
    oc_open_dm odm; D(oc_decode_open_dm(&p, &odm));
    oc_set_presence sp; D(oc_decode_set_presence(&p, &sp));
    oc_presence_update pu; D(oc_decode_presence_update(&p, &pu));
    oc_typing ty; D(oc_decode_typing(&p, &ty));
    oc_typing_update tu2; D(oc_decode_typing_update(&p, &tu2));
    oc_upload_begin ub; D(oc_decode_upload_begin(&p, &ub));
    oc_upload_ready urd; D(oc_decode_upload_ready(&p, &urd));
    oc_upload_chunk uc; D(oc_decode_upload_chunk(&p, &uc));
    oc_upload_ack uak; D(oc_decode_upload_ack(&p, &uak));
    oc_upload_end ue2; D(oc_decode_upload_end(&p, &ue2));
    oc_upload_ok uok; D(oc_decode_upload_ok(&p, &uok));
    oc_download_begin db; D(oc_decode_download_begin(&p, &db));
    oc_download_info di; D(oc_decode_download_info(&p, &di));
    oc_download_chunk dc; D(oc_decode_download_chunk(&p, &dc));
    oc_download_end den; D(oc_decode_download_end(&p, &den));
    oc_transfer_cancel tc; D(oc_decode_transfer_cancel(&p, &tc));

    /* Array/paging decoders with small caller buffers. */
    { oc_cursor cur[4]; uint16_t nc = 0;
      oc_rbuf_init(&p, buf, len);
      rc = oc_decode_backfill_request(&p, cur, 4, &nc);
      CHECK(rc == OC_OK || rc == OC_E_MALFORMED); }
    { oc_channel_list_entry ce[4]; uint16_t nc = 0;
      oc_rbuf_init(&p, buf, len);
      rc = oc_decode_channel_list(&p, ce, 4, &nc);
      CHECK(rc == OC_OK || rc == OC_E_MALFORMED); }
    { oc_user_list_entry ue[4]; uint16_t nc = 0;
      oc_rbuf_init(&p, buf, len);
      rc = oc_decode_user_list(&p, ue, 4, &nc);
      CHECK(rc == OC_OK || rc == OC_E_MALFORMED); }
    { oc_reaction_entry re[4]; uint16_t nc = 0; uint64_t mid = 0;
      oc_rbuf_init(&p, buf, len);
      rc = oc_decode_reactions(&p, re, 4, &nc, &mid);
      CHECK(rc == OC_OK || rc == OC_E_MALFORMED); }
    { oc_search_result_entry se[4]; uint16_t nc = 0; uint8_t tr2 = 0;
      oc_rbuf_init(&p, buf, len);
      rc = oc_decode_search_results(&p, se, 4, &nc, &tr2);
      CHECK(rc == OC_OK || rc == OC_E_MALFORMED); }

    #undef D
}

static void test_fuzz_random(void) {
    uint8_t buf[600];
    for (int iter = 0; iter < OC_FUZZ_RANDOM_ITERS; iter++) {
        size_t len = fz() % (sizeof buf + 1);   /* 0 .. sizeof buf */
        for (size_t i = 0; i < len; i++) buf[i] = (uint8_t)fz();

        /* Full-frame parse must never crash and must classify cleanly. */
        oc_header hdr; oc_rbuf p;
        oc_result fr = oc_parse_frame(buf, len, &hdr, &p);
        CHECK(fr == OC_OK || fr == OC_E_MALFORMED || fr == OC_E_TOO_LARGE);
        if (fr == OC_OK) {
            /* A parsed frame's payload view must stay inside the buffer. */
            CHECK(p.data >= buf && p.data + p.len <= buf + len);
            decode_all(p.data, p.len);
        }

        /* Also fuzz the decoders directly over the raw bytes. */
        decode_all(buf, len);
    }
}

/* Semi-structured: a valid 8-byte header (plausible length/version/type) over a
 * random payload — reaches deeper into the decoders than pure noise. */
static void test_fuzz_framed(void) {
    static uint8_t buf[OC_MAX_FRAME_SIZE];
    static const uint16_t types[] = {
        0x0001,0x0002,0x0003,0x0010,0x0011,0x0012,0x0013,0x0020,0x0021,0x0022,
        0x0023,0x0024,0x0025,0x0026,0x0027,0x0028,0x0029,0x002A,0x002B,0x002C,
        0x002D,0x002E,0x002F,0x0032,0x0030,0x0031,0x0040,0x0041,0x0042,0x0043,
        0x0044,0x0045,0x0046,0x0047,0x0050,0x0051,0x0052,0x0053,0x0054,0x0055,
        0x0056,0x0057,0x0060,0x0061,0x00FF
    };
    for (int iter = 0; iter < OC_FUZZ_FRAMED_ITERS; iter++) {
        size_t paylen = fz() % 200;
        uint32_t length = (uint32_t)(paylen + 4);   /* version(2)+type(2)+payload */
        buf[0] = (uint8_t)(length >> 24); buf[1] = (uint8_t)(length >> 16);
        buf[2] = (uint8_t)(length >> 8);  buf[3] = (uint8_t)length;
        buf[4] = 0; buf[5] = 1;                       /* version 1 */
        uint16_t t = types[fz() % (sizeof types / sizeof types[0])];
        buf[6] = (uint8_t)(t >> 8); buf[7] = (uint8_t)t;
        for (size_t i = 0; i < paylen; i++) buf[8 + i] = (uint8_t)fz();

        oc_header hdr; oc_rbuf p;
        oc_result fr = oc_parse_frame(buf, 8 + paylen, &hdr, &p);
        CHECK(fr == OC_OK || fr == OC_E_MALFORMED || fr == OC_E_TOO_LARGE);
        if (fr == OC_OK) {
            CHECK(p.data + p.len <= buf + 8 + paylen);
            decode_all(p.data, p.len);
        }
    }
}

int run_fuzz_tests(void) {
    printf("test_fuzz: %d random + %d framed iterations through parse + all decoders\n",
           OC_FUZZ_RANDOM_ITERS, OC_FUZZ_FRAMED_ITERS);
    test_fuzz_random();
    test_fuzz_framed();
    return failures;
}
