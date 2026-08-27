/*
 * oc_gfx — the SDL3 renderer implementation.
 *
 * Everything that is not a plain rect becomes triangles through
 * SDL_RenderGeometry: rounded rects are corner-arc fans, strokes are rings,
 * lines are quads, icons arrive pre-tessellated from gfx_icons.c. DIP -> pixel
 * scaling happens here, at the last moment before SDL sees a coordinate, so
 * callers never carry the scale themselves.
 */

#include "gfx_priv.h"

#include <SDL3/SDL.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

enum { CLIP_MAX = 16, CSEG = 6, ICON_FLOATS = 16384 };

struct gfx {
    SDL_Renderer *r;
    float         scale;
    SDL_Rect      clips[CLIP_MAX];
    int           nclip;
};

struct gfx_tex {
    SDL_Texture *t;
    int          w, h;
};

static SDL_FColor fcol(uint32_t rgb, float a)
{
    SDL_FColor c;
    c.r = ((rgb >> 16) & 0xff) / 255.0f;
    c.g = ((rgb >> 8) & 0xff) / 255.0f;
    c.b = (rgb & 0xff) / 255.0f;
    c.a = a;
    return c;
}

gfx *gfx_create(SDL_Renderer *r)
{
    gfx *g = calloc(1, sizeof *g);
    if (!g)
        return NULL;
    g->r = r;
    g->scale = 1.0f;
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    return g;
}

void gfx_destroy(gfx *g)
{
    free(g);
}

void gfx_set_scale(gfx *g, float scale)
{
    g->scale = scale > 0.0f ? scale : 1.0f;
}

float gfx_scale(const gfx *g)
{
    return g->scale;
}

void gfx_begin(gfx *g, uint32_t clear_rgb)
{
    SDL_FColor c = fcol(clear_rgb, 1.0f);
    g->nclip = 0;
    SDL_SetRenderClipRect(g->r, NULL);
    SDL_SetRenderDrawColorFloat(g->r, c.r, c.g, c.b, 1.0f);
    SDL_RenderClear(g->r);
}

void gfx_end(gfx *g)
{
    SDL_RenderPresent(g->r);
}

void gfx_fill(gfx *g, gfx_rect r, uint32_t rgb, float a)
{
    SDL_FColor c = fcol(rgb, a);
    SDL_FRect fr = { r.x * g->scale, r.y * g->scale, r.w * g->scale,
                     r.h * g->scale };
    SDL_SetRenderDrawColorFloat(g->r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(g->r, &fr);
}

/* The rounded-rect outline: 4 corner arcs of CSEG chords each, clockwise
 * from the top-left corner's end. Writes (CSEG + 1) * 4 points. */
static int round_outline(gfx_rect r, float rad, float scale, SDL_FPoint *out)
{
    float half = (r.w < r.h ? r.w : r.h) / 2.0f;
    if (rad > half)
        rad = half;
    if (rad < 0.0f)
        rad = 0.0f;
    struct { float cx, cy, a0; } corner[4] = {
        { r.x + r.w - rad, r.y + rad, -1.5707963f },       /* top-right */
        { r.x + r.w - rad, r.y + r.h - rad, 0.0f },        /* bottom-right */
        { r.x + rad, r.y + r.h - rad, 1.5707963f },        /* bottom-left */
        { r.x + rad, r.y + rad, 3.1415926f },              /* top-left */
    };
    int n = 0;
    for (int c = 0; c < 4; c++)
        for (int i = 0; i <= CSEG; i++) {
            float t = corner[c].a0 + (float)i / CSEG * 1.5707963f;
            out[n].x = (corner[c].cx + rad * cosf(t)) * scale;
            out[n].y = (corner[c].cy + rad * sinf(t)) * scale;
            n++;
        }
    return n;
}

static void geometry(gfx *g, SDL_Texture *tex, const SDL_Vertex *v, int nv,
                     const int *idx, int ni)
{
    SDL_RenderGeometry(g->r, tex, v, nv, idx, ni);
}

void gfx_fill_round(gfx *g, gfx_rect r, float radius, uint32_t rgb, float a)
{
    if (radius <= 0.5f) {
        gfx_fill(g, r, rgb, a);
        return;
    }
    SDL_FPoint pts[(CSEG + 1) * 4];
    int n = round_outline(r, radius, g->scale, pts);
    SDL_FColor c = fcol(rgb, a);

    SDL_Vertex v[(CSEG + 1) * 4 + 1];
    v[0].position.x = (r.x + r.w / 2.0f) * g->scale;
    v[0].position.y = (r.y + r.h / 2.0f) * g->scale;
    v[0].color = c;
    v[0].tex_coord.x = v[0].tex_coord.y = 0.0f;
    for (int i = 0; i < n; i++) {
        v[i + 1].position = pts[i];
        v[i + 1].color = c;
        v[i + 1].tex_coord.x = v[i + 1].tex_coord.y = 0.0f;
    }
    int idx[(CSEG + 1) * 4 * 3];
    int ni = 0;
    for (int i = 0; i < n; i++) {
        idx[ni++] = 0;
        idx[ni++] = 1 + i;
        idx[ni++] = 1 + (i + 1) % n;
    }
    geometry(g, NULL, v, n + 1, idx, ni);
}

void gfx_stroke_round(gfx *g, gfx_rect r, float radius, float w, uint32_t rgb,
                      float a)
{
    /* Centered on the path, like the Direct2D stroke it replaces. */
    float h = w / 2.0f;
    gfx_rect outer = { r.x - h, r.y - h, r.w + w, r.h + w };
    gfx_rect inner = { r.x + h, r.y + h, r.w - w, r.h - w };
    if (inner.w < 0.0f || inner.h < 0.0f) {
        gfx_fill_round(g, outer, radius + h, rgb, a);
        return;
    }
    SDL_FPoint po[(CSEG + 1) * 4], pi[(CSEG + 1) * 4];
    int n = round_outline(outer, radius + h, g->scale, po);
    round_outline(inner, radius - h, g->scale, pi);

    SDL_FColor c = fcol(rgb, a);
    SDL_Vertex v[(CSEG + 1) * 8];
    for (int i = 0; i < n; i++) {
        v[i * 2].position = po[i];
        v[i * 2].color = c;
        v[i * 2].tex_coord.x = v[i * 2].tex_coord.y = 0.0f;
        v[i * 2 + 1].position = pi[i];
        v[i * 2 + 1].color = c;
        v[i * 2 + 1].tex_coord.x = v[i * 2 + 1].tex_coord.y = 0.0f;
    }
    int idx[(CSEG + 1) * 4 * 6];
    int ni = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        idx[ni++] = i * 2;
        idx[ni++] = j * 2;
        idx[ni++] = i * 2 + 1;
        idx[ni++] = j * 2;
        idx[ni++] = j * 2 + 1;
        idx[ni++] = i * 2 + 1;
    }
    geometry(g, NULL, v, n * 2, idx, ni);
}

void gfx_line(gfx *g, float x0, float y0, float x1, float y1, float w,
              uint32_t rgb, float a)
{
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-6f)
        return;
    float nx = -dy / len * w / 2.0f, ny = dx / len * w / 2.0f;
    SDL_FColor c = fcol(rgb, a);
    SDL_Vertex v[4];
    const float px[4] = { x0 + nx, x1 + nx, x1 - nx, x0 - nx };
    const float py[4] = { y0 + ny, y1 + ny, y1 - ny, y0 - ny };
    for (int i = 0; i < 4; i++) {
        v[i].position.x = px[i] * g->scale;
        v[i].position.y = py[i] * g->scale;
        v[i].color = c;
        v[i].tex_coord.x = v[i].tex_coord.y = 0.0f;
    }
    const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    geometry(g, NULL, v, 4, idx, 6);
}

enum { ESEG = 28 };

void gfx_ellipse(gfx *g, float cx, float cy, float rx, float ry, uint32_t rgb,
                 float a)
{
    SDL_FColor c = fcol(rgb, a);
    SDL_Vertex v[ESEG + 1];
    v[0].position.x = cx * g->scale;
    v[0].position.y = cy * g->scale;
    v[0].color = c;
    v[0].tex_coord.x = v[0].tex_coord.y = 0.0f;
    for (int i = 0; i < ESEG; i++) {
        float t = (float)i / ESEG * 6.2831853f;
        v[i + 1].position.x = (cx + rx * cosf(t)) * g->scale;
        v[i + 1].position.y = (cy + ry * sinf(t)) * g->scale;
        v[i + 1].color = c;
        v[i + 1].tex_coord.x = v[i + 1].tex_coord.y = 0.0f;
    }
    int idx[ESEG * 3];
    int ni = 0;
    for (int i = 0; i < ESEG; i++) {
        idx[ni++] = 0;
        idx[ni++] = 1 + i;
        idx[ni++] = 1 + (i + 1) % ESEG;
    }
    geometry(g, NULL, v, ESEG + 1, idx, ni);
}

void gfx_ellipse_stroke(gfx *g, float cx, float cy, float rx, float ry,
                        float w, uint32_t rgb, float a)
{
    SDL_FColor c = fcol(rgb, a);
    float h = w / 2.0f;
    SDL_Vertex v[ESEG * 2];
    for (int i = 0; i < ESEG; i++) {
        float t = (float)i / ESEG * 6.2831853f;
        float co = cosf(t), si = sinf(t);
        v[i * 2].position.x = (cx + (rx + h) * co) * g->scale;
        v[i * 2].position.y = (cy + (ry + h) * si) * g->scale;
        v[i * 2 + 1].position.x = (cx + (rx - h) * co) * g->scale;
        v[i * 2 + 1].position.y = (cy + (ry - h) * si) * g->scale;
        for (int k = 0; k < 2; k++) {
            v[i * 2 + k].color = c;
            v[i * 2 + k].tex_coord.x = v[i * 2 + k].tex_coord.y = 0.0f;
        }
    }
    int idx[ESEG * 6];
    int ni = 0;
    for (int i = 0; i < ESEG; i++) {
        int j = (i + 1) % ESEG;
        idx[ni++] = i * 2;
        idx[ni++] = j * 2;
        idx[ni++] = i * 2 + 1;
        idx[ni++] = j * 2;
        idx[ni++] = j * 2 + 1;
        idx[ni++] = i * 2 + 1;
    }
    geometry(g, NULL, v, ESEG * 2, idx, ni);
}

void gfx_clip_push(gfx *g, gfx_rect r)
{
    SDL_Rect px = { (int)floorf(r.x * g->scale), (int)floorf(r.y * g->scale),
                    (int)ceilf(r.w * g->scale), (int)ceilf(r.h * g->scale) };
    if (g->nclip > 0) {
        SDL_Rect cur = g->clips[g->nclip - 1];
        SDL_Rect out;
        if (!SDL_GetRectIntersection(&cur, &px, &out)) {
            out.x = out.y = 0;
            out.w = out.h = 0;
        }
        px = out;
    }
    if (g->nclip < CLIP_MAX)
        g->clips[g->nclip++] = px;
    SDL_SetRenderClipRect(g->r, &px);
}

void gfx_clip_pop(gfx *g)
{
    if (g->nclip > 0)
        g->nclip--;
    SDL_SetRenderClipRect(g->r, g->nclip ? &g->clips[g->nclip - 1] : NULL);
}

gfx_tex *gfx_tex_create(gfx *g, int w, int h, const void *rgba)
{
    gfx_tex *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->t = SDL_CreateTexture(g->r, SDL_PIXELFORMAT_RGBA32,
                             SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t->t) {
        free(t);
        return NULL;
    }
    SDL_UpdateTexture(t->t, NULL, rgba, w * 4);
    SDL_SetTextureBlendMode(t->t, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(t->t, SDL_SCALEMODE_LINEAR);
    t->w = w;
    t->h = h;
    return t;
}

gfx_tex *gfx_tex_create_text(gfx *g, const void *bgra_premul, int stride,
                             int w, int h)
{
    gfx_tex *t = calloc(1, sizeof *t);
    if (!t)
        return NULL;
    t->t = SDL_CreateTexture(g->r, SDL_PIXELFORMAT_BGRA32,
                             SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t->t) {
        free(t);
        return NULL;
    }
    SDL_UpdateTexture(t->t, NULL, bgra_premul, stride);
    SDL_SetTextureBlendMode(t->t, SDL_BLENDMODE_BLEND_PREMULTIPLIED);
    t->w = w;
    t->h = h;
    return t;
}

void gfx_tex_destroy(gfx_tex *t)
{
    if (!t)
        return;
    SDL_DestroyTexture(t->t);
    free(t);
}

void gfx_tex_size(const gfx_tex *t, int *w, int *h)
{
    if (w)
        *w = t->w;
    if (h)
        *h = t->h;
}

void gfx_tex_draw(gfx *g, gfx_tex *t, gfx_rect dst, float radius, float a)
{
    SDL_SetTextureAlphaModFloat(t->t, a);
    if (radius <= 0.5f) {
        SDL_FRect fr = { dst.x * g->scale, dst.y * g->scale, dst.w * g->scale,
                         dst.h * g->scale };
        SDL_RenderTexture(g->r, t->t, NULL, &fr);
        SDL_SetTextureAlphaModFloat(t->t, 1.0f);
        return;
    }
    /* Rounded corners by geometry: the outline fanned from the center, each
     * vertex UV-mapped back into the texture. */
    SDL_FPoint pts[(CSEG + 1) * 4];
    int n = round_outline(dst, radius, g->scale, pts);
    SDL_FColor c = { 1.0f, 1.0f, 1.0f, 1.0f };

    SDL_Vertex v[(CSEG + 1) * 4 + 1];
    float sx = dst.x * g->scale, sy = dst.y * g->scale;
    float sw = dst.w * g->scale, sh = dst.h * g->scale;
    v[0].position.x = sx + sw / 2.0f;
    v[0].position.y = sy + sh / 2.0f;
    v[0].color = c;
    v[0].tex_coord.x = 0.5f;
    v[0].tex_coord.y = 0.5f;
    for (int i = 0; i < n; i++) {
        v[i + 1].position = pts[i];
        v[i + 1].color = c;
        v[i + 1].tex_coord.x = (pts[i].x - sx) / sw;
        v[i + 1].tex_coord.y = (pts[i].y - sy) / sh;
    }
    int idx[(CSEG + 1) * 4 * 3];
    int ni = 0;
    for (int i = 0; i < n; i++) {
        idx[ni++] = 0;
        idx[ni++] = 1 + i;
        idx[ni++] = 1 + (i + 1) % n;
    }
    geometry(g, t->t, v, n + 1, idx, ni);
    SDL_SetTextureAlphaModFloat(t->t, 1.0f);
}

void gfx_icon(gfx *g, int icon_id, gfx_rect box, float stroke_w, uint32_t rgb,
              float a)
{
    static float xy[ICON_FLOATS];
    int nf = gfx__icon_points(icon_id, box.x, box.y, box.w, box.h, stroke_w,
                              xy, ICON_FLOATS);
    if (!nf)
        return;
    SDL_FColor c = fcol(rgb, a);
    static SDL_Vertex v[ICON_FLOATS / 2];
    int nv = nf / 2;
    for (int i = 0; i < nv; i++) {
        v[i].position.x = xy[i * 2] * g->scale;
        v[i].position.y = xy[i * 2 + 1] * g->scale;
        v[i].color = c;
        v[i].tex_coord.x = v[i].tex_coord.y = 0.0f;
    }
    geometry(g, NULL, v, nv, NULL, 0);
}

int gfx_readback(gfx *g, void *rgba, int w, int h)
{
    SDL_Rect r = { 0, 0, w, h };
    SDL_Surface *s = SDL_RenderReadPixels(g->r, &r);
    if (!s)
        return 0;
    SDL_Surface *conv = SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(s);
    if (!conv)
        return 0;
    for (int y = 0; y < h && y < conv->h; y++)
        memcpy((unsigned char *)rgba + (size_t)y * w * 4,
               (unsigned char *)conv->pixels + (size_t)y * conv->pitch,
               (size_t)(w < conv->w ? w : conv->w) * 4);
    SDL_DestroySurface(conv);
    return 1;
}
