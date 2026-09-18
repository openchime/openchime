/* Voice-input recognition built into the daemon (stt.h). */
#define _POSIX_C_SOURCE 200809L
#include "stt.h"

#include "stt_moonshine.h"
#include "stt_render.h"
#include "tts_data.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

const char *const OC_STT_DATA_FILES[] = {
    "frontend.model.ort", "frontend.weights.ort", "encoder.ort", "adapter.ort",
    "cross_kv.ort", "decoder_kv.ort", "tokenizer.bin", "streaming_config.json", NULL,
};

/* The directory oc_stt_data_ready chose and checked. Set once at startup. */
static char g_data_dir[512];

int oc_stt_data_ready(char *err, size_t errcap) {
    char exe[512] = "";
    oc_tts_exe_dir(exe, sizeof exe);
    char dir[512];
    if (!oc_data_dir_find("OPENCHIME_STT_DATA_DIR", getenv("OPENCHIME_STT_DATA_DIR"), OC_STT_DATA_SYSTEM_DIR,
                          exe, "stt", dir, sizeof dir, err, errcap))
        return 0;
    if (oc_tts_data_verify(dir, OC_STT_MODEL_VERSION, OC_STT_DATA_FILES, err, errcap) != 0) return 0;
    snprintf(g_data_dir, sizeof g_data_dir, "%s", dir);
    return 1;
}

int oc_stt_manifest(const char *dir) {
    char err[256] = "";
    if (oc_tts_data_write_manifest(dir, OC_STT_MODEL_VERSION, OC_STT_DATA_FILES, err, sizeof err) != 0) {
        fprintf(stderr, "openchimed: %s\n", err);
        return 1;
    }
    fprintf(stderr, "openchimed: wrote %s/%s for %s\n", dir, OC_TTS_DATA_MANIFEST, OC_STT_MODEL_VERSION);
    return 0;
}

/* ---- the engine ------------------------------------------------------------------ */

typedef struct {
    moonshine  *model;
    oc_tts_map  files[MOONSHINE_FILES];   /* consumed in place: they outlive the model */
} moonshine_engine;

static void engine_close(void *engine) {
    moonshine_engine *e = engine;
    if (!e) return;
    moonshine_close(e->model);
    for (int i = 0; i < MOONSHINE_FILES; i++) oc_tts_map_close(&e->files[i]);
    free(e);
}

static void *engine_open(void *ctx, char *err, size_t errcap) {
    (void)ctx;
    if (!g_data_dir[0]) { snprintf(err, errcap, "no recognizer data"); return NULL; }
    moonshine_engine *e = calloc(1, sizeof *e);
    if (!e) { snprintf(err, errcap, "out of memory"); return NULL; }
    moonshine_blob blobs[MOONSHINE_FILES];
    for (int i = 0; i < MOONSHINE_FILES; i++) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", g_data_dir, MOONSHINE_FILE_NAMES[i]);
        if (oc_tts_map_open(path, &e->files[i]) != 0) {
            snprintf(err, errcap, "cannot map %s", path);
            engine_close(e);
            return NULL;
        }
        blobs[i].data = e->files[i].p;
        blobs[i].len = e->files[i].n;
    }
    char why[256] = "";
    e->model = moonshine_open(blobs, why, sizeof why);
    if (!e->model) { snprintf(err, errcap, "recognizer: %s", why); engine_close(e); return NULL; }
    return e;
}

static int engine_hear(void *engine, const int16_t *pcm, size_t samples, char **text, char *err, size_t errcap) {
    moonshine_engine *e = engine;
    float *f = malloc((samples ? samples : 1) * sizeof(float));
    if (!f) { snprintf(err, errcap, "out of memory"); return -1; }
    for (size_t i = 0; i < samples; i++) f[i] = (float)pcm[i] / 32768.0f;
    int rc = moonshine_hear(e->model, f, samples, text, err, errcap);
    free(f);
    return rc;
}

static const oc_stt_engine MOONSHINE_ENGINE = {
    .version = OC_STT_MODEL_VERSION,
    .lang = OC_STT_LANG,
    .ctx = NULL,
    .open = engine_open,
    .close = engine_close,
    .hear = engine_hear,
};

const oc_stt_engine *oc_stt_engine_for(const char *lang) {
    if (lang && *lang && strcmp(lang, MOONSHINE_ENGINE.lang) == 0) return &MOONSHINE_ENGINE;
    return NULL;
}

/* ---- --stt-hear ------------------------------------------------------------------ */

#define OC_PI 3.14159265358979323846

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* A 16-bit mono PCM WAV, any rate, to 16 kHz by windowed-sinc interpolation, so
 * a 24 kHz read-aloud rendering can be heard back without aliasing. */
static int16_t *read_wav_16k(const char *path, size_t *out_n, char *err, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, cap, "cannot read %s", path); return NULL; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    rewind(f);
    uint8_t *b = len > 44 ? malloc((size_t)len) : NULL;
    size_t got = b ? fread(b, 1, (size_t)len, f) : 0;
    fclose(f);
    if (!b || got != (size_t)len || memcmp(b, "RIFF", 4) || memcmp(b + 8, "WAVE", 4)) {
        free(b);
        snprintf(err, cap, "%s is not a WAV file", path);
        return NULL;
    }
    uint32_t rate = 0;
    const uint8_t *data = NULL;
    size_t data_len = 0;
    for (size_t off = 12; off + 8 <= (size_t)len;) {
        uint32_t clen = rd32(b + off + 4);
        if (!memcmp(b + off, "fmt ", 4) && clen >= 16) {
            if (rd16(b + off + 8) != 1 || rd16(b + off + 10) != 1 || rd16(b + off + 22) != 16) {
                free(b);
                snprintf(err, cap, "%s: need 16-bit mono PCM", path);
                return NULL;
            }
            rate = rd32(b + off + 12);
        } else if (!memcmp(b + off, "data", 4)) {
            data = b + off + 8;
            data_len = clen <= (size_t)len - off - 8 ? clen : (size_t)len - off - 8;
            break;
        }
        off += 8 + clen + (clen & 1);
    }
    if (!rate || !data) { free(b); snprintf(err, cap, "%s: no format or data", path); return NULL; }
    size_t in_n = data_len / 2;
    size_t n = (size_t)((double)in_n * MOONSHINE_RATE / rate);
    int16_t *out = malloc((n ? n : 1) * sizeof *out);
    if (!out) { free(b); snprintf(err, cap, "out of memory"); return NULL; }
    double ratio = (double)rate / MOONSHINE_RATE, cutoff = ratio > 1 ? 1.0 / ratio : 1.0;
    const int taps = 16;
    for (size_t i = 0; i < n; i++) {
        double x = (double)i * ratio, acc = 0, wsum = 0;
        long c = (long)floor(x);
        for (long k = c - taps + 1; k <= c + taps; k++) {
            if (k < 0 || (size_t)k >= in_n) continue;
            double t = x - (double)k, s = t == 0 ? 1.0 : sin(OC_PI * t * cutoff) / (OC_PI * t * cutoff);
            double win = 0.5 + 0.5 * cos(OC_PI * t / (taps + 1));
            double wgt = s * win * cutoff;
            acc += wgt * (int16_t)rd16(data + 2 * (size_t)k);
            wsum += wgt;
        }
        double v = wsum != 0 ? acc / wsum : 0;
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    free(b);
    *out_n = n;
    return out;
}

int oc_stt_hear(const char *path) {
    char err[512] = "";
    if (!oc_stt_data_ready(err, sizeof err)) { fprintf(stderr, "openchimed: %s\n", err); return 1; }
    size_t n = 0;
    int16_t *pcm = read_wav_16k(path, &n, err, sizeof err);
    if (!pcm) { fprintf(stderr, "openchimed: %s\n", err); return 1; }
    double t0 = now_s();
    void *engine = engine_open(NULL, err, sizeof err);
    if (!engine) { free(pcm); fprintf(stderr, "openchimed: %s\n", err); return 1; }
    double t1 = now_s();
    char *text = NULL;
    int rc = engine_hear(engine, pcm, n, &text, err, sizeof err);
    double t2 = now_s();
    engine_close(engine);
    free(pcm);
    if (rc != 0) { fprintf(stderr, "openchimed: %s\n", err); return 1; }
    double secs = (double)n / MOONSHINE_RATE;
    printf("%s\n", text);
    fprintf(stderr, "openchimed: model loaded in %.2f s; %.1f s of audio heard in %.2f s (%.2fx real time)\n",
            t1 - t0, secs, t2 - t1, secs > 0 ? (t2 - t1) / secs : 0.0);
    free(text);
    return 0;
}
