/* sdltext's browser backend: the page's 2D canvas measures and rasters text
 * (docs/WEB.md). Pixels leave through the sink, as from every backend. */
#ifndef ST_CANVAS_H
#define ST_CANVAS_H
#include "sdltext.h"
st_ctx *st_canvas_create(const st_sink *sink);
#endif
