/*
 * OpenChime client — the local store, which stores nothing locally (ARCH-88).
 *
 * Everything durable lives in ONE place: the OS credential store, one entry per
 * workspace holding the session token, the TOFU pin, and the book fields (the
 * address the user typed, the account, and a last-used stamp). Because there is
 * one credential per workspace, the *list* of credentials IS the workspace book —
 * enumeration replaces a file (oc_secret_each).
 *
 * Cached history and the offline outbox are deliberately NOT here. The daemon is
 * the source of truth for history and already remembers each user's read position
 * server-side (delivery_cursors, REQ-090), so a cold client asks the server where
 * it was rather than remembering — Slack's model. The outbox lives in RAM on the
 * net thread for the life of the process (REQ-102 asks for "queued locally, sent
 * on reconnect", not for surviving a process exit).
 *
 * So: no database, and no files at all. `path` is kept in the API only so a
 * caller can still say "persistence off" by passing NULL.
 */

#include "store.h"

#include <stdlib.h>
#include <string.h>

struct oc_store { oc_secret *secret; };

static void put_u64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t get_u64(const uint8_t *p) {
    uint64_t v = 0; for (int i = 0; i < 8; i++) v |= (uint64_t)p[i] << (8 * i); return v;
}

/* ---- credential blob -------------------------------------------------------
 * One entry per workspace holding the token AND the pin. The pin is not secret,
 * but it is integrity-sensitive — rewriting a pin is how a MITM gets accepted
 * (ARCH-10) — so it belongs beside the credential, not in a file anyone can
 * edit. The book fields ride along so the credential IS the book entry. */
#define SEC_VER    1
#define SEC_LABEL  128
#define SEC_USER   128
/* [ver][flags][expiry u64][token 32][pin 32][last_used u64][label 128][user 128] */
#define SEC_BLOB   (2 + 8 + OC_SESSION_TOKEN_LEN + OC_TLS_FINGERPRINT_LEN + 8 + SEC_LABEL + SEC_USER)
enum { SEC_HAS_TOKEN = 1, SEC_HAS_PIN = 2, SEC_HAS_BOOK = 4 };
#define SEC_EXPIRY(b) ((b) + 2)
#define SEC_TOKEN(b)  ((b) + 10)
#define SEC_PIN(b)    ((b) + 10 + OC_SESSION_TOKEN_LEN)
#define SEC_USED(b)   ((b) + 10 + OC_SESSION_TOKEN_LEN + OC_TLS_FINGERPRINT_LEN)
#define SEC_LBL(b)    ((char *)((b) + 18 + OC_SESSION_TOKEN_LEN + OC_TLS_FINGERPRINT_LEN))
#define SEC_USR(b)    (SEC_LBL(b) + SEC_LABEL)

static int sec_load(oc_store *s, const char *ws, uint8_t *blob) {
    size_t got = 0;
    memset(blob, 0, SEC_BLOB);
    if (!oc_secret_get(s->secret, ws, blob, SEC_BLOB, &got)) { memset(blob, 0, SEC_BLOB); return 0; }
    if (got != SEC_BLOB || blob[0] != SEC_VER) { memset(blob, 0, SEC_BLOB); return 0; }
    SEC_LBL(blob)[SEC_LABEL - 1] = '\0';   /* fixed-width fields; never trust the tail */
    SEC_USR(blob)[SEC_USER - 1]  = '\0';
    return 1;
}
static void sec_store(oc_store *s, const char *ws, uint8_t *blob) {
    blob[0] = SEC_VER;
    if (!(blob[1] & (SEC_HAS_TOKEN | SEC_HAS_PIN | SEC_HAS_BOOK))) oc_secret_del(s->secret, ws);
    else oc_secret_put(s->secret, ws, blob, SEC_BLOB);
}

/* ---- open / close ---------------------------------------------------------- */

oc_store *oc_store_open(const char *path) {
    /* Nothing is opened: the store writes no files. `path` only distinguishes
     * "persistence on" from NULL ("off"), which callers already use for the
     * sign-in screen's Remember-me. */
    if (!path || !path[0]) return NULL;
    return calloc(1, sizeof(oc_store));
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


/* ---- the workspace book (credential enumeration) ---------------------------
 * No file: one credential per workspace, so listing the credentials IS listing
 * the book. The address/account/last-used ride in the same blob as the token and
 * pin, which is what makes "forget" a single delete with nothing left behind. */

void oc_store_workspace_remember(oc_store *s, const char *workspace,
                                 const char *label, const char *username,
                                 uint64_t now_ms) {
    if (!s || !workspace || !workspace[0] || !s->secret) return;
    uint8_t b[SEC_BLOB];
    sec_load(s, workspace, b);          /* keep the token/pin already stored */
    b[1] |= SEC_HAS_BOOK;
    put_u64(SEC_USED(b), now_ms);
    /* A NULL label/username preserves what is stored, so a silent reconnect —
     * which carries no credential — never blanks the switcher. */
    if (label && label[0])       snprintf(SEC_LBL(b), SEC_LABEL, "%s", label);
    if (username && username[0]) snprintf(SEC_USR(b), SEC_USER, "%s", username);
    sec_store(s, workspace, b);
}

void oc_store_workspace_forget(oc_store *s, const char *workspace) {
    if (!s || !workspace || !s->secret) return;
    oc_secret_del(s->secret, workspace);   /* token, pin and book in one delete */
}

/* Enumeration hands back accounts in no particular order, so collect and sort
 * most-recently-used first — the order the switcher renders. */
typedef struct { char ws[256]; char label[SEC_LABEL]; char user[SEC_USER]; uint64_t used; } bentry;
typedef struct { oc_store *s; bentry *v; size_t n, cap; } bcollect;

static void book_collect(void *ud, const char *account) {
    bcollect *c = ud;
    uint8_t b[SEC_BLOB];
    if (!sec_load(c->s, account, b) || !(b[1] & SEC_HAS_BOOK)) return;
    if (c->n == c->cap) {
        size_t want = c->cap ? c->cap * 2 : 8;
        bentry *q = realloc(c->v, want * sizeof *q);
        if (!q) return;
        c->v = q; c->cap = want;
    }
    bentry *e = &c->v[c->n++];
    snprintf(e->ws,    sizeof e->ws,    "%.*s", (int)sizeof e->ws - 1, account);
    snprintf(e->label, sizeof e->label, "%.*s", SEC_LABEL - 1, SEC_LBL(b));
    snprintf(e->user,  sizeof e->user,  "%.*s", SEC_USER - 1,  SEC_USR(b));
    e->used = get_u64(SEC_USED(b));
}

static int bentry_cmp(const void *a, const void *b) {
    uint64_t x = ((const bentry *)a)->used, y = ((const bentry *)b)->used;
    return x > y ? -1 : x < y ? 1 : 0;
}

void oc_store_workspace_each(oc_store *s, oc_store_workspace_cb cb, void *ctx) {
    if (!s || !cb || !s->secret) return;
    bcollect c = { s, NULL, 0, 0 };
    if (!oc_secret_each(s->secret, book_collect, &c)) { free(c.v); return; }
    if (c.n > 1) qsort(c.v, c.n, sizeof c.v[0], bentry_cmp);
    for (size_t i = 0; i < c.n; i++)
        cb(ctx, c.v[i].ws, c.v[i].label, c.v[i].user, c.v[i].used);
    free(c.v);
}
