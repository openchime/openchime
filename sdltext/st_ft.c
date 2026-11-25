/*
 * sdltext — the FreeType backend (Linux).
 *
 * fontconfig resolves families, HarfBuzz shapes, FreeType rasterizes; the
 * paragraph is laid out here, as the canvas backend lays out its own
 * (st_canvas.c): the text is cut into items -- a change of style, a run of
 * blanks, a newline, an inline box, a line break opportunity or a change of
 * bidi level starts one -- each item is shaped, the items are broken into
 * lines, and each line's items are placed in visual order (UAX #9 L2).
 * Grapheme clusters, break opportunities and bidi levels come from the common
 * core (st_priv.h, over utf8proc), so every offset the API speaks is a byte
 * offset into the caller's text, and carets and hit tests land on cluster
 * boundaries, as on the DirectWrite backend.
 *
 * FAMILIES ARE THE SYSTEM'S (ARCH-97). A format's family is asked of
 * fontconfig with "sans-serif" after it, so a family the system lacks
 * ("Segoe UI" on Linux) resolves to the system's interface font, and
 * "system-ui" is that font by name. fontconfig's sorted list for the request
 * is kept: a cluster the matched face has no glyph for is shaped with the
 * first face in the list that has, the fallback DirectWrite and the browser
 * apply inside a run. Bold and italic the family lacks are synthesized as
 * fontconfig's rules ask (an emboldened outline, a slanted matrix).
 *
 * SHAPING IS IN DESIGN UNITS. HarfBuzz shapes at the face's units per em and
 * the advances are scaled to DIPs, so a layout's geometry is the same at every
 * DPI and zoom; only rasterization sees the context's scale. Glyph bitmaps are
 * kept per (face, glyph, pixel size, quarter-pixel phase, synthesis), the
 * cache emptied between draws once it grows past a bound.
 *
 * Pixels leave through the sink (st_ft.h): premultiplied BGRA, grayscale
 * antialiased, colour glyphs (emoji) as their font draws them.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_OUTLINE_H
#include FT_SYNTHESIS_H
#include FT_TRUETYPE_TABLES_H
#include <fontconfig/fontconfig.h>
#include <hb.h>
#include <hb-ot.h>

#include "sdltext.h"
#include "st_ft.h"
#include "st_priv.h"
#include "../third_party/utf8proc/utf8proc.h"

/* ---- faces and fonts ------------------------------------------------------- */

/* One font file's face, shared by every request that resolves to it. Its
 * metrics are fractions of the em, so any size scales them. */
typedef struct st_face {
    struct st_face *next;
    char      *file;
    int        index;
    FT_Face    ft;
    hb_font_t *hb;               /* scaled to the units per em */
    float      upem;
    float      asc, desc, gap;
    float      ul_pos, ul_thick; /* underline centre, below the baseline */
    float      so_pos, so_thick; /* strikeout top, above the baseline */
    float      cap;              /* cap height, 0 when the font does not say */
    FT_F26Dot6 cur_px;           /* the pixel size last set on ft, 26.6 */
} st_face;

/* A face as one fontconfig pattern uses it: the pattern may ask for a
 * synthesized bold or slant. `face` is NULL until the entry is first used,
 * and `failed` set if it could not be loaded. */
typedef struct {
    st_face   *face;
    FcCharSet *cs;               /* the font's coverage, owned by the font set */
    int        embolden, has_matrix, failed;
    FT_Matrix  matrix;
} st_use;

/* A (family, weight, italic) request, resolved: fontconfig's list for it,
 * best match first. */
typedef struct st_font {
    struct st_font *next;
    char      *family;
    int        weight, italic;
    FcPattern *pat;
    FcFontSet *set;
    st_use    *uses;             /* one per entry of set */
} st_font;

/* A rendered glyph, at one pixel size and quarter-pixel phase. */
typedef struct st_glyph {
    struct st_glyph *next;
    const st_face   *face;
    uint32_t         gid;
    FT_F26Dot6       px;
    int              phase, embolden;
    FT_Matrix        matrix;
    int              left, top, w, h;
    int              bgra;       /* premultiplied colour rather than coverage */
    unsigned char   *bits;       /* w*h coverage, or w*h*4 BGRA */
} st_glyph;

enum { GLYPH_BUCKETS = 1024, GLYPH_CACHE_MAX = 6000 };

struct st_ctx {
    float       scale;
    st_sink     sink;
    FT_Library  ft;
    st_face    *faces;
    st_font    *fonts;
    st_font    *emoji;           /* fontconfig's "emoji" family, once asked for */
    int         emoji_tried;
    st_glyph   *glyphs[GLYPH_BUCKETS];
    int         nglyphs;
};

struct st_format {
    st_ctx        *ctx;
    st_format_desc d;
    char           family[128];
    st_font       *font;
};

/* ---- the layout ------------------------------------------------------------- */

typedef struct {
    int off, len;                /* bytes, a whole number of characters */
    uint32_t rgb; float alpha; int has_color;
    int weight, italic, underline, strike, hide;   /* -1 where not set */
    float size;                  /* 0 where not set */
    char family[128];            /* "" where not set */
} st_range;

typedef struct { uint32_t id; int off, len; float w, h, base; } st_box;

/* One shaped glyph. `cs`..`ce` are the units of the shaping cluster it belongs
 * to; it is placed `dx` right of that cluster's left edge and `dy` above the
 * baseline, in DIPs. */
typedef struct {
    const st_use *use;
    uint32_t      gid;
    int           cs, ce;
    float         dx, dy;
    int           invisible;     /* a control character: takes its room, draws nothing */
} st_gpos;

/* One run of one style on one line. */
typedef struct {
    int   off, len;              /* bytes */
    int   u0, un;                /* UTF-16 units */
    float x, w;                  /* line-relative x and width, DIPs */
    int   line;
    int   box;                   /* index into boxes, or -1 */
    int   space, newline;
    int   level;                 /* bidi level: odd reads right to left */
    uint32_t rgb; float alpha; int has_color;
    int   weight, italic, underline, strike, hide;
    float size;
    const char *family;
    st_font *font;
    float asc, desc;             /* DIPs */
    int   g0, g1;                /* its glyphs in the layout's array; a split item's halves share them */
} st_item;

typedef struct { float y, h, base, w, x0; int first, n; } st_lineinfo;   /* x0: where alignment starts it */

struct st_layout {
    st_ctx    *ctx;
    st_format *fmt;
    char      *text; size_t len;
    float      max_w, max_h; int align;
    st_map     map;
    uint16_t  *w16;
    uint8_t   *cb, *brk, *lv;    /* cluster starts, break opportunities, bidi levels, per unit */
    int        para_rtl;
    st_range  *ranges; int nranges, rangecap;
    st_box    *boxes;  int nboxes, boxcap;
    /* the computed layout */
    int        laid;
    st_item   *items; int nitems, itemcap;
    st_gpos   *gl; int ngl, glcap;
    st_lineinfo *lines; int nlines, linecap;
    float     *adv;              /* [wlen + 1] logical prefix advance of each unit, DIPs */
    float      w, h;
    /* trimming: drawing stops at unit trim_u, and the sign follows at ell_x */
    int        trimmed, trim_u, ell_g0, ell_g1, ell_item;
    float      ell_x, ell_w;
};

/* strdup is POSIX, not C11: the GUI compiles this file with -std=c11. */
static char *st_dup(const char *s)
{
    size_t n = strlen(s) + 1;
    char *d = malloc(n);
    if (d) memcpy(d, s, n);
    return d;
}

/* ---- faces ---------------------------------------------------------------------- */

static st_face *face_get(st_ctx *c, const char *file, int index)
{
    for (st_face *f = c->faces; f; f = f->next)
        if (f->index == index && strcmp(f->file, file) == 0)
            return f;
    st_face *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->file = st_dup(file);
    f->index = index;
    if (!f->file || FT_New_Face(c->ft, file, index, &f->ft)) {
        free(f->file);
        free(f);
        return NULL;
    }
    /* HarfBuzz reads the same file, the face index in the low 16 bits and a
     * named instance of a variable font above them, as fontconfig and
     * FreeType encode it. */
    hb_blob_t *blob = hb_blob_create_from_file_or_fail(file);
    hb_face_t *hf = blob ? hb_face_create(blob, (unsigned)index & 0xFFFF) : NULL;
    hb_blob_destroy(blob);
    f->hb = hf ? hb_font_create(hf) : NULL;
    hb_face_destroy(hf);
    if (!f->hb) {
        FT_Done_Face(f->ft);
        free(f->file);
        free(f);
        return NULL;
    }
    hb_ot_font_set_funcs(f->hb);
    if ((index >> 16) > 0)
        hb_font_set_var_named_instance(f->hb, (unsigned)(index >> 16) - 1);
    FT_Face ft = f->ft;
    f->upem = ft->units_per_EM ? (float)ft->units_per_EM : 1000.0f;
    hb_font_set_scale(f->hb, (int)f->upem, (int)f->upem);

    if (ft->ascender > 0) {
        f->asc = ft->ascender / f->upem;
        f->desc = -ft->descender / f->upem;
        float gap = (ft->height - (ft->ascender - ft->descender)) / f->upem;
        f->gap = gap > 0 ? gap : 0;
    } else {
        f->asc = 0.8f;
        f->desc = 0.2f;
    }
    f->ul_pos = ft->underline_thickness ? -ft->underline_position / f->upem : 0.1f;
    f->ul_thick = ft->underline_thickness ? ft->underline_thickness / f->upem : 0.05f;
    f->so_pos = 0.3f;
    f->so_thick = f->ul_thick;
    TT_OS2 *os2 = FT_Get_Sfnt_Table(ft, FT_SFNT_OS2);
    if (os2 && os2->version != 0xFFFF) {
        if (os2->yStrikeoutSize > 0) {
            f->so_pos = os2->yStrikeoutPosition / f->upem;
            f->so_thick = os2->yStrikeoutSize / f->upem;
        }
        if (os2->version >= 2 && os2->sCapHeight > 0)
            f->cap = os2->sCapHeight / f->upem;
    }
    f->next = c->faces;
    c->faces = f;
    return f;
}

/* The face entry `i` of a font's list stands for, loaded on first use. */
static const st_use *use_get(st_ctx *c, st_font *fo, int i)
{
    st_use *u = &fo->uses[i];
    if (u->face || u->failed)
        return u->face ? u : NULL;
    FcPattern *p = FcFontRenderPrepare(NULL, fo->pat, fo->set->fonts[i]);
    FcChar8 *file = NULL;
    int index = 0;
    if (!p || FcPatternGetString(p, FC_FILE, 0, &file) != FcResultMatch) {
        if (p) FcPatternDestroy(p);
        u->failed = 1;
        return NULL;
    }
    FcPatternGetInteger(p, FC_INDEX, 0, &index);
    FcBool emb = FcFalse;
    if (FcPatternGetBool(p, FC_EMBOLDEN, 0, &emb) == FcResultMatch)
        u->embolden = emb ? 1 : 0;
    FcMatrix *m = NULL;
    if (FcPatternGetMatrix(p, FC_MATRIX, 0, &m) == FcResultMatch && m &&
        (m->xx != 1 || m->xy != 0 || m->yx != 0 || m->yy != 1)) {
        u->has_matrix = 1;
        u->matrix.xx = (FT_Fixed)(m->xx * 65536.0);
        u->matrix.xy = (FT_Fixed)(m->xy * 65536.0);
        u->matrix.yx = (FT_Fixed)(m->yx * 65536.0);
        u->matrix.yy = (FT_Fixed)(m->yy * 65536.0);
    }
    u->face = face_get(c, (const char *)file, index);
    FcPatternDestroy(p);
    if (!u->face) {
        u->failed = 1;
        return NULL;
    }
    return u;
}

static int is_generic(const char *family)
{
    static const char *const gen[] = { "system-ui", "sans-serif", "serif", "monospace", "emoji", "cursive", "fantasy" };
    for (size_t i = 0; i < sizeof gen / sizeof gen[0]; i++)
        if (strcmp(family, gen[i]) == 0)
            return 1;
    return 0;
}

/* The request as fontconfig sees it, substituted. "sans-serif" follows the
 * family, so a family the system lacks is the system's interface font. */
static FcPattern *request(const char *family, int weight, int italic)
{
    FcPattern *p = FcPatternCreate();
    if (!p) return NULL;
    FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)(family && family[0] ? family : "system-ui"));
    FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)"sans-serif");
    FcPatternAddInteger(p, FC_WEIGHT, FcWeightFromOpenType(weight > 0 ? weight : 400));
    FcPatternAddInteger(p, FC_SLANT, italic ? FC_SLANT_ITALIC : FC_SLANT_ROMAN);
    FcConfigSubstitute(NULL, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    return p;
}

static st_font *font_get(st_ctx *c, const char *family, int weight, int italic)
{
    if (!family) family = "";
    for (st_font *f = c->fonts; f; f = f->next)
        if (f->weight == weight && f->italic == italic && strcmp(f->family, family) == 0)
            return f->set ? f : NULL;
    st_font *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->family = st_dup(family);
    f->weight = weight;
    f->italic = italic;
    f->pat = request(family, weight, italic);
    FcResult r;
    if (f->family && f->pat)
        f->set = FcFontSort(NULL, f->pat, FcTrue, NULL, &r);
    if (f->set && f->set->nfont > 0) {
        f->uses = calloc((size_t)f->set->nfont, sizeof *f->uses);
        if (f->uses)
            for (int i = 0; i < f->set->nfont; i++)
                FcPatternGetCharSet(f->set->fonts[i], FC_CHARSET, 0, &f->uses[i].cs);
    }
    if (!f->uses || !use_get(c, f, 0)) {
        /* Remembered as unresolvable, so it is not asked again. */
        if (f->set) FcFontSetDestroy(f->set);
        f->set = NULL;
    }
    f->next = c->fonts;
    c->fonts = f;
    return f->set ? f : NULL;
}

/* Characters that need no glyph of their own: joiners, selectors, controls. */
static int no_glyph_needed(uint32_t cp)
{
    return cp < 0x20 || cp == 0x7F || cp == 0x200B || cp == 0x200C || cp == 0x200D || cp == 0x2060 ||
           (cp >= 0xFE00 && cp <= 0xFE0F) || (cp >= 0xE0100 && cp <= 0xE01EF) || cp == 0xFEFF ||
           (cp >= 0xE0020 && cp <= 0xE007F) || cp == 0x2028 || cp == 0x2029;
}

static int covers(const st_use *u, const uint32_t *cps, int n, int base_only)
{
    if (!u->cs) return 0;
    for (int i = 0; i < n; i++) {
        if (no_glyph_needed(cps[i])) continue;
        if (!FcCharSetHasChar(u->cs, cps[i])) return 0;
        if (base_only) return 1;
    }
    return 1;
}

/* Whether a cluster asks to be drawn as an emoji: a presentation selector or
 * a skin tone says so, a flag is one, and a pictograph outside the BMP is one
 * by default; a text selector says not. */
static int emoji_cluster(const uint32_t *cps, int n)
{
    int emoji = 0;
    for (int i = 0; i < n; i++) {
        if (cps[i] == 0xFE0E) return 0;
        if (cps[i] == 0xFE0F || cps[i] == 0x20E3 || (cps[i] >= 0x1F3FB && cps[i] <= 0x1F3FF)) emoji = 1;
    }
    if (n && cps[0] >= 0x1F1E6 && cps[0] <= 0x1F1FF) emoji = 1;
    if (n && cps[0] >= 0x1F000 &&
        utf8proc_get_property((utf8proc_int32_t)cps[0])->boundclass == UTF8PROC_BOUNDCLASS_EXTENDED_PICTOGRAPHIC)
        emoji = 1;
    return emoji;
}

/* Which face shapes a cluster. An emoji comes from fontconfig's "emoji"
 * family first, as DirectWrite and the browsers draw colour emoji: a
 * family's own sorted list ranks colour fonts last, or trims them away.
 * Otherwise the match if it has every character, else the first entry of its
 * list that does, else the first with the base character, else the match
 * (which draws its "no glyph" box). */
static const st_use *pick_use(st_ctx *c, st_font *fo, const uint32_t *cps, int n)
{
    if (emoji_cluster(cps, n)) {
        if (!c->emoji_tried) {
            c->emoji_tried = 1;
            c->emoji = font_get(c, "emoji", 400, 0);
        }
        if (c->emoji && covers(&c->emoji->uses[0], cps, n, 1)) return &c->emoji->uses[0];
    }
    if (covers(&fo->uses[0], cps, n, 0)) return &fo->uses[0];
    for (int pass = 0; pass < 2; pass++)
        for (int i = 1; i < fo->set->nfont; i++)
            if (!fo->uses[i].failed && covers(&fo->uses[i], cps, n, pass) && use_get(c, fo, i))
                return &fo->uses[i];
    return &fo->uses[0];
}

/* ---- context ---------------------------------------------------------------------- */

st_ctx *st_ft_create(const st_sink *sink)
{
    if (!FcInit())
        return NULL;
    st_ctx *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->scale = 1.0f;
    if (sink) c->sink = *sink;
    if (FT_Init_FreeType(&c->ft)) {
        free(c);
        return NULL;
    }
    return c;
}

static void glyphs_flush(st_ctx *c)
{
    for (int b = 0; b < GLYPH_BUCKETS; b++) {
        st_glyph *g = c->glyphs[b];
        while (g) {
            st_glyph *n = g->next;
            free(g->bits);
            free(g);
            g = n;
        }
        c->glyphs[b] = NULL;
    }
    c->nglyphs = 0;
}

void st_ctx_destroy(st_ctx *c)
{
    if (!c) return;
    glyphs_flush(c);
    for (st_font *f = c->fonts; f;) {
        st_font *n = f->next;
        if (f->set) FcFontSetDestroy(f->set);
        if (f->pat) FcPatternDestroy(f->pat);
        free(f->uses);
        free(f->family);
        free(f);
        f = n;
    }
    for (st_face *f = c->faces; f;) {
        st_face *n = f->next;
        hb_font_destroy(f->hb);
        FT_Done_Face(f->ft);
        free(f->file);
        free(f);
        f = n;
    }
    FT_Done_FreeType(c->ft);
    free(c);
}

void st_ctx_set_scale(st_ctx *c, float scale)
{
    c->scale = scale > 0.0f ? scale : 1.0f;
}

float st_ctx_scale(const st_ctx *c)
{
    return c->scale;
}

/* Whether fontconfig's best match for the family is that family. A generic
 * name is always there: fontconfig resolves it to something. */
int st_family_present(st_ctx *c, const char *family)
{
    (void)c;
    if (!family || !family[0]) return 0;
    if (is_generic(family)) return 1;
    FcPattern *p = FcPatternCreate();
    if (!p) return 0;
    FcPatternAddString(p, FC_FAMILY, (const FcChar8 *)family);
    FcConfigSubstitute(NULL, p, FcMatchPattern);
    FcDefaultSubstitute(p);
    FcResult r;
    FcPattern *m = FcFontMatch(NULL, p, &r);
    int found = 0;
    FcChar8 *name = NULL;
    for (int i = 0; m && !found && FcPatternGetString(m, FC_FAMILY, i, &name) == FcResultMatch; i++)
        found = FcStrCmpIgnoreCase(name, (const FcChar8 *)family) == 0;
    if (m) FcPatternDestroy(m);
    FcPatternDestroy(p);
    return found;
}

/* The regular face's ascent and cap height, over its units per em. The cap
 * height is the font's own figure where it gives one, else the ink of "H". */
int st_family_metrics(st_ctx *c, const char *family, float *ascent_em, float *cap_em)
{
    *ascent_em = 0.0f;
    *cap_em = 0.0f;
    if (!c || !family || !st_family_present(c, family)) return 0;
    st_font *fo = font_get(c, family, 400, 0);
    if (!fo) return 0;
    const st_face *f = fo->uses[0].face;
    float cap = f->cap;
    if (cap <= 0) {
        FT_UInt gi = FT_Get_Char_Index(f->ft, 'H');
        if (gi && !FT_Load_Glyph(f->ft, gi, FT_LOAD_NO_SCALE) && f->ft->glyph->format == FT_GLYPH_FORMAT_OUTLINE) {
            FT_BBox bb;
            FT_Outline_Get_CBox(&f->ft->glyph->outline, &bb);
            cap = bb.yMax / f->upem;
        }
    }
    if (!(f->asc > 0) || !(cap > 0)) return 0;
    *ascent_em = f->asc;
    *cap_em = cap;
    return 1;
}

/* ---- formats ------------------------------------------------------------------------ */

st_format *st_format_create(st_ctx *c, const st_format_desc *d)
{
    st_format *f = calloc(1, sizeof *f);
    if (!f) return NULL;
    f->ctx = c;
    f->d = *d;
    if (f->d.weight <= 0) f->d.weight = 400;
    snprintf(f->family, sizeof f->family, "%s", d->family ? d->family : "");
    f->d.family = f->family;
    f->font = font_get(c, f->family, f->d.weight, f->d.italic);
    if (!f->font) {
        free(f);
        return NULL;
    }
    return f;
}

void st_format_destroy(st_format *f)
{
    free(f);
}

/* The format's own line: its matched face's ascent, descent and gap. */
static void natural_metrics(const st_font *fo, float size, float *asc, float *desc)
{
    const st_face *f = fo->uses[0].face;
    *asc = f->asc * size;
    *desc = (f->desc + f->gap) * size;
}

float st_line_height(st_ctx *c, st_format *f)
{
    (void)c;
    if (f->d.line_height > 0) return f->d.line_height;
    float a, d;
    natural_metrics(f->font, f->d.size, &a, &d);
    return a + d;
}

/* ---- layouts -------------------------------------------------------------------------- */

st_layout *st_layout_create(st_ctx *c, st_format *f, const char *utf8, size_t len, float max_w, float max_h, int align)
{
    st_layout *l = calloc(1, sizeof *l);
    if (!l) return NULL;
    l->ctx = c;
    l->fmt = f;
    l->max_w = max_w;
    l->max_h = max_h;
    l->align = align;
    l->text = malloc(len + 1);
    if (!l->text) { free(l); return NULL; }
    if (len) memcpy(l->text, utf8, len);
    l->text[len] = 0;
    l->len = len;
    int units = 0;
    if (!st__map_build(l->text, len, &l->map, &l->w16, &units)) {
        free(l->text);
        free(l);
        return NULL;
    }
    if (!st__clusters_build(l->w16, l->map.wlen, &l->cb) || !st__break_opps(l->w16, l->map.wlen, l->cb, &l->brk) ||
        !st__bidi_levels(l->w16, l->map.wlen, -1, &l->lv, &l->para_rtl)) {
        st_layout_destroy(l);
        return NULL;
    }
    return l;
}

static void layout_drop(st_layout *l)
{
    free(l->items); l->items = NULL; l->nitems = l->itemcap = 0;
    free(l->gl); l->gl = NULL; l->ngl = l->glcap = 0;
    free(l->lines); l->lines = NULL; l->nlines = l->linecap = 0;
    free(l->adv); l->adv = NULL;
    l->laid = 0;
}

void st_layout_destroy(st_layout *l)
{
    if (!l) return;
    layout_drop(l);
    st__map_free(&l->map);
    free(l->cb); free(l->brk); free(l->lv); free(l->w16);
    free(l->ranges);
    free(l->boxes);
    free(l->text);
    free(l);
}

/* A byte range clamped to the text and widened to whole characters, as the
 * map converts it: a range starting inside a character styles all of it. */
static int clamp_range(const st_layout *l, size_t off, size_t len, int *a, int *b)
{
    if (off > l->len) off = l->len;
    if (len > l->len - off) len = l->len - off;
    *a = st__w2b(&l->map, st__b2w(&l->map, off));
    *b = st__w2b(&l->map, st__b2w(&l->map, off + len));
    return *b > *a;
}

static st_range *range_add(st_layout *l, size_t off, size_t len)
{
    int a, b;
    if (!clamp_range(l, off, len, &a, &b)) return NULL;
    if (l->nranges == l->rangecap) {
        int cap = l->rangecap ? l->rangecap * 2 : 16;
        st_range *g = realloc(l->ranges, (size_t)cap * sizeof *g);
        if (!g) return NULL;
        l->ranges = g;
        l->rangecap = cap;
    }
    st_range *r = &l->ranges[l->nranges++];
    memset(r, 0, sizeof *r);
    r->off = a;
    r->len = b - a;
    r->weight = r->italic = r->underline = r->strike = -1;
    l->laid = 0;
    return r;
}

void st_range_color(st_layout *l, size_t off, size_t len, uint32_t rgb, float alpha)
{
    st_range *r = range_add(l, off, len);
    if (r) { r->rgb = rgb; r->alpha = alpha; r->has_color = 1; }
}
void st_range_weight(st_layout *l, size_t off, size_t len, int weight)
{
    st_range *r = range_add(l, off, len);
    if (r) r->weight = weight;
}
void st_range_italic(st_layout *l, size_t off, size_t len, bool on)
{
    st_range *r = range_add(l, off, len);
    if (r) r->italic = on;
}
void st_range_underline(st_layout *l, size_t off, size_t len, bool on)
{
    st_range *r = range_add(l, off, len);
    if (r) r->underline = on;
}
void st_range_strike(st_layout *l, size_t off, size_t len, bool on)
{
    st_range *r = range_add(l, off, len);
    if (r) r->strike = on;
}
void st_range_family(st_layout *l, size_t off, size_t len, const char *family)
{
    st_range *r = range_add(l, off, len);
    if (r) snprintf(r->family, sizeof r->family, "%s", family ? family : "");
}
void st_range_size(st_layout *l, size_t off, size_t len, float size)
{
    st_range *r = range_add(l, off, len);
    if (r && size > 0) r->size = size;
}
void st_range_hide(st_layout *l, size_t off, size_t len)
{
    st_range *r = range_add(l, off, len);
    if (r) r->hide = 1;
}

int st_range_box(st_layout *l, size_t off, size_t len, float w, float h, float baseline, uint32_t box_id)
{
    int a, b;
    if (!clamp_range(l, off, len, &a, &b)) return 0;
    if (l->nboxes == l->boxcap) {
        int cap = l->boxcap ? l->boxcap * 2 : 16;
        st_box *g = realloc(l->boxes, (size_t)cap * sizeof *g);
        if (!g) return 0;
        l->boxes = g;
        l->boxcap = cap;
    }
    l->boxes[l->nboxes++] = (st_box){ box_id, a, b - a, w, h, baseline };
    l->laid = 0;
    return 1;
}

/* The style in force at byte `off`: the format's, then every range covering
 * it, in the order they were set. */
static void style_at(const st_layout *l, int off, st_item *it)
{
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

static int same_style(const st_item *a, const st_item *b)
{
    return a->rgb == b->rgb && a->alpha == b->alpha && a->has_color == b->has_color && a->weight == b->weight &&
           a->italic == b->italic && a->underline == b->underline && a->strike == b->strike && a->hide == b->hide &&
           a->size == b->size && strcmp(a->family, b->family) == 0 && a->box == b->box;
}

static int is_blank(char ch) { return ch == ' ' || ch == '\t'; }

static int item_push(st_layout *l, const st_item *it)
{
    if (l->nitems == l->itemcap) {
        int cap = l->itemcap ? l->itemcap * 2 : 64;
        st_item *g = realloc(l->items, (size_t)cap * sizeof *g);
        if (!g) return 0;
        l->items = g;
        l->itemcap = cap;
    }
    l->items[l->nitems++] = *it;
    return 1;
}

/* Cut the text into items. An item never splits a grapheme cluster. */
static int make_items(st_layout *l)
{
    size_t at = 0;
    while (at < l->len) {
        st_item it;
        memset(&it, 0, sizeof it);
        style_at(l, (int)at, &it);
        it.off = (int)at;
        int ua = st__b2w(&l->map, at);
        it.level = l->lv[ua];
        if (l->text[at] == '\n') {
            it.len = 1;
            it.newline = 1;
            at++;
        } else if (it.box >= 0) {
            const st_box *b = &l->boxes[it.box];
            it.len = b->off + b->len - (int)at;
            at += (size_t)it.len;
        } else {
            int blank = is_blank(l->text[at]);
            it.space = blank;
            int uc = st__cluster_next(l->cb, l->map.wlen, ua);
            size_t end = (size_t)st__w2b(&l->map, uc);
            while (end < l->len && l->text[end] != '\n' && is_blank(l->text[end]) == blank) {
                st_item probe;
                style_at(l, (int)end, &probe);
                if (!same_style(&probe, &it) || probe.box >= 0) break;
                if (l->lv[uc] != it.level) break;
                if (!blank && l->brk[uc] == ST_BRK_ALLOWED) break;
                uc = st__cluster_next(l->cb, l->map.wlen, uc);
                end = (size_t)st__w2b(&l->map, uc);
            }
            if (end <= at) end = at + 1;
            it.len = (int)(end - at);
            at = end;
        }
        it.u0 = st__b2w(&l->map, (size_t)it.off);
        it.un = st__b2w(&l->map, (size_t)(it.off + it.len)) - it.u0;
        if (!item_push(l, &it)) return 0;
    }
    return 1;
}

static int gl_reserve(st_layout *l, int more)
{
    if (l->ngl + more <= l->glcap) return 1;
    int cap = l->glcap ? l->glcap : 256;
    while (cap < l->ngl + more) cap *= 2;
    st_gpos *g = realloc(l->gl, (size_t)cap * sizeof *g);
    if (!g) return 0;
    l->gl = g;
    l->glcap = cap;
    return 1;
}

/* Shape units [a, b) with one face, in the item's direction: the glyphs are
 * appended, and each shaping cluster's advance is shared out over the grapheme
 * clusters it holds (a ligature over two letters gives each a caret stop),
 * into ua[]. */
static int shape_run(st_layout *l, hb_buffer_t *buf, const st_use *u, const st_item *it, int a, int b, float *ua)
{
    const st_face *f = u->face;
    float k = it->size / f->upem;
    hb_buffer_clear_contents(buf);
    hb_buffer_add_utf16(buf, l->w16, l->map.wlen, (unsigned)a, b - a);
    hb_buffer_set_direction(buf, (it->level & 1) ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
    hb_buffer_guess_segment_properties(buf);
    hb_shape(f->hb, buf, NULL, 0);
    unsigned n = 0;
    hb_glyph_info_t *gi = hb_buffer_get_glyph_infos(buf, &n);
    hb_glyph_position_t *gp = hb_buffer_get_glyph_positions(buf, &n);
    if (!gl_reserve(l, (int)n)) return 0;

    /* A tab is four spaces wide; a control character takes no room. */
    hb_codepoint_t space = 0;
    hb_font_get_nominal_glyph(f->hb, ' ', &space);
    hb_position_t space_adv = hb_font_get_glyph_h_advance(f->hb, space);

    for (unsigned i = 0; i < n;) {
        /* The glyphs of one shaping cluster are adjacent in the buffer. */
        int cs = (int)gi[i].cluster;
        unsigned j = i;
        while (j < n && (int)gi[j].cluster == cs) j++;
        /* Clusters are monotonic in the buffer: rising left to right,
         * falling right to left. Where this one ends is the next one's start. */
        int ce = b;
        if (!(it->level & 1) && j < n) ce = (int)gi[j].cluster;
        if ((it->level & 1) && i > 0) ce = (int)gi[i - 1].cluster;
        float acc = 0, total = 0;
        for (unsigned m = i; m < j; m++) {
            st_gpos *g = &l->gl[l->ngl++];
            int units;
            uint32_t cp = st__cp_at(l->w16, l->map.wlen, cs, &units);
            float adv = gp[m].x_advance * k;
            g->use = u;
            g->gid = gi[m].codepoint;
            g->cs = cs;
            g->ce = ce;
            g->invisible = 0;
            if (cp == '\t') {
                g->gid = space;
                adv = space_adv * 4 * k;
            } else if (cp < 0x20 || cp == 0x7F) {
                g->invisible = 1;
                adv = 0;
            }
            g->dx = acc + gp[m].x_offset * k;
            g->dy = gp[m].y_offset * k;
            acc += adv;
            total += adv;
        }
        int starts = 0;
        for (int v = cs; v < ce; v++) starts += l->cb[v] ? 1 : 0;
        if (!starts) ua[cs] += total;
        else
            for (int v = cs; v < ce; v++)
                if (l->cb[v]) ua[v] += total / starts;
        i = j;
    }
    return 1;
}

/* The code points of the grapheme cluster at unit u, up to cap. */
static int cluster_cps(const st_layout *l, int u, int v, uint32_t *cps, int cap)
{
    int n = 0;
    while (u < v && n < cap) {
        int k;
        cps[n++] = st__cp_at(l->w16, l->map.wlen, u, &k);
        u += k;
    }
    return n;
}

/* Shape every item: glyphs into l->gl, the advance of each unit into l->adv
 * (a prefix over the whole text, so a split item needs no new numbers), and
 * each item's width, ascent and descent. */
static int shape_items(st_layout *l)
{
    int n = l->map.wlen;
    float *ua = calloc((size_t)n + 1, sizeof *ua);
    l->adv = calloc((size_t)n + 1, sizeof *l->adv);
    hb_buffer_t *buf = hb_buffer_create();
    if (!ua || !l->adv || !hb_buffer_allocation_successful(buf)) {
        free(ua);
        hb_buffer_destroy(buf);
        return 0;
    }
    int ok = 1;
    for (int i = 0; ok && i < l->nitems; i++) {
        st_item *it = &l->items[i];
        it->g0 = it->g1 = l->ngl;
        it->font = font_get(l->ctx, it->family, it->weight, it->italic);
        if (!it->font) it->font = l->fmt->font;
        if (it->box >= 0) {
            ua[it->u0] += l->boxes[it->box].w;
            continue;
        }
        if (it->hide) continue;
        natural_metrics(it->font, it->size, &it->asc, &it->desc);
        if (it->newline) continue;
        /* Runs of clusters one face can draw, shaped one at a time. */
        int end = it->u0 + it->un, a = it->u0;
        const st_use *cur = NULL;
        for (int u = it->u0; ok && u <= end;) {
            int v = u < end ? st__cluster_next(l->cb, n, u) : end;
            const st_use *pick = cur;
            if (u < end) {
                uint32_t cps[16];
                int ncp = cluster_cps(l, u, v, cps, 16);
                pick = pick_use(l->ctx, it->font, cps, ncp);
            }
            if (cur && (pick != cur || u == end)) {
                const st_use *use = cur;
                ok = shape_run(l, buf, use, it, a, u, ua);
                float fa = use->face->asc * it->size, fd = (use->face->desc + use->face->gap) * it->size;
                if (fa > it->asc) it->asc = fa;
                if (fd > it->desc) it->desc = fd;
                a = u;
            }
            cur = pick;
            if (u == end) break;
            u = v;
        }
        /* Right to left, the runs are shaped in logical order and sit in
         * reverse; each glyph is placed from its own cluster's edge (st_draw),
         * so the order they are kept in does not matter. */
        it->g1 = l->ngl;
    }
    hb_buffer_destroy(buf);
    if (ok) {
        l->adv[0] = 0;
        for (int u = 0; u < n; u++) l->adv[u + 1] = l->adv[u] + ua[u];
        for (int i = 0; i < l->nitems; i++) {
            st_item *it = &l->items[i];
            it->w = (it->newline || it->hide) ? 0 : l->adv[it->u0 + it->un] - l->adv[it->u0];
        }
    }
    free(ua);
    return ok;
}

/* The x of logical position u (u0 <= u <= u0+un) in its item: left to right
 * the prefix's width from the item's left edge; right to left, that much in
 * from its right edge, since the first unit sits at the right. */
static float pos_x(const st_layout *l, const st_item *it, int u)
{
    if (it->newline || it->hide) return it->x;
    if (u < it->u0) u = it->u0;
    if (u > it->u0 + it->un) u = it->u0 + it->un;
    float w = l->adv[u] - l->adv[it->u0];
    return (it->level & 1) ? it->x + it->w - w : it->x + w;
}

/* The visual order of a line's items (UAX #9 L2) and their x positions: the
 * trailing blanks keep the paragraph's level (L1) and sit past the line's
 * width, outside the reordering, where nothing is drawn for them. */
static void place_line(st_layout *l, int first, int n, int *order, uint8_t *levels)
{
    int m = 0, last = first + n;
    while (last > first && (l->items[last - 1].space || l->items[last - 1].newline)) last--;
    for (int j = first; j < last; j++) levels[m++] = (uint8_t)l->items[j].level;
    st__bidi_reorder(levels, m, order);
    float x = 0;
    for (int k = 0; k < m; k++) { st_item *it = &l->items[first + order[k]]; it->x = x; x += it->w; }
    for (int j = last; j < first + n; j++) { st_item *it = &l->items[j]; it->x = x; x += it->w; }
}

/* Split item i at unit `cut` (a cluster boundary inside it): the halves share
 * the shaped glyphs, each drawing those of its own clusters. */
static int split_item(st_layout *l, int i, int cut)
{
    st_item rest = l->items[i];
    if (!item_push(l, &rest)) return 0;
    memmove(&l->items[i + 2], &l->items[i + 1], (size_t)(l->nitems - i - 2) * sizeof rest);
    st_item *it = &l->items[i], *r = &l->items[i + 1];
    int cb = st__w2b(&l->map, cut);
    *r = *it;
    r->off = cb; r->len = it->off + it->len - cb; r->u0 = cut; r->un = it->u0 + it->un - cut;
    it->len = cb - it->off; it->un = cut - it->u0;
    it->w = l->adv[it->u0 + it->un] - l->adv[it->u0];
    r->w = l->adv[r->u0 + r->un] - l->adv[r->u0];
    return 1;
}

static int line_push(st_layout *l)
{
    if (l->nlines == l->linecap) {
        int cap = l->linecap ? l->linecap * 2 : 16;
        st_lineinfo *g = realloc(l->lines, (size_t)cap * sizeof *g);
        if (!g) return 0;
        l->lines = g;
        l->linecap = cap;
    }
    memset(&l->lines[l->nlines], 0, sizeof l->lines[0]);
    return 1;
}

static void trim_line(st_layout *l, hb_buffer_t *buf);

/* Lay the shaped items into lines. */
static int break_lines(st_layout *l)
{
    int wrap = l->fmt->d.wrap && l->max_w > 0;
    float lh_fixed = l->fmt->d.line_height, base_fixed = l->fmt->d.baseline;
    l->nlines = 0; l->w = 0; l->trimmed = 0;
    float y = 0;
    int i = 0, overflow = 0;
    while (i < l->nitems || l->nlines == 0) {
        if (!line_push(l)) return 0;
        st_lineinfo *ln = &l->lines[l->nlines];
        ln->first = i;
        float x = 0, asc = 0, desc = 0;
        int n = 0;
        while (i < l->nitems) {
            st_item *it = &l->items[i];
            if (it->newline) {
                it->x = x; it->line = l->nlines; i++; n++;
                if (it->asc > asc) asc = it->asc;
                if (it->desc > desc) desc = it->desc;
                break;
            }
            if (wrap && n > 0 && !it->space && x + it->w > l->max_w + 0.01f) break;   /* the word goes to the next line */
            if (wrap && it->w > l->max_w && !it->space && it->box < 0 && it->un > 1) {
                /* A word wider than the line: cut it where it stops fitting. */
                int cut = st__cluster_next(l->cb, l->map.wlen, it->u0);
                for (;;) {
                    int next = st__cluster_next(l->cb, l->map.wlen, cut);
                    if (next >= it->u0 + it->un) break;
                    if (x + (l->adv[next] - l->adv[it->u0]) > l->max_w) break;
                    cut = next;
                }
                if (cut > it->u0 && cut < it->u0 + it->un) {
                    if (!split_item(l, i, cut)) return 0;
                    it = &l->items[i];
                }
            }
            it->x = x; it->line = l->nlines;
            x += it->w; i++; n++;
            if (it->asc > asc) asc = it->asc;
            if (it->desc > desc) desc = it->desc;
            if (it->box >= 0) {
                float ba = l->boxes[it->box].base, bd = l->boxes[it->box].h - ba;
                if (ba > asc) asc = ba;
                if (bd > desc) desc = bd;
            }
        }
        if (n == 0 && i >= l->nitems && l->nlines > 0) break;
        if (asc == 0 && desc == 0) natural_metrics(l->fmt->font, l->fmt->d.size, &asc, &desc);
        /* trailing blanks do not count toward the line's width */
        float wline = x;
        for (int k = i - 1; k >= ln->first; k--) {
            if (l->items[k].space || l->items[k].newline) wline = l->items[k].x;
            else break;
        }
        ln->n = n;
        ln->w = wline;
        ln->h = lh_fixed > 0 ? lh_fixed : asc + desc;
        ln->base = lh_fixed > 0 ? base_fixed : asc;
        ln->y = y;
        y += ln->h;
        if (wline > l->w) l->w = wline;
        l->nlines++;
        if (l->max_h > 0 && y >= l->max_h && i < l->nitems) break;
        if (!wrap && l->nlines == 1 && !(i < l->nitems && l->items[i - 1].newline)) {
            /* one line: everything else stays on it; trimmed if asked */
            if (l->fmt->d.trimming == ST_TRIM_ELLIPSIS && l->max_w > 0 && x > l->max_w) overflow = 1;
            break;
        }
        /* Text that ends in a newline ends in an empty line, where the caret
         * after it stands, as on DirectWrite. */
        if (i > 0 && i == l->nitems && l->items[i - 1].newline && !(l->max_h > 0 && y >= l->max_h)) {
            if (!line_push(l)) return 0;
            st_lineinfo *e = &l->lines[l->nlines];
            float a, d;
            natural_metrics(l->fmt->font, l->fmt->d.size, &a, &d);
            e->first = i;
            e->h = lh_fixed > 0 ? lh_fixed : a + d;
            e->base = lh_fixed > 0 ? base_fixed : a;
            e->y = y;
            y += e->h;
            l->nlines++;
            break;
        }
    }
    l->h = y;
    int *order = malloc(((size_t)l->nitems + 1) * sizeof *order);
    uint8_t *levels = malloc((size_t)l->nitems + 1);
    if (!order || !levels) { free(order); free(levels); return 0; }
    for (int k = 0; k < l->nlines; k++) place_line(l, l->lines[k].first, l->lines[k].n, order, levels);
    free(order);
    free(levels);
    if (overflow) {
        hb_buffer_t *buf = hb_buffer_create();
        trim_line(l, buf);
        hb_buffer_destroy(buf);
        if (l->trimmed) l->w = l->lines[0].w;
    }
    /* Alignment: left and right are the reading direction's leading and
     * trailing, as DirectWrite's are: a right-to-left paragraph aligned
     * "left" sits at the right. */
    int align = l->align;
    if (l->para_rtl) align = align == ST_ALIGN_LEFT ? ST_ALIGN_RIGHT : align == ST_ALIGN_RIGHT ? ST_ALIGN_LEFT : align;
    if (align != ST_ALIGN_LEFT && l->max_w > 0) {
        for (int k = 0; k < l->nlines; k++) {
            float dx = align == ST_ALIGN_CENTER ? (l->max_w - l->lines[k].w) / 2 : (l->max_w - l->lines[k].w);
            if (dx < 0) dx = 0;
            l->lines[k].x0 = dx;
            for (int j = l->lines[k].first; j < l->lines[k].first + l->lines[k].n; j++) l->items[j].x += dx;
            if (l->trimmed && k == 0) l->ell_x += dx;
        }
    }
    return 1;
}

/* The one line of a trimmed layout, cut where the sign fits after it: the
 * last cluster boundary whose x leaves room for "…" within the width, with no
 * blank before the sign, as DirectWrite leaves none. Nothing fits: the first
 * cluster stays, clipped, with no sign, as DirectWrite never trims a line to
 * nothing. The cut is in the line's visual order, left to right. */
static void trim_line(st_layout *l, hb_buffer_t *buf)
{
    st_lineinfo *ln = &l->lines[0];
    /* The sign, shaped in the format's font. */
    st_item sign;
    memset(&sign, 0, sizeof sign);
    sign.size = l->fmt->d.size;
    const uint32_t ell = 0x2026;
    const st_use *u = pick_use(l->ctx, l->fmt->font, &ell, 1);
    float ell_w = 0;
    int g0 = l->ngl;
    {
        float k = sign.size / u->face->upem;
        hb_buffer_clear_contents(buf);
        uint32_t s[1] = { ell };
        hb_buffer_add_utf32(buf, s, 1, 0, 1);
        hb_buffer_set_direction(buf, HB_DIRECTION_LTR);
        hb_buffer_guess_segment_properties(buf);
        hb_shape(u->face->hb, buf, NULL, 0);
        unsigned n = 0;
        hb_glyph_info_t *gi = hb_buffer_get_glyph_infos(buf, &n);
        hb_glyph_position_t *gp = hb_buffer_get_glyph_positions(buf, &n);
        if (!gl_reserve(l, (int)n)) return;
        for (unsigned m = 0; m < n; m++) {
            st_gpos *g = &l->gl[l->ngl++];
            g->use = u; g->gid = gi[m].codepoint; g->cs = g->ce = 0; g->invisible = 0;
            g->dx = ell_w + gp[m].x_offset * k;
            g->dy = gp[m].y_offset * k;
            ell_w += gp[m].x_advance * k;
        }
    }
    /* The last fitting boundary, over the line's items in visual order. */
    int best_u = -1, best_item = -1, first_item = -1;
    float best_x = 0;
    for (int j = ln->first; j < ln->first + ln->n; j++) {
        const st_item *it = &l->items[j];
        if (it->newline || it->hide) continue;
        if (first_item < 0) first_item = j;
        for (int v = it->u0; v <= it->u0 + it->un; v = st__cluster_next(l->cb, l->map.wlen, v)) {
            float xv = pos_x(l, it, v);
            if (xv + ell_w > l->max_w + 0.01f) break;
            /* no blank before the sign */
            if (v > it->u0 && is_blank(l->text[st__w2b(&l->map, v) - 1])) { if (v == it->u0 + it->un) break; continue; }
            if (v > it->u0 || best_u < 0) { best_u = v; best_item = j; best_x = xv; }
            if (v == it->u0 + it->un) break;
        }
    }
    l->trimmed = 1;
    if (best_u <= (first_item >= 0 ? l->items[first_item].u0 : 0)) {
        /* nothing fits: the first cluster, no sign */
        best_item = first_item;
        best_u = first_item >= 0 ? st__cluster_next(l->cb, l->map.wlen, l->items[first_item].u0) : 0;
        best_x = first_item >= 0 ? pos_x(l, &l->items[first_item], best_u) : 0;
        l->ngl = g0;
        ell_w = 0;
    }
    l->trim_u = best_u;
    l->ell_item = best_item;
    l->ell_x = best_x;
    l->ell_w = ell_w;
    l->ell_g0 = g0;
    l->ell_g1 = l->ngl;
    ln->w = best_x + ell_w;
}

static int ensure(st_layout *l)
{
    if (l->laid) return 1;
    layout_drop(l);
    if (!make_items(l) || !shape_items(l) || !break_lines(l)) {
        layout_drop(l);
        return 0;
    }
    l->laid = 1;
    return 1;
}

/* ---- metrics, lines, boxes ---------------------------------------------------------- */

void st_layout_metrics(const st_layout *l, st_metrics *m)
{
    if (!ensure((st_layout *)l)) { m->w = m->h = 0; m->lines = 0; return; }
    m->w = l->w;
    m->h = l->h;
    m->lines = l->nlines;
}

int st_layout_lines(const st_layout *l, st_line *out, int cap)
{
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

int st_layout_boxes(const st_layout *l, uint32_t *ids, st_rect *rects, int cap)
{
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

float st_text_width(st_ctx *c, st_format *f, const char *utf8, size_t len)
{
    st_layout *l = st_layout_create(c, f, utf8 ? utf8 : "", utf8 ? len : 0, 0.0f, 0.0f, ST_ALIGN_LEFT);
    if (!l) return 0.0f;
    st_metrics m;
    st_layout_metrics(l, &m);
    st_layout_destroy(l);
    return m.w;
}

/* ---- hit-testing ------------------------------------------------------------------------ */

/* The item logical position u lies in: the one holding unit u, or, at an
 * item's end, that item (so the end of the text has a place). */
static const st_item *item_of_pos(const st_layout *l, int u)
{
    const st_item *at_end = NULL;
    for (int i = 0; i < l->nitems; i++) {
        const st_item *it = &l->items[i];
        if (u >= it->u0 && u < it->u0 + it->un) return it;
        if (u == it->u0 + it->un) at_end = it;
    }
    return at_end;
}

/* Point -> the cluster under it: its leading half answers "before", its
 * trailing half "after" (the left half of a right-to-left cluster is its
 * trailing one). A point beside the line answers the nearest cluster's near
 * edge, outside. */
size_t st_hit_point(const st_layout *l, float x, float y, bool *inside, bool *trailing)
{
    if (inside) *inside = false;
    if (trailing) *trailing = false;
    if (!ensure((st_layout *)l) || !l->nlines) return 0;
    int k = 0;
    while (k + 1 < l->nlines && y >= l->lines[k + 1].y) k++;
    const st_lineinfo *ln = &l->lines[k];
    int in_y = y >= 0 && y < l->h;
    const st_item *near_it = NULL;
    int near_u = -1;
    float near_d = 1e30f, near_lo = 0;
    for (int j = ln->first; j < ln->first + ln->n; j++) {
        const st_item *it = &l->items[j];
        if (it->newline || it->hide) continue;
        int end = it->u0 + it->un;
        for (int u = it->u0; u < end;) {
            int v = st__cluster_next(l->cb, l->map.wlen, u);
            if (v > end) v = end;
            float xa = pos_x(l, it, u), xb = pos_x(l, it, v);
            float lo = xa < xb ? xa : xb, hi = xa < xb ? xb : xa;
            if (hi > lo && x >= lo && x < hi) {
                float mid = (lo + hi) / 2;
                if (inside) *inside = in_y;
                if (trailing) *trailing = (it->level & 1) ? x < mid : x >= mid;
                return (size_t)st__w2b(&l->map, u);
            }
            float d = x < lo ? lo - x : x - hi;
            if (d < near_d) { near_d = d; near_it = it; near_u = u; near_lo = lo; }
            u = v;
        }
    }
    if (!near_it) {
        /* An empty line: its newline, or the end of the text. */
        for (int j = ln->first; j < ln->first + ln->n; j++)
            if (l->items[j].newline) return (size_t)l->items[j].off;
        return ln->n ? (size_t)l->items[ln->first].off : l->len;
    }
    int left_of = x < near_lo;
    if (trailing) *trailing = (near_it->level & 1) ? left_of : !left_of;
    return (size_t)st__w2b(&l->map, near_u);
}

st_rect st_hit_pos(const st_layout *l, size_t off, bool trailing)
{
    st_rect r = { 0, 0, 0, 0 };
    if (!ensure((st_layout *)l) || !l->nlines) return r;
    int u = st__cluster_prev(l->cb, l->map.wlen, st__b2w(&l->map, off));
    const st_lineinfo *last = &l->lines[l->nlines - 1];
    const st_item *it = item_of_pos(l, u);
    if (!it || (u == l->map.wlen && last->n == 0)) {
        /* No text, or the end of text that ends in a newline: the start of
         * the last line. */
        r.x = last->x0;
        r.y = last->y;
        r.h = last->h;
        return r;
    }
    int v = u < it->u0 + it->un ? st__cluster_next(l->cb, l->map.wlen, u) : u;
    if (v > it->u0 + it->un) v = it->u0 + it->un;
    float x0 = pos_x(l, it, u), x1 = pos_x(l, it, v);
    const st_lineinfo *ln = &l->lines[it->line < l->nlines ? it->line : l->nlines - 1];
    r.x = trailing ? x1 : x0;
    r.y = ln->y;
    r.w = fabsf(x1 - x0);
    r.h = ln->h;
    return r;
}

int st_hit_range(const st_layout *l, size_t off, size_t len, st_rect *out, int cap)
{
    if (!ensure((st_layout *)l) || !l->nlines) return 0;
    int ua = st__b2w(&l->map, off), ub = st__b2w(&l->map, off + len);
    if (ub <= ua) return 0;
    int n = 0;
    for (int k = 0; k < l->nlines; k++) {
        const st_lineinfo *ln = &l->lines[k];
        float x0 = 0, x1 = 0;
        int any = 0;
        for (int j = ln->first; j < ln->first + ln->n; j++) {
            const st_item *it = &l->items[j];
            if (it->newline) continue;
            int a = it->u0 > ua ? it->u0 : ua, b = it->u0 + it->un < ub ? it->u0 + it->un : ub;
            if (b <= a) continue;
            float xa = pos_x(l, it, a), xb = pos_x(l, it, b);
            float lo = xa < xb ? xa : xb, hi = xa < xb ? xb : xa;
            if (!any || lo < x0) x0 = lo;
            if (!any || hi > x1) x1 = hi;
            any = 1;
        }
        if (any) {
            if (n < cap) out[n] = (st_rect){ x0, ln->y, x1 - x0, ln->h };
            n++;
        }
    }
    return n;
}

/* ---- rasterization -------------------------------------------------------------------------- */

static unsigned glyph_hash(const st_face *f, uint32_t gid, FT_F26Dot6 px, int phase, int emb, const FT_Matrix *m)
{
    uintptr_t h = (uintptr_t)f;
    h = h * 31 + gid;
    h = h * 31 + (uintptr_t)px;
    h = h * 31 + (uintptr_t)(phase + 4 * emb);
    h = h * 31 + (uintptr_t)m->xy;
    return (unsigned)(h ^ (h >> 15)) % GLYPH_BUCKETS;
}

/* Copy FreeType's bitmap out, as coverage or as premultiplied BGRA, scaled
 * by `k` (a colour font's fixed strike drawn at another size). */
static int glyph_copy(st_glyph *g, const FT_GlyphSlot slot, float k)
{
    const FT_Bitmap *bm = &slot->bitmap;
    int sw = (int)bm->width, sh = (int)bm->rows;
    g->left = slot->bitmap_left;
    g->top = slot->bitmap_top;
    if (sw <= 0 || sh <= 0) { g->w = g->h = 0; return 1; }
    int pitch = bm->pitch;
    const unsigned char *row0 = pitch >= 0 ? bm->buffer : bm->buffer - (ptrdiff_t)pitch * (sh - 1);
    if (bm->pixel_mode == FT_PIXEL_MODE_BGRA) {
        int dw = k == 1.0f ? sw : (int)ceilf(sw * k), dh = k == 1.0f ? sh : (int)ceilf(sh * k);
        if (dw < 1) dw = 1;
        if (dh < 1) dh = 1;
        g->bits = malloc((size_t)dw * dh * 4);
        if (!g->bits) return 0;
        g->bgra = 1;
        g->w = dw;
        g->h = dh;
        if (k != 1.0f) {
            g->left = (int)floorf(slot->bitmap_left * k);
            g->top = (int)ceilf(slot->bitmap_top * k);
        }
        /* Area-averaged down (or nearest up): a strike is usually far larger
         * than the size asked for. */
        for (int y = 0; y < dh; y++)
            for (int x = 0; x < dw; x++) {
                int sx0 = (int)(x / k), sy0 = (int)(y / k);
                int sx1 = (int)((x + 1) / k), sy1 = (int)((y + 1) / k);
                if (sx1 <= sx0) sx1 = sx0 + 1;
                if (sy1 <= sy0) sy1 = sy0 + 1;
                if (sx1 > sw) sx1 = sw;
                if (sy1 > sh) sy1 = sh;
                unsigned acc[4] = { 0, 0, 0, 0 }, cnt = 0;
                for (int sy = sy0; sy < sy1; sy++)
                    for (int sx = sx0; sx < sx1; sx++) {
                        const unsigned char *p = row0 + (ptrdiff_t)sy * pitch + sx * 4;
                        for (int c = 0; c < 4; c++) acc[c] += p[c];
                        cnt++;
                    }
                unsigned char *d = g->bits + ((size_t)y * dw + x) * 4;
                for (int c = 0; c < 4; c++) d[c] = (unsigned char)(cnt ? acc[c] / cnt : 0);
            }
        return 1;
    }
    g->bits = malloc((size_t)sw * sh);
    if (!g->bits) return 0;
    g->w = sw;
    g->h = sh;
    for (int y = 0; y < sh; y++) {
        const unsigned char *src = row0 + (ptrdiff_t)y * pitch;
        unsigned char *dst = g->bits + (size_t)y * sw;
        if (bm->pixel_mode == FT_PIXEL_MODE_MONO)
            for (int x = 0; x < sw; x++) dst[x] = (src[x >> 3] & (0x80 >> (x & 7))) ? 255 : 0;
        else if (bm->pixel_mode == FT_PIXEL_MODE_GRAY)
            memcpy(dst, src, (size_t)sw);
        else
            memset(dst, 0, (size_t)sw);
    }
    return 1;
}

/* A glyph at `ppem` pixels, its origin `phase` quarter pixels right of a
 * whole pixel. Kept until the cache is emptied (never during a draw). */
static const st_glyph *glyph_get(st_ctx *c, const st_use *u, uint32_t gid, float ppem, int phase)
{
    st_face *f = u->face;
    FT_Face ft = f->ft;
    FT_F26Dot6 px = (FT_F26Dot6)lroundf(ppem * 64.0f);
    if (px < 64) px = 64;
    FT_Matrix ident = { 0x10000, 0, 0, 0x10000 };
    const FT_Matrix *m = u->has_matrix ? &u->matrix : &ident;
    if (!FT_IS_SCALABLE(ft)) phase = 0;
    unsigned h = glyph_hash(f, gid, px, phase, u->embolden, m);
    for (st_glyph *g = c->glyphs[h]; g; g = g->next)
        if (g->face == f && g->gid == gid && g->px == px && g->phase == phase && g->embolden == u->embolden &&
            g->matrix.xx == m->xx && g->matrix.xy == m->xy && g->matrix.yx == m->yx && g->matrix.yy == m->yy)
            return g;

    st_glyph *g = calloc(1, sizeof *g);
    if (!g) return NULL;
    g->face = f; g->gid = gid; g->px = px; g->phase = phase; g->embolden = u->embolden; g->matrix = *m;
    float k = 1.0f;
    int ok;
    if (FT_IS_SCALABLE(ft)) {
        if (f->cur_px != px) {
            ok = FT_Set_Char_Size(ft, 0, px, 72, 72) == 0;
            f->cur_px = ok ? px : 0;
        }
    } else if (ft->num_fixed_sizes > 0) {
        /* A bitmap font (colour emoji): the smallest strike at least as
         * large, else the largest, scaled to the size. */
        int best = 0;
        for (int i = 0; i < ft->num_fixed_sizes; i++) {
            FT_Pos have = ft->available_sizes[i].y_ppem, cur = ft->available_sizes[best].y_ppem;
            if ((have >= px && (cur < px || have < cur)) || (cur < px && have > cur)) best = i;
        }
        if (f->cur_px != -1 - best) {
            FT_Select_Size(ft, best);
            f->cur_px = -1 - best;
        }
        if (ft->available_sizes[best].y_ppem > 0)
            k = (float)px / (float)ft->available_sizes[best].y_ppem;
    }
    FT_Matrix tm = *m;
    FT_Vector delta = { phase * 16, 0 };
    FT_Set_Transform(ft, &tm, &delta);
    FT_Int32 flags = FT_LOAD_DEFAULT | FT_LOAD_TARGET_LIGHT;
    if (FT_HAS_COLOR(ft)) flags |= FT_LOAD_COLOR;
    ok = FT_Load_Glyph(ft, gid, flags) == 0;
    if (ok && u->embolden && ft->glyph->format == FT_GLYPH_FORMAT_OUTLINE)
        FT_GlyphSlot_Embolden(ft->glyph);
    if (ok && ft->glyph->format != FT_GLYPH_FORMAT_BITMAP)
        ok = FT_Render_Glyph(ft->glyph, FT_RENDER_MODE_NORMAL) == 0;
    if (!ok || !glyph_copy(g, ft->glyph, k)) {
        /* Remembered blank, so a glyph that will not render is not retried. */
        free(g->bits);
        g->bits = NULL;
        g->w = g->h = 0;
    }
    FT_Set_Transform(ft, NULL, NULL);
    g->next = c->glyphs[h];
    c->glyphs[h] = g;
    c->nglyphs++;
    return g;
}

/* What one draw puts down, in pixels relative to the layout's origin. */
typedef struct {
    const st_glyph *g;          /* NULL: a solid rectangle */
    int x, y, w, h;             /* the glyph's pen position, or the rectangle */
    uint32_t rgb; float alpha;
} st_op;

typedef struct { st_op *ops; int n, cap; } st_ops;

static int op_add(st_ops *o, st_op op)
{
    if (o->n == o->cap) {
        int cap = o->cap ? o->cap * 2 : 256;
        st_op *g = realloc(o->ops, (size_t)cap * sizeof *g);
        if (!g) return 0;
        o->ops = g;
        o->cap = cap;
    }
    o->ops[o->n++] = op;
    return 1;
}

static void put_glyph(st_ctx *c, st_ops *o, const st_gpos *g, float size, float x, float baseline,
                      uint32_t rgb, float alpha)
{
    float s = c->scale;
    float gx = x * s;
    float q = floorf(gx * 4.0f + 0.5f);
    int ix = (int)floorf(q / 4.0f), phase = (int)(q - ix * 4.0f);
    const st_glyph *gl = glyph_get(c, g->use, g->gid, size * s, phase);
    if (!gl || !gl->w) return;
    int iy = (int)lroundf((baseline - g->dy) * s);
    op_add(o, (st_op){ gl, ix, iy, 0, 0, rgb, alpha });
}

static void put_rect(st_ctx *c, st_ops *o, float x0, float x1, float top, float thick, uint32_t rgb, float alpha)
{
    float s = c->scale;
    int px0 = (int)lroundf(x0 * s), px1 = (int)lroundf(x1 * s);
    int h = (int)lroundf(thick * s);
    if (h < 1) h = 1;
    if (px1 <= px0) return;
    op_add(o, (st_op){ NULL, px0, (int)lroundf(top * s), px1 - px0, h, rgb, alpha });
}

static void blend_px(unsigned char *d, unsigned cov, uint32_t rgb, unsigned a255)
{
    unsigned sa = cov * a255 / 255;
    if (!sa) return;
    unsigned inv = 255 - sa;
    d[0] = (unsigned char)((((rgb) & 0xff) * sa + d[0] * inv + 127) / 255);
    d[1] = (unsigned char)((((rgb >> 8) & 0xff) * sa + d[1] * inv + 127) / 255);
    d[2] = (unsigned char)((((rgb >> 16) & 0xff) * sa + d[2] * inv + 127) / 255);
    d[3] = (unsigned char)((sa * 255 + d[3] * inv + 127) / 255);
}

void st_draw(st_ctx *c, st_layout *l, float x, float y, uint32_t rgb, float alpha)
{
    if (!c || !l || !c->sink.blit || !ensure(l)) return;
    if (c->nglyphs > GLYPH_CACHE_MAX) glyphs_flush(c);
    float s = c->scale;
    st_ops o = { NULL, 0, 0 };

    for (int i = 0; i < l->nitems; i++) {
        const st_item *it = &l->items[i];
        if (it->newline || it->hide || it->box >= 0) continue;
        const st_lineinfo *ln = &l->lines[it->line];
        if (l->max_h > 0 && ln->y >= l->max_h) break;
        if (l->trimmed && (it->line > 0 || it->u0 >= l->trim_u)) continue;
        uint32_t col = it->has_color ? it->rgb : rgb;
        float a = it->has_color ? it->alpha * alpha : alpha;
        float base = ln->y + ln->base;
        int end = it->u0 + it->un;
        if (l->trimmed && end > l->trim_u) end = l->trim_u;
        for (int k = it->g0; k < it->g1; k++) {
            const st_gpos *g = &l->gl[k];
            if (g->invisible || g->cs < it->u0 || g->cs >= end) continue;
            float left = (it->level & 1) ? pos_x(l, it, g->ce) : pos_x(l, it, g->cs);
            put_glyph(c, &o, g, it->size, left + g->dx, base, col, a);
        }
        if (it->underline || it->strike) {
            const st_face *f = it->font->uses[0].face;
            float x0 = it->x, x1 = it->x + it->w;
            if (end < it->u0 + it->un) {
                float xa = pos_x(l, it, it->u0), xb = pos_x(l, it, end);
                x0 = xa < xb ? xa : xb;
                x1 = xa < xb ? xb : xa;
            }
            if (it->underline) {
                float th = f->ul_thick * it->size;
                put_rect(c, &o, x0, x1, base + f->ul_pos * it->size - th / 2, th, col, a);
            }
            if (it->strike)
                put_rect(c, &o, x0, x1, base - f->so_pos * it->size, f->so_thick * it->size, col, a);
        }
    }
    if (l->trimmed && l->ell_g1 > l->ell_g0) {
        const st_item *it = l->ell_item >= 0 ? &l->items[l->ell_item] : NULL;
        uint32_t col = it && it->has_color ? it->rgb : rgb;
        float a = it && it->has_color ? it->alpha * alpha : alpha;
        const st_lineinfo *ln = &l->lines[0];
        for (int k = l->ell_g0; k < l->ell_g1; k++)
            put_glyph(c, &o, &l->gl[k], l->fmt->d.size, l->ell_x + l->gl[k].dx, ln->y + ln->base, col, a);
    }

    /* The raster spans the layout box (the aligned width, the trimmed box),
     * a pixel of margin, and whatever ink reaches past it. */
    float span = l->w;
    if (l->max_w > 0 && (l->align != ST_ALIGN_LEFT || l->trimmed) && l->max_w > span) span = l->max_w;
    int x0 = -1, y0 = -1;
    int x1 = (int)ceilf(span * s) + 1, y1 = (int)ceilf(l->h * s) + 1;
    for (int k = 0; k < o.n; k++) {
        const st_op *p = &o.ops[k];
        int ax = p->g ? p->x + p->g->left : p->x, ay = p->g ? p->y - p->g->top : p->y;
        int bx = ax + (p->g ? p->g->w : p->w), by = ay + (p->g ? p->g->h : p->h);
        if (ax < x0) x0 = ax;
        if (ay < y0) y0 = ay;
        if (bx > x1) x1 = bx;
        if (by > y1) y1 = by;
    }
    int pw = x1 - x0, ph = y1 - y0;
    if (pw <= 0 || ph <= 0 || pw > 16384 || ph > 16384) { free(o.ops); return; }
    unsigned char *px = calloc((size_t)pw * ph, 4);
    if (!px) { free(o.ops); return; }

    for (int k = 0; k < o.n; k++) {
        const st_op *p = &o.ops[k];
        float af = p->alpha < 0 ? 0 : p->alpha > 1 ? 1 : p->alpha;
        unsigned a255 = (unsigned)lroundf(af * 255.0f);
        if (!p->g) {
            for (int yy = p->y; yy < p->y + p->h; yy++)
                for (int xx = p->x; xx < p->x + p->w; xx++)
                    blend_px(px + ((size_t)(yy - y0) * pw + (xx - x0)) * 4, 255, p->rgb, a255);
            continue;
        }
        const st_glyph *g = p->g;
        int ox = p->x + g->left - x0, oy = p->y - g->top - y0;
        for (int gy = 0; gy < g->h; gy++) {
            unsigned char *row = px + ((size_t)(oy + gy) * pw + ox) * 4;
            if (g->bgra) {
                const unsigned char *src = g->bits + (size_t)gy * g->w * 4;
                for (int gx = 0; gx < g->w; gx++) {
                    unsigned char *d = row + gx * 4;
                    unsigned sa = src[gx * 4 + 3] * a255 / 255;
                    if (!sa) continue;
                    unsigned inv = 255 - sa;
                    for (int ch = 0; ch < 3; ch++)
                        d[ch] = (unsigned char)((src[gx * 4 + ch] * a255 + d[ch] * inv + 127) / 255);
                    d[3] = (unsigned char)((sa * 255 + d[3] * inv + 127) / 255);
                }
            } else {
                const unsigned char *src = g->bits + (size_t)gy * g->w;
                for (int gx = 0; gx < g->w; gx++)
                    if (src[gx]) blend_px(row + gx * 4, src[gx], p->rgb, a255);
            }
        }
    }
    free(o.ops);
    c->sink.blit(c->sink.user, px, pw * 4, pw, ph, x + x0 / s, y + y0 / s);
    free(px);
}
