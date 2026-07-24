/* Outbound mobile-push emitter (ARCH-85). See push.h. The outbound HTTP mirrors
 * the enrollment client (daemon/enroll.c) over the CA-verifying TLS wrapper; JSON
 * is emitted with snprintf and parsed with the vendored jsmn (declarations only —
 * the impl lives in daemon/jwt.c's TU). Recipient selection is plain SQLite on a
 * dedicated read-only connection. */

#include "push.h"
#include "tls.h"

#include <mbedtls/base64.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>
#include <mbedtls/pk.h>
#include <mbedtls/sha256.h>

#define JSMN_HEADER
#include "jsmn.h"

#include <netdb.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define OC_PUSH_MAX_TARGETS 256
#define OC_PUSH_MAX_QUEUE   4096

/* --- RNG ------------------------------------------------------------------- */

typedef struct {
    mbedtls_entropy_context  entropy;
    mbedtls_ctr_drbg_context drbg;
} push_rng;

static int rng_init(push_rng *r) {
    mbedtls_entropy_init(&r->entropy);
    mbedtls_ctr_drbg_init(&r->drbg);
    const char *pers = "oc-push";
    return mbedtls_ctr_drbg_seed(&r->drbg, mbedtls_entropy_func, &r->entropy,
                                 (const unsigned char *)pers, strlen(pers));
}

static void rng_free(push_rng *r) {
    mbedtls_ctr_drbg_free(&r->drbg);
    mbedtls_entropy_free(&r->entropy);
}

static int b64_std(const uint8_t *in, size_t inlen, char *out, size_t outcap, size_t *outlen) {
    size_t ol = 0;
    if (mbedtls_base64_encode((unsigned char *)out, outcap, &ol, in, inlen) != 0) return -1;
    out[ol] = '\0';
    *outlen = ol;
    return 0;
}

/* --- minimal JSON reads (flat object + a string array), from daemon/enroll.c - */

#define PUSH_MAX_TOKENS 512

static int tok_eq(const char *js, const jsmntok_t *t, const char *s) {
    int len = t->end - t->start;
    return t->type == JSMN_STRING && len == (int)strlen(s) &&
           strncmp(js + t->start, s, (size_t)len) == 0;
}

static int skip_tok(const jsmntok_t *t, int i, int ntok) {
    if (i >= ntok) return i;
    if (t[i].type == JSMN_STRING || t[i].type == JSMN_PRIMITIVE) return i + 1;
    int j = i + 1;
    if (t[i].type == JSMN_OBJECT) {
        for (int c = 0; c < t[i].size; c++) { j = skip_tok(t, j, ntok); j = skip_tok(t, j, ntok); }
    } else if (t[i].type == JSMN_ARRAY) {
        for (int c = 0; c < t[i].size; c++) j = skip_tok(t, j, ntok);
    }
    return j;
}

static int find_value(const char *js, const jsmntok_t *t, int ntok, const char *key) {
    if (ntok < 1 || t[0].type != JSMN_OBJECT) return -1;
    int i = 1;
    for (int c = 0; c < t[0].size && i + 1 < ntok; c++) {
        int vidx = i + 1;
        if (tok_eq(js, &t[i], key)) return vidx;
        i = skip_tok(t, vidx, ntok);
    }
    return -1;
}

/* Iterate the string elements of the top-level "staleTokens" array, invoking cb
 * for each. Returns the number seen. */
static int json_stale_tokens(const char *js, size_t jlen, void (*cb)(void *, const char *), void *ud) {
    jsmntok_t toks[PUSH_MAX_TOKENS];
    jsmn_parser p;
    jsmn_init(&p);
    int n = jsmn_parse(&p, js, jlen, toks, PUSH_MAX_TOKENS);
    if (n < 0) return 0;
    int v = find_value(js, toks, n, "staleTokens");
    if (v < 0 || toks[v].type != JSMN_ARRAY) return 0;
    int count = toks[v].size;
    int idx = v + 1;
    int seen = 0;
    for (int i = 0; i < count && idx < n; i++) {
        if (toks[idx].type == JSMN_STRING) {
            size_t len = (size_t)(toks[idx].end - toks[idx].start);
            char tk[OC_DEVICE_TOKEN_MAX];
            if (len < sizeof tk) {
                memcpy(tk, js + toks[idx].start, len);
                tk[len] = '\0';
                cb(ud, tk);
                seen++;
            }
        }
        idx = skip_tok(toks, idx, n);
    }
    return seen;
}

/* --- outbound HTTP over CA-verified TLS (adapted from daemon/enroll.c) ------ */

typedef struct {
    char host[256];
    char endpoint[300];   /* host[:port]; sized above host+port so "%s:%s" can't truncate */
    char port[16];
    int  use_tls;
    oc_tls_client tls;
} push_ctx;

typedef struct {
    int         fd;
    int         tls;
    oc_tls_conn conn;
} push_conn;

static int parse_url(const char *url, push_ctx *ctx) {
    memset(ctx, 0, sizeof *ctx);
    const char *p = url;
    if (strncmp(p, "https://", 8) == 0) { p += 8; ctx->use_tls = 1; snprintf(ctx->port, sizeof ctx->port, "443"); }
    else if (strncmp(p, "http://", 7) == 0) { p += 7; ctx->use_tls = 0; snprintf(ctx->port, sizeof ctx->port, "80"); }
    else return -1;

    char hostport[256];
    size_t i = 0;
    while (*p && *p != '/' && i < sizeof hostport - 1) hostport[i++] = *p++;
    hostport[i] = '\0';
    if (i == 0) return -1;

    char *colon = strchr(hostport, ':');
    if (colon) { *colon = '\0'; snprintf(ctx->port, sizeof ctx->port, "%s", colon + 1); }
    snprintf(ctx->host, sizeof ctx->host, "%s", hostport);

    int is_default = (ctx->use_tls && strcmp(ctx->port, "443") == 0) ||
                     (!ctx->use_tls && strcmp(ctx->port, "80") == 0);
    if (is_default) snprintf(ctx->endpoint, sizeof ctx->endpoint, "%s", ctx->host);
    else snprintf(ctx->endpoint, sizeof ctx->endpoint, "%s:%s", ctx->host, ctx->port);
    return 0;
}

static int tcp_connect(const push_ctx *ctx) {
    struct addrinfo hints, *res = NULL, *rp;
    memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo(ctx->host, ctx->port, &hints, &res) != 0) return -1;
    int fd = -1;
    for (rp = res; rp; rp = rp->ai_next) {
        fd = socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (fd < 0) continue;
        if (connect(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        struct timeval tv = { 30, 0 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    }
    return fd;
}

static int conn_open(push_conn *c, push_ctx *ctx) {
    memset(c, 0, sizeof *c);
    c->fd = tcp_connect(ctx);
    if (c->fd < 0) return -1;
    if (!ctx->use_tls) return 0;
    if (oc_tls_conn_init(&c->conn, &ctx->tls.conf, c->fd) != 0) { close(c->fd); return -1; }
    if (oc_tls_conn_set_hostname(&c->conn, ctx->host) != 0) { oc_tls_conn_free(&c->conn); close(c->fd); return -1; }
    for (;;) {
        oc_tls_status st = oc_tls_handshake(&c->conn);
        if (st == OC_TLS_OK) break;
        if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
        oc_tls_conn_free(&c->conn); close(c->fd);
        return -1;
    }
    c->tls = 1;
    return 0;
}

static void conn_close(push_conn *c) {
    if (!c) return;
    if (c->tls) oc_tls_conn_free(&c->conn);
    if (c->fd >= 0) close(c->fd);
    c->fd = -1; c->tls = 0;
}

static int conn_write(push_conn *c, const void *buf, size_t len) {
    const char *p = buf; size_t sent = 0;
    while (sent < len) {
        if (c->tls) {
            size_t n = 0;
            oc_tls_status st = oc_tls_write(&c->conn, p + sent, len - sent, &n);
            if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
            if (st != OC_TLS_OK || n == 0) return -1;
            sent += n;
        } else {
            ssize_t n = write(c->fd, p + sent, len - sent);
            if (n <= 0) return -1;
            sent += (size_t)n;
        }
    }
    return 0;
}

static long conn_read(push_conn *c, void *buf, size_t cap) {
    if (c->tls) {
        for (;;) {
            size_t n = 0;
            oc_tls_status st = oc_tls_read(&c->conn, buf, cap, &n);
            if (st == OC_TLS_WANT_READ || st == OC_TLS_WANT_WRITE) continue;
            if (st == OC_TLS_CLOSED) return 0;
            if (st != OC_TLS_OK) return -1;
            return (long)n;
        }
    }
    ssize_t n = read(c->fd, buf, cap);
    return (n < 0) ? -1 : (long)n;
}

/* POST the signed notify body to /api/machine/push/notify. Adds the three CP-12
 * headers. Reads the whole response (Connection: close) and de-chunks a
 * Transfer-Encoding: chunked reply (Kestrel uses it for dynamic JSON). */
static int post_notify(push_ctx *ctx, const char *audience, long ts, const char *sig,
                       const char *body, int *status, char *resp, size_t resp_cap, size_t *resp_len) {
    push_conn c;
    if (conn_open(&c, ctx) != 0) return -1;

    char head[1024];
    size_t blen = strlen(body);
    int n = snprintf(head, sizeof head,
        "POST /api/machine/push/notify HTTP/1.1\r\nHost: %s\r\n"
        "Content-Type: application/json\r\n"
        "X-OpenChime-Audience: %s\r\nX-OpenChime-Timestamp: %ld\r\nX-OpenChime-Signature: %s\r\n"
        "Content-Length: %zu\r\nConnection: close\r\n\r\n",
        ctx->endpoint, audience, ts, sig, blen);
    if (n < 0 || n >= (int)sizeof head ||
        conn_write(&c, head, (size_t)n) != 0 || conn_write(&c, body, blen) != 0) {
        conn_close(&c);
        return -1;
    }

    char raw[16384];
    size_t total = 0;
    for (;;) {
        if (total >= sizeof raw - 1) break;
        long r = conn_read(&c, raw + total, sizeof raw - 1 - total);
        if (r < 0) { conn_close(&c); return -1; }
        if (r == 0) break;
        total += (size_t)r;
    }
    conn_close(&c);
    raw[total] = '\0';

    char *term = strstr(raw, "\r\n\r\n");
    if (!term || strncmp(raw, "HTTP/1.", 7) != 0) return -1;
    *status = atoi(raw + 9);

    int chunked = 0;
    for (char *p = raw; p < term; ) {
        char *eol = strstr(p, "\r\n");
        if (!eol || eol > term) break;
        if (strncasecmp(p, "Transfer-Encoding:", 18) == 0 && strcasestr(p, "chunked")) chunked = 1;
        p = eol + 2;
    }

    char *rbody = term + 4;
    char *end = raw + total;
    size_t out = 0;
    if (chunked) {
        char *p = rbody;
        while (p < end) {
            char *nl = memchr(p, '\n', (size_t)(end - p));
            if (!nl) break;
            long sz = strtol(p, NULL, 16);
            p = nl + 1;
            if (sz <= 0) break;
            if (p + sz > end) sz = (long)(end - p);
            if (out + (size_t)sz < resp_cap - 1) { memcpy(resp + out, p, (size_t)sz); out += (size_t)sz; }
            p += sz;
            if (p < end && *p == '\r') p++;
            if (p < end && *p == '\n') p++;
        }
    } else {
        size_t bl = (size_t)(end - rbody);
        if (bl > resp_cap - 1) bl = resp_cap - 1;
        memcpy(resp, rbody, bl);
        out = bl;
    }
    resp[out] = '\0';
    *resp_len = out;
    return 0;
}

/* --- exposed helpers ------------------------------------------------------- */

int oc_push_dnd_active(int enabled, int start_min, int end_min, int now_min) {
    if (!enabled) return 0;
    if (start_min == end_min) return 0;             /* empty window */
    if (start_min < end_min) return now_min >= start_min && now_min < end_min;
    return now_min >= start_min || now_min < end_min; /* wraps past midnight */
}

int oc_push_collect(sqlite3 *db, uint64_t channel_id, uint64_t author_id,
                    int now_min, oc_push_target *out, int max) {
    sqlite3_stmt *st = NULL;
    if (sqlite3_prepare_v2(db,
            "SELECT dt.platform, dt.token, u.dnd_enabled, u.dnd_start_min, u.dnd_end_min "
            "FROM channel_members cm "
            "JOIN device_tokens dt ON dt.user_id = cm.user_id "
            "JOIN users u ON u.id = cm.user_id "
            "LEFT JOIN notification_prefs np "
            "  ON np.user_id = cm.user_id AND np.channel_id = cm.channel_id "
            "WHERE cm.channel_id = ?1 AND cm.user_id <> ?2 "
            "  AND u.disabled = 0 AND COALESCE(np.level, 0) = 0;", -1, &st, NULL) != SQLITE_OK) {
        return 0;
    }
    sqlite3_bind_int64(st, 1, (sqlite3_int64)channel_id);
    sqlite3_bind_int64(st, 2, (sqlite3_int64)author_id);

    int n = 0;
    while (n < max && sqlite3_step(st) == SQLITE_ROW) {
        int dnd_en    = sqlite3_column_int(st, 2);
        int dnd_start = sqlite3_column_int(st, 3);
        int dnd_end   = sqlite3_column_int(st, 4);
        if (oc_push_dnd_active(dnd_en, dnd_start, dnd_end, now_min)) continue;

        const char *platform = (const char *)sqlite3_column_text(st, 0);
        const char *token    = (const char *)sqlite3_column_text(st, 1);
        if (!token || !*token) continue;
        out[n].platform = (platform && strcmp(platform, "fcm") == 0) ? OC_PUSH_FCM : OC_PUSH_APNS;
        snprintf(out[n].token, sizeof out[n].token, "%s", token);
        n++;
    }
    sqlite3_finalize(st);
    return n;
}

int oc_push_sign(const char *privkey_pem, const char *audience, const char *body,
                 long ts, char *sig_b64, size_t sig_cap) {
    push_rng rng;
    if (rng_init(&rng) != 0) { rng_free(&rng); return -1; }

    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc = -1;
    if (mbedtls_pk_parse_key(&pk, (const unsigned char *)privkey_pem, strlen(privkey_pem) + 1,
                             NULL, 0, mbedtls_ctr_drbg_random, &rng.drbg) != 0) goto done;

    uint8_t bh[32];
    if (mbedtls_sha256((const unsigned char *)body, strlen(body), bh, 0) != 0) goto done;
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", bh[i]);

    char canon[640];
    int cn = snprintf(canon, sizeof canon, "openchime-machine-v1|%s|%ld|%s", audience, ts, hex);
    if (cn < 0 || cn >= (int)sizeof canon) goto done;

    uint8_t h[32];
    if (mbedtls_sha256((const unsigned char *)canon, (size_t)cn, h, 0) != 0) goto done;

    uint8_t der[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t der_len = 0;
    if (mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, h, sizeof h, der, sizeof der, &der_len,
                        mbedtls_ctr_drbg_random, &rng.drbg) != 0) goto done;

    size_t sl = 0;
    if (b64_std(der, der_len, sig_b64, sig_cap, &sl) != 0) goto done;
    rc = 0;

done:
    mbedtls_pk_free(&pk);
    rng_free(&rng);
    return rc;
}

int oc_push_build_body(uint64_t channel_id, const oc_push_target *targets, int n,
                       char *out, size_t cap) {
    char chbuf[24];
    snprintf(chbuf, sizeof chbuf, "%llu", (unsigned long long)channel_id);

    int w = snprintf(out, cap, "{\"notifications\":[");
    if (w < 0 || (size_t)w >= cap) return -1;
    size_t off = (size_t)w;

    for (int i = 0; i < n; i++) {
        const char *plat = targets[i].platform == OC_PUSH_FCM ? "fcm" : "apns";
        w = snprintf(out + off, cap - off,
                     "%s{\"platform\":\"%s\",\"token\":\"%s\",\"channelId\":\"%s\"}",
                     i ? "," : "", plat, targets[i].token, chbuf);
        if (w < 0 || (size_t)w >= cap - off) return -1;
        off += (size_t)w;
    }
    w = snprintf(out + off, cap - off, "]}");
    if (w < 0 || (size_t)w >= cap - off) return -1;
    return 0;
}

/* --- worker ---------------------------------------------------------------- */

typedef struct pnode {
    uint64_t channel_id;
    uint64_t author_id;
    struct pnode *next;
} pnode;

struct oc_push {
    oc_dbwriter    *dbw;         /* borrowed; for pruning stale tokens */
    push_ctx        ctx;
    char           *audience;
    char           *privkey;
    sqlite3        *rdb;         /* own read-only connection */

    pthread_t       thread;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    pnode          *head, *tail;
    int             qlen;
    int             stopping;
    int             started;
};

static void prune_cb(void *ud, const char *token) {
    oc_push *p = ud;
    oc_dbwriter_prune_device_token(p->dbw, token);
}

static void do_notify(oc_push *p, uint64_t channel_id, uint64_t author_id) {
    time_t nowsec = time(NULL);
    int now_min = (int)((nowsec / 60) % 1440);      /* minutes-of-day UTC */

    oc_push_target targets[OC_PUSH_MAX_TARGETS];
    int n = oc_push_collect(p->rdb, channel_id, author_id, now_min, targets, OC_PUSH_MAX_TARGETS);
    if (n <= 0) return;

    size_t cap = (size_t)n * (OC_DEVICE_TOKEN_MAX + 96) + 64;
    char *body = malloc(cap);
    if (!body) return;
    if (oc_push_build_body(channel_id, targets, n, body, cap) != 0) { free(body); return; }

    long ts = (long)nowsec;
    char sig[512];
    if (oc_push_sign(p->privkey, p->audience, body, ts, sig, sizeof sig) != 0) { free(body); return; }

    int status = 0;
    char resp[8192];
    size_t rlen = 0;
    if (post_notify(&p->ctx, p->audience, ts, sig, body, &status, resp, sizeof resp, &rlen) == 0 &&
        status == 200) {
        json_stale_tokens(resp, rlen, prune_cb, p);
    }
    free(body);
}

static void *worker(void *arg) {
    oc_push *p = arg;
    for (;;) {
        pthread_mutex_lock(&p->mu);
        while (!p->stopping && !p->head) pthread_cond_wait(&p->cv, &p->mu);
        if (p->stopping && !p->head) { pthread_mutex_unlock(&p->mu); break; }
        pnode *node = p->head;
        p->head = node->next;
        if (!p->head) p->tail = NULL;
        p->qlen--;
        pthread_mutex_unlock(&p->mu);

        do_notify(p, node->channel_id, node->author_id);
        free(node);
    }
    return NULL;
}

oc_push *oc_push_start(const char *db_path, oc_dbwriter *dbw,
                       const char *push_url, const char *ca_bundle,
                       const char *audience, const char *privkey_pem) {
    if (!db_path || !dbw || !push_url || !*push_url || !audience || !privkey_pem) return NULL;

    oc_push *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->dbw = dbw;
    p->audience = strdup(audience);
    p->privkey = strdup(privkey_pem);
    if (!p->audience || !p->privkey) goto fail;

    if (parse_url(push_url, &p->ctx) != 0) goto fail;
    if (p->ctx.use_tls && oc_tls_client_init_ca(&p->ctx.tls, ca_bundle) != 0) goto fail;

    if (sqlite3_open_v2(db_path, &p->rdb, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) goto fail_tls;

    if (pthread_mutex_init(&p->mu, NULL) != 0) goto fail_db;
    if (pthread_cond_init(&p->cv, NULL) != 0) { pthread_mutex_destroy(&p->mu); goto fail_db; }
    if (pthread_create(&p->thread, NULL, worker, p) != 0) {
        pthread_cond_destroy(&p->cv); pthread_mutex_destroy(&p->mu); goto fail_db;
    }
    p->started = 1;
    return p;

fail_db:
    sqlite3_close(p->rdb);
fail_tls:
    if (p->ctx.use_tls) oc_tls_client_free(&p->ctx.tls);
fail:
    free(p->audience);
    free(p->privkey);
    free(p);
    return NULL;
}

void oc_push_notify(oc_push *p, uint64_t channel_id, uint64_t author_id) {
    if (!p) return;
    pthread_mutex_lock(&p->mu);
    if (p->stopping || p->qlen >= OC_PUSH_MAX_QUEUE) { pthread_mutex_unlock(&p->mu); return; }
    pnode *node = calloc(1, sizeof *node);
    if (!node) { pthread_mutex_unlock(&p->mu); return; }
    node->channel_id = channel_id;
    node->author_id = author_id;
    if (p->tail) p->tail->next = node; else p->head = node;
    p->tail = node;
    p->qlen++;
    pthread_cond_signal(&p->cv);
    pthread_mutex_unlock(&p->mu);
}

void oc_push_stop(oc_push *p) {
    if (!p) return;
    if (p->started) {
        pthread_mutex_lock(&p->mu);
        p->stopping = 1;
        pthread_cond_broadcast(&p->cv);
        pthread_mutex_unlock(&p->mu);
        pthread_join(p->thread, NULL);
        pthread_cond_destroy(&p->cv);
        pthread_mutex_destroy(&p->mu);
    }
    for (pnode *n = p->head; n; ) { pnode *nx = n->next; free(n); n = nx; }
    if (p->rdb) sqlite3_close(p->rdb);
    if (p->ctx.use_tls) oc_tls_client_free(&p->ctx.tls);
    free(p->audience);
    free(p->privkey);
    free(p);
}
