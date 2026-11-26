/*
 * sdltext's test program: one program for every backend (sdltext.h), its exit
 * code its failure count.
 *
 * The backend is chosen at compile time: DirectWrite on Windows
 * (build/sdltext_test.exe, mk/windows.mk), FreeType, HarfBuzz and fontconfig
 * elsewhere (build/sdltext_test, mk/tests.mk). Every assertion runs on every
 * backend. Only what depends on the font itself differs, and it is the block
 * below: the families the system is expected to have (DejaVu Sans on Linux,
 * Segoe UI on Windows), the bounds of a measured width and line height in
 * them, and a code point the primary family lacks. Everything else is
 * asserted as a relation the API promises whatever the font: carets advance,
 * lines are contiguous, a bold range is wider than a regular one, uniform
 * spacing is the same at every weight, a hidden range takes no room.
 *
 * Pixels are read from a memory sink, as the client's texture upload would
 * receive them: premultiplied BGRA, placed in DIPs.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sdltext.h"

#ifdef _WIN32
#include "st_dwrite.h"
#define BACKEND_NAME  "DirectWrite"
#define FAMILY        "Segoe UI"
#define ALT_FAMILY    "Georgia"
/* "Hello world" at 16 DIPs: about 4.9 em of advances in Segoe UI. */
#define HELLO_W_LO    65.0f
#define HELLO_W_HI    92.0f
/* Segoe UI's natural line is about 1.33 em. */
#define LINE_H_LO     19.0f
#define LINE_H_HI     24.0f
/* Two ideographs Segoe UI has no glyph for; the system's fallback does. */
#define MISSING_A     "\xE4\xB8\xAD"          /* U+4E2D */
#define MISSING_B     "\xE5\x9B\xBD"          /* U+56FD */
static st_ctx *backend(const st_sink *sink) { return st_dwrite_create(sink); }
#else
#include "st_ft.h"
#define BACKEND_NAME  "FreeType"
#define FAMILY        "DejaVu Sans"
#define ALT_FAMILY    "DejaVu Serif"
/* "Hello world" at 16 DIPs: about 5.6 em of advances in DejaVu Sans. */
#define HELLO_W_LO    80.0f
#define HELLO_W_HI   100.0f
/* DejaVu Sans's natural line is about 1.16 em. */
#define LINE_H_LO     17.0f
#define LINE_H_HI     21.0f
/* Two mathematical capitals DejaVu Sans lacks and DejaVu Serif has (both
 * from fonts-dejavu-core), so fontconfig's fallback must supply them. */
#define MISSING_A     "\xF0\x9D\x90\x80"      /* U+1D400 */
#define MISSING_B     "\xF0\x9D\x90\x81"      /* U+1D401 */
static st_ctx *backend(const st_sink *sink) { return st_ft_create(sink); }
#endif

static int g_fails, g_checks;

#define CHECK(cond) check_((cond) ? 1 : 0, __LINE__, #cond)
#define NEAR(a, b, tol) check_near_((float)(a), (float)(b), (float)(tol), __LINE__, #a, #b)

static int check_(int ok, int line, const char *what)
{
    g_checks++;
    if (!ok) {
        g_fails++;
        fprintf(stderr, "st_test.c:%d: FAIL: %s\n", line, what);
    }
    return ok;
}

static int check_near_(float a, float b, float tol, int line, const char *ea, const char *eb)
{
    g_checks++;
    if (fabsf(a - b) <= tol)
        return 1;
    g_fails++;
    fprintf(stderr, "st_test.c:%d: FAIL: %s (%.3f) is not within %.3f of %s (%.3f)\n",
            line, ea, a, tol, eb, b);
    return 0;
}

/* ---- the memory sink -------------------------------------------------------- */

/* The last raster a draw delivered, copied out: the buffer is valid for the
 * call only (sdltext.h). */
typedef struct {
    unsigned char *px;
    int   w, h, calls;
    float dip_x, dip_y;
} capture;

static void sink_blit(void *user, const void *bgra, int stride, int px_w, int px_h,
                      float dip_x, float dip_y)
{
    capture *c = user;
    free(c->px);
    c->px = malloc((size_t)px_w * (size_t)px_h * 4);
    c->w = c->px ? px_w : 0;
    c->h = c->px ? px_h : 0;
    c->dip_x = dip_x;
    c->dip_y = dip_y;
    c->calls++;
    for (int y = 0; c->px && y < px_h; y++)
        memcpy(c->px + (size_t)y * px_w * 4, (const unsigned char *)bgra + (size_t)y * stride,
               (size_t)px_w * 4);
}

static const unsigned char *pix(const capture *c, int x, int y)
{
    return c->px + ((size_t)y * c->w + x) * 4;
}

/* Pixels with any coverage at all. */
static int ink_count(const capture *c)
{
    int n = 0;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++)
            n += pix(c, x, y)[3] > 0;
    return n;
}

/* The ink's bounding box, in DIPs on the drawing surface. 0 when blank. */
static int ink_box(const capture *c, float scale, st_rect *out)
{
    int x0 = c->w, y0 = c->h, x1 = -1, y1 = -1;
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++)
            if (pix(c, x, y)[3] > 0) {
                if (x < x0) x0 = x;
                if (x > x1) x1 = x;
                if (y < y0) y0 = y;
                if (y > y1) y1 = y;
            }
    if (x1 < 0)
        return 0;
    out->x = c->dip_x + x0 / scale;
    out->y = c->dip_y + y0 / scale;
    out->w = (x1 + 1 - x0) / scale;
    out->h = (y1 + 1 - y0) / scale;
    return 1;
}

/* ---- helpers ------------------------------------------------------------------ */

static st_ctx  *g_ctx;
static capture  g_cap;

static st_format *fmt(const char *family, float size, int weight, int wrap,
                      float line_height, float baseline)
{
    st_format_desc d;
    memset(&d, 0, sizeof d);
    d.family = family;
    d.size = size;
    d.weight = weight;
    d.wrap = wrap;
    d.line_height = line_height;
    d.baseline = baseline;
    return st_format_create(g_ctx, &d);
}

static st_layout *lay(st_format *f, const char *s, float max_w)
{
    return st_layout_create(g_ctx, f, s, strlen(s), max_w, 10000.0f, ST_ALIGN_LEFT);
}

static size_t off_of(const char *s, const char *needle)
{
    const char *p = strstr(s, needle);
    return p ? (size_t)(p - s) : 0;
}

static float range_w(const st_layout *l, size_t off, size_t len)
{
    st_rect r[4];
    int n = st_hit_range(l, off, len, r, 4);
    return n >= 1 ? r[0].w : 0.0f;
}

static float caret(const st_layout *l, size_t off)
{
    return st_hit_pos(l, off, false).x;
}

static int draw_ink(st_layout *l, uint32_t rgb, float alpha)
{
    g_cap.calls = 0;
    st_draw(g_ctx, l, 0.0f, 0.0f, rgb, alpha);
    return g_cap.calls == 1 ? ink_count(&g_cap) : -1;
}

/* ---- families --------------------------------------------------------------- */

static void t_families(void)
{
    CHECK(st_family_present(g_ctx, FAMILY));
    CHECK(st_family_present(g_ctx, ALT_FAMILY));
    CHECK(!st_family_present(g_ctx, "No Such Family Qx7"));

    float asc = -1, cap = -1;
    CHECK(st_family_metrics(g_ctx, FAMILY, &asc, &cap) == 1);
    CHECK(asc > 0.6f && asc < 1.3f);
    CHECK(cap > 0.5f && cap < asc);
    CHECK(st_family_metrics(g_ctx, "No Such Family Qx7", &asc, &cap) == 0);
    CHECK(asc == 0.0f && cap == 0.0f);

    st_ctx_set_scale(g_ctx, 2.0f);
    NEAR(st_ctx_scale(g_ctx), 2.0f, 0.0f);
    st_ctx_set_scale(g_ctx, 1.0f);
    NEAR(st_ctx_scale(g_ctx), 1.0f, 0.0f);
}

/* ---- metrics and wrapping ----------------------------------------------------- */

static void t_metrics(void)
{
    st_format *f = fmt(FAMILY, 16.0f, 400, 1, 0, 0);
    st_format *nw = fmt(FAMILY, 16.0f, 400, 0, 0, 0);
    CHECK(f && nw);
    if (!f || !nw) return;

    const char *hello = "Hello world";
    st_layout *l = lay(f, hello, 1000.0f);
    st_metrics m;
    st_layout_metrics(l, &m);
    CHECK(m.lines == 1);
    CHECK(m.w > HELLO_W_LO && m.w < HELLO_W_HI);
    float lh = st_line_height(g_ctx, f);
    CHECK(lh > LINE_H_LO && lh < LINE_H_HI);
    NEAR(m.h, lh, 0.5f);
    NEAR(st_text_width(g_ctx, f, hello, strlen(hello)), m.w, 0.5f);
    st_layout_destroy(l);

    /* An empty layout still has a line: a caret has somewhere to stand. */
    l = lay(f, "", 1000.0f);
    st_layout_metrics(l, &m);
    CHECK(m.lines == 1);
    NEAR(m.w, 0.0f, 0.01f);
    CHECK(m.h > 0.0f);
    st_layout_destroy(l);

    /* Wrapped at word boundaries, every line inside the width, the lines
     * contiguous and covering the text. */
    const char *para = "The quick brown fox jumps over the lazy dog and keeps running far away";
    l = lay(f, para, 120.0f);
    st_layout_metrics(l, &m);
    CHECK(m.lines >= 3);
    CHECK(m.w <= 120.5f);
    st_line ln[32];
    int n = st_layout_lines(l, ln, 32);
    CHECK(n == m.lines);
    size_t at = 0;
    float y = 0, total = 0;
    for (int i = 0; i < n; i++) {
        CHECK(ln[i].off == at);
        CHECK(ln[i].len > 0);
        NEAR(ln[i].y, y, 0.01f);
        NEAR(ln[i].height, ln[0].height, 0.01f);
        if (i > 0) CHECK(para[ln[i].off - 1] == ' ');
        at = ln[i].off + ln[i].len;
        y += ln[i].height;
        total += ln[i].height;
    }
    CHECK(at == strlen(para));
    NEAR(m.h, total, 0.5f);
    st_layout_destroy(l);

    /* The same text unwrapped is one line. */
    l = lay(nw, para, 120.0f);
    st_layout_metrics(l, &m);
    CHECK(m.lines == 1);
    CHECK(m.w > 120.0f);
    st_layout_destroy(l);

    /* A mandatory break wraps whatever the width. */
    l = lay(f, "one\ntwo\nthree", 1000.0f);
    st_layout_metrics(l, &m);
    CHECK(m.lines == 3);
    st_layout_destroy(l);

    /* Trimming: a non-wrapping format cut with an ellipsis stays inside its
     * box, on one line. */
    st_format_desc d;
    memset(&d, 0, sizeof d);
    d.family = FAMILY;
    d.size = 16.0f;
    d.weight = 400;
    d.trimming = ST_TRIM_ELLIPSIS;
    st_format *tr = st_format_create(g_ctx, &d);
    l = lay(tr, para, 90.0f);
    st_layout_metrics(l, &m);
    CHECK(m.lines == 1);
    CHECK(m.w <= 91.0f);
    CHECK(draw_ink(l, 0x000000, 1.0f) > 0);
    st_rect ib;
    if (CHECK(ink_box(&g_cap, 1.0f, &ib)))
        CHECK(ib.x + ib.w <= 92.0f);
    st_layout_destroy(l);
    st_format_destroy(tr);

    /* Alignment is the layout's: centred and right-aligned lines start where
     * the box puts them. */
    float w = st_text_width(g_ctx, f, "Hi", 2);
    l = st_layout_create(g_ctx, f, "Hi", 2, 200.0f, 100.0f, ST_ALIGN_CENTER);
    NEAR(caret(l, 0), (200.0f - w) / 2, 0.75f);
    st_layout_destroy(l);
    l = st_layout_create(g_ctx, f, "Hi", 2, 200.0f, 100.0f, ST_ALIGN_RIGHT);
    NEAR(caret(l, 0), 200.0f - w, 0.75f);
    st_layout_destroy(l);

    st_format_destroy(f);
    st_format_destroy(nw);
}

/* ---- range styling over multibyte text ---------------------------------------- */

static void t_ranges(void)
{
    st_format *f = fmt(FAMILY, 16.0f, 400, 1, 0, 0);
    if (!CHECK(f != NULL)) return;
    const char *t = "h\xC3\xA9llo w\xC3\xB6rld \xE2\x9C\x93 end";   /* héllo wörld ✓ end */
    size_t len = strlen(t);
    size_t w0 = off_of(t, "w\xC3\xB6rld"), wn = 6;               /* "wörld": six bytes */
    size_t h0 = 0, hn = 6;                                         /* "héllo" */

    st_layout *plain = lay(f, t, 1000.0f);
    st_metrics pm;
    st_layout_metrics(plain, &pm);
    float plain_w = range_w(plain, w0, wn);
    CHECK(plain_w > 0.0f);

    /* Weight: the bold range is wider, and only it moves what follows. */
    st_layout *l = lay(f, t, 1000.0f);
    st_range_weight(l, w0, wn, 700);
    CHECK(range_w(l, w0, wn) > plain_w + 0.5f);
    NEAR(caret(l, w0), caret(plain, w0), 0.01f);
    st_layout_destroy(l);

    /* Size: a larger range is wider and makes its line taller. */
    l = lay(f, t, 1000.0f);
    st_range_size(l, w0, wn, 32.0f);
    st_metrics m;
    st_layout_metrics(l, &m);
    CHECK(range_w(l, w0, wn) > plain_w * 1.6f);
    CHECK(m.h > pm.h + 4.0f);
    st_layout_destroy(l);

    /* Family: another face measures the same bytes differently. */
    l = lay(f, t, 1000.0f);
    st_range_family(l, w0, wn, ALT_FAMILY);
    CHECK(fabsf(range_w(l, w0, wn) - plain_w) > 0.3f);
    st_layout_destroy(l);

    /* A range that starts inside a character styles the whole character; one
     * past the end is clamped. Byte 2 is the second byte of "é". */
    l = lay(f, t, 1000.0f);
    st_range_weight(l, 2, 1, 700);
    st_range_color(l, len + 10, 4, 0xFF0000, 1.0f);
    st_range_weight(l, len - 1, 1000, 700);
    st_layout_metrics(l, &m);
    CHECK(m.lines == 1);
    st_rect r1[2], r2[2];
    CHECK(st_hit_range(l, 2, 1, r1, 2) == 1);
    CHECK(st_hit_range(l, 1, 2, r2, 2) == 1);
    NEAR(r1[0].x, r2[0].x, 0.01f);
    NEAR(r1[0].w, r2[0].w, 0.01f);
    st_layout_destroy(l);

    /* Hidden bytes keep their offsets and take no room: "**bold** text" with
     * its delimiters hidden measures as "bold text". */
    const char *md = "**bold** text";
    l = lay(f, md, 1000.0f);
    st_range_hide(l, 0, 2);
    st_range_hide(l, 6, 2);
    st_layout_metrics(l, &m);
    NEAR(m.w, st_text_width(g_ctx, f, "bold text", 9), 1.0f);
    NEAR(caret(l, 2), 0.0f, 0.5f);
    CHECK(caret(l, 10) > caret(l, 3));
    st_layout_destroy(l);

    /* Italic, underline and strike change the ink, not the line. */
    int base_ink = draw_ink(plain, 0x000000, 1.0f);
    unsigned char *base_px = NULL;
    int bw = g_cap.w, bh = g_cap.h;
    if (g_cap.px) {
        base_px = malloc((size_t)bw * bh * 4);
        if (base_px) memcpy(base_px, g_cap.px, (size_t)bw * bh * 4);
    }
    CHECK(base_ink > 0);

    l = lay(f, t, 1000.0f);
    st_range_italic(l, h0, hn, true);
    st_layout_metrics(l, &m);
    CHECK(m.lines == 1);
    CHECK(draw_ink(l, 0x000000, 1.0f) > 0);
    CHECK(base_px && (g_cap.w != bw || g_cap.h != bh ||
                      memcmp(g_cap.px, base_px, (size_t)bw * bh * 4) != 0));
    st_layout_destroy(l);
    free(base_px);

    l = lay(f, t, 1000.0f);
    st_range_underline(l, 0, len, true);
    CHECK(draw_ink(l, 0x000000, 1.0f) > base_ink + 20);
    st_layout_destroy(l);

    l = lay(f, t, 1000.0f);
    st_range_strike(l, 0, len, true);
    CHECK(draw_ink(l, 0x000000, 1.0f) > base_ink + 20);
    st_layout_destroy(l);

    /* Turning a style off again over part of its range. */
    l = lay(f, t, 1000.0f);
    st_range_underline(l, 0, len, true);
    st_range_underline(l, 0, len, false);
    int off_ink = draw_ink(l, 0x000000, 1.0f);
    CHECK(off_ink >= base_ink - 2 && off_ink <= base_ink + 2);
    st_layout_destroy(l);

    st_layout_destroy(plain);
    st_format_destroy(f);
}

/* ---- the three hit tests ------------------------------------------------------ */

static void t_hits(void)
{
    st_format *f = fmt(FAMILY, 16.0f, 400, 1, 0, 0);
    if (!CHECK(f != NULL)) return;
    const char *s = "Hello";
    st_layout *l = lay(f, s, 1000.0f);
    st_metrics m;
    st_layout_metrics(l, &m);

    /* Offset -> caret: leading edges advance; the last character's trailing
     * edge is the text's width. */
    float x[6];
    for (int i = 0; i < 5; i++) x[i] = caret(l, (size_t)i);
    x[5] = st_hit_pos(l, 4, true).x;
    NEAR(x[0], 0.0f, 0.01f);
    for (int i = 0; i < 5; i++) CHECK(x[i + 1] > x[i]);
    NEAR(x[5], m.w, 0.5f);
    st_rect cr = st_hit_pos(l, 1, false);
    NEAR(cr.w, x[2] - x[1], 0.5f);
    NEAR(cr.y, 0.0f, 0.01f);
    NEAR(cr.h, m.h, 0.5f);

    /* Point -> offset: either half of a glyph, and outside the text. */
    float mid_y = m.h / 2;
    bool in = false, tr = true;
    CHECK(st_hit_point(l, (x[1] + x[2]) / 2 - 1.0f, mid_y, &in, &tr) == 1);
    CHECK(in && !tr);
    CHECK(st_hit_point(l, (x[1] + x[2]) / 2 + 1.0f, mid_y, &in, &tr) == 1);
    CHECK(in && tr);
    CHECK(st_hit_point(l, m.w + 40.0f, mid_y, &in, &tr) == 4);
    CHECK(!in && tr);
    st_hit_point(l, 2.0f, m.h + 30.0f, &in, &tr);
    CHECK(!in);
    st_layout_destroy(l);

    /* A point on a two-byte character answers the offset it starts at. */
    const char *mb = "a\xC3\xA9z";
    l = lay(f, mb, 1000.0f);
    float e0 = caret(l, 1), e1 = caret(l, 3);
    CHECK(e1 > e0);
    CHECK(st_hit_point(l, (e0 + e1) / 2 - 0.5f, 5.0f, &in, &tr) == 1);
    st_layout_destroy(l);

    /* Range -> rectangles: one per line touched, the count reported even
     * past the cap. */
    l = lay(f, "Hello", 1000.0f);
    st_rect rr[4];
    CHECK(st_hit_range(l, 1, 3, rr, 4) == 1);
    NEAR(rr[0].x, x[1], 0.5f);
    NEAR(rr[0].w, x[4] - x[1], 0.5f);
    CHECK(st_hit_range(l, 2, 0, rr, 4) == 0);
    st_layout_destroy(l);

    const char *para = "alpha beta gamma delta epsilon zeta eta theta";
    l = lay(f, para, 100.0f);
    st_line ln[16];
    int n = st_layout_lines(l, ln, 16);
    CHECK(n >= 2);
    if (n >= 2) {
        bool in2 = false;
        CHECK(st_hit_point(l, 0.5f, ln[1].y + ln[1].height / 2, &in2, NULL) == ln[1].off);
        CHECK(in2);
        st_rect p = st_hit_pos(l, ln[1].off, false);
        NEAR(p.y, ln[1].y, 0.5f);
        NEAR(p.x, 0.0f, 0.5f);
        size_t a = ln[0].off + 1, b = ln[1].off + 2;
        CHECK(st_hit_range(l, a, b - a, rr, 4) == 2);
        CHECK(rr[1].y > rr[0].y);
        CHECK(st_hit_range(l, a, b - a, rr, 1) == 2);
    }
    st_layout_destroy(l);
    st_format_destroy(f);
}

/* ---- inline boxes ------------------------------------------------------------- */

static void t_boxes(void)
{
    st_format *f = fmt(FAMILY, 16.0f, 400, 1, 0, 0);
    if (!CHECK(f != NULL)) return;
    const char *s = "ab :x: cd :y: ef";
    size_t b1 = off_of(s, ":x:"), b2 = off_of(s, ":y:");
    st_layout *l = lay(f, s, 1000.0f);
    CHECK(st_range_box(l, b1, 3, 20.0f, 18.0f, 14.0f, 7) == 1);
    CHECK(st_range_box(l, b2, 3, 30.0f, 40.0f, 32.0f, 9) == 1);
    CHECK(st_range_box(l, strlen(s) + 5, 2, 10.0f, 10.0f, 8.0f, 11) == 0);

    uint32_t ids[4];
    st_rect r[4];
    int n = st_layout_boxes(l, ids, r, 4);
    CHECK(n == 2);
    int i7 = n >= 1 && ids[0] == 7 ? 0 : 1, i9 = 1 - i7;
    if (n == 2) {
        CHECK(ids[i7] == 7 && ids[i9] == 9);
        NEAR(r[i7].w, 20.0f, 0.5f);
        NEAR(r[i9].w, 30.0f, 0.5f);
        CHECK(r[i7].h > 0.0f && r[i9].h > 0.0f);
        /* The box sits where its bytes are, and the text resumes after it. */
        NEAR(r[i7].x, caret(l, b1), 0.5f);
        NEAR(caret(l, b1 + 3), r[i7].x + 20.0f, 0.5f);
        CHECK(r[i9].x > r[i7].x + 20.0f);
    }
    CHECK(st_layout_boxes(l, ids, r, 1) == 1);

    /* The tall box makes its line tall enough to hold it. */
    st_metrics m;
    st_layout_metrics(l, &m);
    CHECK(m.lines == 1);
    CHECK(m.h >= 40.0f - 0.5f);

    /* A box draws nothing: the caller composites its image there. */
    CHECK(draw_ink(l, 0x000000, 1.0f) > 0);
    st_layout_destroy(l);
    st_format_destroy(f);
}

/* ---- line metrics ------------------------------------------------------------- */

static void t_lines(void)
{
    st_format *f = fmt(FAMILY, 16.0f, 400, 1, 0, 0);
    if (!CHECK(f != NULL)) return;
    st_layout *l = lay(f, "a\nbb\nccc", 1000.0f);
    st_line ln[8];
    int n = st_layout_lines(l, ln, 8);
    CHECK(n == 3);
    if (n == 3) {
        CHECK(ln[0].off == 0 && ln[0].len == 2);
        CHECK(ln[1].off == 2 && ln[1].len == 3);
        CHECK(ln[2].off == 5 && ln[2].len == 3);
        for (int i = 0; i < 3; i++) {
            NEAR(ln[i].height, st_line_height(g_ctx, f), 0.5f);
            CHECK(ln[i].baseline > 0.0f && ln[i].baseline < ln[i].height);
            if (i) NEAR(ln[i].y, ln[i - 1].y + ln[i - 1].height, 0.01f);
        }
    }
    CHECK(st_layout_lines(l, ln, 2) == 2);
    st_layout_destroy(l);

    /* Text ending in a newline ends in an empty line, where the caret after
     * the newline stands. */
    l = lay(f, "ab\n", 1000.0f);
    n = st_layout_lines(l, ln, 8);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(ln[1].off == 3 && ln[1].len == 0);
        st_rect end = st_hit_pos(l, 3, false);
        NEAR(end.y, ln[1].y, 0.5f);
        NEAR(end.x, 0.0f, 0.5f);
    }
    st_layout_destroy(l);
    st_format_destroy(f);
}

/* ---- uniform line height across weights ----------------------------------------- */

static void uniform_lines(st_layout *l)
{
    st_line ln[16];
    st_metrics m;
    st_layout_metrics(l, &m);
    int n = st_layout_lines(l, ln, 16);
    CHECK(n >= 2);
    for (int i = 0; i < n; i++) {
        NEAR(ln[i].height, 24.0f, 0.01f);
        NEAR(ln[i].baseline, 18.0f, 0.01f);
        NEAR(ln[i].y, 24.0f * i, 0.01f);
    }
    NEAR(m.h, 24.0f * m.lines, 0.01f);
}

static void t_uniform(void)
{
    const char *s = "Regular words and bold words share one line box";
    size_t len = strlen(s);
    st_format *u = fmt(FAMILY, 16.0f, 400, 1, 24.0f, 18.0f);
    st_format *ub = fmt(FAMILY, 16.0f, 700, 1, 24.0f, 18.0f);
    if (!CHECK(u && ub)) return;
    NEAR(st_line_height(g_ctx, u), 24.0f, 0.01f);
    NEAR(st_line_height(g_ctx, ub), 24.0f, 0.01f);

    st_layout *l = lay(u, s, 120.0f);
    uniform_lines(l);
    st_layout_destroy(l);

    l = lay(ub, s, 120.0f);
    uniform_lines(l);
    st_layout_destroy(l);

    l = lay(u, s, 120.0f);
    st_range_weight(l, 0, len / 2, 700);
    st_range_weight(l, len / 2, len - len / 2, 300);
    uniform_lines(l);
    st_layout_destroy(l);

    l = lay(u, s, 120.0f);
    st_range_size(l, 8, 5, 20.0f);
    st_range_italic(l, 0, 7, true);
    uniform_lines(l);
    st_layout_destroy(l);

    st_format_destroy(u);
    st_format_destroy(ub);
}

/* ---- a right-to-left run ----------------------------------------------------------- */

static void t_rtl(void)
{
    st_format *f = fmt(FAMILY, 20.0f, 400, 1, 0, 0);
    if (!CHECK(f != NULL)) return;
    /* A Hebrew word inside left-to-right text: shin with a qamats (one
     * cluster of four bytes), then lamed, vav and final mem. */
    const char *s = "abc \xD7\xA9\xD6\xB8\xD7\x9C\xD7\x95\xD7\x9D def";
    size_t h = 4, len = 10;
    const size_t starts[4] = { h, h + 4, h + 6, h + 8 };
    st_layout *l = lay(f, s, 1000.0f);
    st_metrics m;
    st_layout_metrics(l, &m);
    CHECK(m.lines == 1);

    /* Read right to left: each cluster's leading edge is left of the one
     * before it, and the run sits between the two Latin words. */
    float cx[4];
    for (int i = 0; i < 4; i++) cx[i] = caret(l, starts[i]);
    for (int i = 1; i < 4; i++) CHECK(cx[i] < cx[i - 1]);
    st_rect run[2];
    CHECK(st_hit_range(l, h, len, run, 2) == 1);
    CHECK(run[0].x >= caret(l, 3) - 0.5f);
    CHECK(run[0].x + run[0].w <= caret(l, h + len + 1) + 0.5f);
    NEAR(run[0].x + run[0].w, cx[0], 0.5f);

    /* Every point across the run answers a cluster's start: never the mark,
     * never the middle of a character. */
    int bad = 0;
    for (float x = run[0].x + 0.25f; x < run[0].x + run[0].w; x += 0.5f) {
        size_t o = st_hit_point(l, x, m.h / 2, NULL, NULL);
        int ok = 0;
        for (int i = 0; i < 4; i++) ok |= o == starts[i];
        bad += !ok;
    }
    CHECK(bad == 0);

    /* The first cluster: its right half is its leading side, its left half
     * the trailing one. */
    bool in = false, tr = true;
    CHECK(st_hit_point(l, cx[0] - 1.0f, m.h / 2, &in, &tr) == h);
    CHECK(in && !tr);
    CHECK(st_hit_point(l, cx[1] + 1.0f, m.h / 2, &in, &tr) == h);
    CHECK(in && tr);
    st_layout_destroy(l);
    st_format_destroy(f);
}

/* ---- a code point the primary family lacks --------------------------------------- */

static void t_fallback(void)
{
    st_format *f = fmt(FAMILY, 32.0f, 400, 1, 0, 0);
    if (!CHECK(f != NULL)) return;

    /* Each draws as its own glyph from the fallback: two missing characters
     * drawn as the same "no glyph" box would give the same pixels. */
    st_layout *a = lay(f, MISSING_A, 1000.0f);
    st_layout *b = lay(f, MISSING_B, 1000.0f);
    st_metrics ma, mb;
    st_layout_metrics(a, &ma);
    st_layout_metrics(b, &mb);
    CHECK(ma.w > 4.0f && mb.w > 4.0f);
    int ia = draw_ink(a, 0x000000, 1.0f);
    capture ca = g_cap;
    g_cap.px = NULL;
    int ib = draw_ink(b, 0x000000, 1.0f);
    CHECK(ia > 20 && ib > 20);
    CHECK(ca.px && g_cap.px && (ca.w != g_cap.w || ca.h != g_cap.h ||
                                memcmp(ca.px, g_cap.px, (size_t)ca.w * ca.h * 4) != 0));
    free(ca.px);
    st_layout_destroy(a);
    st_layout_destroy(b);

    /* Between two characters the family has, it takes room and its bytes
     * hit-test as one character. */
    char mixed[16];
    snprintf(mixed, sizeof mixed, "A%sB", MISSING_A);
    size_t mlen = strlen(MISSING_A);
    st_layout *l = lay(f, mixed, 1000.0f);
    float x0 = caret(l, 1), x1 = caret(l, 1 + mlen);
    CHECK(x1 - x0 > 4.0f);
    CHECK(st_hit_point(l, (x0 + x1) / 2, 10.0f, NULL, NULL) == 1);
    NEAR(caret(l, 2), x0, 0.01f);
    st_layout_destroy(l);

    /* A family the system does not have still draws, from its fallback. */
    st_format *g = fmt("No Such Family Qx7", 16.0f, 400, 1, 0, 0);
    if (CHECK(g != NULL)) {
        l = lay(g, "Hello", 1000.0f);
        st_metrics m;
        st_layout_metrics(l, &m);
        CHECK(m.w > 10.0f);
        CHECK(draw_ink(l, 0x000000, 1.0f) > 20);
        st_layout_destroy(l);
        st_format_destroy(g);
    }
    st_format_destroy(f);
}

/* ---- pixels in a memory sink --------------------------------------------------------- */

static void t_pixels(void)
{
    st_format *f = fmt(FAMILY, 20.0f, 400, 1, 0, 0);
    if (!CHECK(f != NULL)) return;
    st_layout *l = lay(f, "Hello", 1000.0f);
    st_metrics m;
    st_layout_metrics(l, &m);

    g_cap.calls = 0;
    st_draw(g_ctx, l, 10.0f, 20.0f, 0xFF0000, 1.0f);
    CHECK(g_cap.calls == 1);
    CHECK(g_cap.w > 0 && g_cap.h > 0);
    int ink = ink_count(&g_cap), premul = 1, red = 1, amax = 0;
    for (int y = 0; y < g_cap.h; y++)
        for (int x = 0; x < g_cap.w; x++) {
            const unsigned char *p = pix(&g_cap, x, y);
            if (p[0] > p[3] || p[1] > p[3] || p[2] > p[3]) premul = 0;
            if (p[3] && (abs(p[2] - p[3]) > 1 || p[1] > 1 || p[0] > 1)) red = 0;
            if (p[3] > amax) amax = p[3];
        }
    CHECK(ink > 40);
    CHECK(premul);
    CHECK(red);
    CHECK(amax >= 200);

    /* The ink lands where the layout was drawn. */
    st_rect box1;
    if (CHECK(ink_box(&g_cap, 1.0f, &box1))) {
        CHECK(box1.x >= 10.0f - 2.0f && box1.x + box1.w <= 10.0f + m.w + 2.0f);
        CHECK(box1.y >= 20.0f - 2.0f && box1.y + box1.h <= 20.0f + m.h + 2.0f);
    }

    /* Half alpha: half the coverage at most. */
    st_draw(g_ctx, l, 0.0f, 0.0f, 0x000000, 0.5f);
    amax = 0;
    for (int y = 0; y < g_cap.h; y++)
        for (int x = 0; x < g_cap.w; x++)
            if (pix(&g_cap, x, y)[3] > amax) amax = pix(&g_cap, x, y)[3];
    CHECK(amax > 80 && amax <= 129);

    /* At twice the scale, twice the pixels for the same DIPs. */
    st_ctx_set_scale(g_ctx, 2.0f);
    st_draw(g_ctx, l, 10.0f, 20.0f, 0x000000, 1.0f);
    st_rect box2;
    if (CHECK(ink_box(&g_cap, 2.0f, &box2))) {
        NEAR(box2.x, box1.x, 1.0f);
        NEAR(box2.w, box1.w, 1.5f);
        NEAR(box2.h, box1.h, 1.5f);
    }
    st_ctx_set_scale(g_ctx, 1.0f);
    st_layout_destroy(l);

    /* A colour range: red where the default applies, blue where the range
     * does, and neither in the other's place. */
    const char *s = "AAAA BBBB";
    l = lay(f, s, 1000.0f);
    st_range_color(l, 5, 4, 0x0000FF, 1.0f);
    st_rect ra[2], rb[2];
    st_hit_range(l, 0, 4, ra, 2);
    st_hit_range(l, 5, 4, rb, 2);
    st_draw(g_ctx, l, 0.0f, 0.0f, 0xFF0000, 1.0f);
    int red_in_a = 0, blue_in_a = 0, red_in_b = 0, blue_in_b = 0;
    for (int y = 0; y < g_cap.h; y++)
        for (int x = 0; x < g_cap.w; x++) {
            const unsigned char *p = pix(&g_cap, x, y);
            if (p[3] < 64) continue;
            float dx = g_cap.dip_x + x + 0.5f;
            int is_red = p[2] > p[0] + 32, is_blue = p[0] > p[2] + 32;
            if (dx > ra[0].x + 1 && dx < ra[0].x + ra[0].w - 1) { red_in_a += is_red; blue_in_a += is_blue; }
            if (dx > rb[0].x + 1 && dx < rb[0].x + rb[0].w - 1) { red_in_b += is_red; blue_in_b += is_blue; }
        }
    CHECK(red_in_a > 20 && blue_in_a == 0);
    CHECK(blue_in_b > 20 && red_in_b == 0);
    st_layout_destroy(l);
    st_format_destroy(f);
}

int main(void)
{
    st_sink sink = { &g_cap, sink_blit };
    g_ctx = backend(&sink);
    if (!g_ctx) {
        fprintf(stderr, "sdltext_test: the %s backend could not be created\n", BACKEND_NAME);
        return 1;
    }
    t_families();
    t_metrics();
    t_ranges();
    t_hits();
    t_boxes();
    t_lines();
    t_uniform();
    t_rtl();
    t_fallback();
    t_pixels();
    st_ctx_destroy(g_ctx);
    free(g_cap.px);
    printf("sdltext_test (%s): %d checks, %d failed\n", BACKEND_NAME, g_checks, g_fails);
    return g_fails > 255 ? 255 : g_fails;
}
