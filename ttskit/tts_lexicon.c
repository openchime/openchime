/* The pronunciation dictionary: packing lexicon.ipa into lexicon.bin, and looking
 * words up in the mapped file (tts_priv.h describes the format). */
#define _POSIX_C_SOURCE 200809L
#include "tts_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void wr32(FILE *f, uint32_t v) {
    uint8_t b[4] = { (uint8_t)v, (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24) };
    fwrite(b, 1, 4, f);
}

typedef struct { char *word, *ipa; size_t seq; } entry;

static int cmp_entry(const void *a, const void *b) {
    const entry *x = a, *y = b;
    int c = strcmp(x->word, y->word);
    return c ? c : (x->seq < y->seq ? -1 : x->seq > y->seq);
}

int tts_pack_lexicon(const char *ipa_path, const char *bin_path, char *err, size_t errcap) {
    FILE *in = fopen(ipa_path, "r");
    if (!in) { tts_seterr(err, errcap, "cannot open %s", ipa_path); return -1; }
    entry *e = NULL;
    size_t n = 0, cap = 0;
    char line[1024];
    int rc = -1;
    while (fgets(line, sizeof line, in)) {
        char *nl = strpbrk(line, "\r\n");
        if (nl) *nl = '\0';
        if (!line[0] || line[0] == '#') continue;
        char *tab = strchr(line, '\t');
        if (!tab || tab == line || !tab[1]) { tts_seterr(err, errcap, "malformed line: %s", line); goto out; }
        *tab = '\0';
        if (strlen(line) >= TTS_WORD_MAX || strlen(tab + 1) >= TTS_IPA_MAX) continue;
        for (char *c = line; *c; c++) if (*c >= 'A' && *c <= 'Z') *c = (char)(*c - 'A' + 'a');
        if (n == cap) {
            cap = cap ? cap * 2 : 65536;
            entry *ne = realloc(e, cap * sizeof *ne);
            if (!ne) { tts_seterr(err, errcap, "out of memory"); goto out; }
            e = ne;
        }
        e[n].word = strdup(line);
        e[n].ipa = strdup(tab + 1);
        e[n].seq = n;
        if (!e[n].word || !e[n].ipa) { tts_seterr(err, errcap, "out of memory"); goto out; }
        n++;
    }
    qsort(e, n, sizeof *e, cmp_entry);
    /* The first pronunciation of a word in the input wins; the sort breaks ties
     * by input order, so later duplicates are the ones dropped. */
    size_t m = 0;
    for (size_t i = 0; i < n; i++) {
        if (m && strcmp(e[m - 1].word, e[i].word) == 0) { free(e[i].word); free(e[i].ipa); continue; }
        e[m++] = e[i];
    }
    n = m;

    FILE *out = fopen(bin_path, "wb");
    if (!out) { tts_seterr(err, errcap, "cannot create %s", bin_path); goto out; }
    uint32_t pool = 0;
    for (size_t i = 0; i < n; i++) pool += (uint32_t)(strlen(e[i].word) + 1 + strlen(e[i].ipa) + 1);
    fwrite(TTS_LEX_MAGIC, 1, 8, out);
    wr32(out, TTS_LEX_VERSION); wr32(out, (uint32_t)n); wr32(out, pool);
    wr32(out, 0); wr32(out, 0); wr32(out, 0);
    uint32_t off = 0;
    for (size_t i = 0; i < n; i++) {
        wr32(out, off);
        wr32(out, off + (uint32_t)strlen(e[i].word) + 1);
        off += (uint32_t)(strlen(e[i].word) + 1 + strlen(e[i].ipa) + 1);
    }
    for (size_t i = 0; i < n; i++) {
        fwrite(e[i].word, 1, strlen(e[i].word) + 1, out);
        fwrite(e[i].ipa, 1, strlen(e[i].ipa) + 1, out);
    }
    rc = ferror(out) ? -1 : 0;
    if (fclose(out) != 0) rc = -1;
    if (rc) tts_seterr(err, errcap, "write failed: %s", bin_path);
out:
    for (size_t i = 0; i < n; i++) { free(e[i].word); free(e[i].ipa); }
    free(e);
    fclose(in);
    return rc;
}

int tts_lexicon_open(const char *path, const tts_map *mem, tts_lexicon *lx, char *err, size_t errcap) {
    memset(lx, 0, sizeof *lx);
    if (mem ? (lx->map = *mem, lx->map.owned = 0, !mem->p) : tts_map_open(path, &lx->map) != 0) { tts_seterr(err, errcap, "cannot map %s", path); return -1; }
    const uint8_t *p = lx->map.p;
    size_t n = lx->map.n;
    if (n < 32 || memcmp(p, TTS_LEX_MAGIC, 8) != 0 || tts_rd32(p + 8) != TTS_LEX_VERSION) {
        tts_seterr(err, errcap, "%s is not a version %u ttskit lexicon", path, TTS_LEX_VERSION);
        tts_map_close(&lx->map);
        return -1;
    }
    uint32_t count = tts_rd32(p + 12), pool = tts_rd32(p + 16);
    if ((n - 32) / 8 < count || n - 32 - (size_t)count * 8 != pool || pool == 0 || p[n - 1] != '\0') {
        tts_seterr(err, errcap, "%s is truncated or corrupt", path);
        tts_map_close(&lx->map);
        return -1;
    }
    lx->count = count;
    lx->index = p + 32;
    lx->pool = (const char *)p + 32 + (size_t)count * 8;
    lx->pool_size = pool;
    return 0;
}

void tts_lexicon_close(tts_lexicon *lx) {
    tts_map_close(&lx->map);
    memset(lx, 0, sizeof *lx);
}

const char *tts_lexicon_find(const tts_lexicon *lx, const char *word) {
    if (!lx->count) return NULL;
    size_t lo = 0, hi = lx->count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uint32_t wo = tts_rd32(lx->index + mid * 8);
        if (wo >= lx->pool_size) return NULL;
        int c = strcmp(word, lx->pool + wo);
        if (c == 0) {
            uint32_t io = tts_rd32(lx->index + mid * 8 + 4);
            return io < lx->pool_size ? lx->pool + io : NULL;
        }
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}
