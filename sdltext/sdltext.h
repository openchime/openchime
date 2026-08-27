/*
 * sdltext — portable text layout, measurement and hit-testing for the
 * graphical clients (REQ-200, ARCH-97).
 *
 * The GUI draws every pixel itself, and text is the one primitive a portable
 * renderer cannot supply: correct shaping, wrapping, fonts and caret geometry
 * are platform services. This library is the seam — one API, implemented per
 * platform (DirectWrite on Windows; FreeType/fontconfig when the Linux client
 * lands), so the application layer above it is written once.
 *
 * Three contracts, stated here because every caller depends on them:
 *
 *   OFFSETS ARE UTF-8 BYTE OFFSETS into the string the layout was created
 *   from. They match oc_rt_scan's spans (client/core/richtext.h) directly;
 *   whatever wide encoding a backend shapes with is its private business, and
 *   the conversion burden lives in the backend once instead of at every call
 *   site.
 *
 *   A FORMAT IS IMMUTABLE once created. Anything that varies per use —
 *   alignment, styling ranges — belongs to the layout, so a format can be
 *   shared by every caller without the mutate-and-restore hazards a shared
 *   mutable object invites.
 *
 *   THE FAMILY COMES FROM THE OS (ARCH-97). A backend resolves families from
 *   the system's font collection and nothing is ever bundled.
 *
 * A context is created by a backend-specific constructor (st_dwrite.h on
 * Windows); this header never names a platform type.
 */

#ifndef SDLTEXT_H
#define SDLTEXT_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef struct st_ctx    st_ctx;
typedef struct st_format st_format;
typedef struct st_layout st_layout;

typedef struct { float x, y, w, h; } st_rect;

enum { ST_ALIGN_LEFT = 0, ST_ALIGN_CENTER, ST_ALIGN_RIGHT };
enum { ST_TRIM_NONE = 0, ST_TRIM_ELLIPSIS };

/* Everything a format is. `family` is UTF-8 and must name an installed family
 * (the caller decides fallbacks — the platform's system stack applies within
 * a shaped run regardless). `line_height`/`baseline` of 0 mean natural
 * spacing; both set means uniform spacing, the transcript's mode. */
typedef struct {
    const char *family;
    float       size;          /* DIPs */
    int         weight;        /* 100..900 */
    bool        italic;
    bool        wrap;          /* word wrap; a non-wrapping format may trim */
    int         trimming;      /* ST_TRIM_* */
    float       line_height;   /* 0 = natural */
    float       baseline;      /* required when line_height is set */
} st_format_desc;

/* ---- lifecycle ---- */

void st_ctx_destroy(st_ctx *);
/* DPI x zoom. Layout geometry stays in DIPs; a raster-path backend renders at
 * this scale so glyphs are sharp at any DPI. */
void st_ctx_set_scale(st_ctx *, float scale);
float st_ctx_scale(const st_ctx *);

st_format *st_format_create(st_ctx *, const st_format_desc *);
void       st_format_destroy(st_format *);

/* `max_w`/`max_h` bound wrapping and trimming; alignment is per-layout.
 * The text is copied — the caller's buffer is not retained. */
st_layout *st_layout_create(st_ctx *, st_format *, const char *utf8, size_t len,
                            float max_w, float max_h, int align);
void       st_layout_destroy(st_layout *);

/* ---- range styling (byte offsets; out-of-range requests are clamped) ---- */

void st_range_color    (st_layout *, size_t off, size_t len, uint32_t rgb, float alpha);
void st_range_weight   (st_layout *, size_t off, size_t len, int weight);
void st_range_italic   (st_layout *, size_t off, size_t len, bool on);
void st_range_underline(st_layout *, size_t off, size_t len, bool on);
void st_range_strike   (st_layout *, size_t off, size_t len, bool on);
void st_range_family   (st_layout *, size_t off, size_t len, const char *family);
void st_range_size     (st_layout *, size_t off, size_t len, float size);
/* Collapse the bytes to (near) nothing — markdown delimiters. The bytes stay
 * addressable so offsets from oc_rt_scan keep meaning, but they take no
 * visible space. */
void st_range_hide     (st_layout *, size_t off, size_t len);

/* Reserve a box in the text flow — emoji and other inline images. The range's
 * glyphs are replaced by empty space of exactly (w, h) with `baseline` DIPs
 * above the text baseline; after layout, st_layout_boxes reports where each
 * box landed so the caller composites its image there. This replaces both of
 * the old tricks (transparent glyphs overdrawn, and per-glyph emoji raster,
 * which costs ~1.5 ms per layout — measured, spike 3). */
void st_range_box(st_layout *, size_t off, size_t len,
                  float w, float h, float baseline, uint32_t box_id);
int  st_layout_boxes(const st_layout *, uint32_t *ids, st_rect *rects, int cap);

/* ---- measurement ---- */

typedef struct { float w, h; int lines; } st_metrics;
typedef struct {
    float  y, height, baseline;   /* DIPs, layout-relative */
    size_t off, len;              /* the line's bytes */
} st_line;

void  st_layout_metrics(const st_layout *, st_metrics *);
int   st_layout_lines(const st_layout *, st_line *out, int cap);
float st_text_width (st_ctx *, st_format *, const char *utf8, size_t len);
float st_line_height(st_ctx *, st_format *);

/* ---- hit-testing (the three shapes the clients use) ---- */

/* Point -> byte offset. `trailing` set means the hit lands past the middle of
 * the glyph — a caret goes after it. `inside` false means the point was
 * outside the text (offset is the nearest position). */
size_t st_hit_point(const st_layout *, float x, float y,
                    bool *inside, bool *trailing);
/* Byte offset -> caret rectangle (x is the caret line; w is the glyph run's
 * advance at that position). */
st_rect st_hit_pos(const st_layout *, size_t off, bool trailing);
/* Byte range -> covering rectangles, one per line touched. Returns the count,
 * which may exceed `cap` (the caller sees truncation, same contract as
 * oc_rt_scan). */
int st_hit_range(const st_layout *, size_t off, size_t len,
                 st_rect *out, int cap);

/* ---- drawing ---- */

/* Draw at (x, y) in DIPs, default color `rgb`/`alpha`; st_range_color
 * overrides per range. Where the pixels land is the context's business: the
 * backend constructor was given a sink (see the backend header). */
void st_draw(st_ctx *, st_layout *, float x, float y, uint32_t rgb, float alpha);

#endif /* SDLTEXT_H */
