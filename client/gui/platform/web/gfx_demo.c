/*
 * OpenChime web client — the drawing layer in a browser (docs/WEB.md, step
 * two). oc_gfx over SDL3 under Emscripten, drawing into the page's canvas: the
 * same renderer and the same primitives the Windows client's scene is made of
 * (fills, rounded rects, strokes, lines, discs, the Lucide icons, the clip
 * stack), here drawing a conversation's chrome by hand, with boxes where text
 * will go once sdltext has its canvas backend. It proves the renderer and the
 * event loop on the web; it is not the client.
 */
#include <SDL3/SDL.h>
#include <emscripten.h>
#include <stdio.h>
#include "gfx.h"
#include "icons.h"

static SDL_Window   *g_win;
static SDL_Renderer *g_ren;
static gfx          *g_gfx;
static float         g_mx, g_my;
static int           g_frames;

enum { RAIL = 70, SIDEBAR = 250, HEADER = 56, ROW = 32 };

static void text_box(gfx *g, float x, float y, float w, float h, uint32_t rgb) {
    /* A line of text, as a bar for now: sdltext's canvas backend is the next step. */
    gfx_fill_round(g, (gfx_rect){ x, y + h * 0.25f, w, h * 0.5f }, h * 0.25f, rgb, 0.35f);
}

static void frame(void) {
    int w, h;
    SDL_GetRenderOutputSize(g_ren, &w, &h);
    gfx *g = g_gfx;
    float W = (float)w / gfx_scale(g), H = (float)h / gfx_scale(g);
    SDL_Event ev;
    while (SDL_PollEvent(&ev)) {
        if (ev.type == SDL_EVENT_MOUSE_MOTION) { g_mx = ev.motion.x; g_my = ev.motion.y; }
    }
    gfx_begin(g, 0xFFFFFF);
    /* The rail. */
    gfx_fill(g, (gfx_rect){ 0, 0, RAIL, H }, 0x1E2A44, 1.0f);
    gfx_fill_round(g, (gfx_rect){ 17, 16, 36, 36 }, 10, 0x3B6FD9, 1.0f);
    static const int RAIL_ICONS[] = { OC_ICON_HOME, OC_ICON_DMS, OC_ICON_ACTIVITY, OC_ICON_FILE, OC_ICON_BOOKMARK };
    for (int i = 0; i < 5; i++) {
        float y = 84 + i * 68.0f;
        int hot = g_mx < RAIL && g_my >= y - 10 && g_my < y + 34;
        if (hot || i == 0) gfx_fill_round(g, (gfx_rect){ 15, y - 8, 40, 40 }, 10, 0xFFFFFF, i == 0 ? 0.18f : 0.10f);
        gfx_icon(g, RAIL_ICONS[i], (gfx_rect){ 23, y, 24, 24 }, 1.75f, 0xFFFFFF, 1.0f);
        text_box(g, 20, y + 28, 30, 10, 0xFFFFFF);
    }
    /* The sidebar. */
    gfx_fill(g, (gfx_rect){ RAIL, 0, SIDEBAR, H }, 0xF4F5F7, 1.0f);
    gfx_line(g, RAIL + SIDEBAR, 0, RAIL + SIDEBAR, H, 1, 0xDDE1E6, 1.0f);
    text_box(g, RAIL + 18, 16, 60, 24, 0x1E2A44);
    gfx_stroke_round(g, (gfx_rect){ RAIL + 16, 58, 150, 34 }, 8, 1, 0xC9CED6, 1.0f);
    gfx_icon(g, OC_ICON_SEARCH, (gfx_rect){ RAIL + 26, 66, 18, 18 }, 1.75f, 0x6B7280, 1.0f);
    gfx_stroke_round(g, (gfx_rect){ RAIL + 174, 58, 60, 34 }, 8, 1, 0xC9CED6, 1.0f);
    text_box(g, RAIL + 184, 66, 40, 18, 0x1E2A44);
    float y = 112;
    static const int SHELF[] = { OC_ICON_DMS, OC_ICON_SQUARE_PEN, OC_ICON_USER };
    for (int i = 0; i < 3; i++, y += ROW) {
        gfx_icon(g, SHELF[i], (gfx_rect){ RAIL + 20, y + 7, 18, 18 }, 1.75f, 0x4B5563, 1.0f);
        text_box(g, RAIL + 46, y + 6, 90 + 30 * i, 20, 0x374151);
    }
    gfx_line(g, RAIL + 16, y + 6, RAIL + SIDEBAR - 16, y + 6, 1, 0xDDE1E6, 1.0f);
    y += 20;
    text_box(g, RAIL + 20, y + 6, 70, 20, 0x6B7280);
    y += ROW;
    for (int i = 0; i < 14 && y < H - ROW; i++, y += ROW) {
        int sel = i == 3, hot = g_mx >= RAIL && g_mx < RAIL + SIDEBAR && g_my >= y && g_my < y + ROW;
        if (sel || hot) gfx_fill_round(g, (gfx_rect){ RAIL + 8, y + 2, SIDEBAR - 16, ROW - 4 }, 6, sel ? 0xCFE0F7 : 0xE7EAEF, 1.0f);
        text_box(g, RAIL + 22, y + 6, 10, 20, 0x6B7280);
        text_box(g, RAIL + 42, y + 6, 70 + (i * 37) % 90, 20, (i % 4 == 1) ? 0x111827 : 0x4B5563);
        if (i % 5 == 1) {
            gfx_fill_round(g, (gfx_rect){ RAIL + SIDEBAR - 46, y + 7, 30, 18 }, 9, 0x2563EB, 1.0f);
            text_box(g, RAIL + SIDEBAR - 36, y + 11, 10, 10, 0xFFFFFF);
        }
    }
    /* The conversation: header, transcript, composer. */
    float x0 = RAIL + SIDEBAR;
    gfx_fill(g, (gfx_rect){ x0, 0, W - x0, HEADER }, 0xFFFFFF, 1.0f);
    gfx_line(g, x0, HEADER, W, HEADER, 1, 0xDDE1E6, 1.0f);
    text_box(g, x0 + 24, 16, 90, 24, 0x111827);
    gfx_stroke_round(g, (gfx_rect){ W - 300, 12, 110, 32 }, 6, 1, 0xC9CED6, 1.0f);
    gfx_icon(g, OC_ICON_SPARKLES, (gfx_rect){ W - 290, 19, 18, 18 }, 1.75f, 0x374151, 1.0f);
    text_box(g, W - 266, 19, 66, 18, 0x374151);
    for (int i = 0; i < 3; i++) {
        gfx_stroke_round(g, (gfx_rect){ W - 176 + i * 50, 12, 40, 32 }, 6, 1, 0xC9CED6, 1.0f);
        gfx_icon(g, i == 0 ? OC_ICON_BELL : i == 1 ? OC_ICON_PLUS : OC_ICON_USER, (gfx_rect){ W - 165 + i * 50, 19, 18, 18 }, 1.75f, 0x374151, 1.0f);
    }
    gfx_clip_push(g, (gfx_rect){ x0, HEADER, W - x0, H - HEADER - 110 });
    float my = HEADER + 20 - (float)((g_frames / 4) % 40);
    for (int i = 0; i < 9; i++) {
        float h = 58 + 22 * (i % 3);
        uint32_t tints[] = { 0x2563EB, 0xDB2777, 0x059669, 0xD97706 };
        gfx_ellipse(g, x0 + 44, my + 20, 18, 18, tints[i % 4], 1.0f);
        text_box(g, x0 + 74, my + 6, 110, 20, 0x111827);
        text_box(g, x0 + 190, my + 9, 36, 14, 0x9CA3AF);
        for (int l = 0; l < 1 + i % 3; l++) text_box(g, x0 + 74, my + 30 + l * 22, (W - x0 - 140) * (l == i % 3 ? 0.55f : 0.95f), 20, 0x374151);
        if (i % 3 == 2) {
            gfx_icon(g, OC_ICON_MESSAGE, (gfx_rect){ x0 + 74, my + h - 4, 16, 16 }, 1.75f, 0x2563EB, 1.0f);
            text_box(g, x0 + 96, my + h - 4, 60, 16, 0x2563EB);
            h += 22;
        }
        if (i == 4) {
            gfx_line(g, x0 + 24, my + h + 12, W - 24, my + h + 12, 1, 0xDDE1E6, 1.0f);
            gfx_fill_round(g, (gfx_rect){ (x0 + W) / 2 - 70, my + h + 1, 140, 22 }, 11, 0xFFFFFF, 1.0f);
            gfx_stroke_round(g, (gfx_rect){ (x0 + W) / 2 - 70, my + h + 1, 140, 22 }, 11, 1, 0xDDE1E6, 1.0f);
            text_box(g, (x0 + W) / 2 - 50, my + h + 4, 100, 16, 0x6B7280);
            h += 30;
        }
        my += h + 10;
    }
    gfx_clip_pop(g);
    gfx_stroke_round(g, (gfx_rect){ x0 + 20, H - 100, W - x0 - 40, 84 }, 8, 1, 0xC9CED6, 1.0f);
    text_box(g, x0 + 36, H - 72, 160, 20, 0x9CA3AF);
    gfx_icon(g, OC_ICON_PLUS, (gfx_rect){ x0 + 34, H - 38, 18, 18 }, 1.75f, 0x374151, 1.0f);
    gfx_fill_round(g, (gfx_rect){ W - 76, H - 46, 40, 28 }, 6, 0x2563EB, 1.0f);
    gfx_end(g);
    g_frames++;
}

int main(void) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { printf("gfx-demo: SDL_Init failed: %s\n", SDL_GetError()); return 1; }
    g_win = SDL_CreateWindow("OpenChime", 1100, 720, SDL_WINDOW_RESIZABLE);
    g_ren = g_win ? SDL_CreateRenderer(g_win, NULL) : NULL;
    g_gfx = g_ren ? gfx_create(g_ren) : NULL;
    if (!g_gfx) { printf("gfx-demo: no renderer: %s\n", SDL_GetError()); return 1; }
    gfx_set_scale(g_gfx, 1.0f);
    printf("gfx-demo: drawing with %s\n", SDL_GetRendererName(g_ren));
    emscripten_set_main_loop(frame, 0, 1);
    return 0;
}
