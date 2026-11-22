/* Image previews (ARCH-137, daemon/preview.c).
 *
 * What is proved here, on images made in memory (tests/preview_samples.c):
 *   - a JPEG, PNG or WebP larger than the inline box gets both rungs, in its
 *     own format, each fitted to its box with the aspect kept;
 *   - a small image gets none;
 *   - GIF, HEIC and an animated PNG pass through;
 *   - each bound refuses a crafted input: the byte cap, the pixel cap read
 *     from a header that claims a huge image (allocating nothing for it), and
 *     the time limit on an injected clock that moves;
 *   - damaged input is refused, never crashed on;
 *   - the transfer worker's job reads an original from the store, makes and
 *     stores both rungs, and stores nothing for an image that gets none;
 *   - the rows: written only while the attachment stands, served behind the
 *     download's gate, kept from the orphan sweep, reclaimed under pressure
 *     before any attachment and with no tombstone, and gone with their
 *     attachment. */

#include "preview.h"
#include "preview_samples.h"
#include "check.h"
#include "testenv.h"
#include "dbwriter.h"
#include "xferpool.h"
#include "blobstore.h"
#include "protocol.h"

#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Both rungs made, in `mime`, each one decoding to the size it claims. */
static void check_made(const uint8_t *img, size_t len, uint32_t w, uint32_t h, const char *mime) {
    oc_preview_limits lim;
    oc_preview_limits_default(&lim);
    oc_preview_set s;
    CHECK(oc_preview_make(img, len, &lim, &s) == OC_PREVIEW_MADE);
    CHECK(s.width == w && s.height == h);
    uint32_t want_w[2], want_h[2];
    oc_preview_fit(w, h, lim.inline_px, &want_w[0], &want_h[0]);
    oc_preview_fit(w, h, lim.expanded_px, &want_w[1], &want_h[1]);
    for (int i = 0; i < OC_PREVIEW_RUNGS; i++) {
        CHECK(s.rung[i].data && s.rung[i].len > 0);
        CHECK(s.rung[i].mime && strcmp(s.rung[i].mime, mime) == 0);
        CHECK(s.rung[i].width == want_w[i] && s.rung[i].height == want_h[i]);
        uint32_t dw = 0, dh = 0;
        CHECK(oc_preview_probe(s.rung[i].data, s.rung[i].len, &dw, &dh) == 0);
        CHECK(dw == want_w[i] && dh == want_h[i]);
        oc_preview_set again;     /* a rung decodes: previewing it is no error */
        oc_preview_status st = oc_preview_make(s.rung[i].data, s.rung[i].len, &lim, &again);
        CHECK(st == OC_PREVIEW_MADE || st == OC_PREVIEW_SMALL);
        oc_preview_set_free(&again);
    }
    CHECK(s.rung[0].len < len);   /* the point of it */
    oc_preview_set_free(&s);
}

static void test_fit(void) {
    uint32_t w, h;
    oc_preview_fit(4000, 3000, 480, &w, &h);  CHECK(w == 480 && h == 360);
    oc_preview_fit(3000, 4000, 480, &w, &h);  CHECK(w == 360 && h == 480);
    oc_preview_fit(300, 200, 480, &w, &h);    CHECK(w == 300 && h == 200);   /* never up */
    oc_preview_fit(10000, 1, 480, &w, &h);    CHECK(w == 480 && h == 1);     /* never below 1 */
}

static void test_sniff(void) {
    size_t n;
    uint8_t *j = sample_jpeg(64, 64, &n);
    CHECK(j && oc_preview_sniff(j, n) == OC_IMG_JPEG);
    free(j);
    uint8_t *p = sample_png(64, 64, 0, &n);
    CHECK(p && oc_preview_sniff(p, n) == OC_IMG_PNG);
    free(p);
    uint8_t *w = sample_webp(64, 64, 0, &n);
    CHECK(w && oc_preview_sniff(w, n) == OC_IMG_WEBP);
    free(w);
    CHECK(oc_preview_sniff((const uint8_t *)"GIF89a\x01\x00", 8) == OC_IMG_GIF);
    CHECK(oc_preview_sniff((const uint8_t *)"\0\0\0\x18" "ftypheic\0\0\0\0", 16) == OC_IMG_HEIC);
    CHECK(oc_preview_sniff((const uint8_t *)"hello", 5) == OC_IMG_UNKNOWN);
}

static void test_made_per_format(void) {
    size_t n;
    uint8_t *img = sample_jpeg(2000, 1500, &n);
    CHECK(img != NULL);
    if (img) check_made(img, n, 2000, 1500, "image/jpeg");
    free(img);

    img = sample_png(1800, 900, 1, &n);
    CHECK(img != NULL);
    if (img) {
        check_made(img, n, 1800, 900, "image/png");
        /* The alpha stays: the rung is colour type 6, RGBA. */
        oc_preview_set s;
        CHECK(oc_preview_make(img, n, NULL, &s) == OC_PREVIEW_MADE);
        CHECK(s.rung[0].len > 25 && s.rung[0].data[25] == 6);
        oc_preview_set_free(&s);
    }
    free(img);

    img = sample_png(1000, 700, 0, &n);
    CHECK(img != NULL);
    if (img) {
        oc_preview_set s;
        CHECK(oc_preview_make(img, n, NULL, &s) == OC_PREVIEW_MADE);
        CHECK(s.rung[0].len > 25 && s.rung[0].data[25] == 2);   /* RGB stays RGB */
        oc_preview_set_free(&s);
    }
    free(img);

    img = sample_webp(1200, 900, 0, &n);
    CHECK(img != NULL);
    if (img) check_made(img, n, 1200, 900, "image/webp");
    free(img);

    img = sample_webp(1200, 900, 1, &n);
    CHECK(img != NULL);
    if (img) {
        oc_preview_set s;
        CHECK(oc_preview_make(img, n, NULL, &s) == OC_PREVIEW_MADE);
        /* Lossless in, lossless out: the VP8L chunk. */
        CHECK(s.rung[0].len > 16 && memcmp(s.rung[0].data + 12, "VP8L", 4) == 0);
        oc_preview_set_free(&s);
    }
    free(img);
}

static void test_small_gets_none(void) {
    size_t n;
    uint8_t *img = sample_png(200, 120, 0, &n);
    oc_preview_set s;
    CHECK(img && oc_preview_make(img, n, NULL, &s) == OC_PREVIEW_SMALL);
    CHECK(s.rung[0].data == NULL && s.rung[1].data == NULL);
    CHECK(s.width == 200 && s.height == 120);
    free(img);
    img = sample_jpeg(480, 480, &n);
    CHECK(img && oc_preview_make(img, n, NULL, &s) == OC_PREVIEW_SMALL);
    free(img);
}

static void test_passthrough(void) {
    oc_preview_set s;
    static const uint8_t gif[] = "GIF89a\x10\x00\x10\x00\x80\x00\x00\xff\xff\xff\x00\x00\x00,"
                                 "\x00\x00\x00\x00\x10\x00\x10\x00\x00\x02\x02\x44\x01\x00;";
    CHECK(oc_preview_make(gif, sizeof gif - 1, NULL, &s) == OC_PREVIEW_PASSTHROUGH);
    CHECK(s.format == OC_IMG_GIF && !s.rung[0].data);
    static const uint8_t heic[] = "\0\0\0\x18" "ftypheic\0\0\0\0mif1heic";
    CHECK(oc_preview_make(heic, sizeof heic - 1, NULL, &s) == OC_PREVIEW_PASSTHROUGH);
    CHECK(s.format == OC_IMG_HEIC);
    CHECK(oc_preview_make((const uint8_t *)"%PDF-1.7", 8, NULL, &s) == OC_PREVIEW_PASSTHROUGH);

    /* An animated PNG: an acTL chunk put in after the IHDR. */
    size_t n;
    uint8_t *png = sample_png(1200, 800, 0, &n);
    CHECK(png != NULL);
    if (png) {
        static const uint8_t actl[20] = { 0, 0, 0, 8, 'a', 'c', 'T', 'L', 0, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0 };
        uint8_t *apng = malloc(n + sizeof actl);
        memcpy(apng, png, 33);                      /* signature + IHDR */
        memcpy(apng + 33, actl, sizeof actl);
        memcpy(apng + 33 + sizeof actl, png + 33, n - 33);
        CHECK(oc_preview_make(apng, n + sizeof actl, NULL, &s) == OC_PREVIEW_PASSTHROUGH);
        free(apng);
    }
    free(png);
}

static void test_byte_cap(void) {
    size_t n;
    uint8_t *img = sample_jpeg(1600, 1200, &n);
    oc_preview_limits lim;
    oc_preview_limits_default(&lim);
    lim.max_bytes = n - 1;
    oc_preview_set s;
    CHECK(img && oc_preview_make(img, n, &lim, &s) == OC_PREVIEW_TOO_BIG);
    CHECK(!s.rung[0].data && !s.rung[1].data);
    lim.max_bytes = n;
    CHECK(img && oc_preview_make(img, n, &lim, &s) == OC_PREVIEW_MADE);
    oc_preview_set_free(&s);
    free(img);
}

/* Decompression bombs: a header that claims more pixels than the cap, on a
 * file of a few hundred bytes. Refused from the header; nothing the size of
 * the claim is allocated (a 60000 x 60000 RGBA buffer is 14 GB, so a decode
 * that tried would fail here as out of memory, not as over the cap). */
static void test_pixel_cap(void) {
    oc_preview_set s;
    size_t n;
    uint8_t *png = sample_png(600, 600, 0, &n);
    CHECK(png != NULL);
    if (png) {
        png[16] = 0; png[17] = 0; png[18] = 0xEA; png[19] = 0x60;   /* 60000 wide */
        png[20] = 0; png[21] = 0; png[22] = 0xEA; png[23] = 0x60;   /* 60000 high */
        CHECK(be32(png + 16) == 60000);
        CHECK(oc_preview_make(png, n, NULL, &s) == OC_PREVIEW_TOO_MANY_PX);
        CHECK(s.width == 60000 && s.height == 60000);
    }
    free(png);

    uint8_t *jpg = sample_jpeg(600, 600, &n);
    CHECK(jpg != NULL);
    if (jpg) {
        /* The SOF0 marker's height and width, both 65000. */
        for (size_t i = 0; i + 9 < n; i++)
            if (jpg[i] == 0xFF && jpg[i + 1] == 0xC0) {
                jpg[i + 5] = 0xFD; jpg[i + 6] = 0xE8;
                jpg[i + 7] = 0xFD; jpg[i + 8] = 0xE8;
                break;
            }
        CHECK(oc_preview_make(jpg, n, NULL, &s) == OC_PREVIEW_TOO_MANY_PX);
        CHECK(s.width == 65000 && s.height == 65000);
    }
    free(jpg);

    /* At the cap exactly it is made; one pixel over, refused. */
    oc_preview_limits lim;
    oc_preview_limits_default(&lim);
    uint8_t *img = sample_png(1000, 500, 0, &n);
    lim.max_pixels = 1000 * 500;
    CHECK(img && oc_preview_make(img, n, &lim, &s) == OC_PREVIEW_MADE);
    oc_preview_set_free(&s);
    lim.max_pixels = 1000 * 500 - 1;
    CHECK(img && oc_preview_make(img, n, &lim, &s) == OC_PREVIEW_TOO_MANY_PX);
    free(img);
}

/* A clock that moves a second every time it is read. */
static uint64_t racing(void *ctx) {
    uint64_t *t = ctx;
    return *t += 1000;
}

static void test_time_limit(void) {
    size_t n;
    uint8_t *imgs[3];
    size_t lens[3];
    imgs[0] = sample_jpeg(1600, 1200, &n); lens[0] = n;
    imgs[1] = sample_png(1600, 1200, 0, &n); lens[1] = n;
    imgs[2] = sample_webp(1600, 1200, 0, &n); lens[2] = n;
    for (int i = 0; i < 3; i++) {
        CHECK(imgs[i] != NULL);
        if (!imgs[i]) continue;
        uint64_t t = 0;
        oc_preview_limits lim;
        oc_preview_limits_default(&lim);
        lim.timeout_ms = 5000;
        lim.clock_ms = racing;
        lim.clock_ctx = &t;
        oc_preview_set s;
        CHECK(oc_preview_make(imgs[i], lens[i], &lim, &s) == OC_PREVIEW_TIMED_OUT);
        CHECK(!s.rung[0].data && !s.rung[1].data);
        CHECK(t <= 1000u * 20);     /* given up within a few looks past the limit */
        free(imgs[i]);
    }
}

static void test_damaged(void) {
    size_t n;
    oc_preview_set s;
    uint8_t *j = sample_jpeg(1200, 900, &n);
    CHECK(j && oc_preview_make(j, n / 3, NULL, &s) == OC_PREVIEW_MALFORMED);   /* truncated */
    free(j);
    uint8_t *p = sample_png(1200, 900, 0, &n);
    if (p) {
        for (size_t i = 60; i < n && i < 400; i++) p[i] ^= 0x5A;              /* bad IDAT */
        CHECK(oc_preview_make(p, n, NULL, &s) == OC_PREVIEW_MALFORMED);
    }
    free(p);
    uint8_t *w = sample_webp(1200, 900, 0, &n);
    CHECK(w && oc_preview_make(w, n / 2, NULL, &s) == OC_PREVIEW_MALFORMED);
    free(w);
    CHECK(oc_preview_make((const uint8_t *)"\xFF\xD8\xFF", 3, NULL, &s) == OC_PREVIEW_MALFORMED);
    CHECK(oc_preview_make((const uint8_t *)"\x89PNG\r\n\x1a\n", 8, NULL, &s) == OC_PREVIEW_MALFORMED);
}

/* ---- the transfer worker's job ---------------------------------------------- */

static oc_xfer_job *await_job(oc_xferpool *p) {
    oc_xfer_job *j = NULL;
    (void)WAIT_UNTIL((j = oc_xferpool_next_result(p)) != NULL, 10000);
    return j;
}

static int put_blob(oc_blobstore *bs, const char *key, const uint8_t *d, size_t n) {
    oc_blob_writer *bw = oc_blob_put_begin(bs, key, n);
    if (!bw) return -1;
    if (oc_blob_put_chunk(bw, d, n) != 0) { oc_blob_put_abort(bw); return -1; }
    return oc_blob_put_commit(bw);
}

static oc_xfer_job *preview_job(const char *key) {
    oc_xfer_job *j = oc_xfer_job_new(OC_XFER_PREVIEW, 0);
    j->attachment_id = 42;
    j->key = strdup(key);
    j->pv = calloc(1, sizeof *j->pv);
    oc_preview_limits_default(&j->pv->lim);
    for (int i = 0; i < OC_PREVIEW_RUNGS; i++) {
        j->pv->keys[i] = malloc(strlen(key) + 3);
        sprintf(j->pv->keys[i], "%s-%d", key, i + 1);
    }
    return j;
}

static void test_worker(void) {
    char dir[256];
    CHECK(oc_test_mkdtemp("preview-pool", dir, sizeof dir) == 0);
    oc_blobstore *bs = oc_blobstore_open(dir, NULL);
    CHECK(bs != NULL);
    oc_xferpool *p = bs ? oc_xferpool_start(bs, 2) : NULL;
    CHECK(p != NULL);
    if (!p) { oc_blobstore_close(bs); oc_test_rmtree(dir); return; }
    size_t n;
    uint8_t *big = sample_jpeg(2000, 1500, &n);
    CHECK(big && put_blob(bs, "aa01", big, n) == 0);
    free(big);

    /* Made: both rungs stored under their keys; the result comes back though
     * the job belongs to no connection. */
    oc_xferpool_submit(p, preview_job("aa01"));
    oc_xfer_job *j = await_job(p);
    CHECK(j && j->op == OC_XFER_PREVIEW && j->rc == 0 && j->pv);
    if (j && j->pv) {
        CHECK(j->pv->status == OC_PREVIEW_MADE && j->pv->stored);
        CHECK(j->pv->width == 2000 && j->pv->height == 1500);
        CHECK(j->pv->rung_w[0] == 480 && j->pv->rung_h[0] == 360);
        CHECK(j->pv->rung_w[1] == 1600 && j->pv->rung_h[1] == 1200);
        for (int i = 0; i < OC_PREVIEW_RUNGS; i++) {
            uint64_t sz = 0;
            oc_blob_reader *br = oc_blob_get_begin(bs, j->pv->keys[i], &sz);
            CHECK(br && sz == j->pv->rung_size[i] && sz > 0);
            if (br) oc_blob_get_close(br);
            CHECK(j->pv->rung_mime[i] && strcmp(j->pv->rung_mime[i], "image/jpeg") == 0);
        }
    }
    oc_xfer_job_free(j);

    /* A small image: nothing stored, its size still known. */
    uint8_t *small = sample_png(100, 80, 0, &n);
    CHECK(small && put_blob(bs, "aa02", small, n) == 0);
    free(small);
    oc_xferpool_submit(p, preview_job("aa02"));
    j = await_job(p);
    CHECK(j && j->pv && j->pv->status == OC_PREVIEW_SMALL && !j->pv->stored);
    CHECK(j && j->pv && j->pv->width == 100 && j->pv->height == 80);
    uint64_t sz;
    oc_blob_reader *br = oc_blob_get_begin(bs, "aa02-1", &sz);
    CHECK(br == NULL);
    if (br) oc_blob_get_close(br);
    oc_xfer_job_free(j);

    /* Over the byte cap: refused before a byte of it is read. */
    big = sample_png(1200, 900, 0, &n);
    CHECK(big && put_blob(bs, "aa03", big, n) == 0);
    free(big);
    oc_xfer_job *cj = preview_job("aa03");
    cj->pv->lim.max_bytes = 1024;
    oc_xferpool_submit(p, cj);
    j = await_job(p);
    CHECK(j && j->pv && j->pv->status == OC_PREVIEW_TOO_BIG && !j->pv->stored);
    oc_xfer_job_free(j);

    /* No such original: an error, and nothing stored. */
    oc_xferpool_submit(p, preview_job("aa04"));
    j = await_job(p);
    CHECK(j && j->rc != 0 && j->pv && !j->pv->stored);
    oc_xfer_job_free(j);

    oc_xferpool_stop(p);
    oc_blobstore_close(bs);
    oc_test_rmtree(dir);
}

/* ---- the rows ---------------------------------------------------------------- */

static oc_dbres *db_wait(oc_dbwriter *w) {
    oc_dbres *r = NULL;
    (void)WAIT_UNTIL((r = oc_dbwriter_next_result(w)) != NULL, 5000);
    return r;
}

static void db_exec(sqlite3 *db, const char *sql) { CHECK(sqlite3_exec(db, sql, NULL, NULL, NULL) == SQLITE_OK); }

static int64_t db_scalar(sqlite3 *db, const char *sql) {
    sqlite3_stmt *st = NULL;
    int64_t v = -1;
    if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK && sqlite3_step(st) == SQLITE_ROW) v = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return v;
}

/* PREVIEW_STORE of both rungs of `aid`, keyed `key`-1 and `key`-2: the result. */
static oc_dbres *store_rows(oc_dbwriter *w, uint64_t aid, const char *key) {
    oc_job *j = oc_job_new(OC_JOB_PREVIEW_STORE, 0);
    j->attachment_id = aid;
    j->pv_width = 4000; j->pv_height = 3000;
    j->pv_count = 2;
    for (int i = 0; i < 2; i++) {
        j->pv_keys[i] = malloc(strlen(key) + 3);
        sprintf(j->pv_keys[i], "%s-%d", key, i + 1);
        j->pv_mime[i] = strdup("image/jpeg");
        j->pv_w[i] = i ? 1600 : 480; j->pv_h[i] = i ? 1200 : 360;
        j->pv_size[i] = i ? 90000 : 9000;
    }
    oc_dbwriter_submit(w, j);
    return db_wait(w);
}

static oc_dbres *lookup(oc_dbwriter *w, uint64_t user, uint64_t aid, uint8_t rung) {
    oc_job *j = oc_job_new(OC_JOB_PREVIEW_LOOKUP, 21);
    j->user_id = user; j->attachment_id = aid; j->pv_rung = rung;
    oc_dbwriter_submit(w, j);
    return db_wait(w);
}

static int lookup_err(oc_dbwriter *w, uint64_t user, uint64_t aid, uint8_t rung) {
    oc_dbres *r = lookup(w, user, aid, rung);
    int e = !r ? -1 : r->type == OC_RES_PREVIEW_ERR ? (int)r->err_code : 0;
    if (r) oc_dbres_free(r);
    return e;
}

/* A storage pass reclaiming at most `batch`: whether `key` was reclaimed, and
 * where in the pass's list. */
static int maint_reclaims(oc_dbwriter *w, int evict, size_t batch, const char *key, size_t *at) {
    oc_job *j = oc_job_new(OC_JOB_STORAGE_MAINT, 0);
    j->maint_grace_ms = 60ull * 60 * 1000; j->maint_batch = batch; j->maint_evict = evict;
    oc_dbwriter_submit(w, j);
    oc_dbres *r = db_wait(w);
    CHECK(r && r->type == OC_RES_STORAGE_MAINT);
    int found = 0;
    for (size_t i = 0; r && i < r->n_reclaim && !found; i++)
        if (r->reclaim[i].storage_key && strcmp(r->reclaim[i].storage_key, key) == 0) { found = 1; if (at) *at = i; }
    if (r) oc_dbres_free(r);
    return found;
}

static void test_rows(void) {
    char dir[256], dbp[512];
    CHECK(oc_test_mkdtemp("preview-rows", dir, sizeof dir) == 0);
    oc_test_path(dbp, sizeof dbp, dir, "previews.db");
    oc_dbwriter *w = oc_dbwriter_start(dbp);
    CHECK(w != NULL);
    if (!w) { oc_test_rmtree(dir); return; }
    oc_dbwriter_set_pw_mem(w, 2048);
    uint64_t alice = oc_dbwriter_register_local(w, "pv-alice", "pw", OC_ROLE_OWNER, 1000);
    uint64_t bob   = oc_dbwriter_register_local(w, "pv-bob", "pw", OC_ROLE_MEMBER, 1000);
    CHECK(alice && bob);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(dbp, &db) == SQLITE_OK);
    sqlite3_busy_timeout(db, 5000);
    char sql[1024];
    /* 400: a sent picture in the general channel; 401: one in a private
     * channel bob is not in; 402: a picture with no previews. */
    db_exec(db, "INSERT INTO channels(id, kind, name, is_public, created_at_ms) VALUES(77, 'channel', 'private', 0, 1);");
    snprintf(sql, sizeof sql, "INSERT INTO channel_members(channel_id, user_id, joined_at_ms) VALUES(77, %llu, 1);",
             (unsigned long long)alice);
    db_exec(db, sql);
    snprintf(sql, sizeof sql,
             "INSERT INTO messages(id, channel_id, author_id, body, created_at_ms) VALUES(9101, %d, %llu, 'pic', 1);"
             "INSERT INTO messages(id, channel_id, author_id, body, created_at_ms) VALUES(9102, 77, %llu, 'pic', 1);"
             "INSERT INTO attachments(id, channel_id, message_id, uploader_id, storage_key, filename, mime, size,"
             " sha256, created_at_ms) VALUES"
             " (400, %d, 9101, %llu, '0190', 'a.jpg', 'image/jpeg', 5000000, x'00', 1),"
             " (401, 77, 9102, %llu, '0191', 'b.jpg', 'image/jpeg', 5000000, x'00', 1),"
             " (402, %d, 9101, %llu, '0192', 'c.jpg', 'image/jpeg', 5000, x'00', 1);",
             OC_DEFAULT_CHANNEL, (unsigned long long)alice, (unsigned long long)alice,
             OC_DEFAULT_CHANNEL, (unsigned long long)alice, (unsigned long long)alice,
             OC_DEFAULT_CHANNEL, (unsigned long long)alice);
    db_exec(db, sql);

    /* Stored: the rows, and the original's size on the attachment. */
    oc_dbres *r = store_rows(w, 400, "0190");
    CHECK(r && r->type == OC_RES_PREVIEW_STORED && !r->pv_drop[0] && !r->pv_drop[1]);
    if (r) oc_dbres_free(r);
    r = store_rows(w, 401, "0191");
    if (r) oc_dbres_free(r);
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM attachment_previews WHERE attachment_id = 400;") == 2);
    CHECK(db_scalar(db, "SELECT width * 10000 + height FROM attachments WHERE id = 400;") == 40003000);

    /* Served behind the download's gate; a rung never made is PREVIEW_MISSING. */
    r = lookup(w, bob, 400, 1);
    CHECK(r && r->type == OC_RES_PREVIEW_META && r->pv_key && strcmp(r->pv_key, "0190-1") == 0);
    CHECK(r && r->pv_w == 480 && r->pv_h == 360 && r->pv_size == 9000 && r->pv_mime &&
          strcmp(r->pv_mime, "image/jpeg") == 0);
    if (r) oc_dbres_free(r);
    r = lookup(w, bob, 400, 2);
    CHECK(r && r->type == OC_RES_PREVIEW_META && r->pv_key && strcmp(r->pv_key, "0190-2") == 0 && r->pv_w == 1600);
    if (r) oc_dbres_free(r);
    CHECK(lookup_err(w, bob, 401, 1) == OC_ERR_FORBIDDEN);
    CHECK(lookup_err(w, alice, 401, 1) == 0);
    CHECK(lookup_err(w, bob, 402, 1) == OC_ERR_PREVIEW_MISSING);
    CHECK(lookup_err(w, bob, 999, 1) == OC_ERR_UNKNOWN_ATTACHMENT);

    /* Serving a rung makes it recently used. */
    db_exec(db, "UPDATE attachment_previews SET last_used_ms = 5;");
    oc_job *j = oc_job_new(OC_JOB_PREVIEW_TOUCH, 0);
    j->attachment_id = 400; j->pv_rung = 1;
    oc_dbwriter_submit(w, j);
    oc_dbres *tr = db_wait(w);
    if (tr) oc_dbres_free(tr);
    CHECK(db_scalar(db, "SELECT last_used_ms FROM attachment_previews WHERE attachment_id = 400 AND rung = 1;") > 5);
    CHECK(db_scalar(db, "SELECT last_used_ms FROM attachment_previews WHERE attachment_id = 400 AND rung = 2;") == 5);

    /* The orphan sweep leaves a preview's blob alone while its row stands. */
    uint8_t ref[2] = { 0, 0 };
    const char *keys[2] = { "0190-1", "0190-9" };
    CHECK(oc_db_blob_referenced(dbp, keys, 2, ref) == 0 && ref[0] == 1 && ref[1] == 0);

    /* Without pressure the previews stay; under pressure the least recently
     * served goes first -- before any attachment, which stays, untombstoned. */
    CHECK(!maint_reclaims(w, 0, 64, "0190-2", NULL));
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM attachment_previews;") == 4);
    size_t at = 99;
    CHECK(maint_reclaims(w, 1, 1, "0190-2", &at) && at == 0);
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM attachment_previews WHERE attachment_id = 400 AND rung = 2;") == 0);
    CHECK(db_scalar(db, "SELECT reclaimed_at_ms FROM attachments WHERE id = 400;") == 0);
    CHECK(lookup_err(w, bob, 400, 2) == OC_ERR_PREVIEW_MISSING);   /* the client falls back to the original */
    CHECK(lookup_err(w, bob, 400, 1) == 0);

    /* They go with their attachment, pressure or not; and previews finished
     * after it went are not recorded, their keys handed back for deletion. */
    db_exec(db, "UPDATE attachments SET reclaimed_at_ms = 1, reclaim_reason = 1 WHERE id = 400;");
    CHECK(maint_reclaims(w, 0, 64, "0190-1", NULL));
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM attachment_previews WHERE attachment_id = 400;") == 0);
    CHECK(lookup_err(w, bob, 400, 1) == OC_ERR_ATTACHMENT_GONE);
    r = store_rows(w, 400, "0190");
    CHECK(r && r->type == OC_RES_PREVIEW_STORED && r->pv_drop[0] && r->pv_drop[1] &&
          strcmp(r->pv_drop[0], "0190-1") == 0 && strcmp(r->pv_drop[1], "0190-2") == 0);
    if (r) oc_dbres_free(r);
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM attachment_previews WHERE attachment_id = 400;") == 0);

    sqlite3_close(db);
    oc_dbwriter_stop(w);
    oc_test_rmtree(dir);
}

int run_preview_tests(void) {
    printf("test_preview: the rungs per format, the fit, none for a small image, passthrough, "
           "the byte, pixel and time bounds, damaged input; the worker's job; the rows -- the gate, touch, "
           "kept from the orphan sweep, reclaimed first under pressure and with their attachment\n");
    test_fit();
    test_sniff();
    test_made_per_format();
    test_small_gets_none();
    test_passthrough();
    test_byte_cap();
    test_pixel_cap();
    test_time_limit();
    test_damaged();
    test_worker();
    test_rows();
    return failures;
}
