/*
 * sdltext — the FreeType backend's constructor (Linux).
 *
 * Families resolve through fontconfig (ARCH-97: the system's fonts, nothing
 * bundled), and a character the chosen face lacks comes from fontconfig's
 * fallback list for it. HarfBuzz shapes each run; the paragraph layout --
 * clusters, line breaks, bidirectional order -- is sdltext's own, from the
 * common core (st_priv.h), as on the canvas backend. FreeType rasterizes.
 *
 * Pixels leave through the SINK, as from every backend: st_draw rasterizes the
 * layout into premultiplied BGRA, grayscale-antialiased, at the context's
 * scale, and hands the buffer to the sink with the DIP position its top-left
 * pixel belongs at.
 */

#ifndef ST_FT_H
#define ST_FT_H

#include "sdltext.h"

/* `sink` may be NULL for a context used only to measure and hit-test. NULL
 * when FreeType or fontconfig cannot be initialised. */
st_ctx *st_ft_create(const st_sink *sink);

#endif /* ST_FT_H */
