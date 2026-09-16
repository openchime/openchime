/* The guesser: packing a Phonetisaurus joint n-gram model into guesses.bin, and
 * decoding a word against the mapped file (tts_priv.h describes the format;
 * TTSKIT.md §5 explains the model).
 *
 * The model's vocabulary is joint tokens — a chunk of one or two letters paired
 * with zero, one or two phonemes ("ph}f", "e|r}ɚ", "e}_") — and its n-grams say
 * how likely each token is after the ones before it. Guessing a word is finding
 * the split of its letters into tokens whose sequence the model scores best;
 * the phonemes of those tokens, in order, are the pronunciation. */
#define _POSIX_C_SOURCE 200809L
#include "tts_priv.h"

#include <float.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- packing ------------------------------------------------------------------------ */

typedef struct { char *name; } ptoken;
typedef struct { uint16_t id[TTS_GS_MAXORDER]; float lp, bo; } pgram;

static int g_cmp_order;
static int cmp_gram(const void *a, const void *b) {
    const pgram *x = a, *y = b;
    for (int i = 0; i < g_cmp_order; i++)
        if (x->id[i] != y->id[i]) return x->id[i] < y->id[i] ? -1 : 1;
    return 0;
}

static void w32(FILE *f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}
static void w16(FILE *f, uint16_t v) { uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) }; fwrite(b, 1, 2, f); }
static void wf(FILE *f, float v) { uint32_t u; memcpy(&u, &v, 4); w32(f, u); }

/* Token names are unique and few (hundreds); a linear table with a hash in front
 * keeps packing simple. */
typedef struct { ptoken *v; uint32_t n, cap; uint32_t *hash; uint32_t hcap; } ptokens;

static uint32_t fnv(const char *s) { uint32_t h = 2166136261u; while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; } return h; }

static int tok_id(ptokens *t, const char *name, int add) {
    uint32_t h = fnv(name) & (t->hcap - 1);
    while (t->hash[h]) {
        if (!strcmp(t->v[t->hash[h] - 1].name, name)) return (int)t->hash[h] - 1;
        h = (h + 1) & (t->hcap - 1);
    }
    if (!add || t->n >= 65535 || t->n * 2 >= t->hcap) return -1;
    if (t->n == t->cap) {
        t->cap = t->cap ? t->cap * 2 : 1024;
        ptoken *nv = realloc(t->v, t->cap * sizeof *nv);
        if (!nv) return -1;
        t->v = nv;
    }
    t->v[t->n].name = strdup(name);
    if (!t->v[t->n].name) return -1;
    t->hash[h] = t->n + 1;
    return (int)t->n++;
}

int tts_pack_guesser(const char *arpa_path, const char *bin_path, const char *lang,
                     char *err, size_t errcap) {
    /* Refused rather than truncated, for the reason tts_pack_lexicon gives. */
    if (!lang || !*lang || strlen(lang) >= TTS_LANG_MAX) {
        tts_seterr(err, errcap, "a language tag of 1 to %u characters is required", TTS_LANG_MAX - 1);
        return -1;
    }
    FILE *in = fopen(arpa_path, "r");
    if (!in) { tts_seterr(err, errcap, "cannot open %s", arpa_path); return -1; }
    ptokens toks = { 0 };
    toks.hcap = 1 << 16;
    toks.hash = calloc(toks.hcap, sizeof *toks.hash);
    pgram *grams[TTS_GS_MAXORDER + 1] = { 0 };
    uint32_t cnt[TTS_GS_MAXORDER + 1] = { 0 }, declared[TTS_GS_MAXORDER + 1] = { 0 };
    int order = 0, section = 0, rc = -1;
    char *line = NULL;
    size_t lcap = 0;
    if (!toks.hash) { tts_seterr(err, errcap, "out of memory"); goto out; }
    /* <s> and </s> first, so their ids are fixed at 0 and 1. */
    tok_id(&toks, "<s>", 1);
    tok_id(&toks, "</s>", 1);

    while (getline(&line, &lcap, in) > 0) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (!line[0]) continue;
        int k, c;
        if (sscanf(line, "ngram %d=%d", &k, &c) == 2) {
            if (k < 1 || k > TTS_GS_MAXORDER || c < 0) { tts_seterr(err, errcap, "unsupported order %d", k); goto out; }
            declared[k] = (uint32_t)c;
            if (k > order) order = k;
            continue;
        }
        if (line[0] == '\\') {
            if (sscanf(line, "\\%d-grams:", &k) == 1) {
                if (k < 1 || k > order) { tts_seterr(err, errcap, "section for undeclared order %d", k); goto out; }
                section = k;
                grams[k] = calloc(declared[k] ? declared[k] : 1, sizeof *grams[k]);
                if (!grams[k]) { tts_seterr(err, errcap, "out of memory"); goto out; }
            } else {
                section = 0;                                  /* \data\ or \end\ */
            }
            continue;
        }
        if (!section) continue;
        /* logprob TAB w1 w2 ... [TAB backoff] */
        char *f1 = strtok(line, "\t");
        char *f2 = strtok(NULL, "\t");
        char *f3 = strtok(NULL, "\t");
        if (!f1 || !f2) continue;
        if (cnt[section] >= declared[section]) { tts_seterr(err, errcap, "more %d-grams than declared", section); goto out; }
        pgram *g = &grams[section][cnt[section]];
        g->lp = (float)strtod(f1, NULL);
        g->bo = f3 ? (float)strtod(f3, NULL) : 0.0f;
        int w = 0;
        for (char *tok = strtok(f2, " "); tok; tok = strtok(NULL, " ")) {
            if (w >= section) break;
            int id = tok_id(&toks, tok, section == 1);
            if (id < 0) { tts_seterr(err, errcap, "unknown token %s in a %d-gram", tok, section); goto out; }
            g->id[w++] = (uint16_t)id;
        }
        if (w != section) { tts_seterr(err, errcap, "a %d-gram with %d words", section, w); goto out; }
        cnt[section]++;
    }
    if (order == 0 || !grams[1]) { tts_seterr(err, errcap, "%s has no n-grams", arpa_path); goto out; }
    for (int k = 1; k <= order; k++) {
        if (cnt[k] != declared[k]) { tts_seterr(err, errcap, "%d-grams: %u declared, %u read", k, declared[k], cnt[k]); goto out; }
        g_cmp_order = k;
        qsort(grams[k], cnt[k], sizeof *grams[k], cmp_gram);
    }

    {
        FILE *out = fopen(bin_path, "wb");
        if (!out) { tts_seterr(err, errcap, "cannot create %s", bin_path); goto out; }
        /* Split each token name into graphemes and phonemes, '|' removed and "_"
         * as the empty phoneme; <s> and </s> have neither. */
        char **graph = calloc(toks.n, sizeof *graph), **phon = calloc(toks.n, sizeof *phon);
        uint32_t pool = 0;
        for (uint32_t i = 0; i < toks.n && graph && phon; i++) {
            const char *name = toks.v[i].name;
            const char *br = i < 2 ? NULL : strchr(name, '}');
            graph[i] = calloc(strlen(name) + 1, 1);
            phon[i] = calloc(strlen(name) + 1, 1);
            if (!graph[i] || !phon[i]) break;
            if (br) {
                size_t gn = 0, pn = 0;
                for (const char *q = name; q < br; q++) if (*q != '|') graph[i][gn++] = *q;
                if (strcmp(br + 1, "_") != 0)
                    for (const char *q = br + 1; *q; q++) if (*q != '|') phon[i][pn++] = *q;
            }
            pool += (uint32_t)(strlen(graph[i]) + 1 + strlen(phon[i]) + 1);
        }
        char tag[TTS_LANG_MAX];
        memset(tag, 0, sizeof tag);
        memcpy(tag, lang, strlen(lang));
        fwrite(TTS_GS_MAGIC, 1, 8, out);
        w32(out, TTS_GS_VERSION); w32(out, (uint32_t)order); w32(out, toks.n); w32(out, pool);
        for (int k = 1; k <= TTS_GS_MAXORDER; k++) w32(out, k <= order ? cnt[k] : 0);
        w32(out, 0); w32(out, 0);
        fwrite(tag, 1, sizeof tag, out);
        uint32_t off = 0;
        for (uint32_t i = 0; i < toks.n; i++) {
            w32(out, off); off += (uint32_t)strlen(graph[i]) + 1;
            w32(out, off); off += (uint32_t)strlen(phon[i]) + 1;
        }
        for (uint32_t i = 0; i < toks.n; i++) {
            fwrite(graph[i], 1, strlen(graph[i]) + 1, out);
            fwrite(phon[i], 1, strlen(phon[i]) + 1, out);
        }
        for (int k = 1; k <= order; k++)
            for (uint32_t i = 0; i < cnt[k]; i++) {
                for (int j = 0; j < k; j++) w16(out, grams[k][i].id[j]);
                wf(out, grams[k][i].lp);
                wf(out, grams[k][i].bo);
            }
        rc = ferror(out) ? -1 : 0;
        if (fclose(out) != 0) rc = -1;
        if (rc) tts_seterr(err, errcap, "write failed: %s", bin_path);
        for (uint32_t i = 0; i < toks.n; i++) { if (graph) free(graph[i]); if (phon) free(phon[i]); }
        free(graph); free(phon);
    }
out:
    free(line);
    for (uint32_t i = 0; i < toks.n; i++) free(toks.v[i].name);
    free(toks.v); free(toks.hash);
    for (int k = 0; k <= TTS_GS_MAXORDER; k++) free(grams[k]);
    fclose(in);
    return rc;
}

/* ---- opening ------------------------------------------------------------------------ */

static const char *tok_graph(const tts_guesser *g, uint32_t id) { return g->pool + tts_rd32(g->tokens + (size_t)id * 8); }
static const char *tok_phon(const tts_guesser *g, uint32_t id)  { return g->pool + tts_rd32(g->tokens + (size_t)id * 8 + 4); }

int tts_guesser_open(const char *path, const tts_map *mem, tts_guesser *g, char *err, size_t errcap) {
    memset(g, 0, sizeof *g);
    if (mem ? (g->map = *mem, g->map.owned = 0, !mem->p) : tts_map_open(path, &g->map) != 0) { tts_seterr(err, errcap, "cannot map %s", path); return -1; }
    const uint8_t *p = g->map.p;
    size_t n = g->map.n;
    if (n < TTS_GS_HEADER || memcmp(p, TTS_GS_MAGIC, 8) != 0 || tts_rd32(p + 8) != TTS_GS_VERSION) {
        tts_seterr(err, errcap, "%s is not a version %u ttskit guesser", path, TTS_GS_VERSION);
        tts_map_close(&g->map);
        return -1;
    }
    g->order = tts_rd32(p + 12);
    g->n_tokens = tts_rd32(p + 16);
    uint32_t pool = tts_rd32(p + 20);
    size_t need = TTS_GS_HEADER + (size_t)g->n_tokens * 8 + pool;
    int ok = g->order >= 1 && g->order <= TTS_GS_MAXORDER && g->n_tokens >= 2 && g->n_tokens <= 65535 && need <= n;
    for (uint32_t k = 1; ok && k <= g->order; k++) {
        g->count[k] = tts_rd32(p + 24 + (k - 1) * 4);
        size_t entry = 2 * (size_t)k + 8;
        g->table[k] = p + need;
        if ((n - need) / entry < g->count[k]) ok = 0;
        else need += entry * g->count[k];
    }
    if (!ok || need != n) {
        tts_seterr(err, errcap, "%s is truncated or corrupt", path);
        tts_map_close(&g->map);
        return -1;
    }
    if (memchr(p + 64, '\0', TTS_LANG_MAX) == NULL) {
        tts_seterr(err, errcap, "%s has an unterminated language tag", path);
        tts_map_close(&g->map);
        return -1;
    }
    memcpy(g->lang, p + 64, TTS_LANG_MAX);
    g->tokens = p + TTS_GS_HEADER;
    g->pool = (const char *)p + TTS_GS_HEADER + (size_t)g->n_tokens * 8;
    for (uint32_t i = 0; i < g->n_tokens; i++) {
        if (tts_rd32(g->tokens + (size_t)i * 8) >= pool || tts_rd32(g->tokens + (size_t)i * 8 + 4) >= pool) {
            tts_seterr(err, errcap, "%s has a token outside its pool", path);
            tts_map_close(&g->map);
            return -1;
        }
    }
    if (g->pool[pool - 1] != '\0') { tts_seterr(err, errcap, "%s pool is unterminated", path); tts_map_close(&g->map); return -1; }

    /* Tokens by first grapheme byte (counting sort). */
    g->by_first = calloc(g->n_tokens ? g->n_tokens : 1, sizeof *g->by_first);
    if (!g->by_first) { tts_map_close(&g->map); tts_seterr(err, errcap, "out of memory"); return -1; }
    uint32_t counts[256] = { 0 };
    for (uint32_t i = 2; i < g->n_tokens; i++) { const char *s = tok_graph(g, i); if (s[0]) counts[(uint8_t)s[0]]++; }
    g->by_first_start[0] = 0;
    for (int c = 0; c < 256; c++) g->by_first_start[c + 1] = g->by_first_start[c] + counts[c];
    uint32_t fill[256];
    memcpy(fill, g->by_first_start, sizeof fill);
    for (uint32_t i = 2; i < g->n_tokens; i++) { const char *s = tok_graph(g, i); if (s[0]) g->by_first[fill[(uint8_t)s[0]]++] = (uint16_t)i; }
    return 0;
}

void tts_guesser_close(tts_guesser *g) {
    free(g->by_first);
    tts_map_close(&g->map);
    memset(g, 0, sizeof *g);
}

/* ---- scoring -------------------------------------------------------------------------- */

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static float rdf(const uint8_t *p) { uint32_t u = tts_rd32(p); float f; memcpy(&f, &u, 4); return f; }

/* The entry for the id tuple `ids` (length k), or NULL. */
static const uint8_t *find_gram(const tts_guesser *g, const uint16_t *ids, uint32_t k) {
    if (k < 1 || k > g->order || !g->count[k]) return NULL;
    size_t entry = 2 * (size_t)k + 8, lo = 0, hi = g->count[k];
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const uint8_t *e = g->table[k] + mid * entry;
        int c = 0;
        for (uint32_t j = 0; j < k && !c; j++) {
            uint16_t v = rd16(e + 2 * j);
            if (ids[j] != v) c = ids[j] < v ? -1 : 1;
        }
        if (!c) return e;
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

/* log10 P(w | hist[0..hn)) with Katz backoff: the longest n-gram present, plus the
 * backoff weights of the histories that were shortened to reach it. */
static float logprob(const tts_guesser *g, const uint16_t *hist, uint32_t hn, uint16_t w) {
    uint16_t ids[TTS_GS_MAXORDER];
    float bo = 0.0f;
    if (hn > g->order - 1) { hist += hn - (g->order - 1); hn = g->order - 1; }
    for (uint32_t start = 0; start <= hn; start++) {
        uint32_t k = hn - start + 1;
        memcpy(ids, hist + start, (hn - start) * sizeof *ids);
        ids[k - 1] = w;
        const uint8_t *e = find_gram(g, ids, k);
        if (e) return bo + rdf(e + 2 * k);
        if (k > 1) {
            const uint8_t *h = find_gram(g, hist + start, k - 1);
            if (h) bo += rdf(h + 2 * (k - 1) + 4);
        }
    }
    return -99.0f;                                     /* unseen token: effectively impossible */
}

/* ---- decoding ------------------------------------------------------------------------- */

/* Hypotheses kept per letter position. 32 agrees with Phonetisaurus's own
 * decoder on 3,009 of 3,010 words at ~1.7 ms a word; 16 drops to 99.8%, and
 * 256 is 18 times slower for the same answers (TTSKIT.md §5). */
#define BEAM 32

typedef struct {
    uint16_t hist[TTS_GS_MAXORDER - 1];
    uint8_t  hn;
    double   cost;                 /* -log10 probability so far */
    int32_t  prev;                 /* hypothesis index at the earlier position, -1 at start */
    uint16_t token;
} hyp;

int tts_guess(const tts_guesser *g, const char *word, char *ipa, size_t cap) {
    if (!ipa || cap == 0) return 0;
    ipa[0] = '\0';
    size_t len = strlen(word);
    if (!g || !g->map.p || len == 0 || len >= TTS_WORD_MAX) return 0;

    /* Hypotheses per letter position, flat: pos * BEAM + i. */
    size_t slots = (len + 1) * BEAM;
    hyp *h = malloc(slots * sizeof *h);
    uint32_t *nh = calloc(len + 1, sizeof *nh);
    if (!h || !nh) { free(h); free(nh); return 0; }
    uint32_t hmax = g->order - 1;

    h[0].hn = 1; h[0].hist[0] = 0; h[0].cost = 0; h[0].prev = -1; h[0].token = 0;
    nh[0] = 1;
    for (size_t pos = 0; pos < len; pos++) {
        for (uint32_t i = 0; i < nh[pos]; i++) {
            const hyp *src = &h[pos * BEAM + i];
            uint8_t c = (uint8_t)word[pos];
            for (uint32_t t = g->by_first_start[c]; t < g->by_first_start[c + 1]; t++) {
                uint16_t id = g->by_first[t];
                const char *gr = tok_graph(g, id);
                size_t gl = strlen(gr);
                if (pos + gl > len || memcmp(word + pos, gr, gl) != 0) continue;
                double cost = src->cost - logprob(g, src->hist, src->hn, id);
                size_t np = pos + gl;
                hyp cand;
                if (src->hn < hmax) { memcpy(cand.hist, src->hist, src->hn * sizeof *cand.hist); cand.hist[src->hn] = id; cand.hn = (uint8_t)(src->hn + 1); }
                else { memcpy(cand.hist, src->hist + 1, (hmax - 1) * sizeof *cand.hist); cand.hist[hmax - 1] = id; cand.hn = (uint8_t)hmax; }
                cand.cost = cost; cand.prev = (int32_t)(pos * BEAM + i); cand.token = id;
                /* Same history at the same position: keep the cheaper. */
                hyp *row = &h[np * BEAM];
                int32_t same = -1, worst = -1;
                for (uint32_t j = 0; j < nh[np]; j++) {
                    if (row[j].hn == cand.hn && !memcmp(row[j].hist, cand.hist, cand.hn * sizeof *cand.hist)) { same = (int32_t)j; break; }
                    if (worst < 0 || row[j].cost > row[worst].cost) worst = (int32_t)j;
                }
                if (same >= 0) { if (cost < row[same].cost) row[same] = cand; }
                else if (nh[np] < BEAM) row[nh[np]++] = cand;
                else if (cost < row[worst].cost) row[worst] = cand;
            }
        }
    }

    /* Close with </s> and walk back from the best. */
    int32_t best = -1;
    double best_cost = DBL_MAX;
    for (uint32_t j = 0; j < nh[len]; j++) {
        const hyp *e = &h[len * BEAM + j];
        double cost = e->cost - logprob(g, e->hist, e->hn, 1);
        if (cost < best_cost) { best_cost = cost; best = (int32_t)(len * BEAM + j); }
    }
    int ok = 0;
    if (best >= 0) {
        uint16_t path[TTS_WORD_MAX];
        int np = 0;
        for (int32_t k = best; k >= 0 && h[k].prev >= 0 && np < TTS_WORD_MAX; k = h[k].prev) path[np++] = h[k].token;
        size_t n = 0;
        ok = 1;
        for (int k = np - 1; k >= 0 && ok; k--) {
            const char *ph = tok_phon(g, path[k]);
            size_t pl = strlen(ph);
            if (n + pl + 1 > cap) ok = 0;
            else { memcpy(ipa + n, ph, pl); n += pl; }
        }
        ipa[ok ? n : 0] = '\0';
        if (ok && n == 0) ok = 0;
    }
    free(h); free(nh);
    return ok;
}
