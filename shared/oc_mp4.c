/* The restricted MP4 writer and reader (oc_mp4.h).
 *
 * Box layout written (docs/details/VIDEO-MESSAGES.md §5):
 *   ftyp · moov{ mvhd · trak(video) · trak(audio) } · mdat
 * or, audio-only (read-aloud):
 *   ftyp · moov{ mvhd · trak(audio) } · mdat
 * The whole file is assembled in memory (ARCH-88: a client writes no files).
 * Each sample is its own chunk (stsc has one entry), which keeps the tables
 * trivially correct for interleaved writing and costs a few bytes a sample.
 * VP9 has no frame reordering, so there is no ctts.
 *
 * References: ISO/IEC 14496-12 (ISO BMFF); "VP Codec ISO Media File Format
 * Binding" (vp09 / vpcC); "Encapsulation of Opus in ISO Base Media File Format"
 * (Opus / dOps). */
#define _POSIX_C_SOURCE 200809L
#include "oc_mp4.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(_WIN32)
#include <sys/types.h>
#include <unistd.h>
#endif

/* ---- a growable big-endian byte buffer ---------------------------------------- */

typedef struct { uint8_t *d; size_t n, cap; int bad; } bbuf;

static void bb_need(bbuf *b, size_t more) {
    if (b->bad) return;
    if (b->n + more <= b->cap) return;
    size_t cap = b->cap ? b->cap * 2 : 4096;
    while (cap < b->n + more) cap *= 2;
    uint8_t *d = realloc(b->d, cap);
    if (!d) { b->bad = 1; return; }
    b->d = d; b->cap = cap;
}
static void u8(bbuf *b, uint8_t v)   { bb_need(b, 1); if (!b->bad) b->d[b->n++] = v; }
static void u16(bbuf *b, uint16_t v) { u8(b, (uint8_t)(v >> 8)); u8(b, (uint8_t)v); }
static void u32(bbuf *b, uint32_t v) { u16(b, (uint16_t)(v >> 16)); u16(b, (uint16_t)v); }
static void bytes(bbuf *b, const void *p, size_t n) {
    bb_need(b, n); if (!b->bad) { memcpy(b->d + b->n, p, n); b->n += n; }
}
static void fourcc(bbuf *b, const char *s) { bytes(b, s, 4); }
static void zeros(bbuf *b, size_t n) { for (size_t i = 0; i < n; i++) u8(b, 0); }

/* Open a box: write a placeholder size and the type, return where it started. */
static size_t box_begin(bbuf *b, const char *type) { size_t at = b->n; u32(b, 0); fourcc(b, type); return at; }
static size_t fullbox_begin(bbuf *b, const char *type, uint8_t version, uint32_t flags) {
    size_t at = box_begin(b, type); u32(b, ((uint32_t)version << 24) | (flags & 0xFFFFFF)); return at;
}
static void box_end(bbuf *b, size_t at) {
    if (b->bad) return;
    uint32_t sz = (uint32_t)(b->n - at);
    b->d[at] = (uint8_t)(sz >> 24); b->d[at + 1] = (uint8_t)(sz >> 16);
    b->d[at + 2] = (uint8_t)(sz >> 8); b->d[at + 3] = (uint8_t)sz;
}

static void unity_matrix(bbuf *b) {
    u32(b, 0x00010000); u32(b, 0); u32(b, 0);
    u32(b, 0); u32(b, 0x00010000); u32(b, 0);
    u32(b, 0); u32(b, 0); u32(b, 0x40000000);
}

/* ---- writing ---------------------------------------------------------------------- */

typedef struct { uint64_t off; uint32_t size; uint32_t dur; uint8_t sync; } wsample;

typedef struct { wsample *s; uint32_t n, cap; } wtrack;

struct oc_mp4_writer {
    bbuf     data;                  /* sample bytes, in the order written: mdat's payload */
    FILE    *spill;                 /* ...or, opened on a file, where they go instead */
    char    *path;                  /* that file's: the finished file's path */
    uint64_t spilled;
    int      width, height;         /* 0 × 0: audio-only */
    unsigned input_rate;            /* dOps input sample rate */
    wtrack   video, audio;
    int64_t  last_pts_us;
    int      have_video;
};

static int wt_push(wtrack *t, wsample s) {
    if (t->n == t->cap) {
        uint32_t cap = t->cap ? t->cap * 2 : 1024;
        wsample *ns = realloc(t->s, cap * sizeof *ns);
        if (!ns) return -1;
        t->s = ns; t->cap = cap;
    }
    t->s[t->n++] = s;
    return 0;
}

oc_mp4_writer *oc_mp4_writer_open(int width, int height) {
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) return NULL;
    oc_mp4_writer *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->width = width; w->height = height;
    w->input_rate = OC_MP4_AUDIO_TIMESCALE;
    return w;
}

oc_mp4_writer *oc_mp4_writer_open_audio(unsigned input_rate) {
    if (input_rate == 0) return NULL;
    oc_mp4_writer *w = calloc(1, sizeof *w);
    if (!w) return NULL;
    w->input_rate = input_rate;
    return w;
}

static int data_append(oc_mp4_writer *w, const uint8_t *data, size_t len, uint64_t *off) {
    if (w->video.n + w->audio.n >= OC_MP4_MAX_SAMPLES) return -1;
    if (w->spill) {
        if (len == 0 || w->spilled + len > OC_MP4_MAX_FILE) return -1;
        if (fwrite(data, 1, len, w->spill) != len) return -1;
        *off = w->spilled;
        w->spilled += len;
        return 0;
    }
    if (len == 0 || len > OC_MP4_MAX_FILE || w->data.n + len > OC_MP4_MAX_FILE) return -1;
    *off = w->data.n;
    bytes(&w->data, data, len);
    return w->data.bad ? -1 : 0;
}

uint64_t oc_mp4_writer_bytes(const oc_mp4_writer *w) { return w ? (w->spill ? w->spilled : w->data.n) : 0; }

int oc_mp4_write_video(oc_mp4_writer *w, const uint8_t *data, size_t len, int64_t pts_us, int keyframe) {
    if (!w || !data || w->width == 0) return -1;
    if (!w->have_video) {
        if (!keyframe) return -1;                 /* a track must start on a keyframe */
        w->last_pts_us = pts_us; w->have_video = 1;
    } else {
        if (pts_us < w->last_pts_us) return -1;
        /* The previous frame lasts until this one: its duration is the gap. */
        wsample *prev = &w->video.s[w->video.n - 1];
        uint64_t gap = (uint64_t)(pts_us - w->last_pts_us);
        uint64_t d = (gap * OC_MP4_VIDEO_TIMESCALE + 500000) / 1000000;
        prev->dur = (uint32_t)(d ? d : 1);
        w->last_pts_us = pts_us;
    }
    uint64_t off;
    if (data_append(w, data, len, &off) != 0) return -1;
    /* Provisional duration: one 30 fps frame, replaced when the next arrives. */
    return wt_push(&w->video, (wsample){ off, (uint32_t)len, OC_MP4_VIDEO_TIMESCALE / 30, (uint8_t)(keyframe ? 1 : 0) });
}

int oc_mp4_write_audio(oc_mp4_writer *w, const uint8_t *data, size_t len, unsigned samples) {
    if (!w || !data || samples == 0) return -1;
    uint64_t off;
    if (data_append(w, data, len, &off) != 0) return -1;
    return wt_push(&w->audio, (wsample){ off, (uint32_t)len, samples, 1 });
}

static uint64_t track_duration(const wtrack *t) {
    uint64_t d = 0;
    for (uint32_t i = 0; i < t->n; i++) d += t->s[i].dur;
    return d;
}

/* stts: run-length (count, delta). */
static void write_stts(bbuf *b, const wtrack *t) {
    size_t at = fullbox_begin(b, "stts", 0, 0);
    size_t count_at = b->n; u32(b, 0);
    uint32_t entries = 0;
    for (uint32_t i = 0; i < t->n;) {
        uint32_t j = i;
        while (j < t->n && t->s[j].dur == t->s[i].dur) j++;
        u32(b, j - i); u32(b, t->s[i].dur);
        entries++; i = j;
    }
    if (!b->bad) { b->d[count_at] = (uint8_t)(entries >> 24); b->d[count_at + 1] = (uint8_t)(entries >> 16);
                   b->d[count_at + 2] = (uint8_t)(entries >> 8); b->d[count_at + 3] = (uint8_t)entries; }
    box_end(b, at);
}

static void write_sample_tables(bbuf *b, const wtrack *t, uint64_t data_base, int video) {
    write_stts(b, t);
    if (video) {
        size_t at = fullbox_begin(b, "stss", 0, 0);
        uint32_t n = 0;
        for (uint32_t i = 0; i < t->n; i++) n += t->s[i].sync;
        u32(b, n);
        for (uint32_t i = 0; i < t->n; i++) if (t->s[i].sync) u32(b, i + 1);
        box_end(b, at);
    }
    size_t at = fullbox_begin(b, "stsc", 0, 0);
    u32(b, 1); u32(b, 1); u32(b, 1); u32(b, 1);           /* every chunk holds one sample */
    box_end(b, at);
    at = fullbox_begin(b, "stsz", 0, 0);
    u32(b, 0); u32(b, t->n);
    for (uint32_t i = 0; i < t->n; i++) u32(b, t->s[i].size);
    box_end(b, at);
    at = fullbox_begin(b, "stco", 0, 0);
    u32(b, t->n);
    for (uint32_t i = 0; i < t->n; i++) u32(b, (uint32_t)(data_base + t->s[i].off));
    box_end(b, at);
}

static void write_dinf(bbuf *b) {
    size_t dinf = box_begin(b, "dinf");
    size_t dref = fullbox_begin(b, "dref", 0, 0);
    u32(b, 1);
    size_t url = fullbox_begin(b, "url ", 0, 1);          /* flag 1: data in this file */
    box_end(b, url);
    box_end(b, dref);
    box_end(b, dinf);
}

static void write_hdlr(bbuf *b, const char *type, const char *name) {
    size_t at = fullbox_begin(b, "hdlr", 0, 0);
    u32(b, 0); fourcc(b, type); zeros(b, 12);
    bytes(b, name, strlen(name) + 1);
    box_end(b, at);
}

static void write_tkhd(bbuf *b, uint32_t id, uint64_t dur_ms, int video, int width, int height) {
    size_t at = fullbox_begin(b, "tkhd", 0, 3);           /* enabled, in movie */
    u32(b, 0); u32(b, 0); u32(b, id); u32(b, 0);
    u32(b, (uint32_t)dur_ms);
    zeros(b, 8);
    u16(b, 0); u16(b, 0);                                  /* layer, alternate group */
    u16(b, video ? 0 : 0x0100); u16(b, 0);                 /* volume */
    unity_matrix(b);
    u32(b, video ? (uint32_t)width << 16 : 0);
    u32(b, video ? (uint32_t)height << 16 : 0);
    box_end(b, at);
}

static void write_mdhd(bbuf *b, uint32_t timescale, uint64_t duration) {
    size_t at = fullbox_begin(b, "mdhd", 0, 0);
    u32(b, 0); u32(b, 0); u32(b, timescale); u32(b, (uint32_t)duration);
    u16(b, 0x55C4);                                        /* "und" */
    u16(b, 0);
    box_end(b, at);
}

static void build_moov(bbuf *b, const oc_mp4_writer *w, uint64_t data_base) {
    uint64_t vdur = track_duration(&w->video), adur = track_duration(&w->audio);
    uint64_t vms = vdur * 1000 / OC_MP4_VIDEO_TIMESCALE;
    uint64_t audible = adur > OC_MP4_OPUS_PRESKIP ? adur - OC_MP4_OPUS_PRESKIP : 0;
    uint64_t ams = audible * 1000 / OC_MP4_AUDIO_TIMESCALE;
    uint64_t movie_ms = vms > ams ? vms : ams;
    uint32_t next_id = 1;

    size_t moov = box_begin(b, "moov");
    size_t mvhd = fullbox_begin(b, "mvhd", 0, 0);
    u32(b, 0); u32(b, 0); u32(b, 1000); u32(b, (uint32_t)movie_ms);
    u32(b, 0x00010000); u16(b, 0x0100); zeros(b, 10);
    unity_matrix(b); zeros(b, 24);
    u32(b, (w->video.n ? 1u : 0u) + (w->audio.n ? 1u : 0u) + 1u);
    box_end(b, mvhd);

    if (w->video.n) {
        size_t trak = box_begin(b, "trak");
        write_tkhd(b, next_id++, vms, 1, w->width, w->height);
        size_t mdia = box_begin(b, "mdia");
        write_mdhd(b, OC_MP4_VIDEO_TIMESCALE, vdur);
        write_hdlr(b, "vide", "OpenChime video");
        size_t minf = box_begin(b, "minf");
        size_t vmhd = fullbox_begin(b, "vmhd", 0, 1); u16(b, 0); zeros(b, 6); box_end(b, vmhd);
        write_dinf(b);
        size_t stbl = box_begin(b, "stbl");
        size_t stsd = fullbox_begin(b, "stsd", 0, 0); u32(b, 1);
        size_t vp09 = box_begin(b, "vp09");
        zeros(b, 6); u16(b, 1);                            /* reserved, data reference index */
        u16(b, 0); u16(b, 0); zeros(b, 12);
        u16(b, (uint16_t)w->width); u16(b, (uint16_t)w->height);
        u32(b, 0x00480000); u32(b, 0x00480000); u32(b, 0);
        u16(b, 1);                                         /* frame count */
        zeros(b, 32);                                      /* compressor name */
        u16(b, 0x0018); u16(b, 0xFFFF);
        size_t vpcc = fullbox_begin(b, "vpcC", 1, 0);
        u8(b, 0);                                          /* profile 0 */
        /* Level by picture size: 3.0 to 540p, 3.1 to 720p, 4.0 beyond. */
        long px = (long)w->width * w->height;
        u8(b, px <= 960L * 540 ? 30 : px <= 1280L * 720 ? 31 : 40);
        u8(b, (8 << 4) | (1 << 1) | 0);                    /* 8-bit, 4:2:0 colocated, limited range */
        u8(b, 1); u8(b, 1); u8(b, 1);                      /* BT.709 primaries, transfer, matrix */
        u16(b, 0);                                         /* no codec init data */
        box_end(b, vpcc);
        box_end(b, vp09);
        box_end(b, stsd);
        write_sample_tables(b, &w->video, data_base, 1);
        box_end(b, stbl); box_end(b, minf); box_end(b, mdia); box_end(b, trak);
    }

    if (w->audio.n) {
        size_t trak = box_begin(b, "trak");
        write_tkhd(b, next_id++, ams, 0, 0, 0);
        /* The edit list skips the decoder's pre-roll, so playback starts on the
         * first audible sample rather than 6.5 ms of priming. */
        size_t edts = box_begin(b, "edts");
        size_t elst = fullbox_begin(b, "elst", 0, 0);
        u32(b, 1); u32(b, (uint32_t)ams); u32(b, OC_MP4_OPUS_PRESKIP); u16(b, 1); u16(b, 0);
        box_end(b, elst); box_end(b, edts);
        size_t mdia = box_begin(b, "mdia");
        write_mdhd(b, OC_MP4_AUDIO_TIMESCALE, adur);
        write_hdlr(b, "soun", "OpenChime audio");
        size_t minf = box_begin(b, "minf");
        size_t smhd = fullbox_begin(b, "smhd", 0, 0); u16(b, 0); u16(b, 0); box_end(b, smhd);
        write_dinf(b);
        size_t stbl = box_begin(b, "stbl");
        size_t stsd = fullbox_begin(b, "stsd", 0, 0); u32(b, 1);
        size_t opus = box_begin(b, "Opus");
        zeros(b, 6); u16(b, 1);
        u16(b, 0); u16(b, 0); u32(b, 0);
        u16(b, 1);                                         /* channels */
        u16(b, 16); u16(b, 0); u16(b, 0);
        u32(b, OC_MP4_AUDIO_TIMESCALE << 16);
        size_t dops = box_begin(b, "dOps");
        u8(b, 0);                                          /* version */
        u8(b, 1);                                          /* output channels */
        u16(b, OC_MP4_OPUS_PRESKIP);
        u32(b, w->input_rate);                             /* input sample rate */
        u16(b, 0);                                         /* output gain */
        u8(b, 0);                                          /* channel mapping family 0 */
        box_end(b, dops);
        box_end(b, opus);
        box_end(b, stsd);
        write_sample_tables(b, &w->audio, data_base, 0);
        box_end(b, stbl); box_end(b, minf); box_end(b, mdia); box_end(b, trak);
    }
    box_end(b, moov);
}

void oc_mp4_writer_abort(oc_mp4_writer *w) {
    if (!w) return;
    if (w->spill) {
        fclose(w->spill);
        char part[4200];
        snprintf(part, sizeof part, "%s.part", w->path ? w->path : "");
        remove(part);
    }
    free(w->path);
    free(w->data.d); free(w->video.s); free(w->audio.s);
    free(w);
}

static void ftyp_box(bbuf *file) {
    size_t ftyp = box_begin(file, "ftyp");
    fourcc(file, "isom"); u32(file, 0x200);
    fourcc(file, "isom"); fourcc(file, "iso6"); fourcc(file, "mp41");
    box_end(file, ftyp);
}

static uint32_t writer_duration_ms(const oc_mp4_writer *w) {
    uint64_t vms = track_duration(&w->video) * 1000 / OC_MP4_VIDEO_TIMESCALE;
    uint64_t adur = track_duration(&w->audio);
    uint64_t ams = (adur > OC_MP4_OPUS_PRESKIP ? adur - OC_MP4_OPUS_PRESKIP : 0) * 1000 / OC_MP4_AUDIO_TIMESCALE;
    return (uint32_t)(vms > ams ? vms : ams);
}

#if !defined(_WIN32)
oc_mp4_writer *oc_mp4_writer_open_file(const char *path, int width, int height) {
    if (!path || strlen(path) > 4000) return NULL;
    oc_mp4_writer *w = oc_mp4_writer_open(width, height);
    if (!w) return NULL;
    char part[4200];
    snprintf(part, sizeof part, "%s.part", path);
    w->path = strdup(path);
    w->spill = w->path ? fopen(part, "w+b") : NULL;
    if (!w->spill) { oc_mp4_writer_abort(w); return NULL; }
    return w;
}

int oc_mp4_writer_finish_file(oc_mp4_writer *w, uint64_t *bytes, uint32_t *duration_ms) {
    if (!w || !w->spill || w->video.n == 0 || fflush(w->spill) != 0) { oc_mp4_writer_abort(w); return -1; }
    bbuf head = {0};
    ftyp_box(&head);
    bbuf probe = {0};
    build_moov(&probe, w, 0);
    size_t moov_len = probe.n;
    int bad = probe.bad;
    free(probe.d);
    uint64_t data_base = head.n + moov_len + 8;
    if (bad || data_base + w->spilled > OC_MP4_MAX_FILE) { free(head.d); oc_mp4_writer_abort(w); return -1; }
    build_moov(&head, w, data_base);
    u32(&head, (uint32_t)(8 + w->spilled)); fourcc(&head, "mdat");
    FILE *out = head.bad ? NULL : fopen(w->path, "wb");
    int ok = out && fwrite(head.d, 1, head.n, out) == head.n;
    free(head.d);
    /* The samples, after the index: copied from the part. */
    ok = ok && fseek(w->spill, 0, SEEK_SET) == 0;
    uint8_t buf[65536];
    size_t n;
    while (ok && (n = fread(buf, 1, sizeof buf, w->spill)) > 0) ok = fwrite(buf, 1, n, out) == n;
    ok = ok && !ferror(w->spill);
    if (out && fclose(out) != 0) ok = 0;
    if (!ok && out) remove(w->path);
    if (ok) {
        if (bytes) *bytes = data_base + w->spilled;
        if (duration_ms) *duration_ms = writer_duration_ms(w);
    }
    oc_mp4_writer_abort(w);   /* closes and removes the part */
    return ok ? 0 : -1;
}
#endif

int oc_mp4_writer_finish(oc_mp4_writer *w, uint8_t **out, size_t *len, uint32_t *duration_ms) {
    int empty = w && (w->width ? w->video.n == 0 : w->audio.n == 0);
    if (!w || !out || !len || empty || w->data.bad || w->spill) { oc_mp4_writer_abort(w); return -1; }
    *out = NULL; *len = 0;

    bbuf file = {0};
    ftyp_box(&file);
    size_t ftyp_len = file.n;

    /* moov's size does not depend on the offsets it holds (stco is fixed width),
     * so build it once to measure, then again with the real data base. */
    bbuf probe = {0};
    build_moov(&probe, w, 0);
    size_t moov_len = probe.n;
    int bad = probe.bad;
    free(probe.d);
    uint64_t data_base = ftyp_len + moov_len + 8;          /* + the mdat header */
    if (!bad && data_base + w->data.n <= OC_MP4_MAX_FILE) {
        bb_need(&file, moov_len + 8 + w->data.n);
        build_moov(&file, w, data_base);
        u32(&file, (uint32_t)(8 + w->data.n)); fourcc(&file, "mdat");
        bytes(&file, w->data.d, w->data.n);
        bad = file.bad || file.n != data_base + w->data.n;
    } else {
        bad = 1;
    }
    if (bad) { free(file.d); oc_mp4_writer_abort(w); return -1; }

    if (duration_ms) *duration_ms = writer_duration_ms(w);
    *out = file.d; *len = file.n;
    oc_mp4_writer_abort(w);                                /* frees the tables and sample copy */
    return 0;
}

/* ---- the export writer (oc_mp4.h) ------------------------------------------------
 *
 *   ftyp · mdat (samples, streamed to the file) · moov{ mvhd · trak(avc1) · trak(mp4a) }
 *
 * The sample tables are the only thing held: a few bytes a sample. References:
 * ISO/IEC 14496-15 (avc1 / avcC); ISO/IEC 14496-14 and -1 (mp4a / esds, MPEG-1
 * audio as object type 0x6B). */
#if !defined(_WIN32)

#define EXPORT_TIMESCALE_VIDEO 90000u

struct oc_mp4_export {
    FILE    *f;
    char    *path;
    int      width, height;
    unsigned audio_rate;
    uint64_t mdat_at;               /* where mdat's header is */
    uint64_t pos;                   /* the file's length so far */
    wtrack   video, audio;
    int64_t  last_pts_us;
    int      have_video;
    uint8_t *sps, *pps;
    size_t   sps_len, pps_len;
    int      bad;
};

static int x_write(oc_mp4_export *x, const void *p, size_t n) {
    if (x->bad) return -1;
    if (n && fwrite(p, 1, n, x->f) != n) { x->bad = 1; return -1; }
    x->pos += n;
    if (x->pos > OC_MP4_EXPORT_MAX_FILE) { x->bad = 1; return -1; }
    return 0;
}

oc_mp4_export *oc_mp4_export_open(const char *path, int width, int height, unsigned audio_rate) {
    if (!path || width <= 0 || height <= 0 || width > 65535 || height > 65535 || audio_rate == 0) return NULL;
    oc_mp4_export *x = calloc(1, sizeof *x);
    if (!x) return NULL;
    x->path = strdup(path);
    x->f = x->path ? fopen(path, "wb") : NULL;
    if (!x->f) { free(x->path); free(x); return NULL; }
    x->width = width; x->height = height; x->audio_rate = audio_rate;
    bbuf h = {0};
    size_t ftyp = box_begin(&h, "ftyp");
    fourcc(&h, "isom"); u32(&h, 0x200);
    fourcc(&h, "isom"); fourcc(&h, "iso2"); fourcc(&h, "avc1"); fourcc(&h, "mp41");
    box_end(&h, ftyp);
    x->mdat_at = h.n;
    u32(&h, 0); fourcc(&h, "mdat");                        /* its size is written at finish */
    if (h.bad || x_write(x, h.d, h.n) != 0) { free(h.d); oc_mp4_export_abort(x); return NULL; }
    free(h.d);
    return x;
}

/* The next NAL unit of an Annex B buffer at or after `*at`: its payload in
 * [*start, *end), `*at` moved past it. 0, or -1 when there are no more. */
static int next_nal(const uint8_t *b, size_t n, size_t *at, size_t *start, size_t *end) {
    size_t i = *at;
    while (i + 3 <= n && !(b[i] == 0 && b[i + 1] == 0 && b[i + 2] == 1)) i++;
    if (i + 3 > n) return -1;
    i += 3;
    size_t j = i;
    while (j + 3 <= n && !(b[j] == 0 && b[j + 1] == 0 && (b[j + 2] == 1 || (b[j + 2] == 0 && j + 3 < n && b[j + 3] == 1)))) j++;
    if (j + 3 > n) j = n;
    *start = i; *end = j; *at = j;
    return i < j ? 0 : -1;
}

static int keep_param(uint8_t **dst, size_t *dst_len, const uint8_t *p, size_t n) {
    if (*dst) return 0;                                    /* the first is the sample entry's */
    if (n > 0xFFFF) return -1;
    *dst = malloc(n);
    if (!*dst) return -1;
    memcpy(*dst, p, n);
    *dst_len = n;
    return 0;
}

int oc_mp4_export_video(oc_mp4_export *x, const uint8_t *au, size_t len, int64_t pts_us) {
    if (!x || !au || x->bad) return -1;
    if (x->have_video && pts_us < x->last_pts_us) return -1;
    if (x->video.n + x->audio.n >= OC_MP4_MAX_SAMPLES) return -1;
    uint64_t off = x->pos;
    uint32_t size = 0;
    int idr = 0;
    size_t at = 0, s, e;
    while (next_nal(au, len, &at, &s, &e) == 0) {
        uint8_t type = au[s] & 0x1F;
        if (type == 7) { if (keep_param(&x->sps, &x->sps_len, au + s, e - s) != 0) return -1; continue; }
        if (type == 8) { if (keep_param(&x->pps, &x->pps_len, au + s, e - s) != 0) return -1; continue; }
        if (type == 9) continue;                           /* access unit delimiter */
        if (type == 5) idr = 1;
        uint8_t hdr[4] = { (uint8_t)((e - s) >> 24), (uint8_t)((e - s) >> 16), (uint8_t)((e - s) >> 8), (uint8_t)(e - s) };
        if (x_write(x, hdr, 4) != 0 || x_write(x, au + s, e - s) != 0) return -1;
        size += (uint32_t)(4 + e - s);
    }
    if (size == 0) return 0;                               /* parameter sets alone: no sample */
    if (!x->have_video) {
        if (!idr || !x->sps || !x->pps) { x->bad = 1; return -1; }
        x->last_pts_us = pts_us; x->have_video = 1;
    } else {
        wsample *prev = &x->video.s[x->video.n - 1];
        uint64_t d = ((uint64_t)(pts_us - x->last_pts_us) * EXPORT_TIMESCALE_VIDEO + 500000) / 1000000;
        prev->dur = (uint32_t)(d ? d : 1);
        x->last_pts_us = pts_us;
    }
    return wt_push(&x->video, (wsample){ off, size, EXPORT_TIMESCALE_VIDEO / 30, (uint8_t)idr });
}

int oc_mp4_export_audio(oc_mp4_export *x, const uint8_t *frame, size_t len, unsigned samples) {
    if (!x || !frame || !len || len > 0xFFFF || !samples || x->bad) return -1;
    if (x->video.n + x->audio.n >= OC_MP4_MAX_SAMPLES) return -1;
    uint64_t off = x->pos;
    if (x_write(x, frame, len) != 0) return -1;
    return wt_push(&x->audio, (wsample){ off, (uint32_t)len, samples, 1 });
}

/* An MPEG-4 descriptor's header: its tag and a one-byte size (all here are short). */
static void desc(bbuf *b, uint8_t tag, uint8_t size) { u8(b, tag); u8(b, size); }

static void build_export_moov(bbuf *b, const oc_mp4_export *x, uint32_t audio_delay, uint64_t audio_samples,
                              uint64_t *movie_ms_out) {
    uint64_t vdur = track_duration(&x->video), adur = track_duration(&x->audio);
    uint64_t vms = vdur * 1000 / EXPORT_TIMESCALE_VIDEO;
    if (audio_samples > adur) audio_samples = adur > audio_delay ? adur - audio_delay : 0;
    uint64_t ams = audio_samples * 1000 / x->audio_rate;
    uint64_t movie_ms = vms > ams ? vms : ams;
    *movie_ms_out = movie_ms;
    uint32_t next_id = 1;

    size_t moov = box_begin(b, "moov");
    size_t mvhd = fullbox_begin(b, "mvhd", 0, 0);
    u32(b, 0); u32(b, 0); u32(b, 1000); u32(b, (uint32_t)movie_ms);
    u32(b, 0x00010000); u16(b, 0x0100); zeros(b, 10);
    unity_matrix(b); zeros(b, 24);
    u32(b, (x->video.n ? 1u : 0u) + (x->audio.n ? 1u : 0u) + 1u);
    box_end(b, mvhd);

    if (x->video.n) {
        size_t trak = box_begin(b, "trak");
        write_tkhd(b, next_id++, vms, 1, x->width, x->height);
        size_t mdia = box_begin(b, "mdia");
        write_mdhd(b, EXPORT_TIMESCALE_VIDEO, vdur);
        write_hdlr(b, "vide", "OpenChime video");
        size_t minf = box_begin(b, "minf");
        size_t vmhd = fullbox_begin(b, "vmhd", 0, 1); u16(b, 0); zeros(b, 6); box_end(b, vmhd);
        write_dinf(b);
        size_t stbl = box_begin(b, "stbl");
        size_t stsd = fullbox_begin(b, "stsd", 0, 0); u32(b, 1);
        size_t avc1 = box_begin(b, "avc1");
        zeros(b, 6); u16(b, 1);                            /* reserved, data reference index */
        u16(b, 0); u16(b, 0); zeros(b, 12);
        u16(b, (uint16_t)x->width); u16(b, (uint16_t)x->height);
        u32(b, 0x00480000); u32(b, 0x00480000); u32(b, 0);
        u16(b, 1);                                         /* frame count */
        zeros(b, 32);                                      /* compressor name */
        u16(b, 0x0018); u16(b, 0xFFFF);
        size_t avcc = box_begin(b, "avcC");
        u8(b, 1);                                          /* configuration version */
        u8(b, x->sps[1]); u8(b, x->sps[2]); u8(b, x->sps[3]);   /* profile, compatibility, level */
        u8(b, 0xFF);                                       /* 4-byte NAL lengths */
        u8(b, 0xE1); u16(b, (uint16_t)x->sps_len); bytes(b, x->sps, x->sps_len);
        u8(b, 1);    u16(b, (uint16_t)x->pps_len); bytes(b, x->pps, x->pps_len);
        box_end(b, avcc);
        box_end(b, avc1);
        box_end(b, stsd);
        write_sample_tables(b, &x->video, 0, 1);
        box_end(b, stbl); box_end(b, minf); box_end(b, mdia); box_end(b, trak);
    }

    if (x->audio.n) {
        size_t trak = box_begin(b, "trak");
        write_tkhd(b, next_id++, ams, 0, 0, 0);
        /* The encoder's and decoder's delay is skipped and the padding after the
         * last real sample is not played: the edit is exactly the audio. */
        size_t edts = box_begin(b, "edts");
        size_t elst = fullbox_begin(b, "elst", 0, 0);
        u32(b, 1); u32(b, (uint32_t)ams); u32(b, audio_delay); u16(b, 1); u16(b, 0);
        box_end(b, elst); box_end(b, edts);
        size_t mdia = box_begin(b, "mdia");
        write_mdhd(b, x->audio_rate, adur);
        write_hdlr(b, "soun", "OpenChime audio");
        size_t minf = box_begin(b, "minf");
        size_t smhd = fullbox_begin(b, "smhd", 0, 0); u16(b, 0); u16(b, 0); box_end(b, smhd);
        write_dinf(b);
        size_t stbl = box_begin(b, "stbl");
        size_t stsd = fullbox_begin(b, "stsd", 0, 0); u32(b, 1);
        size_t mp4a = box_begin(b, "mp4a");
        zeros(b, 6); u16(b, 1);
        u32(b, 0); u32(b, 0);
        u16(b, 1);                                         /* channels */
        u16(b, 16); u16(b, 0); u16(b, 0);
        u32(b, x->audio_rate << 16);
        uint32_t max_frame = 0;
        uint64_t total = 0;
        for (uint32_t i = 0; i < x->audio.n; i++) {
            if (x->audio.s[i].size > max_frame) max_frame = x->audio.s[i].size;
            total += x->audio.s[i].size;
        }
        uint32_t avg_bps = adur ? (uint32_t)(total * 8 * x->audio_rate / adur) : 0;
        size_t esds = fullbox_begin(b, "esds", 0, 0);
        desc(b, 0x03, 3 + 2 + 13 + 2 + 1);                 /* ES_Descriptor */
        u16(b, 0); u8(b, 0);                               /* ES_ID, flags */
        desc(b, 0x04, 13);                                 /* DecoderConfigDescriptor */
        u8(b, 0x6B);                                       /* MPEG-1 audio: MP3 */
        u8(b, (0x05 << 2) | 1);                            /* audio stream */
        u8(b, (uint8_t)(max_frame >> 16)); u16(b, (uint16_t)max_frame);   /* buffer size */
        u32(b, avg_bps); u32(b, avg_bps);                  /* max and average bit rate: CBR */
        desc(b, 0x06, 1); u8(b, 0x02);                     /* SLConfigDescriptor, predefined MP4 */
        box_end(b, esds);
        box_end(b, mp4a);
        box_end(b, stsd);
        write_sample_tables(b, &x->audio, 0, 0);
        box_end(b, stbl); box_end(b, minf); box_end(b, mdia); box_end(b, trak);
    }
    box_end(b, moov);
}

static void export_free(oc_mp4_export *x) {
    free(x->video.s); free(x->audio.s); free(x->sps); free(x->pps); free(x->path);
    free(x);
}

void oc_mp4_export_abort(oc_mp4_export *x) {
    if (!x) return;
    if (x->f) fclose(x->f);
    if (x->path) unlink(x->path);
    export_free(x);
}

int oc_mp4_export_finish(oc_mp4_export *x, uint32_t audio_delay, uint64_t audio_samples,
                         uint64_t *bytes_out, uint32_t *duration_ms) {
    if (!x) return -1;
    if (x->bad || x->video.n == 0 || !x->sps || x->sps_len < 4 || !x->pps) { oc_mp4_export_abort(x); return -1; }
    uint64_t mdat_len = x->pos - x->mdat_at;
    uint8_t sz[4] = { (uint8_t)(mdat_len >> 24), (uint8_t)(mdat_len >> 16), (uint8_t)(mdat_len >> 8), (uint8_t)mdat_len };
    bbuf m = {0};
    uint64_t movie_ms = 0;
    build_export_moov(&m, x, audio_delay, audio_samples, &movie_ms);
    int ok = !m.bad && mdat_len <= 0xFFFFFFFFu && x_write(x, m.d, m.n) == 0;
    free(m.d);
    ok = ok && fseeko(x->f, (off_t)x->mdat_at, SEEK_SET) == 0 && fwrite(sz, 1, 4, x->f) == 4;
    ok = ok && fflush(x->f) == 0;
    int closed = fclose(x->f);
    x->f = NULL;
    if (!ok || closed != 0) { oc_mp4_export_abort(x); return -1; }
    if (bytes_out) *bytes_out = x->pos;
    if (duration_ms) *duration_ms = (uint32_t)movie_ms;
    export_free(x);
    return 0;
}

#endif

/* ---- reading ---------------------------------------------------------------------- */

typedef struct { const uint8_t *p; size_t n; } span;

static int rd32(span s, size_t at, uint32_t *v) {
    if (at > s.n || s.n - at < 4) return -1;
    *v = ((uint32_t)s.p[at] << 24) | ((uint32_t)s.p[at + 1] << 16) | ((uint32_t)s.p[at + 2] << 8) | s.p[at + 3];
    return 0;
}
static int rd16(span s, size_t at, uint16_t *v) {
    if (at > s.n || s.n - at < 2) return -1;
    *v = (uint16_t)((s.p[at] << 8) | s.p[at + 1]);
    return 0;
}

/* Find the first child box of `type` directly inside `parent` (its payload). */
static int child(span parent, const char *type, span *out) {
    size_t at = 0;
    while (parent.n - at >= 8) {
        uint32_t sz;
        if (rd32(parent, at, &sz) != 0) return -1;
        if (sz < 8 || sz > parent.n - at) return -1;       /* truncated or 64-bit size: not this profile */
        if (memcmp(parent.p + at + 4, type, 4) == 0) {
            out->p = parent.p + at + 8; out->n = sz - 8;
            return 0;
        }
        at += sz;
    }
    return -1;
}

static int path(span root, const char *const *types, int n, span *out) {
    span cur = root;
    for (int i = 0; i < n; i++) if (child(cur, types[i], &cur) != 0) return -1;
    *out = cur;
    return 0;
}

typedef struct { uint32_t count; span s; } table;

/* A full box's entry table: version/flags (4), count (4), then `entry` bytes each. */
static int read_table(span box, size_t lead, size_t entry, table *t) {
    uint32_t count;
    if (rd32(box, 4 + lead, &count) != 0) return -1;
    if (count > OC_MP4_MAX_SAMPLES) return -1;
    size_t start = 8 + lead;
    if (start > box.n || (box.n - start) / (entry ? entry : 1) < count) return -1;
    t->count = count; t->s.p = box.p + start; t->s.n = (size_t)count * entry;
    return 0;
}

static int parse_track(span stbl, size_t file_len, int video, oc_mp4_track *tr) {
    span stts, stsz, stsc, stco, stss;
    if (child(stbl, "stts", &stts) || child(stbl, "stsz", &stsz) ||
        child(stbl, "stsc", &stsc) || child(stbl, "stco", &stco)) return -1;

    uint32_t fixed_size, n;
    if (rd32(stsz, 4, &fixed_size) || rd32(stsz, 8, &n)) return -1;
    if (n == 0 || n > OC_MP4_MAX_SAMPLES) return -1;
    if (fixed_size == 0 && (stsz.n < 12 || (stsz.n - 12) / 4 < n)) return -1;

    tr->samples = calloc(n, sizeof *tr->samples);
    if (!tr->samples) return -1;
    tr->n_samples = n;

    /* sizes */
    for (uint32_t i = 0; i < n; i++) {
        uint32_t sz = fixed_size;
        if (!fixed_size && rd32(stsz, 12 + (size_t)i * 4, &sz)) return -1;
        tr->samples[i].size = sz;
    }
    /* timing */
    table t;
    if (read_table(stts, 0, 8, &t)) return -1;
    uint32_t idx = 0; uint64_t dts = 0;
    for (uint32_t e = 0; e < t.count && idx < n; e++) {
        uint32_t cnt, delta;
        if (rd32(t.s, (size_t)e * 8, &cnt) || rd32(t.s, (size_t)e * 8 + 4, &delta)) return -1;
        for (uint32_t k = 0; k < cnt && idx < n; k++, idx++) {
            tr->samples[idx].dts = dts; tr->samples[idx].duration = delta; dts += delta;
        }
    }
    if (idx != n) return -1;
    tr->duration = dts;
    /* chunks to offsets */
    table co, sc;
    if (read_table(stco, 0, 4, &co) || read_table(stsc, 0, 12, &sc) || sc.count == 0) return -1;
    uint32_t s = 0;
    for (uint32_t c = 0; c < co.count && s < n; c++) {
        uint32_t per = 0;
        for (uint32_t e = 0; e < sc.count; e++) {
            uint32_t first, spc;
            if (rd32(sc.s, (size_t)e * 12, &first) || rd32(sc.s, (size_t)e * 12 + 4, &spc)) return -1;
            if (first == 0) return -1;
            if (first - 1 <= c) per = spc;
        }
        if (per == 0 || per > OC_MP4_MAX_SAMPLES) return -1;
        uint32_t off32;
        if (rd32(co.s, (size_t)c * 4, &off32)) return -1;
        uint64_t off = off32;
        for (uint32_t k = 0; k < per && s < n; k++, s++) {
            if (off > file_len || file_len - off < tr->samples[s].size) return -1;
            tr->samples[s].offset = off;
            off += tr->samples[s].size;
        }
    }
    if (s != n) return -1;
    /* sync */
    if (video && child(stbl, "stss", &stss) == 0) {
        table ss;
        if (read_table(stss, 0, 4, &ss)) return -1;
        for (uint32_t e = 0; e < ss.count; e++) {
            uint32_t num;
            if (rd32(ss.s, (size_t)e * 4, &num) || num == 0 || num > n) return -1;
            tr->samples[num - 1].sync = 1;
        }
    } else {
        for (uint32_t i = 0; i < n; i++) tr->samples[i].sync = 1;
    }
    if (video && !tr->samples[0].sync) return -1;          /* must start on a keyframe */
    tr->present = 1;
    return 0;
}

void oc_mp4_info_free(oc_mp4_info *info) {
    if (!info) return;
    free(info->video.samples); free(info->audio.samples);
    memset(info, 0, sizeof *info);
}

/* The tracks of `moov`, their samples checked against a file of `file_len`
 * bytes, into `info`; the durations from them. 0, or -1 with `info` freed. */
static int parse_moov(span moov, size_t file_len, oc_mp4_info *info) {
    size_t len = file_len;
    /* Walk each trak and classify it by its sample entry. */
    size_t at = 0;
    int rc = 0;
    while (rc == 0 && moov.n - at >= 8) {
        uint32_t sz;
        if (rd32(moov, at, &sz) || sz < 8 || sz > moov.n - at) { rc = -1; break; }
        if (memcmp(moov.p + at + 4, "trak", 4) != 0) { at += sz; continue; }
        span trak = { moov.p + at + 8, sz - 8 }, mdhd, hdlr, stbl, stsd;
        at += sz;
        if (path(trak, (const char *const[]){ "mdia", "mdhd" }, 2, &mdhd) ||
            path(trak, (const char *const[]){ "mdia", "hdlr" }, 2, &hdlr) ||
            path(trak, (const char *const[]){ "mdia", "minf", "stbl" }, 3, &stbl) ||
            child(stbl, "stsd", &stsd)) { rc = -1; break; }
        uint32_t timescale, entries;
        if (rd32(mdhd, 12, &timescale) || timescale == 0) { rc = -1; break; }
        if (hdlr.n < 12) { rc = -1; break; }
        if (rd32(stsd, 4, &entries) || entries != 1) { rc = -1; break; }
        span entries_span = { stsd.p + 8, stsd.n >= 8 ? stsd.n - 8 : 0 }, vp09, opus;
        if (memcmp(hdlr.p + 8, "vide", 4) == 0 && child(entries_span, "vp09", &vp09) == 0) {
            if (info->video.present) { rc = -1; break; }
            uint16_t w, h;
            if (rd16(vp09, 24, &w) || rd16(vp09, 26, &h) || w == 0 || h == 0) { rc = -1; break; }
            info->width = w; info->height = h;
            span rest = { vp09.p + 78, vp09.n > 78 ? vp09.n - 78 : 0 }, vpcc;
            if (child(rest, "vpcC", &vpcc) || vpcc.n < 12) { rc = -1; break; }
            info->vp9_profile = vpcc.p[4];
            info->vp9_bit_depth = (uint8_t)(vpcc.p[6] >> 4);
            info->video.timescale = timescale;
            if (parse_track(stbl, len, 1, &info->video)) { rc = -1; break; }
        } else if (memcmp(hdlr.p + 8, "soun", 4) == 0 && child(entries_span, "Opus", &opus) == 0) {
            if (info->audio.present) { rc = -1; break; }
            span rest = { opus.p + 28, opus.n > 28 ? opus.n - 28 : 0 }, dops;
            if (child(rest, "dOps", &dops) || dops.n < 11 || dops.p[0] != 0) { rc = -1; break; }
            info->opus_channels = dops.p[1];
            info->opus_preskip = (uint16_t)((dops.p[2] << 8) | dops.p[3]);
            if (info->opus_channels < 1 || info->opus_channels > 2) { rc = -1; break; }
            info->audio.timescale = timescale;
            if (parse_track(stbl, len, 0, &info->audio)) { rc = -1; break; }
        } else {
            rc = -1;                                       /* any other track: not this profile */
        }
    }
    if (rc != 0 || (!info->video.present && !info->audio.present)) { oc_mp4_info_free(info); return -1; }
    if (info->video.present && (info->vp9_profile != 0 || info->vp9_bit_depth != 8)) { oc_mp4_info_free(info); return -1; }

    uint64_t vms = info->video.present ? info->video.duration * 1000 / info->video.timescale : 0, ams = 0;
    if (info->audio.present) {
        uint64_t d = info->audio.duration > info->opus_preskip ? info->audio.duration - info->opus_preskip : 0;
        ams = d * 1000 / info->audio.timescale;
    }
    info->duration_ms = (uint32_t)(vms > ams ? vms : ams);
    return 0;
}

int oc_mp4_parse(const uint8_t *data, size_t len, oc_mp4_info *info) {
    memset(info, 0, sizeof *info);
    if (!data || len < 16 || len > OC_MP4_MAX_FILE) return -1;
    span file = { data, len }, ftyp, moov;
    if (child(file, "ftyp", &ftyp) || child(file, "moov", &moov)) return -1;
    if (child(file, "mdat", &(span){0}) != 0) return -1;
    return parse_moov(moov, len, info);
}

int oc_mp4_parse_head(const uint8_t *head, size_t head_len, uint64_t file_len, oc_mp4_info *info,
                      uint64_t *mdat_payload) {
    memset(info, 0, sizeof *info);
    if (!head || head_len < 16 || file_len > OC_MP4_MAX_FILE || head_len > file_len) return -1;
    /* ftyp, then moov, then mdat's header: all inside `head`, mdat running to the
     * file's end. That order is the profile's (moov before mdat). */
    span h = { head, head_len };
    size_t at = 0;
    uint32_t sz;
    if (rd32(h, 0, &sz) || sz < 8 || sz > head_len || memcmp(head + 4, "ftyp", 4) != 0) return -1;
    at = sz;
    if (rd32(h, at, &sz) || sz < 8 || sz > head_len - at || memcmp(head + at + 4, "moov", 4) != 0) return -1;
    span moov = { head + at + 8, sz - 8 };
    at += sz;
    if (rd32(h, at, &sz) || head_len - at < 8 || memcmp(head + at + 4, "mdat", 4) != 0) return -1;
    if (sz < 8 || (uint64_t)at + sz != file_len) return -1;
    if (mdat_payload) *mdat_payload = at + 8;
    return parse_moov(moov, (size_t)file_len, info);
}

int oc_mp4_keyframe_before(const oc_mp4_info *info, uint64_t dts) {
    int best = -1;
    for (uint32_t i = 0; i < info->video.n_samples; i++) {
        if (info->video.samples[i].dts > dts) break;
        if (info->video.samples[i].sync) best = (int)i;
    }
    return best;
}
