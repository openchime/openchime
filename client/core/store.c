/*
 * OpenChime client — the local store, with no database engine (ARCH-88). See store.h.
 *
 * Three backings, chosen by what the data *is*:
 *
 *   credential store   session token, TOFU pin   (per workspace, via oc_secret)
 *   <dir>/book         the workspace book        (one text line per workspace)
 *   <dir>/<key>.log    cached history            (append-only record log)
 *   <dir>/<key>.out    the offline outbox        (rewritten whole; it is tiny)
 *
 * The cache is a LOG, not a table: one writer (the net thread), read whole at
 * startup, never queried. Records are length-prefixed and CRC'd, so a tail torn
 * by a crash fails its checksum and is dropped on the next load. Edits and
 * deletes append tombstones that are folded in memory at load time, and the file
 * is compacted once it exceeds its record budget. None of this needs indexes,
 * transactions, concurrency or a query planner — which is exactly why a SQL
 * engine was the wrong shape for it.
 *
 * Everything is little-endian and self-framed. There is no schema version to
 * migrate: a record the reader cannot parse simply ends the load.
 */

#include "store.h"

#include "oc_port.h"    /* oc_mkdir, OC_PATH_SEP */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#  include <windows.h>
#endif

#define CACHE_MAX_MSGS   2000    /* per workspace; oldest dropped on compaction */
#define CACHE_COMPACT_AT 6000    /* records on disk before a load rewrites the file */
#define OUTBOX_MAX       256

struct oc_store { char dir[512]; oc_secret *secret; };

static void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v;
}

/* ---- credential blob -------------------------------------------------------
 * One entry per workspace holding the token AND the pin. The pin is not secret,
 * but it is integrity-sensitive — rewriting a pin is how a MITM gets accepted
 * (ARCH-10) — so it belongs beside the credential, not in a file anyone can
 * edit. Layout: [ver][flags][expiry u64][token 32][pin 32]. */
#define SEC_VER   1
#define SEC_BLOB  (2 + 8 + OC_SESSION_TOKEN_LEN + OC_TLS_FINGERPRINT_LEN)
enum { SEC_HAS_TOKEN = 1, SEC_HAS_PIN = 2 };
#define SEC_EXPIRY(b) ((b) + 2)
#define SEC_TOKEN(b)  ((b) + 10)
#define SEC_PIN(b)    ((b) + 10 + OC_SESSION_TOKEN_LEN)

static int sec_load(oc_store *s, const char *ws, uint8_t *blob) {
    size_t got = 0;
    memset(blob, 0, SEC_BLOB);
    if (!oc_secret_get(s->secret, ws, blob, SEC_BLOB, &got)) { memset(blob, 0, SEC_BLOB); return 0; }
    if (got != SEC_BLOB || blob[0] != SEC_VER) { memset(blob, 0, SEC_BLOB); return 0; }
    return 1;
}
static void sec_store(oc_store *s, const char *ws, uint8_t *blob) {
    blob[0] = SEC_VER;
    if (!(blob[1] & (SEC_HAS_TOKEN | SEC_HAS_PIN))) oc_secret_del(s->secret, ws);
    else oc_secret_put(s->secret, ws, blob, SEC_BLOB);
}

/* ---- paths ----------------------------------------------------------------
 * A workspace is "host:port", which is not a legal filename on Windows. Map it
 * to a sanitized stem plus an FNV-1a hash, so two workspaces cannot collide
 * after sanitizing (`a:1` and `a_1` would otherwise share a file). */
static void ws_key(const char *ws, char *out, size_t cap) {
    uint32_t h = 2166136261u;
    for (const char *p = ws; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    char stem[40]; size_t n = 0;
    for (const char *p = ws; *p && n < sizeof stem - 1; p++) {
        char c = *p;
        int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
        stem[n++] = ok ? c : '_';
    }
    stem[n] = '\0';
    snprintf(out, cap, "%s-%08lx", stem, (unsigned long)h);
}
static void ws_path(oc_store *s, const char *ws, const char *ext, char *out, size_t cap) {
    char key[64]; ws_key(ws, key, sizeof key);
    snprintf(out, cap, "%s%c%s.%s", s->dir, OC_PATH_SEP, key, ext);
}
static void book_path(oc_store *s, char *out, size_t cap) {
    snprintf(out, cap, "%s%cbook", s->dir, OC_PATH_SEP);
}

/* Replace `path` with `tmp` atomically, so a reader — including a second handle
 * in another process (store.h's switcher contract) — sees either the whole old
 * file or the whole new one, with no locking. */
static int atomic_replace(const char *tmp, const char *path) {
#ifdef _WIN32
    /* rename() refuses to overwrite on Windows; MoveFileEx does it atomically. */
    WCHAR wt[1024], wp[1024];
    if (MultiByteToWideChar(CP_UTF8, 0, tmp, -1, wt, 1024) <= 0) return -1;
    if (MultiByteToWideChar(CP_UTF8, 0, path, -1, wp, 1024) <= 0) return -1;
    return MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING) ? 0 : -1;
#else
    return rename(tmp, path);
#endif
}

/* ---- record framing: [u32 len][u32 crc32(payload)][payload] ---------------- */

static uint32_t crc32_of(const uint8_t *d, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(c & 1)));
    }
    return ~c;
}

typedef struct { uint8_t *p; size_t n, cap; int bad; } buf;
static int buf_need(buf *b, size_t extra) {
    if (b->n + extra <= b->cap) return 1;
    size_t want = b->cap ? b->cap * 2 : 256;
    while (want < b->n + extra) want *= 2;
    uint8_t *q = realloc(b->p, want);
    if (!q) { b->bad = 1; return 0; }
    b->p = q; b->cap = want; return 1;
}
static void bw_u8(buf *b, uint8_t v)   { if (buf_need(b, 1)) b->p[b->n++] = v; }
static void bw_u64(buf *b, uint64_t v) { if (buf_need(b, 8)) { put_u64(b->p + b->n, v); b->n += 8; } }
static void bw_raw(buf *b, const void *d, size_t n) {
    if (buf_need(b, n)) { memcpy(b->p + b->n, d, n); b->n += n; }
}
/* A NULL string is stored as length 0xFFFFFFFF, distinct from "" (a deleted
 * message's body is NULL, and that difference is user-visible). */
static void bw_str(buf *b, const char *s) {
    uint32_t n = s ? (uint32_t)strlen(s) : 0xFFFFFFFFu;
    if (buf_need(b, 4)) for (int i = 0; i < 4; i++) b->p[b->n++] = (uint8_t)(n >> (8 * i));
    if (s) bw_raw(b, s, strlen(s));
}

static void rec_append(const char *path, const buf *payload) {
    if (payload->bad) return;
    FILE *f = fopen(path, "ab");
    if (!f) return;
    uint8_t hdr[8];
    uint32_t len = (uint32_t)payload->n, crc = crc32_of(payload->p, payload->n);
    for (int i = 0; i < 4; i++) hdr[i]     = (uint8_t)(len >> (8 * i));
    for (int i = 0; i < 4; i++) hdr[4 + i] = (uint8_t)(crc >> (8 * i));
    fwrite(hdr, 1, 8, f);
    fwrite(payload->p, 1, payload->n, f);
    fclose(f);
}

static uint8_t *slurp(const char *path, size_t *out_n) {
    *out_n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long sz = ftell(f);
    if (sz <= 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    uint8_t *d = malloc((size_t)sz);
    if (!d) { fclose(f); return NULL; }
    *out_n = fread(d, 1, (size_t)sz, f);
    fclose(f);
    return d;
}

/* Iterate whole valid records; a bad CRC or short tail ends iteration. */
typedef void (*rec_cb)(void *ctx, const uint8_t *p, size_t n);
static int rec_each(const uint8_t *d, size_t n, rec_cb cb, void *ctx) {
    size_t off = 0; int count = 0;
    while (off + 8 <= n) {
        uint32_t len = 0, crc = 0;
        for (int i = 0; i < 4; i++) len |= (uint32_t)d[off + i]     << (8 * i);
        for (int i = 0; i < 4; i++) crc |= (uint32_t)d[off + 4 + i] << (8 * i);
        if (len > n - off - 8) break;
        if (crc32_of(d + off + 8, len) != crc) break;
        cb(ctx, d + off + 8, len);
        off += 8 + len; count++;
    }
    return count;
}

/* Bounds-checked payload reader. */
typedef struct { const uint8_t *p; size_t n, off; int bad; } rd;
static uint8_t rd_u8(rd *r) { if (r->off + 1 > r->n) { r->bad = 1; return 0; } return r->p[r->off++]; }
static uint64_t rd_u64(rd *r) {
    if (r->off + 8 > r->n) { r->bad = 1; return 0; }
    uint64_t v = get_u64(r->p + r->off); r->off += 8; return v;
}
static void rd_raw(rd *r, void *out, size_t n) {
    if (r->off + n > r->n) { r->bad = 1; memset(out, 0, n); return; }
    memcpy(out, r->p + r->off, n); r->off += n;
}
static char *rd_str(rd *r) {
    if (r->off + 4 > r->n) { r->bad = 1; return NULL; }
    uint32_t n = 0;
    for (int i = 0; i < 4; i++) n |= (uint32_t)r->p[r->off + i] << (8 * i);
    r->off += 4;
    if (n == 0xFFFFFFFFu) return NULL;                 /* stored NULL */
    if (r->off + n > r->n) { r->bad = 1; return NULL; }
    char *s = malloc((size_t)n + 1);
    if (!s) { r->bad = 1; return NULL; }
    memcpy(s, r->p + r->off, n); s[n] = '\0';
    r->off += n;
    return s;
}

/* ---- open / close ---------------------------------------------------------- */

oc_store *oc_store_open(const char *path) {
    if (!path || !path[0]) return NULL;
    oc_mkdir(path);                       /* the state dir; harmless if it exists */
    char probe[600];
    snprintf(probe, sizeof probe, "%s%c.w", path, OC_PATH_SEP);
    FILE *f = fopen(probe, "wb");
    if (!f) return NULL;                  /* unwritable -> run without persistence */
    fclose(f);
    remove(probe);

    oc_store *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    snprintf(s->dir, sizeof s->dir, "%s", path);
    return s;
}

void oc_store_close(oc_store *s) { free(s); }

void oc_store_set_secret(oc_store *s, oc_secret *secret) {
    if (s) s->secret = secret;
}

/* ---- session token + TOFU pin (credential store) --------------------------- */

int oc_store_load_session(oc_store *s, const char *workspace,
                          uint8_t token[OC_SESSION_TOKEN_LEN], uint64_t *expiry,
                          uint64_t now_ms) {
    if (!s || !workspace || !s->secret) return 0;   /* no OS store -> no session */
    uint8_t b[SEC_BLOB];
    if (!sec_load(s, workspace, b) || !(b[1] & SEC_HAS_TOKEN)) return 0;
    uint64_t exp = get_u64(SEC_EXPIRY(b));
    if (now_ms != 0 && exp != 0 && exp <= now_ms) return 0;   /* expired */
    memcpy(token, SEC_TOKEN(b), OC_SESSION_TOKEN_LEN);
    if (expiry) *expiry = exp;
    return 1;
}

void oc_store_save_session(oc_store *s, const char *workspace,
                           const uint8_t token[OC_SESSION_TOKEN_LEN], uint64_t expiry) {
    if (!s || !workspace || !s->secret) return;     /* never persisted elsewhere */
    uint8_t b[SEC_BLOB];
    sec_load(s, workspace, b);                      /* keep any pin already stored */
    b[1] |= SEC_HAS_TOKEN;
    put_u64(SEC_EXPIRY(b), expiry);
    memcpy(SEC_TOKEN(b), token, OC_SESSION_TOKEN_LEN);
    sec_store(s, workspace, b);
}

void oc_store_clear_session(oc_store *s, const char *workspace) {
    if (!s || !workspace || !s->secret) return;
    uint8_t b[SEC_BLOB];
    if (!sec_load(s, workspace, b)) { oc_secret_del(s->secret, workspace); return; }
    b[1] &= (uint8_t)~SEC_HAS_TOKEN;
    memset(SEC_TOKEN(b), 0, OC_SESSION_TOKEN_LEN);
    put_u64(SEC_EXPIRY(b), 0);
    sec_store(s, workspace, b);                     /* the pin survives a logout */
}

int oc_store_load_pin(oc_store *s, const char *workspace,
                      uint8_t pin[OC_TLS_FINGERPRINT_LEN]) {
    if (!s || !workspace || !s->secret) return 0;
    uint8_t b[SEC_BLOB];
    if (!sec_load(s, workspace, b) || !(b[1] & SEC_HAS_PIN)) return 0;
    memcpy(pin, SEC_PIN(b), OC_TLS_FINGERPRINT_LEN);
    return 1;
}

void oc_store_save_pin(oc_store *s, const char *workspace,
                       const uint8_t pin[OC_TLS_FINGERPRINT_LEN]) {
    if (!s || !workspace || !s->secret) return;
    uint8_t b[SEC_BLOB];
    sec_load(s, workspace, b);
    b[1] |= SEC_HAS_PIN;
    memcpy(SEC_PIN(b), pin, OC_TLS_FINGERPRINT_LEN);
    sec_store(s, workspace, b);
}

/* ---- cached history (append-only log) -------------------------------------- */

enum { CREC_MSG = 1, CREC_EDIT = 2, CREC_DEL = 3 };

typedef struct {
    uint64_t channel_id, message_id, author_id, server_time;
    char *author_name, *body;
    int edited, deleted;
} cmsg;
typedef struct { cmsg *v; size_t n, cap; int records; } cfold;

static cmsg *fold_find(cfold *f, uint64_t mid) {
    for (size_t i = 0; i < f->n; i++) if (f->v[i].message_id == mid) return &f->v[i];
    return NULL;
}

static void fold_rec(void *ctx, const uint8_t *p, size_t n) {
    cfold *f = ctx;
    rd r = { p, n, 0, 0 };
    uint8_t type = rd_u8(&r);
    f->records++;
    if (type == CREC_MSG) {
        cmsg m; memset(&m, 0, sizeof m);
        m.channel_id  = rd_u64(&r);
        m.message_id  = rd_u64(&r);
        m.author_id   = rd_u64(&r);
        m.server_time = rd_u64(&r);
        m.edited  = rd_u8(&r);
        m.deleted = rd_u8(&r);
        m.author_name = rd_str(&r);
        m.body        = rd_str(&r);
        if (r.bad) { free(m.author_name); free(m.body); return; }
        cmsg *ex = fold_find(f, m.message_id);
        if (ex) { free(ex->author_name); free(ex->body); *ex = m; return; }
        if (f->n == f->cap) {
            size_t want = f->cap ? f->cap * 2 : 64;
            cmsg *q = realloc(f->v, want * sizeof *q);
            if (!q) { free(m.author_name); free(m.body); return; }
            f->v = q; f->cap = want;
        }
        f->v[f->n++] = m;
    } else if (type == CREC_EDIT) {
        uint64_t mid = rd_u64(&r);
        char *body = rd_str(&r);
        if (r.bad) { free(body); return; }
        cmsg *ex = fold_find(f, mid);
        if (ex) { free(ex->body); ex->body = body; ex->edited = 1; }
        else free(body);
    } else if (type == CREC_DEL) {
        uint64_t mid = rd_u64(&r);
        if (r.bad) return;
        cmsg *ex = fold_find(f, mid);
        if (ex) { free(ex->body); ex->body = NULL; ex->deleted = 1; }
    }
}

static int cmsg_cmp(const void *a, const void *b) {
    uint64_t x = ((const cmsg *)a)->message_id, y = ((const cmsg *)b)->message_id;
    return x < y ? -1 : x > y ? 1 : 0;
}
static void fold_free(cfold *f) {
    for (size_t i = 0; i < f->n; i++) { free(f->v[i].author_name); free(f->v[i].body); }
    free(f->v);
}

static void cache_load(oc_store *s, const char *ws, cfold *f) {
    memset(f, 0, sizeof *f);
    char path[700]; ws_path(s, ws, "log", path, sizeof path);
    size_t n = 0;
    uint8_t *d = slurp(path, &n);
    if (!d) return;
    rec_each(d, n, fold_rec, f);
    free(d);
    if (f->n > 1) qsort(f->v, f->n, sizeof f->v[0], cmsg_cmp);
}

static void cache_write_msg(const char *path, const cmsg *m) {
    buf b; memset(&b, 0, sizeof b);
    bw_u8(&b, CREC_MSG);
    bw_u64(&b, m->channel_id); bw_u64(&b, m->message_id);
    bw_u64(&b, m->author_id);  bw_u64(&b, m->server_time);
    bw_u8(&b, (uint8_t)(m->edited ? 1 : 0)); bw_u8(&b, (uint8_t)(m->deleted ? 1 : 0));
    bw_str(&b, m->author_name); bw_str(&b, m->body);
    rec_append(path, &b);
    free(b.p);
}

/* Rewrite the log as one record per live message, newest CACHE_MAX_MSGS kept —
 * dropping folded tombstones and any torn tail in the same pass. */
static void cache_compact(oc_store *s, const char *ws, cfold *f) {
    char path[700], tmp[720];
    ws_path(s, ws, "log", path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    remove(tmp);
    size_t start = f->n > CACHE_MAX_MSGS ? f->n - CACHE_MAX_MSGS : 0;
    for (size_t i = start; i < f->n; i++) cache_write_msg(tmp, &f->v[i]);
    if (start == f->n) { remove(tmp); remove(path); return; }
    atomic_replace(tmp, path);
}

void oc_store_save_message(oc_store *s, const char *workspace, uint64_t channel_id,
                           uint64_t message_id, uint64_t author_id,
                           const char *author_name, uint64_t server_time,
                           const char *body, int edited, int deleted) {
    if (!s || !workspace) return;
    char path[700]; ws_path(s, workspace, "log", path, sizeof path);
    cmsg m;
    m.channel_id = channel_id; m.message_id = message_id;
    m.author_id = author_id;   m.server_time = server_time;
    m.author_name = (char *)author_name; m.body = (char *)body;
    m.edited = edited; m.deleted = deleted;
    cache_write_msg(path, &m);
}

void oc_store_edit_message(oc_store *s, const char *workspace, uint64_t message_id,
                           const char *body) {
    if (!s || !workspace) return;
    char path[700]; ws_path(s, workspace, "log", path, sizeof path);
    buf b; memset(&b, 0, sizeof b);
    bw_u8(&b, CREC_EDIT); bw_u64(&b, message_id); bw_str(&b, body);
    rec_append(path, &b);
    free(b.p);
}

void oc_store_delete_message(oc_store *s, const char *workspace, uint64_t message_id) {
    if (!s || !workspace) return;
    char path[700]; ws_path(s, workspace, "log", path, sizeof path);
    buf b; memset(&b, 0, sizeof b);
    bw_u8(&b, CREC_DEL); bw_u64(&b, message_id);
    rec_append(path, &b);
    free(b.p);
}

void oc_store_each_message(oc_store *s, const char *workspace,
                           oc_store_msg_cb cb, void *ctx) {
    if (!s || !workspace || !cb) return;
    cfold f;
    cache_load(s, workspace, &f);
    size_t start = f.n > CACHE_MAX_MSGS ? f.n - CACHE_MAX_MSGS : 0;
    for (size_t i = start; i < f.n; i++) {
        const cmsg *m = &f.v[i];
        cb(ctx, m->channel_id, m->message_id, m->author_id,
           m->author_name ? m->author_name : "", m->server_time,
           m->body, m->edited, m->deleted);
    }
    /* Startup is the natural moment to pay for compaction: the whole log has just
     * been folded, and nothing else is competing for the disk yet. */
    if (f.records > CACHE_COMPACT_AT || f.n > CACHE_MAX_MSGS) cache_compact(s, workspace, &f);
    fold_free(&f);
}

/* ---- offline outbox (small; rewritten whole) -------------------------------- */

typedef struct { uint8_t idem[OC_IDEM_SIZE]; uint64_t channel_id; char *body; } orow;
typedef struct { orow *v; size_t n, cap; } ofold;

static void ofold_rec(void *ctx, const uint8_t *p, size_t n) {
    ofold *f = ctx;
    rd r = { p, n, 0, 0 };
    orow o; memset(&o, 0, sizeof o);
    rd_raw(&r, o.idem, OC_IDEM_SIZE);
    o.channel_id = rd_u64(&r);
    o.body = rd_str(&r);
    if (r.bad) { free(o.body); return; }
    if (f->n == f->cap) {
        size_t want = f->cap ? f->cap * 2 : 16;
        orow *q = realloc(f->v, want * sizeof *q);
        if (!q) { free(o.body); return; }
        f->v = q; f->cap = want;
    }
    f->v[f->n++] = o;
}
static void ofold_load(oc_store *s, const char *ws, ofold *f) {
    memset(f, 0, sizeof *f);
    char path[700]; ws_path(s, ws, "out", path, sizeof path);
    size_t n = 0;
    uint8_t *d = slurp(path, &n);
    if (!d) return;
    rec_each(d, n, ofold_rec, f);
    free(d);
}
static void ofold_write(oc_store *s, const char *ws, const ofold *f) {
    char path[700], tmp[720];
    ws_path(s, ws, "out", path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    remove(tmp);
    if (f->n == 0) { remove(path); return; }
    for (size_t i = 0; i < f->n; i++) {
        buf b; memset(&b, 0, sizeof b);
        bw_raw(&b, f->v[i].idem, OC_IDEM_SIZE);
        bw_u64(&b, f->v[i].channel_id);
        bw_str(&b, f->v[i].body);
        rec_append(tmp, &b);
        free(b.p);
    }
    atomic_replace(tmp, path);
}
static void ofold_free(ofold *f) {
    for (size_t i = 0; i < f->n; i++) free(f->v[i].body);
    free(f->v);
}

void oc_store_outbox_add(oc_store *s, const char *workspace,
                         const uint8_t idem[OC_IDEM_SIZE], uint64_t channel_id,
                         const char *body) {
    if (!s || !workspace) return;
    ofold f; ofold_load(s, workspace, &f);
    if (f.n >= OUTBOX_MAX) { ofold_free(&f); return; }
    for (size_t i = 0; i < f.n; i++)                       /* idempotent add */
        if (memcmp(f.v[i].idem, idem, OC_IDEM_SIZE) == 0) { ofold_free(&f); return; }
    if (f.n == f.cap) {
        size_t want = f.cap ? f.cap * 2 : 16;
        orow *q = realloc(f.v, want * sizeof *q);
        if (!q) { ofold_free(&f); return; }
        f.v = q; f.cap = want;
    }
    memcpy(f.v[f.n].idem, idem, OC_IDEM_SIZE);
    f.v[f.n].channel_id = channel_id;
    f.v[f.n].body = body ? strdup(body) : NULL;
    f.n++;
    ofold_write(s, workspace, &f);
    ofold_free(&f);
}

void oc_store_outbox_remove(oc_store *s, const char *workspace,
                            const uint8_t idem[OC_IDEM_SIZE]) {
    if (!s || !workspace) return;
    ofold f; ofold_load(s, workspace, &f);
    size_t w = 0;
    for (size_t i = 0; i < f.n; i++) {
        if (memcmp(f.v[i].idem, idem, OC_IDEM_SIZE) == 0) { free(f.v[i].body); continue; }
        f.v[w++] = f.v[i];
    }
    f.n = w;
    ofold_write(s, workspace, &f);
    ofold_free(&f);
}

void oc_store_outbox_each(oc_store *s, const char *workspace,
                          oc_store_outbox_cb cb, void *ctx) {
    if (!s || !workspace || !cb) return;
    ofold f; ofold_load(s, workspace, &f);
    for (size_t i = 0; i < f.n; i++)
        cb(ctx, f.v[i].idem, f.v[i].channel_id, f.v[i].body ? f.v[i].body : "");
    ofold_free(&f);
}

/* ---- the workspace book (a text file) --------------------------------------
 * Not a credential — an address, an account name and a timestamp — so it stays a
 * plain file. Tab-separated, since a tab cannot occur in a hostname or username
 * and so needs no quoting; rewritten whole under an atomic rename, which is what
 * makes store.h's "safe from a second handle" contract hold without locking. */
typedef struct { char ws[256], label[128], user[128]; uint64_t used; } brow;
typedef struct { brow *v; size_t n, cap; } bfold;

static void bfold_free(bfold *b) { free(b->v); }

static void bfold_load(oc_store *s, bfold *b) {
    memset(b, 0, sizeof *b);
    char path[600]; book_path(s, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return;
    char line[700];
    while (fgets(line, sizeof line, f)) {
        char *nl = strpbrk(line, "\r\n"); if (nl) *nl = '\0';
        if (!line[0]) continue;
        char *t1 = strchr(line, '\t'); if (!t1) continue; *t1++ = '\0';
        char *t2 = strchr(t1, '\t');   if (!t2) continue; *t2++ = '\0';
        char *t3 = strchr(t2, '\t');   if (!t3) continue; *t3++ = '\0';
        if (b->n == b->cap) {
            size_t want = b->cap ? b->cap * 2 : 8;
            brow *q = realloc(b->v, want * sizeof *q);
            if (!q) break;
            b->v = q; b->cap = want;
        }
        brow *r = &b->v[b->n++];
        memset(r, 0, sizeof *r);
        r->used = strtoull(line, NULL, 10);
        snprintf(r->ws,    sizeof r->ws,    "%s", t1);
        snprintf(r->label, sizeof r->label, "%s", t2);
        snprintf(r->user,  sizeof r->user,  "%s", t3);
    }
    fclose(f);
}

static int brow_cmp(const void *a, const void *b) {   /* most-recently-used first */
    uint64_t x = ((const brow *)a)->used, y = ((const brow *)b)->used;
    return x > y ? -1 : x < y ? 1 : 0;
}

static void bfold_write(oc_store *s, bfold *b) {
    char path[600], tmp[620];
    book_path(s, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return;
    for (size_t i = 0; i < b->n; i++)
        fprintf(f, "%llu\t%s\t%s\t%s\n", (unsigned long long)b->v[i].used,
                b->v[i].ws, b->v[i].label, b->v[i].user);
    fclose(f);
    if (b->n == 0) { remove(tmp); remove(path); return; }
    atomic_replace(tmp, path);
}

void oc_store_workspace_remember(oc_store *s, const char *workspace,
                                 const char *label, const char *username,
                                 uint64_t now_ms) {
    if (!s || !workspace || !workspace[0]) return;
    bfold b; bfold_load(s, &b);
    brow *r = NULL;
    for (size_t i = 0; i < b.n; i++)
        if (strcmp(b.v[i].ws, workspace) == 0) { r = &b.v[i]; break; }
    if (!r) {
        if (b.n == b.cap) {
            size_t want = b.cap ? b.cap * 2 : 8;
            brow *q = realloc(b.v, want * sizeof *q);
            if (!q) { bfold_free(&b); return; }
            b.v = q; b.cap = want;
        }
        r = &b.v[b.n++];
        memset(r, 0, sizeof *r);
        snprintf(r->ws, sizeof r->ws, "%s", workspace);
    }
    /* A NULL label/username preserves what is stored, so a silent reconnect —
     * which carries no credential — never blanks the switcher. */
    if (label && label[0])       snprintf(r->label, sizeof r->label, "%s", label);
    if (username && username[0]) snprintf(r->user,  sizeof r->user,  "%s", username);
    r->used = now_ms;
    if (b.n > 1) qsort(b.v, b.n, sizeof b.v[0], brow_cmp);
    bfold_write(s, &b);
    bfold_free(&b);
}

void oc_store_workspace_forget(oc_store *s, const char *workspace) {
    if (!s || !workspace) return;
    bfold b; bfold_load(s, &b);
    size_t w = 0;
    for (size_t i = 0; i < b.n; i++)
        if (strcmp(b.v[i].ws, workspace) != 0) b.v[w++] = b.v[i];
    b.n = w;
    bfold_write(s, &b);
    bfold_free(&b);
    /* "Forget" must leave nothing: the credential (token + pin) and both files. */
    if (s->secret) oc_secret_del(s->secret, workspace);
    char path[700];
    ws_path(s, workspace, "log", path, sizeof path); remove(path);
    ws_path(s, workspace, "out", path, sizeof path); remove(path);
}

void oc_store_workspace_each(oc_store *s, oc_store_workspace_cb cb, void *ctx) {
    if (!s || !cb) return;
    bfold b; bfold_load(s, &b);
    if (b.n > 1) qsort(b.v, b.n, sizeof b.v[0], brow_cmp);
    for (size_t i = 0; i < b.n; i++)
        cb(ctx, b.v[i].ws, b.v[i].label, b.v[i].user, b.v[i].used);
    bfold_free(&b);
}
