/*
 * OpenChime — the voice model's data file, made from the model as it is
 * published (READ-ALOUD.md, TTSKIT.md): `openchimed --voice-pack <model.onnx>
 * <voices.npz> <out>` writes voice.bin (ttskit.h has the format).
 *
 * It reads the .onnx file (protobuf) and the stored zip of the voices with no
 * library, takes exactly the tensors the forward pass needs, names them in
 * ttskit's own terms and writes each in the layout ttskit's kernels read
 * (ttskit/tts_priv.h). The same inputs give the same bytes on every run and
 * machine. Anything that is not exactly the expected model -- a tensor missing,
 * of another shape or type, an operator with other settings, a tensor the table
 * does not account for, a voice too many or too few -- is refused, naming the
 * first difference. Plain C in every build, so `make test` drives it.
 */
#ifndef OC_VOICE_PACK_H
#define OC_VOICE_PACK_H

#include <stddef.h>

/* How a tensor is taken from the model and laid out (voice_pack.c). */
enum {
    VK_F32 = 1,      /* float or half, as it is, widened to f32, size-1 axes dropped */
    VK_EPS,          /* the node's epsilon attribute, f32 [1] */
    VK_QMM,          /* MatMulInteger B: int8 [round4(n)][round16(k)], transposed */
    VK_QCONV,        /* ConvInteger W: uint8 [round4(cout)][k][round16(cin)] */
    VK_LSTM_W,       /* DynamicQuantizeLSTM W of one direction: int8 [4H][round16(in)] */
    VK_LSTM_R,       /* ... R of one direction: int8 [4H][H] */
    VK_LSTM_B,       /* ... B of one direction: f32 [8H] */
    VK_MATF,         /* float MatMul B: f32 [k][round8(n)] */
    VK_CONVF,        /* float Conv W on one input channel: f32 [k][round8(cout)] */
    VK_CONVT,        /* ConvTranspose W: f32 [s][k/s][cin][round8(cout)] */
    VK_CONVT_DW,     /* depthwise ConvTranspose W: f32 [k][channels] */
    VK_NORM1         /* a norm with scale 1 and bias 0: checked, not stored */
};
/* The source tensor's type, as the model format numbers them. */
enum { OT_F32 = 1, OT_U8 = 2, OT_I8 = 3, OT_F16 = 10 };

/* One tensor of voice.bin and where it comes from: input `input` of the node
 * called `node`, through any Reshape, Cast or Identity in between, which must be
 * a constant of type `dtype` and shape `dims`. `geom` is the operator's settings
 * the forward pass assumes (all 0: none checked): for a convolution stride,
 * dilation, pad at the start, pad at the end; for a transposed one stride, pad
 * at the start, pad at the end, output padding. `eps` is a VK_NORM1's epsilon. */
typedef struct {
    const char   *name;          /* in voice.bin; NULL for VK_NORM1 */
    const char   *node;
    unsigned char kind, input, dir, dtype, ndim;
    int           dims[4];
    int           geom[4];
    float         eps;
} oc_voice_row;

typedef struct {
    const oc_voice_row *rows;
    size_t              nrows;
    const char *const  *voices;   /* the style tables: "<id>.npy" in the zip, "voice.<id>" in the file */
    size_t              nvoices;
    int                 style_rows, style_dim;
} oc_voice_spec;

/* The voice model read-aloud ships. */
extern const oc_voice_spec OC_VOICE_SPEC;

/* Write voice.bin for `spec` from the model's and the voices' bytes to `out`
 * (through a temporary file beside it, renamed into place). 0, with the file's
 * size in *written (may be NULL); -1 with the reason in `err`. */
int oc_voice_pack_mem(const oc_voice_spec *spec, const unsigned char *model, size_t model_len,
                      const unsigned char *voices, size_t voices_len, const char *out, size_t *written,
                      char *err, size_t errcap);
/* The same from the two files, for OC_VOICE_SPEC. */
int oc_voice_pack(const char *model, const char *voices, const char *out, size_t *written, char *err, size_t errcap);
/* `openchimed --voice-pack`: says what it wrote or why not on stderr; the exit code. */
int oc_voice_pack_cli(const char *model, const char *voices, const char *out);

#endif
