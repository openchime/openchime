/* MANUAL: renders the read-aloud voice model's reference outputs, the fixtures in
 * tests/data/voice/ that ttskit's own rendering of the model is compared against.
 *
 * Deliberately NOT part of any suite or Makefile target: it runs the upstream .onnx
 * model with the full prebuilt ONNX Runtime, which nothing in the build links for
 * this purpose. Its outputs are committed; rerun it only to remake them, and then
 * commit what it writes.
 *
 * Build and run from the repository folder, x64 only, with the upstream model
 * file, its voices.npz and ONNX Runtime 1.30.0's prebuilt release fetched from
 * the archive and checked (docs/VENDORS.md, "The archive"):
 *
 *     scripts/build/speech_sources.sh fetch build/speech-src
 *     PRE=build/speech-src/onnxruntime-linux-x64-1.30.0
 *     cc -std=c99 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -I"$PRE/include" -Ittskit -Innkit -Idaemon \
 *        tests/manual/voice_ref.c daemon/tts_voice_tokens.c \
 *        $(find ttskit nnkit -maxdepth 1 -name '*.c' ! -name tts_pack.c) \
 *        -L"$PRE/lib" -lonnxruntime -Wl,-rpath,"$PWD/$PRE/lib" -lm -o build/voice_ref
 *     build/voice_ref build/speech-src/voice-src/<the model file> build/speech-src/voice-src/voices.npz \
 *        ttskit/data/en-US tests/data/voice
 * (or with --dump first, below).
 *
 * For each fixture (a sentence and a voice) it does what the daemon does to
 * reach the model -- ttskit's tts_text for the IPA, the daemon's token function
 * for the ids, the style row min(chars, 399) of the voice's 400 x 256 table with
 * chars counted as UTF-8 characters, the speed 1.2 -- then runs the model twice,
 * A and B, in the same session (one thread, every graph optimization on, as the
 * daemon configures it). The model draws random numbers (RandomNormalLike,
 * RandomUniformLike), so the two runs differ; the SNR of B against A is the
 * run-to-run noise floor a different implementation is held to. It prints the
 * sample counts, the SNR, the render times and the real-time factor of each run,
 * and the process's peak RSS, and fails loudly if A and B differ in length.
 *
 * It also renders each fixture with the noise off, the reference a different
 * implementation is compared against. The model's sine source draws two kinds of
 * random numbers: RandomUniformLike (low 0, high 1), one initial phase per harmonic
 * (the fundamental's is then set to 0), and RandomNormalLike (mean 0, scale 1),
 * one per harmonic and sample, multiplied by 0.003 where the frame is voiced and
 * 0.1 / 3 where it is not, before it is added to the sine waves. The program
 * copies the model in memory and replaces each of those two nodes with
 * ConstantOfShape(Shape(input)) of the node's dtype and the value 0, so both
 * produce zeros of the same shape and type; the copy is loaded from memory and
 * never written. It renders each fixture twice, Q1 and Q2, in that session,
 * and fails loudly if the two are not bit-identical.
 *
 * Files written to the output folder, per fixture <name>:
 *   <name>.ids   the model's input_ids, int64 little-endian, one per token
 *   <name>.f32   run A's RAW "waveform" output (before the daemon's high pass,
 *                tail trim and fade), float32 little-endian, 24,000 per second
 *   <name>.quiet.f32  the noise-off run Q1's RAW "waveform" output, the same
 *                format
 * and one `manifest`, plain text:
 *   - lines starting with '#' are comments; every other line is one fixture;
 *   - a fixture line is ten fields separated by single spaces, then the text:
 *       <name> <voice> <row> <speed> <tokens> <ids-file> <samples> <f32-file>
 *       <samples-b> <snr-db> <text...>
 *     name      the fixture's name, [a-z0-9-]
 *     voice     the voice id ("expr-voice-2-m" ...)
 *     row       the style row used, an integer in 0..399
 *     speed     the speed input, a decimal ("1.2"; strtof gives the float the
 *               daemon passes)
 *     tokens    the number of int64 ids in <ids-file>
 *     samples   the number of float32 samples in <f32-file> (run A)
 *     samples-b the number run B produced (equal to samples, or this program
 *               failed)
 *     snr-db    10 log10(sum A^2 / sum (B - A)^2), with two decimals
 *     text      the sentence as given to tts_text, to the end of the line
 *   so `sscanf(line, "%63s %31s %d %f %d %63s %ld %63s %ld %lf %n", ...)` reads
 *   the fields and the text starts at the %n offset;
 *   - a line starting with "#quiet " is the noise-off reference of the fixture
 *     line before it, four fields after the keyword separated by single spaces:
 *       #quiet <name> <quiet-file> <samples-quiet> <snr-a-db>
 *     name          the fixture's name, as on its fixture line
 *     quiet-file    the noise-off output, <name>.quiet.f32
 *     samples-quiet the number of float32 samples in <quiet-file>
 *     snr-a-db      10 log10(sum Q^2 / sum (A - Q)^2), with two decimals: how
 *                   far the noise moves run A from the noise-off output Q
 *     `sscanf(line, "#quiet %63s %63s %ld %lf", ...)` reads it; a reader that
 *     does not want it skips it with the other comments, so the fixture lines
 *     read as before.
 *   File names are relative to the manifest's folder.
 *
 * With --dump it writes no fixtures. It renders every fixture once with the
 * noise off, from a copy of the noise-off model that also has the named
 * intermediate tensors as graph outputs, and writes them to a scratch folder
 * (outside the tree: dumps are for comparing a forward pass by hand, never
 * committed):
 *
 *     build/voice_ref --dump <scratch-dir> /bert_encoder/Add_output_0,duration:i64 \
 *        <model.onnx> <voices.npz> ttskit/data/en-US
 *
 * Each name is a tensor of the graph; ":i64" marks one of int64 elements (float32
 * otherwise). Per fixture <name> and tensor number k (from 0, in the order given)
 * it writes <name>.<k>.bin, the tensor's elements little-endian, and one line of
 * <name>.txt: "<k> <tensor> <f32|i64> <rank> <dims...>". ONNX Runtime optimises
 * the copy as it does the model, except that a tensor made an output can no
 * longer be fused away; so the program also renders the noise-off model without
 * the extra outputs and says whether the two waveforms are bit-identical. Name
 * tensors that the optimised graph keeps (a fused node's output), or that line
 * says "DIFFERENT". */
#include "ttskit.h"
#include "tts_voice.h"

#include <onnxruntime_c_api.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#define STYLE_ROWS 400
#define STYLE_DIM  256

/* Short and long, punctuation and numbers, a man's and a woman's voice in turn.
 * Each sentence is rendered in one voice so the committed audio stays small. */
static const struct { const char *name, *voice, *text; } FIXTURES[] = {
    { "short-m",       "expr-voice-2-m", "Yes." },
    { "greeting-f",    "expr-voice-2-f", "Hello, how are you today?" },
    { "numbers-m",     "expr-voice-2-m", "It moved to 3:15 on the 2nd, and costs $40." },
    { "punctuation-f", "expr-voice-2-f", "Wait... really? Fine; let's talk!" },
    { "long-m",        "expr-voice-2-m", "Before the train left, she checked her ticket twice and sat down "
                                         "by the window." },
    { "figures-f",     "expr-voice-2-f", "We sold 2,500 on the 21st." },
};
#define NFIX (sizeof FIXTURES / sizeof FIXTURES[0])

static const OrtApi *ort;

static void check(OrtStatus *st, const char *what) {
    if (!st) return;
    fprintf(stderr, "voice_ref: %s: %s\n", what, ort->GetErrorMessage(st));
    exit(1);
}

static void die(const char *fmt, const char *arg) {
    fprintf(stderr, "voice_ref: ");
    fprintf(stderr, fmt, arg);
    fprintf(stderr, "\n");
    exit(1);
}

static uint8_t *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot open %s", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(n > 0 ? (size_t)n : 1);
    if (!p || fread(p, 1, (size_t)n, f) != (size_t)n) die("cannot read %s", path);
    fclose(f);
    *len = (size_t)n;
    return p;
}

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }

/* One voice's 400 x 256 float32 table in voices.npz (a stored zip of .npy files,
 * zip64 sizes as numpy writes them), copied out. */
static void voice_table(const uint8_t *npz, size_t len, const char *voice, float *out) {
    const uint8_t *p = npz, *end = npz + len;
    char want[64];
    snprintf(want, sizeof want, "%s.npy", voice);
    while (p + 30 <= end && le32(p) == 0x04034b50) {
        uint16_t method = le16(p + 8), name_len = le16(p + 26), extra_len = le16(p + 28);
        uint64_t size = le32(p + 18);
        const uint8_t *name = p + 30, *extra = name + name_len, *data = extra + extra_len;
        if (data > end) break;
        for (const uint8_t *e = extra; size == 0xFFFFFFFFu && e + 4 <= data;) {
            uint16_t id = le16(e), l = le16(e + 2);
            if (id == 1 && l >= 16 && e + 20 <= data) size = le32(e + 12) | (uint64_t)le32(e + 16) << 32;
            e += 4 + l;
        }
        if ((uint64_t)(end - data) < size) break;
        if (name_len == strlen(want) && !memcmp(name, want, name_len)) {
            if (method != 0 || size < 10 || memcmp(data, "\x93NUMPY", 6)) die("voices.npz: %s is not a stored .npy", voice);
            size_t hl = data[6] == 1 ? 10 + (size_t)le16(data + 8) : 12 + (size_t)le32(data + 8);
            char hdr[256];
            size_t hs = data[6] == 1 ? 10 : 12;
            snprintf(hdr, sizeof hdr, "%.*s", (int)(hl - hs < sizeof hdr ? hl - hs : sizeof hdr - 1), (const char *)data + hs);
            if (hl > size || !strstr(hdr, "'descr': '<f4'") || !strstr(hdr, "'fortran_order': False") ||
                !strstr(hdr, "'shape': (400, 256)") || size - hl != (size_t)STYLE_ROWS * STYLE_DIM * 4)
                die("voices.npz: %s is not a 400 x 256 float32 table", voice);
            for (size_t i = 0; i < (size_t)STYLE_ROWS * STYLE_DIM; i++) {
                uint32_t u = le32(data + hl + 4 * i);
                memcpy(&out[i], &u, 4);
            }
            return;
        }
        p = data + size;
    }
    die("voices.npz: no voice %s", voice);
}

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* One run of the model; returns a malloc'd copy of "waveform". */
static float *run(OrtSession *s, OrtMemoryInfo *mem, int64_t *ids, size_t n, float *style, float speed,
                  size_t *count) {
    int64_t ids_shape[2] = { 1, (int64_t)n }, style_shape[2] = { 1, STYLE_DIM }, speed_shape[1] = { 1 };
    OrtValue *in[3] = { NULL, NULL, NULL }, *out = NULL;
    check(ort->CreateTensorWithDataAsOrtValue(mem, ids, n * sizeof *ids, ids_shape, 2,
          ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[0]), "input_ids");
    check(ort->CreateTensorWithDataAsOrtValue(mem, style, STYLE_DIM * sizeof *style, style_shape, 2,
          ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[1]), "style");
    check(ort->CreateTensorWithDataAsOrtValue(mem, &speed, sizeof speed, speed_shape, 1,
          ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[2]), "speed");
    static const char *IN[3] = { "input_ids", "style", "speed" }, *OUT[1] = { "waveform" };
    check(ort->Run(s, NULL, IN, (const OrtValue *const *)in, 3, OUT, 1, &out), "Run");
    OrtTensorTypeAndShapeInfo *info;
    float *data;
    check(ort->GetTensorTypeAndShape(out, &info), "GetTensorTypeAndShape");
    check(ort->GetTensorShapeElementCount(info, count), "GetTensorShapeElementCount");
    ort->ReleaseTensorTypeAndShapeInfo(info);
    check(ort->GetTensorMutableData(out, (void **)&data), "GetTensorMutableData");
    float *copy = malloc(*count * sizeof *copy + 1);
    if (!copy) die("%s", "out of memory");
    memcpy(copy, data, *count * sizeof *copy);
    for (int i = 0; i < 3; i++) ort->ReleaseValue(in[i]);
    ort->ReleaseValue(out);
    return copy;
}

/* Written little-endian whatever the host. */
static void write_le(const char *dir, const char *name, const char *ext, const void *v, size_t n, size_t width) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s.%s", dir, name, ext);
    FILE *f = fopen(path, "wb");
    if (!f) die("cannot write %s", path);
    const uint8_t *p = v;
    uint16_t probe = 1;
    int little = *(const uint8_t *)&probe == 1;
    for (size_t i = 0; i < n; i++) {
        uint8_t b[8];
        for (size_t j = 0; j < width; j++) b[j] = p[i * width + (little ? j : width - 1 - j)];
        if (fwrite(b, 1, width, f) != width) die("cannot write %s", path);
    }
    if (fclose(f) != 0) die("cannot write %s", path);
}

/* The noise-off copy of the model: a minimal protobuf (wire format) rewrite of
 * ModelProto.graph (field 7), GraphProto.node (1), NodeProto input (1), output
 * (2), name (3), op_type (4), attribute (5); AttributeProto name (1), i (3),
 * t (5), type (20); TensorProto dims (1), data_type (2), raw_data (9). */
typedef struct { uint8_t *p; size_t n, cap; } pbuf;

static void put(pbuf *b, const void *v, size_t n) {
    if (b->n + n > b->cap) {
        b->cap = (b->n + n) * 2 + 64;
        b->p = realloc(b->p, b->cap);
        if (!b->p) die("%s", "out of memory");
    }
    memcpy(b->p + b->n, v, n);
    b->n += n;
}

static void put_varint(pbuf *b, uint64_t v) {
    uint8_t c[10];
    size_t n = 0;
    do { c[n++] = (uint8_t)((v & 0x7F) | (v > 0x7F ? 0x80 : 0)); v >>= 7; } while (v);
    put(b, c, n);
}

static void put_field(pbuf *b, unsigned field, const void *v, size_t n) {
    put_varint(b, (uint64_t)field << 3 | 2);
    put_varint(b, n);
    put(b, v, n);
}

static void put_str(pbuf *b, unsigned field, const char *s) { put_field(b, field, s, strlen(s)); }

/* The next field of [*p, end): its number, wire type and, for wire type 2, its
 * bytes; for 0, its value in *v. 0 at the end, dies on anything malformed. */
static int next_field(const uint8_t **p, const uint8_t *end, unsigned *field, unsigned *wire,
                      const uint8_t **data, size_t *len, uint64_t *v) {
    if (*p >= end) return 0;
    uint64_t key = 0, x = 0;
    for (int pass = 0; pass < 2; pass++) {
        uint64_t r = 0;
        for (int sh = 0;; sh += 7) {
            if (*p >= end || sh > 63) die("%s", "model: malformed protobuf");
            uint8_t c = *(*p)++;
            r |= (uint64_t)(c & 0x7F) << sh;
            if (!(c & 0x80)) break;
        }
        if (pass == 0) {
            key = r;
            if ((key & 7) != 0 && (key & 7) != 2) break;
        } else x = r;
    }
    *field = (unsigned)(key >> 3);
    *wire = (unsigned)(key & 7);
    *data = *p;
    *len = 0;
    *v = x;
    switch (*wire) {
    case 0: break;
    case 1: *len = 8; break;
    case 2: *len = (size_t)x; break;
    case 5: *len = 4; break;
    default: die("%s", "model: unsupported protobuf wire type");
    }
    if ((size_t)(end - *p) < *len) die("%s", "model: truncated protobuf");
    *p += *len;
    return 1;
}

static int bytes_are(const uint8_t *d, size_t n, const char *s) { return n == strlen(s) && !memcmp(d, s, n); }

/* One NodeProto: copied as is, or, for RandomNormalLike and RandomUniformLike,
 * replaced by Shape(input) then ConstantOfShape of the node's dtype and value 0. */
static void rewrite_node(pbuf *g, const uint8_t *n, size_t len, int *replaced) {
    const uint8_t *p = n, *end = n + len, *d;
    size_t l;
    uint64_t v;
    unsigned field, wire;
    char in[512] = "", out[512] = "", name[512] = "", op[64] = "";
    int nin = 0, nout = 0;
    long long dtype = -1;
    while (next_field(&p, end, &field, &wire, &d, &l, &v)) {
        char *dst = field == 1 ? in : field == 2 ? out : field == 3 ? name : field == 4 ? op : NULL;
        if (dst && wire == 2) {
            size_t cap = field == 4 ? sizeof op : sizeof in;
            snprintf(dst, cap, "%.*s", (int)(l < cap ? l : cap - 1), (const char *)d);
            nin += field == 1;
            nout += field == 2;
        }
        if (field == 5 && wire == 2) {
            const uint8_t *ap = d, *ae = d + l, *ad;
            size_t al;
            uint64_t av;
            unsigned af, aw;
            int is_dtype = 0;
            long long iv = -1;
            while (next_field(&ap, ae, &af, &aw, &ad, &al, &av)) {
                if (af == 1 && aw == 2 && bytes_are(ad, al, "dtype")) is_dtype = 1;
                if (af == 3 && aw == 0) iv = (long long)av;
            }
            if (is_dtype) dtype = iv;
        }
    }
    if (strcmp(op, "RandomNormalLike") != 0 && strcmp(op, "RandomUniformLike") != 0) {
        put_field(g, 1, n, len);
        return;
    }
    if (!strstr(name, "/decoder/generator/m_source/l_sin_gen/")) die("model: unexpected random node %s", name);
    if (nin != 1 || nout != 1) die("model: %s does not have one input and one output", name);
    size_t width = dtype == 1 ? 4 : dtype == 10 ? 2 : dtype == 11 ? 8 : 0;
    if (!width) die("model: %s has no float dtype attribute", name);

    char mid[600];
    snprintf(mid, sizeof mid, "%s_zero_shape", out);
    pbuf node = { 0 }, attr = { 0 }, tensor = { 0 };
    char nm[600];

    snprintf(nm, sizeof nm, "%s_zero_shape", name);
    put_str(&node, 1, in);
    put_str(&node, 2, mid);
    put_str(&node, 3, nm);
    put_str(&node, 4, "Shape");
    put_field(g, 1, node.p, node.n);
    node.n = 0;

    static const uint8_t zero[8];
    put_varint(&tensor, 1 << 3 | 0);         /* dims: [1] */
    put_varint(&tensor, 1);
    put_varint(&tensor, 2 << 3 | 0);         /* data_type */
    put_varint(&tensor, (uint64_t)dtype);
    put_field(&tensor, 9, zero, width);      /* raw_data: one zero */
    put_str(&attr, 1, "value");
    put_field(&attr, 5, tensor.p, tensor.n);
    put_varint(&attr, 20 << 3 | 0);          /* type: TENSOR */
    put_varint(&attr, 4);
    snprintf(nm, sizeof nm, "%s_zeros", name);
    put_str(&node, 1, mid);
    put_str(&node, 2, out);
    put_str(&node, 3, nm);
    put_str(&node, 4, "ConstantOfShape");
    put_field(&node, 5, attr.p, attr.n);
    put_field(g, 1, node.p, node.n);
    free(node.p);
    free(attr.p);
    free(tensor.p);
    printf("noise off: %s (%s) -> ConstantOfShape(Shape(%s)), dtype %lld, value 0\n", name, op, in, dtype);
    (*replaced)++;
}

/* An extra graph output: a ValueInfoProto (name 1, type 2) whose TypeProto
 * (tensor_type 1) gives only the element type (elem_type 1), no shape. */
static void put_output(pbuf *g, const char *name, int elem) {
    pbuf vi = { 0 }, type = { 0 }, tensor = { 0 };
    put_varint(&tensor, 1 << 3 | 0);
    put_varint(&tensor, (uint64_t)elem);
    put_field(&type, 1, tensor.p, tensor.n);
    put_str(&vi, 1, name);
    put_field(&vi, 2, type.p, type.n);
    put_field(g, 12, vi.p, vi.n);
    free(vi.p);
    free(type.p);
    free(tensor.p);
}

/* The model with both random operators producing zeros, as a malloc'd buffer,
 * and the n tensors `extra` (element types `elem`) added as graph outputs. */
static uint8_t *quiet_model(const uint8_t *m, size_t len, size_t *out_len, char **extra, const int *elem, int n) {
    pbuf model = { 0 }, graph = { 0 };
    const uint8_t *p = m, *end = m + len, *d;
    size_t l;
    uint64_t v;
    unsigned field, wire;
    int graphs = 0, replaced = 0;
    while (next_field(&p, end, &field, &wire, &d, &l, &v)) {
        if (field != 7 || wire != 2) {
            put_varint(&model, (uint64_t)field << 3 | wire);
            if (wire == 0) put_varint(&model, v);
            else if (wire == 2) put_varint(&model, l), put(&model, d, l);
            else put(&model, d, l);
            continue;
        }
        graphs++;
        const uint8_t *gp = d, *ge = d + l, *gd;
        size_t gl;
        uint64_t gv;
        unsigned gf, gw;
        while (next_field(&gp, ge, &gf, &gw, &gd, &gl, &gv)) {
            if (gf == 1 && gw == 2) { rewrite_node(&graph, gd, gl, &replaced); continue; }
            put_varint(&graph, (uint64_t)gf << 3 | gw);
            if (gw == 0) put_varint(&graph, gv);
            else if (gw == 2) put_varint(&graph, gl), put(&graph, gd, gl);
            else put(&graph, gd, gl);
        }
        for (int i = 0; i < n; i++) put_output(&graph, extra[i], elem[i]);
        put_field(&model, 7, graph.p, graph.n);
    }
    free(graph.p);
    if (graphs != 1 || replaced != 2) {
        fprintf(stderr, "voice_ref: model: %d graphs and %d random nodes; expected 1 and 2\n", graphs, replaced);
        exit(1);
    }
    *out_len = model.n;
    return model.p;
}

/* Fixture f's model inputs as the daemon makes them: its ids (returns how
 * many), its style row, and the character count and row number. */
static size_t fixture_inputs(tts *t, const uint8_t *npz, size_t npz_len, size_t f, int64_t *ids, float *style,
                             size_t *chars, size_t *row) {
    static float table[STYLE_ROWS * STYLE_DIM];
    static char ipa[16384];
    const char *text = FIXTURES[f].text;
    if (!tts_text(t, text, ipa, sizeof ipa)) die("nothing to say in \"%s\"", text);
    long long ll[TTS_VOICE_MAX_TOKENS];
    size_t n = tts_voice_tokens(ipa, ll, TTS_VOICE_MAX_TOKENS);
    for (size_t i = 0; i < n; i++) ids[i] = ll[i];
    *chars = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p; p++)
        if ((*p & 0xC0) != 0x80) (*chars)++;
    *row = *chars < STYLE_ROWS - 1 ? *chars : STYLE_ROWS - 1;
    voice_table(npz, npz_len, FIXTURES[f].voice, table);
    memcpy(style, table + *row * STYLE_DIM, sizeof(float) * STYLE_DIM);
    return n;
}

/* --dump: every fixture's chosen intermediate tensors, noise off (see the top). */
static int dump_main(int argc, char **argv) {
    if (argc != 7) {
        fprintf(stderr, "usage: voice_ref --dump SCRATCH-DIR NAME[:i64],... MODEL.onnx VOICES.npz LEXICON-DIR\n");
        return 2;
    }
    const char *outdir = argv[2];
    enum { MAXOUT = 64 };
    char *names[MAXOUT], *list = strdup(argv[3]);
    int elem[MAXOUT], n_out = 0;
    for (char *tok = strtok(list, ","); tok; tok = strtok(NULL, ",")) {
        if (n_out == MAXOUT) die("%s", "too many tensors");
        char *colon = strrchr(tok, ':');
        elem[n_out] = colon && !strcmp(colon, ":i64") ? 7 : 1;
        if (colon && !strcmp(colon, ":i64")) *colon = '\0';
        names[n_out++] = tok;
    }
    char err[256] = "";
    tts *t = tts_load(argv[6], "en-US", err, sizeof err);
    if (!t) die("pronunciation data: %s", err);
    size_t npz_len, model_len, plain_len, extra_len;
    uint8_t *npz = slurp(argv[5], &npz_len), *model = slurp(argv[4], &model_len);
    uint8_t *plain = quiet_model(model, model_len, &plain_len, NULL, NULL, 0);
    uint8_t *extra = quiet_model(model, model_len, &extra_len, names, elem, n_out);
    free(model);

    ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!ort) die("%s", "ONNX Runtime does not provide this API version");
    OrtEnv *env;
    OrtSessionOptions *so;
    OrtSession *sp, *se;
    OrtMemoryInfo *mem;
    check(ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "voice_ref", &env), "CreateEnv");
    check(ort->CreateSessionOptions(&so), "CreateSessionOptions");
    check(ort->SetIntraOpNumThreads(so, 1), "SetIntraOpNumThreads");
    check(ort->SetInterOpNumThreads(so, 1), "SetInterOpNumThreads");
    check(ort->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL), "SetSessionGraphOptimizationLevel");
    check(ort->CreateSessionFromArray(env, plain, plain_len, so, &sp), "CreateSessionFromArray (noise off)");
    check(ort->CreateSessionFromArray(env, extra, extra_len, so, &se), "CreateSessionFromArray (with outputs)");
    free(plain);
    free(extra);
    check(ort->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &mem), "CreateCpuMemoryInfo");

    int differ_any = 0;
    for (size_t f = 0; f < NFIX; f++) {
        int64_t ids[TTS_VOICE_MAX_TOKENS];
        float style[STYLE_DIM], speed = TTS_VOICE_SPEED;
        size_t chars, row, n = fixture_inputs(t, npz, npz_len, f, ids, style, &chars, &row);
        size_t np;
        float *ref = run(sp, mem, ids, n, style, speed, &np);

        int64_t ids_shape[2] = { 1, (int64_t)n }, style_shape[2] = { 1, STYLE_DIM }, speed_shape[1] = { 1 };
        OrtValue *in[3] = { NULL, NULL, NULL }, *out[MAXOUT + 1] = { NULL };
        check(ort->CreateTensorWithDataAsOrtValue(mem, ids, n * sizeof *ids, ids_shape, 2,
              ONNX_TENSOR_ELEMENT_DATA_TYPE_INT64, &in[0]), "input_ids");
        check(ort->CreateTensorWithDataAsOrtValue(mem, style, sizeof style, style_shape, 2,
              ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[1]), "style");
        check(ort->CreateTensorWithDataAsOrtValue(mem, &speed, sizeof speed, speed_shape, 1,
              ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &in[2]), "speed");
        static const char *IN[3] = { "input_ids", "style", "speed" };
        const char *outn[MAXOUT + 1];
        outn[0] = "waveform";
        for (int i = 0; i < n_out; i++) outn[i + 1] = names[i];
        check(ort->Run(se, NULL, IN, (const OrtValue *const *)in, 3, outn, (size_t)n_out + 1, out), "Run");

        char path[512];
        snprintf(path, sizeof path, "%s/%s.txt", outdir, FIXTURES[f].name);
        FILE *idx = fopen(path, "w");
        if (!idx) die("cannot write %s", path);
        size_t nw = 0;
        float *wave = NULL;
        for (int i = 0; i <= n_out; i++) {
            OrtTensorTypeAndShapeInfo *info;
            size_t count, rank;
            int64_t dims[8];
            void *data;
            check(ort->GetTensorTypeAndShape(out[i], &info), "GetTensorTypeAndShape");
            check(ort->GetTensorShapeElementCount(info, &count), "GetTensorShapeElementCount");
            check(ort->GetDimensionsCount(info, &rank), "GetDimensionsCount");
            if (rank > 8) die("%s", "a tensor of rank above 8");
            check(ort->GetDimensions(info, dims, rank), "GetDimensions");
            ort->ReleaseTensorTypeAndShapeInfo(info);
            check(ort->GetTensorMutableData(out[i], &data), "GetTensorMutableData");
            if (i == 0) {
                nw = count;
                wave = data;
                continue;
            }
            char ext[32];
            snprintf(ext, sizeof ext, "%d.bin", i - 1);
            write_le(outdir, FIXTURES[f].name, ext, data, count, elem[i - 1] == 7 ? 8 : 4);
            fprintf(idx, "%d %s %s %zu", i - 1, names[i - 1], elem[i - 1] == 7 ? "i64" : "f32", rank);
            for (size_t d = 0; d < rank; d++) fprintf(idx, " %lld", (long long)dims[d]);
            fprintf(idx, "\n");
        }
        if (fclose(idx) != 0) die("cannot write %s", path);
        int same = nw == np && !memcmp(wave, ref, np * sizeof *ref);
        differ_any |= !same;
        printf("%-14s tokens %3zu samples %7zu frames %5zu  waveform with the extra outputs: %s\n",
               FIXTURES[f].name, n, nw, nw / 600, same ? "bit-identical" : "DIFFERENT");
        for (int i = 0; i <= n_out; i++) ort->ReleaseValue(out[i]);
        for (int i = 0; i < 3; i++) ort->ReleaseValue(in[i]);
        free(ref);
    }
    ort->ReleaseMemoryInfo(mem);
    ort->ReleaseSession(se);
    ort->ReleaseSession(sp);
    ort->ReleaseSessionOptions(so);
    ort->ReleaseEnv(env);
    tts_free(t);
    free(npz);
    free(list);
    return differ_any;
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "--dump")) return dump_main(argc, argv);
    if (argc != 5) {
        fprintf(stderr, "usage: voice_ref MODEL.onnx VOICES.npz LEXICON-DIR OUT-DIR\n");
        return 2;
    }
    const char *outdir = argv[4];
    char err[256] = "";
    tts *t = tts_load(argv[3], "en-US", err, sizeof err);
    if (!t) die("pronunciation data: %s", err);
    size_t npz_len;
    uint8_t *npz = slurp(argv[2], &npz_len);

    ort = OrtGetApiBase()->GetApi(ORT_API_VERSION);
    if (!ort) die("%s", "ONNX Runtime does not provide this API version");
    OrtEnv *env;
    OrtSessionOptions *so;
    OrtSession *s;
    OrtMemoryInfo *mem;
    check(ort->CreateEnv(ORT_LOGGING_LEVEL_WARNING, "voice_ref", &env), "CreateEnv");
    check(ort->CreateSessionOptions(&so), "CreateSessionOptions");
    check(ort->SetIntraOpNumThreads(so, 1), "SetIntraOpNumThreads");
    check(ort->SetInterOpNumThreads(so, 1), "SetInterOpNumThreads");
    check(ort->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL), "SetSessionGraphOptimizationLevel");
    double t0 = now();
    check(ort->CreateSession(env, argv[1], so, &s), "CreateSession");
    printf("session created in %.2f s\n", now() - t0);
    size_t model_len, quiet_len;
    uint8_t *model = slurp(argv[1], &model_len);
    uint8_t *quiet = quiet_model(model, model_len, &quiet_len, NULL, NULL, 0);
    free(model);
    OrtSession *q;
    t0 = now();
    check(ort->CreateSessionFromArray(env, quiet, quiet_len, so, &q), "CreateSessionFromArray (noise off)");
    printf("noise-off session created in %.2f s\n", now() - t0);
    free(quiet);
    check(ort->CreateCpuMemoryInfo(OrtDeviceAllocator, OrtMemTypeDefault, &mem), "CreateCpuMemoryInfo");

    char path[512];
    snprintf(path, sizeof path, "%s/manifest", outdir);
    FILE *man = fopen(path, "w");
    if (!man) die("cannot write %s", path);
    fprintf(man, "# The voice model's reference renderings, made by tests/manual/voice_ref.c\n"
                 "# (its header documents this format): run A's raw output, with run B's length\n"
                 "# and SNR against A, both runs in one ONNX Runtime session on one thread;\n"
                 "# then, on a #quiet line, the output with the model's random operators\n"
                 "# producing zeros, and run A's SNR against it.\n"
                 "# name voice row speed tokens ids-file samples f32-file samples-b snr-db text\n"
                 "# #quiet name quiet-file samples-quiet snr-a-db\n");

    double total_secs = 0, total_audio = 0;
    int mismatch = 0, unequal = 0;
    for (size_t f = 0; f < NFIX; f++) {
        const char *text = FIXTURES[f].text;
        int64_t ids[TTS_VOICE_MAX_TOKENS];
        float style[STYLE_DIM];
        size_t chars, row, n = fixture_inputs(t, npz, npz_len, f, ids, style, &chars, &row);
        float speed = TTS_VOICE_SPEED;

        size_t na, nb;
        double ta = now();
        float *a = run(s, mem, ids, n, style, speed, &na);
        double tb = now();
        float *b = run(s, mem, ids, n, style, speed, &nb);
        double te = now();
        double sig = 0, noise = 0;
        for (size_t i = 0; i < na && i < nb; i++) {
            sig += (double)a[i] * a[i];
            noise += ((double)b[i] - a[i]) * ((double)b[i] - a[i]);
        }
        double snr = noise > 0 ? 10 * log10(sig / noise) : INFINITY;
        double secs = (double)na / TTS_VOICE_RATE;
        printf("%-14s %s chars %3zu row %3zu tokens %3zu  samples A %7zu B %7zu  audio %6.3f s  "
               "render A %6.3f s B %6.3f s  RTF A %.3f B %.3f  SNR B/A %6.2f dB\n",
               FIXTURES[f].name, FIXTURES[f].voice, chars, row, n, na, nb, secs, tb - ta, te - tb,
               (tb - ta) / secs, (te - tb) / secs, snr);
        if (na != nb) {
            printf("*** SAMPLE COUNTS DIFFER for %s: A %zu, B %zu ***\n", FIXTURES[f].name, na, nb);
            mismatch = 1;
        }

        size_t nq1, nq2;
        double tq = now();
        float *q1 = run(q, mem, ids, n, style, speed, &nq1);
        double tq2 = now();
        float *q2 = run(q, mem, ids, n, style, speed, &nq2);
        double tqe = now();
        size_t differ = 0;
        double maxdiff = 0, qsig = 0, qnoise = 0;
        for (size_t i = 0; i < nq1 && i < nq2; i++) {
            if (memcmp(&q1[i], &q2[i], sizeof *q1) != 0) {
                differ++;
                double dd = fabs((double)q1[i] - q2[i]);
                if (dd > maxdiff) maxdiff = dd;
            }
        }
        for (size_t i = 0; i < nq1 && i < na; i++) {
            qsig += (double)q1[i] * q1[i];
            qnoise += ((double)a[i] - q1[i]) * ((double)a[i] - q1[i]);
        }
        double snr_q = qnoise > 0 ? 10 * log10(qsig / qnoise) : INFINITY;
        printf("%-14s noise off: samples Q1 %7zu Q2 %7zu  render Q1 %6.3f s Q2 %6.3f s  Q2 vs Q1: %s"
               "  SNR A/Q1 %6.2f dB\n", FIXTURES[f].name, nq1, nq2, tq2 - tq, tqe - tq2,
               nq1 == nq2 && differ == 0 ? "bit-identical" : "DIFFERENT", snr_q);
        if (nq1 != nq2 || differ) {
            printf("*** NOISE-OFF RUNS DIFFER for %s: samples %zu and %zu, %zu samples differ, largest by %g ***\n",
                   FIXTURES[f].name, nq1, nq2, differ, maxdiff);
            unequal = 1;
        }
        if (nq1 != na) printf("note: %s: noise-off length %zu, run A length %zu\n", FIXTURES[f].name, nq1, na);
        total_secs += (tb - ta) + (te - tb);
        total_audio += secs + (double)nb / TTS_VOICE_RATE;

        write_le(outdir, FIXTURES[f].name, "ids", ids, n, 8);
        write_le(outdir, FIXTURES[f].name, "f32", a, na, 4);
        fprintf(man, "%s %s %zu %g %zu %s.ids %zu %s.f32 %zu %.2f %s\n", FIXTURES[f].name, FIXTURES[f].voice,
                row, (double)speed, n, FIXTURES[f].name, na, FIXTURES[f].name, nb, snr, text);
        write_le(outdir, FIXTURES[f].name, "quiet.f32", q1, nq1, 4);
        fprintf(man, "#quiet %s %s.quiet.f32 %zu %.2f\n", FIXTURES[f].name, FIXTURES[f].name, nq1, snr_q);
        free(q1);
        free(q2);
        free(a);
        free(b);
    }
    if (fclose(man) != 0) die("cannot write %s", path);

    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    printf("total: render %.3f s for %.3f s of audio, RTF %.3f; peak RSS %ld MB\n", total_secs, total_audio,
           total_secs / total_audio, ru.ru_maxrss / 1024);
    if (mismatch) printf("*** RUN A AND RUN B DIFFER IN LENGTH FOR AT LEAST ONE FIXTURE ***\n");
    if (unequal) printf("*** THE TWO NOISE-OFF RUNS DIFFER FOR AT LEAST ONE FIXTURE ***\n");

    ort->ReleaseMemoryInfo(mem);
    ort->ReleaseSession(q);
    ort->ReleaseSession(s);
    ort->ReleaseSessionOptions(so);
    ort->ReleaseEnv(env);
    tts_free(t);
    free(npz);
    return mismatch || unequal;
}
