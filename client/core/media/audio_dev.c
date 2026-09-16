/* The audio device layer over miniaudio (audio_dev.h). */
#define _POSIX_C_SOURCE 200809L
#include "audio_dev.h"
#include "oc_media.h"

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* miniaudio is vendored as one header; its implementation is compiled here and
 * nowhere else. Only device I/O is wanted — no decoders, graph or engine. Its
 * own code is not held to this tree's warning set, so the warnings it trips are
 * silenced for the include alone. */
#define MA_NO_DECODING
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MA_NO_RESOURCE_MANAGER
#define MA_NO_NODE_GRAPH
#define MA_NO_ENGINE
#define MINIAUDIO_IMPLEMENTATION
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wtype-limits"
#pragma GCC diagnostic ignored "-Wcast-function-type"
#endif
#include "../../../third_party/miniaudio/miniaudio.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

/* ---- a single-producer, single-consumer ring of int16 samples ---------------------- */

typedef struct {
    int16_t      *buf;
    size_t        cap;          /* samples; a power of two */
    atomic_size_t head;         /* written by the producer */
    atomic_size_t tail;         /* written by the consumer */
} ring;

static int ring_init(ring *r, size_t min_samples) {
    size_t cap = 1024;
    while (cap < min_samples) cap *= 2;
    r->buf = calloc(cap, sizeof *r->buf);
    if (!r->buf) return -1;
    r->cap = cap;
    atomic_init(&r->head, 0);
    atomic_init(&r->tail, 0);
    return 0;
}
static size_t ring_used(const ring *r) { return atomic_load(&r->head) - atomic_load(&r->tail); }
static size_t ring_push(ring *r, const int16_t *s, size_t n) {
    size_t head = atomic_load(&r->head), room = r->cap - (head - atomic_load(&r->tail));
    if (n > room) n = room;
    for (size_t i = 0; i < n; i++) r->buf[(head + i) & (r->cap - 1)] = s[i];
    atomic_store(&r->head, head + n);
    return n;
}
static size_t ring_pop(ring *r, int16_t *s, size_t n) {
    size_t tail = atomic_load(&r->tail), used = atomic_load(&r->head) - tail;
    if (n > used) n = used;
    if (s) for (size_t i = 0; i < n; i++) s[i] = r->buf[(tail + i) & (r->cap - 1)];
    atomic_store(&r->tail, tail + n);
    return n;
}

/* ---- devices ---------------------------------------------------------------------- */

struct oc_audio_dev {
    int           capture;
    int           synthetic;
    int           rate, channels;
    ma_context    ctx;
    int           have_ctx;
    ma_device     dev;
    int           have_dev;
    ring          ring;
    /* Capture timing: the clock time of the first captured sample, and the count
     * of frames pushed since. Sample k was captured at base + k/rate. */
    _Atomic int64_t  base_us;
    atomic_uint_fast64_t pushed;      /* frames the callback produced */
    atomic_uint_fast64_t popped;      /* frames the reader consumed (capture) */
    atomic_uint_fast64_t overruns;
    atomic_uint_fast64_t played;      /* frames the device consumed (playback) */
    atomic_int    level;
    _Atomic float gain;
    atomic_int    flush;              /* playback: drop the ring on the next consume */
    atomic_size_t flush_to;           /* ... but only what was written before it was asked for */
    /* Synthetic: when the tone source / real-time sink last caught up. */
    int64_t       syn_start_us;
    uint64_t      syn_frames;
};

/* Carry out a flush the producer asked for, on the consumer's side, where the
 * tail belongs. It drops what was in the ring WHEN IT WAS ASKED FOR and nothing
 * after: the producer writes again the moment it has asked (a seek refills
 * immediately), and this runs a device period later, so a flush that simply
 * emptied the ring would eat that fresh audio — silently, since dropped frames
 * are never counted as played. That cost the first 213 ms of every read-aloud
 * message, and left the audio clock permanently short of the file's duration,
 * so a player that had reached the end never said so (REQ-291, ARCH-111). */
static void take_flush(oc_audio_dev *d) {
    if (!atomic_exchange(&d->flush, 0)) return;
    size_t to = atomic_load(&d->flush_to), tail = atomic_load(&d->ring.tail);
    /* The consumer may already be past the mark; the tail never goes backwards. */
    if ((ptrdiff_t)(to - tail) > 0) atomic_store(&d->ring.tail, to);
}

static int use_synthetic(void) {
    const char *t = getenv("OPENCHIME_TEST_AUDIO");
    return t && strcmp(t, "synthetic") == 0;
}

static int peak(const int16_t *s, size_t n) {
    int p = 0;
    for (size_t i = 0; i < n; i++) { int v = s[i] < 0 ? -s[i] : s[i]; if (v > p) p = v; }
    return p > 32767 ? 32767 : p;
}

static void capture_cb(ma_device *dev, void *out, const void *in, ma_uint32 frames) {
    (void)out;
    oc_audio_dev *d = dev->pUserData;
    if (!in || frames == 0) return;
    if (atomic_load(&d->pushed) == 0 && atomic_load(&d->base_us) == 0)
        atomic_store(&d->base_us, oc_media_clock_us() - (int64_t)frames * 1000000 / d->rate);
    size_t n = (size_t)frames * (size_t)d->channels;
    size_t put = ring_push(&d->ring, in, n);
    if (put < n) atomic_fetch_add(&d->overruns, (n - put) / (size_t)d->channels);
    /* Counted as produced even when dropped, so timestamps stay on the clock. */
    atomic_fetch_add(&d->pushed, frames);
    atomic_store(&d->level, peak(in, n));
}

static void playback_cb(ma_device *dev, void *out, const void *in, ma_uint32 frames) {
    (void)in;
    oc_audio_dev *d = dev->pUserData;
    int16_t *o = out;
    size_t n = (size_t)frames * (size_t)d->channels;
    take_flush(d);
    size_t got = ring_pop(&d->ring, o, n);
    float g = atomic_load(&d->gain);
    if (g < 0.999f)
        for (size_t i = 0; i < got; i++) o[i] = (int16_t)(o[i] * g);
    if (got < n) memset(o + got, 0, (n - got) * sizeof *o);
    atomic_fetch_add(&d->played, got / (size_t)d->channels);
    atomic_store(&d->level, peak(o, got));
}

int oc_audio_list(int capture, oc_audio_device *out, int cap) {
    if (use_synthetic()) {
        if (cap < 1) return 0;
        memset(out, 0, sizeof *out);
        strcpy(out->id, "synthetic");
        strcpy(out->name, capture ? "Test tone" : "Test sink");
        out->is_default = 1;
        return 1;
    }
    ma_context ctx;
    if (ma_context_init(NULL, 0, NULL, &ctx) != MA_SUCCESS) return OC_AUDIO_FAILED;
    ma_device_info *play, *capt;
    ma_uint32 np, nc;
    if (ma_context_get_devices(&ctx, &play, &np, &capt, &nc) != MA_SUCCESS) {
        ma_context_uninit(&ctx);
        return OC_AUDIO_FAILED;
    }
    ma_device_info *list = capture ? capt : play;
    ma_uint32 n = capture ? nc : np;
    int count = 0;
    for (ma_uint32 i = 0; i < n && count < cap; i++, count++) {
        oc_audio_device *o = &out[count];
        memset(o, 0, sizeof *o);
        /* The id is the raw ma_device_id bytes, hex-encoded, so it survives as a string. */
        const unsigned char *raw = (const unsigned char *)&list[i].id;
        _Static_assert(sizeof o->id > sizeof list[i].id * 2, "device id must fit hex-encoded");
        /* Trailing zero bytes are left off; parse_id zero-fills them back. */
        size_t len = sizeof list[i].id;
        while (len > 0 && raw[len - 1] == 0) len--;
        for (size_t k = 0; k < len; k++) snprintf(o->id + 2 * k, 3, "%02x", raw[k]);
        snprintf(o->name, sizeof o->name, "%s", list[i].name);
        o->is_default = list[i].isDefault ? 1 : 0;
    }
    ma_context_uninit(&ctx);
    return count;
}

static int parse_id(const char *hex, ma_device_id *id) {
    memset(id, 0, sizeof *id);
    size_t n = strlen(hex);
    if (n % 2 || n / 2 > sizeof *id) return -1;
    unsigned char *raw = (unsigned char *)id;
    for (size_t k = 0; k < n / 2; k++) {
        unsigned v;
        if (sscanf(hex + 2 * k, "%2x", &v) != 1) return -1;
        raw[k] = (unsigned char)v;
    }
    return 0;
}

static oc_audio_dev *open_dev(int capture, const char *id, int rate, int channels, int *err) {
    int dummy;
    if (!err) err = &dummy;
    if (rate < 8000 || rate > 192000 || channels < 1 || channels > 2) { *err = OC_AUDIO_FAILED; return NULL; }
    oc_audio_dev *d = calloc(1, sizeof *d);
    if (!d) { *err = OC_AUDIO_FAILED; return NULL; }
    d->capture = capture; d->rate = rate; d->channels = channels;
    atomic_store(&d->gain, 1.0f);
    /* Two seconds of samples: enough to ride out a slow reader or writer. */
    if (ring_init(&d->ring, (size_t)rate * (size_t)channels * 2) != 0) { free(d); *err = OC_AUDIO_FAILED; return NULL; }

    if (use_synthetic()) {
        d->synthetic = 1;
        d->syn_start_us = oc_media_clock_us();
        atomic_store(&d->base_us, d->syn_start_us);
        *err = OC_AUDIO_OK;
        return d;
    }

    if (ma_context_init(NULL, 0, NULL, &d->ctx) != MA_SUCCESS) { oc_audio_close(d); *err = OC_AUDIO_FAILED; return NULL; }
    d->have_ctx = 1;
    ma_device_id dev_id;
    int have_id = id && *id && parse_id(id, &dev_id) == 0;
    ma_device_config cfg = ma_device_config_init(capture ? ma_device_type_capture : ma_device_type_playback);
    if (capture) {
        cfg.capture.pDeviceID = have_id ? &dev_id : NULL;
        cfg.capture.format = ma_format_s16;
        cfg.capture.channels = (ma_uint32)channels;
        cfg.dataCallback = capture_cb;
    } else {
        cfg.playback.pDeviceID = have_id ? &dev_id : NULL;
        cfg.playback.format = ma_format_s16;
        cfg.playback.channels = (ma_uint32)channels;
        cfg.dataCallback = playback_cb;
    }
    cfg.sampleRate = (ma_uint32)rate;
    cfg.periodSizeInMilliseconds = 10;
    cfg.pUserData = d;
    ma_result r = ma_device_init(&d->ctx, &cfg, &d->dev);
    if (r != MA_SUCCESS) {
        oc_audio_close(d);
        *err = r == MA_ACCESS_DENIED ? OC_AUDIO_DENIED : r == MA_NO_DEVICE ? OC_AUDIO_NODEVICE : OC_AUDIO_FAILED;
        return NULL;
    }
    d->have_dev = 1;
    if (ma_device_start(&d->dev) != MA_SUCCESS) { oc_audio_close(d); *err = OC_AUDIO_FAILED; return NULL; }
    *err = OC_AUDIO_OK;
    return d;
}

oc_audio_dev *oc_audio_capture_open(const char *id, int rate, int channels, int *err) {
    return open_dev(1, id, rate, channels, err);
}
oc_audio_dev *oc_audio_playback_open(const char *id, int rate, int channels, int *err) {
    return open_dev(0, id, rate, channels, err);
}

/* The synthetic devices run their "callback" lazily, from the reader's or
 * writer's thread, for however many frames real time says are due. */
static void synthetic_catch_up(oc_audio_dev *d) {
    if (!d->capture) take_flush(d);
    int64_t now = oc_media_clock_us();
    uint64_t due = (uint64_t)((now - d->syn_start_us) * d->rate / 1000000);
    if (due <= d->syn_frames) return;
    size_t frames = (size_t)(due - d->syn_frames);
    int16_t chunk[960 * 2];
    while (frames > 0) {
        size_t n = frames > 960 ? 960 : frames;
        if (d->capture) {
            for (size_t i = 0; i < n; i++) {
                double t = (double)(d->syn_frames + i) / d->rate;
                int16_t v = (int16_t)(8000.0 * __builtin_sin(2.0 * 3.141592653589793 * 440.0 * t));
                for (int c = 0; c < d->channels; c++) chunk[i * (size_t)d->channels + (size_t)c] = v;
            }
            size_t put = ring_push(&d->ring, chunk, n * (size_t)d->channels);
            if (put < n * (size_t)d->channels)
                atomic_fetch_add(&d->overruns, (n * (size_t)d->channels - put) / (size_t)d->channels);
            atomic_fetch_add(&d->pushed, n);
            atomic_store(&d->level, 8000);
        } else {
            size_t got = ring_pop(&d->ring, NULL, n * (size_t)d->channels);
            atomic_fetch_add(&d->played, got / (size_t)d->channels);
        }
        d->syn_frames += n;
        frames -= n;
    }
}

size_t oc_audio_capture_read(oc_audio_dev *d, int16_t *pcm, size_t frames, int64_t *pts_us) {
    if (!d->capture) return 0;
    if (d->synthetic) synthetic_catch_up(d);
    /* The oldest waiting sample is `pushed - used` frames into the stream. */
    size_t used = ring_used(&d->ring) / (size_t)d->channels;
    uint64_t index = atomic_load(&d->pushed) - used;
    size_t n = ring_pop(&d->ring, pcm, frames * (size_t)d->channels) / (size_t)d->channels;
    if (pts_us) *pts_us = atomic_load(&d->base_us) + (int64_t)(index * 1000000 / (uint64_t)d->rate);
    atomic_fetch_add(&d->popped, n);
    return n;
}

uint64_t oc_audio_capture_overruns(oc_audio_dev *d) { return atomic_load(&d->overruns); }

size_t oc_audio_playback_write(oc_audio_dev *d, const int16_t *pcm, size_t frames) {
    if (d->capture) return 0;
    if (d->synthetic) synthetic_catch_up(d);
    return ring_push(&d->ring, pcm, frames * (size_t)d->channels) / (size_t)d->channels;
}

size_t oc_audio_playback_queued(oc_audio_dev *d) {
    if (d->synthetic) synthetic_catch_up(d);
    return ring_used(&d->ring) / (size_t)d->channels;
}

uint64_t oc_audio_playback_position(oc_audio_dev *d) {
    if (d->synthetic) synthetic_catch_up(d);
    return atomic_load(&d->played);
}

void oc_audio_playback_flush(oc_audio_dev *d) {
    /* Only the consumer moves the tail, so the flush is a request the consumer
     * (the device callback, or the synthetic catch-up) carries out -- against the
     * mark set here, not against whatever the ring holds by then (take_flush). */
    atomic_store(&d->flush_to, atomic_load(&d->ring.head));
    atomic_store(&d->flush, 1);
    if (d->synthetic) synthetic_catch_up(d);
}

void oc_audio_playback_volume(oc_audio_dev *d, float gain) {
    atomic_store(&d->gain, gain < 0 ? 0 : gain > 1 ? 1 : gain);
}

int oc_audio_level(oc_audio_dev *d) { return atomic_load(&d->level); }

void oc_audio_close(oc_audio_dev *d) {
    if (!d) return;
    if (d->have_dev) ma_device_uninit(&d->dev);
    if (d->have_ctx) ma_context_uninit(&d->ctx);
    free(d->ring.buf);
    free(d);
}
