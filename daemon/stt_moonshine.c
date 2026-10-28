/* Moonshine Tiny Streaming through the ONNX Runtime C API (stt_moonshine.h). */
#define _POSIX_C_SOURCE 200809L
#include "stt_moonshine.h"

#include <onnxruntime_c_api.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The frontend graph was exported against whole 80 ms chunks; a chunk that is
 * not a multiple of 80 samples loses its remainder. Moonshine feeds 1280. */
#define CHUNK 1280

enum { S_FRONTEND, S_ENCODER, S_ADAPTER, S_CROSS, S_DECODER, SESSIONS };

typedef struct {
    int encoder_dim, decoder_dim, depth, nheads, head_dim, vocab, bos, eos, d_front, c1;
} ms_config;

typedef struct {
    char *name;
    int64_t shape[4];
    size_t dims;
    float *data;
} split_weight;

struct moonshine {
    const OrtApi *ort;
    OrtEnv *env;
    OrtMemoryInfo *mem;
    OrtSession *s[SESSIONS];
    ms_config cfg;
    moonshine_tokens tok;
    split_weight w[4];
    size_t n_w;
};

static void seterr(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

static int failed(const OrtApi *ort, OrtStatus *st, const char *what, char *err, size_t cap) {
    if (!st) return 0;
    seterr(err, cap, "%s: %s", what, ort->GetErrorMessage(st));
    ort->ReleaseStatus(st);
    return -1;
}

/* One integer from streaming_config.json. The file is pinned by its SHA-256, so
 * this reads the shape of one known file rather than parsing JSON at large. */
static int config_int(const moonshine_blob *b, const char *key, int *out) {
    char pat[48];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    size_t plen = strlen(pat);
    for (size_t i = 0; i + plen < b->len; i++) {
        if (memcmp(b->data + i, pat, plen)) continue;
        size_t j = i + plen;
        while (j < b->len && (b->data[j] == ' ' || b->data[j] == ':' || b->data[j] == '\n')) j++;
        long v = 0;
        int any = 0;
        while (j < b->len && b->data[j] >= '0' && b->data[j] <= '9') v = v * 10 + (b->data[j++] - '0'), any = 1;
        if (!any) return -1;
        *out = (int)v;
        return 0;
    }
    return -1;
}

static OrtSession *open_session(moonshine *m, const moonshine_blob *b, char *err, size_t cap) {
    const OrtApi *ort = m->ort;
    OrtSessionOptions *so = NULL;
    OrtSession *s = NULL;
    if (failed(ort, ort->CreateSessionOptions(&so), "session options", err, cap)) return NULL;
    if (failed(ort, ort->SetIntraOpNumThreads(so, 1), "intra-op threads", err, cap) ||
        failed(ort, ort->SetInterOpNumThreads(so, 1), "inter-op threads", err, cap) ||
        failed(ort, ort->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL), "optimization", err, cap) ||
        failed(ort, ort->AddSessionConfigEntry(so, "session.use_ort_model_bytes_directly", "1"), "config", err, cap) ||
        failed(ort, ort->CreateSessionFromArray(m->env, b->data, b->len, so, &s), "load", err, cap)) {
        ort->ReleaseSessionOptions(so);
        return NULL;
    }
    ort->ReleaseSessionOptions(so);
    return s;
}

/* frontend.weights.ort has no inputs: running it once yields the frontend's
 * float weights, which the frontend then takes as extra inputs on every run.
 * Kept apart so an ONNX Runtime optimizer cannot fold the int8 storage away. */
static int load_split_weights(moonshine *m, const moonshine_blob *b, char *err, size_t cap) {
    const OrtApi *ort = m->ort;
    OrtSession *ws = open_session(m, b, err, cap);
    if (!ws) return -1;
    OrtAllocator *al = NULL;
    size_t n = 0;
    int rc = -1;
    char *names[4] = {0};
    OrtValue *out[4] = {0};
    if (failed(ort, ort->GetAllocatorWithDefaultOptions(&al), "allocator", err, cap) ||
        failed(ort, ort->SessionGetOutputCount(ws, &n), "weights", err, cap))
        goto done;
    if (n == 0 || n > 4) { seterr(err, cap, "frontend weights: %zu outputs", n); goto done; }
    for (size_t i = 0; i < n; i++)
        if (failed(ort, ort->SessionGetOutputName(ws, i, al, &names[i]), "weights", err, cap)) goto done;
    if (failed(ort, ort->Run(ws, NULL, NULL, NULL, 0, (const char *const *)names, n, out), "weights run", err, cap))
        goto done;
    for (size_t i = 0; i < n; i++) {
        OrtTensorTypeAndShapeInfo *info = NULL;
        ONNXTensorElementDataType ty;
        size_t dims = 0, count = 0;
        float *src = NULL;
        if (failed(ort, ort->GetTensorTypeAndShape(out[i], &info), "weights", err, cap)) goto done;
        OrtStatus *st = ort->GetTensorElementType(info, &ty);
        if (!st) st = ort->GetDimensionsCount(info, &dims);
        if (!st && dims <= 4) st = ort->GetDimensions(info, m->w[i].shape, dims);
        if (!st) st = ort->GetTensorShapeElementCount(info, &count);
        ort->ReleaseTensorTypeAndShapeInfo(info);
        if (failed(ort, st, "weights", err, cap)) goto done;
        if (ty != ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || dims > 4) { seterr(err, cap, "frontend weight %zu is not a float tensor", i); goto done; }
        if (failed(ort, ort->GetTensorMutableData(out[i], (void **)&src), "weights", err, cap)) goto done;
        m->w[i].dims = dims;
        m->w[i].name = strdup(names[i]);
        m->w[i].data = malloc(count * sizeof(float));
        if (!m->w[i].name || !m->w[i].data) { seterr(err, cap, "out of memory"); goto done; }
        memcpy(m->w[i].data, src, count * sizeof(float));
        m->n_w = i + 1;
    }
    rc = 0;
done:
    for (size_t i = 0; i < 4; i++) {
        if (out[i]) ort->ReleaseValue(out[i]);
        if (names[i]) al->Free(al, names[i]);
    }
    ort->ReleaseSession(ws);
    return rc;
}

moonshine *moonshine_open(const moonshine_blob files[MOONSHINE_FILES], char *err, size_t errcap) {
    moonshine *m = calloc(1, sizeof *m);
    if (!m) { seterr(err, errcap, "out of memory"); return NULL; }
    m->ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!m->ort) { seterr(err, errcap, "ONNX Runtime API %d unavailable", ORT_API_VERSION); free(m); return NULL; }
    ms_config *c = &m->cfg;
    const moonshine_blob *cf = &files[MOONSHINE_CONFIG];
    if (config_int(cf, "encoder_dim", &c->encoder_dim) || config_int(cf, "decoder_dim", &c->decoder_dim) ||
        config_int(cf, "depth", &c->depth) || config_int(cf, "nheads", &c->nheads) ||
        config_int(cf, "head_dim", &c->head_dim) || config_int(cf, "vocab_size", &c->vocab) ||
        config_int(cf, "bos_id", &c->bos) || config_int(cf, "eos_id", &c->eos) ||
        config_int(cf, "d_model_frontend", &c->d_front) || config_int(cf, "c1", &c->c1)) {
        seterr(err, errcap, "streaming_config.json is not the expected shape");
        free(m);
        return NULL;
    }
    if (moonshine_tokens_load(&m->tok, files[MOONSHINE_TOKENIZER].data, files[MOONSHINE_TOKENIZER].len)) {
        seterr(err, errcap, "tokenizer.bin is malformed");
        free(m);
        return NULL;
    }
    if (failed(m->ort, m->ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "openchimed-stt", &m->env), "env", err, errcap) ||
        failed(m->ort, m->ort->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &m->mem), "memory", err, errcap)) {
        moonshine_close(m);
        return NULL;
    }
    static const int map[SESSIONS] = { MOONSHINE_FRONTEND, MOONSHINE_ENCODER, MOONSHINE_ADAPTER,
                                       MOONSHINE_CROSS_KV, MOONSHINE_DECODER };
    for (int i = 0; i < SESSIONS; i++) {
        char why[256];
        m->s[i] = open_session(m, &files[map[i]], why, sizeof why);
        if (!m->s[i]) {
            seterr(err, errcap, "%s: %s", MOONSHINE_FILE_NAMES[map[i]], why);
            moonshine_close(m);
            return NULL;
        }
    }
    if (load_split_weights(m, &files[MOONSHINE_FRONTEND_WEIGHTS], err, errcap)) {
        moonshine_close(m);
        return NULL;
    }
    return m;
}

void moonshine_close(moonshine *m) {
    if (!m) return;
    for (int i = 0; i < SESSIONS; i++)
        if (m->s[i]) m->ort->ReleaseSession(m->s[i]);
    for (size_t i = 0; i < m->n_w; i++) {
        free(m->w[i].name);
        free(m->w[i].data);
    }
    if (m->mem) m->ort->ReleaseMemoryInfo(m->mem);
    if (m->env) m->ort->ReleaseEnv(m->env);
    moonshine_tokens_free(&m->tok);
    free(m);
}

static OrtValue *tensor(moonshine *m, void *data, size_t bytes, const int64_t *shape, size_t dims,
                        ONNXTensorElementDataType ty, OrtStatus **st) {
    OrtValue *v = NULL;
    if (!*st) *st = m->ort->CreateTensorWithDataAsOrtValue(m->mem, data, bytes, shape, dims, ty, &v);
    return v;
}

static void *data_of(moonshine *m, OrtValue *v) {
    void *p = NULL;
    OrtStatus *st = m->ort->GetTensorMutableData(v, &p);
    if (st) { m->ort->ReleaseStatus(st); return NULL; }
    return p;
}

static int64_t dim1(moonshine *m, OrtValue *v) {
    OrtTensorTypeAndShapeInfo *info = NULL;
    int64_t shape[5] = {0};
    size_t n = 0;
    if (m->ort->GetTensorTypeAndShape(v, &info)) return -1;
    OrtStatus *st = m->ort->GetDimensionsCount(info, &n);
    if (!st && n >= 2 && n <= 5) st = m->ort->GetDimensions(info, shape, n);
    m->ort->ReleaseTensorTypeAndShapeInfo(info);
    if (st) { m->ort->ReleaseStatus(st); return -1; }
    return n >= 2 ? shape[1] : -1;
}

/* The frontend over the whole utterance, in CHUNK-sample pieces with its state
 * carried between them; returns the features (frames x d_front) or NULL. */
static float *run_frontend(moonshine *m, const float *pcm, size_t samples, int64_t *frames_out,
                           char *err, size_t cap) {
    const OrtApi *ort = m->ort;
    const ms_config *c = &m->cfg;
    float sample_buf[79] = {0};
    int64_t sample_len = 0, frame_count = 0;
    float *conv1 = calloc((size_t)c->d_front * 4, sizeof(float));
    float *conv2 = calloc((size_t)c->c1 * 4, sizeof(float));
    float *chunk = malloc(CHUNK * sizeof(float));
    float *feat = NULL;
    size_t nfeat = 0;
    int ok = conv1 && conv2 && chunk;
    size_t chunks = (samples + CHUNK - 1) / CHUNK;
    for (size_t k = 0; ok && k < chunks; k++) {
        size_t off = k * CHUNK, n = samples - off < CHUNK ? samples - off : CHUNK;
        memcpy(chunk, pcm + off, n * sizeof(float));
        if (n < CHUNK) memset(chunk + n, 0, (CHUNK - n) * sizeof(float)); /* pad the tail with silence */
        OrtStatus *st = NULL;
        int64_t s_audio[] = {1, CHUNK}, s_buf[] = {1, 79}, s_one[] = {1};
        int64_t s_c1[] = {1, c->d_front, 4}, s_c2[] = {1, c->c1, 4};
        OrtValue *in[6 + 4] = {0};
        const char *names[6 + 4] = { "audio_chunk", "sample_buffer", "sample_len", "conv1_buffer", "conv2_buffer", "frame_count" };
        in[0] = tensor(m, chunk, CHUNK * sizeof(float), s_audio, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &st);
        in[1] = tensor(m, sample_buf, sizeof sample_buf, s_buf, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &st);
        in[2] = tensor(m, &sample_len, sizeof sample_len, s_one, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &st);
        in[3] = tensor(m, conv1, (size_t)c->d_front * 4 * sizeof(float), s_c1, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &st);
        in[4] = tensor(m, conv2, (size_t)c->c1 * 4 * sizeof(float), s_c2, 3, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &st);
        in[5] = tensor(m, &frame_count, sizeof frame_count, s_one, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &st);
        for (size_t i = 0; i < m->n_w; i++) {
            size_t count = 1;
            for (size_t d = 0; d < m->w[i].dims; d++) count *= (size_t)m->w[i].shape[d];
            in[6 + i] = tensor(m, m->w[i].data, count * sizeof(float), m->w[i].shape, m->w[i].dims,
                               ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &st);
            names[6 + i] = m->w[i].name;
        }
        static const char *const outn[] = { "features", "sample_buffer_out", "sample_len_out",
                                            "conv1_buffer_out", "conv2_buffer_out", "frame_count_out" };
        OrtValue *out[6] = {0};
        if (!st) st = ort->Run(m->s[S_FRONTEND], NULL, names, (const OrtValue *const *)in, 6 + m->n_w, outn, 6, out);
        for (size_t i = 0; i < 6 + m->n_w; i++) if (in[i]) ort->ReleaseValue(in[i]);
        if (failed(ort, st, "frontend", err, cap)) { ok = 0; break; }
        int64_t f = dim1(m, out[0]);
        float *p = NULL;
        if (f < 0) { seterr(err, cap, "frontend: bad output shape"); ok = 0; }
        if (ok && f > 0) {
            float *grown = realloc(feat, (nfeat + (size_t)f) * (size_t)c->d_front * sizeof(float));
            if (!grown) { seterr(err, cap, "out of memory"); ok = 0; }
            else {
                feat = grown;
                p = data_of(m, out[0]);
                if (p) memcpy(feat + nfeat * (size_t)c->d_front, p, (size_t)f * (size_t)c->d_front * sizeof(float));
                nfeat += (size_t)f;
            }
        }
        if (ok) {
            float *b1 = data_of(m, out[1]), *b3 = data_of(m, out[3]), *b4 = data_of(m, out[4]);
            int64_t *q2 = data_of(m, out[2]), *q5 = data_of(m, out[5]);
            if (!b1 || !b3 || !b4 || !q2 || !q5 || (f > 0 && !p)) { seterr(err, cap, "frontend: no output data"); ok = 0; }
            else {
                memcpy(sample_buf, b1, sizeof sample_buf);
                sample_len = *q2;
                memcpy(conv1, b3, (size_t)c->d_front * 4 * sizeof(float));
                memcpy(conv2, b4, (size_t)c->c1 * 4 * sizeof(float));
                frame_count = *q5;
            }
        }
        for (int i = 0; i < 6; i++) if (out[i]) ort->ReleaseValue(out[i]);
    }
    free(conv1);
    free(conv2);
    free(chunk);
    if (!ok) { free(feat); return NULL; }
    *frames_out = (int64_t)nfeat;
    return feat;
}

/* Run one single-input, single-output graph. */
static OrtValue *run1(moonshine *m, int s, const char *in_name, OrtValue *in, const char *out_name,
                      char *err, size_t cap) {
    OrtValue *out = NULL;
    if (failed(m->ort, m->ort->Run(m->s[s], NULL, &in_name, (const OrtValue *const *)&in, 1, &out_name, 1, &out),
               out_name, err, cap))
        return NULL;
    return out;
}

int moonshine_hear(moonshine *m, const float *pcm, size_t samples, char **text, char *err, size_t errcap) {
    const OrtApi *ort = m->ort;
    const ms_config *c = &m->cfg;
    *text = NULL;
    int64_t frames = 0;
    float *feat = samples ? run_frontend(m, pcm, samples, &frames, err, errcap) : NULL;
    if (samples && !feat) return -1;
    if (frames <= 0) {
        free(feat);
        *text = strdup("");
        return *text ? 0 : -1;
    }

    int rc = -1;
    OrtStatus *st = NULL;
    OrtValue *v_feat = NULL, *encoded = NULL, *v_pos = NULL, *memory = NULL;
    OrtValue *kv[2] = {0}, *self_k = NULL, *self_v = NULL;
    int64_t *ids = NULL;
    size_t nids = 0;
    int64_t s_feat[] = {1, frames, c->encoder_dim}, s_one[] = {1}, pos = 0;

    v_feat = tensor(m, feat, (size_t)frames * (size_t)c->encoder_dim * sizeof(float), s_feat, 3,
                    ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &st);
    if (failed(ort, st, "features", err, errcap)) goto done;
    if (!(encoded = run1(m, S_ENCODER, "features", v_feat, "encoded", err, errcap))) goto done;

    v_pos = tensor(m, &pos, sizeof pos, s_one, 1, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &st);
    if (failed(ort, st, "position", err, errcap)) goto done;
    {
        const char *in_n[] = { "encoded", "pos_offset" }, *out_n[] = { "memory" };
        OrtValue *in[] = { encoded, v_pos };
        if (failed(ort, ort->Run(m->s[S_ADAPTER], NULL, in_n, (const OrtValue *const *)in, 2, out_n, 1, &memory),
                   "adapter", err, errcap))
            goto done;
    }
    {
        const char *in_n[] = { "memory" }, *out_n[] = { "k_cross", "v_cross" };
        if (failed(ort, ort->Run(m->s[S_CROSS], NULL, in_n, (const OrtValue *const *)&memory, 1, out_n, 2, kv),
                   "cross_kv", err, errcap))
            goto done;
    }

    /* Greedy decoding from the start token. The self-attention cache starts
     * empty and each step's output cache is the next step's input. */
    size_t max_tokens = moonshine_max_tokens(samples);
    ids = malloc((max_tokens + 2) * sizeof *ids);
    float empty = 0;
    int64_t s_empty[] = {c->depth, 1, c->nheads, 0, c->head_dim};
    self_k = tensor(m, &empty, 0, s_empty, 5, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &st);
    self_v = tensor(m, &empty, 0, s_empty, 5, ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &st);
    if (!ids) { seterr(err, errcap, "out of memory"); goto done; }
    if (failed(ort, st, "cache", err, errcap)) goto done;
    int64_t token = c->bos;
    for (size_t step = 0; step <= max_tokens; step++) {
        int64_t s_tok[] = {1, 1};
        OrtValue *v_tok = tensor(m, &token, sizeof token, s_tok, 2, ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &st);
        if (failed(ort, st, "token", err, errcap)) goto done;
        const char *in_n[] = { "token", "k_self", "v_self", "out_k_cross", "out_v_cross" };
        const char *out_n[] = { "logits", "out_k_self", "out_v_self" };
        OrtValue *in[] = { v_tok, self_k, self_v, kv[0], kv[1] };
        OrtValue *out[3] = {0};
        st = ort->Run(m->s[S_DECODER], NULL, in_n, (const OrtValue *const *)in, 5, out_n, 3, out);
        ort->ReleaseValue(v_tok);
        if (failed(ort, st, "decoder", err, errcap)) goto done;
        ort->ReleaseValue(self_k);
        ort->ReleaseValue(self_v);
        self_k = out[1];
        self_v = out[2];
        float *logits = data_of(m, out[0]);
        if (!logits) { ort->ReleaseValue(out[0]); seterr(err, errcap, "decoder: no logits"); goto done; }
        int best = 0;
        for (int i = 1; i < c->vocab; i++)
            if (logits[i] > logits[best]) best = i;
        ort->ReleaseValue(out[0]);
        if (best == c->eos || step == max_tokens) break;
        ids[nids++] = best;
        token = best;
    }

    size_t cap = nids * 64 + 1;
    char *out = malloc(cap);
    if (!out) { seterr(err, errcap, "out of memory"); goto done; }
    if (moonshine_detok(&m->tok, ids, nids, out, cap) < 0) { free(out); seterr(err, errcap, "detokenize"); goto done; }
    *text = out;
    rc = 0;
done: {
    /* A block of its own: C99 lets a label only precede a statement, and a
     * declaration here is a C23 extension the release's compiler refuses. */
    OrtValue *all[] = { v_feat, encoded, v_pos, memory, kv[0], kv[1], self_k, self_v };
    for (size_t i = 0; i < sizeof all / sizeof all[0]; i++) if (all[i]) ort->ReleaseValue(all[i]);
    }
    free(feat);
    free(ids);
    return rc;
}
