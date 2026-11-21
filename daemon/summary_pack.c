/* summary.bin from Qwen's published files (summary_pack.h). */
#define _POSIX_C_SOURCE 200809L
#include "summary_pack.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <mbedtls/sha256.h>

#include "json.h"
#include "nnkit.h"
#include "sumkit.h"

const char *const OC_SUMMARY_INPUTS[SP_FILES] = { "config.json", "tokenizer.json", "model.safetensors" };

/* Qwen's pre-tokenizer: the one split its tokenizer.json must name, which
 * sumkit's tokenizer implements (sumkit/lm_tokens.c). */
static const char PRETOKENIZE[] =
    "(?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\\r\\n\\p{L}\\p{N}]?[\\p{L}\\p{M}]+|\\p{N}| ?[^\\s\\p{L}\\p{M}\\p{N}]+[\\r\\n]*|"
    "\\s*[\\r\\n]+|\\s+(?!\\S)|\\s+";

static void seterr(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* ---- files -------------------------------------------------------------------- */

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = NULL;
    size_t cap = 0, n = 0;
    for (;;) {
        if (n + 65536 + 1 > cap) {
            cap = (n + 65536 + 1) * 2;
            char *nb = realloc(buf, cap);
            if (!nb) { free(buf); fclose(f); return NULL; }
            buf = nb;
        }
        size_t r = fread(buf + n, 1, cap - n - 1, f);
        n += r;
        if (r == 0) break;
    }
    int bad = ferror(f);
    fclose(f);
    if (bad) { free(buf); return NULL; }
    buf[n] = '\0';
    *len = n;
    return buf;
}

static int sha_file(const char *path, uint8_t out[32]) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    mbedtls_sha256_context c;
    mbedtls_sha256_init(&c);
    mbedtls_sha256_starts(&c, 0);
    static unsigned char buf[1 << 20];
    ssize_t r;
    while ((r = read(fd, buf, sizeof buf)) > 0) mbedtls_sha256_update(&c, buf, (size_t)r);
    close(fd);
    mbedtls_sha256_finish(&c, out);
    mbedtls_sha256_free(&c);
    return r < 0 ? -1 : 0;
}

static int pread_all(int fd, void *buf, size_t n, uint64_t off) {
    uint8_t *p = buf;
    while (n) {
        ssize_t r = pread(fd, p, n, (off_t)off);
        if (r <= 0) return -1;
        p += r;
        n -= (size_t)r;
        off += (uint64_t)r;
    }
    return 0;
}

/* ---- JSON ------------------------------------------------------------------------ */

/* The whole document tokenized, however large (tokenizer.json is). */
static int parse_json(oc_json *d, const char *js, size_t len) {
    jsmn_parser p;
    jsmn_init(&p);
    int n = jsmn_parse(&p, js, len, NULL, 0);
    if (n <= 0) return -1;
    d->js = js;
    d->t = malloc((size_t)n * sizeof *d->t);
    if (!d->t) return -1;
    jsmn_init(&p);
    d->n = jsmn_parse(&p, js, len, d->t, (unsigned)n);
    if (d->n != n) { oc_json_free(d); return -1; }
    return 0;
}

static int tok_is(const oc_json *d, int i, const char *s) {
    size_t n = strlen(s);
    return i >= 0 && d->t[i].type == JSMN_STRING && (size_t)(d->t[i].end - d->t[i].start) == n &&
           !memcmp(d->js + d->t[i].start, s, n);
}

static int get_num(const oc_json *d, int obj, const char *key, double *out) {
    int i = oc_json_get(d, obj, key);
    if (i < 0 || d->t[i].type != JSMN_PRIMITIVE) return -1;
    char buf[64];
    int n = d->t[i].end - d->t[i].start;
    if (n <= 0 || n >= (int)sizeof buf) return -1;
    memcpy(buf, d->js + d->t[i].start, (size_t)n);
    buf[n] = '\0';
    if (buf[0] != '-' && (buf[0] < '0' || buf[0] > '9')) return -1;
    char *end;
    *out = strtod(buf, &end);
    return *end ? -1 : 0;
}

static int get_u32(const oc_json *d, int obj, const char *key, uint32_t *out) {
    double v;
    if (get_num(d, obj, key, &v) != 0 || v < 1 || v > 1e9 || v != floor(v)) return -1;
    *out = (uint32_t)v;
    return 0;
}

/* ---- the configuration ------------------------------------------------------------- */

static int read_config(const char *js, size_t len, sk_config *c, char *err, size_t cap) {
    oc_json d;
    if (parse_json(&d, js, len) != 0 || d.t[0].type != JSMN_OBJECT) {
        seterr(err, cap, "config.json is not a JSON object");
        return -1;
    }
    int rc = -1, tc = oc_json_get(&d, 0, "text_config");
    if (tc < 0) tc = 0;
    memset(c, 0, sizeof *c);
    double eps, theta, frac = 0;
    int rp = oc_json_get(&d, tc, "rope_parameters"), lt = oc_json_get(&d, tc, "layer_types");
    int tie = oc_json_get(&d, tc, "tie_word_embeddings"), gate = oc_json_get(&d, tc, "attn_output_gate");
    if (tie < 0) tie = oc_json_get(&d, 0, "tie_word_embeddings");
    if (!tok_is(&d, oc_json_get(&d, tc, "model_type"), "qwen3_5_text")) {
        seterr(err, cap, "config.json is not a Qwen3.5 text model's (model_type)");
        goto done;
    }
    if (get_u32(&d, tc, "num_hidden_layers", &c->n_layer) || get_u32(&d, tc, "hidden_size", &c->n_embd) ||
        get_u32(&d, tc, "intermediate_size", &c->n_ff) || get_u32(&d, tc, "vocab_size", &c->n_vocab) ||
        get_u32(&d, tc, "num_attention_heads", &c->n_head) || get_u32(&d, tc, "num_key_value_heads", &c->n_head_kv) ||
        get_u32(&d, tc, "head_dim", &c->head_dim) || get_u32(&d, tc, "linear_num_key_heads", &c->lin_k_heads) ||
        get_u32(&d, tc, "linear_num_value_heads", &c->lin_v_heads) ||
        get_u32(&d, tc, "linear_key_head_dim", &c->lin_k_dim) ||
        get_u32(&d, tc, "linear_value_head_dim", &c->lin_v_dim) ||
        get_u32(&d, tc, "linear_conv_kernel_dim", &c->conv_kernel) || get_num(&d, tc, "rms_norm_eps", &eps) ||
        rp < 0 || get_num(&d, rp, "rope_theta", &theta) || get_num(&d, rp, "partial_rotary_factor", &frac)) {
        seterr(err, cap, "config.json lacks a dimension the model needs, or has one that is not a number");
        goto done;
    }
    if (tie < 0 || !oc_json_true(&d, tie)) { seterr(err, cap, "config.json: the output layer is not tied to the embeddings"); goto done; }
    if (gate < 0 || !oc_json_true(&d, gate)) { seterr(err, cap, "config.json: the attention has no output gate"); goto done; }
    if (c->n_layer > SK_MAX_LAYERS || lt < 0 || d.t[lt].type != JSMN_ARRAY || (uint32_t)d.t[lt].size != c->n_layer) {
        seterr(err, cap, "config.json: layer_types does not name every layer");
        goto done;
    }
    for (uint32_t l = 0, i = (uint32_t)lt + 1; l < c->n_layer; l++, i++) {
        if (tok_is(&d, (int)i, "linear_attention")) c->linear |= 1ull << l;
        else if (!tok_is(&d, (int)i, "full_attention")) { seterr(err, cap, "config.json: layer %u is of an unknown type", l); goto done; }
    }
    c->rms_eps = (float)eps;
    c->rope_theta = (float)theta;
    c->rot_dims = (uint32_t)lrint(frac * c->head_dim);
    if (!(eps > 0) || !(theta > 0) || !(frac > 0) || fabs(frac * c->head_dim - c->rot_dims) > 1e-6 || c->rot_dims % 2 ||
        c->head_dim % 32 || c->n_head % c->n_head_kv || c->lin_v_heads % c->lin_k_heads || c->conv_kernel > 16) {
        seterr(err, cap, "config.json: dimensions the engine cannot run");
        goto done;
    }
    rc = 0;
done:
    oc_json_free(&d);
    return rc;
}

/* ---- the tokenizer ------------------------------------------------------------------ */

/* GPT-2's byte-level alphabet: a token's text, one character a byte, back to
 * its bytes. -1 for a character outside the alphabet. */
static int byte_of(uint32_t cp) {
    static int16_t from[324];
    static int ready;
    if (!ready) {
        for (int i = 0; i < 324; i++) from[i] = -1;
        int n = 0;
        for (int b = 0; b < 256; b++) {
            int printable = (b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255);
            from[printable ? b : 256 + n++] = (int16_t)b;
        }
        ready = 1;
    }
    return cp < 324 ? from[cp] : -1;
}

static long utf8_next(const unsigned char *s, size_t n, size_t *i, uint32_t *cp) {
    unsigned char c = s[*i];
    int len = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
    if (!len || *i + (size_t)len > n) return -1;
    uint32_t v = len == 1 ? c : len == 2 ? c & 0x1f : len == 3 ? c & 0x0f : c & 0x07;
    for (int k = 1; k < len; k++) {
        if ((s[*i + (size_t)k] & 0xc0) != 0x80) return -1;
        v = v << 6 | (s[*i + (size_t)k] & 0x3f);
    }
    *i += (size_t)len;
    *cp = v;
    return len;
}

/* A string to id table, open addressing over FNV-1a. */
typedef struct { const char *s; uint32_t len, id; } slot;
typedef struct { slot *v; size_t cap; } strmap;

static uint64_t fnv(const char *s, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) h = (h ^ (unsigned char)s[i]) * 1099511628211ull;
    return h;
}
static int map_put(strmap *m, const char *s, uint32_t len, uint32_t id) {
    size_t i = fnv(s, len) & (m->cap - 1);
    while (m->v[i].s) {
        if (m->v[i].len == len && !memcmp(m->v[i].s, s, len)) return -1;   /* a repeat */
        i = (i + 1) & (m->cap - 1);
    }
    m->v[i] = (slot){ s, len, id };
    return 0;
}
static long map_get(const strmap *m, const char *s, size_t len) {
    size_t i = fnv(s, len) & (m->cap - 1);
    while (m->v[i].s) {
        if (m->v[i].len == len && !memcmp(m->v[i].s, s, len)) return m->v[i].id;
        i = (i + 1) & (m->cap - 1);
    }
    return -1;
}

typedef struct {
    uint8_t *bytes;   size_t nbytes;
    int32_t *offs;    uint32_t n_tok;
    int32_t *merges;  uint32_t n_merges;
    int32_t *added;   uint32_t n_added;
    uint32_t eog[SK_MAX_EOG], n_eog;
} toktab;

static void toktab_free(toktab *t) { free(t->bytes); free(t->offs); free(t->merges); free(t->added); }

static int read_tokenizer(const char *js, size_t len, toktab *tt, char *err, size_t cap) {
    oc_json d;
    memset(tt, 0, sizeof *tt);
    if (parse_json(&d, js, len) != 0 || d.t[0].type != JSMN_OBJECT) { seterr(err, cap, "tokenizer.json is not a JSON object"); return -1; }
    int rc = -1;
    uint32_t n_tok = 0;
    char **text = NULL;          /* each id's bytes, unescaped and decoded */
    uint32_t *tlen = NULL;
    char *keys = NULL;           /* the vocabulary's strings as written, for the merges */
    strmap m = { 0 };
    int nm = oc_json_get(&d, 0, "normalizer"), pre = oc_json_get(&d, 0, "pre_tokenizer"), mo = oc_json_get(&d, 0, "model");
    if (!tok_is(&d, oc_json_get(&d, nm, "type"), "NFC")) { seterr(err, cap, "tokenizer.json: not NFC-normalised"); goto done; }
    int seq = oc_json_get(&d, pre, "pretokenizers");
    if (!tok_is(&d, oc_json_get(&d, pre, "type"), "Sequence") || seq < 0 || d.t[seq].type != JSMN_ARRAY || d.t[seq].size != 2) {
        seterr(err, cap, "tokenizer.json: not the pre-tokenizer sumkit implements");
        goto done;
    }
    int split = seq + 1, bl = oc_json_skip(&d, split);
    int pat = oc_json_get(&d, oc_json_get(&d, split, "pattern"), "Regex");
    static char pbuf[512];
    if (!tok_is(&d, oc_json_get(&d, split, "type"), "Split") || pat < 0 || oc_json_str(&d, pat, pbuf, sizeof pbuf) ||
        strcmp(pbuf, PRETOKENIZE) || !tok_is(&d, oc_json_get(&d, split, "behavior"), "Isolated") ||
        !tok_is(&d, oc_json_get(&d, bl, "type"), "ByteLevel") || oc_json_true(&d, oc_json_get(&d, bl, "use_regex")) ||
        oc_json_true(&d, oc_json_get(&d, bl, "add_prefix_space"))) {
        seterr(err, cap, "tokenizer.json: not the pre-tokenizer sumkit implements");
        goto done;
    }
    if (!tok_is(&d, oc_json_get(&d, mo, "type"), "BPE") || oc_json_true(&d, oc_json_get(&d, mo, "byte_fallback")) ||
        oc_json_true(&d, oc_json_get(&d, mo, "ignore_merges"))) {
        seterr(err, cap, "tokenizer.json: not a byte-level BPE model sumkit implements");
        goto done;
    }
    int vo = oc_json_get(&d, mo, "vocab"), me = oc_json_get(&d, mo, "merges"), ad = oc_json_get(&d, 0, "added_tokens");
    if (vo < 0 || d.t[vo].type != JSMN_OBJECT || me < 0 || d.t[me].type != JSMN_ARRAY || ad < 0 || d.t[ad].type != JSMN_ARRAY) {
        seterr(err, cap, "tokenizer.json: no vocabulary, merges or added tokens");
        goto done;
    }
    /* Ids: the vocabulary's and the added tokens', the largest naming n_tok. */
    for (int k = 0, i = vo + 1; k < d.t[vo].size; k++, i = oc_json_skip(&d, i + 1)) {
        double v;
        char nb[32];
        int vi = i + 1, l = d.t[vi].end - d.t[vi].start;
        if (d.t[vi].type != JSMN_PRIMITIVE || l <= 0 || l >= 31) { seterr(err, cap, "tokenizer.json: a vocabulary id that is not a number"); goto done; }
        memcpy(nb, d.js + d.t[vi].start, (size_t)l); nb[l] = 0;
        v = strtod(nb, NULL);
        if (v < 0 || v > 1e7) { seterr(err, cap, "tokenizer.json: a vocabulary id out of range"); goto done; }
        if ((uint32_t)v + 1 > n_tok) n_tok = (uint32_t)v + 1;
    }
    for (int k = 0, i = ad + 1; k < d.t[ad].size; k++, i = oc_json_skip(&d, i)) {
        double v;
        if (get_num(&d, i, "id", &v) || v < 0 || v > 1e7) { seterr(err, cap, "tokenizer.json: an added token without an id"); goto done; }
        if ((uint32_t)v + 1 > n_tok) n_tok = (uint32_t)v + 1;
    }
    text = calloc(n_tok, sizeof *text);
    tlen = calloc(n_tok, sizeof *tlen);
    keys = malloc(len);
    m.cap = 1;
    while (m.cap < 2 * (size_t)d.t[vo].size + 16) m.cap <<= 1;
    m.v = calloc(m.cap, sizeof *m.v);
    if (!text || !tlen || !keys || !m.v) { seterr(err, cap, "out of memory"); goto done; }
    size_t kat = 0;
    for (int k = 0, i = vo + 1; k < d.t[vo].size; k++, i = oc_json_skip(&d, i + 1)) {
        long kl = oc_json_unescape(d.js + d.t[i].start, (size_t)(d.t[i].end - d.t[i].start), keys + kat, len - kat);
        uint32_t id = (uint32_t)strtod(d.js + d.t[i + 1].start, NULL);
        if (kl <= 0 || text[id]) { seterr(err, cap, "tokenizer.json: a vocabulary entry that is empty, unreadable or repeated"); goto done; }
        if (map_put(&m, keys + kat, (uint32_t)kl, id)) { seterr(err, cap, "tokenizer.json: a vocabulary string twice"); goto done; }
        /* its bytes: one a character of the byte-level alphabet */
        char *b = malloc((size_t)kl);
        if (!b) { seterr(err, cap, "out of memory"); goto done; }
        size_t at = 0, bn = 0;
        while (at < (size_t)kl) {
            uint32_t cp;
            int by;
            if (utf8_next((const unsigned char *)keys + kat, (size_t)kl, &at, &cp) < 0 || (by = byte_of(cp)) < 0) {
                free(b);
                seterr(err, cap, "tokenizer.json: a vocabulary string outside the byte-level alphabet");
                goto done;
            }
            b[bn++] = (char)by;
        }
        text[id] = b;
        tlen[id] = (uint32_t)bn;
        kat += (size_t)kl;
    }
    tt->added = malloc((size_t)d.t[ad].size * sizeof *tt->added + 1);
    if (!tt->added) { seterr(err, cap, "out of memory"); goto done; }
    for (int k = 0, i = ad + 1; k < d.t[ad].size; k++, i = oc_json_skip(&d, i)) {
        double v;
        get_num(&d, i, "id", &v);
        uint32_t id = (uint32_t)v;
        int ci = oc_json_get(&d, i, "content");
        char *b = malloc(256);
        if (!b || ci < 0 || oc_json_str(&d, ci, b, 256) != 0 || !b[0]) {
            free(b);
            seterr(err, cap, "tokenizer.json: an added token whose content cannot be read");
            goto done;
        }
        if (text[id]) {   /* an added token the vocabulary also has must be the same text */
            int same = tlen[id] == strlen(b) && !memcmp(text[id], b, tlen[id]);
            free(b);
            if (!same) { seterr(err, cap, "tokenizer.json: added token %u differs from the vocabulary's", id); goto done; }
        } else {
            text[id] = b;
            tlen[id] = (uint32_t)strlen(b);
        }
        tt->added[tt->n_added++] = (int32_t)id;
        if ((!strcmp(text[id], "<|im_end|>") || !strcmp(text[id], "<|endoftext|>")) && tt->n_eog < SK_MAX_EOG)
            tt->eog[tt->n_eog++] = id;
    }
    if (!tt->n_eog) { seterr(err, cap, "tokenizer.json: no end-of-answer token (<|im_end|>)"); goto done; }
    /* The merges, by rank: "left right" (or [left, right]) to the three ids. */
    tt->n_merges = (uint32_t)d.t[me].size;
    tt->merges = malloc((size_t)tt->n_merges * 3 * sizeof *tt->merges + 1);
    if (!tt->merges) { seterr(err, cap, "out of memory"); goto done; }
    char *mb = malloc(4096);
    if (!mb) { seterr(err, cap, "out of memory"); goto done; }
    for (uint32_t k = 0, i = (uint32_t)me + 1; k < tt->n_merges; k++, i = (uint32_t)oc_json_skip(&d, (int)i)) {
        long l1, l2;
        if (d.t[i].type == JSMN_STRING) {
            long l = oc_json_unescape(d.js + d.t[i].start, (size_t)(d.t[i].end - d.t[i].start), mb, 4096);
            char *sp = l > 0 ? memchr(mb, ' ', (size_t)l) : NULL;
            if (!sp) { free(mb); seterr(err, cap, "tokenizer.json: merge %u is not two tokens", k); goto done; }
            l1 = sp - mb;
            l2 = l - l1 - 1;
            memmove(sp, sp + 1, (size_t)l2);
        } else if (d.t[i].type == JSMN_ARRAY && d.t[i].size == 2) {
            l1 = oc_json_unescape(d.js + d.t[i + 1].start, (size_t)(d.t[i + 1].end - d.t[i + 1].start), mb, 2048);
            l2 = oc_json_unescape(d.js + d.t[i + 2].start, (size_t)(d.t[i + 2].end - d.t[i + 2].start), mb + (l1 > 0 ? l1 : 0), 2048);
        } else l1 = l2 = -1;
        long a = l1 > 0 ? map_get(&m, mb, (size_t)l1) : -1, b = l2 > 0 ? map_get(&m, mb + l1, (size_t)l2) : -1;
        long r = a >= 0 && b >= 0 ? map_get(&m, mb, (size_t)(l1 + l2)) : -1;
        if (r < 0) { free(mb); seterr(err, cap, "tokenizer.json: merge %u names a token the vocabulary does not have", k); goto done; }
        tt->merges[3 * k] = (int32_t)a;
        tt->merges[3 * k + 1] = (int32_t)b;
        tt->merges[3 * k + 2] = (int32_t)r;
    }
    free(mb);
    /* The bytes end to end, and where each token's start. */
    tt->n_tok = n_tok;
    tt->offs = malloc(((size_t)n_tok + 1) * sizeof *tt->offs);
    size_t total = 0;
    for (uint32_t i = 0; i < n_tok; i++) total += tlen[i];
    tt->bytes = malloc(total + 1);
    if (!tt->offs || !tt->bytes || total > 0x7fffffff) { seterr(err, cap, "out of memory"); goto done; }
    for (uint32_t i = 0; i < n_tok; i++) {
        tt->offs[i] = (int32_t)tt->nbytes;
        if (tlen[i]) memcpy(tt->bytes + tt->nbytes, text[i], tlen[i]);
        tt->nbytes += tlen[i];
    }
    tt->offs[n_tok] = (int32_t)tt->nbytes;
    rc = 0;
done:
    if (text) for (uint32_t i = 0; i < n_tok; i++) free(text[i]);
    free(text);
    free(tlen);
    free(keys);
    free(m.v);
    oc_json_free(&d);
    if (rc) toktab_free(tt);
    return rc;
}

/* ---- the weights: safetensors ------------------------------------------------------- */

typedef struct {
    char     name[128];
    int      dtype;      /* 0 F32, 1 BF16, 2 F16 */
    uint32_t ndim, dims[8];  /* the skipped tensors may have more than 4 */
    uint64_t off, bytes; /* from the file's start */
    int      used;
} stensor;

typedef struct { stensor *v; size_t n; uint64_t size; } stfile;

static int read_safetensors(int fd, uint64_t fsize, stfile *sf, char *err, size_t cap) {
    uint8_t h8[8];
    memset(sf, 0, sizeof *sf);
    if (fsize < 8 || pread_all(fd, h8, 8, 0)) { seterr(err, cap, "model.safetensors is too short"); return -1; }
    uint64_t hl = 0;
    for (int i = 7; i >= 0; i--) hl = hl << 8 | h8[i];
    if (hl < 2 || hl > (64u << 20) || 8 + hl > fsize) { seterr(err, cap, "model.safetensors has no header"); return -1; }
    char *js = malloc(hl + 1);
    if (!js || pread_all(fd, js, hl, 8)) { free(js); seterr(err, cap, "model.safetensors cannot be read"); return -1; }
    js[hl] = 0;
    oc_json d;
    if (parse_json(&d, js, hl) || d.t[0].type != JSMN_OBJECT) {
        free(js);
        seterr(err, cap, "model.safetensors' header is not a JSON object");
        return -1;
    }
    int rc = -1;
    sf->v = calloc((size_t)d.t[0].size + 1, sizeof *sf->v);
    if (!sf->v) { seterr(err, cap, "out of memory"); goto done; }
    sf->size = fsize;
    uint64_t base = 8 + hl;
    for (int k = 0, i = 1; k < d.t[0].size; k++, i = oc_json_skip(&d, i + 1)) {
        stensor *t = &sf->v[sf->n];
        if (oc_json_str(&d, i, t->name, sizeof t->name)) { seterr(err, cap, "model.safetensors: a tensor name that cannot be read"); goto done; }
        if (!strcmp(t->name, "__metadata__")) continue;
        int o = i + 1, dt = oc_json_get(&d, o, "dtype"), sh = oc_json_get(&d, o, "shape"), of = oc_json_get(&d, o, "data_offsets");
        t->dtype = tok_is(&d, dt, "F32") ? 0 : tok_is(&d, dt, "BF16") ? 1 : tok_is(&d, dt, "F16") ? 2 : -1;
        if (t->dtype < 0 || sh < 0 || d.t[sh].type != JSMN_ARRAY || d.t[sh].size > 8 || of < 0 ||
            d.t[of].type != JSMN_ARRAY || d.t[of].size != 2) {
            seterr(err, cap, "model.safetensors: %s is of a type or shape the converter does not read", t->name);
            goto done;
        }
        uint64_t count = 1;
        t->ndim = (uint32_t)d.t[sh].size;
        for (uint32_t j = 0; j < t->ndim; j++) {
            double v = strtod(d.js + d.t[sh + 1 + (int)j].start, NULL);
            if (v < 1 || v > 1e9) { seterr(err, cap, "model.safetensors: %s has an impossible shape", t->name); goto done; }
            t->dims[j] = (uint32_t)v;
            count *= t->dims[j];
        }
        double a = strtod(d.js + d.t[of + 1].start, NULL), b = strtod(d.js + d.t[of + 2].start, NULL);
        uint64_t es = t->dtype == 0 ? 4 : 2;
        if (a < 0 || b < a || (uint64_t)(b - a) != count * es || base + (uint64_t)b > fsize) {
            seterr(err, cap, "model.safetensors: %s's data is not where its header says", t->name);
            goto done;
        }
        t->off = base + (uint64_t)a;
        t->bytes = (uint64_t)(b - a);
        sf->n++;
    }
    rc = 0;
done:
    oc_json_free(&d);
    free(js);
    if (rc) { free(sf->v); sf->v = NULL; }
    return rc;
}

static stensor *st_find(stfile *sf, const char *name) {
    for (size_t i = 0; i < sf->n; i++)
        if (!strcmp(sf->v[i].name, name)) return &sf->v[i];
    return NULL;
}

/* ---- ggml's reference quantisers (no importance matrix) -------------------------------- */

#define GROUP_MAX_EPS 1e-15f
static inline int nearest_int(float f) {
    float v = f + 12582912.0f;
    int32_t i;
    memcpy(&i, &v, sizeof i);
    return (i & 0x007fffff) - 0x00400000;
}
static inline int imax(int a, int b) { return a > b ? a : b; }
static inline int imin(int a, int b) { return a < b ? a : b; }

static float make_qkx2_quants(int n, int nmax, const float *x, const float *weights, uint8_t *L, float *the_min,
                              uint8_t *Laux, float rmin, float rdelta, int nstep) {
    float min = x[0], max = x[0], sum_w = weights[0], sum_x = sum_w * x[0];
    for (int i = 1; i < n; ++i) {
        if (x[i] < min) min = x[i];
        if (x[i] > max) max = x[i];
        float w = weights[i];
        sum_w += w;
        sum_x += w * x[i];
    }
    if (min > 0) min = 0;
    if (max == min) {
        for (int i = 0; i < n; ++i) L[i] = 0;
        *the_min = -min;
        return 0.f;
    }
    float iscale = nmax / (max - min), scale = 1 / iscale, best_error = 0;
    for (int i = 0; i < n; ++i) {
        int l = nearest_int(iscale * (x[i] - min));
        L[i] = (uint8_t)imax(0, imin(nmax, l));
        float diff = scale * L[i] + min - x[i];
        best_error += weights[i] * diff * diff;
    }
    for (int is = 0; is <= nstep; ++is) {
        iscale = (rmin + rdelta * is + nmax) / (max - min);
        float sum_l = 0, sum_l2 = 0, sum_xl = 0;
        for (int i = 0; i < n; ++i) {
            int l = imax(0, imin(nmax, nearest_int(iscale * (x[i] - min))));
            Laux[i] = (uint8_t)l;
            float w = weights[i];
            sum_l += w * l;
            sum_l2 += w * l * l;
            sum_xl += w * l * x[i];
        }
        float D = sum_w * sum_l2 - sum_l * sum_l;
        if (D > 0) {
            float this_scale = (sum_w * sum_xl - sum_x * sum_l) / D, this_min = (sum_l2 * sum_x - sum_l * sum_xl) / D;
            if (this_min > 0) {
                this_min = 0;
                this_scale = sum_xl / sum_l2;
            }
            float cur_error = 0;
            for (int i = 0; i < n; ++i) {
                float diff = this_scale * Laux[i] + this_min - x[i];
                cur_error += weights[i] * diff * diff;
            }
            if (cur_error < best_error) {
                for (int i = 0; i < n; ++i) L[i] = Laux[i];
                best_error = cur_error;
                scale = this_scale;
                min = this_min;
            }
        }
    }
    *the_min = -min;
    return scale;
}

static float make_qx_quants(int n, int nmax, const float *x, int8_t *L) {
    float max = 0, amax = 0;
    for (int i = 0; i < n; ++i) {
        float ax = fabsf(x[i]);
        if (ax > amax) { amax = ax; max = x[i]; }
    }
    if (amax < GROUP_MAX_EPS) {
        for (int i = 0; i < n; ++i) L[i] = 0;
        return 0.f;
    }
    float iscale = -nmax / max, sumlx = 0, suml2 = 0;
    for (int i = 0; i < n; ++i) {
        int l = imax(-nmax, imin(nmax - 1, nearest_int(iscale * x[i])));
        L[i] = (int8_t)(l + nmax);
        float w = x[i] * x[i];
        sumlx += w * x[i] * l;
        suml2 += w * l * l;
    }
    float scale = suml2 ? sumlx / suml2 : 0.0f, best = scale * sumlx;
    for (int is = -9; is <= 9; ++is) {
        if (is == 0) continue;
        iscale = -(nmax + 0.1f * is) / max;
        sumlx = suml2 = 0;
        for (int i = 0; i < n; ++i) {
            int l = imax(-nmax, imin(nmax - 1, nearest_int(iscale * x[i])));
            float w = x[i] * x[i];
            sumlx += w * x[i] * l;
            suml2 += w * l * l;
        }
        if (suml2 > 0 && sumlx * sumlx > best * suml2) {
            for (int i = 0; i < n; ++i) L[i] = (int8_t)(nmax + imax(-nmax, imin(nmax - 1, nearest_int(iscale * x[i]))));
            scale = sumlx / suml2;
            best = scale * sumlx;
        }
    }
    return scale;
}

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }

static void quantize_q4k(const float *x, uint8_t *y, int k) {
    uint8_t L[NN_QK], Laux[32];
    float weights[32], mins[8], scales[8];
    for (int b = 0; b < k / NN_QK; b++, x += NN_QK, y += NN_Q4K_BYTES) {
        float max_scale = 0, max_min = 0;
        for (int j = 0; j < 8; ++j) {
            float sum_x2 = 0;
            for (int l = 0; l < 32; ++l) sum_x2 += x[32 * j + l] * x[32 * j + l];
            float av_x = sqrtf(sum_x2 / 32);
            for (int l = 0; l < 32; ++l) weights[l] = av_x + fabsf(x[32 * j + l]);
            scales[j] = make_qkx2_quants(32, 15, x + 32 * j, weights, L + 32 * j, &mins[j], Laux, -1.f, 0.1f, 20);
            if (scales[j] > max_scale) max_scale = scales[j];
            if (mins[j] > max_min) max_min = mins[j];
        }
        uint8_t *sc = y + 4, *qs = y + 16;
        memset(sc, 0, 12);
        float inv_scale = max_scale > 0 ? 63.f / max_scale : 0.f, inv_min = max_min > 0 ? 63.f / max_min : 0.f;
        for (int j = 0; j < 8; ++j) {
            uint8_t ls = (uint8_t)imin(63, nearest_int(inv_scale * scales[j])), lm = (uint8_t)imin(63, nearest_int(inv_min * mins[j]));
            if (j < 4) {
                sc[j] = ls;
                sc[j + 4] = lm;
            } else {
                sc[j + 4] = (uint8_t)((ls & 0xF) | ((lm & 0xF) << 4));
                sc[j - 4] |= (uint8_t)((ls >> 4) << 6);
                sc[j] |= (uint8_t)((lm >> 4) << 6);
            }
        }
        uint16_t dh = nn_f32_to_f16(max_scale / 63.f), mh = nn_f32_to_f16(max_min / 63.f);
        put16(y, dh);
        put16(y + 2, mh);
        float d0 = nn_f16_to_f32(dh), m0 = nn_f16_to_f32(mh);
        for (int j = 0; j < 8; ++j) {
            int s, m;
            if (j < 4) { s = sc[j] & 63; m = sc[j + 4] & 63; }
            else { s = (sc[j + 4] & 0xF) | ((sc[j - 4] >> 6) << 4); m = (sc[j + 4] >> 4) | ((sc[j] >> 6) << 4); }
            float d = d0 * s;
            if (!d) continue;
            float dm = m0 * m;
            for (int ii = 0; ii < 32; ++ii) L[32 * j + ii] = (uint8_t)imax(0, imin(15, nearest_int((x[32 * j + ii] + dm) / d)));
        }
        for (int j = 0; j < NN_QK; j += 64)
            for (int l = 0; l < 32; ++l) qs[j / 2 + l] = (uint8_t)(L[j + l] | (L[j + l + 32] << 4));
    }
}

static void quantize_q6k(const float *x, uint8_t *y, int k) {
    int8_t L[NN_QK];
    float scales[16];
    for (int b = 0; b < k / NN_QK; b++, x += NN_QK, y += NN_Q6K_BYTES) {
        float max_scale = 0, max_abs_scale = 0;
        for (int ib = 0; ib < 16; ++ib) {
            float scale = make_qx_quants(16, 32, x + 16 * ib, L + 16 * ib);
            scales[ib] = scale;
            if (fabsf(scale) > max_abs_scale) { max_abs_scale = fabsf(scale); max_scale = scale; }
        }
        memset(y, 0, NN_Q6K_BYTES);
        if (max_abs_scale < GROUP_MAX_EPS) continue;
        float iscale = -128.f / max_scale;
        uint16_t dh = nn_f32_to_f16(1 / iscale);
        int8_t *sc = (int8_t *)(y + 192);
        put16(y + 208, dh);
        for (int ib = 0; ib < 16; ++ib) sc[ib] = (int8_t)imin(127, nearest_int(iscale * scales[ib]));
        float d0 = nn_f16_to_f32(dh);
        for (int j = 0; j < 16; ++j) {
            float d = d0 * sc[j];
            if (!d) continue;
            for (int ii = 0; ii < 16; ++ii) L[16 * j + ii] = (int8_t)(imax(-32, imin(31, nearest_int(x[16 * j + ii] / d))) + 32);
        }
        uint8_t *ql = y, *qh = y + 128;
        for (int j = 0; j < NN_QK; j += 128, ql += 64, qh += 32)
            for (int l = 0; l < 32; ++l) {
                uint8_t q1 = L[j + l] & 0xF, q2 = L[j + l + 32] & 0xF, q3 = L[j + l + 64] & 0xF, q4 = L[j + l + 96] & 0xF;
                ql[l] = (uint8_t)(q1 | (q3 << 4));
                ql[l + 32] = (uint8_t)(q2 | (q4 << 4));
                qh[l] = (uint8_t)((L[j + l] >> 4) | ((L[j + l + 32] >> 4) << 2) | ((L[j + l + 64] >> 4) << 4) | ((L[j + l + 96] >> 4) << 6));
            }
    }
}

/* ---- the plan: what each tensor becomes ------------------------------------------------ */

/* Where a tensor comes from, and how. */
enum { X_COPY = 0, X_PLUS1, X_NEGEXP, X_CONV };
typedef struct {
    char     name[SK_NAME_MAX];
    char     src[128];
    uint32_t dtype, ndim, dims[SK_MAX_DIMS];
    int      xf;
    uint64_t off, bytes;
} plan;

/* The quantisation plan: 6 bits for every matrix but the feed-forward layer's
 * gate and up projections, which are most of the weights and take 4 bits. A
 * matrix whose rows are not whole blocks stays bfloat16. Held to the KL
 * divergence of Unsloth's Q4_K_M (SUMKIT.md §4). */
static uint32_t mat_type(const char *what, uint32_t layer, uint32_t n_layer, uint32_t cols, int full) {
    (void)layer;
    (void)n_layer;
    if (full || cols % NN_QK) return SK_BF16;
    if (!strcmp(what, "ffn_gate") || !strcmp(what, "ffn_up")) return SK_Q4K;
    return SK_Q6K;
}

static int add_plan(plan *p, int n, const char *name, const char *src, uint32_t dtype, uint32_t rows, uint32_t cols, int xf) {
    plan *e = &p[n];
    memset(e, 0, sizeof *e);
    snprintf(e->name, sizeof e->name, "%s", name);
    snprintf(e->src, sizeof e->src, "%s", src);
    e->dtype = dtype;
    e->ndim = rows ? 2 : 1;
    e->dims[0] = rows ? rows : cols;
    if (rows) e->dims[1] = cols;
    e->xf = xf;
    return n + 1;
}

static int make_plan(const sk_config *c, int full, plan *p) {
    int n = 0;
    char nm[SK_NAME_MAX], src[128];
    const char *lm = "model.language_model.";
    uint32_t kd = c->lin_k_heads * c->lin_k_dim, vd = c->lin_v_heads * c->lin_v_dim, hd = c->head_dim;
    snprintf(src, sizeof src, "%sembed_tokens.weight", lm);
    n = add_plan(p, n, "embed", src, mat_type("embed", 0, c->n_layer, c->n_embd, full), c->n_vocab, c->n_embd, X_COPY);
    snprintf(src, sizeof src, "%snorm.weight", lm);
    n = add_plan(p, n, "norm", src, SK_F32, 0, c->n_embd, X_PLUS1);
    for (uint32_t l = 0; l < c->n_layer; l++) {
#define T(our, theirs, dt, rows, cols, xf) \
        (snprintf(nm, sizeof nm, "L%02u.%s", l, our), snprintf(src, sizeof src, "%slayers.%u.%s", lm, l, theirs), \
         n = add_plan(p, n, nm, src, dt, rows, cols, xf))
#define M(our, theirs, rows, cols) T(our, theirs, mat_type(our, l, c->n_layer, cols, full), rows, cols, X_COPY)
        T("in_norm", "input_layernorm.weight", SK_F32, 0, c->n_embd, X_PLUS1);
        T("post_norm", "post_attention_layernorm.weight", SK_F32, 0, c->n_embd, X_PLUS1);
        M("ffn_gate", "mlp.gate_proj.weight", c->n_ff, c->n_embd);
        M("ffn_up", "mlp.up_proj.weight", c->n_ff, c->n_embd);
        M("ffn_down", "mlp.down_proj.weight", c->n_embd, c->n_ff);
        if (c->linear >> l & 1) {
            M("qkv", "linear_attn.in_proj_qkv.weight", 2 * kd + vd, c->n_embd);
            M("z", "linear_attn.in_proj_z.weight", vd, c->n_embd);
            T("a", "linear_attn.in_proj_a.weight", SK_F32, c->lin_v_heads, c->n_embd, X_COPY);
            T("b", "linear_attn.in_proj_b.weight", SK_F32, c->lin_v_heads, c->n_embd, X_COPY);
            T("conv", "linear_attn.conv1d.weight", SK_F32, c->conv_kernel, 2 * kd + vd, X_CONV);
            T("dt", "linear_attn.dt_bias", SK_F32, 0, c->lin_v_heads, X_COPY);
            T("A", "linear_attn.A_log", SK_F32, 0, c->lin_v_heads, X_NEGEXP);
            T("gnorm", "linear_attn.norm.weight", SK_F32, 0, c->lin_v_dim, X_COPY);
            M("out", "linear_attn.out_proj.weight", c->n_embd, vd);
        } else {
            M("q", "self_attn.q_proj.weight", 2 * c->n_head * hd, c->n_embd);
            M("k", "self_attn.k_proj.weight", c->n_head_kv * hd, c->n_embd);
            M("v", "self_attn.v_proj.weight", c->n_head_kv * hd, c->n_embd);
            M("o", "self_attn.o_proj.weight", c->n_embd, c->n_head * hd);
            T("q_norm", "self_attn.q_norm.weight", SK_F32, 0, hd, X_PLUS1);
            T("k_norm", "self_attn.k_norm.weight", SK_F32, 0, hd, X_PLUS1);
        }
#undef M
#undef T
    }
    return n;
}

static int cmp_plan(const void *a, const void *b) { return strcmp(((const plan *)a)->name, ((const plan *)b)->name); }

/* The source's shape for a planned tensor: conv1d is [channels][1][kernel]. */
static int src_shape_ok(const plan *e, const stensor *s) {
    if (e->xf == X_CONV)
        return s->ndim == 3 && s->dims[0] == e->dims[1] && s->dims[1] == 1 && s->dims[2] == e->dims[0];
    if (s->ndim != e->ndim) return 0;
    for (uint32_t i = 0; i < e->ndim; i++)
        if (s->dims[i] != e->dims[i]) return 0;
    return 1;
}

/* ---- writing ------------------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void putf(uint8_t *p, float f) {
    union { float f; uint32_t u; } v = { .f = f };
    put32(p, v.u);
}
static uint64_t round64(uint64_t n) { return (n + 63) & ~(uint64_t)63; }

/* `n` source values at `off` to floats. */
static int read_floats(int fd, const stensor *s, uint64_t first, size_t n, float *out, uint8_t *tmp) {
    size_t es = s->dtype == 0 ? 4 : 2;
    if (pread_all(fd, tmp, n * es, s->off + first * es)) return -1;
    for (size_t i = 0; i < n; i++) {
        if (s->dtype == 0) memcpy(&out[i], tmp + 4 * i, 4);
        else {
            uint16_t h = (uint16_t)(tmp[2 * i] | tmp[2 * i + 1] << 8);
            out[i] = s->dtype == 1 ? nn_bf16_to_f32(h) : nn_f16_to_f32(h);
        }
    }
    return 0;
}

/* One tensor's bytes, written row by row. */
static int write_tensor(FILE *f, int fd, const plan *e, const stensor *s, char *err, size_t cap) {
    uint32_t rows = e->ndim == 2 ? e->dims[0] : 1, cols = e->ndim == 2 ? e->dims[1] : e->dims[0];
    size_t rb = (size_t)sk_tensor_bytes(e->dtype, 1, &cols);
    float *row = malloc((size_t)cols * sizeof *row);
    uint8_t *tmp = malloc((size_t)cols * 4), *out = malloc(rb + 16);
    int rc = -1;
    if (!row || !tmp || !out) { seterr(err, cap, "out of memory"); goto done; }
    if (e->xf == X_CONV) {
        /* [channels][kernel] to [kernel][channels]: the whole (small) tensor. */
        float *all = malloc((size_t)rows * cols * sizeof *all);
        uint8_t *t2 = malloc((size_t)rows * cols * 4);
        if (!all || !t2 || read_floats(fd, s, 0, (size_t)rows * cols, all, t2)) {
            free(all); free(t2);
            seterr(err, cap, "cannot read %s", s->name);
            goto done;
        }
        for (uint32_t j = 0; j < rows; j++) {
            for (uint32_t ch = 0; ch < cols; ch++) putf(out + 4 * ch, all[(size_t)ch * rows + j]);
            if (fwrite(out, 1, rb, f) != rb) { free(all); free(t2); seterr(err, cap, "cannot write"); goto done; }
        }
        free(all);
        free(t2);
        rc = 0;
        goto done;
    }
    for (uint32_t r = 0; r < rows; r++) {
        if (read_floats(fd, s, (uint64_t)r * cols, cols, row, tmp)) { seterr(err, cap, "cannot read %s", s->name); goto done; }
        if (e->xf == X_PLUS1) for (uint32_t i = 0; i < cols; i++) row[i] = row[i] + 1.0f;
        if (e->xf == X_NEGEXP) for (uint32_t i = 0; i < cols; i++) row[i] = -expf(row[i]);
        switch (e->dtype) {
        case SK_F32: for (uint32_t i = 0; i < cols; i++) putf(out + 4 * i, row[i]); break;
        case SK_BF16:
            /* the source's bfloat16 as it is (it is never converted) */
            for (uint32_t i = 0; i < cols; i++) {
                union { float f; uint32_t u; } v = { .f = row[i] };
                put16(out + 2 * i, (uint16_t)(v.u >> 16));
            }
            break;
        case SK_Q4K: quantize_q4k(row, out, (int)cols); break;
        case SK_Q6K: quantize_q6k(row, out, (int)cols); break;
        }
        if (fwrite(out, 1, rb, f) != rb) { seterr(err, cap, "cannot write"); goto done; }
    }
    rc = 0;
done:
    free(row);
    free(tmp);
    free(out);
    return rc;
}

static int write_raw(FILE *f, const void *p, size_t n) { return fwrite(p, 1, n, f) == n ? 0 : -1; }

int oc_summary_pack(const char *dir, const char *out, int full, const uint8_t (*sha_in)[32],
                    int (*pause)(void *ctx), void *pause_ctx, uint64_t *written, char *err, size_t errcap) {
    char path[SP_FILES][1100], tmpname[1200];
    for (int i = 0; i < SP_FILES; i++) snprintf(path[i], sizeof path[i], "%s/%s", dir, OC_SUMMARY_INPUTS[i]);
    snprintf(tmpname, sizeof tmpname, "%s.tmp", out);
    char *cjs = NULL, *tjs = NULL;
    size_t clen = 0, tlen = 0;
    sk_config c;
    toktab tt = { 0 };
    stfile sf = { 0 };
    plan *p = NULL;
    uint8_t *head = NULL;
    FILE *f = NULL;
    int fd = -1, rc = -1, np = 0;
    uint8_t sha[SP_FILES][32];

    if (!(cjs = read_all(path[SP_CONFIG], &clen))) { seterr(err, errcap, "cannot read %s", path[SP_CONFIG]); goto done; }
    if (!(tjs = read_all(path[SP_TOKENIZER], &tlen))) { seterr(err, errcap, "cannot read %s", path[SP_TOKENIZER]); goto done; }
    if (read_config(cjs, clen, &c, err, errcap) || read_tokenizer(tjs, tlen, &tt, err, errcap)) goto done;
    if (tt.n_tok > c.n_vocab) { seterr(err, errcap, "the tokenizer has more tokens than the model's vocabulary"); goto done; }
    c.n_tok = tt.n_tok;
    c.n_merges = tt.n_merges;
    c.n_eog = tt.n_eog;
    memcpy(c.eog, tt.eog, sizeof c.eog);
    fd = open(path[SP_WEIGHTS], O_RDONLY | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) { seterr(err, errcap, "cannot read %s", path[SP_WEIGHTS]); goto done; }
    if (read_safetensors(fd, (uint64_t)st.st_size, &sf, err, errcap)) goto done;

    /* The plan, checked against the file: every planned tensor there with its
     * shape; every text-model tensor there planned. */
    p = calloc(8 + 20 * (size_t)c.n_layer, sizeof *p);
    if (!p) { seterr(err, errcap, "out of memory"); goto done; }
    np = make_plan(&c, full, p);
    for (int i = 0; i < np; i++) {
        stensor *s = st_find(&sf, p[i].src);
        if (!s) { seterr(err, errcap, "model.safetensors has no %s", p[i].src); goto done; }
        if (!src_shape_ok(&p[i], s)) { seterr(err, errcap, "model.safetensors: %s is not the shape the configuration implies", p[i].src); goto done; }
        s->used = 1;
    }
    for (size_t i = 0; i < sf.n; i++)
        if (!sf.v[i].used && strncmp(sf.v[i].name, "model.visual.", 13) && strncmp(sf.v[i].name, "mtp.", 4)) {
            seterr(err, errcap, "model.safetensors has %s, which the configuration does not imply", sf.v[i].name);
            goto done;
        }
    /* The tokenizer's tables join the plan as tensors written from memory. */
    np = add_plan(p, np, "tok.bytes", "", SK_U8, 0, (uint32_t)(tt.nbytes ? tt.nbytes : 1), X_COPY);
    np = add_plan(p, np, "tok.offs", "", SK_I32, 0, tt.n_tok + 1, X_COPY);
    np = add_plan(p, np, "tok.merges", "", SK_I32, tt.n_merges, 3, X_COPY);
    np = add_plan(p, np, "tok.added", "", SK_I32, 0, tt.n_added ? tt.n_added : 1, X_COPY);
    if (!tt.nbytes || !tt.n_merges || !tt.n_added) { seterr(err, errcap, "tokenizer.json: an empty table"); goto done; }
    qsort(p, (size_t)np, sizeof *p, cmp_plan);
    uint64_t at = round64(SK_HEADER + (uint64_t)np * SK_ENTRY);
    for (int i = 0; i < np; i++) {
        p[i].bytes = sk_tensor_bytes(p[i].dtype, p[i].ndim, p[i].dims);
        p[i].off = at;
        at = round64(at + p[i].bytes);
    }
    uint64_t size = p[np - 1].off + p[np - 1].bytes;

    if (sha_in) memcpy(sha, sha_in, sizeof sha);
    else
        for (int i = 0; i < SP_FILES; i++)
            if (sha_file(path[i], sha[i])) { seterr(err, errcap, "cannot read %s", path[i]); goto done; }

    /* The header and the index. */
    uint64_t data = round64(SK_HEADER + (uint64_t)np * SK_ENTRY);
    head = calloc(1, (size_t)data);
    if (!head) { seterr(err, errcap, "out of memory"); goto done; }
    memcpy(head, SK_MAGIC, 8);
    put32(head + 8, SK_VERSION);
    put32(head + 12, (uint32_t)np);
    put32(head + 16, SK_HEADER);
    put32(head + 20, SK_ENTRY);
    put64(head + 24, data);
    put64(head + 32, size);
    for (int i = 0; i < SP_FILES; i++) memcpy(head + 40 + 32 * i, sha[i], 32);
    put32(head + 136, full ? 1 : 0);
    const uint32_t dims[13] = { c.n_layer, c.n_embd, c.n_ff, c.n_vocab, c.n_head, c.n_head_kv, c.head_dim, c.rot_dims,
                                c.lin_k_heads, c.lin_v_heads, c.lin_k_dim, c.lin_v_dim, c.conv_kernel };
    for (int i = 0; i < 13; i++) put32(head + 140 + 4 * i, dims[i]);
    put64(head + 192, c.linear);
    putf(head + 200, c.rms_eps);
    putf(head + 204, c.rope_theta);
    put32(head + 208, c.n_eog);
    for (int i = 0; i < SK_MAX_EOG; i++) put32(head + 212 + 4 * i, c.eog[i]);
    put32(head + 228, c.n_tok);
    put32(head + 232, c.n_merges);
    for (int i = 0; i < np; i++) {
        uint8_t *e = head + SK_HEADER + (size_t)i * SK_ENTRY;
        memcpy(e, p[i].name, strlen(p[i].name));
        put32(e + 48, p[i].dtype);
        put32(e + 52, p[i].ndim);
        for (int d = 0; d < SK_MAX_DIMS; d++) put32(e + 56 + 4 * d, p[i].dims[d]);
        put64(e + 72, p[i].off);
        put64(e + 80, p[i].bytes);
    }
    f = fopen(tmpname, "wb");
    if (!f || write_raw(f, head, (size_t)data)) { seterr(err, errcap, "cannot write %s", tmpname); goto done; }
    static const uint8_t zeros[64];
    uint64_t pos = data;
    for (int i = 0; i < np; i++) {
        if (pause && pause(pause_ctx)) { seterr(err, errcap, "stopped"); goto done; }
        if (p[i].off > pos && write_raw(f, zeros, (size_t)(p[i].off - pos))) { seterr(err, errcap, "cannot write %s", tmpname); goto done; }
        int r;
        if (!strcmp(p[i].name, "tok.bytes")) r = write_raw(f, tt.bytes, tt.nbytes);
        else if (!strcmp(p[i].name, "tok.offs")) r = write_raw(f, tt.offs, ((size_t)tt.n_tok + 1) * 4);
        else if (!strcmp(p[i].name, "tok.merges")) r = write_raw(f, tt.merges, (size_t)tt.n_merges * 12);
        else if (!strcmp(p[i].name, "tok.added")) r = write_raw(f, tt.added, (size_t)tt.n_added * 4);
        else r = write_tensor(f, fd, &p[i], st_find(&sf, p[i].src), err, errcap);
        if (r) { if (!err[0]) seterr(err, errcap, "cannot write %s", tmpname); goto done; }
        pos = p[i].off + p[i].bytes;
    }
    if (fflush(f) || fsync(fileno(f)) || fclose(f)) { f = NULL; seterr(err, errcap, "cannot write %s", tmpname); goto done; }
    f = NULL;
    if (rename(tmpname, out)) { seterr(err, errcap, "cannot put the model at %s", out); goto done; }
    if (written) *written = size;
    rc = 0;
done:
    if (f) fclose(f);
    if (rc) unlink(tmpname);
    if (fd >= 0) close(fd);
    free(head);
    free(p);
    free(sf.v);
    toktab_free(&tt);
    free(cjs);
    free(tjs);
    return rc;
}

int oc_summary_pack_cli(int argc, char **argv) {
    int full = argc == 5 && !strcmp(argv[2], "--full");
    if (argc != 4 && !full) {
        fprintf(stderr, "usage: openchimed --summary-pack [--full] DIR OUT\n");
        return 2;
    }
    char err[512] = "";
    uint64_t n = 0;
    if (oc_summary_pack(argv[2 + full], argv[3 + full], full, NULL, NULL, NULL, &n, err, sizeof err) != 0) {
        fprintf(stderr, "openchimed: --summary-pack: %s\n", err);
        return 1;
    }
    fprintf(stderr, "openchimed: wrote %s (%llu bytes, %s)\n", argv[3 + full], (unsigned long long)n,
            full ? "unquantised" : "quantised");
    return 0;
}
