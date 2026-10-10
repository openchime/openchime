/*
 * sdltext — the canvas backend (the web, docs/WEB.md step three).
 *
 * The browser's 2D canvas measures and rasterizes text; this file is the
 * layout engine over it, because a canvas gives advances and glyph runs, not
 * paragraphs: line breaking, per-range styles, inline boxes, metrics and
 * hit-testing are done here, on the byte<->UTF-16 map st_common.c keeps, so
 * every offset the API speaks is a byte offset into the caller's text as on
 * the DirectWrite backend. Pixels leave through the same sink (st_dwrite.h):
 * premultiplied BGRA, which gfx_tex_create_text takes as it is.
 *
 * The family comes from the page's system font stack (ARCH-97): a format's
 * family is asked for first, the browser falls back after it. Nothing is
 * bundled.
 *
 * Shaping is the browser's, per run; what this engine does not yet do is
 * bidirectional text and grapheme-aware hit-testing inside complex scripts,
 * where it falls back to UTF-16 units.
 */
#include <emscripten.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "sdltext.h"
#include "st_canvas.h"
#include "st_priv.h"

enum { ST_MAX_RANGES = 96, ST_MAX_BOXES = 64, ST_MAX_LINES = 512, ST_MAX_ITEMS = 2048 };

/* ---- the page side: one offscreen canvas, measured and drawn through ------- */

EM_JS(void, stc_init, (void), {
    if (Module.stc) return;
    var c = (typeof OffscreenCanvas !== 'undefined') ? new OffscreenCanvas(16, 16) : document.createElement('canvas');
    Module.stc = { canvas: c, ctx: c.getContext('2d', { willReadFrequently: true }), w: 16, h: 16 };
});
/* Advances of the UTF-16 prefixes of `text` (n units) in `font`, into out[0..n], in CSS px. */
EM_JS(void, stc_advances, (const char *font, const char *text, int n, float *out), {
    var ctx = Module.stc.ctx;
    ctx.font = UTF8ToString(font);
    var s = UTF8ToString(text);
    HEAPF32[out >> 2] = 0;
    /* Measure whole prefixes rather than summing single characters, so
     * kerning and ligatures land where the browser puts them. */
    for (var i = 1; i <= n; i++) HEAPF32[(out >> 2) + i] = ctx.measureText(s.substring(0, i)).width;
});
EM_JS(float, stc_width, (const char *font, const char *text), {
    var ctx = Module.stc.ctx;
    ctx.font = UTF8ToString(font);
    return ctx.measureText(UTF8ToString(text)).width;
});
/* The font's ascent and descent in CSS px, from a representative measure. */
EM_JS(void, stc_font_metrics, (const char *font, float *asc, float *desc), {
    var ctx = Module.stc.ctx;
    ctx.font = UTF8ToString(font);
    var m = ctx.measureText('Mgjy');
    var a = m.fontBoundingBoxAscent, d = m.fontBoundingBoxDescent;
    if (!(a > 0)) { a = m.actualBoundingBoxAscent * 1.15; d = m.actualBoundingBoxDescent * 1.3; }
    HEAPF32[asc >> 2] = a; HEAPF32[desc >> 2] = d;
});
EM_JS(void, stc_begin, (int w, int h), {
    var s = Module.stc;
    if (s.w !== w || s.h !== h) { s.canvas.width = w; s.canvas.height = h; s.w = w; s.h = h; s.ctx = s.canvas.getContext('2d', { willReadFrequently: true }); }
    s.ctx.clearRect(0, 0, w, h);
    s.ctx.textBaseline = 'alphabetic';
});
EM_JS(void, stc_text, (float x, float y, const char *font, int rgb, float alpha, const char *text), {
    var ctx = Module.stc.ctx;
    ctx.font = UTF8ToString(font);
    ctx.fillStyle = 'rgba(' + ((rgb >> 16) & 255) + ',' + ((rgb >> 8) & 255) + ',' + (rgb & 255) + ',' + alpha + ')';
    ctx.fillText(UTF8ToString(text), x, y);
});
EM_JS(void, stc_rect, (float x, float y, float w, float h, int rgb, float alpha), {
    var ctx = Module.stc.ctx;
    ctx.fillStyle = 'rgba(' + ((rgb >> 16) & 255) + ',' + ((rgb >> 8) & 255) + ',' + (rgb & 255) + ',' + alpha + ')';
    ctx.fillRect(x, y, w, h);
});
/* The raster as premultiplied BGRA into `out` (w*h*4 bytes). */
EM_JS(void, stc_end, (int w, int h, unsigned char *out), {
    var d = Module.stc.ctx.getImageData(0, 0, w, h).data;
    var n = w * h;
    for (var i = 0; i < n; i++) {
        var r = d[i * 4], g = d[i * 4 + 1], b = d[i * 4 + 2], a = d[i * 4 + 3];
        HEAPU8[out + i * 4] = (b * a + 127) / 255 | 0;
        HEAPU8[out + i * 4 + 1] = (g * a + 127) / 255 | 0;
        HEAPU8[out + i * 4 + 2] = (r * a + 127) / 255 | 0;
        HEAPU8[out + i * 4 + 3] = a;
    }
});
EM_JS(int, stc_family_present, (const char *family), {
    try { return document.fonts.check('12px "' + UTF8ToString(family) + '"') ? 1 : 0; } catch (e) { return 0; }
});

/* Counts since the last reading, for the shim's performance line. */
static int g_n_layouts, g_n_draws, g_n_rasters, g_n_measures;
void st_canvas_stats(int *layouts, int *draws, int *rasters, int *measures) {
    *layouts = g_n_layouts; *draws = g_n_draws; *rasters = g_n_rasters; *measures = g_n_measures;
    g_n_layouts = g_n_draws = g_n_rasters = g_n_measures = 0;
}

/* ---- the measurement cache ------------------------------------------------------
 * The same labels are measured on every frame: a channel name, a timestamp,
 * a button. The advances of a (font, text) pair never change, so they are
 * kept, bounded; a miss costs one trip into the canvas. */
enum { MCACHE_SIZE = 4096 };
typedef struct { uint64_t key; char *font; char *text; float *adv; int n; } mcache_entry;
static mcache_entry g_mcache[MCACHE_SIZE];

static uint64_t hash_str(uint64_t h, const char *s) { while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; } return h; }

static const float *measure_cached(const char *font, const char *text, int n) {
    uint64_t key = hash_str(hash_str(1469598103934665603ULL, font) ^ 0x9E3779B97F4A7C15ULL, text);
    mcache_entry *e = &g_mcache[key % MCACHE_SIZE];
    if (e->adv && e->key == key && e->n == n && strcmp(e->font, font) == 0 && strcmp(e->text, text) == 0) return e->adv;
    float *adv = malloc(((size_t)n + 1) * sizeof *adv);
    if (!adv) return NULL;
    g_n_measures++;
    stc_advances(font, text, n, adv);
    char *f = strdup(font), *t = strdup(text);
    if (!f || !t) { free(f); free(t); free(e->adv); e->adv = adv; e->key = 0; return adv; }   /* kept unkeyed: a miss next time */
    free(e->font); free(e->text); free(e->adv);
    e->key = key; e->font = f; e->text = t; e->adv = adv; e->n = n;
    return adv;
}

/* ---- objects ---------------------------------------------------------------- */

struct st_ctx { float scale; st_sink sink; };

struct st_format { st_ctx *ctx; st_format_desc d; char family[96]; };

typedef struct { int off, len; uint32_t rgb; float alpha; int has_color; int weight; int italic; int underline; int strike;
                 float size; char family[64]; int hide; } st_range;   /* -1 / "" where not set */
typedef struct { uint32_t id; int off, len; float w, h, base; } st_box;

/* One run of one style on one line: the item, measured. */
typedef struct {
    int   off, len;          /* bytes */
    int   u0, un;            /* UTF-16 units */
    float x, w;              /* line-relative x, width (DIPs) */
    int   line;
    int   box;               /* index into boxes, or -1 */
    int   space;             /* a run of blanks: may vanish at a line's end */
    int   newline;
    /* resolved style */
    uint32_t rgb; float alpha; int has_color; int weight, italic, underline, strike, hide; float size; const char *family;
    float asc, desc;
    char  font[192];
} st_item;

typedef struct { float y, h, base, w; int first, n; } st_lineinfo;

struct st_layout {
    st_ctx    *ctx;
    st_format *fmt;
    char      *text; size_t len;
    float      max_w, max_h; int align;
    st_map     map;
    st_range   ranges[ST_MAX_RANGES]; int nranges;
    st_box     boxes[ST_MAX_BOXES]; int nboxes;
    /* the computed layout */
    int        laid;
    st_item   *items; int nitems;
    st_lineinfo lines[ST_MAX_LINES]; int nlines;
    float     *adv;          /* [wlen + 1] x of each UTF-16 unit, line-relative, DIPs */
    float      w, h;
    int        trimmed;      /* ellipsis applied: the last item draws with "…" */
    /* the last raster, kept while nothing about the layout changes */
    unsigned char *px; int pw, ph; float px_scale, px_alpha; uint32_t px_rgb; float px_pad;
};

static char *st_dup(const char *s, size_t n) { char *d = malloc(n + 1); if (d) { memcpy(d, s, n); d[n] = 0; } return d; }

static void font_string(char *out, size_t cap, const char *family, float px, int weight, int italic) {
    const char *fallback = ", 'Segoe UI', system-ui, -apple-system, Roboto, 'Helvetica Neue', Arial, 'Noto Color Emoji', 'Apple Color Emoji', 'Segoe UI Emoji', sans-serif";
    snprintf(out, cap, "%s%d %.2fpx \"%s\"%s", italic ? "italic " : "", weight > 0 ? weight : 400, px, family && family[0] ? family : "system-ui", fallback);
}

/* ---- context and formats ------------------------------------------------------ */

st_ctx *st_canvas_create(const st_sink *sink) {
    st_ctx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->scale = 1.0f;
    if (sink) c->sink = *sink;
    stc_init();
    return c;
}
void st_ctx_destroy(st_ctx *c) { free(c); }
void st_ctx_set_scale(st_ctx *c, float scale) { if (c && scale > 0) c->scale = scale; }
float st_ctx_scale(const st_ctx *c) { return c ? c->scale : 1.0f; }
int st_family_present(st_ctx *c, const char *family) { (void)c; return family ? stc_family_present(family) : 0; }

st_format *st_format_create(st_ctx *c, const st_format_desc *d) {
    st_format *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->ctx = c; f->d = *d;
    snprintf(f->family, sizeof f->family, "%s", d->family ? d->family : "");
    f->d.family = f->family;
    return f;
}
void st_format_destroy(st_format *f) { free(f); }

/* A font's ascent and descent, kept per font string: asked for on every layout. */
static void natural_metrics(const char *family, float size, int weight, int italic, float *asc, float *desc) {
    char font[192];
    font_string(font, sizeof font, family, size, weight, italic);
    enum { FM_SIZE = 256 };
    static struct { char *font; float a, d; } fm[FM_SIZE];
    uint64_t key = hash_str(1469598103934665603ULL, font);
    size_t slot = key % FM_SIZE;
    if (fm[slot].font && strcmp(fm[slot].font, font) == 0) { *asc = fm[slot].a; *desc = fm[slot].d; return; }
    float a = 0, d = 0;
    stc_font_metrics(font, &a, &d);
    if (!(a > 0)) { a = size * 0.9f; d = size * 0.25f; }
    *asc = a; *desc = d;
    char *f = strdup(font);
    if (f) { free(fm[slot].font); fm[slot].font = f; fm[slot].a = a; fm[slot].d = d; }
}

float st_line_height(st_ctx *c, st_format *f) {
    (void)c;
    if (f->d.line_height > 0) return f->d.line_height;
    float a, d; natural_metrics(f->family, f->d.size, f->d.weight, f->d.italic, &a, &d);
    return a + d;
}

/* ---- layouts ------------------------------------------------------------------ */

st_layout *st_layout_create(st_ctx *c, st_format *f, const char *utf8, size_t len, float max_w, float max_h, int align) {
    st_layout *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    g_n_layouts++;
    l->ctx = c; l->fmt = f; l->max_w = max_w; l->max_h = max_h; l->align = align;
    l->text = st_dup(utf8 ? utf8 : "", len);
    l->len = len;
    if (!l->text || !st__map_build(l->text, len, &l->map, NULL, NULL)) { free(l->text); free(l); return NULL; }
    return l;
}

static void layout_drop(st_layout *l) {
    free(l->items); l->items = NULL; l->nitems = 0; free(l->adv); l->adv = NULL; l->laid = 0;
    free(l->px); l->px = NULL;
}

void st_layout_destroy(st_layout *l) {
    if (!l) return;
    layout_drop(l);
    st__map_free(&l->map);
    free(l->text);
    free(l);
}

static st_range *range_add(st_layout *l, size_t off, size_t len) {
    if (l->nranges == ST_MAX_RANGES) return NULL;
    st_range *r = &l->ranges[l->nranges++];
    memset(r, 0, sizeof *r);
    r->off = (int)off; r->len = (int)len; r->weight = -1; r->italic = -1; r->underline = -1; r->strike = -1; r->size = 0; r->hide = 0;
    l->laid = 0;
    return r;
}
void st_range_color(st_layout *l, size_t off, size_t len, uint32_t rgb, float alpha) { st_range *r = range_add(l, off, len); if (r) { r->rgb = rgb; r->alpha = alpha; r->has_color = 1; } }
void st_range_weight(st_layout *l, size_t off, size_t len, int weight) { st_range *r = range_add(l, off, len); if (r) r->weight = weight; }
void st_range_italic(st_layout *l, size_t off, size_t len, bool on) { st_range *r = range_add(l, off, len); if (r) r->italic = on; }
void st_range_underline(st_layout *l, size_t off, size_t len, bool on) { st_range *r = range_add(l, off, len); if (r) r->underline = on; }
void st_range_strike(st_layout *l, size_t off, size_t len, bool on) { st_range *r = range_add(l, off, len); if (r) r->strike = on; }
void st_range_family(st_layout *l, size_t off, size_t len, const char *family) { st_range *r = range_add(l, off, len); if (r) snprintf(r->family, sizeof r->family, "%s", family ? family : ""); }
void st_range_size(st_layout *l, size_t off, size_t len, float size) { st_range *r = range_add(l, off, len); if (r) r->size = size; }
void st_range_hide(st_layout *l, size_t off, size_t len) { st_range *r = range_add(l, off, len); if (r) r->hide = 1; }
void st_range_box(st_layout *l, size_t off, size_t len, float w, float h, float baseline, uint32_t box_id) {
    if (l->nboxes == ST_MAX_BOXES) return;
    l->boxes[l->nboxes++] = (st_box){ box_id, (int)off, (int)len, w, h, baseline };
    l->laid = 0;
}

/* The style in force at byte `off`: the format's, then every range covering it, in order. */
static void style_at(const st_layout *l, int off, st_item *it) {
    const st_format *f = l->fmt;
    it->rgb = 0; it->alpha = 1; it->has_color = 0;
    it->weight = f->d.weight; it->italic = f->d.italic; it->underline = 0; it->strike = 0; it->hide = 0;
    it->size = f->d.size; it->family = f->family; it->box = -1;
    for (int i = 0; i < l->nranges; i++) {
        const st_range *r = &l->ranges[i];
        if (off < r->off || off >= r->off + r->len) continue;
        if (r->has_color) { it->rgb = r->rgb; it->alpha = r->alpha; it->has_color = 1; }
        if (r->weight >= 0) it->weight = r->weight;
        if (r->italic >= 0) it->italic = r->italic;
        if (r->underline >= 0) it->underline = r->underline;
        if (r->strike >= 0) it->strike = r->strike;
        if (r->size > 0) it->size = r->size;
        if (r->family[0]) it->family = r->family;
        if (r->hide) it->hide = 1;
    }
    for (int b = 0; b < l->nboxes; b++)
        if (off >= l->boxes[b].off && off < l->boxes[b].off + l->boxes[b].len) { it->box = b; break; }
}

static int same_style(const st_item *a, const st_item *b) {
    return a->rgb == b->rgb && a->alpha == b->alpha && a->has_color == b->has_color && a->weight == b->weight && a->italic == b->italic &&
           a->underline == b->underline && a->strike == b->strike && a->hide == b->hide && a->size == b->size && a->family == b->family && a->box == b->box;
}

static int utf8_step(const char *s, size_t len, size_t at) {
    unsigned char c = (unsigned char)s[at];
    int n = c < 0x80 ? 1 : (c & 0xE0) == 0xC0 ? 2 : (c & 0xF0) == 0xE0 ? 3 : (c & 0xF8) == 0xF0 ? 4 : 1;
    if (at + (size_t)n > len) n = 1;
    return n;
}

static int is_blank(char c) { return c == ' ' || c == '\t'; }

/* Split the text into items: a change of style, a run of blanks, a newline,
 * or a box starts one. */
static int make_items(st_layout *l) {
    size_t cap = 64; l->items = malloc(cap * sizeof *l->items); if (!l->items) return 0;
    l->nitems = 0;
    size_t at = 0;
    while (at < l->len) {
        st_item it; memset(&it, 0, sizeof it);
        style_at(l, (int)at, &it);
        it.off = (int)at;
        if (l->text[at] == '\n') { it.len = 1; it.newline = 1; at++; }
        else if (it.box >= 0) { it.len = l->boxes[it.box].len; at += (size_t)it.len; if (at > l->len) at = l->len; }
        else {
            int blank = is_blank(l->text[at]);
            it.space = blank;
            size_t end = at;
            while (end < l->len && l->text[end] != '\n' && is_blank(l->text[end]) == blank) {
                st_item probe; style_at(l, (int)end, &probe);
                if (end != at && (!same_style(&probe, &it) || probe.box >= 0)) break;
                end += (size_t)utf8_step(l->text, l->len, end);
                if (!blank && end < l->len && is_blank(l->text[end])) break;
            }
            if (end == at) end = at + (size_t)utf8_step(l->text, l->len, at);
            it.len = (int)(end - at); at = end;
        }
        it.u0 = st__b2w(&l->map, (size_t)it.off); it.un = st__b2w(&l->map, (size_t)(it.off + it.len)) - it.u0;
        if ((size_t)l->nitems == cap) { cap *= 2; st_item *g = realloc(l->items, cap * sizeof *g); if (!g) return 0; l->items = g; }
        l->items[l->nitems++] = it;
    }
    return 1;
}

/* Measure every item: its width, and the advance of each of its UTF-16 units into l->adv. */
static int measure_items(st_layout *l) {
    l->adv = calloc((size_t)l->map.wlen + 2, sizeof *l->adv);
    if (!l->adv) return 0;
    float asc0, desc0; natural_metrics(l->fmt->family, l->fmt->d.size, l->fmt->d.weight, l->fmt->d.italic, &asc0, &desc0);
    for (int i = 0; i < l->nitems; i++) {
        st_item *it = &l->items[i];
        font_string(it->font, sizeof it->font, it->family, it->size, it->weight, it->italic);
        if (it->family == l->fmt->family && it->size == l->fmt->d.size && it->weight == l->fmt->d.weight && it->italic == l->fmt->d.italic) { it->asc = asc0; it->desc = desc0; }
        else natural_metrics(it->family, it->size, it->weight, it->italic, &it->asc, &it->desc);
        if (it->newline) { it->w = 0; continue; }
        if (it->box >= 0) { it->w = l->boxes[it->box].w; for (int u = 0; u <= it->un; u++) l->adv[it->u0 + u] = u ? it->w : 0; continue; }
        if (it->hide) { it->w = 0; continue; }
        char *piece = st_dup(l->text + it->off, (size_t)it->len);
        if (!piece) return 0;
        const float *tmp = measure_cached(it->font, piece, it->un);
        if (!tmp) { free(piece); return 0; }
        for (int u = 0; u <= it->un; u++) l->adv[it->u0 + u] = tmp[u];
        it->w = tmp[it->un];
        free(piece);
    }
    return 1;
}

/* Lay the measured items into lines. */
static void break_lines(st_layout *l) {
    int wrap = l->fmt->d.wrap && l->max_w > 0;
    float lh_fixed = l->fmt->d.line_height, base_fixed = l->fmt->d.baseline;
    l->nlines = 0; l->w = 0; l->trimmed = 0;
    float y = 0;
    int i = 0;
    while (i < l->nitems || l->nlines == 0) {
        if (l->nlines == ST_MAX_LINES) break;
        st_lineinfo *ln = &l->lines[l->nlines];
        memset(ln, 0, sizeof *ln);
        ln->first = i;
        float x = 0, asc = 0, desc = 0;
        int n = 0;
        while (i < l->nitems) {
            st_item *it = &l->items[i];
            if (it->newline) { it->x = x; it->line = l->nlines; i++; n++; if (it->asc > asc) asc = it->asc; if (it->desc > desc) desc = it->desc; break; }
            float w = it->w;
            if (wrap && n > 0 && !it->space && x + w > l->max_w + 0.01f) break;   /* the word goes to the next line */
            if (wrap && w > l->max_w && !it->space && it->box < 0 && it->un > 1) {
                /* a single word wider than the line: cut it where it stops fitting */
                int cut = 1;
                while (cut < it->un && x + (l->adv[it->u0 + cut] - l->adv[it->u0]) <= l->max_w) cut++;
                if (cut < it->un) {
                    int cb = st__w2b(&l->map, it->u0 + cut);
                    if (cb > it->off && cb < it->off + it->len) {
                        /* split the item in two */
                        if ((size_t)l->nitems + 1 > (size_t)ST_MAX_ITEMS) { /* no room: keep it whole */ }
                        else {
                            st_item *g = realloc(l->items, ((size_t)l->nitems + 1) * sizeof *g);
                            if (g) {
                                l->items = g; it = &l->items[i];
                                memmove(&l->items[i + 1], &l->items[i], (size_t)(l->nitems - i) * sizeof *it);
                                l->nitems++;
                                st_item *rest = &l->items[i + 1];
                                rest->off = cb; rest->len = it->off + it->len - cb; rest->u0 = it->u0 + cut; rest->un = it->un - cut;
                                float base = l->adv[it->u0 + cut];
                                rest->w = it->w - (base - l->adv[it->u0]);
                                for (int u = 0; u <= rest->un; u++) l->adv[rest->u0 + u] -= (base - l->adv[it->u0]);
                                it->len = cb - it->off; it->un = cut; it->w = base - l->adv[it->u0];
                                w = it->w;
                            }
                        }
                    }
                }
            }
            it->x = x; it->line = l->nlines;
            x += w; i++; n++;
            if (it->asc > asc) asc = it->asc;
            if (it->desc > desc) desc = it->desc;
            if (it->box >= 0) { float ba = l->boxes[it->box].base, bd = l->boxes[it->box].h - ba; if (ba > asc) asc = ba; if (bd > desc) desc = bd; }
        }
        if (n == 0 && i >= l->nitems && l->nlines > 0) break;
        if (asc == 0 && desc == 0) natural_metrics(l->fmt->family, l->fmt->d.size, l->fmt->d.weight, l->fmt->d.italic, &asc, &desc);
        /* trailing blanks do not count toward the line's width */
        float wline = x;
        for (int k = i - 1; k >= ln->first; k--) { if (l->items[k].space) wline = l->items[k].x; else break; }
        ln->n = n; ln->w = wline;
        ln->h = lh_fixed > 0 ? lh_fixed : asc + desc;
        ln->base = lh_fixed > 0 ? base_fixed : asc;
        ln->y = y;
        y += ln->h;
        if (wline > l->w) l->w = wline;
        l->nlines++;
        if (l->max_h > 0 && y >= l->max_h && i < l->nitems) break;
        if (!wrap && l->nlines == 1 && !(i < l->nitems && l->items[i - 1].newline)) {
            /* one line: everything else stays on it (already placed); trim if asked */
            if (l->fmt->d.trimming == ST_TRIM_ELLIPSIS && l->max_w > 0 && x > l->max_w) l->trimmed = 1;
            break;
        }
    }
    l->h = y;
    /* alignment: shift each line */
    if (l->align != ST_ALIGN_LEFT && l->max_w > 0) {
        for (int k = 0; k < l->nlines; k++) {
            float dx = l->align == ST_ALIGN_CENTER ? (l->max_w - l->lines[k].w) / 2 : (l->max_w - l->lines[k].w);
            if (dx < 0) dx = 0;
            for (int j = l->lines[k].first; j < l->lines[k].first + l->lines[k].n; j++) l->items[j].x += dx;
        }
    }
}

static int ensure(st_layout *l) {
    if (l->laid) return 1;
    layout_drop(l);
    if (!make_items(l) || !measure_items(l)) { layout_drop(l); return 0; }
    break_lines(l);
    l->laid = 1;
    return 1;
}

/* ---- metrics, lines, boxes ---------------------------------------------------- */

void st_layout_metrics(const st_layout *l, st_metrics *m) {
    if (!ensure((st_layout *)l)) { m->w = m->h = 0; m->lines = 0; return; }
    m->w = l->trimmed && l->max_w > 0 ? l->max_w : l->w;
    m->h = l->h; m->lines = l->nlines;
}
int st_layout_lines(const st_layout *l, st_line *out, int cap) {
    if (!ensure((st_layout *)l)) return 0;
    int n = 0;
    for (int k = 0; k < l->nlines && n < cap; k++) {
        const st_lineinfo *ln = &l->lines[k];
        int off = ln->n ? l->items[ln->first].off : (int)l->len, end = off;
        for (int j = ln->first; j < ln->first + ln->n; j++) end = l->items[j].off + l->items[j].len;
        out[n++] = (st_line){ ln->y, ln->h, ln->base, (size_t)off, (size_t)(end - off) };
    }
    return n;
}
int st_layout_boxes(const st_layout *l, uint32_t *ids, st_rect *rects, int cap) {
    if (!ensure((st_layout *)l)) return 0;
    int n = 0;
    for (int i = 0; i < l->nitems && n < cap; i++) {
        const st_item *it = &l->items[i];
        if (it->box < 0) continue;
        const st_box *b = &l->boxes[it->box];
        const st_lineinfo *ln = &l->lines[it->line];
        ids[n] = b->id;
        rects[n] = (st_rect){ it->x, ln->y + ln->base - b->base, b->w, b->h };
        n++;
    }
    return n;
}
float st_text_width(st_ctx *c, st_format *f, const char *utf8, size_t len) {
    (void)c;
    char font[192]; font_string(font, sizeof font, f->family, f->d.size, f->d.weight, f->d.italic);
    char *piece = st_dup(utf8 ? utf8 : "", len);
    if (!piece) return 0;
    float w = stc_width(font, piece);
    free(piece);
    return w;
}

/* ---- hit-testing ---------------------------------------------------------------- */

static const st_item *item_of_unit(const st_layout *l, int u, int *idx) {
    for (int i = 0; i < l->nitems; i++) if (u >= l->items[i].u0 && u < l->items[i].u0 + l->items[i].un) { if (idx) *idx = i; return &l->items[i]; }
    return NULL;
}

/* x of UTF-16 unit `u` within its line (its left edge). */
static float unit_x(const st_layout *l, int u, int *line) {
    int idx = -1;
    const st_item *it = item_of_unit(l, u, &idx);
    if (!it) {
        /* the end of the text: after the last item */
        if (l->nitems) { const st_item *last = &l->items[l->nitems - 1]; if (line) *line = last->line; return last->newline ? 0 : last->x + last->w; }
        if (line) *line = 0;
        return 0;
    }
    if (line) *line = it->line;
    if (it->hide || it->newline) return it->x;
    return it->x + (l->adv[u] - l->adv[it->u0]);
}

size_t st_hit_point(const st_layout *l, float x, float y, bool *inside, bool *trailing) {
    if (!ensure((st_layout *)l) || !l->nlines) { if (inside) *inside = false; if (trailing) *trailing = false; return 0; }
    int k = 0;
    while (k + 1 < l->nlines && y >= l->lines[k + 1].y) k++;
    const st_lineinfo *ln = &l->lines[k];
    int in = y >= 0 && y <= l->h && x >= 0;
    /* the units on this line */
    int u_first = ln->n ? l->items[ln->first].u0 : l->map.wlen, u_last = u_first;
    for (int j = ln->first; j < ln->first + ln->n; j++) if (!l->items[j].newline) u_last = l->items[j].u0 + l->items[j].un;
    int best = u_first; float bestd = 1e9f; int trail = 0;
    for (int u = u_first; u <= u_last; u++) {
        float ux = unit_x(l, u, NULL);
        float d = fabsf(ux - x);
        if (d < bestd) { bestd = d; best = u; }
    }
    if (best == u_last && u_last > u_first) { best = u_last - 1; trail = 1; }
    else if (best < u_last) {
        float left = unit_x(l, best, NULL), right = unit_x(l, best + 1, NULL);
        if (x > (left + right) / 2) trail = 1;
        if (x > right) in = 0;
    }
    if (inside) *inside = in && x <= ln->w + 0.5f;
    if (trailing) *trailing = trail;
    return (size_t)st__w2b(&l->map, best);
}

st_rect st_hit_pos(const st_layout *l, size_t off, bool trailing) {
    st_rect r = { 0, 0, 0, 0 };
    if (!ensure((st_layout *)l) || !l->nlines) return r;
    int u = st__b2w(&l->map, off);
    int line = 0;
    float x0 = unit_x(l, u, &line);
    float x1 = u < l->map.wlen ? unit_x(l, u + 1, NULL) : x0;
    if (u < l->map.wlen) { int l1; unit_x(l, u + 1, &l1); if (l1 != line) x1 = x0; }
    const st_lineinfo *ln = &l->lines[line < l->nlines ? line : l->nlines - 1];
    r.x = trailing ? x1 : x0; r.y = ln->y; r.w = x1 - x0; r.h = ln->h;
    return r;
}

int st_hit_range(const st_layout *l, size_t off, size_t len, st_rect *out, int cap) {
    if (!ensure((st_layout *)l) || !l->nlines || cap <= 0) return 0;
    int ua = st__b2w(&l->map, off), ub = st__b2w(&l->map, off + len);
    if (ub <= ua) return 0;
    int n = 0;
    for (int k = 0; k < l->nlines && n < cap; k++) {
        const st_lineinfo *ln = &l->lines[k];
        float x0 = -1, x1 = -1;
        for (int j = ln->first; j < ln->first + ln->n; j++) {
            const st_item *it = &l->items[j];
            if (it->newline) continue;
            int a = it->u0 > ua ? it->u0 : ua, b = it->u0 + it->un < ub ? it->u0 + it->un : ub;
            if (b <= a) continue;
            float xa = it->x + (it->hide ? 0 : l->adv[a] - l->adv[it->u0]);
            float xb = it->x + (it->hide ? 0 : l->adv[b] - l->adv[it->u0]);
            if (x0 < 0 || xa < x0) x0 = xa;
            if (xb > x1) x1 = xb;
        }
        if (x0 >= 0) out[n++] = (st_rect){ x0, ln->y, x1 - x0, ln->h };
    }
    return n;
}

/* ---- drawing ---------------------------------------------------------------------- */

void st_draw(st_ctx *c, st_layout *l, float x, float y, uint32_t rgb, float alpha) {
    if (!c || !l || !ensure(l) || !c->sink.blit) return;
    g_n_draws++;
    float s = c->scale;
    /* The raster spans the aligned width: centred or right-aligned lines sit
     * past the content's own width. */
    float span = l->w;
    if (l->max_w > 0 && (l->align != ST_ALIGN_LEFT || l->trimmed) && l->max_w > span) span = l->max_w;
    float wdip = span + 2.0f, hdip = l->h + 2.0f;
    int pw = (int)ceilf(wdip * s) + 2, ph = (int)ceilf(hdip * s) + 2;
    if (pw <= 0 || ph <= 0 || pw > 8192 || ph > 8192) return;
    const float pad = 1.0f;
    if (l->px && l->pw == pw && l->ph == ph && l->px_scale == s && l->px_rgb == rgb && l->px_alpha == alpha) {
        c->sink.blit(c->sink.user, l->px, pw * 4, pw, ph, x - pad, y - pad);
        return;
    }
    g_n_rasters++;
    stc_begin(pw, ph);
    for (int i = 0; i < l->nitems; i++) {
        const st_item *it = &l->items[i];
        if (it->newline || it->hide || it->box >= 0) continue;
        const st_lineinfo *ln = &l->lines[it->line];
        if (l->max_h > 0 && ln->y >= l->max_h) break;
        char font[192];
        font_string(font, sizeof font, it->family, it->size * s, it->weight, it->italic);
        uint32_t col = it->has_color ? it->rgb : rgb;
        float a = it->has_color ? it->alpha * alpha : alpha;
        float px = (it->x + pad) * s, py = (ln->y + ln->base + pad) * s;
        char *piece = st_dup(l->text + it->off, (size_t)it->len);
        if (!piece) continue;
        if (l->trimmed && it->line == 0 && it->x + it->w > l->max_w) {
            /* cut this item to fit with an ellipsis after it */
            float ell = stc_width(font, "\xE2\x80\xA6") / s;
            int cut = it->un;
            while (cut > 0 && it->x + (l->adv[it->u0 + cut] - l->adv[it->u0]) + ell > l->max_w) cut--;
            int cb = st__w2b(&l->map, it->u0 + cut) - it->off;
            if (cb < 0) cb = 0;
            char *cutp = malloc((size_t)cb + 4);
            if (cutp) { memcpy(cutp, piece, (size_t)cb); memcpy(cutp + cb, "\xE2\x80\xA6", 4); free(piece); piece = cutp; }
            stc_text(px, py, font, (int)col, a, piece);
            free(piece);
            break;
        }
        stc_text(px, py, font, (int)col, a, piece);
        if (it->underline) stc_rect(px, py + 1.5f * s, it->w * s, (s < 1.5f ? 1.0f : s), (int)col, a);
        if (it->strike) stc_rect(px, py - it->asc * 0.3f * s, it->w * s, (s < 1.5f ? 1.0f : s), (int)col, a);
        free(piece);
    }
    unsigned char *px = malloc((size_t)pw * (size_t)ph * 4);
    if (!px) return;
    stc_end(pw, ph, px);
    c->sink.blit(c->sink.user, px, pw * 4, pw, ph, x - pad, y - pad);
    free(l->px);
    l->px = px; l->pw = pw; l->ph = ph; l->px_scale = s; l->px_rgb = rgb; l->px_alpha = alpha; l->px_pad = pad;
}

