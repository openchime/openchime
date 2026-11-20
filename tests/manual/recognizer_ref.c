/* MANUAL: makes voice input's recognizer references, the fixtures in
 * tests/data/recognizer/ that sttkit's own run of the model is compared against
 * (docs/STTKIT.md §8).
 *
 * Deliberately NOT part of any suite or Makefile target: it runs the published
 * .ort graphs with the full prebuilt ONNX Runtime, which nothing in the build
 * links. Its outputs are committed; rerun it only to remake them, and then
 * commit what it writes.
 *
 * Build and run from the repository folder, with the published files fetched as
 * docs/VENDORS.md says (at its pinned URLs and SHA-256s) into a folder SRC, and
 * ONNX Runtime 1.30.0's prebuilt release for this machine
 * (onnxruntime-linux-<x64|aarch64>-1.30.0.tgz) unpacked into PRE:
 *
 *     PRE=third_party/onnxruntime-1.30.0-prebuilt
 *     cc -std=c99 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -ffp-contract=off -I"$PRE/include" \
 *        -Ittskit -Innkit -Isttkit -Idaemon tests/manual/recognizer_ref.c daemon/tts_voice_tokens.c \
 *        $(find ttskit nnkit -maxdepth 1 -name '*.c' ! -name tts_pack.c) sttkit/stt_tokens.c \
 *        -L"$PRE/lib" -lonnxruntime -Wl,-rpath,"$PWD/$PRE/lib" -lm -o build/recognizer_ref
 *     build/recognizer_ref SRC ttskit/data tests/data/recognizer
 *
 * Each fixture is speech the read-aloud voice renders as the daemon renders it
 * (ttskit's text, the daemon's token function, the style row for the text's
 * length, the noise on with the daemon's seed), written to 16-bit samples as a
 * WAV holds them and brought to 16 kHz by the windowed-sinc resampler
 * `openchimed --stt-hear` uses -- or, for `silence`, 1.5 s of zeros. No
 * recording of a person is used. Each is then heard as voice input hears an
 * utterance: the frontend over 1,280-sample chunks, the encoder, the adapter,
 * the cross-attention keys and values, and greedy decoding from the start
 * token with the self-attention cache, stopped at the end token or the
 * tokens-per-second ceiling.
 *
 * Files written to the output folder, per fixture <name>:
 *   <name>.s16         the 16 kHz input, int16 little-endian
 *   <name>.enc.f32     the encoder's output, frames x 320 float32 little-endian
 *   <name>.logits.f32  the first decoding step's logits, 32,768 float32
 * and one `manifest`, plain text: '#' lines are comments; every other line is
 *   <name> <samples> <frames> <tokens> <ids> <text...>
 * with <ids> the decoded token ids joined by commas ("-" for none), and <text>
 * what the tokenizer makes of them. Run twice on one machine it writes the same
 * bytes. */
#include <onnxruntime_c_api.h>

#include "ttskit.h"
#include "tts_voice.h"
#include "sttkit.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RATE  16000
#define CHUNK 1280
#define LANG  "en-US"

typedef struct { const char *name, *voice; float speed; const char *text; } fixture;

static const fixture FIXTURES[] = {
    { "greeting-m", "expr-voice-2-m", 1.2f, "Good morning, everyone. The build is green again." },
    { "numbers-f",  "expr-voice-3-f", 1.2f, "Meet me at 3:15 on Tuesday in room 204." },
    { "names-m",    "expr-voice-4-m", 1.2f, "Ask Priya to send the quarterly report to Marcus." },
    { "question-f", "expr-voice-5-f", 1.2f, "Can we move the release to Friday afternoon?" },
    { "long-f",     "expr-voice-2-f", 1.2f,
      "When the deployment finishes, check the logs and post a short summary in the channel." },
    { "rushed-m",   "expr-voice-3-m", 2.6f,
      "Okay so first the database migration then the cache warmup then the load test then the release notes "
      "then the announcement and finally the retrospective." },
    { "silence",    NULL,             0.0f, NULL },
};
#define NFIX (sizeof FIXTURES / sizeof FIXTURES[0])

static const char *const FILES[] = { "frontend.model.ort", "frontend.weights.ort", "encoder.ort", "adapter.ort",
                                     "cross_kv.ort", "decoder_kv.ort", "tokenizer.bin" };
enum { F_FRONT, F_WEIGHTS, F_ENC, F_ADAPT, F_CROSS, F_DEC, F_TOK, NFILES };

static const OrtApi *ort;
static OrtEnv *env;
static OrtMemoryInfo *mem;

static void die(const char *what) { fprintf(stderr, "recognizer_ref: %s\n", what); exit(1); }
static void ck(OrtStatus *st, const char *what) {
    if (!st) return;
    fprintf(stderr, "recognizer_ref: %s: %s\n", what, ort->GetErrorMessage(st));
    exit(1);
}

static unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "recognizer_ref: cannot read %s\n", path); exit(1); }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *p = malloc((size_t)len + 1);
    if (!p || fread(p, 1, (size_t)len, f) != (size_t)len) die("short read");
    fclose(f);
    *n = (size_t)len;
    return p;
}

static void spill(const char *dir, const char *name, const char *ext, const void *p, size_t n) {
    char path[4200];
    snprintf(path, sizeof path, "%s/%s.%s", dir, name, ext);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n || fclose(f) != 0) { fprintf(stderr, "recognizer_ref: cannot write %s\n", path); exit(1); }
}

static OrtSession *session(const unsigned char *b, size_t n) {
    OrtSessionOptions *so;
    OrtSession *s;
    ck(ort->CreateSessionOptions(&so), "options");
    ck(ort->SetIntraOpNumThreads(so, 1), "threads");
    ck(ort->SetInterOpNumThreads(so, 1), "threads");
    ck(ort->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL), "optimization");
    ck(ort->CreateSessionFromArray(env, b, n, so, &s), "load");
    ort->ReleaseSessionOptions(so);
    return s;
}

static OrtValue *tensor(void *data, size_t bytes, const int64_t *shape, size_t dims, ONNXTensorElementDataType ty) {
    OrtValue *v;
    ck(ort->CreateTensorWithDataAsOrtValue(mem, data, bytes, shape, dims, ty, &v), "tensor");
    return v;
}

static void *data_of(OrtValue *v) {
    void *p;
    ck(ort->GetTensorMutableData(v, &p), "data");
    return p;
}

static int64_t dim_of(OrtValue *v, size_t k) {
    OrtTensorTypeAndShapeInfo *info;
    int64_t shape[8];
    size_t n;
    ck(ort->GetTensorTypeAndShape(v, &info), "shape");
    ck(ort->GetDimensionsCount(info, &n), "shape");
    ck(ort->GetDimensions(info, shape, n), "shape");
    ort->ReleaseTensorTypeAndShapeInfo(info);
    return k < n ? shape[k] : -1;
}

/* ---- the speech --------------------------------------------------------------- */

static size_t utf8_chars(const char *s) {
    size_t n = 0;
    for (; *s; s++) n += ((unsigned char)*s & 0xC0) != 0x80;
    return n;
}

/* `openchimed --stt-hear`'s resampler, any rate to 16 kHz. */
static int16_t *to_16k(const int16_t *in, size_t in_n, unsigned rate, size_t *out_n) {
    size_t n = (size_t)((double)in_n * RATE / rate);
    int16_t *out = malloc((n ? n : 1) * sizeof *out);
    if (!out) die("out of memory");
    double ratio = (double)rate / RATE, cutoff = ratio > 1 ? 1.0 / ratio : 1.0;
    const int taps = 16;
    for (size_t i = 0; i < n; i++) {
        double x = (double)i * ratio, acc = 0, wsum = 0;
        long c = (long)floor(x);
        for (long k = c - taps + 1; k <= c + taps; k++) {
            if (k < 0 || (size_t)k >= in_n) continue;
            double t = x - (double)k, s = t == 0 ? 1.0 : sin(M_PI * t * cutoff) / (M_PI * t * cutoff);
            double win = 0.5 + 0.5 * cos(M_PI * t / (taps + 1));
            double wgt = s * win * cutoff;
            acc += wgt * in[k];
            wsum += wgt;
        }
        double v = wsum != 0 ? acc / wsum : 0;
        out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    *out_n = n;
    return out;
}

static int16_t *speak(tts *t, voice *v, const fixture *fx, size_t *n16) {
    if (!fx->text) {
        *n16 = RATE * 3 / 2;
        int16_t *z = calloc(*n16, sizeof *z);
        if (!z) die("out of memory");
        return z;
    }
    char ipa[8192], err[512] = "";
    long long ids64[TTS_VOICE_MAX_TOKENS];
    int ids[TTS_VOICE_MAX_TOKENS];
    float style[VOICE_STYLE_DIM];
    if (!tts_text(t, fx->text, ipa, sizeof ipa)) die("nothing to say");
    size_t tokens = tts_voice_tokens(ipa, ids64, TTS_VOICE_MAX_TOKENS);
    for (size_t i = 0; i < tokens; i++) ids[i] = (int)ids64[i];
    if (voice_style(v, fx->voice, utf8_chars(fx->text), style) != 0) die("no such voice");
    size_t n = 0;
    float *pcm = voice_say(v, ids, (int)tokens, style, fx->speed, 1, TTS_VOICE_SEED, &n, err, sizeof err);
    if (!pcm) die(err);
    int16_t *s16 = malloc((n ? n : 1) * sizeof *s16);
    if (!s16) die("out of memory");
    for (size_t i = 0; i < n; i++) {
        float s = pcm[i] * 32767.0f;
        s = s > 32767.0f ? 32767.0f : s < -32768.0f ? -32768.0f : s;
        s16[i] = (int16_t)lrintf(s);
    }
    free(pcm);
    int16_t *out = to_16k(s16, n, VOICE_RATE, n16);
    free(s16);
    return out;
}

/* ---- hearing it ------------------------------------------------------------- */

typedef struct { char *name; int64_t shape[4]; size_t dims; float *data; } weight;

static size_t weights(OrtSession *ws, weight *w) {
    OrtAllocator *al;
    size_t n;
    char *names[4];
    OrtValue *out[4] = { 0 };
    ck(ort->GetAllocatorWithDefaultOptions(&al), "allocator");
    ck(ort->SessionGetOutputCount(ws, &n), "weights");
    if (n > 4) die("frontend weights: too many outputs");
    for (size_t i = 0; i < n; i++) ck(ort->SessionGetOutputName(ws, i, al, &names[i]), "weights");
    ck(ort->Run(ws, NULL, NULL, NULL, 0, (const char *const *)names, n, out), "weights");
    for (size_t i = 0; i < n; i++) {
        OrtTensorTypeAndShapeInfo *info;
        size_t count;
        ck(ort->GetTensorTypeAndShape(out[i], &info), "weights");
        ck(ort->GetDimensionsCount(info, &w[i].dims), "weights");
        ck(ort->GetDimensions(info, w[i].shape, w[i].dims), "weights");
        ck(ort->GetTensorShapeElementCount(info, &count), "weights");
        ort->ReleaseTensorTypeAndShapeInfo(info);
        w[i].name = strdup(names[i]);
        w[i].data = malloc(count * sizeof(float));
        memcpy(w[i].data, data_of(out[i]), count * sizeof(float));
        ort->ReleaseValue(out[i]);
        al->Free(al, names[i]);
    }
    return n;
}

static size_t max_tokens(size_t samples) {
    size_t n = (size_t)ceil((double)samples / RATE * 6.5);
    return n > 256 ? 256 : n;
}

/* One utterance: returns the token count, with the encoder output, its frame
 * count, the first step's logits and the ids filled in. */
static size_t hear(OrtSession **s, const weight *w, size_t nw, const float *pcm, size_t samples, float **enc,
                   int64_t *frames, float *logits0, int64_t *ids) {
    float sbuf[79] = { 0 }, chunk[CHUNK];
    int64_t slen = 0, fcount = 0;
    float *c1 = calloc(320 * 4, sizeof(float)), *c2 = calloc(640 * 4, sizeof(float)), *feat = NULL;
    size_t nfeat = 0, chunks = (samples + CHUNK - 1) / CHUNK;
    for (size_t k = 0; k < chunks; k++) {
        size_t off = k * CHUNK, n = samples - off < CHUNK ? samples - off : CHUNK;
        memcpy(chunk, pcm + off, n * sizeof(float));
        if (n < CHUNK) memset(chunk + n, 0, (CHUNK - n) * sizeof(float));
        int64_t s_a[] = { 1, CHUNK }, s_b[] = { 1, 79 }, s_1[] = { 1 }, s_c1[] = { 1, 320, 4 }, s_c2[] = { 1, 640, 4 };
        OrtValue *in[10];
        const char *names[10] = { "audio_chunk", "sample_buffer", "sample_len", "conv1_buffer", "conv2_buffer", "frame_count" };
        in[0] = tensor(chunk, sizeof chunk, s_a, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
        in[1] = tensor(sbuf, sizeof sbuf, s_b, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
        in[2] = tensor(&slen, sizeof slen, s_1, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
        in[3] = tensor(c1, 320 * 4 * sizeof(float), s_c1, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
        in[4] = tensor(c2, 640 * 4 * sizeof(float), s_c2, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
        in[5] = tensor(&fcount, sizeof fcount, s_1, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
        for (size_t i = 0; i < nw; i++) {
            size_t count = 1;
            for (size_t d = 0; d < w[i].dims; d++) count *= (size_t)w[i].shape[d];
            in[6 + i] = tensor(w[i].data, count * sizeof(float), w[i].shape, w[i].dims, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
            names[6 + i] = w[i].name;
        }
        static const char *const outn[] = { "features", "sample_buffer_out", "sample_len_out",
                                            "conv1_buffer_out", "conv2_buffer_out", "frame_count_out" };
        OrtValue *out[6] = { 0 };
        ck(ort->Run(s[F_FRONT], NULL, names, (const OrtValue *const *)in, 6 + nw, outn, 6, out), "frontend");
        for (size_t i = 0; i < 6 + nw; i++) ort->ReleaseValue(in[i]);
        int64_t f = dim_of(out[0], 1);
        feat = realloc(feat, (nfeat + (size_t)f) * 320 * sizeof(float));
        memcpy(feat + nfeat * 320, data_of(out[0]), (size_t)f * 320 * sizeof(float));
        nfeat += (size_t)f;
        memcpy(sbuf, data_of(out[1]), sizeof sbuf);
        slen = *(int64_t *)data_of(out[2]);
        memcpy(c1, data_of(out[3]), 320 * 4 * sizeof(float));
        memcpy(c2, data_of(out[4]), 640 * 4 * sizeof(float));
        fcount = *(int64_t *)data_of(out[5]);
        for (int i = 0; i < 6; i++) ort->ReleaseValue(out[i]);
    }
    free(c1);
    free(c2);
    *frames = (int64_t)nfeat;

    int64_t s_feat[] = { 1, (int64_t)nfeat, 320 }, s_1[] = { 1 }, pos = 0;
    OrtValue *v_feat = tensor(feat, nfeat * 320 * sizeof(float), s_feat, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    const char *n_feat = "features", *n_enc = "encoded";
    OrtValue *encoded = NULL, *memory = NULL, *kv[2] = { 0 };
    ck(ort->Run(s[F_ENC], NULL, &n_feat, (const OrtValue *const *)&v_feat, 1, &n_enc, 1, &encoded), "encoder");
    *enc = malloc(nfeat * 320 * sizeof(float));
    memcpy(*enc, data_of(encoded), nfeat * 320 * sizeof(float));
    OrtValue *v_pos = tensor(&pos, sizeof pos, s_1, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
    {
        const char *in_n[] = { "encoded", "pos_offset" }, *out_n[] = { "memory" };
        OrtValue *in[] = { encoded, v_pos };
        ck(ort->Run(s[F_ADAPT], NULL, in_n, (const OrtValue *const *)in, 2, out_n, 1, &memory), "adapter");
    }
    {
        const char *in_n[] = { "memory" }, *out_n[] = { "k_cross", "v_cross" };
        ck(ort->Run(s[F_CROSS], NULL, in_n, (const OrtValue *const *)&memory, 1, out_n, 2, kv), "cross_kv");
    }
    size_t cap = max_tokens(samples), nids = 0;
    float empty = 0;
    int64_t s_empty[] = { 6, 1, 8, 0, 40 };
    OrtValue *sk = tensor(&empty, 0, s_empty, 5, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    OrtValue *sv = tensor(&empty, 0, s_empty, 5, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT);
    int64_t token = 1;
    for (size_t step = 0; step <= cap; step++) {
        int64_t s_tok[] = { 1, 1 };
        OrtValue *v_tok = tensor(&token, sizeof token, s_tok, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64);
        const char *in_n[] = { "token", "k_self", "v_self", "out_k_cross", "out_v_cross" };
        const char *out_n[] = { "logits", "out_k_self", "out_v_self" };
        OrtValue *in[] = { v_tok, sk, sv, kv[0], kv[1] }, *out[3] = { 0 };
        ck(ort->Run(s[F_DEC], NULL, in_n, (const OrtValue *const *)in, 5, out_n, 3, out), "decoder");
        ort->ReleaseValue(v_tok);
        ort->ReleaseValue(sk);
        ort->ReleaseValue(sv);
        sk = out[1];
        sv = out[2];
        const float *lg = data_of(out[0]);
        if (step == 0) memcpy(logits0, lg, 32768 * sizeof(float));
        int best = 0;
        for (int i = 1; i < 32768; i++)
            if (lg[i] > lg[best]) best = i;
        ort->ReleaseValue(out[0]);
        if (best == 2 || step == cap) break;
        ids[nids++] = best;
        token = best;
    }
    ort->ReleaseValue(sk);
    ort->ReleaseValue(sv);
    ort->ReleaseValue(kv[0]);
    ort->ReleaseValue(kv[1]);
    ort->ReleaseValue(memory);
    ort->ReleaseValue(v_pos);
    ort->ReleaseValue(encoded);
    ort->ReleaseValue(v_feat);
    free(feat);
    return nids;
}

int main(int argc, char **argv) {
    if (argc != 4) { fprintf(stderr, "usage: recognizer_ref SRC TTSKIT_DATA OUT\n"); return 2; }
    const char *src = argv[1], *tdata = argv[2], *outdir = argv[3];
    unsigned char *blob[NFILES];
    size_t len[NFILES];
    for (int i = 0; i < NFILES; i++) {
        char path[4200];
        snprintf(path, sizeof path, "%s/%s", src, FILES[i]);
        blob[i] = slurp(path, &len[i]);
    }
    char err[512] = "", textdir[4200], vpath[4200];
    snprintf(textdir, sizeof textdir, "%s/%s", tdata, LANG);
    snprintf(vpath, sizeof vpath, "%s/voice.bin", tdata);
    tts *t = tts_load(textdir, LANG, err, sizeof err);
    if (!t) die(err);
    voice *v = voice_open(vpath, err, sizeof err);
    if (!v) die(err);
    stt_tokens tok;
    if (stt_tokens_load(&tok, blob[F_TOK], len[F_TOK]) != 0) die("tokenizer.bin is malformed");

    ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    ck(ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "recognizer_ref", &env), "env");
    ck(ort->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &mem), "memory");
    OrtSession *s[NFILES] = { 0 };
    for (int i = 0; i < F_TOK; i++) s[i] = session(blob[i], len[i]);
    weight w[4];
    size_t nw = weights(s[F_WEIGHTS], w);

    char mpath[4200];
    snprintf(mpath, sizeof mpath, "%s/manifest", outdir);
    FILE *m = fopen(mpath, "w");
    if (!m) die("cannot write the manifest");
    fprintf(m, "# Voice input's recognizer as ONNX Runtime 1.30.0 runs it (tests/manual/recognizer_ref.c).\n"
               "# <name> <samples> <frames> <tokens> <ids> <text...>\n");
    for (size_t k = 0; k < NFIX; k++) {
        const fixture *fx = &FIXTURES[k];
        size_t n = 0;
        int16_t *pcm16 = speak(t, v, fx, &n);
        float *pcm = malloc(n * sizeof(float));
        for (size_t i = 0; i < n; i++) pcm[i] = (float)pcm16[i] / 32768.0f;
        float *enc = NULL, *logits = malloc(32768 * sizeof(float));
        int64_t frames = 0, ids[260];
        size_t nids = hear(s, w, nw, pcm, n, &enc, &frames, logits, ids);
        spill(outdir, fx->name, "s16", pcm16, n * sizeof *pcm16);
        spill(outdir, fx->name, "enc.f32", enc, (size_t)frames * 320 * sizeof(float));
        spill(outdir, fx->name, "logits.f32", logits, 32768 * sizeof(float));
        char text[4096];
        if (stt_detok(&tok, ids, nids, text, sizeof text) < 0) die("detokenize");
        fprintf(m, "%s %zu %lld %zu ", fx->name, n, (long long)frames, nids);
        if (!nids) fputc('-', m);
        for (size_t i = 0; i < nids; i++) fprintf(m, i ? ",%lld" : "%lld", (long long)ids[i]);
        fprintf(m, " %s\n", text);
        printf("%-11s %5.2f s  %4lld frames  %3zu tokens (ceiling %zu)  %s\n", fx->name, (double)n / RATE,
               (long long)frames, nids, max_tokens(n), text);
        free(pcm16);
        free(pcm);
        free(enc);
        free(logits);
    }
    if (fclose(m) != 0) die("cannot write the manifest");
    return 0;
}
