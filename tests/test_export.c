/* Tests for the export of a video message (ARCH-135, docs/details/VIDEO-MESSAGES.md
 * "Export"): the file-backed H.264 + MP3 MP4 writer. */
#define _POSIX_C_SOURCE 200809L
#include "check.h"
#include "testenv.h"
#include "oc_mp4.h"
#include "export_render.h"
#include "export_worker.h"
#include "blobstore.h"
#include "dbwriter.h"
#include "protocol.h"

#include <sqlite3.h>
#include <sys/time.h>

#include <dirent.h>
#include <poll.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ---- reading back what the writer wrote ------------------------------------------ */

static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); b = NULL; }
    fclose(f);
    *len = b ? (size_t)n : 0;
    return b;
}

/* The first box of `type` anywhere under [p, p+n), searching the containers
 * this writer nests; its payload, or NULL. */
static const uint8_t *find_box(const uint8_t *p, size_t n, const char *type, size_t *out_len) {
    static const char *const containers[] = { "moov", "trak", "mdia", "minf", "stbl", "edts" };
    size_t at = 0;
    while (n - at >= 8) {
        uint32_t sz = be32(p + at);
        if (sz < 8 || sz > n - at) return NULL;
        if (memcmp(p + at + 4, type, 4) == 0) { *out_len = sz - 8; return p + at + 8; }
        for (size_t i = 0; i < sizeof containers / sizeof containers[0]; i++)
            if (memcmp(p + at + 4, containers[i], 4) == 0) {
                const uint8_t *r = find_box(p + at + 8, sz - 8, type, out_len);
                if (r) return r;
            }
        at += sz;
    }
    return NULL;
}

/* A box inside a sample entry, which has fields before its children: found by
 * its type and checked by its size. */
static const uint8_t *find_in_entry(const uint8_t *p, size_t n, const char *type, size_t *out_len) {
    for (size_t i = 4; i + 4 <= n; i++)
        if (memcmp(p + i, type, 4) == 0) {
            uint32_t sz = be32(p + i - 4);
            if (sz >= 8 && sz - 4 <= n - i) { *out_len = sz - 8; return p + i + 4; }
        }
    return NULL;
}

/* ---- the writer -------------------------------------------------------------------- */

static const uint8_t SPS[] = { 0x67, 0x42, 0xC0, 0x1F, 0xDA, 0x01, 0x40, 0x16 };
static const uint8_t PPS[] = { 0x68, 0xCE, 0x3C, 0x80 };

/* An access unit in Annex B form: optionally the parameter sets, then a slice
 * of `slice_len` bytes, IDR or not. */
static size_t make_au(uint8_t *out, int with_params, int idr, size_t slice_len) {
    size_t n = 0;
    static const uint8_t sc4[] = { 0, 0, 0, 1 }, sc3[] = { 0, 0, 1 };
    memcpy(out + n, sc4, 4); n += 4; out[n++] = 0x09; out[n++] = 0xF0;   /* an AUD, dropped */
    if (with_params) {
        memcpy(out + n, sc4, 4); n += 4; memcpy(out + n, SPS, sizeof SPS); n += sizeof SPS;
        memcpy(out + n, sc3, 3); n += 3; memcpy(out + n, PPS, sizeof PPS); n += sizeof PPS;
    }
    memcpy(out + n, sc4, 4); n += 4;
    out[n++] = idr ? 0x65 : 0x41;
    for (size_t i = 1; i < slice_len; i++) out[n++] = (uint8_t)(0x10 + i % 0x40);   /* no zero runs */
    return n;
}

static void test_writer(void) {
    char dir[256], path[512];
    CHECK(oc_test_mkdtemp("export", dir, sizeof dir) == 0);
    oc_test_path(path, sizeof path, dir, "copy.mp4");

    oc_mp4_export *x = oc_mp4_export_open(path, 640, 360, 48000);
    CHECK(x != NULL);
    if (!x) { oc_test_rmtree(dir); return; }
    uint8_t au[256], mp3[64];
    memset(mp3, 0x5A, sizeof mp3);
    /* A first unit without an IDR slice is refused. */
    size_t n = make_au(au, 1, 0, 20);
    oc_mp4_export *y = oc_mp4_export_open(path, 640, 360, 48000);
    CHECK(y && oc_mp4_export_video(y, au, n, 0) == -1);
    oc_mp4_export_abort(y);
    struct stat st;
    CHECK(stat(path, &st) != 0);                           /* abort removes the file */

    x = oc_mp4_export_open(path, 640, 360, 48000);
    CHECK(x != NULL);
    if (!x) { oc_test_rmtree(dir); return; }
    for (int i = 0; i < 60; i++) {
        n = make_au(au, i % 30 == 0, i % 30 == 0, 40 + (size_t)i);
        CHECK(oc_mp4_export_video(x, au, n, (int64_t)i * 33333) == 0);
    }
    for (int i = 0; i < 84; i++) CHECK(oc_mp4_export_audio(x, mp3, sizeof mp3, 1152) == 0);
    /* Backwards time is refused. */
    n = make_au(au, 0, 0, 10);
    CHECK(oc_mp4_export_video(x, au, n, 0) == -1);
    uint64_t bytes = 0;
    uint32_t ms = 0;
    CHECK(oc_mp4_export_finish(x, 1105, 96000, &bytes, &ms) == 0);
    CHECK(ms == 2000);                                     /* the audio's 2 s outlasts 60 frames */

    size_t len = 0;
    uint8_t *f = slurp(path, &len);
    CHECK(f && len == bytes);
    if (!f) { oc_test_rmtree(dir); return; }
    /* ftyp, then mdat sized to its samples, then moov. */
    CHECK(memcmp(f + 4, "ftyp", 4) == 0);
    size_t at = be32(f);
    CHECK(memcmp(f + at + 4, "mdat", 4) == 0);
    size_t mdat_len = be32(f + at);
    size_t moov_at = at + mdat_len;
    CHECK(moov_at + 8 <= len && memcmp(f + moov_at + 4, "moov", 4) == 0);
    CHECK(moov_at + be32(f + moov_at) == len);

    const uint8_t *moov = f + moov_at + 8;
    size_t moov_len = be32(f + moov_at) - 8, bl = 0;
    /* The parameter sets are the sample entry's, from the SPS. */
    const uint8_t *avcc = find_in_entry(moov, moov_len, "avcC", &bl);
    CHECK(avcc && bl >= 7 && avcc[1] == 0x42 && avcc[2] == 0xC0 && avcc[3] == 0x1F && avcc[4] == 0xFF);
    CHECK(avcc && (avcc[5] & 0x1F) == 1 && be32(avcc + 6) >> 16 == sizeof SPS);
    /* MP3 is object type 0x6B. */
    const uint8_t *esds = find_in_entry(moov, moov_len, "esds", &bl);
    CHECK(esds && bl > 12 && esds[4] == 0x03 && esds[9] == 0x04 && esds[11] == 0x6B);
    /* Two sync samples, the IDRs; the first stco entry points at a length-prefixed IDR slice. */
    const uint8_t *stss = find_box(moov, moov_len, "stss", &bl);
    CHECK(stss && be32(stss + 4) == 2 && be32(stss + 8) == 1 && be32(stss + 12) == 31);
    const uint8_t *stco = find_box(moov, moov_len, "stco", &bl);
    CHECK(stco && be32(stco + 4) == 60);
    if (stco) {
        uint32_t off0 = be32(stco + 8), off1 = be32(stco + 12);
        CHECK(off0 + 5 < len && be32(f + off0) == 40 && f[off0 + 4] == 0x65);
        CHECK(off1 + 5 < len && be32(f + off1) == 41 && f[off1 + 4] == 0x41);   /* no AUD, no parameter sets */
    }
    /* The audio's edit skips the delay and plays exactly the 2 s of real audio. */
    const uint8_t *elst = find_box(moov, moov_len, "elst", &bl);
    CHECK(elst && be32(elst + 4) == 1 && be32(elst + 8) == 2000 && be32(elst + 12) == 1105);
    free(f);

    /* An export with no video is not a file. */
    x = oc_mp4_export_open(path, 640, 360, 48000);
    CHECK(x && oc_mp4_export_audio(x, mp3, sizeof mp3, 1152) == 0);
    CHECK(oc_mp4_export_finish(x, 0, 0, &bytes, &ms) == -1);
    CHECK(stat(path, &st) != 0);
    oc_test_rmtree(dir);
}

/* ---- the render, with the stub codecs ------------------------------------------- */

/* A file in the video message profile with `frames` video samples and two Opus
 * packets after each: bytes that are not VP9 or Opus, which the stub never
 * decodes. With `frames` 0, audio alone. */
static uint8_t *make_source(int frames, size_t *len) {
    oc_mp4_writer *w = frames ? oc_mp4_writer_open(320, 240) : oc_mp4_writer_open_audio(48000);
    uint8_t v[100], a[30];
    memset(v, 0xAB, sizeof v);
    memset(a, 0xCD, sizeof a);
    for (int i = 0; i < (frames ? frames : 5); i++) {
        if (frames) oc_mp4_write_video(w, v, 50 + (size_t)i, (int64_t)i * 33333, i % 10 == 0);
        oc_mp4_write_audio(w, a, 20, 960);
        oc_mp4_write_audio(w, a, 21, 960);
    }
    uint8_t *out = NULL;
    uint32_t ms;
    if (oc_mp4_writer_finish(w, &out, len, &ms) != 0) return NULL;
    return out;
}

typedef struct { const uint8_t *d; size_t n, at, fail_after; int read_calls; } mem_src;

/* Short reads on purpose, so the render's own buffering is what is tested. */
static long mem_read(void *ctx, void *buf, size_t cap) {
    mem_src *m = ctx;
    m->read_calls++;
    if (m->fail_after && m->at >= m->fail_after) return -1;
    size_t k = m->n - m->at;
    if (k > cap) k = cap;
    if (k > 777) k = 777;
    memcpy(buf, m->d + m->at, k);
    m->at += k;
    return (long)k;
}

static void test_render(void) {
    char dir[256], path[512];
    CHECK(oc_test_mkdtemp("export-render", dir, sizeof dir) == 0);
    oc_test_path(path, sizeof path, dir, "copy.out");
    size_t len = 0;
    uint8_t *src = make_source(30, &len);
    CHECK(src != NULL);
    if (!src) { oc_test_rmtree(dir); return; }

    /* Every sample reaches the codecs once, in the file's order, with its time. */
    mem_src m = { src, len, 0, 0, 0 };
    uint64_t bytes = 0;
    uint32_t ms = 0;
    char err[200] = "";
    CHECK(oc_export_render(&oc_export_stub_codecs, mem_read, &m, len, path, NULL, &bytes, &ms, err, sizeof err) == OC_EXPORT_OK);
    CHECK(m.at == len);                                    /* read once, front to back */
    size_t olen = 0;
    uint8_t *out = slurp(path, &olen);
    CHECK(out && olen == bytes);
    if (out) {
        out = realloc(out, olen + 1);
        out[olen] = '\0';
        /* contract: the stub's own first line, its record of the parameters the render chose */
        CHECK(strncmp((char *)out, "320x240 30 fps audio 1\n", 23) == 0);
        int nv = 0, na = 0, order_ok = 1;
        char kind_prev = 0;
        long long pts_prev = -1;
        for (char *line = strtok((char *)out, "\n"); line; line = strtok(NULL, "\n")) {
            if (line[0] == 'v') {
                long long pts; size_t l;
                /* The file keeps time in 90 kHz ticks: 3000 a frame, as written. */
                if (sscanf(line, "v %lld %zu", &pts, &l) != 2 || pts != (long long)nv * 3000 * 1000000 / 90000 || l != 50 + (size_t)nv) order_ok = 0;
                if (kind_prev == 'v' || pts <= pts_prev) order_ok = 0;   /* each video sample is followed by its audio */
                pts_prev = pts; nv++; kind_prev = 'v';
            } else if (line[0] == 'a') {
                na++; kind_prev = 'a';
            }
        }
        CHECK(nv == 30 && na == 60 && order_ok);
        CHECK(ms == 29 * 3000 * 1000 / 90000);
        free(out);
    }
    CHECK(oc_export_codecs_builtin() == NULL);            /* this build carries no codecs */

    /* Cancelled: stops between samples, and no file is left. */
    volatile int cancel = 1;
    m = (mem_src){ src, len, 0, 0, 0 };
    CHECK(oc_export_render(&oc_export_stub_codecs, mem_read, &m, len, path, &cancel, &bytes, &ms, err, sizeof err) == OC_EXPORT_CANCELLED);
    struct stat st;
    CHECK(stat(path, &st) != 0);

    /* A read that fails part way: failed, and no file. */
    m = (mem_src){ src, len, 0, len - 200, 0 };
    CHECK(oc_export_render(&oc_export_stub_codecs, mem_read, &m, len, path, NULL, &bytes, &ms, err, sizeof err) == OC_EXPORT_FAILED);
    CHECK(stat(path, &st) != 0);
    /* A source shorter than it says it is: failed. */
    m = (mem_src){ src, len - 10, 0, 0, 0 };
    CHECK(oc_export_render(&oc_export_stub_codecs, mem_read, &m, len, path, NULL, &bytes, &ms, err, sizeof err) != OC_EXPORT_OK);
    CHECK(stat(path, &st) != 0);
    free(src);

    /* Audio alone (read-aloud's kind of file) and bytes that are not an MP4 are not video messages. */
    src = make_source(0, &len);
    m = (mem_src){ src, len, 0, 0, 0 };
    CHECK(oc_export_render(&oc_export_stub_codecs, mem_read, &m, len, path, NULL, &bytes, &ms, err, sizeof err) == OC_EXPORT_NOT_VIDEO);
    free(src);
    uint8_t junk[4096];
    memset(junk, 0x11, sizeof junk);
    m = (mem_src){ junk, sizeof junk, 0, 0, 0 };
    CHECK(oc_export_render(&oc_export_stub_codecs, mem_read, &m, sizeof junk, path, NULL, &bytes, &ms, err, sizeof err) == OC_EXPORT_NOT_VIDEO);
    CHECK(stat(path, &st) != 0);
    oc_test_rmtree(dir);
}

/* ---- the worker -------------------------------------------------------------------- */

/* The gate a read-aloud rendering holds: closed while `closed`, and counting how
 * often the worker asked, so a test sees it waiting without timing it. */
static volatile int gate_closed;
static volatile int gate_asks;
static int test_gate(void *ctx) { (void)ctx; gate_asks++; return gate_closed; }

/* The worker's temporary files left in `dir`. */
static int tmp_files(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) n += strncmp(e->d_name, "export-", 7) == 0;
    closedir(d);
    return n;
}

static int put_blob(oc_blobstore *bs, const char *key, const uint8_t *d, size_t n) {
    oc_blob_writer *w = oc_blob_put_begin(bs, key, n);
    if (!w) return -1;
    if (oc_blob_put_chunk(w, d, n) != 0) { oc_blob_put_abort(w); return -1; }
    return oc_blob_put_commit(w);
}

static void test_worker(void) {
    char dir[256], blobs[512], tmp[512], stale[600];
    CHECK(oc_test_mkdtemp("export-worker", dir, sizeof dir) == 0);
    oc_test_path(blobs, sizeof blobs, dir, "blobs");
    oc_test_path(tmp, sizeof tmp, dir, "tmp");
    CHECK(mkdir(tmp, 0700) == 0);
    /* A file a daemon that stopped mid-export left: gone when the worker starts. */
    oc_test_path(stale, sizeof stale, tmp, "export-00000000000000aa-1.part");
    FILE *sf = fopen(stale, "w");
    if (sf) fclose(sf);
    CHECK(tmp_files(tmp) == 1);

    oc_blobstore *bs = oc_blobstore_open(blobs, NULL);
    CHECK(bs != NULL);
    if (!bs) { oc_test_rmtree(dir); return; }
    size_t len = 0;
    uint8_t *src = make_source(30, &len);
    CHECK(src && put_blob(bs, "00000000000000a1", src, len) == 0);
    CHECK(src && put_blob(bs, "00000000000000a2", src, len) == 0);
    free(src);

    gate_closed = 1;
    gate_asks = 0;
    oc_export_worker *w = oc_export_worker_start(&oc_export_stub_codecs, bs, tmp, test_gate, NULL);
    CHECK(w != NULL);
    if (!w) { oc_blobstore_close(bs); oc_test_rmtree(dir); return; }
    CHECK(tmp_files(tmp) == 0);

    /* While a rendering runs nothing starts: the worker asks the gate, again and
     * again, and starts nothing. */
    CHECK(oc_export_worker_submit(w, 1, 7, 0xa1, "00000000000000a1", len) == 0);
    CHECK(WAIT_UNTIL(gate_asks >= 3, 2000));
    CHECK(oc_export_worker_started(w) == 0);

    /* The same video again joins it; a person's fourth is refused, another's is not. */
    CHECK(oc_export_worker_submit(w, 2, 8, 0xa1, "00000000000000a1", len) == 1);
    CHECK(oc_export_worker_submit(w, 3, 7, 0xa2, "00000000000000a2", len) == 0);
    CHECK(oc_export_worker_submit(w, 4, 7, 0xb3, "00000000000000b3", len) == 0);   /* no such blob */
    CHECK(oc_export_worker_submit(w, 5, 7, 0xa2, "00000000000000a2", len) == -2);  /* 7 has three */
    CHECK(oc_export_worker_submit(w, 6, 9, 0xa2, "00000000000000a2", len) == 1);

    /* The rendering ends: the exports run, one after another, in order. */
    gate_closed = 0;
    oc_export_result r1, r2, r3;
    memset(&r1, 0, sizeof r1); memset(&r2, 0, sizeof r2); memset(&r3, 0, sizeof r3);
    CHECK(WAIT_UNTIL(oc_export_worker_next_result(w, &r1), 5000));
    struct pollfd pfd = { oc_export_worker_eventfd(w), POLLIN, 0 };
    CHECK(poll(&pfd, 1, 0) == 1);                          /* the net loop would have been woken */
    CHECK(r1.attachment_id == 0xa1 && r1.status == OC_EXPORT_OK && r1.n_req == 2);
    CHECK(r1.n_req == 2 && ((r1.req_ids[0] == 1 && r1.req_ids[1] == 2) || (r1.req_ids[0] == 2 && r1.req_ids[1] == 1)));
    char key[OC_EXPORT_KEY_MAX];
    oc_export_copy_key(&oc_export_stub_codecs, 0xa1, key, sizeof key);
    CHECK(strcmp(r1.key, key) == 0 && strlen(key) == 33 && key[16] == '_');
    uint64_t stored = 0;
    oc_blob_reader *rd = oc_blob_get_begin(bs, r1.key, &stored);
    CHECK(rd && stored == r1.bytes && stored > 0);
    oc_blob_get_close(rd);
    CHECK(WAIT_UNTIL(oc_export_worker_next_result(w, &r2), 5000));
    CHECK(r2.attachment_id == 0xa2 && r2.status == OC_EXPORT_OK && r2.n_req == 2);
    CHECK(WAIT_UNTIL(oc_export_worker_next_result(w, &r3), 5000));
    CHECK(r3.attachment_id == 0xb3 && r3.status == OC_EXPORT_FAILED && r3.n_req == 1 && r3.key[0] == '\0');
    CHECK(oc_export_worker_started(w) == 3);               /* four videos asked for, three transcodes */
    CHECK(tmp_files(tmp) == 0);                            /* finished or failed, nothing left */
    oc_export_result_free(&r1);
    oc_export_result_free(&r2);
    oc_export_result_free(&r3);

    /* With nothing queued, a person may ask again. */
    CHECK(oc_export_worker_submit(w, 7, 7, 0xa1, "00000000000000a1", len) == 0);
    oc_export_result r4;
    memset(&r4, 0, sizeof r4);
    CHECK(WAIT_UNTIL(oc_export_worker_next_result(w, &r4), 5000));
    CHECK(r4.status == OC_EXPORT_OK);
    oc_export_result_free(&r4);

    /* Stopped with work queued behind a closed gate: nothing left behind. */
    gate_closed = 1;
    CHECK(oc_export_worker_submit(w, 8, 7, 0xa2, "00000000000000a2", len) == 0);
    oc_export_worker_stop(w);
    CHECK(tmp_files(tmp) == 0);
    oc_blobstore_close(bs);
    oc_test_rmtree(dir);
}

/* ---- the cache, in the database ------------------------------------------------------- */

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

/* EXPORT_LOOKUP for `user` and `aid`, with the codecs' `version`: the result. */
static oc_dbres *lookup(oc_dbwriter *w, uint64_t user, uint64_t aid, const char *version) {
    oc_job *j = oc_job_new(OC_JOB_EXPORT_LOOKUP, 21);
    j->user_id = user; j->attachment_id = aid; j->exp_version = strdup(version);
    oc_dbwriter_submit(w, j);
    return db_wait(w);
}

static int lookup_err(oc_dbwriter *w, uint64_t user, uint64_t aid) {
    oc_dbres *r = lookup(w, user, aid, "v1");
    int e = !r ? -1 : r->type == OC_RES_EXPORT_ERR ? (int)r->err_code : 0;
    if (r) oc_dbres_free(r);
    return e;
}

static void store(oc_dbwriter *w, uint64_t aid, const char *version, const char *key) {
    oc_job *j = oc_job_new(OC_JOB_EXPORT_STORE, 0);
    j->attachment_id = aid; j->exp_version = strdup(version); j->exp_blob_key = strdup(key);
    j->exp_bytes = 4321; j->exp_duration_ms = 10033;
    oc_dbwriter_submit(w, j);
    oc_dbres *r = db_wait(w);
    if (r) oc_dbres_free(r);
}

/* A storage pass reclaiming at most `batch`; whether `key` was among what it
 * reclaimed, and where. */
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

static void test_cache(void) {
    char dir[256], dbp[512];
    CHECK(oc_test_mkdtemp("export-cache", dir, sizeof dir) == 0);
    oc_test_path(dbp, sizeof dbp, dir, "export.db");
    oc_dbwriter *w = oc_dbwriter_start(dbp);
    CHECK(w != NULL);
    if (!w) { oc_test_rmtree(dir); return; }
    oc_dbwriter_set_pw_mem(w, 2048);
    uint64_t alice = oc_dbwriter_register_local(w, "ex-alice", "pw", OC_ROLE_OWNER, 1000);
    uint64_t bob   = oc_dbwriter_register_local(w, "ex-bob", "pw", OC_ROLE_MEMBER, 1000);
    CHECK(alice && bob);
    sqlite3 *db = NULL;
    CHECK(sqlite3_open(dbp, &db) == SQLITE_OK);
    sqlite3_busy_timeout(db, 5000);
    char sql[1024];
    /* 300: a sent video message in the general channel. 301: a sent picture
     * there. 302: a video in a private channel bob is not in. 303: an unsent video. */
    db_exec(db, "INSERT INTO channels(id, kind, name, is_public, created_at_ms) VALUES(88, 'channel', 'private', 0, 1);");
    snprintf(sql, sizeof sql, "INSERT INTO channel_members(channel_id, user_id, joined_at_ms) VALUES(88, %llu, 1);",
             (unsigned long long)alice);
    db_exec(db, sql);
    snprintf(sql, sizeof sql,
             "INSERT INTO messages(id, channel_id, author_id, body, created_at_ms) VALUES(9001, %d, %llu, 'clip', 1);"
             "INSERT INTO messages(id, channel_id, author_id, body, created_at_ms) VALUES(9002, 88, %llu, 'clip', 1);",
             OC_DEFAULT_CHANNEL, (unsigned long long)alice, (unsigned long long)alice);
    db_exec(db, sql);
    const char *const rows[] = {
        "(300, %d, 9001, %llu, '000000000000012c', 'v.mp4', 'video/mp4', 5000000, x'00', 1)",
        "(301, %d, 9001, %llu, '000000000000012d', 'p.png', 'image/png', 1000, x'00', 1)",
        "(302, 88, 9002, %llu, '000000000000012e', 'v.mp4', 'video/mp4', 5000, x'00', 1)",
        "(303, %d, NULL, %llu, '000000000000012f', 'v.mp4', 'video/mp4', 5000, x'00', 1)",
    };
    for (int i = 0; i < 4; i++) {
        char row[256];
        if (i == 2) snprintf(row, sizeof row, rows[i], (unsigned long long)alice);
        else snprintf(row, sizeof row, rows[i], OC_DEFAULT_CHANNEL, (unsigned long long)alice);
        snprintf(sql, sizeof sql, "INSERT INTO attachments(id, channel_id, message_id, uploader_id, storage_key,"
                 " filename, mime, size, sha256, created_at_ms) VALUES%s;", row);
        db_exec(db, sql);
    }
    db_exec(db, "INSERT INTO attachment_media(attachment_id, kind, duration_ms, width, height, poster_id, created_at_ms)"
                " VALUES(300, 1, 10033, 1280, 720, NULL, 1), (302, 1, 1000, 640, 360, NULL, 1), (303, 1, 1000, 640, 360, NULL, 1);");

    /* Refusals: the download's gate first, then what can be exported. */
    CHECK(lookup_err(w, bob, 999) == OC_ERR_UNKNOWN_ATTACHMENT);
    CHECK(lookup_err(w, bob, 303) == OC_ERR_UNKNOWN_ATTACHMENT);     /* not sent: not a video message yet */
    CHECK(lookup_err(w, bob, 302) == OC_ERR_FORBIDDEN);
    CHECK(lookup_err(w, alice, 302) == 0);
    CHECK(lookup_err(w, bob, 301) == OC_ERR_MEDIA_INVALID);

    /* A miss names the source; once stored, a hit names the copy -- for these
     * codecs only. */
    oc_dbres *r = lookup(w, bob, 300, "v1");
    CHECK(r && r->type == OC_RES_EXPORT_META && !r->exp_cached && r->exp_attachment_id == 300);
    CHECK(r && r->exp_src_key && strcmp(r->exp_src_key, "000000000000012c") == 0 && r->exp_src_size == 5000000);
    if (r) oc_dbres_free(r);
    const char *key = "000000000000012c_00000000000000aa";
    store(w, 300, "v1", key);
    r = lookup(w, bob, 300, "v1");
    CHECK(r && r->type == OC_RES_EXPORT_META && r->exp_cached && r->exp_blob_key &&
          strcmp(r->exp_blob_key, key) == 0 && r->exp_bytes == 4321 && r->exp_duration_ms == 10033);
    if (r) oc_dbres_free(r);
    r = lookup(w, bob, 300, "v2");
    CHECK(r && r->type == OC_RES_EXPORT_META && !r->exp_cached);
    if (r) oc_dbres_free(r);

    /* Serving it makes it recently used. */
    db_exec(db, "UPDATE exported_video SET last_used_ms = 5;");
    oc_job *j = oc_job_new(OC_JOB_EXPORT_TOUCH, 0);
    j->attachment_id = 300; j->exp_version = strdup("v1");
    oc_dbwriter_submit(w, j);
    oc_dbres *tr = db_wait(w);
    if (tr) oc_dbres_free(tr);
    CHECK(db_scalar(db, "SELECT last_used_ms FROM exported_video WHERE attachment_id = 300;") > 5);

    /* The orphan sweep leaves its blob alone while the row stands. */
    uint8_t ref = 0;
    const char *keys[1] = { key };
    CHECK(oc_db_blob_referenced(dbp, keys, 1, &ref) == 0 && ref == 1);

    /* Without pressure a copy stays; under pressure it is the first thing to go:
     * a pass that may take one thing takes it, and not the video. */
    CHECK(!maint_reclaims(w, 0, 64, key, NULL));
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM exported_video;") == 1);
    size_t at = 99;
    CHECK(maint_reclaims(w, 1, 1, key, &at) && at == 0);
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM exported_video;") == 0);
    CHECK(db_scalar(db, "SELECT reclaimed_at_ms FROM attachments WHERE id = 300;") == 0);   /* the video stays */

    /* It goes with its video, pressure or not; and a copy finished after its
     * video went is not recorded. */
    store(w, 300, "v1", key);
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM exported_video;") == 1);
    db_exec(db, "UPDATE attachments SET reclaimed_at_ms = 1, reclaim_reason = 1 WHERE id = 300;");
    CHECK(maint_reclaims(w, 0, 64, key, NULL));
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM exported_video;") == 0);
    CHECK(lookup_err(w, bob, 300) == OC_ERR_ATTACHMENT_GONE);
    store(w, 300, "v1", key);
    CHECK(db_scalar(db, "SELECT COUNT(*) FROM exported_video;") == 0);

    sqlite3_close(db);
    oc_dbwriter_stop(w);
    oc_test_rmtree(dir);
}

int run_export_tests(void) {
    printf("test_export: the H.264 + MP3 writer -- layout, parameter sets, sync samples, offsets, the audio edit, refusals, removal; the render with stub codecs -- order, times, one pass, cancel, failed reads, not a video message; the worker -- the read-aloud gate, joining, the per-person limit, order, keys, eventfd, failures, no files left; the cache -- refusals, hit and miss by version, touch, kept from the orphan sweep, reclaimed first under pressure and with its video\n");
    test_writer();
    test_render();
    test_worker();
    test_cache();
    return failures;
}
