/*
 * sdltext — the portable core: the byte<->UTF-16 index map.
 *
 * Public offsets are UTF-8 bytes (sdltext.h); every real shaping engine wants
 * UTF-16 (DirectWrite) or codepoints (FreeType). One decoder here produces
 * both the wide string a backend shapes and the offset map hit-testing
 * converts through, so they cannot drift. Covered by make test on any host —
 * this file has no platform dependencies.
 */

#include "st_priv.h"

#include <stdlib.h>

int st__map_build(const char *utf8, size_t len, st_map *map,
                  uint16_t **out_u16, int *out_units)
{
    map->blen = (int)len;
    map->b2w = malloc((len + 1) * sizeof *map->b2w);
    /* Worst case one UTF-16 unit per byte (ASCII), plus the terminator. */
    uint16_t *w = malloc((len + 1) * sizeof *w);
    int *w2b_tmp = malloc((len + 1) * sizeof *w2b_tmp);
    if (!map->b2w || !w || !w2b_tmp) {
        free(map->b2w);
        free(w);
        free(w2b_tmp);
        map->b2w = NULL;
        return 0;
    }

    size_t i = 0;
    int wn = 0;
    while (i < len) {
        const unsigned char *s = (const unsigned char *)utf8 + i;
        uint32_t cp = 0xFFFD;
        size_t take = 1;

        if (s[0] < 0x80) {
            cp = s[0];
        } else if ((s[0] & 0xE0) == 0xC0 && i + 1 < len &&
                   (s[1] & 0xC0) == 0x80) {
            cp = ((uint32_t)(s[0] & 0x1F) << 6) | (s[1] & 0x3F);
            take = 2;
            if (cp < 0x80) cp = 0xFFFD;                 /* overlong */
        } else if ((s[0] & 0xF0) == 0xE0 && i + 2 < len &&
                   (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80) {
            cp = ((uint32_t)(s[0] & 0x0F) << 12) |
                 ((uint32_t)(s[1] & 0x3F) << 6) | (s[2] & 0x3F);
            take = 3;
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
        } else if ((s[0] & 0xF8) == 0xF0 && i + 3 < len &&
                   (s[1] & 0xC0) == 0x80 && (s[2] & 0xC0) == 0x80 &&
                   (s[3] & 0xC0) == 0x80) {
            cp = ((uint32_t)(s[0] & 0x07) << 18) |
                 ((uint32_t)(s[1] & 0x3F) << 12) |
                 ((uint32_t)(s[2] & 0x3F) << 6) | (s[3] & 0x3F);
            take = 4;
            if (cp < 0x10000 || cp > 0x10FFFF) cp = 0xFFFD;
        }
        if (cp == 0xFFFD)
            take = (s[0] < 0x80) ? 1 : take;   /* keep the decided stride */

        /* Every byte of the sequence maps to the unit where it begins, so a
         * span offset landing mid-sequence styles the whole character rather
         * than corrupting a surrogate pair. */
        for (size_t k = 0; k < take; k++)
            map->b2w[i + k] = wn;

        if (cp >= 0x10000) {
            w2b_tmp[wn] = (int)i;
            w[wn++] = (uint16_t)(0xD800 + ((cp - 0x10000) >> 10));
            w2b_tmp[wn] = (int)i;
            w[wn++] = (uint16_t)(0xDC00 + ((cp - 0x10000) & 0x3FF));
        } else {
            w2b_tmp[wn] = (int)i;
            w[wn++] = (uint16_t)cp;
        }
        i += take;
    }
    map->b2w[len] = wn;
    w2b_tmp[wn] = (int)len;
    w[wn] = 0;

    map->wlen = wn;
    map->w2b = malloc((wn + 1) * sizeof *map->w2b);
    if (!map->w2b) {
        free(map->b2w);
        free(w);
        free(w2b_tmp);
        map->b2w = NULL;
        return 0;
    }
    for (int k = 0; k <= wn; k++)
        map->w2b[k] = w2b_tmp[k];
    free(w2b_tmp);

    if (out_u16) {
        *out_u16 = w;
        if (out_units)
            *out_units = wn;
    } else {
        free(w);
    }
    return 1;
}

void st__map_free(st_map *map)
{
    free(map->b2w);
    free(map->w2b);
    map->b2w = NULL;
    map->w2b = NULL;
    map->blen = map->wlen = 0;
}

int st__b2w(const st_map *m, size_t byte_off)
{
    if (!m->b2w)
        return 0;
    if (byte_off > (size_t)m->blen)
        byte_off = (size_t)m->blen;
    return m->b2w[byte_off];
}

int st__w2b(const st_map *m, int u16_off)
{
    if (!m->w2b)
        return 0;
    if (u16_off < 0)
        u16_off = 0;
    if (u16_off > m->wlen)
        u16_off = m->wlen;
    return m->w2b[u16_off];
}
