/* oc_gfx internals — not part of the public API. */

#ifndef OC_GFX_PRIV_H
#define OC_GFX_PRIV_H

#include "gfx.h"

/* Tessellate a Lucide icon's stroke into triangles: beziers flattened, each
 * polyline expanded to `stroke_w` with round caps and joins. Writes triangle
 * vertices as x,y pairs into `xy` (so 6 floats per triangle); returns the
 * number of FLOATS written, 0 for an unknown icon or a too-small buffer.
 * Pure geometry, no SDL — gfx_sdl.c colors the triangles. */
int gfx__icon_points(int icon_id, float bx, float by, float bw, float bh,
                     float stroke_w, float *xy, int cap);

#endif /* OC_GFX_PRIV_H */
