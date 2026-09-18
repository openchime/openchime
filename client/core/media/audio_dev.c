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

/* ---- the far-end reference -------------------------------------------------------- */

/* One slot per open playback device: its output, mono at 16 kHz, in a ring
 * indexed by sample number since the slot's clock base. Each slot has exactly
 * one producer (its device's callback, or the synthetic sink), so a reader can
 * sum the slots without a lock. */
#define REF_SLOTS 4
#define REF_CAP   32768                       /* 2 s at 16 kHz; a power of two */
typedef struct {
    atomic_int           used;
    _Atomic int64_t      base_us;             /* media clock of sample 0 */
    atomic_uint_fast64_t written;             /* samples produced */
    int16_t              buf[REF_CAP];
    /* The producer's resampler: an average over each output sample's span. */
    uint64_t             acc_pos;
    int64_t              acc_sum;
    int                  acc_n;
} ref_slot;
static ref_slot g_ref[REF_SLOTS];

static int ref_claim(void) {
    for (int i = 0; i < REF_SLOTS; i++) {
        int zero = 0;
        if (atomic_compare_exchange_strong(&g_ref[i].used, &zero, 2)) {   /* 2: being reset */
            atomic_store(&g_ref[i].base_us, 0);
            atomic_store(&g_ref[i].written, 0);
            g_ref[i].acc_pos = 0;
            g_ref[i].acc_sum = 0;
            g_ref[i].acc_n = 0;
            atomic_store(&g_ref[i].used, 1);
            return i;
        }
    }
    return -1;                                /* more players than slots: not referenced */
}

/* Feed `frames` interleaved frames at `rate` into slot `k`. Real-time safe. */
static void ref_feed(int k, const int16_t *s, size_t frames, int channels, int rate) {
    if (k < 0 || !frames) return;
    ref_slot *r = &g_ref[k];
    uint64_t w = atomic_load(&r->written);
    if (atomic_load(&r->base_us) == 0)
        atomic_store(&r->base_us, oc_media_clock_us() - (int64_t)frames * 1000000 / rate);
    for (size_t f = 0; f < frames; f++) {
        int32_t mono = 0;
        for (int c = 0; c < channels; c++) mono += s ? s[f * (size_t)channels + (size_t)c] : 0;
        r->acc_sum += mono / channels;
        r->acc_n++;
        r->acc_pos += OC_AUDIO_REF_RATE;
        while (r->acc_pos >= (uint64_t)rate) {
            r->acc_pos -= (uint64_t)rate;
            r->buf[w & (REF_CAP - 1)] = (int16_t)(r->acc_n ? r->acc_sum / r->acc_n : 0);
            w++;
            r->acc_sum = 0;
            r->acc_n = 0;
        }
    }
    atomic_store(&r->written, w);
}

int oc_audio_reference(int64_t start_us, int16_t *out, size_t n) {
    int32_t mix[1024];
    int contributed = 0;
    memset(out, 0, n * sizeof *out);
    for (size_t at = 0; at < n; at += 1024) {
        size_t len = n - at < 1024 ? n - at : 1024;
        memset(mix, 0, sizeof mix);
        for (int k = 0; k < REF_SLOTS; k++) {
            ref_slot *r = &g_ref[k];
            int64_t base = atomic_load(&r->base_us);
            if (atomic_load(&r->used) != 1 || base == 0) continue;
            uint64_t w = atomic_load(&r->written);
            int64_t first = (start_us - base) * OC_AUDIO_REF_RATE / 1000000 + (int64_t)at;
            int any = 0;
            for (size_t i = 0; i < len; i++) {
                int64_t idx = first + (int64_t)i;
                if (idx < 0 || (uint64_t)idx >= w || w - (uint64_t)idx > REF_CAP) continue;
                mix[i] += r->buf[(uint64_t)idx & (REF_CAP - 1)];
                any = 1;
            }
            if (any && at == 0) contributed++;
        }
        for (size_t i = 0; i < len; i++)
            out[at + i] = (int16_t)(mix[i] > 32767 ? 32767 : mix[i] < -32768 ? -32768 : mix[i]);
    }
    return contributed;
}

/* The microphone's one owner (OC_AUDIO_BUSY). */
static atomic_int g_capture_open;

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
    /* Synthetic capture: what the microphone "hears" instead of the tone, when
     * OPENCHIME_TEST_MIC names a WAV (played once, then silence). */
    int16_t      *syn_clip;
    size_t        syn_clip_n;
    int           loopback;           /* capture of what the output device plays */
    int           ref_slot;           /* playback: its far-end reference slot, or -1 */
    int           owns_mic;           /* capture: holds g_capture_open */
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
    return t && (strcmp(t, "synthetic") == 0 || strcmp(t, "mic-denied") == 0);
}

/* `OPENCHIME_TEST_AUDIO=mic-denied`: the synthetic devices, with the microphone
 * refused as the operating system refuses one it blocks. */
static int mic_denied(void) {
    const char *t = getenv("OPENCHIME_TEST_AUDIO");
    return t && strcmp(t, "mic-denied") == 0;
}

/* OPENCHIME_TEST_MIC: a 16-bit PCM WAV at `rate` with `channels`, for the
 * synthetic microphone to speak -- so voice input is testable end to end on a
 * machine with no microphone. NULL (and the tone) when unset or unusable. */
static int16_t *load_test_clip(int rate, int channels, size_t *n_out) {
    const char *path = getenv("OPENCHIME_TEST_MIC");
    if (!path || !*path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    unsigned char h[12];
    int16_t *pcm = NULL;
    int ok_fmt = 0;
    if (fread(h, 1, 12, f) != 12 || memcmp(h, "RIFF", 4) || memcmp(h + 8, "WAVE", 4)) goto done;
    for (;;) {
        unsigned char c[8];
        if (fread(c, 1, 8, f) != 8) break;
        uint32_t len = (uint32_t)c[4] | (uint32_t)c[5] << 8 | (uint32_t)c[6] << 16 | (uint32_t)c[7] << 24;
        if (!memcmp(c, "fmt ", 4) && len >= 16) {
            unsigned char fm[16];
            if (fread(fm, 1, 16, f) != 16) break;
            unsigned tag = fm[0] | fm[1] << 8, ch = fm[2] | fm[3] << 8, bits = fm[14] | fm[15] << 8;
            uint32_t sr = (uint32_t)fm[4] | (uint32_t)fm[5] << 8 | (uint32_t)fm[6] << 16 | (uint32_t)fm[7] << 24;
            ok_fmt = tag == 1 && (int)ch == channels && bits == 16 && (int)sr == rate;
            if (fseek(f, (long)(len - 16 + (len & 1)), SEEK_CUR) != 0) break;
        } else if (!memcmp(c, "data", 4)) {
            if (!ok_fmt || len < 2) break;
            pcm = malloc(len);
            if (pcm && fread(pcm, 1, len, f) == len) *n_out = len / 2 / (size_t)channels;
            else { free(pcm); pcm = NULL; }
            break;
        } else if (fseek(f, (long)(len + (len & 1)), SEEK_CUR) != 0) {
            break;
        }
    }
done:
    fclose(f);
    return pcm;
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
    /* A loopback device is silent by not calling at all while nothing plays, so
     * the stream would close up over the gap and every later sample would be
     * stamped too early. The gap is written as silence instead, which keeps
     * sample k at base + k/rate like a microphone's. */
    if (d->loopback) {
        int64_t at = oc_media_clock_us() - (int64_t)frames * 1000000 / d->rate;
        int64_t expect = atomic_load(&d->base_us) +
                         (int64_t)(atomic_load(&d->pushed) * 1000000 / (uint64_t)d->rate);
        if (at - expect > 40000) {
            static const int16_t zeros[480 * 2];
            uint64_t gap = (uint64_t)((at - expect) * d->rate / 1000000);
            while (gap > 0) {
                size_t n = gap > 480 ? 480 : (size_t)gap;
                ring_push(&d->ring, zeros, n * (size_t)d->channels);
                atomic_fetch_add(&d->pushed, n);
                gap -= n;
            }
        }
    }
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
    /* What the speaker is given is what the microphone may hear back. */
    ref_feed(d->ref_slot, o, frames, d->channels, d->rate);
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

static oc_audio_dev *open_dev(int capture, int loopback, const char *id, int rate, int channels, int *err) {
    int dummy;
    if (!err) err = &dummy;
    if (rate < 8000 || rate > 192000 || channels < 1 || channels > 2) { *err = OC_AUDIO_FAILED; return NULL; }
    oc_audio_dev *d = calloc(1, sizeof *d);
    if (!d) { *err = OC_AUDIO_FAILED; return NULL; }
    d->capture = capture; d->loopback = loopback; d->rate = rate; d->channels = channels;
    d->ref_slot = -1;
    atomic_store(&d->gain, 1.0f);
    if (capture && !loopback && mic_denied()) { free(d); *err = OC_AUDIO_DENIED; return NULL; }
    if (loopback) {
        /* Not the microphone: it has its own owner, and this takes nothing from it. */
    } else if (capture) {
        int zero = 0;
        if (!atomic_compare_exchange_strong(&g_capture_open, &zero, 1)) { free(d); *err = OC_AUDIO_BUSY; return NULL; }
        d->owns_mic = 1;
    } else {
        d->ref_slot = ref_claim();
    }
    /* Two seconds of samples: enough to ride out a slow reader or writer. */
    if (ring_init(&d->ring, (size_t)rate * (size_t)channels * 2) != 0) { oc_audio_close(d); *err = OC_AUDIO_FAILED; return NULL; }

    if (use_synthetic()) {
        d->synthetic = 1;
        if (capture && !loopback) d->syn_clip = load_test_clip(rate, channels, &d->syn_clip_n);
        d->syn_start_us = oc_media_clock_us();
        atomic_store(&d->base_us, d->syn_start_us);
        *err = OC_AUDIO_OK;
        return d;
    }

    if (ma_context_init(NULL, 0, NULL, &d->ctx) != MA_SUCCESS) { oc_audio_close(d); *err = OC_AUDIO_FAILED; return NULL; }
    d->have_ctx = 1;
    ma_device_id dev_id;
    int have_id = id && *id && parse_id(id, &dev_id) == 0;
    ma_device_config cfg = ma_device_config_init(loopback ? ma_device_type_loopback
                                                 : capture ? ma_device_type_capture : ma_device_type_playback);
    if (capture) {
        /* A loopback device is named by the OUTPUT it listens to; NULL is the default. */
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
    return open_dev(1, 0, id, rate, channels, err);
}
oc_audio_dev *oc_audio_playback_open(const char *id, int rate, int channels, int *err) {
    return open_dev(0, 0, id, rate, channels, err);
}
oc_audio_dev *oc_audio_loopback_open(const char *output_id, int rate, int channels, int *err) {
    return open_dev(1, 1, output_id, rate, channels, err);
}

/* The synthetic computer sound: a 660 Hz tone over a spread of quieter ones, a
 * continuous function of the media clock alone -- so a synthetic microphone can
 * hear it back as an echo (OPENCHIME_TEST_MIC_ECHO) exactly as a room would,
 * whenever either opened. Continuous matters: a signal computed per sample index
 * shifts by whole samples as two clocks' grids slide past each other, which a
 * canceller sees as a room that keeps changing. */
static double syn_loop_value(double t_s) {
    static const double hz[6] = { 1234.0, 1777.0, 2391.0, 3113.0, 4567.0, 6007.0 };
    double v = 6000.0 * __builtin_sin(2.0 * 3.141592653589793 * 660.0 * t_s);
    for (int i = 0; i < 6; i++) v += 700.0 * __builtin_sin(2.0 * 3.141592653589793 * hz[i] * t_s + i);
    return v;
}

/* 0 none; 1 the echo over the microphone's own tone; 2 the echo alone. */
static int mic_echo(void) {
    const char *t = getenv("OPENCHIME_TEST_MIC_ECHO");
    if (!t || !*t || strcmp(t, "0") == 0) return 0;
    return strcmp(t, "only") == 0 ? 2 : 1;
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
            if (d->syn_clip) {
                for (size_t i = 0; i < n * (size_t)d->channels; i++) {
                    size_t k = (size_t)d->syn_frames * (size_t)d->channels + i;
                    chunk[i] = k < d->syn_clip_n * (size_t)d->channels ? d->syn_clip[k] : 0;
                }
            } else if (d->loopback) {
                for (size_t i = 0; i < n; i++) {
                    double t = ((double)d->syn_start_us + (double)(d->syn_frames + i) * 1e6 / d->rate) / 1e6;
                    int16_t v = (int16_t)syn_loop_value(t);
                    for (int c = 0; c < d->channels; c++) chunk[i * (size_t)d->channels + (size_t)c] = v;
                }
            } else {
                int echo = mic_echo();
                for (size_t i = 0; i < n; i++) {
                    double t = (double)(d->syn_frames + i) / d->rate;
                    double v = echo == 2 ? 0.0 : 8000.0 * __builtin_sin(2.0 * 3.141592653589793 * 440.0 * t);
                    /* The speakers heard back 20 ms later at half strength. */
                    if (echo) v += 0.5 * syn_loop_value(((double)d->syn_start_us + (double)(d->syn_frames + i) * 1e6 / d->rate) / 1e6 - 0.020);
                    if (v > 32767) v = 32767;
                    if (v < -32768) v = -32768;
                    for (int c = 0; c < d->channels; c++) chunk[i * (size_t)d->channels + (size_t)c] = (int16_t)v;
                }
            }
            size_t put = ring_push(&d->ring, chunk, n * (size_t)d->channels);
            if (put < n * (size_t)d->channels)
                atomic_fetch_add(&d->overruns, (n * (size_t)d->channels - put) / (size_t)d->channels);
            atomic_fetch_add(&d->pushed, n);
            atomic_store(&d->level, d->syn_clip ? peak(chunk, n * (size_t)d->channels) : 8000);
        } else {
            size_t got = ring_pop(&d->ring, chunk, n * (size_t)d->channels);
            if (got < n * (size_t)d->channels) memset(chunk + got, 0, (n * (size_t)d->channels - got) * sizeof *chunk);
            atomic_fetch_add(&d->played, got / (size_t)d->channels);
            ref_feed(d->ref_slot, chunk, n, d->channels, d->rate);
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
    if (d->ref_slot >= 0) atomic_store(&g_ref[d->ref_slot].used, 0);
    if (d->owns_mic) atomic_store(&g_capture_open, 0);
    free(d->syn_clip);
    free(d->ring.buf);
    free(d);
}
