/*
 * oc_gfx — the SDL backend's test program (Windows, console).
 *
 * Runs against SDL's SOFTWARE renderer on a plain surface: no window, no GPU,
 * so it is deterministic, headless, and asserts real pixels — fills, rounded
 * corners, the clip stack, lines, icon tessellation, both texture paths and
 * the DIP scale. Built by `make windows-gfx-test`; exit code is the failure
 * count (docs/TESTING.md, the platform-backend exception).
 */

#include <SDL3/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gfx.h"
#include "icons.h"

static int failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);         \
            failures++;                                                      \
        }                                                                    \
    } while (0)

enum { W = 400, H = 300 };
static unsigned char shot[W * H * 4];

static const unsigned char *px(int x, int y)
{
    return &shot[(size_t)(y * W + x) * 4];
}

static int near_rgb(const unsigned char *p, int r, int g, int b)
{
    return abs(p[0] - r) < 40 && abs(p[1] - g) < 40 && abs(p[2] - b) < 40;
}

int main(void)
{
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
    CHECK(SDL_Init(SDL_INIT_VIDEO));
    SDL_Surface *surf = SDL_CreateSurface(W, H, SDL_PIXELFORMAT_RGBA32);
    CHECK(surf != NULL);
    SDL_Renderer *ren = SDL_CreateSoftwareRenderer(surf);
    CHECK(ren != NULL);
    gfx *g = gfx_create(ren);
    CHECK(g != NULL);
    if (!g)
        return failures;

    /* -- fills, rounded corners, background ------------------------------ */
    gfx_begin(g, 0x101020);
    gfx_fill(g, (gfx_rect){ 10, 10, 40, 40 }, 0xFF0000, 1.0f);
    gfx_fill_round(g, (gfx_rect){ 100, 10, 60, 40 }, 12.0f, 0x00C000, 1.0f);
    CHECK(gfx_readback(g, shot, W, H));
    CHECK(near_rgb(px(30, 30), 255, 0, 0));         /* inside the fill */
    CHECK(near_rgb(px(5, 5), 16, 16, 32));          /* background */
    CHECK(near_rgb(px(130, 30), 0, 192, 0));        /* rounded center */
    CHECK(near_rgb(px(101, 11), 16, 16, 32));       /* rounded corner cut */

    /* -- clip stack ------------------------------------------------------- */
    gfx_begin(g, 0x000000);
    gfx_clip_push(g, (gfx_rect){ 50, 50, 100, 100 });
    gfx_clip_push(g, (gfx_rect){ 50, 50, 50, 50 });   /* intersects */
    gfx_fill(g, (gfx_rect){ 0, 0, W, H }, 0xFFFFFF, 1.0f);
    gfx_clip_pop(g);
    gfx_clip_pop(g);
    CHECK(gfx_readback(g, shot, W, H));
    CHECK(near_rgb(px(75, 75), 255, 255, 255));     /* inside both clips */
    CHECK(near_rgb(px(125, 125), 0, 0, 0));         /* inside 1st, outside 2nd */
    CHECK(near_rgb(px(25, 25), 0, 0, 0));           /* outside both */

    /* -- line -------------------------------------------------------------- */
    gfx_begin(g, 0x000000);
    gfx_line(g, 10, 200, 200, 200, 4.0f, 0x4080FF, 1.0f);
    CHECK(gfx_readback(g, shot, W, H));
    CHECK(near_rgb(px(100, 200), 64, 128, 255));
    CHECK(near_rgb(px(100, 210), 0, 0, 0));

    /* -- icon tessellation ------------------------------------------------- */
    gfx_begin(g, 0x000000);
    gfx_icon(g, OC_ICON_PLUS, (gfx_rect){ 200, 100, 48, 48 }, 2.0f,
             0xFFFFFF, 1.0f);
    CHECK(gfx_readback(g, shot, W, H));
    int lit = 0;
    for (int y = 100; y < 148; y++)
        for (int x = 200; x < 248; x++)
            if (px(x, y)[0] > 128)
                lit++;
    CHECK(lit > 50);                                /* strokes landed */
    CHECK(near_rgb(px(224, 124), 255, 255, 255));   /* the + crosses center */
    CHECK(near_rgb(px(204, 104), 0, 0, 0));         /* corner stays empty */

    /* -- textures: straight RGBA, and the premultiplied text path ---------- */
    unsigned char quad[4 * 4] = {
        255, 0, 0, 255,   0, 255, 0, 255,
        0, 0, 255, 255,   255, 255, 0, 255,
    };
    gfx_tex *t = gfx_tex_create(g, 2, 2, quad);
    CHECK(t != NULL);
    gfx_begin(g, 0x000000);
    gfx_tex_draw(g, t, (gfx_rect){ 0, 0, 80, 80 }, 0.0f, 1.0f);
    CHECK(gfx_readback(g, shot, W, H));
    CHECK(px(10, 10)[0] > 150);                     /* red quadrant */
    CHECK(px(70, 10)[1] > 150);                     /* green quadrant */
    CHECK(px(10, 70)[2] > 150);                     /* blue quadrant */

    /* Rounded draw cuts the corner. */
    gfx_begin(g, 0x000000);
    gfx_tex_draw(g, t, (gfx_rect){ 100, 100, 80, 80 }, 24.0f, 1.0f);
    CHECK(gfx_readback(g, shot, W, H));
    CHECK(near_rgb(px(102, 102), 0, 0, 0));         /* corner cut */
    CHECK(px(140, 140)[0] > 40 || px(140, 140)[1] > 40);

    /* Premultiplied BGRA (what sdltext's sink emits): half-covered white. */
    unsigned char prem[4] = { 128, 128, 128, 128 };  /* B G R A, premul */
    gfx_tex *tt = gfx_tex_create_text(g, prem, 4, 1, 1);
    CHECK(tt != NULL);
    gfx_begin(g, 0x000000);
    gfx_tex_draw(g, tt, (gfx_rect){ 0, 0, 20, 20 }, 0.0f, 1.0f);
    CHECK(gfx_readback(g, shot, W, H));
    CHECK(px(10, 10)[0] > 90 && px(10, 10)[0] < 170);   /* blended, not opaque */

    /* -- DIP scale --------------------------------------------------------- */
    gfx_set_scale(g, 2.0f);
    gfx_begin(g, 0x000000);
    gfx_fill(g, (gfx_rect){ 10, 10, 20, 20 }, 0xFFFFFF, 1.0f);
    CHECK(gfx_readback(g, shot, W, H));
    CHECK(near_rgb(px(30, 30), 255, 255, 255));     /* 10..30 DIP -> 20..60 px */
    CHECK(near_rgb(px(55, 55), 255, 255, 255));
    CHECK(near_rgb(px(65, 65), 0, 0, 0));
    gfx_set_scale(g, 1.0f);

    gfx_tex_destroy(t);
    gfx_tex_destroy(tt);
    gfx_destroy(g);
    SDL_DestroyRenderer(ren);
    SDL_DestroySurface(surf);
    SDL_Quit();

    printf(failures ? "gfx: %d FAILURE(S)\n" : "gfx: OK\n", failures);
    return failures;
}
