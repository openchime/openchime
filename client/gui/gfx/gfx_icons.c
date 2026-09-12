/*
 * oc_gfx — the Lucide stroke tessellator.
 *
 * Direct2D stroked these paths with an ID2D1PathGeometry and a round-cap
 * stroke style; an SDL renderer draws triangles and nothing else, so the
 * stroke is built here: cubics flattened to polylines, each polyline expanded
 * to a quad per segment, and a disc fanned at every joint and cap — which is
 * exactly what a round join and a round cap are. Pure geometry, no SDL, so
 * this file compiles (and can be tested) anywhere.
 */

#include "gfx_priv.h"
#include "icons.h"

#include <math.h>

/* Flattening and fan granularity. A transcript icon is 16-24 DIPs; at that
 * size 12 chords per cubic and 10 triangles per disc are below a pixel of
 * error even at 2x scale. */
enum { BEZ_STEPS = 12, DISC_TRIS = 10, MAX_POLY = 1024 };

typedef struct {
    float *xy;
    int cap, n;      /* floats written; overflow just stops adding */
} emit;

static void tri(emit *e, float ax, float ay, float bx, float by, float cx,
                float cy)
{
    if (e->n + 6 > e->cap)
        return;
    e->xy[e->n++] = ax;
    e->xy[e->n++] = ay;
    e->xy[e->n++] = bx;
    e->xy[e->n++] = by;
    e->xy[e->n++] = cx;
    e->xy[e->n++] = cy;
}

static void disc(emit *e, float cx, float cy, float r)
{
    float px = cx + r, py = cy;
    for (int i = 1; i <= DISC_TRIS; i++) {
        float t = (float)i / DISC_TRIS * 6.2831853f;
        float nx = cx + r * cosf(t), ny = cy + r * sinf(t);
        tri(e, cx, cy, px, py, nx, ny);
        px = nx;
        py = ny;
    }
}

static void segment(emit *e, float ax, float ay, float bx, float by, float r)
{
    float dx = bx - ax, dy = by - ay;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 1e-6f)
        return;
    float nx = -dy / len * r, ny = dx / len * r;
    tri(e, ax + nx, ay + ny, bx + nx, by + ny, bx - nx, by - ny);
    tri(e, ax + nx, ay + ny, bx - nx, by - ny, ax - nx, ay - ny);
}

/* Expand one polyline: quads along it, discs at the ends and at REAL corners
 * only (`joint[i]`). A flattened bezier's interior points are smooth — the
 * quads already overlap there — and a disc at each of them is what blew the
 * vertex budget and dropped whole icons. */
static void stroke_poly(emit *e, const float *p, const unsigned char *joint,
                        int n, int closed, float r)
{
    if (n < 2) {
        if (n == 1)
            disc(e, p[0], p[1], r);   /* a lone moveto: a dot */
        return;
    }
    for (int i = 0; i + 1 < n; i++)
        segment(e, p[i * 2], p[i * 2 + 1], p[i * 2 + 2], p[i * 2 + 3], r);
    if (closed)
        segment(e, p[(n - 1) * 2], p[(n - 1) * 2 + 1], p[0], p[1], r);
    for (int i = 0; i < n; i++)
        if (joint[i] || i == 0 || i == n - 1)
            disc(e, p[i * 2], p[i * 2 + 1], r);
}

int gfx__icon_points(int icon_id, float bx, float by, float bw, float bh,
                     float stroke_w, float *xy, int cap)
{
    if (icon_id < 0 || icon_id >= OC_ICON_COUNT)
        return 0;
    const oc_icon *ic = &OC_ICONS[icon_id];

    /* Fit the 24x24 viewBox into the box, preserving aspect, centered. */
    float s = (bw < bh ? bw : bh) / OC_ICON_VIEWBOX;
    float ox = bx + (bw - OC_ICON_VIEWBOX * s) / 2.0f;
    float oy = by + (bh - OC_ICON_VIEWBOX * s) / 2.0f;
    float r = stroke_w / 2.0f;

    emit e = { xy, cap, 0 };
    float poly[MAX_POLY * 2];
    unsigned char joint[MAX_POLY];
    int pn = 0, closed = 0;
    float cx = 0, cy = 0;   /* pen, viewBox units */

    for (int i = 0; i < ic->n; i++) {
        const oc_icon_seg *g = &ic->segs[i];
        switch (g->op) {
        case 'M':
            if (pn)
                stroke_poly(&e, poly, joint, pn, closed, r);
            pn = 0;
            closed = 0;
            cx = g->x0;
            cy = g->y0;
            poly[pn * 2] = ox + cx * s;
            poly[pn * 2 + 1] = oy + cy * s;
            joint[pn] = 1;
            pn = 1;
            break;
        case 'L':
            if (pn < MAX_POLY) {
                cx = g->x0;
                cy = g->y0;
                poly[pn * 2] = ox + cx * s;
                poly[pn * 2 + 1] = oy + cy * s;
                joint[pn] = 1;
                pn++;
            }
            break;
        case 'C':
            for (int k = 1; k <= BEZ_STEPS && pn < MAX_POLY; k++) {
                float t = (float)k / BEZ_STEPS, u = 1.0f - t;
                float px = u * u * u * cx + 3 * u * u * t * g->x0 +
                           3 * u * t * t * g->x1 + t * t * t * g->x2;
                float py = u * u * u * cy + 3 * u * u * t * g->y0 +
                           3 * u * t * t * g->y1 + t * t * t * g->y2;
                poly[pn * 2] = ox + px * s;
                poly[pn * 2 + 1] = oy + py * s;
                joint[pn] = (unsigned char)(k == BEZ_STEPS);
                pn++;
            }
            cx = g->x2;
            cy = g->y2;
            break;
        case 'Z':
            closed = 1;
            break;
        default:
            break;
        }
    }
    if (pn)
        stroke_poly(&e, poly, joint, pn, closed, r);
    return e.n;
}
