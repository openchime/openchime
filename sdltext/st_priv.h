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

#endif /* ST_PRIV_H */
