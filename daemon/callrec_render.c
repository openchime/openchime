/* Making a call's recording from its spool -- see callrec_render.h and
 * docs/CALLS.md §8. */

#include "callrec_render.h"
#include "callrec.h"       /* the spool's layout and timeline kinds */
#include "oc_mp4.h"

#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_PEOPLE   ((int)OC_MAX_CALL_PARTICIPANTS)
#define AWIN         64            /* frames of one voice held to put them in order */
#define CONCEAL_MAX  3             /* a gap this long is concealed; longer is silence (DTX) */
#define MAIN_H       576           /* the large area; the strip below it */
#define TILE_W       192
#define TILE_H       144
#define STRIP_TILES  6
#define KEY_EVERY    (OC_CALLREC_FPS * 2)
#define SWITCH_MS    300           /* how long another must out-speak the speaker to take over */

static uint64_t be64(const uint8_t *p) { uint64_t v = 0; for (int i = 0; i < 8; i++) v = (v << 8) | p[i]; return v; }
static uint32_t be32(const uint8_t *p) { return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3]; }
static uint32_t be16(const uint8_t *p) { return ((uint32_t)p[0] << 8) | p[1]; }

static void seterr(char *err, size_t cap, const char *what) { if (err && cap) snprintf(err, cap, "%s", what); }

/* ---- reading the spool ---------------------------------------------------------- */

/* One voice: its frames, read in the order they were had, held by frame number
 * in a window so they come out in order. */
typedef struct {
    uint64_t user;
    FILE    *f;
    int      eof, bad;
    int      anchored;
    uint64_t first_at;
    uint32_t first_frame;
    uint32_t next;                 /* the next frame to play */
    struct { int have; uint32_t frame; uint16_t len; uint8_t data[1500]; } win[AWIN];
    /* A frame beyond the window -- after a silence (DTX sends nothing), or a
     * device that joined again and counts afresh -- waits here until the call
     * reaches the time it was had, and playing goes on from it. */
    int      pend;
    uint64_t pend_at;
    uint32_t pend_frame;
    uint16_t pend_len;
    uint8_t  pend_data[1500];
    void    *dec;
    int      missed;               /* frames concealed since the last real one */
    int16_t  pcm[OC_CALLREC_FRAME];
    double   level;                /* smoothed, for who is speaking */
} voice;

/* One video stream (a camera, or the shared screen): its frames read as time
 * reaches them, decoded, the newest picture kept. */
typedef struct {
    uint64_t user;                 /* 0: the shared screen */
    FILE    *f;
    size_t   hdr;                  /* the record's header: 21 a camera's, 25 a share's */
    int      eof, bad;
    int      peeked;
    uint64_t at, sharer;
    uint8_t  key;
    uint32_t len;
    uint8_t *data;
    size_t   cap;
    void    *dec;
    int      started;              /* a keyframe decoded */
    oc_frame pic;                  /* the newest picture, our copy */
    uint8_t *pic_buf;
    int      have_pic;
    uint64_t pic_sharer;
} vstream;

typedef struct { uint64_t at; uint8_t kind; uint64_t user; } tl_event;

static int voice_fill(voice *v) {
    /* Read until the window holds what comes next or the file ends. */
    while (!v->eof && !v->bad && !v->pend) {
        uint8_t h[14];
        size_t got = fread(h, 1, sizeof h, v->f);
        if (got == 0) { v->eof = 1; break; }
        if (got != sizeof h) { v->bad = 1; break; }
        uint64_t at = be64(h);
        uint32_t frame = be32(h + 8);
        uint16_t len = (uint16_t)be16(h + 12);
        if (len == 0 || len > sizeof v->win[0].data) { v->bad = 1; break; }
        uint8_t buf[1500];
        if (fread(buf, 1, len, v->f) != len) { v->bad = 1; break; }
        if (!v->anchored) { v->anchored = 1; v->first_at = at; v->first_frame = frame; v->next = frame; }
        int32_t ahead = (int32_t)(frame - v->next);
        if (ahead < 0 && ahead > -AWIN) continue;      /* too late: already played */
        if (ahead < 0 || ahead >= AWIN) {
            /* Far from what plays: after a silence, or counted afresh by a
             * device that came back -- it plays when it was had. */
            v->pend = 1; v->pend_at = at; v->pend_frame = frame; v->pend_len = len;
            memcpy(v->pend_data, buf, len);
            return 0;
        }
        int k = (int)(frame % AWIN);
        v->win[k].have = 1; v->win[k].frame = frame; v->win[k].len = len;
        memcpy(v->win[k].data, buf, len);
        if ((int32_t)(frame - v->next) >= AWIN / 2) return 0;   /* enough held */
    }
    return v->bad ? -1 : 0;
}

/* The voice's next 20 ms, played at its own clock: decoded, concealed or
 * silent. 1 if it said anything, 0 if silent. */
static int voice_frame(voice *v, const oc_callrec_codecs *c, uint64_t t) {
    if (v->pend && t + 20 > v->pend_at) {
        /* The call has reached the waiting frame: what was before it is not
         * coming, and the voice plays on from it. */
        for (int i = 0; i < AWIN; i++) v->win[i].have = 0;
        v->next = v->pend_frame;
        int k = (int)(v->next % AWIN);
        v->win[k].have = 1; v->win[k].frame = v->pend_frame; v->win[k].len = v->pend_len;
        memcpy(v->win[k].data, v->pend_data, v->pend_len);
        v->pend = 0;
    }
    if (voice_fill(v) != 0) return -1;
    int k = (int)(v->next % AWIN);
    int said = 0;
    if (v->win[k].have && v->win[k].frame == v->next) {
        if (c->audio_decode(v->dec, v->win[k].data, v->win[k].len, v->pcm) != 0) return -1;
        v->win[k].have = 0;
        v->missed = 0;
        said = 1;
    } else if (v->missed < CONCEAL_MAX && v->anchored) {
        if (c->audio_decode(v->dec, NULL, 0, v->pcm) != 0) memset(v->pcm, 0, sizeof v->pcm);
        v->missed++;
        said = 1;
    } else {
        memset(v->pcm, 0, sizeof v->pcm);
    }
    v->next++;
    return said;
}

static int vstream_peek(vstream *s) {
    if (s->peeked || s->eof || s->bad) return s->peeked;
    uint8_t h[25];
    size_t got = fread(h, 1, s->hdr, s->f);
    if (got == 0) { s->eof = 1; return 0; }
    if (got != s->hdr) { s->bad = 1; return 0; }
    s->at = be64(h);
    if (s->hdr == 25) { s->sharer = be64(h + 8); s->key = h[16]; s->len = be32(h + 21); }
    else              { s->sharer = 0; s->key = h[12]; s->len = be32(h + 17); }
    if (s->len == 0 || s->len > (64u << 20)) { s->bad = 1; return 0; }
    if (s->len > s->cap) {
        uint8_t *g = realloc(s->data, s->len);
        if (!g) { s->bad = 1; return 0; }
        s->data = g; s->cap = s->len;
    }
    if (fread(s->data, 1, s->len, s->f) != s->len) { s->bad = 1; return 0; }
    s->peeked = 1;
    return 1;
}

static int keep_picture(vstream *s, const oc_frame *f) {
    int w = f->width, h = f->height;
    if (w <= 0 || h <= 0 || w > 8192 || h > 8192 || (w & 1) || (h & 1)) return -1;
    size_t need = (size_t)w * h * 3 / 2;
    if (!s->pic_buf || s->pic.width != w || s->pic.height != h) {
        uint8_t *g = realloc(s->pic_buf, need);
        if (!g) return -1;
        s->pic_buf = g;
    }
    s->pic.width = w; s->pic.height = h;
    s->pic.plane[0] = s->pic_buf; s->pic.stride[0] = w;
    s->pic.plane[1] = s->pic_buf + (size_t)w * h; s->pic.stride[1] = w / 2;
    s->pic.plane[2] = s->pic.plane[1] + (size_t)(w / 2) * (h / 2); s->pic.stride[2] = w / 2;
    for (int p = 0; p < 3; p++) {
        int pw = p ? w / 2 : w, ph = p ? h / 2 : h;
        for (int y = 0; y < ph; y++) memcpy(s->pic.plane[p] + (size_t)y * s->pic.stride[p],
                                            f->plane[p] + (size_t)y * f->stride[p], (size_t)pw);
    }
    s->have_pic = 1;
    return 0;
}

/* Decode what the stream had by `t`, keeping its newest picture. */
static int vstream_until(vstream *s, uint64_t t, const oc_callrec_codecs *c) {
    while (vstream_peek(s) && s->at <= t) {
        s->peeked = 0;
        if (!s->started && !s->key) continue;            /* from a keyframe */
        if (s->user == 0 && s->sharer != s->pic_sharer && s->started && !s->key) continue;
        if (s->key) s->started = 1;
        oc_frame f;
        int r = c->video_decode(s->dec, s->data, s->len, &f);
        if (r < 0) { s->started = 0; continue; }         /* wait for the next keyframe */
        if (r == 0) { if (keep_picture(s, &f) != 0) return -1; s->pic_sharer = s->sharer; }
    }
    return s->bad ? -1 : 0;
}

/* ---- the picture ---------------------------------------------------------------- */

static void fill_rect(oc_frame *dst, int x, int y, int w, int h, uint8_t Y, uint8_t U, uint8_t V) {
    for (int r = y; r < y + h; r++) memset(dst->plane[0] + (size_t)r * dst->stride[0] + x, Y, (size_t)w);
    for (int r = y / 2; r < (y + h) / 2; r++) {
        memset(dst->plane[1] + (size_t)r * dst->stride[1] + x / 2, U, (size_t)w / 2);
        memset(dst->plane[2] + (size_t)r * dst->stride[2] + x / 2, V, (size_t)w / 2);
    }
}

/* `src` fitted inside the box, its shape kept, centred, by nearest pixel. */
static void blit_fit(oc_frame *dst, int bx, int by, int bw, int bh, const oc_frame *src) {
    int w = bw, h = (int)((int64_t)src->height * bw / src->width);
    if (h > bh) { h = bh; w = (int)((int64_t)src->width * bh / src->height); }
    w &= ~1; h &= ~1;
    if (w < 2 || h < 2) return;
    int x0 = (bx + (bw - w) / 2) & ~1, y0 = (by + (bh - h) / 2) & ~1;
    for (int y = 0; y < h; y++) {
        int sy = (int)((int64_t)y * src->height / h);
        uint8_t *d = dst->plane[0] + (size_t)(y0 + y) * dst->stride[0] + x0;
        const uint8_t *s = src->plane[0] + (size_t)sy * src->stride[0];
        for (int x = 0; x < w; x++) d[x] = s[(int64_t)x * src->width / w];
    }
    for (int p = 1; p < 3; p++)
        for (int y = 0; y < h / 2; y++) {
            int sy = (int)((int64_t)y * (src->height / 2) / (h / 2));
            uint8_t *d = dst->plane[p] + (size_t)(y0 / 2 + y) * dst->stride[p] + x0 / 2;
            const uint8_t *s = src->plane[p] + (size_t)sy * src->stride[p];
            for (int x = 0; x < w / 2; x++) d[x] = s[(int64_t)x * (src->width / 2) / (w / 2)];
        }
}

/* ---- people: avatars, initials, names and the speaking ring (CALLS.md §8) ---------- */

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wcast-qual"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_GIF
#define STBI_NO_BMP
#define STBI_NO_PSD
#define STBI_NO_TGA
#define STBI_NO_HDR
#define STBI_NO_PIC
#define STBI_NO_PNM
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "../third_party/stb/stb_image.h"
#include "../third_party/stb/stb_truetype.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
/* The libraries are static to this file, and what of them it does not call
 * is reported at the end of the file, whatever the state around them: so for
 * this file, an unused static function is no error. */
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

/* The font names are drawn in (Noto Sans Regular, SIL OFL), compiled in. */
extern const unsigned char oc_callrec_font[];
extern const size_t oc_callrec_font_len;

static stbtt_fontinfo g_font;
static int g_font_ok;
static pthread_once_t g_font_once = PTHREAD_ONCE_INIT;
static void font_init(void) {
    g_font_ok = oc_callrec_font_len > 0 &&
                stbtt_InitFont(&g_font, oc_callrec_font, stbtt_GetFontOffsetForIndex(oc_callrec_font, 0)) != 0;
}
static int font_ready(void) { pthread_once(&g_font_once, font_init); return g_font_ok; }

/* The next code point of UTF-8 `s` at `*i`; 0 at the end, 0xFFFD for a bad byte. */
static uint32_t utf8_next(const char *s, size_t *i) {
    const unsigned char *p = (const unsigned char *)s + *i;
    if (!p[0]) return 0;
    uint32_t c = p[0]; int n = 0;
    if (c < 0x80) n = 0; else if ((c & 0xE0) == 0xC0) { c &= 0x1F; n = 1; }
    else if ((c & 0xF0) == 0xE0) { c &= 0x0F; n = 2; } else if ((c & 0xF8) == 0xF0) { c &= 0x07; n = 3; }
    else { (*i)++; return 0xFFFD; }
    for (int k = 1; k <= n; k++) {
        if ((p[k] & 0xC0) != 0x80) { *i += (size_t)k; return 0xFFFD; }
        c = (c << 6) | (p[k] & 0x3F);
    }
    *i += (size_t)n + 1;
    return c;
}

/* Whether the font draws every character of `s`. */
static int text_drawable(const char *s) {
    if (!font_ready() || !s || !*s) return 0;
    size_t i = 0; uint32_t c;
    while ((c = utf8_next(s, &i)) != 0) if (c != ' ' && !stbtt_FindGlyphIndex(&g_font, (int)c)) return 0;
    return 1;
}

static int text_width(const char *s, float px) {
    float scale = stbtt_ScaleForPixelHeight(&g_font, px), w = 0;
    size_t i = 0; uint32_t c, prev = 0;
    while ((c = utf8_next(s, &i)) != 0) {
        int adv, lsb;
        stbtt_GetCodepointHMetrics(&g_font, (int)c, &adv, &lsb);
        if (prev) w += scale * (float)stbtt_GetCodepointKernAdvance(&g_font, (int)prev, (int)c);
        w += scale * (float)adv;
        prev = c;
    }
    return (int)(w + 0.5f);
}

/* `s` in white, `px` high, centred on `cx` with its top at `top`, no wider
 * than `max_w` -- cut short with an ellipsis when it is. Drawn only when the
 * font has every character of it. */
static void draw_text(oc_frame *dst, int cx, int top, int max_w, const char *s, float px) {
    if (!text_drawable(s)) return;
    char buf[OC_CALLREC_NAME_MAX * 4 + 8];
    snprintf(buf, sizeof buf, "%s", s);
    while (text_width(buf, px) > max_w && strlen(buf) > 1) {
        /* Drop the last character (and any ellipsis), then add one. */
        size_t n = strlen(buf);
        if (n >= 3 && !memcmp(buf + n - 3, "\xE2\x80\xA6", 3)) { buf[n - 3] = '\0'; n -= 3; }
        while (n > 0 && ((unsigned char)buf[n - 1] & 0xC0) == 0x80) n--;
        if (n > 0) n--;
        buf[n] = '\0';
        if (!n) return;
        strcat(buf, "\xE2\x80\xA6");
    }
    float scale = stbtt_ScaleForPixelHeight(&g_font, px);
    int ascent, descent, gap;
    stbtt_GetFontVMetrics(&g_font, &ascent, &descent, &gap);
    int base = top + (int)((float)ascent * scale + 0.5f);
    float x = (float)(cx - text_width(buf, px) / 2);
    size_t i = 0; uint32_t c, prev = 0;
    while ((c = utf8_next(buf, &i)) != 0) {
        if (prev) x += scale * (float)stbtt_GetCodepointKernAdvance(&g_font, (int)prev, (int)c);
        int adv, lsb, x0, y0, x1, y1;
        stbtt_GetCodepointHMetrics(&g_font, (int)c, &adv, &lsb);
        stbtt_GetCodepointBitmapBox(&g_font, (int)c, scale, scale, &x0, &y0, &x1, &y1);
        int gw = x1 - x0, gh = y1 - y0;
        if (gw > 0 && gh > 0 && gw <= 512 && gh <= 512) {
            uint8_t *bm = malloc((size_t)gw * (size_t)gh);
            if (bm) {
                stbtt_MakeCodepointBitmap(&g_font, bm, gw, gh, gw, scale, scale, (int)c);
                int ox = (int)x + x0, oy = base + y0;
                for (int r = 0; r < gh; r++) {
                    int yy = oy + r;
                    if (yy < 0 || yy >= dst->height) continue;
                    for (int q = 0; q < gw; q++) {
                        int xx = ox + q;
                        if (xx < 0 || xx >= dst->width) continue;
                        unsigned a = bm[(size_t)r * (size_t)gw + (size_t)q];
                        if (!a) continue;
                        uint8_t *Y = dst->plane[0] + (size_t)yy * (size_t)dst->stride[0] + (size_t)xx;
                        *Y = (uint8_t)(*Y + (235 - *Y) * a / 255);
                        if (a > 128) {   /* white has no colour */
                            dst->plane[1][(size_t)(yy / 2) * (size_t)dst->stride[1] + (size_t)(xx / 2)] = 128;
                            dst->plane[2][(size_t)(yy / 2) * (size_t)dst->stride[2] + (size_t)(xx / 2)] = 128;
                        }
                    }
                }
                free(bm);
            }
        }
        x += scale * (float)adv;
        prev = c;
    }
}

/* A person's initials: the first letter of their first and last words. */
static void initials_of(const char *name, char *out, size_t cap) {
    out[0] = '\0';
    if (!name) return;
    size_t i = 0, n = 0; uint32_t c, first = 0, last = 0; int at_word = 1;
    while ((c = utf8_next(name, &i)) != 0) {
        if (c == ' ') { at_word = 1; continue; }
        if (at_word) { if (!first) first = c; else last = c; at_word = 0; }
    }
    uint32_t cs[2] = { first, last };
    for (int k = 0; k < 2 && cs[k]; k++) {
        uint32_t u = cs[k] >= 'a' && cs[k] <= 'z' ? cs[k] - 32 : cs[k];
        char e[5]; size_t el = 0;
        if (u < 0x80) e[el++] = (char)u;
        else if (u < 0x800) { e[el++] = (char)(0xC0 | (u >> 6)); e[el++] = (char)(0x80 | (u & 0x3F)); }
        else if (u < 0x10000) { e[el++] = (char)(0xE0 | (u >> 12)); e[el++] = (char)(0x80 | ((u >> 6) & 0x3F)); e[el++] = (char)(0x80 | (u & 0x3F)); }
        else { e[el++] = (char)(0xF0 | (u >> 18)); e[el++] = (char)(0x80 | ((u >> 12) & 0x3F)); e[el++] = (char)(0x80 | ((u >> 6) & 0x3F)); e[el++] = (char)(0x80 | (u & 0x3F)); }
        if (n + el + 1 > cap) break;
        memcpy(out + n, e, el); n += el;
    }
    out[n] = '\0';
}

/* Who someone is, as the picture draws them: their name, and their avatar as
 * a square I420 picture. */
#define AV_SIZE 256
typedef struct { uint64_t user; char name[OC_CALLREC_NAME_MAX + 1]; uint8_t *av; int has_av; } face;

/* An avatar's bytes into a square picture: its middle, scaled. 0, or -1. */
static int face_avatar(face *f, const uint8_t *data, size_t len) {
    if (!data || !len || len > (64u << 20)) return -1;
    int w, h, n;
    uint8_t *rgb = stbi_load_from_memory(data, (int)len, &w, &h, &n, 3);
    if (!rgb) return -1;
    uint8_t *yuv = malloc((size_t)AV_SIZE * AV_SIZE * 3 / 2);
    if (!yuv) { stbi_image_free(rgb); return -1; }
    int side = w < h ? w : h, sx = (w - side) / 2, sy = (h - side) / 2;
    uint8_t *Y = yuv, *U = yuv + AV_SIZE * AV_SIZE, *V = U + AV_SIZE * AV_SIZE / 4;
    for (int y = 0; y < AV_SIZE; y++)
        for (int x = 0; x < AV_SIZE; x++) {
            const uint8_t *p = rgb + ((size_t)(sy + y * side / AV_SIZE) * (size_t)w + (size_t)(sx + x * side / AV_SIZE)) * 3;
            int r = p[0], g = p[1], b = p[2];
            Y[y * AV_SIZE + x] = (uint8_t)((66 * r + 129 * g + 25 * b + 128) / 256 + 16);
            if (!(x & 1) && !(y & 1)) {
                U[(y / 2) * (AV_SIZE / 2) + x / 2] = (uint8_t)((-38 * r - 74 * g + 112 * b + 128) / 256 + 128);
                V[(y / 2) * (AV_SIZE / 2) + x / 2] = (uint8_t)((112 * r - 94 * g - 18 * b + 128) / 256 + 128);
            }
        }
    stbi_image_free(rgb);
    free(f->av);
    f->av = yuv;
    f->has_av = 1;
    return 0;
}

void oc_callrec_face_layout(int tile, oc_callrec_layout *o) {
    if (tile < 0) { o->x = 0; o->y = 0; o->w = OC_CALLREC_WIDTH; o->h = MAIN_H; o->name_px = 34; }
    else {
        o->x = (OC_CALLREC_WIDTH - STRIP_TILES * TILE_W) / 2 + tile * TILE_W + 4;
        o->y = MAIN_H + 4; o->w = TILE_W - 8; o->h = TILE_H - 8; o->name_px = 16;
    }
    int m = o->w < o->h ? o->w : o->h;
    o->r = (m / 4) & ~1;
    o->cx = (o->x + o->w / 2) & ~1;
    o->cy = (o->y + o->h * 42 / 100) & ~1;
    o->name_y = o->cy + o->r + o->h / 30;
}

void oc_callrec_person_colour(uint64_t user, uint8_t *Y, uint8_t *U, uint8_t *V) {
    uint64_t h = user * 0x9E3779B97F4A7C15ull;
    *Y = (uint8_t)(70 + (h >> 56) % 100);
    *U = (uint8_t)(96 + (h >> 40) % 64);
    *V = (uint8_t)(96 + (h >> 24) % 64);
}

/* The circle: the avatar inside it, or the colour. */
static void draw_disc(oc_frame *dst, int cx, int cy, int r, const face *f, uint64_t user) {
    uint8_t cY, cU, cV;
    oc_callrec_person_colour(user, &cY, &cU, &cV);
    for (int dy = -r; dy < r; dy++) {
        int yy = cy + dy;
        if (yy < 0 || yy >= dst->height) continue;
        for (int dx = -r; dx < r; dx++) {
            if (dx * dx + dy * dy > r * r) continue;
            int xx = cx + dx;
            if (xx < 0 || xx >= dst->width) continue;
            uint8_t Y = cY, U = cU, V = cV;
            if (f && f->has_av) {
                int ax = (dx + r) * AV_SIZE / (2 * r), ay = (dy + r) * AV_SIZE / (2 * r);
                Y = f->av[ay * AV_SIZE + ax];
                U = f->av[AV_SIZE * AV_SIZE + (ay / 2) * (AV_SIZE / 2) + ax / 2];
                V = f->av[AV_SIZE * AV_SIZE * 5 / 4 + (ay / 2) * (AV_SIZE / 2) + ax / 2];
            }
            dst->plane[0][(size_t)yy * (size_t)dst->stride[0] + (size_t)xx] = Y;
            if (!(yy & 1) && !(xx & 1)) {
                dst->plane[1][(size_t)(yy / 2) * (size_t)dst->stride[1] + (size_t)(xx / 2)] = U;
                dst->plane[2][(size_t)(yy / 2) * (size_t)dst->stride[2] + (size_t)(xx / 2)] = V;
            }
        }
    }
}

static void draw_ring(oc_frame *dst, int x, int y, int w, int h) {
    int t = OC_CALLREC_RING_W;
    fill_rect(dst, x, y, w, t, OC_CALLREC_RING_Y, OC_CALLREC_RING_U, OC_CALLREC_RING_V);
    fill_rect(dst, x, y + h - t, w, t, OC_CALLREC_RING_Y, OC_CALLREC_RING_U, OC_CALLREC_RING_V);
    fill_rect(dst, x, y, t, h, OC_CALLREC_RING_Y, OC_CALLREC_RING_U, OC_CALLREC_RING_V);
    fill_rect(dst, x + w - t, y, t, h, OC_CALLREC_RING_Y, OC_CALLREC_RING_U, OC_CALLREC_RING_V);
}

/* One person in box `tile` (-1 the large one): their camera while it is on and
 * has a picture, else their circle; their name under it (or, on a camera, on a
 * band across its foot); the ring while they speak. */
static void draw_person(oc_frame *dst, int tile, uint64_t user, const face *f, const vstream *cam, int cam_on,
                        int speaking) {
    oc_callrec_layout L;
    oc_callrec_face_layout(tile, &L);
    fill_rect(dst, L.x, L.y, L.w, L.h, 16, 128, 128);
    const char *name = f && f->name[0] ? f->name : NULL;
    if (cam && cam_on && cam->have_pic) {
        blit_fit(dst, L.x, L.y, L.w, L.h, &cam->pic);
        if (name) {
            int band = ((int)((float)L.name_px * 1.5f)) & ~1;
            fill_rect(dst, L.x, (L.y + L.h - band) & ~1, L.w, band, 16, 128, 128);
            draw_text(dst, L.cx, L.y + L.h - band + L.name_px / 5, L.w - 16, name, (float)L.name_px);
        }
    } else {
        draw_disc(dst, L.cx, L.cy, L.r, f, user);
        if (!(f && f->has_av) && name) {
            char ini[16];
            initials_of(name, ini, sizeof ini);
            float px = (float)L.r * 0.75f;
            if (ini[0]) draw_text(dst, L.cx, L.cy - (int)(px * 0.6f), 2 * L.r, ini, px);
        }
        if (name) draw_text(dst, L.cx, L.name_y, L.w - 16, name, (float)L.name_px);
    }
    if (speaking) draw_ring(dst, L.x, L.y, L.w, L.h);
}

/* The spool's `people` file, and their avatars. */
static int load_faces(const char *dir, const oc_callrec_assets *assets, face *out, int cap) {
    char path[1200];
    snprintf(path, sizeof path, "%s/people", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    int n = 0;
    uint8_t h[9];
    while (n < cap && fread(h, 1, sizeof h, f) == sizeof h) {
        face *p = &out[n];
        memset(p, 0, sizeof *p);
        p->user = be64(h);
        size_t nl = h[8];
        if (nl > OC_CALLREC_NAME_MAX || fread(p->name, 1, nl, f) != nl) break;
        p->name[nl] = '\0';
        uint8_t kl;
        char key[OC_CALLREC_AVKEY_MAX + 1];
        if (fread(&kl, 1, 1, f) != 1 || kl > OC_CALLREC_AVKEY_MAX || fread(key, 1, kl, f) != kl) break;
        key[kl] = '\0';
        if (kl && assets && assets->load) {
            uint8_t *data = NULL; size_t len = 0;
            if (assets->load(assets->ctx, key, &data, &len) == 0) face_avatar(p, data, len);
            free(data);
        }
        n++;
    }
    fclose(f);
    return n;
}

static const face *face_of(const face *faces, int n, uint64_t user) {
    for (int i = 0; i < n; i++) if (faces[i].user == user) return &faces[i];
    return NULL;
}

int oc_callrec_compose(uint8_t *i420, const oc_callrec_face *in, int n) {
    if (!i420 || n < 0) return -1;
    oc_frame cv;
    memset(&cv, 0, sizeof cv);
    cv.width = OC_CALLREC_WIDTH; cv.height = OC_CALLREC_HEIGHT;
    cv.plane[0] = i420;
    cv.plane[1] = i420 + OC_CALLREC_WIDTH * OC_CALLREC_HEIGHT;
    cv.plane[2] = i420 + OC_CALLREC_WIDTH * OC_CALLREC_HEIGHT * 5 / 4;
    cv.stride[0] = OC_CALLREC_WIDTH; cv.stride[1] = cv.stride[2] = OC_CALLREC_WIDTH / 2;
    fill_rect(&cv, 0, 0, OC_CALLREC_WIDTH, OC_CALLREC_HEIGHT, 24, 128, 128);
    for (int i = 0; i < n && i <= STRIP_TILES; i++) {
        face f;
        memset(&f, 0, sizeof f);
        f.user = in[i].user;
        snprintf(f.name, sizeof f.name, "%s", in[i].name ? in[i].name : "");
        if (in[i].avatar) face_avatar(&f, in[i].avatar, in[i].avatar_len);
        draw_person(&cv, i - 1, f.user, &f, NULL, 0, in[i].speaking);
        free(f.av);
    }
    return 0;
}

/* ---- the render ---------------------------------------------------------------- */

typedef struct { oc_mp4_writer *mp4; int64_t pts_us; int bad; } emit_ctx;
static void emit_video(void *ctx, const uint8_t *data, size_t len, int keyframe) {
    emit_ctx *e = ctx;
    if (!e->bad && oc_mp4_write_video(e->mp4, data, len, e->pts_us, keyframe) != 0) e->bad = 1;
}

static int read_meta(const char *dir, uint64_t *started) {
    char path[1200];
    snprintf(path, sizeof path, "%s/meta", dir);
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    uint8_t m[30];
    int ok = fread(m, 1, sizeof m, f) == sizeof m && memcmp(m, "OCREC1", 6) == 0;
    fclose(f);
    if (ok) *started = be64(m + 22);
    return ok ? 0 : -1;
}

static int read_timeline(const char *dir, tl_event **out, size_t *n) {
    char path[1200];
    snprintf(path, sizeof path, "%s/timeline", dir);
    *out = NULL; *n = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;                                  /* none: nothing happened */
    size_t cap = 0;
    uint8_t r[17];
    size_t got;
    int bad = 0;
    while ((got = fread(r, 1, sizeof r, f)) > 0) {
        if (got != sizeof r) { bad = 1; break; }
        if (*n == cap) {
            cap = cap ? cap * 2 : 64;
            tl_event *g = realloc(*out, cap * sizeof *g);
            if (!g) { bad = 1; break; }
            *out = g;
        }
        (*out)[(*n)++] = (tl_event){ be64(r), r[8], be64(r + 9) };
    }
    fclose(f);
    return bad ? -1 : 0;
}

/* Who is in the call at `t`, and whose camera and screen are on, by the
 * timeline (everyone in it when the recording began is told as a join). */
typedef struct { uint64_t user; int in, cam; } person;

static person *person_of(person *pp, int *np, uint64_t user) {
    for (int i = 0; i < *np; i++) if (pp[i].user == user) return &pp[i];
    if (*np == MAX_PEOPLE) return NULL;
    pp[*np] = (person){ user, 0, 0 };
    return &pp[(*np)++];
}

/* Whether `user` is speaking now: their voice above the speaking level. */
#define SPEAK_LEVEL 150.0
static int voice_speaking(const voice *vo, int nvo, uint64_t user) {
    for (int i = 0; i < nvo; i++) if (vo[i].user == user && vo[i].level > SPEAK_LEVEL) return 1;
    return 0;
}

oc_callrec_status oc_callrec_render(const oc_callrec_codecs *c, const oc_callrec_assets *assets,
                                    const char *dir, const char *out_path,
                                    const volatile int *cancel, uint64_t *bytes, uint32_t *duration_ms,
                                    char *err, size_t errcap) {
    if (!c || !dir || !out_path) { seterr(err, errcap, "bad request"); return OC_CALLREC_FAILED; }
    uint64_t t0 = 0;
    if (read_meta(dir, &t0) != 0) { seterr(err, errcap, "the spool has no meta"); return OC_CALLREC_FAILED; }
    tl_event *tl = NULL; size_t ntl = 0;
    if (read_timeline(dir, &tl, &ntl) != 0) { free(tl); seterr(err, errcap, "the timeline is damaged"); return OC_CALLREC_FAILED; }

    voice   *vo = calloc(MAX_PEOPLE, sizeof *vo);
    vstream *cams = calloc(MAX_PEOPLE, sizeof *cams);
    vstream  share;
    memset(&share, 0, sizeof share);
    int nvo = 0, ncam = 0;
    oc_callrec_status st = OC_CALLREC_FAILED;
    oc_mp4_writer *mp4 = NULL;
    void *venc = NULL, *aenc = NULL;
    uint8_t *canvas = NULL;
    person people[MAX_PEOPLE];
    int npeople = 0;
    /* Who each is: their names and avatars (the `people` file). */
    face *faces = calloc(MAX_PEOPLE * 2, sizeof *faces);
    int nfaces = faces ? load_faces(dir, assets, faces, MAX_PEOPLE * 2) : 0;
    uint64_t t_end = t0;
    if (!vo || !cams) { seterr(err, errcap, "out of memory"); goto done; }

    /* The streams, by their files' names. */
    DIR *d = opendir(dir);
    if (!d) { seterr(err, errcap, "the spool cannot be read"); goto done; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        char path[1200];
        snprintf(path, sizeof path, "%s/%s", dir, de->d_name);
        if (!strncmp(de->d_name, "audio.", 6) && nvo < MAX_PEOPLE) {
            voice *v = &vo[nvo];
            v->user = strtoull(de->d_name + 6, NULL, 10);
            if (!v->user || !(v->f = fopen(path, "rb"))) continue;
            nvo++;
        } else if (!strncmp(de->d_name, "camera.", 7) && ncam < MAX_PEOPLE) {
            vstream *s = &cams[ncam];
            s->user = strtoull(de->d_name + 7, NULL, 10);
            s->hdr = 21;
            if (!s->user || !(s->f = fopen(path, "rb"))) continue;
            ncam++;
        } else if (!strcmp(de->d_name, "share")) {
            share.hdr = 25;
            share.f = fopen(path, "rb");
        }
    }
    closedir(d);
    if (!nvo && !ncam && !share.f) { st = OC_CALLREC_EMPTY; seterr(err, errcap, "nothing was recorded"); goto done; }

    /* How long it runs: to the last thing anyone sent. A first pass over each
     * file's headers, which also finds a damaged file before any work. */
    for (int i = 0; i < nvo; i++) {
        uint8_t h[14];
        size_t got;
        while ((got = fread(h, 1, sizeof h, vo[i].f)) == sizeof h) {
            uint64_t at = be64(h);
            if (at > t_end) t_end = at;
            if (fseek(vo[i].f, (long)be16(h + 12), SEEK_CUR) != 0) { got = 1; break; }
        }
        if (got != 0) { seterr(err, errcap, "a voice's spool is damaged"); goto done; }
        rewind(vo[i].f);
        if (!(vo[i].dec = c->audio_dec_open())) { seterr(err, errcap, "no audio decoder"); goto done; }
        person_of(people, &npeople, vo[i].user);
    }
    vstream *all[MAX_PEOPLE + 1];
    int nall = 0;
    for (int i = 0; i < ncam; i++) all[nall++] = &cams[i];
    if (share.f) all[nall++] = &share;
    for (int i = 0; i < nall; i++) {
        vstream *s = all[i];
        while (vstream_peek(s)) { s->peeked = 0; if (s->at > t_end) t_end = s->at; }
        if (s->bad) { seterr(err, errcap, "a picture's spool is damaged"); goto done; }
        rewind(s->f);
        s->eof = 0;
        if (!(s->dec = c->video_dec_open())) { seterr(err, errcap, "no video decoder"); goto done; }
        if (s->user) person_of(people, &npeople, s->user);
    }
    for (size_t i = 0; i < ntl; i++)
        if (tl[i].kind == OC_CALLREC_TL_JOIN) person_of(people, &npeople, tl[i].user);
    if (t_end <= t0) t_end = t0 + 1000 / OC_CALLREC_FPS;

    mp4 = oc_mp4_writer_open_file(out_path, OC_CALLREC_WIDTH, OC_CALLREC_HEIGHT);
    venc = c->video_enc_open();
    aenc = c->audio_enc_open();
    canvas = malloc((size_t)OC_CALLREC_WIDTH * OC_CALLREC_HEIGHT * 3 / 2);
    if (!mp4 || !venc || !aenc || !canvas) { seterr(err, errcap, "the recording cannot be started"); goto done; }
    oc_frame cv;
    memset(&cv, 0, sizeof cv);
    cv.width = OC_CALLREC_WIDTH; cv.height = OC_CALLREC_HEIGHT;
    cv.plane[0] = canvas;
    cv.plane[1] = canvas + OC_CALLREC_WIDTH * OC_CALLREC_HEIGHT;
    cv.plane[2] = canvas + OC_CALLREC_WIDTH * OC_CALLREC_HEIGHT * 5 / 4;
    cv.stride[0] = OC_CALLREC_WIDTH; cv.stride[1] = cv.stride[2] = OC_CALLREC_WIDTH / 2;

    /* Each voice starts where its first frame was had. */
    for (int i = 0; i < nvo; i++) voice_fill(&vo[i]);

    uint64_t total_slots = (t_end - t0 + 19) / 20;
    /* The profile's bound (oc_mp4.h): its audio and video samples together --
     * 50 a second and 15 a second -- about 51 minutes. Longer is refused, said
     * so, rather than made and cut short. */
    if (total_slots + total_slots * OC_CALLREC_FPS / 50 >= OC_MP4_MAX_SAMPLES) {
        st = OC_CALLREC_FAILED;
        seterr(err, errcap, "longer than a recording can hold (about 51 minutes)");
        goto done;
    }
    uint64_t vframe = 0;
    size_t ti = 0;
    uint64_t speaker = 0, rival = 0, rival_since = 0;
    int share_on = 0;
    uint64_t sharer = 0;
    emit_ctx ec = { mp4, 0, 0 };
    for (uint64_t slot = 0; slot < total_slots; slot++) {
        if (cancel && *cancel) { st = OC_CALLREC_CANCELLED; seterr(err, errcap, "cancelled"); goto done; }
        uint64_t t = t0 + slot * 20;
        /* The audio: each voice's frame for this slot, mixed. */
        int32_t mix[OC_CALLREC_FRAME];
        memset(mix, 0, sizeof mix);
        for (int i = 0; i < nvo; i++) {
            voice *v = &vo[i];
            if (!v->anchored || t + 20 <= v->first_at) continue;   /* not yet speaking here */
            int said = voice_frame(v, c, t);
            if (said < 0) { seterr(err, errcap, "a voice cannot be decoded"); goto done; }
            double lvl = 0;
            for (int k = 0; k < OC_CALLREC_FRAME; k++) { mix[k] += v->pcm[k]; lvl += abs(v->pcm[k]); }
            lvl /= OC_CALLREC_FRAME;
            v->level = v->level * 0.85 + lvl * 0.15;
        }
        int16_t out[OC_CALLREC_FRAME];
        for (int k = 0; k < OC_CALLREC_FRAME; k++) {
            /* Soft above 3/4 of full scale, so two loud voices do not clip. */
            int32_t s = mix[k], a = s < 0 ? -s : s;
            if (a > 24576) a = 24576 + (a - 24576) / 4;
            if (a > 32767) a = 32767;
            out[k] = (int16_t)(s < 0 ? -a : a);
        }
        uint8_t pkt[1500];
        int plen = c->audio_encode(aenc, out, pkt, sizeof pkt);
        if (plen <= 0 || oc_mp4_write_audio(mp4, pkt, (size_t)plen, OC_CALLREC_FRAME) != 0) {
            seterr(err, errcap, "the recording cannot be written: larger than 256 MB, or the disk is full"); goto done;
        }
        /* Who is speaking: the loudest, once they have out-spoken the speaker
         * for a while; the speaker stays until then. */
        double best = 0; uint64_t loud = 0;
        for (int i = 0; i < nvo; i++) if (vo[i].level > best) { best = vo[i].level; loud = vo[i].user; }
        if (best > SPEAK_LEVEL && loud != speaker) {
            if (!speaker) speaker = loud;
            else if (loud != rival) { rival = loud; rival_since = t; }
            else if (t - rival_since >= SWITCH_MS) { speaker = loud; rival = 0; }
        } else if (loud == speaker) {
            rival = 0;
        }
        /* The picture, at the recording's frame rate. */
        while (vframe * 1000 / OC_CALLREC_FPS <= slot * 20) {
            uint64_t tv = t0 + vframe * 1000 / OC_CALLREC_FPS;
            for (; ti < ntl && tl[ti].at <= tv; ti++) {
                person *p = person_of(people, &npeople, tl[ti].user);
                switch (tl[ti].kind) {
                case OC_CALLREC_TL_JOIN:       if (p) p->in = 1; break;
                case OC_CALLREC_TL_LEAVE:      if (p) { p->in = 0; p->cam = 0; } break;
                case OC_CALLREC_TL_CAMERA_ON:  if (p) p->cam = 1; break;
                case OC_CALLREC_TL_CAMERA_OFF: if (p) p->cam = 0; break;
                case OC_CALLREC_TL_SHARE_ON:   share_on = 1; sharer = tl[ti].user; break;
                case OC_CALLREC_TL_SHARE_OFF:  if (sharer == tl[ti].user) share_on = 0; break;
                }
            }
            for (int i = 0; i < nall; i++)
                if (vstream_until(all[i], tv, c) != 0) { seterr(err, errcap, "a picture cannot be read"); goto done; }
            /* Everyone heard or seen counts as in, whatever the timeline missed. */
            for (int i = 0; i < nvo; i++) if (vo[i].anchored && vo[i].first_at <= tv) {
                person *p = person_of(people, &npeople, vo[i].user); if (p && !p->in) p->in = 1; }
            if (!speaker) for (int i = 0; i < npeople; i++) if (people[i].in) { speaker = people[i].user; break; }
            fill_rect(&cv, 0, 0, OC_CALLREC_WIDTH, OC_CALLREC_HEIGHT, 24, 128, 128);
            int sharing = share_on && share.have_pic && share.pic_sharer == sharer;
            if (sharing) {
                fill_rect(&cv, 0, 0, OC_CALLREC_WIDTH, MAIN_H, 16, 128, 128);
                blit_fit(&cv, 0, 0, OC_CALLREC_WIDTH, MAIN_H, &share.pic);
            } else if (speaker) {
                const vstream *cam = NULL; int cam_on = 0;
                for (int i = 0; i < ncam; i++) if (cams[i].user == speaker) cam = &cams[i];
                for (int i = 0; i < npeople; i++) if (people[i].user == speaker) cam_on = people[i].cam;
                draw_person(&cv, -1, speaker, face_of(faces, nfaces, speaker), cam, cam_on,
                            voice_speaking(vo, nvo, speaker));
            }
            /* The strip: everyone in but the one large. */
            int tile = 0;
            for (int i = 0; i < npeople && tile < STRIP_TILES; i++) {
                if (!people[i].in || (!sharing && people[i].user == speaker)) continue;
                const vstream *cam = NULL;
                for (int k = 0; k < ncam; k++) if (cams[k].user == people[i].user) cam = &cams[k];
                draw_person(&cv, tile, people[i].user, face_of(faces, nfaces, people[i].user), cam, people[i].cam,
                            voice_speaking(vo, nvo, people[i].user));
                tile++;
            }
            ec.pts_us = (int64_t)(vframe * 1000000 / OC_CALLREC_FPS);
            cv.pts_us = ec.pts_us;
            if (c->video_encode(venc, &cv, vframe % KEY_EVERY == 0, emit_video, &ec) != 0 || ec.bad) {
                seterr(err, errcap, "the picture cannot be written"); goto done;
            }
            vframe++;
        }
    }
    if (c->video_encode(venc, NULL, 0, emit_video, &ec) != 0 || ec.bad) {
        seterr(err, errcap, "the picture cannot be finished"); goto done;
    }
    int rc = oc_mp4_writer_finish_file(mp4, bytes, duration_ms);
    mp4 = NULL;
    if (rc != 0) { seterr(err, errcap, "the recording is longer than a file of it may be, or cannot be written"); goto done; }
    st = OC_CALLREC_OK;

done:
    if (mp4) oc_mp4_writer_abort(mp4);
    if (venc) c->video_enc_close(venc);
    if (aenc) c->audio_enc_close(aenc);
    free(canvas);
    for (int i = 0; vo && i < nvo; i++) { if (vo[i].f) fclose(vo[i].f); if (vo[i].dec) c->audio_dec_close(vo[i].dec); }
    for (int i = 0; cams && i < ncam; i++) {
        if (cams[i].f) fclose(cams[i].f);
        if (cams[i].dec) c->video_dec_close(cams[i].dec);
        free(cams[i].data); free(cams[i].pic_buf);
    }
    if (share.f) fclose(share.f);
    if (share.dec) c->video_dec_close(share.dec);
    free(share.data); free(share.pic_buf);
    for (int i = 0; i < nfaces; i++) free(faces[i].av);
    free(faces);
    free(vo); free(cams); free(tl);
    return st;
}

/* ---- the stand-in codecs ----------------------------------------------------------- */

typedef struct { int16_t last; } stub_adec_t;
static void *stub_adec_open(void) { return calloc(1, sizeof(stub_adec_t)); }
static int stub_adec(void *d, const uint8_t *pkt, size_t len, int16_t *pcm) {
    stub_adec_t *s = d;
    if (pkt && len) s->last = (int16_t)(pkt[0] * 100);
    for (int i = 0; i < OC_CALLREC_FRAME; i++) pcm[i] = s->last;
    return 0;
}
static void stub_close(void *d) { free(d); }

typedef struct { uint8_t buf[64 * 48 * 3 / 2]; } stub_vdec_t;
static void *stub_vdec_open(void) { return calloc(1, sizeof(stub_vdec_t)); }
static int stub_vdec(void *d, const uint8_t *data, size_t len, oc_frame *out) {
    stub_vdec_t *s = d;
    if (!len) return -1;
    memset(s->buf, data[0], 64 * 48);
    memset(s->buf + 64 * 48, 128, 64 * 48 / 2);
    memset(out, 0, sizeof *out);
    out->width = 64; out->height = 48;
    out->plane[0] = s->buf; out->plane[1] = s->buf + 64 * 48; out->plane[2] = s->buf + 64 * 48 * 5 / 4;
    out->stride[0] = 64; out->stride[1] = 32; out->stride[2] = 32;
    return 0;
}

static void *stub_venc_open(void) { return calloc(1, 1); }
static int stub_venc(void *e, const oc_frame *f, int force_key,
                     void (*emit)(void *ctx, const uint8_t *data, size_t len, int keyframe), void *ctx) {
    (void)e; (void)force_key;
    if (!f) return 0;
    uint8_t out[1 + OC_CALLREC_STUB_TILES];
    /* Each box sampled inside the top of its circle: clear of initials and
     * names, and inside a camera's or a share's picture. */
    for (int i = -1; i < OC_CALLREC_STUB_TILES; i++) {
        oc_callrec_layout L;
        oc_callrec_face_layout(i, &L);
        int py = L.cy - L.r * 7 / 10;
        out[1 + i] = i < STRIP_TILES ? f->plane[0][(size_t)py * f->stride[0] + (size_t)L.cx] : 0;
    }
    emit(ctx, out, sizeof out, 1);
    return 0;
}

static void *stub_aenc_open(void) { return calloc(1, 1); }
static int stub_aenc(void *e, const int16_t *pcm, uint8_t *out, size_t cap) {
    (void)e;
    if (cap < 2) return -1;
    long sum = 0;
    for (int i = 0; i < OC_CALLREC_FRAME; i++) sum += abs(pcm[i]);
    long mean = sum / OC_CALLREC_FRAME;
    out[0] = (uint8_t)(mean >> 8); out[1] = (uint8_t)mean;
    return 2;
}

const oc_callrec_codecs oc_callrec_stub_codecs = {
    "stub-1",
    stub_adec_open, stub_adec, stub_close,
    stub_vdec_open, stub_vdec, stub_close,
    stub_venc_open, stub_venc, stub_close,
    stub_aenc_open, stub_aenc, stub_close,
};

#ifndef OC_CALLREC
/* Built without CALLREC: no engine, and nobody records a call here. */
const oc_callrec_codecs *oc_callrec_codecs_builtin(void) { return NULL; }
#endif
