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

enum { CLIP_MAX = 16, CSEG = 14, ICON_FLOATS = 16384 };

enum { ICON_CACHE_N = 64, ICON_SS = 4, DISC_CACHE_N = 32 };
typedef struct {
    int          rpx10, wpx10;    /* radius / ring width, tenth-pixels; w 0 = disc */
    SDL_Texture *tex;
    int          side;
    unsigned     stamp;
} disc_ent;
typedef struct {
    int          id, pw;      /* icon + pixel size */
    int          sw10;        /* stroke width x10, quantized */
    SDL_Texture *tex;         /* white on transparent, at 2x pw */
    unsigned     stamp;
} icon_ent;

struct gfx {
    SDL_Renderer *r;
    float         scale;
    SDL_Rect      clips[CLIP_MAX];
    int           nclip;
    icon_ent      icons[ICON_CACHE_N];
    unsigned      icon_stamp;
    disc_ent      discs[DISC_CACHE_N];
    unsigned      disc_stamp;
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
    if (!g)
        return;
    for (int i = 0; i < ICON_CACHE_N; i++)
        if (g->icons[i].tex)
            SDL_DestroyTexture(g->icons[i].tex);
    for (int i = 0; i < DISC_CACHE_N; i++)
        if (g->discs[i].tex)
            SDL_DestroyTexture(g->discs[i].tex);
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

/* THE EDGE RAMP.
 *
 * SDL_RenderGeometry has no antialiasing. Every triangle edge lands on a hard
 * pixel boundary, so an arc drawn as chords comes out a staircase with no
 * shading between the shape and what is behind it -- which next to
 * DirectWrite's antialiased text reads as "low resolution". More chords do not
 * help: the steps are per pixel, not per chord.
 *
 * Discs (disc_tex) and icons (icon_raster) each already solve this their own
 * way, and rounded rectangles can use neither -- too large to cache, too
 * varied to raster. So the coverage goes into the geometry. Every rounded
 * outline is emitted TWICE, half a device pixel inside the true edge at full
 * alpha and half a device pixel outside it at zero, with a triangle strip
 * between; the hardware interpolates, and that strip is the one-pixel coverage
 * ramp a real rasterizer would have computed.
 *
 * It costs nothing on the straight runs. An axis-aligned edge sitting on a
 * pixel boundary puts the ramp's two endpoints exactly on the pixel centers
 * either side of it, so those pixels sample 0 and 1 and the edge stays crisp.
 * Only where the outline cuts a pixel diagonally -- the corners -- does the
 * ramp produce intermediate values, which is precisely where they were
 * missing. */

enum { RN = (CSEG + 1) * 4 };   /* points in one rounded outline */

/* Half a device pixel, expressed in the layout units the callers speak. */
static float feather_u(const gfx *g)
{
    float s = g->scale > 0.01f ? g->scale : 1.0f;
    return 0.5f / s;
}

/* `r`'s outline pushed outward by `t` (negative insets it), corners following. */
static int round_ring(gfx_rect r, float rad, float t, float scale,
                      SDL_FPoint *out)
{
    gfx_rect o = { r.x - t, r.y - t, r.w + 2.0f * t, r.h + 2.0f * t };
    float rr = rad + t;
    if (rr < 0.0f)
        rr = 0.0f;
    return round_outline(o, rr, scale, out);
}

/* A filled rounded shape with the ramp: fan from the center out to the inset
 * ring, then the ramp strip out to the outset one.
 *
 * `tex` NULL fills with `c`. Otherwise every vertex is UV-mapped through the
 * device-pixel rect (mx, my, mw, mh) -- the ramp ring maps slightly outside
 * [0,1], which clamp sampling handles and zero alpha hides regardless. */
static void fan_round(gfx *g, gfx_rect shape, float radius, SDL_FColor c,
                      SDL_Texture *tex, float mx, float my, float mw, float mh)
{
    float e = feather_u(g);
    SDL_FPoint in[RN], out[RN];
    int n = round_ring(shape, radius, -e, g->scale, in);
    round_ring(shape, radius, e, g->scale, out);
    SDL_FColor c0 = c;
    c0.a = 0.0f;

    SDL_Vertex v[RN * 2 + 1];
    v[0].position.x = (shape.x + shape.w / 2.0f) * g->scale;
    v[0].position.y = (shape.y + shape.h / 2.0f) * g->scale;
    v[0].color = c;
    for (int i = 0; i < n; i++) {
        v[1 + i].position = in[i];
        v[1 + i].color = c;
        v[1 + n + i].position = out[i];
        v[1 + n + i].color = c0;
    }
    for (int i = 0; i < 1 + n * 2; i++) {
        if (tex) {
            v[i].tex_coord.x = (v[i].position.x - mx) / mw;
            v[i].tex_coord.y = (v[i].position.y - my) / mh;
        } else {
            v[i].tex_coord.x = v[i].tex_coord.y = 0.0f;
        }
    }

    int idx[RN * 9];
    int ni = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        idx[ni++] = 0;
        idx[ni++] = 1 + i;
        idx[ni++] = 1 + j;
        idx[ni++] = 1 + i;
        idx[ni++] = 1 + j;
        idx[ni++] = 1 + n + i;
        idx[ni++] = 1 + j;
        idx[ni++] = 1 + n + j;
        idx[ni++] = 1 + n + i;
    }
    geometry(g, tex, v, 1 + n * 2, idx, ni);
}

void gfx_fill_round(gfx *g, gfx_rect r, float radius, uint32_t rgb, float a)
{
    float e = feather_u(g);
    /* Below the ramp's own width there is no room for it, and a square fill is
     * what the radius rounds to anyway. */
    if (radius <= 0.5f || r.w <= 2.0f * e || r.h <= 2.0f * e) {
        gfx_fill(g, r, rgb, a);
        return;
    }
    fan_round(g, r, radius, fcol(rgb, a), NULL, 0.0f, 0.0f, 1.0f, 1.0f);
}

void gfx_stroke_round(gfx *g, gfx_rect r, float radius, float w, uint32_t rgb,
                      float a)
{
    /* Centered on the path, like the Direct2D stroke it replaces. */
    float h = w / 2.0f;
    float e = feather_u(g);
    if (r.w - 2.0f * (h + e) < 0.0f || r.h - 2.0f * (h + e) < 0.0f) {
        /* Thicker than the shape: there is no hole left, so it is a fill. */
        gfx_rect outer = { r.x - h, r.y - h, r.w + w, r.h + w };
        gfx_fill_round(g, outer, radius + h, rgb, a);
        return;
    }
    /* Four rings: the ramp's outer lip, the opaque band, the ramp's inner lip.
     * A stroke thinner than the ramp collapses the band to the path itself and
     * becomes a triangular ramp two pixels wide -- the right answer for a
     * hairline, and the reason `hi` is clamped rather than allowed to invert
     * the geometry. */
    float hi = h - e;
    if (hi < 0.0f)
        hi = 0.0f;
    const float off[4] = { h + e, hi, -hi, -(h + e) };
    SDL_FColor c = fcol(rgb, a), c0 = c;
    c0.a = 0.0f;
    const SDL_FColor *col[4] = { &c0, &c, &c, &c0 };

    SDL_FPoint ring[4][RN];
    int n = 0;
    for (int k = 0; k < 4; k++)
        n = round_ring(r, radius, off[k], g->scale, ring[k]);

    SDL_Vertex v[RN * 4];
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < n; i++) {
            SDL_Vertex *p = &v[k * n + i];
            p->position = ring[k][i];
            p->color = *col[k];
            p->tex_coord.x = p->tex_coord.y = 0.0f;
        }
    int idx[RN * 18];
    int ni = 0;
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            int o = k * n, q = (k + 1) * n;
            idx[ni++] = o + i;
            idx[ni++] = o + j;
            idx[ni++] = q + i;
            idx[ni++] = o + j;
            idx[ni++] = q + j;
            idx[ni++] = q + i;
        }
    geometry(g, NULL, v, n * 4, idx, ni);
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

/* Small circles — presence dots, avatar discs, radio marks — get ANALYTIC
 * antialiasing: a cached white texture whose per-pixel alpha is the exact
 * signed-distance coverage of the disc (or ring), drawn tinted. A triangle
 * fan has no AA and reads as a chunky polygon at dot size; D2D's ellipses
 * never did. Ellipses that are not circles keep the fan (none are small). */
static SDL_Texture *disc_tex(gfx *g, float rpx, float wpx)
{
    int rk = (int)(rpx * 10.0f + 0.5f), wk = (int)(wpx * 10.0f + 0.5f);
    disc_ent *e = NULL, *lru = &g->discs[0];
    for (int i = 0; i < DISC_CACHE_N; i++) {
        disc_ent *c = &g->discs[i];
        if (c->tex && c->rpx10 == rk && c->wpx10 == wk) { e = c; break; }
        if (!c->tex) lru = c;
        else if (lru->tex && c->stamp < lru->stamp) lru = c;
    }
    if (!e) {
        int side = (int)ceilf(rpx * 2.0f) + 2;
        SDL_Surface *sf = SDL_CreateSurface(side, side, SDL_PIXELFORMAT_RGBA32);
        if (!sf)
            return NULL;
        float c0 = side / 2.0f;
        unsigned char *px = sf->pixels;
        for (int y = 0; y < side; y++)
            for (int x = 0; x < side; x++) {
                float dx = x + 0.5f - c0, dy = y + 0.5f - c0;
                float d = sqrtf(dx * dx + dy * dy);
                float cov = rpx - d + 0.5f;
                if (wpx > 0.0f) {
                    float inner = (rpx - wpx) - d + 0.5f;
                    if (inner > 1.0f) inner = 1.0f;
                    if (inner < 0.0f) inner = 0.0f;
                    if (cov > 1.0f) cov = 1.0f;
                    if (cov < 0.0f) cov = 0.0f;
                    cov -= inner;
                }
                if (cov > 1.0f) cov = 1.0f;
                if (cov < 0.0f) cov = 0.0f;
                unsigned char *p = px + (size_t)y * sf->pitch + (size_t)x * 4;
                p[0] = p[1] = p[2] = 255;
                p[3] = (unsigned char)(cov * 255.0f + 0.5f);
            }
        SDL_Texture *t = SDL_CreateTextureFromSurface(g->r, sf);
        SDL_DestroySurface(sf);
        if (!t)
            return NULL;
        SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR);
        if (lru->tex)
            SDL_DestroyTexture(lru->tex);
        lru->rpx10 = rk;
        lru->wpx10 = wk;
        lru->tex = t;
        lru->side = side;
        e = lru;
    }
    e->stamp = ++g->disc_stamp;
    return e->tex;
}

static int disc_draw(gfx *g, float cx, float cy, float r, float w,
                     uint32_t rgb, float a)
{
    float rpx = r * g->scale;
    if (rpx > 64.0f)
        return 0;      /* big shapes: the fan is smooth enough at that size */
    SDL_Texture *t = disc_tex(g, rpx, w * g->scale);
    if (!t)
        return 0;
    SDL_SetTextureColorModFloat(t, ((rgb >> 16) & 0xff) / 255.0f,
                                ((rgb >> 8) & 0xff) / 255.0f,
                                (rgb & 0xff) / 255.0f);
    SDL_SetTextureAlphaModFloat(t, a);
    float pw = 0, ph = 0;
    SDL_GetTextureSize(t, &pw, &ph);
    SDL_FRect dst = { cx * g->scale - pw / 2.0f, cy * g->scale - ph / 2.0f,
                      pw, ph };
    SDL_RenderTexture(g->r, t, NULL, &dst);
    SDL_SetTextureColorModFloat(t, 1.0f, 1.0f, 1.0f);
    SDL_SetTextureAlphaModFloat(t, 1.0f);
    return 1;
}

/* Non-circular ellipses only -- a circle went to disc_tex above, which is
 * exact. These get the same ramp the rounded rects get, for the same reason. */
static void ellipse_ring(gfx *g, float cx, float cy, float rx, float ry,
                         float t, SDL_FPoint *out)
{
    float ax = rx + t, ay = ry + t;
    if (ax < 0.0f) ax = 0.0f;
    if (ay < 0.0f) ay = 0.0f;
    for (int i = 0; i < ESEG; i++) {
        float th = (float)i / ESEG * 6.2831853f;
        out[i].x = (cx + ax * cosf(th)) * g->scale;
        out[i].y = (cy + ay * sinf(th)) * g->scale;
    }
}

void gfx_ellipse(gfx *g, float cx, float cy, float rx, float ry, uint32_t rgb,
                 float a)
{
    if (rx > 0 && ry > 0 && rx - ry < 0.01f && ry - rx < 0.01f &&
        disc_draw(g, cx, cy, rx, 0.0f, rgb, a))
        return;
    float e = feather_u(g);
    SDL_FColor c = fcol(rgb, a), c0 = c;
    c0.a = 0.0f;
    SDL_FPoint in[ESEG], out[ESEG];
    ellipse_ring(g, cx, cy, rx, ry, -e, in);
    ellipse_ring(g, cx, cy, rx, ry, e, out);

    SDL_Vertex v[ESEG * 2 + 1];
    v[0].position.x = cx * g->scale;
    v[0].position.y = cy * g->scale;
    v[0].color = c;
    for (int i = 0; i < ESEG; i++) {
        v[1 + i].position = in[i];
        v[1 + i].color = c;
        v[1 + ESEG + i].position = out[i];
        v[1 + ESEG + i].color = c0;
    }
    for (int i = 0; i < ESEG * 2 + 1; i++)
        v[i].tex_coord.x = v[i].tex_coord.y = 0.0f;

    int idx[ESEG * 9];
    int ni = 0;
    for (int i = 0; i < ESEG; i++) {
        int j = (i + 1) % ESEG;
        idx[ni++] = 0; idx[ni++] = 1 + i; idx[ni++] = 1 + j;
        idx[ni++] = 1 + i; idx[ni++] = 1 + j; idx[ni++] = 1 + ESEG + i;
        idx[ni++] = 1 + j; idx[ni++] = 1 + ESEG + j; idx[ni++] = 1 + ESEG + i;
    }
    geometry(g, NULL, v, ESEG * 2 + 1, idx, ni);
}

void gfx_ellipse_stroke(gfx *g, float cx, float cy, float rx, float ry,
                        float w, uint32_t rgb, float a)
{
    if (rx > 0 && ry > 0 && rx - ry < 0.01f && ry - rx < 0.01f &&
        disc_draw(g, cx, cy, rx + w / 2.0f, w, rgb, a))
        return;
    float h = w / 2.0f, e = feather_u(g);
    float hi = h - e;
    if (hi < 0.0f)
        hi = 0.0f;
    const float off[4] = { h + e, hi, -hi, -(h + e) };
    SDL_FColor c = fcol(rgb, a), c0 = c;
    c0.a = 0.0f;
    const SDL_FColor *col[4] = { &c0, &c, &c, &c0 };

    SDL_FPoint ring[4][ESEG];
    for (int k = 0; k < 4; k++)
        ellipse_ring(g, cx, cy, rx, ry, off[k], ring[k]);

    SDL_Vertex v[ESEG * 4];
    for (int k = 0; k < 4; k++)
        for (int i = 0; i < ESEG; i++) {
            SDL_Vertex *p = &v[k * ESEG + i];
            p->position = ring[k][i];
            p->color = *col[k];
            p->tex_coord.x = p->tex_coord.y = 0.0f;
        }
    int idx[ESEG * 18];
    int ni = 0;
    for (int k = 0; k < 3; k++)
        for (int i = 0; i < ESEG; i++) {
            int j = (i + 1) % ESEG;
            int o = k * ESEG, q = (k + 1) * ESEG;
            idx[ni++] = o + i; idx[ni++] = o + j; idx[ni++] = q + i;
            idx[ni++] = o + j; idx[ni++] = q + j; idx[ni++] = q + i;
        }
    geometry(g, NULL, v, ESEG * 4, idx, ni);
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
        /* A texture drawn at its own pixel size is a raster (text, mostly):
         * placed on a fraction, the linear filter smears every glyph a pixel
         * wide. Snap it to the grid — the old renderer baked the fractional
         * offset into the raster itself, and half-DIP positions are the norm
         * at 150% scale. Scaled draws keep fractional placement: they are
         * being filtered anyway. */
        if (fr.w - (float)t->w < 0.5f && (float)t->w - fr.w < 0.5f) {
            fr.x = SDL_roundf(fr.x);
            fr.y = SDL_roundf(fr.y);
            fr.w = (float)t->w;
            fr.h = (float)t->h;
        }
        SDL_RenderTexture(g->r, t->t, NULL, &fr);
        SDL_SetTextureAlphaModFloat(t->t, 1.0f);
        return;
    }
    /* Rounded corners by geometry, UV-mapped back into the texture, and with
     * the same edge ramp every other rounded shape gets -- an avatar is the
     * one place a jagged arc sits right beside a face. */
    float sx = dst.x * g->scale, sy = dst.y * g->scale;
    float sw = dst.w * g->scale, sh = dst.h * g->scale;
    if (sw < 1.0f) sw = 1.0f;
    if (sh < 1.0f) sh = 1.0f;
    SDL_FColor c = { 1.0f, 1.0f, 1.0f, 1.0f };
    fan_round(g, dst, radius, c, t->t, sx, sy, sw, sh);
    SDL_SetTextureAlphaModFloat(t->t, 1.0f);
}

void gfx_tex_draw_shaped(gfx *g, gfx_tex *t, gfx_rect shape, float radius,
                         gfx_rect map, float a)
{
    SDL_SetTextureAlphaModFloat(t->t, a);
    float mx = map.x * g->scale, my = map.y * g->scale;
    float mw = map.w * g->scale, mh = map.h * g->scale;
    if (mw < 1.0f) mw = 1.0f;
    if (mh < 1.0f) mh = 1.0f;
    SDL_FColor c = { 1.0f, 1.0f, 1.0f, 1.0f };
    fan_round(g, shape, radius, c, t->t, mx, my, mw, mh);
    SDL_SetTextureAlphaModFloat(t->t, 1.0f);
}

/* Icons are rasterized ONCE per (icon, pixel size, stroke) into a white
 * texture — tessellated at 4x and box-downsampled twice — then drawn tinted.
 * SDL_RenderGeometry has no antialiasing, and hard-edged triangles at icon
 * size read as low-res; 4x supersampling restores the smoothness the old
 * antialiased geometry stroke had, and turns the per-frame cost into one
 * textured quad. */
static SDL_Texture *icon_raster(gfx *g, int icon_id, int pw, float stroke_w)
{
    static float xy[ICON_FLOATS];
    static SDL_Vertex v[ICON_FLOATS / 2];

    /* Tessellate in pixels at 4x. */
    float side4 = (float)pw * ICON_SS;
    int nf = gfx__icon_points(icon_id, 0, 0, side4, side4,
                              stroke_w * g->scale * ICON_SS, xy, ICON_FLOATS);
    if (!nf)
        return NULL;

    SDL_Texture *t4 = SDL_CreateTexture(g->r, SDL_PIXELFORMAT_RGBA32,
                                        SDL_TEXTUREACCESS_TARGET,
                                        pw * ICON_SS, pw * ICON_SS);
    SDL_Texture *t2 = SDL_CreateTexture(g->r, SDL_PIXELFORMAT_RGBA32,
                                        SDL_TEXTUREACCESS_TARGET,
                                        pw * 2, pw * 2);
    if (!t4 || !t2) {
        if (t4) SDL_DestroyTexture(t4);
        if (t2) SDL_DestroyTexture(t2);
        return NULL;
    }
    SDL_SetTextureScaleMode(t4, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureScaleMode(t2, SDL_SCALEMODE_LINEAR);
    SDL_SetTextureBlendMode(t2, SDL_BLENDMODE_BLEND);

    SDL_Texture *prev = SDL_GetRenderTarget(g->r);
    SDL_Rect prev_clip;
    bool had_clip = SDL_RenderClipEnabled(g->r);
    if (had_clip)
        SDL_GetRenderClipRect(g->r, &prev_clip);
    SDL_SetRenderClipRect(g->r, NULL);

    SDL_SetRenderTarget(g->r, t4);
    SDL_SetRenderDrawBlendMode(g->r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColorFloat(g->r, 0, 0, 0, 0);
    SDL_RenderClear(g->r);
    SDL_SetRenderDrawBlendMode(g->r, SDL_BLENDMODE_BLEND);
    SDL_FColor white = { 1, 1, 1, 1 };
    int nv = nf / 2;
    for (int i = 0; i < nv; i++) {
        v[i].position.x = xy[i * 2];
        v[i].position.y = xy[i * 2 + 1];
        v[i].color = white;
        v[i].tex_coord.x = v[i].tex_coord.y = 0.0f;
    }
    SDL_RenderGeometry(g->r, NULL, v, nv, NULL, 0);

    /* Two proper halvings: 4x -> 2x here, 2x -> 1x at draw time. */
    SDL_SetRenderTarget(g->r, t2);
    SDL_SetRenderDrawBlendMode(g->r, SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColorFloat(g->r, 0, 0, 0, 0);
    SDL_RenderClear(g->r);
    SDL_SetTextureBlendMode(t4, SDL_BLENDMODE_NONE);
    SDL_RenderTexture(g->r, t4, NULL, NULL);

    SDL_SetRenderTarget(g->r, prev);
    SDL_SetRenderClipRect(g->r, had_clip ? &prev_clip : NULL);
    SDL_SetRenderDrawBlendMode(g->r, SDL_BLENDMODE_BLEND);
    SDL_DestroyTexture(t4);
    return t2;
}

void gfx_icon(gfx *g, int icon_id, gfx_rect box, float stroke_w, uint32_t rgb,
              float a)
{
    float side = box.w < box.h ? box.w : box.h;
    int pw = (int)(side * g->scale + 0.5f);
    if (pw < 2)
        return;
    int sw10 = (int)(stroke_w * 10.0f + 0.5f);

    icon_ent *e = NULL, *lru = &g->icons[0];
    for (int i = 0; i < ICON_CACHE_N; i++) {
        icon_ent *c = &g->icons[i];
        if (c->tex && c->id == icon_id && c->pw == pw && c->sw10 == sw10) {
            e = c;
            break;
        }
        if (!c->tex) { lru = c; }
        else if (lru->tex && c->stamp < lru->stamp) lru = c;
    }
    if (!e) {
        SDL_Texture *t = icon_raster(g, icon_id, pw, stroke_w);
        if (!t)
            return;
        if (lru->tex)
            SDL_DestroyTexture(lru->tex);
        lru->id = icon_id;
        lru->pw = pw;
        lru->sw10 = sw10;
        lru->tex = t;
        e = lru;
    }
    e->stamp = ++g->icon_stamp;

    SDL_SetTextureColorModFloat(e->tex, ((rgb >> 16) & 0xff) / 255.0f,
                                ((rgb >> 8) & 0xff) / 255.0f,
                                (rgb & 0xff) / 255.0f);
    SDL_SetTextureAlphaModFloat(e->tex, a);
    /* Centered in the box, like the tessellated draw was. */
    SDL_FRect dst = { (box.x + (box.w - side) / 2.0f) * g->scale,
                      (box.y + (box.h - side) / 2.0f) * g->scale,
                      (float)pw, (float)pw };
    SDL_RenderTexture(g->r, e->tex, NULL, &dst);
    SDL_SetTextureColorModFloat(e->tex, 1.0f, 1.0f, 1.0f);
    SDL_SetTextureAlphaModFloat(e->tex, 1.0f);
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
