/*
 * sdltext — internals shared between the portable core (st_common.c) and the
 * backends. Not part of the public API.
 */

#ifndef ST_PRIV_H
#define ST_PRIV_H

#include <stddef.h>
#include <stdint.h>

/* The byte<->UTF-16 index map for one layout's text. Both directions are
 * O(1) because hit-testing runs on every mouse move. Built by ONE decoder
 * (st__map_build) that the backend also uses to produce its wide string, so
 * the two can never disagree about where character N is — the classic
 * off-by-surrogate bug this design exists to prevent. */
typedef struct {
    int *b2w;      /* [blen + 1] byte offset -> UTF-16 unit offset */
    int *w2b;      /* [wlen + 1] UTF-16 unit offset -> byte offset */
    int  blen, wlen;
} st_map;

/* Decode `utf8` (len bytes): fills `map` (malloc'd, st__map_free to release)
 * and, when `out_u16` is non-NULL, writes the UTF-16 string (caller frees;
 * NUL-terminated, *out_units units). Invalid sequences become U+FFFD, one
 * byte at a time, so a truncated buffer still round-trips offsets sanely.
 * Returns 0 on allocation failure. */
int  st__map_build(const char *utf8, size_t len, st_map *map,
                   uint16_t **out_u16, int *out_units);
void st__map_free(st_map *map);

/* Clamped conversions; offsets past the end map to the end. */
int st__b2w(const st_map *, size_t byte_off);
int st__w2b(const st_map *, int u16_off);

/* ---- Unicode, for a backend that lays text out itself (st_canvas.c; a
 * FreeType backend later). DirectWrite does all three inside its layout; here
 * they come from utf8proc's tables, on the UTF-16 units the map speaks. ---- */

/* Grapheme clusters (UAX #29): cb[u] is 1 where a cluster starts, for every
 * unit, and cb[n] is 1. The second unit of a surrogate pair, a combining
 * mark, a ZWJ sequence's tail: 0. malloc'd; the caller frees. 0 on failure. */
int  st__clusters_build(const uint16_t *w16, int n, uint8_t **out_cb);
/* The cluster boundary at or before / after unit u. */
int  st__cluster_prev(const uint8_t *cb, int n, int u);
int  st__cluster_next(const uint8_t *cb, int n, int u);

/* Line break opportunities (a working subset of UAX #14, docs/WEB.md):
 * brk[u] says whether a line may break BEFORE unit u: ST_BRK_NONE,
 * ST_BRK_ALLOWED, or ST_BRK_MANDATORY (after a newline). brk[0] is NONE. The
 * rules kept are the ones chat text meets: breaks after spaces and hyphens,
 * around ideographs, none before closing punctuation or inside a number,
 * none at a no-break space or joiner, never inside a cluster. malloc'd. */
enum { ST_BRK_NONE = 0, ST_BRK_ALLOWED = 1, ST_BRK_MANDATORY = 2 };
int  st__break_opps(const uint16_t *w16, int n, const uint8_t *cb, uint8_t **out_brk);

/* Bidirectional levels (UAX #9: P2-P3, X1-X10, W1-W7, N1-N2, I1-I2; the
 * bracket-pair rule N0 and L3-L4 are left out): levels[u] for every unit,
 * even left-to-right. `para_dir` is 0 for left-to-right, 1 for right-to-left,
 * -1 for the first strong character's (P2), with *out_rtl told which it was.
 * malloc'd. 0 on failure. */
int  st__bidi_levels(const uint16_t *w16, int n, int para_dir, uint8_t **out_levels, int *out_rtl);
/* L1 for one line: the units in [a, b) that are whitespace at its end take
 * the paragraph's level. */
void st__bidi_line_reset(const uint16_t *w16, uint8_t *levels, int a, int b, int para_level);
/* L2: `order[i]` becomes the index (into [0, n)) of the i'th element from
 * the left, given one level per element -- units, or a line's runs. */
void st__bidi_reorder(const uint8_t *levels, int n, int *order);

/* The code point at unit u (a surrogate pair read whole), and its unit count. */
uint32_t st__cp_at(const uint16_t *w16, int n, int u, int *units);

#endif /* ST_PRIV_H */
