/* Read-aloud synthesis built into the daemon (tts.h). */
#define _POSIX_C_SOURCE 200809L
#include "tts.h"

#include "speakable.h"
#include "tts_embed.h"
#include "tts_kitten.h"
#include "tts_render.h"
#include "ttskit.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ---- the engine ------------------------------------------------------------------ */

typedef struct {
    tts    *pron;
    kitten *model;
} kitten_engine;

static void *engine_open(void *ctx, char *err, size_t errcap) {
    (void)ctx;
    kitten_engine *k = calloc(1, sizeof *k);
    if (!k) { snprintf(err, errcap, "out of memory"); return NULL; }
    char why[256] = "";
    k->pron = tts_load_mem(oc_tts_lexicon_en_us, OC_TTS_BLOB_LEN(oc_tts_lexicon_en_us), oc_tts_guesses_en_us,
                           OC_TTS_BLOB_LEN(oc_tts_guesses_en_us), OC_TTS_LANG, why, sizeof why);
    if (!k->pron) { snprintf(err, errcap, "pronunciation data: %s", why); free(k); return NULL; }
    k->model = kitten_open(oc_tts_kitten_ort, OC_TTS_BLOB_LEN(oc_tts_kitten_ort), oc_tts_kitten_voices,
                           OC_TTS_BLOB_LEN(oc_tts_kitten_voices), why, sizeof why);
    if (!k->model) { snprintf(err, errcap, "voice model: %s", why); tts_free(k->pron); free(k); return NULL; }
    return k;
}

static void engine_close(void *engine) {
    kitten_engine *k = engine;
    if (!k) return;
    kitten_close(k->model);
    tts_free(k->pron);
    free(k);
}

static int engine_say(void *engine, const char *segment, int voice, float **pcm, size_t *samples,
                      char *err, size_t errcap) {
    kitten_engine *k = engine;
    *pcm = NULL;
    *samples = 0;
    char ipa[4 * OC_TTS_SEGMENT_CHARS + 1024];
    if (!tts_text(k->pron, segment, ipa, sizeof ipa)) return 1;
    /* Characters, not bytes: the style row is indexed the way Python's len()
     * counts, so a segment of accented text must not read as a longer one. */
    size_t chars = 0;
    for (const unsigned char *p = (const unsigned char *)segment; *p; p++)
        if ((*p & 0xC0) != 0x80) chars++;
    return kitten_say(k->model, ipa, chars, voice, pcm, samples, err, errcap) == 0 ? 0 : -1;
}

/* The names a person sees, from the model's own voice aliases. */
static const char *VOICE_LABELS[KITTEN_VOICES] = {
    "Jasper", "Bella", "Bruno", "Luna", "Hugo", "Rosie", "Leo", "Kiki",
};

static const char *engine_voice_id(int voice) { return kitten_voice_name(voice); }
static const char *engine_voice_label(int voice) {
    return voice >= 0 && voice < KITTEN_VOICES ? VOICE_LABELS[voice] : NULL;
}

static const oc_tts_engine KITTEN_ENGINE = {
    .version = OC_TTS_MODEL_VERSION,
    .lang = OC_TTS_LANG,
    .rate = KITTEN_RATE,
    .voices = KITTEN_VOICES,
    .ctx = NULL,
    .voice_id = engine_voice_id,
    .voice_label = engine_voice_label,
    .preview = "This is how your messages are read aloud.",
    .open = engine_open,
    .close = engine_close,
    .say = engine_say,
};

const oc_tts_engine *oc_tts_engine_for(const char *lang) {
    /* Asked by name even with one row, so that adding the second is adding a row
     * rather than finding every caller that assumed there was only ever one. */
    if (lang && *lang && strcmp(lang, KITTEN_ENGINE.lang) == 0) return &KITTEN_ENGINE;
    return NULL;
}

/* ---- --tts-say ------------------------------------------------------------------- */

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void wr16(FILE *f, uint16_t v) { fputc(v & 0xFF, f); fputc(v >> 8, f); }
static void wr32(FILE *f, uint32_t v) { wr16(f, (uint16_t)v); wr16(f, (uint16_t)(v >> 16)); }

/* The raw voice, unencoded: every segment's samples one after another. */
static int say_wav(void *engine, const char *text, int voice, const char *path, double *secs) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "openchimed: cannot write %s\n", path); return 1; }
    fwrite("RIFF\0\0\0\0WAVEfmt ", 1, 16, f);
    wr32(f, 16); wr16(f, 1); wr16(f, 1); wr32(f, KITTEN_RATE); wr32(f, KITTEN_RATE * 2); wr16(f, 2); wr16(f, 16);
    fwrite("data\0\0\0\0", 1, 8, f);
    size_t starts[256], lens[256], total = 0;
    int segs = oc_speakable_segments(text, strlen(text), OC_TTS_SEGMENT_CHARS, starts, lens, 256);
    char err[512] = "";
    for (int s = 0; s < segs; s++) {
        char seg[OC_TTS_SEGMENT_CHARS * 4 + 1];
        snprintf(seg, sizeof seg, "%.*s", (int)lens[s], text + starts[s]);
        float *pcm;
        size_t n;
        int r = engine_say(engine, seg, voice, &pcm, &n, err, sizeof err);
        if (r < 0) { fprintf(stderr, "openchimed: %s\n", err); fclose(f); return 1; }
        for (size_t i = 0; i < n; i++) wr16(f, (uint16_t)(int16_t)(pcm[i] * 32767.0f));
        total += n;
        free(pcm);
    }
    fseek(f, 4, SEEK_SET); wr32(f, (uint32_t)(36 + total * 2));
    fseek(f, 40, SEEK_SET); wr32(f, (uint32_t)(total * 2));
    *secs = (double)total / KITTEN_RATE;
    return fclose(f) == 0 && total ? 0 : 1;
}

int oc_tts_say(const char *voice_name, const char *text, const char *path) {
    int voice = kitten_voice_index(voice_name);
    if (voice < 0) {
        fprintf(stderr, "openchimed: no voice '%s'; voices:", voice_name);
        for (int i = 0; i < KITTEN_VOICES; i++) fprintf(stderr, " %s", kitten_voice_name(i));
        fprintf(stderr, "\n");
        return 2;
    }
    char err[512] = "";
    double t0 = now_s();
    void *engine = engine_open(NULL, err, sizeof err);
    if (!engine) { fprintf(stderr, "openchimed: %s\n", err); return 1; }
    double t1 = now_s(), secs = 0;
    size_t plen = strlen(path);
    int rc;
    if (plen > 4 && strcmp(path + plen - 4, ".wav") == 0) {
        rc = say_wav(engine, text, voice, path, &secs);
    } else {
        uint8_t *mp4;
        size_t len;
        uint32_t ms;
        oc_tts_status st = oc_tts_render(&KITTEN_ENGINE, engine, text, voice, &mp4, &len, &ms, err, sizeof err);
        rc = 1;
        if (st == OC_TTS_NOTHING) fprintf(stderr, "openchimed: nothing to say\n");
        else if (st != OC_TTS_OK) fprintf(stderr, "openchimed: %s\n", err);
        else {
            FILE *f = fopen(path, "wb");
            if (f) {
                int wrote = fwrite(mp4, 1, len, f) == len;
                if (fclose(f) == 0 && wrote) rc = 0;
            }
            if (rc != 0) fprintf(stderr, "openchimed: cannot write %s\n", path);
            secs = ms / 1000.0;
            free(mp4);
            if (rc == 0) fprintf(stderr, "openchimed: %zu bytes (%.1f kbit/s)\n", len, len * 8.0 / 1000.0 / (secs > 0 ? secs : 1));
        }
    }
    double t2 = now_s();
    if (rc == 0)
        fprintf(stderr, "openchimed: model loaded in %.2f s; %.1f s of audio in %.2f s (%.2fx real time) -> %s\n",
                t1 - t0, secs, t2 - t1, (t2 - t1) / (secs > 0 ? secs : 1), path);
    engine_close(engine);
    return rc;
}
