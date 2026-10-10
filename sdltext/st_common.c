/*
 * sdltext — the portable core: the byte<->UTF-16 index map.
 *
 * Public offsets are UTF-8 bytes (sdltext.h); every real shaping engine wants
 * UTF-16 (DirectWrite) or codepoints (FreeType). One decoder here produces
 * both the wide string a backend shapes and the offset map hit-testing
 * converts through, so they cannot drift. Covered by make test on any host —
 * this file has no platform dependencies.
 */

#include "st_priv.h"

#include <stdlib.h>

int st__map_build(const char *utf8, size_t len, st_map *map,
                  uint16_t **out_u16, int *out_units)
{
    map->blen = (int)len;
    map->b2w = malloc((len + 1) * sizeof *map->b2w);
    /* Worst case one UTF-16 unit per byte (ASCII), plus the terminator. */
    uint16_t *w = malloc((len + 1) * sizeof *w);
    int *w2b_tmp = malloc((len + 1) * sizeof *w2b_tmp);
    if (!map->b2w || !w || !w2b_tmp) {
        free(map->b2w);
        free(w);
        free(w2b_tmp);
        map->b2w = NULL;
        return 0;
    }

    size_t i = 0;
    int wn = 0;
    while (i < len) {
        const unsigned char *s = (const unsigned char *)utf8 + i;
        uint32_t cp = 0xFFFD;
        size_t take = 1;

        if (s[0] < 0x80) {
            cp = s[0];
        } else if ((s[0] & 0xE0) == 0xC0 && i + 1 < len &&
                   (s[1] & 0xC0) == 0x80) {
            cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
            take = 2;
            if (cp < 0x80) cp = 0xFFFD;                 /* overlong */
        } else if ((s[0] & 0xF0) == 0xE0 && i + 2 < len &&
                   (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
            cp = ((uint32_t)(s[0] & 0x0F) << 12) |
                 ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            take = 3;
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        } else if ((s[0] & 0xF8) == 0xF0 && i + 3 < len &&
                   (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
                   (s[3] & 0xC0) == 0x80) {
            cp = ((uint32_t)(s[0] & 0x07) << 18) |
                 ((uint32_t)(s[1] & 0x3F) << 12) |
                 ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
            take = 4;
            if (cp < 0x10000 || cp > 0x10FFFF) cp = 0xFFFD;
        }
        if (cp == 0xFFFD)
            take = (s[0] < 0x80) ? 1 : take;   /* keep the decided stride */

        /* Every byte of the sequence maps to the unit where it begins, so a
         * span offset landing mid-sequence styles the whole character rather
         * than corrupting a surrogate pair. */
        for (size_t k = 0; k < take; k++)
            map->b2w[i + k] = wn;

        if (cp >= 0x10000) {
            w2b_tmp[wn] = (int)i;
            w[wn++] = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
            w2b_tmp[wn] = (int)i;
            w[wn++] = (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
            w2b_tmp[wn] = (int)i;
            w[wn++] = (uint16_t)cp;
        }
        i += take;
    }
    map->b2w[len] = wn;
    w2b_tmp[wn] = (int)len;
    w[wn] = 0;

    map->wlen = wn;
    map->w2b = malloc((wn + 1) * sizeof *map->w2b);
    if (!map->w2b) {
        free(map->b2w);
        free(w);
        free(w2b_tmp);
        map->b2w = NULL;
        return 0;
    }
    for (int k = 0; k <= wn; k++)
        map->w2b[k] = w2b_tmp[k];
    free(w2b_tmp);

    if (out_u16) {
        *out_u16 = w;
        if (out_units)
            *out_units = wn;
    } else {
        free(w);
    }
    return 1;
}

void st__map_free(st_map *map)
{
    free(map->b2w);
    free(map->w2b);
    map->b2w = NULL;
    map->w2b = NULL;
    map->blen = map->wlen = 0;
}

int st__b2w(const st_map *m, size_t byte_off)
{
    if (!m->b2w)
        return 0;
    if (byte_off > (size_t)m->blen)
        byte_off = (size_t)m->blen;
    return m->b2w[byte_off];
}

int st__w2b(const st_map *m, int u16_off)
{
    if (!m->w2b)
        return 0;
    if (u16_off < 0)
        u16_off = 0;
    if (u16_off > m->wlen)
        u16_off = m->wlen;
    return m->w2b[u16_off];
}

/* ---- Unicode: clusters, line breaks, bidi (st_priv.h) ------------------------ */

#include <string.h>
#include "../third_party/utf8proc/utf8proc.h"

uint32_t st__cp_at(const uint16_t *w16, int n, int u, int *units)
{
    uint32_t c = w16[u];
    if (c >= 0xD800 && c <= 0xDBFF && u + 1 < n && w16[u + 1] >= 0xDC00 && w16[u + 1] <= 0xDFFF) {
        if (units) *units = 2;
        return 0x10000 + ((c - 0xD800) << 10) + (w16[u + 1] - 0xDC00);
    }
    if (units) *units = 1;
    return c;
}

int st__clusters_build(const uint16_t *w16, int n, uint8_t **out_cb)
{
    uint8_t *cb = calloc((size_t)n + 1, 1);
    if (!cb) return 0;
    utf8proc_int32_t state = 0, prev = 0;
    int u = 0, first = 1;
    while (u < n) {
        int k;
        uint32_t cp = st__cp_at(w16, n, u, &k);
        if (first || utf8proc_grapheme_break_stateful(prev, (utf8proc_int32_t)cp, &state)) cb[u] = 1;
        first = 0;
        prev = (utf8proc_int32_t)cp;
        u += k;
    }
    cb[n] = 1;
    if (n) cb[0] = 1;
    *out_cb = cb;
    return 1;
}

int st__cluster_prev(const uint8_t *cb, int n, int u)
{
    if (u > n) u = n;
    while (u > 0 && !cb[u]) u--;
    return u;
}

int st__cluster_next(const uint8_t *cb, int n, int u)
{
    if (u >= n) return n;
    u++;
    while (u < n && !cb[u]) u++;
    return u;
}

/* The line-break class of a code point, the subset that matters here. */
enum { LB_AL, LB_SP, LB_BK, LB_ZW, LB_GL, LB_WJ, LB_ZWJ, LB_BA, LB_HY, LB_CL, LB_EX, LB_IS, LB_SY,
       LB_OP, LB_NU, LB_ID, LB_CM, LB_PR, LB_PO, LB_QU };

static int lb_class(uint32_t c)
{
    if (c == ' ') return LB_SP;
    if (c == '\n' || c == '\r' || c == 0x85 || c == 0x2028 || c == 0x2029) return LB_BK;
    if (c == '\t') return LB_BA;   /* a tab: a break may follow it */
    if (c == 0x200B) return LB_ZW;
    if (c == 0xA0 || c == 0x202F || c == 0x2007 || c == 0x2011 || c == 0x34F) return LB_GL;
    if (c == 0x2060 || c == 0xFEFF) return LB_WJ;
    if (c == 0x200D) return LB_ZWJ;
    if (c == '-') return LB_HY;
    if (c == 0xAD || c == 0x2010 || c == 0x2012 || c == 0x2013 || c == 0x2014 || c == 0x2027 || c == 0x7C) return LB_BA;
    if (c == '!' || c == '?' || c == 0x3002 || c == 0xFF01 || c == 0xFF1F) return LB_EX;
    if (c == ',' || c == ':' || c == ';' || c == 0x3001 || c == 0xFF0C) return LB_IS;
    if (c == '.') return LB_IS;
    if (c == '/') return LB_SY;
    if (c == ')' || c == ']' || c == '}' || c == 0x3009 || c == 0x300B || c == 0x300D || c == 0x300F ||
        c == 0x3011 || c == 0x3015 || c == 0x3017 || c == 0xFF09 || c == 0xFF3D || c == 0xFF5D || c == 0x2019 || c == 0x201D) return LB_CL;
    if (c == '(' || c == '[' || c == '{' || c == 0x3008 || c == 0x300A || c == 0x300C || c == 0x300E ||
        c == 0x3010 || c == 0x3014 || c == 0x3016 || c == 0xFF08 || c == 0xFF3B || c == 0xFF5B || c == 0x2018 || c == 0x201C) return LB_OP;
    if (c == '"' || c == '\'') return LB_QU;
    if (c == '$' || c == 0xA3 || c == 0xA5 || c == 0x20AC || c == '+' || c == 0x2212) return LB_PR;
    if (c == '%' || c == 0x2030 || c == 0xB0) return LB_PO;
    if (c >= '0' && c <= '9') return LB_NU;
    if ((c >= 0x2E80 && c <= 0x303E) || (c >= 0x3041 && c <= 0x33FF) || (c >= 0x3400 && c <= 0x4DBF) ||
        (c >= 0x4E00 && c <= 0x9FFF) || (c >= 0xA000 && c <= 0xA4CF) || (c >= 0xAC00 && c <= 0xD7AF) ||
        (c >= 0xF900 && c <= 0xFAFF) || (c >= 0xFE30 && c <= 0xFE4F) || (c >= 0xFF10 && c <= 0xFF19) ||
        (c >= 0xFF21 && c <= 0xFF5A) || (c >= 0xFF66 && c <= 0xFF9F) || (c >= 0x1F000 && c <= 0x1FAFF) ||
        (c >= 0x20000 && c <= 0x3FFFF)) return LB_ID;
    const utf8proc_property_t *p = utf8proc_get_property((utf8proc_int32_t)c);
    if (p->category == UTF8PROC_CATEGORY_MN || p->category == UTF8PROC_CATEGORY_MC || p->category == UTF8PROC_CATEGORY_ME ||
        p->category == UTF8PROC_CATEGORY_CF) return LB_CM;
    if (p->category == UTF8PROC_CATEGORY_ND) return LB_NU;
    if (p->category == UTF8PROC_CATEGORY_PE) return LB_CL;
    if (p->category == UTF8PROC_CATEGORY_PS) return LB_OP;
    if (p->category == UTF8PROC_CATEGORY_ZS) return LB_SP;
    return LB_AL;
}

int st__break_opps(const uint16_t *w16, int n, const uint8_t *cb, uint8_t **out_brk)
{
    uint8_t *brk = calloc((size_t)n + 1, 1);
    if (!brk) return 0;
    /* Classes per cluster start (a combining mark takes its base's class,
     * UAX #14 LB9), then the pairs. */
    int prev = -1, prev_prev = -1;    /* the class before unit u, and the one before that */
    int sp_run = 0;                    /* the class before a run of spaces */
    for (int u = 0; u < n; u++) {
        if (!cb[u]) { brk[u] = ST_BRK_NONE; continue; }      /* inside a cluster: never */
        int k;
        uint32_t c = st__cp_at(w16, n, u, &k);
        int cls = lb_class(c);
        if (cls == LB_CM) cls = prev >= 0 ? prev : LB_AL;     /* LB9/LB10 */
        int b = ST_BRK_NONE;
        if (u == 0) b = ST_BRK_NONE;
        else if (prev == LB_BK) b = ST_BRK_MANDATORY;          /* LB4-LB5 */
        else if (cls == LB_BK || cls == LB_SP || cls == LB_ZW) b = ST_BRK_NONE;   /* LB6-LB7 */
        else if (prev == LB_ZW) b = ST_BRK_ALLOWED;            /* LB8 */
        else if (cls == LB_WJ || prev == LB_WJ) b = ST_BRK_NONE;     /* LB11 */
        else if (prev == LB_GL) b = ST_BRK_NONE;               /* LB12 */
        else if (cls == LB_GL && prev != LB_SP && prev != LB_BA && prev != LB_HY) b = ST_BRK_NONE;   /* LB12a */
        else if (cls == LB_CL || cls == LB_EX || cls == LB_IS || cls == LB_SY) b = ST_BRK_NONE;   /* LB13 */
        else if (prev == LB_SP) {
            /* LB14-LB18: after spaces a break is allowed, except after an opener. */
            b = sp_run == LB_OP ? ST_BRK_NONE : ST_BRK_ALLOWED;
        }
        else if (prev == LB_OP) b = ST_BRK_NONE;               /* LB14 */
        else if (prev == LB_QU || cls == LB_QU) b = ST_BRK_NONE;     /* LB19 */
        else if (prev == LB_HY && cls == LB_NU) b = ST_BRK_NONE;     /* LB25 (-5) */
        else if (prev == LB_BA || prev == LB_HY) b = ST_BRK_ALLOWED; /* LB21: break after */
        else if (cls == LB_BA || cls == LB_HY) b = ST_BRK_NONE;      /* LB21: none before */
        else if (prev == LB_ZWJ) b = ST_BRK_NONE;                    /* LB8a */
        else if ((prev == LB_IS || prev == LB_SY) && cls == LB_NU) b = ST_BRK_NONE;   /* LB25 */
        else if (prev == LB_NU && (cls == LB_NU || cls == LB_PO)) b = ST_BRK_NONE;    /* LB25 */
        else if (prev == LB_PR && (cls == LB_NU || cls == LB_ID || cls == LB_AL)) b = ST_BRK_NONE;   /* LB23a/LB25 */
        else if (cls == LB_ID || prev == LB_ID) b = ST_BRK_ALLOWED;  /* LB30/LB31 for ideographs: anywhere */
        else b = ST_BRK_NONE;                                        /* LB28-LB30: letters hold */
        brk[u] = (uint8_t)b;
        if (cls != LB_SP) { if (prev != LB_SP) sp_run = prev; }
        else if (prev != LB_SP) sp_run = prev;
        prev_prev = prev; prev = cls;
        (void)prev_prev;
    }
    brk[n] = ST_BRK_NONE;
    *out_brk = brk;
    return 1;
}

/* ---- bidi ------------------------------------------------------------------- */

enum { BC_L = UTF8PROC_BIDI_CLASS_L, BC_R = UTF8PROC_BIDI_CLASS_R, BC_AL = UTF8PROC_BIDI_CLASS_AL,
       BC_EN = UTF8PROC_BIDI_CLASS_EN, BC_ES = UTF8PROC_BIDI_CLASS_ES, BC_ET = UTF8PROC_BIDI_CLASS_ET,
       BC_AN = UTF8PROC_BIDI_CLASS_AN, BC_CS = UTF8PROC_BIDI_CLASS_CS, BC_NSM = UTF8PROC_BIDI_CLASS_NSM,
       BC_BN = UTF8PROC_BIDI_CLASS_BN, BC_B = UTF8PROC_BIDI_CLASS_B, BC_S = UTF8PROC_BIDI_CLASS_S,
       BC_WS = UTF8PROC_BIDI_CLASS_WS, BC_ON = UTF8PROC_BIDI_CLASS_ON,
       BC_LRE = UTF8PROC_BIDI_CLASS_LRE, BC_LRO = UTF8PROC_BIDI_CLASS_LRO, BC_RLE = UTF8PROC_BIDI_CLASS_RLE,
       BC_RLO = UTF8PROC_BIDI_CLASS_RLO, BC_PDF = UTF8PROC_BIDI_CLASS_PDF,
       BC_LRI = UTF8PROC_BIDI_CLASS_LRI, BC_RLI = UTF8PROC_BIDI_CLASS_RLI, BC_FSI = UTF8PROC_BIDI_CLASS_FSI,
       BC_PDI = UTF8PROC_BIDI_CLASS_PDI };

static int bidi_class_of(uint32_t c)
{
    int b = utf8proc_get_property((utf8proc_int32_t)c)->bidi_class;
    return b ? b : BC_L;
}

static int is_isolate_init(int t) { return t == BC_LRI || t == BC_RLI || t == BC_FSI; }
static int is_neutral_or_iso(int t) { return t == BC_B || t == BC_S || t == BC_WS || t == BC_ON || is_isolate_init(t) || t == BC_PDI; }

/* P2/P3 over [a, b): the first strong character outside isolates. */
static int first_strong(const int *cls, int a, int b)
{
    int depth = 0;
    for (int i = a; i < b; i++) {
        int t = cls[i];
        if (is_isolate_init(t)) depth++;
        else if (t == BC_PDI) { if (depth > 0) depth--; }
        else if (depth == 0) {
            if (t == BC_L) return 0;
            if (t == BC_R || t == BC_AL) return 1;
        }
    }
    return -1;
}

int st__bidi_levels(const uint16_t *w16, int n, int para_dir, uint8_t **out_levels, int *out_rtl)
{
    uint8_t *lv = calloc((size_t)n + 1, 1);
    int *cls = malloc(((size_t)n + 1) * sizeof *cls);
    int *orig = malloc(((size_t)n + 1) * sizeof *orig);
    if (!lv || !cls || !orig) { free(lv); free(cls); free(orig); return 0; }
    /* Classes per unit: a pair's second unit is a BN (removed by X9). */
    for (int u = 0; u < n;) {
        int k; uint32_t c = st__cp_at(w16, n, u, &k);
        cls[u] = bidi_class_of(c);
        if (k == 2) cls[u + 1] = BC_BN;
        u += k;
    }
    memcpy(orig, cls, (size_t)n * sizeof *cls);
    int para = para_dir >= 0 ? para_dir : first_strong(cls, 0, n);
    if (para < 0) para = 0;
    if (out_rtl) *out_rtl = para;

    /* X1-X8: the directional status stack. */
    enum { MAX_DEPTH = 125 };
    struct { uint8_t level; int override; int isolate; } st[MAX_DEPTH + 2];
    int sp = 0;
    st[0].level = (uint8_t)para; st[0].override = 0; st[0].isolate = 0;
    int overflow_iso = 0, overflow_emb = 0, valid_iso = 0;
    for (int i = 0; i < n; i++) {
        int t = cls[i];
        if (t == BC_RLE || t == BC_LRE || t == BC_RLO || t == BC_LRO || is_isolate_init(t)) {
            int rtl = t == BC_RLE || t == BC_RLO || t == BC_RLI;
            if (t == BC_FSI) rtl = first_strong(cls, i + 1, n) == 1;   /* to the matching PDI, near enough */
            int iso = is_isolate_init(t);
            lv[i] = st[sp].level;                                   /* an isolate initiator takes the outer level */
            if (iso) { if (st[sp].override) cls[i] = st[sp].override; }
            else cls[i] = BC_BN;                                    /* embeddings are removed (X9) */
            uint8_t cur = st[sp].level;
            uint8_t next = rtl ? (uint8_t)((cur + 1) | 1) : (uint8_t)((cur + 2) & ~1);
            if (next <= MAX_DEPTH && !overflow_iso && !overflow_emb) {
                if (iso) valid_iso++;
                sp++;
                st[sp].level = next;
                st[sp].override = t == BC_LRO ? BC_L : t == BC_RLO ? BC_R : 0;
                st[sp].isolate = iso;
            } else if (iso) overflow_iso++;
            else if (!overflow_iso) overflow_emb++;
            continue;
        }
        if (t == BC_PDI) {
            if (overflow_iso) overflow_iso--;
            else if (valid_iso) {
                overflow_emb = 0;
                while (sp > 0 && !st[sp].isolate) sp--;
                if (sp > 0) sp--;
                valid_iso--;
            }
            lv[i] = st[sp].level;
            if (st[sp].override) cls[i] = st[sp].override;
            continue;
        }
        if (t == BC_PDF) {
            if (!overflow_iso) {
                if (overflow_emb) overflow_emb--;
                else if (sp > 0 && !st[sp].isolate) sp--;
            }
            cls[i] = BC_BN;
            lv[i] = st[sp].level;
            continue;
        }
        if (t == BC_B) { lv[i] = (uint8_t)para; continue; }
        lv[i] = st[sp].level;
        if (t != BC_BN && st[sp].override) cls[i] = st[sp].override;
    }
    /* X9: BNs take the level of what precedes (or the paragraph's), and are
     * skipped by the rules below by looking past them. */
    for (int i = 0; i < n; i++) if (cls[i] == BC_BN) lv[i] = i ? lv[i - 1] : (uint8_t)para;

    /* X10: level runs, each resolved with sos/eos from the neighbouring levels.
     * (Isolating run sequences are approximated by level runs.) */
    int i = 0;
    while (i < n) {
        if (cls[i] == BC_BN) { i++; continue; }
        int a = i, b = i;
        while (b < n && (lv[b] == lv[a] || cls[b] == BC_BN)) b++;
        /* back off trailing BNs */
        while (b > a && cls[b - 1] == BC_BN) b--;
        uint8_t lvl = lv[a];
        int prev_lvl = a > 0 ? lv[a - 1] : para, next_lvl = b < n ? lv[b] : para;
        int sos = ((prev_lvl > lvl ? prev_lvl : lvl) & 1) ? BC_R : BC_L;
        int eos = ((next_lvl > lvl ? next_lvl : lvl) & 1) ? BC_R : BC_L;
        /* W1-W7 */
        int prev = sos;
        for (int k = a; k < b; k++) {
            if (cls[k] == BC_BN) continue;
            if (cls[k] == BC_NSM) cls[k] = (is_isolate_init(prev) || prev == BC_PDI) ? BC_ON : prev;   /* W1 */
            prev = cls[k];
        }
        prev = sos;
        for (int k = a; k < b; k++) {                                /* W2, W3 */
            if (cls[k] == BC_BN) continue;
            if (cls[k] == BC_EN && prev == BC_AL) cls[k] = BC_AN;
            if (cls[k] == BC_R || cls[k] == BC_L || cls[k] == BC_AL) prev = cls[k];
        }
        for (int k = a; k < b; k++) if (cls[k] == BC_AL) cls[k] = BC_R;
        for (int k = a + 1; k + 1 < b; k++) {                        /* W4 */
            if (cls[k] == BC_ES && cls[k - 1] == BC_EN && cls[k + 1] == BC_EN) cls[k] = BC_EN;
            if (cls[k] == BC_CS && cls[k - 1] == BC_EN && cls[k + 1] == BC_EN) cls[k] = BC_EN;
            if (cls[k] == BC_CS && cls[k - 1] == BC_AN && cls[k + 1] == BC_AN) cls[k] = BC_AN;
        }
        for (int k = a; k < b; k++) {                                /* W5 */
            if (cls[k] != BC_ET) continue;
            int s = k; while (k < b && cls[k] == BC_ET) k++;
            int en = (s > a && cls[s - 1] == BC_EN) || (k < b && cls[k] == BC_EN);
            if (en) for (int m = s; m < k; m++) cls[m] = BC_EN;
            k--;
        }
        for (int k = a; k < b; k++) if (cls[k] == BC_ET || cls[k] == BC_ES || cls[k] == BC_CS) cls[k] = BC_ON;   /* W6 */
        prev = sos;
        for (int k = a; k < b; k++) {                                /* W7 */
            if (cls[k] == BC_BN) continue;
            if (cls[k] == BC_EN && prev == BC_L) cls[k] = BC_L;
            if (cls[k] == BC_R || cls[k] == BC_L) prev = cls[k];
        }
        /* N1-N2: neutrals between like directions take it; else the embedding. */
        for (int k = a; k < b; k++) {
            if (!is_neutral_or_iso(cls[k])) continue;
            int s = k; while (k < b && (is_neutral_or_iso(cls[k]) || cls[k] == BC_BN)) k++;
            int before = sos, after = eos;
            for (int m = s - 1; m >= a; m--) if (cls[m] != BC_BN) { before = cls[m]; break; }
            for (int m = k; m < b; m++) if (cls[m] != BC_BN) { after = cls[m]; break; }
            int bd = before == BC_L ? BC_L : BC_R, ad = after == BC_L ? BC_L : BC_R;   /* EN/AN count as R */
            int res = bd == ad ? bd : ((lvl & 1) ? BC_R : BC_L);
            for (int m = s; m < k; m++) if (cls[m] != BC_BN) cls[m] = res;
            k--;
        }
        /* I1-I2 */
        for (int k = a; k < b; k++) {
            if (cls[k] == BC_BN) continue;
            if (!(lvl & 1)) { if (cls[k] == BC_R) lv[k] = lvl + 1; else if (cls[k] == BC_AN || cls[k] == BC_EN) lv[k] = lvl + 2; }
            else if (cls[k] == BC_L || cls[k] == BC_EN || cls[k] == BC_AN) lv[k] = lvl + 1;
        }
        i = b;
    }
    /* L1 for the paragraph's end: trailing whitespace and separators. */
    st__bidi_line_reset(w16, lv, 0, n, para);
    for (int k = 0; k < n; k++) if (orig[k] == BC_B || orig[k] == BC_S) lv[k] = (uint8_t)para;
    lv[n] = (uint8_t)para;
    free(cls); free(orig);
    *out_levels = lv;
    return 1;
}

void st__bidi_line_reset(const uint16_t *w16, uint8_t *levels, int a, int b, int para_level)
{
    for (int k = b - 1; k >= a; k--) {
        int t = bidi_class_of(w16[k]);
        if (t == BC_WS || t == BC_B || t == BC_S || is_isolate_init(t) || t == BC_PDI || t == BC_BN ||
            (w16[k] >= 0xDC00 && w16[k] <= 0xDFFF)) levels[k] = (uint8_t)para_level;
        else break;
    }
}

void st__bidi_reorder(const uint8_t *levels, int n, int *order)
{
    for (int i = 0; i < n; i++) order[i] = i;
    if (n <= 1) return;
    int hi = 0, lo_odd = 255;
    for (int i = 0; i < n; i++) { if (levels[i] > hi) hi = levels[i]; if ((levels[i] & 1) && levels[i] < lo_odd) lo_odd = levels[i]; }
    if (lo_odd == 255) return;
    for (int level = hi; level >= lo_odd; level--) {
        for (int i = 0; i < n; i++) {
            if (levels[order[i]] < level) continue;
            int j = i;
            while (j < n && levels[order[j]] >= level) j++;
            for (int x = i, y = j - 1; x < y; x++, y--) { int t = order[x]; order[x] = order[y]; order[y] = t; }
            i = j;
        }
    }
}
