/* Kitten mini through the ONNX Runtime C API (tts_kitten.h). */
#define _POSIX_C_SOURCE 200809L
#include "tts_kitten.h"

#include <onnxruntime_c_api.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define STYLE_ROWS 400
#define STYLE_DIM  256
#define TAIL_TRIM  5000            /* Kitten's flat cut, kept here as a floor   */
#define TAIL_WIN   120             /* 5 ms: the window the quiet test measures  */
#define TAIL_QUIET 0.015f          /* rms below this is a pause, not a word     */
#define TAIL_FADE  240             /* 10 ms ramp to zero, so no cut can click   */

/* Kitten's voices, in the order the helper numbers them. */
static const char *VOICE_NAMES[KITTEN_VOICES] = {
    "expr-voice-2-m", "expr-voice-2-f", "expr-voice-3-m", "expr-voice-3-f",
    "expr-voice-4-m", "expr-voice-4-f", "expr-voice-5-m", "expr-voice-5-f",
};

struct kitten {
    const OrtApi *ort;
    OrtEnv *env;
    OrtSession *session;
    OrtMemoryInfo *mem;
    const uint8_t *npz;                              /* voices.npz, lent by the caller */
    size_t npz_len;
    const uint8_t *style[KITTEN_VOICES];             /* STYLE_ROWS x STYLE_DIM little-endian f32 */
};

static void seterr(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* An ORT status to a message; frees the status. Returns -1 if it was an error. */
static int failed(const OrtApi *ort, OrtStatus *st, const char *what, char *err, size_t cap) {
    if (!st) return 0;
    seterr(err, cap, "%s: %s", what, ort->GetErrorMessage(st));
    ort->ReleaseStatus(st);
    return -1;
}

const char *kitten_voice_name(int voice) {
    return voice >= 0 && voice < KITTEN_VOICES ? VOICE_NAMES[voice] : NULL;
}

int kitten_voice_index(const char *name) {
    for (int i = 0; name && i < KITTEN_VOICES; i++)
        if (!strcmp(name, VOICE_NAMES[i])) return i;
    return -1;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* voices.npz is a zip of one .npy per voice, written by numpy.savez: stored, not
 * compressed, so each array's bytes can be used where they lie. Anything else --
 * a compressed member, another dtype or shape -- is refused, not guessed at. */
static int read_voices(kitten *k, char *err, size_t cap) {
    const uint8_t *p = k->npz, *end = k->npz + k->npz_len;
    while (p + 30 <= end && le32(p) == 0x04034b50) {
        uint16_t method = le16(p + 8), flags = le16(p + 6), name_len = le16(p + 26), extra_len = le16(p + 28);
        uint64_t size = le32(p + 18);
        const uint8_t *name = p + 30, *extra = name + name_len, *data = extra + extra_len;
        if (data > end || (flags & 0x08)) break;
        /* numpy writes zip64 members: the 32-bit size is 0xFFFFFFFF and the real
         * one is in the zip64 extra field (id 1: uncompressed, then compressed). */
        for (const uint8_t *e = extra; size == 0xFFFFFFFFu && e + 4 <= data;) {
            uint16_t id = le16(e), len = le16(e + 2);
            if (id == 1 && len >= 16 && e + 4 + 16 <= data) size = le32(e + 12) | (uint64_t)le32(e + 16) << 32;
            e += 4 + len;
        }
        if ((uint64_t)(end - data) < size) break;
        if (method != 0) { seterr(err, cap, "voices.npz: compressed members are not supported"); return -1; }
        char nm[64];
        size_t nl = name_len < sizeof nm - 1 ? name_len : sizeof nm - 1;
        memcpy(nm, name, nl);
        nm[nl] = '\0';
        if (nl > 4 && !strcmp(nm + nl - 4, ".npy")) {
            nm[nl - 4] = '\0';
            int v = kitten_voice_index(nm);
            if (v >= 0) {
                /* .npy: "\x93NUMPY", version, header length, a dict literal, data. */
                if (size < 10 || memcmp(data, "\x93NUMPY", 6) != 0) { seterr(err, cap, "voices.npz: %s is not .npy", nm); return -1; }
                size_t hl = data[6] == 1 ? 10 + (size_t)le16(data + 8) : 12 + (size_t)le32(data + 8);
                if (hl > size) { seterr(err, cap, "voices.npz: %s header is truncated", nm); return -1; }
                const char *h = (const char *)data + (data[6] == 1 ? 10 : 12);
                size_t hlen = hl - (data[6] == 1 ? 10 : 12);
                char hdr[256];
                snprintf(hdr, sizeof hdr, "%.*s", (int)(hlen < sizeof hdr ? hlen : sizeof hdr - 1), h);
                if (!strstr(hdr, "'descr': '<f4'") || !strstr(hdr, "'fortran_order': False") ||
                    !strstr(hdr, "'shape': (400, 256)") || size - hl != (size_t)STYLE_ROWS * STYLE_DIM * 4) {
                    seterr(err, cap, "voices.npz: %s is not a 400 x 256 float32 table", nm);
                    return -1;
                }
                k->style[v] = data + hl;
            }
        }
        p = data + size;
    }
    for (int v = 0; v < KITTEN_VOICES; v++)
        if (!k->style[v]) { seterr(err, cap, "voices.npz: no voice %s", VOICE_NAMES[v]); return -1; }
    return 0;
}

kitten *kitten_open(const unsigned char *model, size_t model_len, const unsigned char *voices,
                    size_t voices_len, char *err, size_t errcap) {
    kitten *k = calloc(1, sizeof *k);
    if (!k) { seterr(err, errcap, "out of memory"); return NULL; }
    k->npz = voices;
    k->npz_len = voices_len;
    if (!voices || read_voices(k, err, errcap) != 0) { if (!voices) seterr(err, errcap, "no voices"); kitten_close(k); return NULL; }

    k->ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!k->ort) { seterr(err, errcap, "ONNX Runtime does not provide API version %d", ORT_API_VERSION); kitten_close(k); return NULL; }
    const OrtApi *ort = k->ort;
    if (failed(ort, ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "openchimed-tts", &k->env), "CreateEnv", err, errcap)) { kitten_close(k); return NULL; }

    OrtSessionOptions *so = NULL;
    if (failed(ort, ort->CreateSessionOptions(&so), "CreateSessionOptions", err, errcap)) { kitten_close(k); return NULL; }
    /* One thread: renders are one sentence at a time on one core, beside the net
     * loop. The CPU arena stays on: measured on a typical sentence, turning it off
     * RAISES the peak (219 MB with it, 282 MB without), because unpooled
     * allocations fragment. The model's bytes are used in place, not copied, so
     * the embedded weights cost only the pages inference touches. */
    int bad = failed(ort, ort->SetIntraOpNumThreads(so, 1), "SetIntraOpNumThreads", err, errcap) ||
              failed(ort, ort->SetInterOpNumThreads(so, 1), "SetInterOpNumThreads", err, errcap) ||
              failed(ort, ort->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL), "SetSessionGraphOptimizationLevel", err, errcap) ||
              failed(ort, ort->AddSessionConfigEntry(so, "session.use_ort_model_bytes_directly", "1"), "model bytes in place", err, errcap) ||
              failed(ort, ort->AddSessionConfigEntry(so, "session.use_ort_model_bytes_for_initializers", "1"), "weights in place", err, errcap);
    if (!bad) bad = failed(ort, ort->CreateSessionFromArray(k->env, model, model_len, so, &k->session), "the voice model", err, errcap);
    ort->ReleaseSessionOptions(so);
    if (!bad) bad = failed(ort, ort->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &k->mem), "CreateCpuMemoryInfo", err, errcap);
    if (bad) { kitten_close(k); return NULL; }
    return k;
}

void kitten_close(kitten *k) {
    if (!k) return;
    if (k->ort) {
        if (k->mem) k->ort->ReleaseMemoryInfo(k->mem);
        if (k->session) k->ort->ReleaseSession(k->session);
        if (k->env) k->ort->ReleaseEnv(k->env);
    }
    free(k);
}

size_t kitten_style_row(size_t chars) { return chars < STYLE_ROWS - 1 ? chars : STYLE_ROWS - 1; }

int kitten_say(kitten *k, const char *ipa, size_t text_chars, int voice, float **pcm, size_t *samples,
               char *err, size_t errcap) {
    *pcm = NULL;
    *samples = 0;
    if (voice < 0 || voice >= KITTEN_VOICES) { seterr(err, errcap, "no voice %d", voice); return -1; }
    const OrtApi *ort = k->ort;

    int64_t ids[KITTEN_MAX_TOKENS];
    size_t n = kitten_tokens(ipa, (long long *)ids, KITTEN_MAX_TOKENS);
    if (n <= 3) { seterr(err, errcap, "nothing to say"); return -1; }

    /* The style row is chosen by how many CHARACTERS were spoken, which is what
     * Kitten's own code indexes with (`min(len(text), rows - 1)`) and what these
     * rows were fitted to. Indexing with the token count instead -- about twice
     * as large, since the IPA of a word is longer than the word -- reaches rows
     * meant for far longer sentences: the model then paces a two-letter word
     * like a clause, emitting a click and most of a second of silence. */
    float style[STYLE_DIM];
    size_t row = kitten_style_row(text_chars);
    const uint8_t *src = k->style[voice] + row * STYLE_DIM * 4;
    for (int i = 0; i < STYLE_DIM; i++) { uint32_t u = le32(src + 4 * i); memcpy(&style[i], &u, 4); }
    float speed = KITTEN_SPEED;

    int64_t ids_shape[2] = { 1, (int64_t)n }, style_shape[2] = { 1, STYLE_DIM }, speed_shape[1] = { 1 };
    OrtValue *in[3] = { NULL, NULL, NULL }, *out = NULL;
    int rc = -1;
    if (failed(ort, ort->CreateTensorWithDataAsOrtValue(k->mem, ids, n * sizeof *ids, ids_shape, 2,
               ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[0]), "input_ids", err, errcap) ||
        failed(ort, ort->CreateTensorWithDataAsOrtValue(k->mem, style, sizeof style, style_shape, 2,
               ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[1]), "style", err, errcap) ||
        failed(ort, ort->CreateTensorWithDataAsOrtValue(k->mem, &speed, sizeof speed, speed_shape, 1,
               ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[2]), "speed", err, errcap))
        goto done;

    static const char *IN_NAMES[3] = { "input_ids", "style", "speed" };
    static const char *OUT_NAMES[1] = { "waveform" };
    if (failed(ort, ort->Run(k->session, NULL, IN_NAMES, (const OrtValue *const *)in, 3, OUT_NAMES, 1, &out), "Run", err, errcap))
        goto done;

    OrtTensorTypeAndShapeInfo *info = NULL;
    size_t count = 0;
    float *data = NULL;
    if (failed(ort, ort->GetTensorTypeAndShape(out, &info), "GetTensorTypeAndShape", err, errcap)) goto done;
    int bad = failed(ort, ort->GetTensorShapeElementCount(info, &count), "GetTensorShapeElementCount", err, errcap);
    ort->ReleaseTensorTypeAndShapeInfo(info);
    if (bad || failed(ort, ort->GetTensorMutableData(out, (void **)&data), "GetTensorMutableData", err, errcap)) goto done;
    if (count == 0) { seterr(err, errcap, "the model produced no audio"); goto done; }

    *pcm = malloc(count * sizeof **pcm);
    if (!*pcm) { seterr(err, errcap, "out of memory"); goto done; }
    /* Take the offset out first. What this model calls silence is not zero: it
     * sits about 0.06 above it, and rises further under the words. Opus removes
     * the offset itself, which is the trouble -- fed a step down from 0.06 to
     * nothing, its filter swings the other way and takes most of a second to
     * settle, so every rendering ended on an audible thump with the last word
     * smeared into it. A 20 Hz high pass, an octave below anything a voice
     * produces, so what is removed is only the offset. */
    const double r = 1.0 - 2.0 * 3.14159265358979 * 20.0 / (double)KITTEN_RATE;
    double x1 = 0, y1 = 0;
    for (size_t i = 0; i < count; i++) {
        double x = data[i];
        double y = x - x1 + r * y1;
        x1 = x;
        y1 = y;
        (*pcm)[i] = (float)(y > 1.0 ? 1.0 : y < -1.0 ? -1.0 : y);
    }

    /* The model appends a burst of noise after the words, and Kitten's own code
     * drops it by cutting the last 5,000 samples flat. That cut is a guess about
     * how much silence the model left, and at 1.2x it is often wrong: on a short
     * line it lands INSIDE the last word, which ends the render mid-waveform and
     * the word sounds bitten off. So take the flat cut as a floor and then run on
     * to the first quiet moment, which is where the words actually stopped; the
     * burst is always the far side of that quiet, so it is still dropped. When
     * there is no quiet left to find, the flat cut stands. Measured after the
     * high pass, since before it even silence carries the offset's energy. */
    size_t end = count > TAIL_TRIM * 2 ? count - TAIL_TRIM : count;
    for (size_t i = end; i + TAIL_WIN <= count; i += TAIL_WIN) {
        double sum = 0;
        for (size_t j = 0; j < TAIL_WIN; j++) sum += (double)(*pcm)[i + j] * (*pcm)[i + j];
        if (sum / TAIL_WIN < (double)TAIL_QUIET * TAIL_QUIET) { end = i; break; }
    }
    count = end;

    /* Land on zero. Wherever the cut falls, the last sample is whatever the wave
     * happened to be doing, and a speaker asked to jump from there to silence
     * makes a click -- so ramp the final few milliseconds down instead. Short
     * enough to be inaudible as a fade, long enough that no step is left. */
    size_t fade = count < TAIL_FADE ? count : TAIL_FADE;
    for (size_t i = 0; i < fade; i++)
        (*pcm)[count - fade + i] *= (float)(fade - 1 - i) / (float)fade;
    *samples = count;
    rc = 0;
done:
    for (int i = 0; i < 3; i++) if (in[i]) ort->ReleaseValue(in[i]);
    if (out) ort->ReleaseValue(out);
    return rc;
}
