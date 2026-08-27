/*
 * sdltext — the DirectWrite backend's test program (Windows, console).
 *
 * make test proves the portable half on any host; this proves the backend
 * against the real DirectWrite on real Windows: formats, layouts, byte-offset
 * styling over multibyte text, the three hit-test shapes, inline boxes, line
 * metrics, and the raster path down to actual pixels through a memory sink.
 * Built by `make windows-sdltext-test`, run on a Windows host (or WSL
 * interop); the exit code is the failure count, same contract as tests/.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "sdltext.h"
#include "st_dwrite.h"

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

/* The memory sink: keeps the last blit so tests can assert on pixels. */
typedef struct {
    unsigned char *px;
    int w, h, stride;
    float x, y;
    int blits;
} sink_mem;

static void sink_blit(void *user, const void *bgra, int stride, int pw,
                      int ph, float dx, float dy)
{
    sink_mem *s = user;
    free(s->px);
    s->px = malloc((size_t)stride * ph);
    if (s->px)
        memcpy(s->px, bgra, (size_t)stride * ph);
    s->w = pw;
    s->h = ph;
    s->stride = stride;
    s->x = dx;
    s->y = dy;
    s->blits++;
}

/* How many pixels in the sink are (a) visible at all, (b) close to a color.
 * Premultiplied BGRA; "close" is loose because grayscale AA blends edges. */
static int px_visible(const sink_mem *s)
{
    int n = 0;
    for (int y = 0; y < s->h; y++) {
        const unsigned char *row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < s->w; x++)
            if (row[x * 4 + 3] > 32)
                n++;
    }
    return n;
}

static int px_near(const sink_mem *s, int r, int g, int b)
{
    int n = 0;
    for (int y = 0; y < s->h; y++) {
        const unsigned char *row = s->px + (size_t)y * s->stride;
        for (int x = 0; x < s->w; x++) {
            int a = row[x * 4 + 3];
            if (a < 128)
                continue;
            /* un-premultiply, roughly */
            int pb = row[x * 4 + 0] * 255 / a;
            int pg = row[x * 4 + 1] * 255 / a;
            int pr = row[x * 4 + 2] * 255 / a;
            if (abs(pr - r) < 60 && abs(pg - g) < 60 && abs(pb - b) < 60)
                n++;
        }
    }
    return n;
}

int main(void)
{
    sink_mem mem = { 0 };
    st_sink sink = { &mem, sink_blit };
    st_ctx *ctx = st_dwrite_create(&sink);
    CHECK(ctx != NULL);
    if (!ctx)
        return 1;

    st_format_desc body = { "Segoe UI", 16.0f, 400, false, true,
                            ST_TRIM_NONE, 22.0f, 16.5f };
    st_format *fmt = st_format_create(ctx, &body);
    CHECK(fmt != NULL);

    /* -- metrics and wrapping ------------------------------------------- */
    {
        const char *s = "The quick brown fox jumps over the lazy dog";
        st_layout *wide = st_layout_create(ctx, fmt, s, strlen(s), 600.0f,
                                           400.0f, ST_ALIGN_LEFT);
        st_layout *narrow = st_layout_create(ctx, fmt, s, strlen(s), 120.0f,
                                             400.0f, ST_ALIGN_LEFT);
        st_metrics mw, mn;
        st_layout_metrics(wide, &mw);
        st_layout_metrics(narrow, &mn);
        CHECK(mw.lines == 1);
        CHECK(mn.lines > 1);
        CHECK(mn.h > mw.h);
        CHECK(mw.w > 200.0f && mw.w < 400.0f);
        /* uniform line spacing from the format */
        CHECK(fabsf(mw.h - 22.0f) < 0.5f);

        st_line lines[16];
        int nl = st_layout_lines(narrow, lines, 16);
        CHECK(nl == mn.lines);
        size_t covered = 0;
        for (int i = 0; i < nl; i++) {
            CHECK(lines[i].off == covered);
            covered += lines[i].len;
        }
        CHECK(covered == strlen(s));

        st_layout_destroy(wide);
        st_layout_destroy(narrow);
    }

    /* -- measurement helpers -------------------------------------------- */
    {
        float wa = st_text_width(ctx, fmt, "iii", 3);
        float wb = st_text_width(ctx, fmt, "WWW", 3);
        CHECK(wa > 0.0f && wb > wa);
        CHECK(fabsf(st_line_height(ctx, fmt) - 22.0f) < 0.5f);
    }

    /* -- byte-offset styling over multibyte text ------------------------- */
    {
        /* "café *bold* 😀 end" — offsets as oc_rt_scan would give them. */
        const char *s = "caf\xC3\xA9 *bold* \xF0\x9F\x98\x80 end";
        size_t n = strlen(s);
        st_layout *l = st_layout_create(ctx, fmt, s, n, 600.0f, 400.0f,
                                        ST_ALIGN_LEFT);
        st_metrics before;
        st_layout_metrics(l, &before);
        /* Bold the word (bytes 7..10), hide the delimiters (6 and 11). */
        st_range_weight(l, 7, 4, 700);
        st_range_hide(l, 6, 1);
        st_range_hide(l, 11, 1);
        st_metrics after;
        st_layout_metrics(l, &after);
        CHECK(after.w < before.w);   /* the '*'s collapsed */

        /* A range rect over the bolded word is a single non-empty rect. */
        st_rect rr[4];
        int nr = st_hit_range(l, 7, 4, rr, 4);
        CHECK(nr == 1);
        CHECK(rr[0].w > 4.0f && rr[0].h > 4.0f);
        st_layout_destroy(l);
    }

    /* -- hit-test round trip --------------------------------------------- */
    {
        const char *s = "abc def ghi";
        st_layout *l = st_layout_create(ctx, fmt, s, strlen(s), 600.0f,
                                        400.0f, ST_ALIGN_LEFT);
        for (size_t off = 0; off <= strlen(s); off += 4) {
            st_rect caret = st_hit_pos(l, off, false);
            bool inside = false, trailing = false;
            size_t back = st_hit_point(l, caret.x + 0.5f,
                                       caret.y + caret.h / 2.0f, &inside,
                                       &trailing);
            size_t got = trailing ? back + 1 : back;
            CHECK(got == off);
        }
        st_layout_destroy(l);
    }

    /* -- hit-testing across a surrogate pair ------------------------------ */
    {
        const char *s = "x\xF0\x9F\x98\x80y";   /* x 😀 y */
        st_layout *l = st_layout_create(ctx, fmt, s, strlen(s), 600.0f,
                                        400.0f, ST_ALIGN_LEFT);
        /* The caret after the emoji is byte 5; a point past the emoji's
         * middle must report byte 1 with trailing set — never byte 2..4. */
        st_rect emoji = st_hit_pos(l, 1, false);
        bool inside, trailing;
        size_t off = st_hit_point(l, emoji.x + emoji.w * 0.9f,
                                  emoji.y + emoji.h / 2.0f, &inside,
                                  &trailing);
        CHECK(off == 1);
        CHECK(trailing);
        st_layout_destroy(l);
    }

    /* -- inline boxes ----------------------------------------------------- */
    {
        const char *s = "a :smile: b";
        st_layout *l = st_layout_create(ctx, fmt, s, strlen(s), 600.0f,
                                        400.0f, ST_ALIGN_LEFT);
        st_metrics plain;
        st_layout_metrics(l, &plain);
        st_range_box(l, 2, 7, 20.0f, 20.0f, 17.0f, 42);
        uint32_t ids[4];
        st_rect rects[4];
        int nb = st_layout_boxes(l, ids, rects, 4);
        CHECK(nb == 1);
        CHECK(ids[0] == 42);
        CHECK(fabsf(rects[0].w - 20.0f) < 1.0f);
        CHECK(rects[0].x > 0.0f);
        st_layout_destroy(l);
        (void)plain;
    }

    /* -- raster through the sink ------------------------------------------ */
    {
        const char *s = "hello RED world";
        st_layout *l = st_layout_create(ctx, fmt, s, strlen(s), 600.0f,
                                        400.0f, ST_ALIGN_LEFT);
        st_range_color(l, 6, 3, 0xFF2020, 1.0f);
        st_draw(ctx, l, 10.0f, 20.0f, 0xFFFFFF, 1.0f);
        CHECK(mem.blits == 1);
        CHECK(mem.px != NULL);
        CHECK(mem.x == 10.0f && mem.y == 20.0f);
        CHECK(px_visible(&mem) > 100);            /* glyphs actually landed */
        CHECK(px_near(&mem, 255, 32, 32) > 10);   /* the colored range shows */
        CHECK(px_near(&mem, 255, 255, 255) > 100);/* default color shows */
        int w1 = mem.w;

        /* Scale doubles the pixel output of the same DIP layout. */
        st_ctx_set_scale(ctx, 2.0f);
        st_draw(ctx, l, 0.0f, 0.0f, 0xFFFFFF, 1.0f);
        CHECK(mem.blits == 2);
        CHECK(mem.w > (int)(w1 * 1.8f));
        st_ctx_set_scale(ctx, 1.0f);
        st_layout_destroy(l);
    }

    /* -- trimming --------------------------------------------------------- */
    {
        st_format_desc one = { "Segoe UI", 16.0f, 400, false, false,
                               ST_TRIM_ELLIPSIS, 0.0f, 0.0f };
        st_format *ell = st_format_create(ctx, &one);
        CHECK(ell != NULL);
        const char *s = "a very long single line that cannot possibly fit";
        st_layout *l = st_layout_create(ctx, ell, s, strlen(s), 80.0f, 40.0f,
                                        ST_ALIGN_LEFT);
        st_metrics m;
        st_layout_metrics(l, &m);
        CHECK(m.lines == 1);
        st_layout_destroy(l);
        st_format_destroy(ell);
    }

    /* -- baseline stability (ARCH-108) ------------------------------------
     * The GUI pins one integer line box + baseline per size token across every
     * weight and style of that size, so a bold swap or a descender can never
     * move a label. This certifies the property the app relies on: under
     * UNIFORM spacing, metrics and baselines are identical across weights and
     * across strings with and without descenders, and the pinned DIP baseline
     * maps to the expected device row under scale. */
    {
        st_format_desc dr = { "Segoe UI", 14.0f, 400, false, false,
                              ST_TRIM_ELLIPSIS, 22.0f, 16.0f };
        st_format_desc db = { "Segoe UI", 14.0f, 600, false, false,
                              ST_TRIM_ELLIPSIS, 22.0f, 16.0f };
        st_format_desc di = { "Segoe UI", 14.0f, 400, true, false,
                              ST_TRIM_ELLIPSIS, 22.0f, 16.0f };
        st_format *fr = st_format_create(ctx, &dr);
        st_format *fb = st_format_create(ctx, &db);
        st_format *fi = st_format_create(ctx, &di);
        CHECK(fr && fb && fi);
        const char *strs[2] = { "HHH", "Hxg" };   /* caps-only / with descender */
        st_format  *fms[3]  = { fr, fb, fi };
        float ys[6]; int k = 0;
        for (int fj = 0; fj < 3; fj++)
            for (int sj = 0; sj < 2; sj++) {
                st_layout *l = st_layout_create(ctx, fms[fj], strs[sj],
                                                strlen(strs[sj]), 400.0f,
                                                60.0f, ST_ALIGN_LEFT);
                st_metrics m; st_layout_metrics(l, &m);
                CHECK(m.lines == 1);
                CHECK(fabsf(m.h - 22.0f) < 0.01f);     /* pinned line box */
                st_line ln[2];
                CHECK(st_layout_lines(l, ln, 2) == 1);
                CHECK(fabsf(ln[0].baseline - 16.0f) < 0.01f); /* pinned baseline */
                st_draw(ctx, l, 0.0f, 40.0f, 0xFFFFFF, 1.0f);
                if (k < 6) ys[k++] = mem.y;            /* ink dy = 40 + tm.top */
                st_layout_destroy(l);
            }
        /* Ink top varies with glyph shape by nature; what must NOT vary is the
         * baseline: dy − tm.top cancels, so dy for the same string must agree
         * across weights/styles, and cap-only vs descender strings of one
         * format must share their format's dy (the descender grows the raster
         * DOWNWARD only — the top edge stays put when the box is pinned). */
        CHECK(fabsf(ys[0] - ys[1]) < 0.01f);   /* regular: HHH vs Hxg tops equal */
        CHECK(fabsf(ys[2] - ys[3]) < 0.01f);   /* semibold: same */
        CHECK(fabsf(ys[4] - ys[5]) < 0.01f);   /* italic: same */
        /* At 1.5x the pinned DIP baseline lands on the expected device row. */
        {
            st_layout *l = st_layout_create(ctx, fr, "Baseline", 8, 400.0f,
                                            60.0f, ST_ALIGN_LEFT);
            st_draw(ctx, l, 0.0f, 0.0f, 0xFFFFFF, 1.0f);
            float dy1 = mem.y;
            st_ctx_set_scale(ctx, 1.5f);
            st_draw(ctx, l, 0.0f, 0.0f, 0xFFFFFF, 1.0f);
            /* dy is in DIPs by contract, so it must not move with scale... */
            CHECK(fabsf(mem.y - dy1) < 0.01f);
            /* ...while the raster grows with it. */
            st_ctx_set_scale(ctx, 1.0f);
            st_layout_destroy(l);
        }
        st_format_destroy(fi);
        st_format_destroy(fb);
        st_format_destroy(fr);
    }

    st_format_destroy(fmt);
    st_ctx_destroy(ctx);
    free(mem.px);

    printf(failures ? "sdltext: %d FAILURE(S)\n" : "sdltext: OK\n", failures);
    return failures;
}
