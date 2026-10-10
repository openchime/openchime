/*
 * OpenChime web client -- the credential store (ARCH-74, CLIENT.md §4), in
 * memory with localStorage behind it. A browser page has no keyring the core
 * can reach; localStorage is what the page has, per origin, and it keeps the
 * remembered workspaces and session tokens across a reload. Values are bytes,
 * stored base64 under "oc.secret.<account>". It is not an encrypted store: a
 * token here is as safe as the origin's storage, which is the same standing
 * every web application's session has.
 */
#include "secret.h"
#include "secret_os.h"
#include <emscripten.h>
#include <stdlib.h>
#include <string.h>

typedef struct { char *account; uint8_t *val; size_t len; } mem_entry;
typedef struct { mem_entry *e; size_t n, cap; } mem_store;

static mem_entry *find(mem_store *s, const char *account) {
    for (size_t i = 0; i < s->n; i++)
        if (strcmp(s->e[i].account, account) == 0) return &s->e[i];
    return NULL;
}

static int mem_get(void *ctx, const char *account, uint8_t *out, size_t cap, size_t *len) {
    mem_entry *e = find(ctx, account);
    if (!e || e->len > cap) return 0;
    memcpy(out, e->val, e->len);
    *len = e->len;
    return 1;
}

static int put_mem(mem_store *s, const char *account, const uint8_t *val, size_t len) {
    uint8_t *copy = malloc(len ? len : 1);
    if (!copy) return 0;
    memcpy(copy, val, len);
    mem_entry *e = find(s, account);
    if (e) { free(e->val); e->val = copy; e->len = len; return 1; }
    if (s->n == s->cap) {
        size_t cap = s->cap ? s->cap * 2 : 8;
        mem_entry *g = realloc(s->e, cap * sizeof *g);
        if (!g) { free(copy); return 0; }
        s->e = g; s->cap = cap;
    }
    char *name = strdup(account);
    if (!name) { free(copy); return 0; }
    s->e[s->n].account = name; s->e[s->n].val = copy; s->e[s->n].len = len;
    s->n++;
    return 1;
}

/* localStorage belongs to the page's thread; the core writes a token from
 * its network thread (a Worker). The write is handed to the main thread with
 * copies it frees. */
static int mem_put(void *ctx, const char *account, const uint8_t *val, size_t len) {
    if (!put_mem(ctx, account, val, len)) return 0;
    char *acc = strdup(account);
    uint8_t *bytes = malloc(len ? len : 1);
    if (!acc || !bytes) { free(acc); free(bytes); return 1; }
    memcpy(bytes, val, len);
    MAIN_THREAD_ASYNC_EM_ASM({
        try {
            const b = HEAPU8.subarray($1, $1 + $2);
            let s = ""; for (let i = 0; i < b.length; i++) s += String.fromCharCode(b[i]);
            localStorage.setItem("oc.secret." + UTF8ToString($0), btoa(s));
        } catch (e) {}
        _free($0); _free($1);
    }, acc, bytes, (int)len);
    return 1;
}

static void mem_del(void *ctx, const char *account) {
    mem_store *s = ctx;
    mem_entry *e = find(s, account);
    char *acc = strdup(account);
    if (acc) MAIN_THREAD_ASYNC_EM_ASM({ try { localStorage.removeItem("oc.secret." + UTF8ToString($0)); } catch (e) {} _free($0); }, acc);
    if (!e) return;
    free(e->account); free(e->val);
    *e = s->e[--s->n];
}

static int mem_each(void *ctx, oc_secret_each_cb cb, void *ud) {
    mem_store *s = ctx;
    for (size_t i = 0; i < s->n; i++) cb(ud, s->e[i].account);
    return 1;
}

static void mem_close(void *ctx) {
    mem_store *s = ctx;
    for (size_t i = 0; i < s->n; i++) { free(s->e[i].account); free(s->e[i].val); }
    free(s->e);
    free(s);
}

/* Called from the page for each stored entry at open: account, bytes. */
static mem_store *g_loading;
EMSCRIPTEN_KEEPALIVE void oc_web_secret_load(const char *account, const uint8_t *val, int len) {
    if (g_loading) put_mem(g_loading, account, val, (size_t)len);
}

oc_secret *oc_secret_open_mem(void) {
    oc_secret *sec = calloc(1, sizeof *sec);
    mem_store *s = calloc(1, sizeof *s);
    if (!sec || !s) { free(sec); free(s); return NULL; }
    sec->get = mem_get; sec->put = mem_put; sec->del = mem_del; sec->each = mem_each;
    sec->close = mem_close; sec->ctx = s;
    g_loading = s;
    EM_ASM({
        try {
            for (let i = 0; i < localStorage.length; i++) {
                const k = localStorage.key(i);
                if (!k || !k.startsWith("oc.secret.")) continue;
                const bin = atob(localStorage.getItem(k) || "");
                const p = _malloc(bin.length + 1);
                for (let j = 0; j < bin.length; j++) HEAPU8[p + j] = bin.charCodeAt(j);
                const a = stringToNewUTF8(k.slice(10));
                _oc_web_secret_load(a, p, bin.length);
                _free(a); _free(p);
            }
        } catch (e) {}
    });
    g_loading = NULL;
    return sec;
}

/* The frontend's one name for the platform store (secret_os.h). */
oc_secret *oc_secret_open_os(const char *service) { (void)service; return oc_secret_open_mem(); }
