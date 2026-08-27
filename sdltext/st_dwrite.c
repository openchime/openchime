/*
 * sdltext — the DirectWrite backend (Windows).
 *
 * Layout, shaping and hit-testing are DirectWrite's; rasterization is a WIC
 * software render target (see st_dwrite.h for why); the byte<->UTF-16 offset
 * map comes from st_common.c so hit-testing and shaping agree about where
 * every character is.
 *
 * Two backend-private rules keep the D2D object model from leaking upward:
 *
 *   BRUSHES NEVER CROSS THE API. Per-range color is data (st_layout holds
 *   rgb/alpha ranges); brushes are created against whichever render target a
 *   draw is about to use and re-applied every draw. A D2D brush belongs to
 *   the target that made it, and letting callers hold brushes is exactly the
 *   coupling that made the old client swap brush globals to take a
 *   screenshot.
 *
 *   FORMATS AND LAYOUTS ARE NEVER MUTATED AFTER THE FACT by the backend
 *   itself — every styling call the public API offers applies to a layout the
 *   caller owns, so two callers can never fight over a shared format's
 *   alignment.
 */

#ifdef _WIN32

#define COBJMACROS
#define CINTERFACE
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "sdltext.h"
#include "st_dwrite.h"
#include "st_priv.h"

/* mingw ships IID_ID2D1Factory in libuuid but not IID_IDWriteFactory (same
 * gap the client works around). */
static const GUID ST_IID_IDWriteFactory =
    { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };
static const GUID ST_IID_IDWriteInlineObject =
    { 0x8339fde3, 0x106f, 0x47ab, { 0x83, 0x73, 0x1c, 0x62, 0x95, 0xeb, 0x10, 0xb3 } };

enum { ST_MAX_COLOR_RANGES = 64, ST_MAX_BOXES = 64 };

struct st_ctx {
    IDWriteFactory      *dw;
    ID2D1Factory        *d2d;
    IWICImagingFactory  *wic;
    IWICBitmap          *surf;      /* raster surface, grown on demand */
    ID2D1RenderTarget   *surf_rt;
    int                  surf_w, surf_h;
    float                scale;
    st_sink              sink;
};

struct st_format {
    st_ctx             *ctx;
    IDWriteTextFormat  *fmt;
    float               size;
};

struct st_layout {
    st_ctx             *ctx;
    IDWriteTextLayout  *lay;
    st_map              map;
    struct { int off, len; uint32_t rgb; float alpha; }
                        colors[ST_MAX_COLOR_RANGES];
    int                 ncolors;
    struct { uint32_t id; int off, len; }
                        boxes[ST_MAX_BOXES];
    int                 nboxes;
};

/* ---- small helpers ------------------------------------------------------ */

static D2D1_COLOR_F st__col(uint32_t rgb, float a)
{
    D2D1_COLOR_F c;
    c.r = ((rgb >> 16) & 0xff) / 255.0f;
    c.g = ((rgb >> 8) & 0xff) / 255.0f;
    c.b = (rgb & 0xff) / 255.0f;
    c.a = a;
    return c;
}

/* UTF-8 -> malloc'd wide string through the shared decoder, so a family name
 * converts by the same rules as layout text. */
static WCHAR *st__wide(const char *utf8, size_t len)
{
    st_map m;
    uint16_t *w = NULL;
    int units = 0;
    if (!st__map_build(utf8, len, &m, &w, &units))
        return NULL;
    st__map_free(&m);
    return (WCHAR *)w;
}

static DWRITE_TEXT_RANGE st__range(const st_layout *l, size_t off, size_t len)
{
    DWRITE_TEXT_RANGE r;
    int a = st__b2w(&l->map, off);
    int b = st__b2w(&l->map, off + len);
    r.startPosition = (UINT32)a;
    r.length = (UINT32)(b - a);
    return r;
}

/* ---- the inline box: a minimal IDWriteInlineObject ---------------------- */

/* Reserves (w, h) in the flow and draws nothing; the caller composites its
 * image at the rect st_layout_boxes reports. This is the real mechanism the
 * old transparent-glyph overdraw approximated. */
typedef struct {
    IDWriteInlineObjectVtbl *lpVtbl;
    LONG                     ref;
    float                    w, h, base;
    IDWriteInlineObjectVtbl  vtbl;
} st_ibox;

static HRESULT STDMETHODCALLTYPE ibox_QI(IDWriteInlineObject *self, REFIID iid,
                                         void **out)
{
    if (IsEqualIID(iid, &IID_IUnknown) ||
        IsEqualIID(iid, &ST_IID_IDWriteInlineObject)) {
        *out = self;
        IDWriteInlineObject_AddRef(self);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static ULONG STDMETHODCALLTYPE ibox_AddRef(IDWriteInlineObject *self)
{
    return (ULONG)InterlockedIncrement(&((st_ibox *)self)->ref);
}

static ULONG STDMETHODCALLTYPE ibox_Release(IDWriteInlineObject *self)
{
    LONG n = InterlockedDecrement(&((st_ibox *)self)->ref);
    if (n == 0)
        free(self);
    return (ULONG)n;
}

static HRESULT STDMETHODCALLTYPE ibox_Draw(IDWriteInlineObject *self,
                                           void *ctx, IDWriteTextRenderer *r,
                                           FLOAT x, FLOAT y, BOOL sideways,
                                           BOOL rtl, IUnknown *effect)
{
    (void)self; (void)ctx; (void)r; (void)x; (void)y;
    (void)sideways; (void)rtl; (void)effect;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ibox_GetMetrics(IDWriteInlineObject *self,
                                                 DWRITE_INLINE_OBJECT_METRICS *m)
{
    st_ibox *b = (st_ibox *)self;
    m->width = b->w;
    m->height = b->h;
    m->baseline = b->base;
    m->supportsSideways = FALSE;
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ibox_GetOverhang(IDWriteInlineObject *self,
                                                  DWRITE_OVERHANG_METRICS *m)
{
    (void)self;
    memset(m, 0, sizeof *m);
    return S_OK;
}

static HRESULT STDMETHODCALLTYPE ibox_GetBreaks(IDWriteInlineObject *self,
                                                DWRITE_BREAK_CONDITION *before,
                                                DWRITE_BREAK_CONDITION *after)
{
    (void)self;
    *before = DWRITE_BREAK_CONDITION_NEUTRAL;
    *after = DWRITE_BREAK_CONDITION_NEUTRAL;
    return S_OK;
}

static IDWriteInlineObject *ibox_create(float w, float h, float base)
{
    st_ibox *b = calloc(1, sizeof *b);
    if (!b)
        return NULL;
    b->vtbl.QueryInterface = ibox_QI;
    b->vtbl.AddRef = ibox_AddRef;
    b->vtbl.Release = ibox_Release;
    b->vtbl.Draw = ibox_Draw;
    b->vtbl.GetMetrics = ibox_GetMetrics;
    b->vtbl.GetOverhangMetrics = ibox_GetOverhang;
    b->vtbl.GetBreakConditions = ibox_GetBreaks;
    b->lpVtbl = &b->vtbl;
    b->ref = 1;
    b->w = w;
    b->h = h;
    b->base = base;
    return (IDWriteInlineObject *)b;
}

/* ---- context ------------------------------------------------------------ */

st_ctx *st_dwrite_create(const st_sink *sink)
{
    /* The client initializes COM for WIC already; tolerate either order. */
    HRESULT ch = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(ch) && ch != RPC_E_CHANGED_MODE)
        return NULL;

    st_ctx *c = calloc(1, sizeof *c);
    if (!c)
        return NULL;
    c->scale = 1.0f;
    if (sink)
        c->sink = *sink;

    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                                   &ST_IID_IDWriteFactory,
                                   (IUnknown **)&c->dw)) ||
        FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED,
                                 &IID_ID2D1Factory, NULL, (void **)&c->d2d)) ||
        FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL,
                                CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory,
                                (void **)&c->wic))) {
        st_ctx_destroy(c);
        return NULL;
    }
    return c;
}

static void st__surf_drop(st_ctx *c)
{
    if (c->surf_rt) {
        ID2D1RenderTarget_Release(c->surf_rt);
        c->surf_rt = NULL;
    }
    if (c->surf) {
        IWICBitmap_Release(c->surf);
        c->surf = NULL;
    }
    c->surf_w = c->surf_h = 0;
}

void st_ctx_destroy(st_ctx *c)
{
    if (!c)
        return;
    st__surf_drop(c);
    if (c->wic)
        IWICImagingFactory_Release(c->wic);
    if (c->d2d)
        ID2D1Factory_Release(c->d2d);
    if (c->dw)
        IDWriteFactory_Release(c->dw);
    free(c);
}

void st_ctx_set_scale(st_ctx *c, float scale)
{
    if (scale <= 0.0f)
        scale = 1.0f;
    c->scale = scale;
}

float st_ctx_scale(const st_ctx *c)
{
    return c->scale;
}

/* Grow the raster surface to at least (w, h) pixels. Rounded up in 256-pixel
 * steps so a transcript of slightly-varying line widths reallocates rarely. */
static int st__surf_ensure(st_ctx *c, int w, int h)
{
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (c->surf && w <= c->surf_w && h <= c->surf_h)
        return 1;
    st__surf_drop(c);
    w = (w + 255) & ~255;
    h = (h + 255) & ~255;

    if (FAILED(IWICImagingFactory_CreateBitmap(c->wic, (UINT)w, (UINT)h,
                                               &GUID_WICPixelFormat32bppPBGRA,
                                               WICBitmapCacheOnDemand,
                                               &c->surf)))
        return 0;
    D2D1_RENDER_TARGET_PROPERTIES p;
    memset(&p, 0, sizeof p);
    p.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
    p.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
    p.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    if (FAILED(ID2D1Factory_CreateWicBitmapRenderTarget(c->d2d, c->surf, &p,
                                                        &c->surf_rt))) {
        st__surf_drop(c);
        return 0;
    }
    /* Grayscale is the client's antialias mode everywhere; on a transparent
     * surface it is also the only correct one. */
    ID2D1RenderTarget_SetTextAntialiasMode(c->surf_rt,
                                           D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    c->surf_w = w;
    c->surf_h = h;
    return 1;
}

/* ---- formats ------------------------------------------------------------ */

st_format *st_format_create(st_ctx *c, const st_format_desc *d)
{
    st_format *f = calloc(1, sizeof *f);
    WCHAR *fam = d->family ? st__wide(d->family, strlen(d->family)) : NULL;
    if (!f || !fam) {
        free(f);
        free(fam);
        return NULL;
    }
    f->ctx = c;
    f->size = d->size;

    HRESULT hr = IDWriteFactory_CreateTextFormat(
        c->dw, fam, NULL,
        (DWRITE_FONT_WEIGHT)(d->weight ? d->weight : 400),
        d->italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, d->size, L"en-us", &f->fmt);
    free(fam);
    if (FAILED(hr)) {
        free(f);
        return NULL;
    }

    IDWriteTextFormat_SetWordWrapping(f->fmt, d->wrap
                                                  ? DWRITE_WORD_WRAPPING_WRAP
                                                  : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (d->line_height > 0.0f)
        IDWriteTextFormat_SetLineSpacing(f->fmt,
                                         DWRITE_LINE_SPACING_METHOD_UNIFORM,
                                         d->line_height, d->baseline);
    if (!d->wrap && d->trimming == ST_TRIM_ELLIPSIS) {
        DWRITE_TRIMMING trim = { DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0 };
        IDWriteInlineObject *sign = NULL;
        if (SUCCEEDED(IDWriteFactory_CreateEllipsisTrimmingSign(c->dw, f->fmt,
                                                                &sign))) {
            IDWriteTextFormat_SetTrimming(f->fmt, &trim, sign);
            IDWriteInlineObject_Release(sign);
        }
    }
    return f;
}

void st_format_destroy(st_format *f)
{
    if (!f)
        return;
    IDWriteTextFormat_Release(f->fmt);
    free(f);
}

/* ---- layouts ------------------------------------------------------------ */

st_layout *st_layout_create(st_ctx *c, st_format *fmt, const char *utf8,
                            size_t len, float max_w, float max_h, int align)
{
    st_layout *l = calloc(1, sizeof *l);
    if (!l)
        return NULL;
    l->ctx = c;

    uint16_t *w16 = NULL;
    int units = 0;
    if (!st__map_build(utf8, len, &l->map, &w16, &units)) {
        free(l);
        return NULL;
    }
    HRESULT hr = IDWriteFactory_CreateTextLayout(c->dw, (const WCHAR *)w16,
                                                 (UINT32)units, fmt->fmt,
                                                 max_w, max_h, &l->lay);
    free(w16);
    if (FAILED(hr)) {
        st__map_free(&l->map);
        free(l);
        return NULL;
    }
    IDWriteTextLayout_SetTextAlignment(
        l->lay, align == ST_ALIGN_CENTER   ? DWRITE_TEXT_ALIGNMENT_CENTER
                : align == ST_ALIGN_RIGHT ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                          : DWRITE_TEXT_ALIGNMENT_LEADING);
    return l;
}

void st_layout_destroy(st_layout *l)
{
    if (!l)
        return;
    IDWriteTextLayout_Release(l->lay);
    st__map_free(&l->map);
    free(l);
}

/* ---- range styling ------------------------------------------------------ */

void st_range_color(st_layout *l, size_t off, size_t len, uint32_t rgb,
                    float alpha)
{
    if (l->ncolors >= ST_MAX_COLOR_RANGES)
        return;
    DWRITE_TEXT_RANGE r = st__range(l, off, len);
    if (!r.length)
        return;
    l->colors[l->ncolors].off = (int)r.startPosition;
    l->colors[l->ncolors].len = (int)r.length;
    l->colors[l->ncolors].rgb = rgb;
    l->colors[l->ncolors].alpha = alpha;
    l->ncolors++;
}

void st_range_weight(st_layout *l, size_t off, size_t len, int weight)
{
    IDWriteTextLayout_SetFontWeight(l->lay, (DWRITE_FONT_WEIGHT)weight,
                                    st__range(l, off, len));
}

void st_range_italic(st_layout *l, size_t off, size_t len, bool on)
{
    IDWriteTextLayout_SetFontStyle(l->lay, on ? DWRITE_FONT_STYLE_ITALIC
                                              : DWRITE_FONT_STYLE_NORMAL,
                                   st__range(l, off, len));
}

void st_range_underline(st_layout *l, size_t off, size_t len, bool on)
{
    IDWriteTextLayout_SetUnderline(l->lay, on ? TRUE : FALSE,
                                   st__range(l, off, len));
}

void st_range_strike(st_layout *l, size_t off, size_t len, bool on)
{
    IDWriteTextLayout_SetStrikethrough(l->lay, on ? TRUE : FALSE,
                                       st__range(l, off, len));
}

void st_range_family(st_layout *l, size_t off, size_t len, const char *family)
{
    WCHAR *fam = st__wide(family, strlen(family));
    if (!fam)
        return;
    IDWriteTextLayout_SetFontFamilyName(l->lay, fam, st__range(l, off, len));
    free(fam);
}

void st_range_size(st_layout *l, size_t off, size_t len, float size)
{
    IDWriteTextLayout_SetFontSize(l->lay, size, st__range(l, off, len));
}

void st_range_hide(st_layout *l, size_t off, size_t len)
{
    /* Near-zero rather than zero: DirectWrite rejects a 0 size, and 0.1 DIP
     * is invisible at any real scale. The bytes keep their positions, which
     * is the point — richtext offsets stay valid. */
    IDWriteTextLayout_SetFontSize(l->lay, 0.1f, st__range(l, off, len));
}

void st_range_box(st_layout *l, size_t off, size_t len, float w, float h,
                  float baseline, uint32_t box_id)
{
    if (l->nboxes >= ST_MAX_BOXES)
        return;
    DWRITE_TEXT_RANGE r = st__range(l, off, len);
    if (!r.length)
        return;
    IDWriteInlineObject *o = ibox_create(w, h, baseline);
    if (!o)
        return;
    IDWriteTextLayout_SetInlineObject(l->lay, o, r);
    IDWriteInlineObject_Release(o);   /* the layout holds it now */
    l->boxes[l->nboxes].id = box_id;
    l->boxes[l->nboxes].off = (int)r.startPosition;
    l->boxes[l->nboxes].len = (int)r.length;
    l->nboxes++;
}

int st_layout_boxes(const st_layout *l, uint32_t *ids, st_rect *rects, int cap)
{
    for (int i = 0; i < l->nboxes && i < cap; i++) {
        DWRITE_HIT_TEST_METRICS hm[2];
        UINT32 n = 0;
        IDWriteTextLayout_HitTestTextRange(l->lay,
                                           (UINT32)l->boxes[i].off,
                                           (UINT32)l->boxes[i].len,
                                           0.0f, 0.0f, hm, 2, &n);
        ids[i] = l->boxes[i].id;
        if (n >= 1) {
            rects[i].x = hm[0].left;
            rects[i].y = hm[0].top;
            rects[i].w = hm[0].width;
            rects[i].h = hm[0].height;
        } else {
            memset(&rects[i], 0, sizeof rects[i]);
        }
    }
    return l->nboxes < cap ? l->nboxes : cap;
}

/* ---- measurement -------------------------------------------------------- */

void st_layout_metrics(const st_layout *l, st_metrics *out)
{
    DWRITE_TEXT_METRICS tm;
    memset(&tm, 0, sizeof tm);
    IDWriteTextLayout_GetMetrics(l->lay, &tm);
    out->w = tm.width;
    out->h = tm.height;
    out->lines = (int)tm.lineCount;
}

int st_layout_lines(const st_layout *l, st_line *out, int cap)
{
    UINT32 n = 0;
    IDWriteTextLayout_GetLineMetrics(l->lay, NULL, 0, &n);
    if (n == 0)
        return 0;
    DWRITE_LINE_METRICS *lm = malloc(n * sizeof *lm);
    if (!lm)
        return 0;
    IDWriteTextLayout_GetLineMetrics(l->lay, lm, n, &n);

    float y = 0.0f;
    UINT32 pos = 0;
    for (UINT32 i = 0; i < n && (int)i < cap; i++) {
        out[i].y = y;
        out[i].height = lm[i].height;
        out[i].baseline = lm[i].baseline;
        int b0 = st__w2b(&l->map, (int)pos);
        int b1 = st__w2b(&l->map, (int)(pos + lm[i].length));
        out[i].off = (size_t)b0;
        out[i].len = (size_t)(b1 - b0);
        y += lm[i].height;
        pos += lm[i].length;
    }
    free(lm);
    return (int)n < cap ? (int)n : cap;
}

float st_text_width(st_ctx *c, st_format *f, const char *utf8, size_t len)
{
    st_map m;
    uint16_t *w16 = NULL;
    int units = 0;
    if (!st__map_build(utf8, len, &m, &w16, &units))
        return 0.0f;
    st__map_free(&m);

    IDWriteTextLayout *lay = NULL;
    float w = 0.0f;
    if (SUCCEEDED(IDWriteFactory_CreateTextLayout(c->dw, (const WCHAR *)w16,
                                                  (UINT32)units, f->fmt,
                                                  1e6f, 1e6f, &lay))) {
        DWRITE_TEXT_METRICS tm;
        if (SUCCEEDED(IDWriteTextLayout_GetMetrics(lay, &tm)))
            w = tm.width;
        IDWriteTextLayout_Release(lay);
    }
    free(w16);
    return w;
}

float st_line_height(st_ctx *c, st_format *f)
{
    IDWriteTextLayout *lay = NULL;
    float h = f->size * 1.35f;   /* fallback only */
    if (SUCCEEDED(IDWriteFactory_CreateTextLayout(c->dw, L"Ag", 2, f->fmt,
                                                  1e6f, 1e6f, &lay))) {
        DWRITE_TEXT_METRICS tm;
        if (SUCCEEDED(IDWriteTextLayout_GetMetrics(lay, &tm)))
            h = tm.height;
        IDWriteTextLayout_Release(lay);
    }
    return h;
}

/* ---- hit-testing -------------------------------------------------------- */

size_t st_hit_point(const st_layout *l, float x, float y, bool *inside,
                    bool *trailing)
{
    BOOL trail = FALSE, in = FALSE;
    DWRITE_HIT_TEST_METRICS hm;
    IDWriteTextLayout_HitTestPoint(l->lay, x, y, &trail, &in, &hm);
    if (inside)
        *inside = in ? true : false;
    if (trailing)
        *trailing = trail ? true : false;
    return (size_t)st__w2b(&l->map, (int)hm.textPosition);
}

st_rect st_hit_pos(const st_layout *l, size_t off, bool trailing)
{
    FLOAT x = 0, y = 0;
    DWRITE_HIT_TEST_METRICS hm;
    memset(&hm, 0, sizeof hm);
    IDWriteTextLayout_HitTestTextPosition(l->lay,
                                          (UINT32)st__b2w(&l->map, off),
                                          trailing ? TRUE : FALSE, &x, &y, &hm);
    st_rect r = { x, hm.top, hm.width, hm.height };
    return r;
}

int st_hit_range(const st_layout *l, size_t off, size_t len, st_rect *out,
                 int cap)
{
    DWRITE_TEXT_RANGE r = st__range(l, off, len);
    UINT32 n = 0;
    IDWriteTextLayout_HitTestTextRange(l->lay, r.startPosition, r.length,
                                       0.0f, 0.0f, NULL, 0, &n);
    if (n == 0)
        return 0;
    DWRITE_HIT_TEST_METRICS *hm = malloc(n * sizeof *hm);
    if (!hm)
        return 0;
    IDWriteTextLayout_HitTestTextRange(l->lay, r.startPosition, r.length,
                                       0.0f, 0.0f, hm, n, &n);
    for (UINT32 i = 0; i < n && (int)i < cap; i++) {
        out[i].x = hm[i].left;
        out[i].y = hm[i].top;
        out[i].w = hm[i].width;
        out[i].h = hm[i].height;
    }
    free(hm);
    return (int)n;
}

/* ---- drawing ------------------------------------------------------------ */

/* Re-apply the layout's color ranges as drawing effects made from `rt`.
 * Returns the brushes so the caller can release them after EndDraw; the
 * layout keeps the effect objects alive between draws, but they are replaced
 * wholesale on the next draw, so a stale target's brush is never used. */
static int st__apply_colors(st_layout *l, ID2D1RenderTarget *rt,
                            ID2D1SolidColorBrush **out)
{
    int n = 0;
    for (int i = 0; i < l->ncolors; i++) {
        D2D1_COLOR_F col = st__col(l->colors[i].rgb, l->colors[i].alpha);
        D2D1_BRUSH_PROPERTIES bp = { 1.0f,
            {{{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f }}} };
        ID2D1SolidColorBrush *b = NULL;
        if (FAILED(ID2D1RenderTarget_CreateSolidColorBrush(rt, &col, &bp, &b)))
            continue;
        DWRITE_TEXT_RANGE r = { (UINT32)l->colors[i].off,
                                (UINT32)l->colors[i].len };
        IDWriteTextLayout_SetDrawingEffect(l->lay, (IUnknown *)b, r);
        out[n++] = b;
    }
    return n;
}

static void st__draw_on(st_layout *l, ID2D1RenderTarget *rt, float x, float y,
                        uint32_t rgb, float alpha)
{
    ID2D1SolidColorBrush *effects[ST_MAX_COLOR_RANGES];
    int neff = st__apply_colors(l, rt, effects);

    D2D1_COLOR_F col = st__col(rgb, alpha);
    D2D1_BRUSH_PROPERTIES bp = { 1.0f,
        {{{ 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f }}} };
    ID2D1SolidColorBrush *brush = NULL;
    if (SUCCEEDED(ID2D1RenderTarget_CreateSolidColorBrush(rt, &col, &bp,
                                                          &brush))) {
        D2D1_POINT_2F org = { x, y };
        ID2D1RenderTarget_DrawTextLayout(rt, org, l->lay, (ID2D1Brush *)brush,
                                         D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        ID2D1SolidColorBrush_Release(brush);
    }
    for (int i = 0; i < neff; i++)
        ID2D1SolidColorBrush_Release(effects[i]);
}

void st_draw(st_ctx *c, st_layout *l, float x, float y, uint32_t rgb,
             float alpha)
{
    if (!c->sink.blit)
        return;

    /* Raster the INK, not the layout box: an aligned layout positions its ink
     * inside the box (centered text sits at maxw/2), and a surface sized to
     * the ink would otherwise capture the box's empty left edge. The ink
     * offset travels to the sink through dip_x/dip_y, so the caller places
     * the texture exactly where the layout put the glyphs. */
    DWRITE_TEXT_METRICS tm;
    memset(&tm, 0, sizeof tm);
    IDWriteTextLayout_GetMetrics(l->lay, &tm);
    int pw = (int)ceilf((tm.width + 2.0f) * c->scale);
    int ph = (int)ceilf((tm.height + 2.0f) * c->scale);
    if (!st__surf_ensure(c, pw, ph))
        return;

    ID2D1RenderTarget_BeginDraw(c->surf_rt);
    D2D1_MATRIX_3X2_F sc = {{{ c->scale, 0.0f, 0.0f, c->scale,
                               -tm.left * c->scale, -tm.top * c->scale }}};
    ID2D1RenderTarget_SetTransform(c->surf_rt, &sc);
    D2D1_COLOR_F clear = { 0, 0, 0, 0 };
    ID2D1RenderTarget_Clear(c->surf_rt, &clear);
    st__draw_on(l, c->surf_rt, 0.0f, 0.0f, rgb, alpha);
    if (FAILED(ID2D1RenderTarget_EndDraw(c->surf_rt, NULL, NULL)))
        return;

    WICRect wr = { 0, 0, pw, ph };
    IWICBitmapLock *lock = NULL;
    if (SUCCEEDED(IWICBitmap_Lock(c->surf, &wr, WICBitmapLockRead, &lock))) {
        UINT stride = 0, sz = 0;
        BYTE *ptr = NULL;
        IWICBitmapLock_GetStride(lock, &stride);
        IWICBitmapLock_GetDataPointer(lock, &sz, &ptr);
        if (ptr)
            c->sink.blit(c->sink.user, ptr, (int)stride, pw, ph,
                         x + tm.left, y + tm.top);
        IWICBitmapLock_Release(lock);
    }
}

int st_dwrite_family_present(st_ctx *c, const char *family)
{
    WCHAR *fam = st__wide(family, strlen(family));
    if (!fam)
        return 0;
    IDWriteFontCollection *fc = NULL;
    int ok = 0;
    if (SUCCEEDED(IDWriteFactory_GetSystemFontCollection(c->dw, &fc, FALSE)) && fc) {
        UINT32 ix = 0;
        BOOL found = FALSE;
        IDWriteFontCollection_FindFamilyName(fc, fam, &ix, &found);
        IDWriteFontCollection_Release(fc);
        ok = found ? 1 : 0;
    }
    free(fam);
    return ok;
}

void st_dwrite_draw_rt(st_ctx *c, st_layout *l, void *d2d_render_target,
                       float x, float y, uint32_t rgb, float alpha)
{
    (void)c;
    st__draw_on(l, (ID2D1RenderTarget *)d2d_render_target, x, y, rgb, alpha);
}

#endif /* _WIN32 */
