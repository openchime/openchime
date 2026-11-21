/*
 * OpenChime — voice.bin from the published voice model (voice_pack.h).
 *
 * Tensor names. Each is <part>.<module>.<what>, in ttskit's terms rather than
 * the upstream graph's:
 *   ctx    the context encoder over the phonemes: emb.tok, emb.pos, emb.type
 *          (embeddings), emb.ln, in (to the layer width), layer.* (the one layer
 *          applied 12 times: q, k, v, o, ln1, ffn, ffn_out, ln2), out (to 512)
 *   pro    the prosody predictor: enc.N.lstm and enc.N.ada (its three LSTMs and
 *          the style projections of the norms between them), dur.lstm and
 *          dur.proj (durations), lstm (shared), f0.N and n.N (the pitch and
 *          energy blocks), f0.proj, n.proj
 *   txt    the text encoder: emb, cnn.N.conv, cnn.N.ln, lstm
 *   dec    the decoder: f0 and n (the stride-2 convolutions of the two
 *          curves), asr, enc, dec.N
 *   gen    the waveform generator: src.harm and src.lin (the harmonic source),
 *          stft.re and stft.im (the forward transform's bases), noise.N, nres.N
 *          and res.N (the noise and main residual blocks), up.N, post,
 *          istft.re and istft.im
 *   voice  voice.<id>: a voice's style table, 400 rows of 256
 *   norm   norm.eps: the epsilon of every norm without an affine part
 * A residual block (pro.f0.N, pro.n.N, dec.enc, dec.dec.N) has conv1, conv2,
 * ada1 and ada2 (the style projections of its two adaptive norms), skip (the
 * 1x1 shortcut, when the width changes) and pool (the depthwise transposed
 * convolution that doubles its length, when it upsamples). A generator block
 * has c1.J and c2.J (its convolutions), ada1.J and ada2.J, and the Snake
 * activations' alpha1.J, alpha2.J and inv1.J, inv2.J (1/alpha, as the model
 * stores it). The last part says what the tensor is: w a weight (with the
 * quantised ones' scale and zero point in its entry), b a bias (a layer norm's
 * beta too), g a layer norm's gamma, eps its epsilon; an LSTM's are fwd.* and
 * bwd.*, each with w (input), r (recurrent) and b (its 8H biases).
 *
 * What the file leaves out: the arithmetic constants of the graph (0.5, sqrt 2,
 * the sample rate and the like), which are the forward pass's code; the norms
 * whose scale is 1 and bias 0 (checked here, so the forward pass passes none);
 * and shapes. Every other constant of the model is accounted for: one this
 * table does not take is refused as unexpected.
 */
#include "voice_pack.h"
#include "tts_data.h"
#include "ttskit.h"

#include <mbedtls/sha256.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void seterr(char *err, size_t cap, const char *fmt, ...) {
    if (!err || !cap) return;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, cap, fmt, ap);
    va_end(ap);
}

/* ---- the table ------------------------------------------------------------- */

#define BERT "/bert/"
#define AL   "/bert/encoder/albert_layer_groups.0/albert_layers.0/"
#define DEC  "/decoder/"
#define GEN  "/decoder/generator/"
#define G0   { 0 }, 0                  /* no settings checked */

static const oc_voice_row ROWS[] = {
    { "ctx.emb.ln.b", BERT "embeddings/LayerNorm/LayerNormalization", VK_F32, 2, 0, OT_F32, 1, { 128 }, G0 },
    { "ctx.emb.ln.eps", BERT "embeddings/LayerNorm/LayerNormalization", VK_EPS, 0, 0, OT_F32, 1, { 1 }, G0 },
    { "ctx.emb.ln.g", BERT "embeddings/LayerNorm/LayerNormalization", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "ctx.emb.pos", BERT "embeddings/position_embeddings/Gather", VK_F32, 0, 0, OT_F32, 2, { 512, 128 }, G0 },
    { "ctx.emb.tok", BERT "embeddings/word_embeddings/Gather", VK_F32, 0, 0, OT_F32, 2, { 178, 128 }, G0 },
    { "ctx.emb.type", BERT "embeddings/token_type_embeddings/Gather", VK_F32, 0, 0, OT_F32, 2, { 2, 128 }, G0 },
    { "ctx.in.b", BERT "encoder/embedding_hidden_mapping_in/Add", VK_F32, 0, 0, OT_F32, 1, { 768 }, G0 },
    { "ctx.in.w", BERT "encoder/embedding_hidden_mapping_in/MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 768 }, G0 },
    { "ctx.layer.ffn.b", AL "ffn/Add", VK_F32, 0, 0, OT_F32, 1, { 2048 }, G0 },
    { "ctx.layer.ffn.w", AL "ffn/MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 768, 2048 }, G0 },
    { "ctx.layer.ffn_out.b", AL "ffn_output/Add", VK_F32, 0, 0, OT_F16, 1, { 768 }, G0 },
    { "ctx.layer.ffn_out.w", AL "ffn_output/MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 2048, 768 }, G0 },
    { "ctx.layer.k.b", AL "attention/key/Add", VK_F32, 0, 0, OT_F32, 1, { 768 }, G0 },
    { "ctx.layer.k.w", AL "attention/key/MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 768, 768 }, G0 },
    { "ctx.layer.ln1.b", AL "attention/LayerNorm/LayerNormalization", VK_F32, 2, 0, OT_F32, 1, { 768 }, G0 },
    { "ctx.layer.ln1.eps", AL "attention/LayerNorm/LayerNormalization", VK_EPS, 0, 0, OT_F32, 1, { 1 }, G0 },
    { "ctx.layer.ln1.g", AL "attention/LayerNorm/LayerNormalization", VK_F32, 1, 0, OT_F32, 1, { 768 }, G0 },
    { "ctx.layer.ln2.b", AL "full_layer_layer_norm/LayerNormalization", VK_F32, 2, 0, OT_F32, 1, { 768 }, G0 },
    { "ctx.layer.ln2.eps", AL "full_layer_layer_norm/LayerNormalization", VK_EPS, 0, 0, OT_F32, 1, { 1 }, G0 },
    { "ctx.layer.ln2.g", AL "full_layer_layer_norm/LayerNormalization", VK_F32, 1, 0, OT_F32, 1, { 768 }, G0 },
    { "ctx.layer.o.b", AL "attention/dense/Add", VK_F32, 0, 0, OT_F32, 1, { 768 }, G0 },
    { "ctx.layer.o.w", AL "attention/dense/MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 768, 768 }, G0 },
    { "ctx.layer.q.b", AL "attention/query/Add", VK_F32, 0, 0, OT_F32, 1, { 768 }, G0 },
    { "ctx.layer.q.w", AL "attention/query/MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 768, 768 }, G0 },
    { "ctx.layer.v.b", AL "attention/value/Add", VK_F32, 0, 0, OT_F32, 1, { 768 }, G0 },
    { "ctx.layer.v.w", AL "attention/value/MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 768, 768 }, G0 },
    { "ctx.out.b", "/bert_encoder/Add", VK_F32, 0, 0, OT_F32, 1, { 512 }, G0 },
    { "ctx.out.w", "/bert_encoder/MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 768, 512 }, G0 },
    { "dec.asr.b", DEC "asr_res/asr_res.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 64 }, G0 },
    { "dec.asr.w", DEC "asr_res/asr_res.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 64, 512, 1 }, { 1, 1, 0, 0 }, 0 },
    { "dec.dec.0.ada1.b", DEC "decode.0/norm1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 2180 }, G0 },
    { "dec.dec.0.ada1.w", DEC "decode.0/norm1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 2180 }, G0 },
    { "dec.dec.0.ada2.b", DEC "decode.0/norm2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 2048 }, G0 },
    { "dec.dec.0.ada2.w", DEC "decode.0/norm2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 2048 }, G0 },
    { "dec.dec.0.conv1.b", DEC "decode.0/conv1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "dec.dec.0.conv1.w", DEC "decode.0/conv1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 1024, 1090, 3 }, { 1, 1, 1, 1 }, 0 },
    { "dec.dec.0.conv2.b", DEC "decode.0/conv2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "dec.dec.0.conv2.w", DEC "decode.0/conv2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 1024, 1024, 3 }, { 1, 1, 1, 1 }, 0 },
    { "dec.dec.0.skip.w", DEC "decode.0/conv1x1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 1024, 1090, 1 }, { 1, 1, 0, 0 }, 0 },
    { "dec.dec.1.ada1.b", DEC "decode.1/norm1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 2180 }, G0 },
    { "dec.dec.1.ada1.w", DEC "decode.1/norm1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 2180 }, G0 },
    { "dec.dec.1.ada2.b", DEC "decode.1/norm2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 2048 }, G0 },
    { "dec.dec.1.ada2.w", DEC "decode.1/norm2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 2048 }, G0 },
    { "dec.dec.1.conv1.b", DEC "decode.1/conv1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "dec.dec.1.conv1.w", DEC "decode.1/conv1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 1024, 1090, 3 }, { 1, 1, 1, 1 }, 0 },
    { "dec.dec.1.conv2.b", DEC "decode.1/conv2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "dec.dec.1.conv2.w", DEC "decode.1/conv2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 1024, 1024, 3 }, { 1, 1, 1, 1 }, 0 },
    { "dec.dec.1.skip.w", DEC "decode.1/conv1x1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 1024, 1090, 1 }, { 1, 1, 0, 0 }, 0 },
    { "dec.dec.2.ada1.b", DEC "decode.2/norm1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 2180 }, G0 },
    { "dec.dec.2.ada1.w", DEC "decode.2/norm1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 2180 }, G0 },
    { "dec.dec.2.ada2.b", DEC "decode.2/norm2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 2048 }, G0 },
    { "dec.dec.2.ada2.w", DEC "decode.2/norm2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 2048 }, G0 },
    { "dec.dec.2.conv1.b", DEC "decode.2/conv1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "dec.dec.2.conv1.w", DEC "decode.2/conv1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 1024, 1090, 3 }, { 1, 1, 1, 1 }, 0 },
    { "dec.dec.2.conv2.b", DEC "decode.2/conv2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "dec.dec.2.conv2.w", DEC "decode.2/conv2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 1024, 1024, 3 }, { 1, 1, 1, 1 }, 0 },
    { "dec.dec.2.skip.w", DEC "decode.2/conv1x1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 1024, 1090, 1 }, { 1, 1, 0, 0 }, 0 },
    { "dec.dec.3.ada1.b", DEC "decode.3/norm1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 2180 }, G0 },
    { "dec.dec.3.ada1.w", DEC "decode.3/norm1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 2180 }, G0 },
    { "dec.dec.3.ada2.b", DEC "decode.3/norm2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "dec.dec.3.ada2.w", DEC "decode.3/norm2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 1024 }, G0 },
    { "dec.dec.3.conv1.b", DEC "decode.3/conv1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "dec.dec.3.conv1.w", DEC "decode.3/conv1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 512, 1090, 3 }, { 1, 1, 1, 1 }, 0 },
    { "dec.dec.3.conv2.b", DEC "decode.3/conv2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "dec.dec.3.conv2.w", DEC "decode.3/conv2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 512, 512, 3 }, { 1, 1, 1, 1 }, 0 },
    { "dec.dec.3.pool.b", DEC "decode.3/pool/ConvTranspose", VK_F32, 2, 0, OT_F16, 1, { 1090 }, G0 },
    { "dec.dec.3.pool.w", DEC "decode.3/pool/ConvTranspose", VK_CONVT_DW, 1, 0, OT_F16, 3, { 1090, 1, 3 }, { 2, 1, 1, 1 }, 0 },
    { "dec.dec.3.skip.w", DEC "decode.3/conv1x1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 512, 1090, 1 }, { 1, 1, 0, 0 }, 0 },
    { "dec.enc.ada1.b", DEC "encode/norm1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 1028 }, G0 },
    { "dec.enc.ada1.w", DEC "encode/norm1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 1028 }, G0 },
    { "dec.enc.ada2.b", DEC "encode/norm2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 2048 }, G0 },
    { "dec.enc.ada2.w", DEC "encode/norm2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 2048 }, G0 },
    { "dec.enc.conv1.b", DEC "encode/conv1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "dec.enc.conv1.w", DEC "encode/conv1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 1024, 514, 3 }, { 1, 1, 1, 1 }, 0 },
    { "dec.enc.conv2.b", DEC "encode/conv2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "dec.enc.conv2.w", DEC "encode/conv2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 1024, 1024, 3 }, { 1, 1, 1, 1 }, 0 },
    { "dec.enc.skip.w", DEC "encode/conv1x1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 1024, 514, 1 }, { 1, 1, 0, 0 }, 0 },
    { "dec.f0.b", DEC "F0_conv/Conv", VK_F32, 2, 0, OT_F32, 1, { 1 }, G0 },
    { "dec.f0.w", DEC "F0_conv/Conv", VK_F32, 1, 0, OT_F32, 3, { 1, 1, 3 }, { 2, 1, 1, 1 }, 0 },
    { "dec.n.b", DEC "N_conv/Conv", VK_F32, 2, 0, OT_F32, 1, { 1 }, G0 },
    { "dec.n.w", DEC "N_conv/Conv", VK_F32, 1, 0, OT_F32, 3, { 1, 1, 3 }, { 2, 1, 1, 1 }, 0 },
    { "gen.istft.im.w", GEN "ConvTranspose_1", VK_F32, 1, 0, OT_F32, 3, { 11, 1, 20 }, { 5, 0, 0, 0 }, 0 },
    { "gen.istft.re.w", GEN "ConvTranspose", VK_F32, 1, 0, OT_F32, 3, { 11, 1, 20 }, { 5, 0, 0, 0 }, 0 },
    { "gen.noise.0.b", GEN "noise_convs.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.noise.0.w", GEN "noise_convs.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 22, 12 }, { 6, 1, 3, 3 }, 0 },
    { "gen.noise.1.b", GEN "noise_convs.1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.noise.1.w", GEN "noise_convs.1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 22, 1 }, { 1, 1, 0, 0 }, 0 },
    { "gen.nres.0.ada1.0.b", GEN "noise_res.0/adain1.0/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.nres.0.ada1.0.w", GEN "noise_res.0/adain1.0/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.nres.0.ada1.1.b", GEN "noise_res.0/adain1.1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.nres.0.ada1.1.w", GEN "noise_res.0/adain1.1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.nres.0.ada1.2.b", GEN "noise_res.0/adain1.2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.nres.0.ada1.2.w", GEN "noise_res.0/adain1.2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.nres.0.ada2.0.b", GEN "noise_res.0/adain2.0/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.nres.0.ada2.0.w", GEN "noise_res.0/adain2.0/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.nres.0.ada2.1.b", GEN "noise_res.0/adain2.1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.nres.0.ada2.1.w", GEN "noise_res.0/adain2.1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.nres.0.ada2.2.b", GEN "noise_res.0/adain2.2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.nres.0.ada2.2.w", GEN "noise_res.0/adain2.2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.nres.0.alpha1.0", GEN "noise_res.0/Mul", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.nres.0.alpha1.1", GEN "noise_res.0/Mul_4", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.nres.0.alpha1.2", GEN "noise_res.0/Mul_8", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.nres.0.alpha2.0", GEN "noise_res.0/Mul_2", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.nres.0.alpha2.1", GEN "noise_res.0/Mul_6", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.nres.0.alpha2.2", GEN "noise_res.0/Mul_10", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.nres.0.c1.0.b", GEN "noise_res.0/convs1.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.nres.0.c1.0.w", GEN "noise_res.0/convs1.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 7 }, { 1, 1, 3, 3 }, 0 },
    { "gen.nres.0.c1.1.b", GEN "noise_res.0/convs1.1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.nres.0.c1.1.w", GEN "noise_res.0/convs1.1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 7 }, { 1, 3, 9, 9 }, 0 },
    { "gen.nres.0.c1.2.b", GEN "noise_res.0/convs1.2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.nres.0.c1.2.w", GEN "noise_res.0/convs1.2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 7 }, { 1, 5, 15, 15 }, 0 },
    { "gen.nres.0.c2.0.b", GEN "noise_res.0/convs2.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.nres.0.c2.0.w", GEN "noise_res.0/convs2.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 7 }, { 1, 1, 3, 3 }, 0 },
    { "gen.nres.0.c2.1.b", GEN "noise_res.0/convs2.1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.nres.0.c2.1.w", GEN "noise_res.0/convs2.1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 7 }, { 1, 1, 3, 3 }, 0 },
    { "gen.nres.0.c2.2.b", GEN "noise_res.0/convs2.2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.nres.0.c2.2.w", GEN "noise_res.0/convs2.2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 7 }, { 1, 1, 3, 3 }, 0 },
    { "gen.nres.0.inv1.0", GEN "noise_res.0/Mul_1", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.nres.0.inv1.1", GEN "noise_res.0/Mul_5", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.nres.0.inv1.2", GEN "noise_res.0/Mul_9", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.nres.0.inv2.0", GEN "noise_res.0/Mul_3", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.nres.0.inv2.1", GEN "noise_res.0/Mul_7", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.nres.0.inv2.2", GEN "noise_res.0/Mul_11", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.nres.1.ada1.0.b", GEN "noise_res.1/adain1.0/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.nres.1.ada1.0.w", GEN "noise_res.1/adain1.0/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.nres.1.ada1.1.b", GEN "noise_res.1/adain1.1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.nres.1.ada1.1.w", GEN "noise_res.1/adain1.1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.nres.1.ada1.2.b", GEN "noise_res.1/adain1.2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.nres.1.ada1.2.w", GEN "noise_res.1/adain1.2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.nres.1.ada2.0.b", GEN "noise_res.1/adain2.0/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.nres.1.ada2.0.w", GEN "noise_res.1/adain2.0/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.nres.1.ada2.1.b", GEN "noise_res.1/adain2.1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.nres.1.ada2.1.w", GEN "noise_res.1/adain2.1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.nres.1.ada2.2.b", GEN "noise_res.1/adain2.2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.nres.1.ada2.2.w", GEN "noise_res.1/adain2.2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.nres.1.alpha1.0", GEN "noise_res.1/Mul", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.nres.1.alpha1.1", GEN "noise_res.1/Mul_4", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.nres.1.alpha1.2", GEN "noise_res.1/Mul_8", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.nres.1.alpha2.0", GEN "noise_res.1/Mul_2", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.nres.1.alpha2.1", GEN "noise_res.1/Mul_6", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.nres.1.alpha2.2", GEN "noise_res.1/Mul_10", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.nres.1.c1.0.b", GEN "noise_res.1/convs1.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.nres.1.c1.0.w", GEN "noise_res.1/convs1.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 11 }, { 1, 1, 5, 5 }, 0 },
    { "gen.nres.1.c1.1.b", GEN "noise_res.1/convs1.1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.nres.1.c1.1.w", GEN "noise_res.1/convs1.1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 11 }, { 1, 3, 15, 15 }, 0 },
    { "gen.nres.1.c1.2.b", GEN "noise_res.1/convs1.2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.nres.1.c1.2.w", GEN "noise_res.1/convs1.2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 11 }, { 1, 5, 25, 25 }, 0 },
    { "gen.nres.1.c2.0.b", GEN "noise_res.1/convs2.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.nres.1.c2.0.w", GEN "noise_res.1/convs2.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 11 }, { 1, 1, 5, 5 }, 0 },
    { "gen.nres.1.c2.1.b", GEN "noise_res.1/convs2.1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.nres.1.c2.1.w", GEN "noise_res.1/convs2.1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 11 }, { 1, 1, 5, 5 }, 0 },
    { "gen.nres.1.c2.2.b", GEN "noise_res.1/convs2.2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.nres.1.c2.2.w", GEN "noise_res.1/convs2.2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 11 }, { 1, 1, 5, 5 }, 0 },
    { "gen.nres.1.inv1.0", GEN "noise_res.1/Mul_1", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.nres.1.inv1.1", GEN "noise_res.1/Mul_5", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.nres.1.inv1.2", GEN "noise_res.1/Mul_9", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.nres.1.inv2.0", GEN "noise_res.1/Mul_3", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.nres.1.inv2.1", GEN "noise_res.1/Mul_7", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.nres.1.inv2.2", GEN "noise_res.1/Mul_11", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.post.b", GEN "conv_post/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 22 }, G0 },
    { "gen.post.w", GEN "conv_post/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 22, 128, 7 }, { 1, 1, 3, 3 }, 0 },
    { "gen.res.0.ada1.0.b", GEN "resblocks.0/adain1.0/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.res.0.ada1.0.w", GEN "resblocks.0/adain1.0/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.res.0.ada1.1.b", GEN "resblocks.0/adain1.1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.res.0.ada1.1.w", GEN "resblocks.0/adain1.1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.res.0.ada1.2.b", GEN "resblocks.0/adain1.2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.res.0.ada1.2.w", GEN "resblocks.0/adain1.2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.res.0.ada2.0.b", GEN "resblocks.0/adain2.0/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.res.0.ada2.0.w", GEN "resblocks.0/adain2.0/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.res.0.ada2.1.b", GEN "resblocks.0/adain2.1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.res.0.ada2.1.w", GEN "resblocks.0/adain2.1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.res.0.ada2.2.b", GEN "resblocks.0/adain2.2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.res.0.ada2.2.w", GEN "resblocks.0/adain2.2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.res.0.alpha1.0", GEN "resblocks.0/Mul", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.0.alpha1.1", GEN "resblocks.0/Mul_4", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.0.alpha1.2", GEN "resblocks.0/Mul_8", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.0.alpha2.0", GEN "resblocks.0/Mul_2", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.0.alpha2.1", GEN "resblocks.0/Mul_6", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.0.alpha2.2", GEN "resblocks.0/Mul_10", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.0.c1.0.b", GEN "resblocks.0/convs1.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.0.c1.0.w", GEN "resblocks.0/convs1.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 1, 1, 1 }, 0 },
    { "gen.res.0.c1.1.b", GEN "resblocks.0/convs1.1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.0.c1.1.w", GEN "resblocks.0/convs1.1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 3, 3, 3 }, 0 },
    { "gen.res.0.c1.2.b", GEN "resblocks.0/convs1.2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.0.c1.2.w", GEN "resblocks.0/convs1.2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 5, 5, 5 }, 0 },
    { "gen.res.0.c2.0.b", GEN "resblocks.0/convs2.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.0.c2.0.w", GEN "resblocks.0/convs2.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 1, 1, 1 }, 0 },
    { "gen.res.0.c2.1.b", GEN "resblocks.0/convs2.1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.0.c2.1.w", GEN "resblocks.0/convs2.1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 1, 1, 1 }, 0 },
    { "gen.res.0.c2.2.b", GEN "resblocks.0/convs2.2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.0.c2.2.w", GEN "resblocks.0/convs2.2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 1, 1, 1 }, 0 },
    { "gen.res.0.inv1.0", GEN "resblocks.0/Mul_1", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.0.inv1.1", GEN "resblocks.0/Mul_5", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.0.inv1.2", GEN "resblocks.0/Mul_9", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.0.inv2.0", GEN "resblocks.0/Mul_3", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.0.inv2.1", GEN "resblocks.0/Mul_7", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.0.inv2.2", GEN "resblocks.0/Mul_11", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.1.ada1.0.b", GEN "resblocks.1/adain1.0/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.res.1.ada1.0.w", GEN "resblocks.1/adain1.0/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.res.1.ada1.1.b", GEN "resblocks.1/adain1.1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.res.1.ada1.1.w", GEN "resblocks.1/adain1.1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.res.1.ada1.2.b", GEN "resblocks.1/adain1.2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.res.1.ada1.2.w", GEN "resblocks.1/adain1.2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.res.1.ada2.0.b", GEN "resblocks.1/adain2.0/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.res.1.ada2.0.w", GEN "resblocks.1/adain2.0/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.res.1.ada2.1.b", GEN "resblocks.1/adain2.1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.res.1.ada2.1.w", GEN "resblocks.1/adain2.1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.res.1.ada2.2.b", GEN "resblocks.1/adain2.2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "gen.res.1.ada2.2.w", GEN "resblocks.1/adain2.2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "gen.res.1.alpha1.0", GEN "resblocks.1/Mul", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.1.alpha1.1", GEN "resblocks.1/Mul_4", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.1.alpha1.2", GEN "resblocks.1/Mul_8", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.1.alpha2.0", GEN "resblocks.1/Mul_2", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.1.alpha2.1", GEN "resblocks.1/Mul_6", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.1.alpha2.2", GEN "resblocks.1/Mul_10", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.1.c1.0.b", GEN "resblocks.1/convs1.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.1.c1.0.w", GEN "resblocks.1/convs1.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 1, 1, 1 }, 0 },
    { "gen.res.1.c1.1.b", GEN "resblocks.1/convs1.1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.1.c1.1.w", GEN "resblocks.1/convs1.1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 3, 3, 3 }, 0 },
    { "gen.res.1.c1.2.b", GEN "resblocks.1/convs1.2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.1.c1.2.w", GEN "resblocks.1/convs1.2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 5, 5, 5 }, 0 },
    { "gen.res.1.c2.0.b", GEN "resblocks.1/convs2.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.1.c2.0.w", GEN "resblocks.1/convs2.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 1, 1, 1 }, 0 },
    { "gen.res.1.c2.1.b", GEN "resblocks.1/convs2.1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.1.c2.1.w", GEN "resblocks.1/convs2.1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 1, 1, 1 }, 0 },
    { "gen.res.1.c2.2.b", GEN "resblocks.1/convs2.2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.1.c2.2.w", GEN "resblocks.1/convs2.2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 1, 1, 1 }, 0 },
    { "gen.res.1.inv1.0", GEN "resblocks.1/Mul_1", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.1.inv1.1", GEN "resblocks.1/Mul_5", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.1.inv1.2", GEN "resblocks.1/Mul_9", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.1.inv2.0", GEN "resblocks.1/Mul_3", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.1.inv2.1", GEN "resblocks.1/Mul_7", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.1.inv2.2", GEN "resblocks.1/Mul_11", VK_F32, 0, 0, OT_F32, 3, { 1, 256, 1 }, G0 },
    { "gen.res.2.ada1.0.b", GEN "resblocks.2/adain1.0/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.2.ada1.0.w", GEN "resblocks.2/adain1.0/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.res.2.ada1.1.b", GEN "resblocks.2/adain1.1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.2.ada1.1.w", GEN "resblocks.2/adain1.1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.res.2.ada1.2.b", GEN "resblocks.2/adain1.2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.2.ada1.2.w", GEN "resblocks.2/adain1.2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.res.2.ada2.0.b", GEN "resblocks.2/adain2.0/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.2.ada2.0.w", GEN "resblocks.2/adain2.0/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.res.2.ada2.1.b", GEN "resblocks.2/adain2.1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.2.ada2.1.w", GEN "resblocks.2/adain2.1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.res.2.ada2.2.b", GEN "resblocks.2/adain2.2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.2.ada2.2.w", GEN "resblocks.2/adain2.2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.res.2.alpha1.0", GEN "resblocks.2/Mul", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.2.alpha1.1", GEN "resblocks.2/Mul_4", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.2.alpha1.2", GEN "resblocks.2/Mul_8", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.2.alpha2.0", GEN "resblocks.2/Mul_2", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.2.alpha2.1", GEN "resblocks.2/Mul_6", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.2.alpha2.2", GEN "resblocks.2/Mul_10", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.2.c1.0.b", GEN "resblocks.2/convs1.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.res.2.c1.0.w", GEN "resblocks.2/convs1.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 3 }, { 1, 1, 1, 1 }, 0 },
    { "gen.res.2.c1.1.b", GEN "resblocks.2/convs1.1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.res.2.c1.1.w", GEN "resblocks.2/convs1.1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 3 }, { 1, 3, 3, 3 }, 0 },
    { "gen.res.2.c1.2.b", GEN "resblocks.2/convs1.2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.res.2.c1.2.w", GEN "resblocks.2/convs1.2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 3 }, { 1, 5, 5, 5 }, 0 },
    { "gen.res.2.c2.0.b", GEN "resblocks.2/convs2.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.res.2.c2.0.w", GEN "resblocks.2/convs2.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 3 }, { 1, 1, 1, 1 }, 0 },
    { "gen.res.2.c2.1.b", GEN "resblocks.2/convs2.1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.res.2.c2.1.w", GEN "resblocks.2/convs2.1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 3 }, { 1, 1, 1, 1 }, 0 },
    { "gen.res.2.c2.2.b", GEN "resblocks.2/convs2.2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.res.2.c2.2.w", GEN "resblocks.2/convs2.2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 3 }, { 1, 1, 1, 1 }, 0 },
    { "gen.res.2.inv1.0", GEN "resblocks.2/Mul_1", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.2.inv1.1", GEN "resblocks.2/Mul_5", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.2.inv1.2", GEN "resblocks.2/Mul_9", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.2.inv2.0", GEN "resblocks.2/Mul_3", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.2.inv2.1", GEN "resblocks.2/Mul_7", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.2.inv2.2", GEN "resblocks.2/Mul_11", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.3.ada1.0.b", GEN "resblocks.3/adain1.0/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.3.ada1.0.w", GEN "resblocks.3/adain1.0/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.res.3.ada1.1.b", GEN "resblocks.3/adain1.1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.3.ada1.1.w", GEN "resblocks.3/adain1.1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.res.3.ada1.2.b", GEN "resblocks.3/adain1.2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.3.ada1.2.w", GEN "resblocks.3/adain1.2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.res.3.ada2.0.b", GEN "resblocks.3/adain2.0/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.3.ada2.0.w", GEN "resblocks.3/adain2.0/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.res.3.ada2.1.b", GEN "resblocks.3/adain2.1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.3.ada2.1.w", GEN "resblocks.3/adain2.1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.res.3.ada2.2.b", GEN "resblocks.3/adain2.2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "gen.res.3.ada2.2.w", GEN "resblocks.3/adain2.2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 256 }, G0 },
    { "gen.res.3.alpha1.0", GEN "resblocks.3/Mul", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.3.alpha1.1", GEN "resblocks.3/Mul_4", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.3.alpha1.2", GEN "resblocks.3/Mul_8", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.3.alpha2.0", GEN "resblocks.3/Mul_2", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.3.alpha2.1", GEN "resblocks.3/Mul_6", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.3.alpha2.2", GEN "resblocks.3/Mul_10", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.3.c1.0.b", GEN "resblocks.3/convs1.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.res.3.c1.0.w", GEN "resblocks.3/convs1.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 3 }, { 1, 1, 1, 1 }, 0 },
    { "gen.res.3.c1.1.b", GEN "resblocks.3/convs1.1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.res.3.c1.1.w", GEN "resblocks.3/convs1.1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 3 }, { 1, 3, 3, 3 }, 0 },
    { "gen.res.3.c1.2.b", GEN "resblocks.3/convs1.2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.res.3.c1.2.w", GEN "resblocks.3/convs1.2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 3 }, { 1, 5, 5, 5 }, 0 },
    { "gen.res.3.c2.0.b", GEN "resblocks.3/convs2.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.res.3.c2.0.w", GEN "resblocks.3/convs2.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 3 }, { 1, 1, 1, 1 }, 0 },
    { "gen.res.3.c2.1.b", GEN "resblocks.3/convs2.1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.res.3.c2.1.w", GEN "resblocks.3/convs2.1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 3 }, { 1, 1, 1, 1 }, 0 },
    { "gen.res.3.c2.2.b", GEN "resblocks.3/convs2.2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 128 }, G0 },
    { "gen.res.3.c2.2.w", GEN "resblocks.3/convs2.2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 128, 128, 3 }, { 1, 1, 1, 1 }, 0 },
    { "gen.res.3.inv1.0", GEN "resblocks.3/Mul_1", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.3.inv1.1", GEN "resblocks.3/Mul_5", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.3.inv1.2", GEN "resblocks.3/Mul_9", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.3.inv2.0", GEN "resblocks.3/Mul_3", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.3.inv2.1", GEN "resblocks.3/Mul_7", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.res.3.inv2.2", GEN "resblocks.3/Mul_11", VK_F32, 0, 0, OT_F32, 3, { 1, 128, 1 }, G0 },
    { "gen.src.harm", GEN "m_source/l_sin_gen/Mul", VK_F32, 1, 0, OT_F32, 3, { 1, 1, 9 }, G0 },
    { "gen.src.lin.b", GEN "m_source/l_linear/Add", VK_F32, 0, 0, OT_F32, 1, { 1 }, G0 },
    { "gen.src.lin.w", GEN "m_source/l_linear/MatMul", VK_MATF, 1, 0, OT_F32, 2, { 9, 1 }, G0 },
    { "gen.stft.im.w", GEN "Conv_1", VK_CONVF, 1, 0, OT_F32, 3, { 11, 1, 20 }, { 5, 1, 0, 0 }, 0 },
    { "gen.stft.re.w", GEN "Conv", VK_CONVF, 1, 0, OT_F32, 3, { 11, 1, 20 }, { 5, 1, 0, 0 }, 0 },
    { "gen.up.0.b", GEN "ups.0/ConvTranspose", VK_F32, 2, 0, OT_F16, 1, { 256 }, G0 },
    { "gen.up.0.w", GEN "ups.0/ConvTranspose", VK_CONVT, 1, 0, OT_F16, 3, { 512, 256, 20 }, { 10, 5, 5, 0 }, 0 },
    { "gen.up.1.b", GEN "ups.1/ConvTranspose", VK_F32, 2, 0, OT_F16, 1, { 128 }, G0 },
    { "gen.up.1.w", GEN "ups.1/ConvTranspose", VK_CONVT, 1, 0, OT_F16, 3, { 256, 128, 12 }, { 6, 3, 3, 0 }, 0 },
    { "norm.eps", "/N.0/norm1/norm/InstanceNormalization", VK_EPS, 0, 0, OT_F32, 1, { 1 }, G0 },
    { "pro.dur.lstm.bwd.b", "/lstm/LSTM_quant", VK_LSTM_B, 3, 1, OT_F32, 2, { 2, 2048 }, G0 },
    { "pro.dur.lstm.bwd.r", "/lstm/LSTM_quant", VK_LSTM_R, 2, 1, OT_I8, 3, { 2, 256, 1024 }, G0 },
    { "pro.dur.lstm.bwd.w", "/lstm/LSTM_quant", VK_LSTM_W, 1, 1, OT_I8, 3, { 2, 640, 1024 }, G0 },
    { "pro.dur.lstm.fwd.b", "/lstm/LSTM_quant", VK_LSTM_B, 3, 0, OT_F32, 2, { 2, 2048 }, G0 },
    { "pro.dur.lstm.fwd.r", "/lstm/LSTM_quant", VK_LSTM_R, 2, 0, OT_I8, 3, { 2, 256, 1024 }, G0 },
    { "pro.dur.lstm.fwd.w", "/lstm/LSTM_quant", VK_LSTM_W, 1, 0, OT_I8, 3, { 2, 640, 1024 }, G0 },
    { "pro.dur.proj.b", "/duration_proj/linear_layer/Add", VK_F32, 0, 0, OT_F32, 1, { 50 }, G0 },
    { "pro.dur.proj.w", "/duration_proj/linear_layer/MatMul", VK_MATF, 1, 0, OT_F16, 2, { 512, 50 }, G0 },
    { "pro.enc.0.ada.b", "/text_encoder/lstms.1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "pro.enc.0.ada.w", "/text_encoder/lstms.1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 1024 }, G0 },
    { "pro.enc.0.lstm.bwd.b", "/text_encoder/lstms.0/LSTM_quant", VK_LSTM_B, 3, 1, OT_F32, 2, { 2, 2048 }, G0 },
    { "pro.enc.0.lstm.bwd.r", "/text_encoder/lstms.0/LSTM_quant", VK_LSTM_R, 2, 1, OT_I8, 3, { 2, 256, 1024 }, G0 },
    { "pro.enc.0.lstm.bwd.w", "/text_encoder/lstms.0/LSTM_quant", VK_LSTM_W, 1, 1, OT_I8, 3, { 2, 640, 1024 }, G0 },
    { "pro.enc.0.lstm.fwd.b", "/text_encoder/lstms.0/LSTM_quant", VK_LSTM_B, 3, 0, OT_F32, 2, { 2, 2048 }, G0 },
    { "pro.enc.0.lstm.fwd.r", "/text_encoder/lstms.0/LSTM_quant", VK_LSTM_R, 2, 0, OT_I8, 3, { 2, 256, 1024 }, G0 },
    { "pro.enc.0.lstm.fwd.w", "/text_encoder/lstms.0/LSTM_quant", VK_LSTM_W, 1, 0, OT_I8, 3, { 2, 640, 1024 }, G0 },
    { "pro.enc.1.ada.b", "/text_encoder/lstms.3/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "pro.enc.1.ada.w", "/text_encoder/lstms.3/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 1024 }, G0 },
    { "pro.enc.1.lstm.bwd.b", "/text_encoder/lstms.2/LSTM_quant", VK_LSTM_B, 3, 1, OT_F32, 2, { 2, 2048 }, G0 },
    { "pro.enc.1.lstm.bwd.r", "/text_encoder/lstms.2/LSTM_quant", VK_LSTM_R, 2, 1, OT_I8, 3, { 2, 256, 1024 }, G0 },
    { "pro.enc.1.lstm.bwd.w", "/text_encoder/lstms.2/LSTM_quant", VK_LSTM_W, 1, 1, OT_I8, 3, { 2, 640, 1024 }, G0 },
    { "pro.enc.1.lstm.fwd.b", "/text_encoder/lstms.2/LSTM_quant", VK_LSTM_B, 3, 0, OT_F32, 2, { 2, 2048 }, G0 },
    { "pro.enc.1.lstm.fwd.r", "/text_encoder/lstms.2/LSTM_quant", VK_LSTM_R, 2, 0, OT_I8, 3, { 2, 256, 1024 }, G0 },
    { "pro.enc.1.lstm.fwd.w", "/text_encoder/lstms.2/LSTM_quant", VK_LSTM_W, 1, 0, OT_I8, 3, { 2, 640, 1024 }, G0 },
    { "pro.enc.2.ada.b", "/text_encoder/lstms.5/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "pro.enc.2.ada.w", "/text_encoder/lstms.5/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 1024 }, G0 },
    { "pro.enc.2.lstm.bwd.b", "/text_encoder/lstms.4/LSTM_quant", VK_LSTM_B, 3, 1, OT_F32, 2, { 2, 2048 }, G0 },
    { "pro.enc.2.lstm.bwd.r", "/text_encoder/lstms.4/LSTM_quant", VK_LSTM_R, 2, 1, OT_I8, 3, { 2, 256, 1024 }, G0 },
    { "pro.enc.2.lstm.bwd.w", "/text_encoder/lstms.4/LSTM_quant", VK_LSTM_W, 1, 1, OT_I8, 3, { 2, 640, 1024 }, G0 },
    { "pro.enc.2.lstm.fwd.b", "/text_encoder/lstms.4/LSTM_quant", VK_LSTM_B, 3, 0, OT_F32, 2, { 2, 2048 }, G0 },
    { "pro.enc.2.lstm.fwd.r", "/text_encoder/lstms.4/LSTM_quant", VK_LSTM_R, 2, 0, OT_I8, 3, { 2, 256, 1024 }, G0 },
    { "pro.enc.2.lstm.fwd.w", "/text_encoder/lstms.4/LSTM_quant", VK_LSTM_W, 1, 0, OT_I8, 3, { 2, 640, 1024 }, G0 },
    { "pro.f0.0.ada1.b", "/F0.0/norm1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "pro.f0.0.ada1.w", "/F0.0/norm1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 1024 }, G0 },
    { "pro.f0.0.ada2.b", "/F0.0/norm2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "pro.f0.0.ada2.w", "/F0.0/norm2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 1024 }, G0 },
    { "pro.f0.0.conv1.b", "/F0.0/conv1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "pro.f0.0.conv1.w", "/F0.0/conv1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 512, 512, 3 }, { 1, 1, 1, 1 }, 0 },
    { "pro.f0.0.conv2.b", "/F0.0/conv2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "pro.f0.0.conv2.w", "/F0.0/conv2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 512, 512, 3 }, { 1, 1, 1, 1 }, 0 },
    { "pro.f0.1.ada1.b", "/F0.1/norm1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "pro.f0.1.ada1.w", "/F0.1/norm1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 1024 }, G0 },
    { "pro.f0.1.ada2.b", "/F0.1/norm2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "pro.f0.1.ada2.w", "/F0.1/norm2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "pro.f0.1.conv1.b", "/F0.1/conv1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "pro.f0.1.conv1.w", "/F0.1/conv1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 512, 3 }, { 1, 1, 1, 1 }, 0 },
    { "pro.f0.1.conv2.b", "/F0.1/conv2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "pro.f0.1.conv2.w", "/F0.1/conv2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 1, 1, 1 }, 0 },
    { "pro.f0.1.pool.b", "/F0.1/pool/ConvTranspose", VK_F32, 2, 0, OT_F16, 1, { 512 }, G0 },
    { "pro.f0.1.pool.w", "/F0.1/pool/ConvTranspose", VK_CONVT_DW, 1, 0, OT_F16, 3, { 512, 1, 3 }, { 2, 1, 1, 1 }, 0 },
    { "pro.f0.1.skip.w", "/F0.1/conv1x1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 512, 1 }, { 1, 1, 0, 0 }, 0 },
    { "pro.f0.2.ada1.b", "/F0.2/norm1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "pro.f0.2.ada1.w", "/F0.2/norm1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "pro.f0.2.ada2.b", "/F0.2/norm2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "pro.f0.2.ada2.w", "/F0.2/norm2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "pro.f0.2.conv1.b", "/F0.2/conv1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "pro.f0.2.conv1.w", "/F0.2/conv1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 1, 1, 1 }, 0 },
    { "pro.f0.2.conv2.b", "/F0.2/conv2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "pro.f0.2.conv2.w", "/F0.2/conv2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 1, 1, 1 }, 0 },
    { "pro.f0.proj.b", "/F0_proj/Conv_output_0_Cast_to_float16_input_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 1 }, G0 },
    { "pro.f0.proj.w", "/F0_proj/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 1, 256, 1 }, { 1, 1, 0, 0 }, 0 },
    { "pro.lstm.bwd.b", "/shared/LSTM_quant", VK_LSTM_B, 3, 1, OT_F32, 2, { 2, 2048 }, G0 },
    { "pro.lstm.bwd.r", "/shared/LSTM_quant", VK_LSTM_R, 2, 1, OT_I8, 3, { 2, 256, 1024 }, G0 },
    { "pro.lstm.bwd.w", "/shared/LSTM_quant", VK_LSTM_W, 1, 1, OT_I8, 3, { 2, 640, 1024 }, G0 },
    { "pro.lstm.fwd.b", "/shared/LSTM_quant", VK_LSTM_B, 3, 0, OT_F32, 2, { 2, 2048 }, G0 },
    { "pro.lstm.fwd.r", "/shared/LSTM_quant", VK_LSTM_R, 2, 0, OT_I8, 3, { 2, 256, 1024 }, G0 },
    { "pro.lstm.fwd.w", "/shared/LSTM_quant", VK_LSTM_W, 1, 0, OT_I8, 3, { 2, 640, 1024 }, G0 },
    { "pro.n.0.ada1.b", "/N.0/norm1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "pro.n.0.ada1.w", "/N.0/norm1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 1024 }, G0 },
    { "pro.n.0.ada2.b", "/N.0/norm2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "pro.n.0.ada2.w", "/N.0/norm2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 1024 }, G0 },
    { "pro.n.0.conv1.b", "/N.0/conv1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "pro.n.0.conv1.w", "/N.0/conv1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 512, 512, 3 }, { 1, 1, 1, 1 }, 0 },
    { "pro.n.0.conv2.b", "/N.0/conv2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "pro.n.0.conv2.w", "/N.0/conv2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 512, 512, 3 }, { 1, 1, 1, 1 }, 0 },
    { "pro.n.1.ada1.b", "/N.1/norm1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 1024 }, G0 },
    { "pro.n.1.ada1.w", "/N.1/norm1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 1024 }, G0 },
    { "pro.n.1.ada2.b", "/N.1/norm2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "pro.n.1.ada2.w", "/N.1/norm2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "pro.n.1.conv1.b", "/N.1/conv1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "pro.n.1.conv1.w", "/N.1/conv1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 512, 3 }, { 1, 1, 1, 1 }, 0 },
    { "pro.n.1.conv2.b", "/N.1/conv2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "pro.n.1.conv2.w", "/N.1/conv2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 1, 1, 1 }, 0 },
    { "pro.n.1.pool.b", "/N.1/pool/ConvTranspose", VK_F32, 2, 0, OT_F16, 1, { 512 }, G0 },
    { "pro.n.1.pool.w", "/N.1/pool/ConvTranspose", VK_CONVT_DW, 1, 0, OT_F16, 3, { 512, 1, 3 }, { 2, 1, 1, 1 }, 0 },
    { "pro.n.1.skip.w", "/N.1/conv1x1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 512, 1 }, { 1, 1, 0, 0 }, 0 },
    { "pro.n.2.ada1.b", "/N.2/norm1/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "pro.n.2.ada1.w", "/N.2/norm1/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "pro.n.2.ada2.b", "/N.2/norm2/fc/Gemm_Add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "pro.n.2.ada2.w", "/N.2/norm2/fc/Gemm_MatMul_quant", VK_QMM, 1, 0, OT_I8, 2, { 128, 512 }, G0 },
    { "pro.n.2.conv1.b", "/N.2/conv1/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "pro.n.2.conv1.w", "/N.2/conv1/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 1, 1, 1 }, 0 },
    { "pro.n.2.conv2.b", "/N.2/conv2/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 256 }, G0 },
    { "pro.n.2.conv2.w", "/N.2/conv2/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 256, 256, 3 }, { 1, 1, 1, 1 }, 0 },
    { "pro.n.proj.b", "/N_proj/Conv_output_0_Cast_to_float16_input_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 1 }, G0 },
    { "pro.n.proj.w", "/N_proj/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 1, 256, 1 }, { 1, 1, 0, 0 }, 0 },
    { "txt.cnn.0.conv.b", "/text_encoder/cnn.0/cnn.0.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "txt.cnn.0.conv.w", "/text_encoder/cnn.0/cnn.0.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 512, 512, 5 }, { 1, 1, 2, 2 }, 0 },
    { "txt.cnn.0.ln.b", "/text_encoder/cnn.0/cnn.0.1/LayerNormalization", VK_F32, 2, 0, OT_F32, 1, { 512 }, G0 },
    { "txt.cnn.0.ln.eps", "/text_encoder/cnn.0/cnn.0.1/LayerNormalization", VK_EPS, 0, 0, OT_F32, 1, { 1 }, G0 },
    { "txt.cnn.0.ln.g", "/text_encoder/cnn.0/cnn.0.1/LayerNormalization", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "txt.cnn.1.conv.b", "/text_encoder/cnn.1/cnn.1.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "txt.cnn.1.conv.w", "/text_encoder/cnn.1/cnn.1.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 512, 512, 5 }, { 1, 1, 2, 2 }, 0 },
    { "txt.cnn.1.ln.b", "/text_encoder/cnn.1/cnn.1.1/LayerNormalization", VK_F32, 2, 0, OT_F32, 1, { 512 }, G0 },
    { "txt.cnn.1.ln.eps", "/text_encoder/cnn.1/cnn.1.1/LayerNormalization", VK_EPS, 0, 0, OT_F32, 1, { 1 }, G0 },
    { "txt.cnn.1.ln.g", "/text_encoder/cnn.1/cnn.1.1/LayerNormalization", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "txt.cnn.2.conv.b", "/text_encoder/cnn.2/cnn.2.0/Conv_output_0_bias_add", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "txt.cnn.2.conv.w", "/text_encoder/cnn.2/cnn.2.0/Conv_quant", VK_QCONV, 1, 0, OT_U8, 3, { 512, 512, 5 }, { 1, 1, 2, 2 }, 0 },
    { "txt.cnn.2.ln.b", "/text_encoder/cnn.2/cnn.2.1/LayerNormalization", VK_F32, 2, 0, OT_F32, 1, { 512 }, G0 },
    { "txt.cnn.2.ln.eps", "/text_encoder/cnn.2/cnn.2.1/LayerNormalization", VK_EPS, 0, 0, OT_F32, 1, { 1 }, G0 },
    { "txt.cnn.2.ln.g", "/text_encoder/cnn.2/cnn.2.1/LayerNormalization", VK_F32, 1, 0, OT_F32, 1, { 512 }, G0 },
    { "txt.emb", "/text_encoder/embedding/Gather", VK_F32, 0, 0, OT_F32, 2, { 178, 512 }, G0 },
    { "txt.lstm.bwd.b", "/text_encoder/lstm/LSTM_quant", VK_LSTM_B, 3, 1, OT_F32, 2, { 2, 2048 }, G0 },
    { "txt.lstm.bwd.r", "/text_encoder/lstm/LSTM_quant", VK_LSTM_R, 2, 1, OT_I8, 3, { 2, 256, 1024 }, G0 },
    { "txt.lstm.bwd.w", "/text_encoder/lstm/LSTM_quant", VK_LSTM_W, 1, 1, OT_I8, 3, { 2, 512, 1024 }, G0 },
    { "txt.lstm.fwd.b", "/text_encoder/lstm/LSTM_quant", VK_LSTM_B, 3, 0, OT_F32, 2, { 2, 2048 }, G0 },
    { "txt.lstm.fwd.r", "/text_encoder/lstm/LSTM_quant", VK_LSTM_R, 2, 0, OT_I8, 3, { 2, 256, 1024 }, G0 },
    { "txt.lstm.fwd.w", "/text_encoder/lstm/LSTM_quant", VK_LSTM_W, 1, 0, OT_I8, 3, { 2, 512, 1024 }, G0 },
    /* Checked, not stored: norms without an affine part (scale 1, bias 0, eps norm.eps). */
    { NULL, "/F0.0/norm2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 512 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, "/F0.1/norm1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 512 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, "/F0.1/norm2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, "/F0.2/norm1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, "/F0.2/norm2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, "/N.0/norm1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 512 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, "/N.0/norm2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 512 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, "/N.1/norm1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 512 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, "/N.1/norm2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, "/N.2/norm1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, "/N.2/norm2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, DEC "decode.0/norm1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 1090 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, DEC "decode.0/norm2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 1024 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, DEC "decode.1/norm1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 1090 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, DEC "decode.1/norm2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 1024 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, DEC "decode.2/norm1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 1090 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, DEC "decode.2/norm2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 1024 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, DEC "decode.3/norm1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 1090 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, DEC "decode.3/norm2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 512 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, DEC "encode/norm1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 514 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, DEC "encode/norm2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 1024 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "noise_res.0/adain1.0/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "noise_res.0/adain1.1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "noise_res.0/adain1.2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "noise_res.0/adain2.0/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "noise_res.0/adain2.1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "noise_res.0/adain2.2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "noise_res.1/adain1.0/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "noise_res.1/adain1.1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "noise_res.1/adain1.2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "noise_res.1/adain2.0/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "noise_res.1/adain2.1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "noise_res.1/adain2.2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.0/adain1.1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.0/adain1.2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.0/adain2.0/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.0/adain2.1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.0/adain2.2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.1/adain1.0/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.1/adain1.1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.1/adain1.2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.1/adain2.0/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.1/adain2.1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.1/adain2.2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 256 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.2/adain1.1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.2/adain1.2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.2/adain2.0/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.2/adain2.1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.2/adain2.2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.3/adain1.0/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.3/adain1.1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.3/adain1.2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.3/adain2.0/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.3/adain2.1/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, GEN "resblocks.3/adain2.2/norm/InstanceNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 128 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, "/text_encoder/lstms.1/LayerNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 512 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, "/text_encoder/lstms.3/LayerNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 512 }, { 0, 0, 0, 0 }, 1e-05f },
    { NULL, "/text_encoder/lstms.5/LayerNormalization", VK_NORM1, 1, 0, OT_F32, 1, { 512 }, { 0, 0, 0, 0 }, 1e-05f },
};

static const char *const VOICES[] = {
    "expr-voice-2-m", "expr-voice-2-f", "expr-voice-3-m", "expr-voice-3-f",
    "expr-voice-4-m", "expr-voice-4-f", "expr-voice-5-m", "expr-voice-5-f",
};

const oc_voice_spec OC_VOICE_SPEC = {
    ROWS, sizeof ROWS / sizeof ROWS[0], VOICES, sizeof VOICES / sizeof VOICES[0], 400, 256,
};

/* ---- protobuf ------------------------------------------------------------- */

typedef struct { const uint8_t *p; size_t n; } span;

static int span_is(span s, const char *z) { size_t l = strlen(z); return s.n == l && memcmp(s.p, z, l) == 0; }

static int span_cmp(span a, span b) {
    int c = memcmp(a.p, b.p, a.n < b.n ? a.n : b.n);
    return c ? c : (a.n > b.n) - (a.n < b.n);
}

static int pb_varint(const uint8_t **p, const uint8_t *end, uint64_t *v) {
    uint64_t r = 0;
    for (int s = 0; s < 64 && *p < end; s += 7) {
        uint8_t c = *(*p)++;
        r |= (uint64_t)(c & 0x7f) << s;
        if (c < 0x80) { *v = r; return 0; }
    }
    return -1;
}

/* The next field: 1 with its number and value (a varint in *v, bytes in *s), 0
 * at the end, -1 on malformed data. */
static int pb_next(const uint8_t **p, const uint8_t *end, uint32_t *field, int *wt, uint64_t *v, span *s) {
    if (*p >= end) return 0;
    uint64_t key, len;
    if (pb_varint(p, end, &key) != 0) return -1;
    *field = (uint32_t)(key >> 3);
    *wt = (int)(key & 7);
    s->p = *p;
    switch (*wt) {
    case 0: return pb_varint(p, end, v) == 0 ? 1 : -1;
    case 1: s->n = 8; break;
    case 5: s->n = 4; break;
    case 2:
        if (pb_varint(p, end, &len) != 0 || len > (uint64_t)(end - *p)) return -1;
        s->p = *p;
        s->n = (size_t)len;
        break;
    default: return -1;
    }
    if (s->n > (size_t)(end - *p)) return -1;
    *p += s->n;
    return 1;
}

static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

/* ---- the .onnx graph -------------------------------------------------------- */

#define MAX_IN  16
#define MAX_OUT 8
#define MAX_DIM 8

typedef struct {
    span    name, op, body;
    span    in[MAX_IN], out[MAX_OUT];
    int     nin, nout;
} gnode;

typedef struct {
    span    name, body, raw;
    int     dtype, ndim, external;
    int64_t dims[MAX_DIM];
    int     used;               /* taken by a row, or a quantised product's parameter */
    int     weight;             /* must be taken: see unexpected() */
    int     plain_use;          /* consumed other than as a Resize's scales */
} ginit;

typedef struct { span name; uint32_t idx; } gkey;

typedef struct {
    gnode   *nodes;
    ginit   *inits;
    size_t   nn, ni;
    gkey    *by_node, *by_out, *by_init;
    size_t   nout;
} graph;

static void graph_free(graph *g) {
    free(g->nodes);
    free(g->inits);
    free(g->by_node);
    free(g->by_out);
    free(g->by_init);
    memset(g, 0, sizeof *g);
}

static int parse_node(span b, gnode *n) {
    memset(n, 0, sizeof *n);
    n->body = b;
    const uint8_t *p = b.p, *end = b.p + b.n;
    uint32_t f;
    int wt, r;
    uint64_t v;
    span s;
    while ((r = pb_next(&p, end, &f, &wt, &v, &s)) == 1) {
        if (wt != 2) continue;
        if (f == 1) { if (n->nin < MAX_IN) n->in[n->nin] = s; n->nin++; }
        else if (f == 2) { if (n->nout < MAX_OUT) n->out[n->nout++] = s; }
        else if (f == 3) n->name = s;
        else if (f == 4) n->op = s;
    }
    if (n->nin > MAX_IN) n->nin = MAX_IN;     /* inputs past these are never read */
    return r;
}

static int parse_tensor(span b, ginit *t) {
    memset(t, 0, sizeof *t);
    t->body = b;
    const uint8_t *p = b.p, *end = b.p + b.n;
    uint32_t f;
    int wt, r;
    uint64_t v;
    span s;
    while ((r = pb_next(&p, end, &f, &wt, &v, &s)) == 1) {
        if (f == 1 && wt == 0) {
            if (t->ndim >= MAX_DIM) return -1;
            t->dims[t->ndim++] = (int64_t)v;
        } else if (f == 1 && wt == 2) {
            const uint8_t *q = s.p, *qe = s.p + s.n;
            while (q < qe) {
                if (t->ndim >= MAX_DIM || pb_varint(&q, qe, &v) != 0) return -1;
                t->dims[t->ndim++] = (int64_t)v;
            }
        } else if (f == 2 && wt == 0) t->dtype = (int)v;
        else if (f == 8 && wt == 2) t->name = s;
        else if (f == 9 && wt == 2) t->raw = s;
        else if ((f == 13 && wt == 2) || (f == 14 && wt == 0 && v == 1)) t->external = 1;
    }
    return r;
}

static int key_cmp(const void *a, const void *b) {
    const gkey *x = a, *y = b;
    int c = span_cmp(x->name, y->name);
    return c ? c : (x->idx > y->idx) - (x->idx < y->idx);
}

static const gkey *key_find(const gkey *k, size_t n, span name) {
    size_t lo = 0, hi = n;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int c = span_cmp(name, k[mid].name);
        if (c == 0) {
            while (mid > 0 && span_cmp(name, k[mid - 1].name) == 0) mid--;   /* the first */
            return &k[mid];
        }
        if (c < 0) hi = mid; else lo = mid + 1;
    }
    return NULL;
}

static span zspan(const char *z) { span s = { (const uint8_t *)z, strlen(z) }; return s; }

static gnode *node_named(const graph *g, const char *name) {
    const gkey *k = key_find(g->by_node, g->nn, zspan(name));
    return k ? &g->nodes[k->idx] : NULL;
}

static ginit *init_named(const graph *g, span name) {
    const gkey *k = key_find(g->by_init, g->ni, name);
    return k ? &g->inits[k->idx] : NULL;
}

static int graph_parse(const uint8_t *p, size_t n, graph *g, char *err, size_t cap) {
    memset(g, 0, sizeof *g);
    const uint8_t *end = p + n, *gp = NULL;
    size_t gn = 0;
    uint32_t f;
    int wt, r;
    uint64_t v;
    span s;
    while ((r = pb_next(&p, end, &f, &wt, &v, &s)) == 1)
        if (f == 7 && wt == 2) { gp = s.p; gn = s.n; }
    if (r < 0 || !gp) { seterr(err, cap, "the model is not an .onnx file"); return -1; }
    /* Two passes over the graph: count, then fill. */
    for (int pass = 0; pass < 2; pass++) {
        const uint8_t *q = gp, *qe = gp + gn;
        size_t nn = 0, ni = 0;
        while ((r = pb_next(&q, qe, &f, &wt, &v, &s)) == 1) {
            if (wt != 2) continue;
            if (f == 1) {
                if (pass && parse_node(s, &g->nodes[nn]) < 0) r = -1;
                nn++;
            } else if (f == 5) {
                if (pass && parse_tensor(s, &g->inits[ni]) < 0) r = -1;
                ni++;
            }
            if (r < 0) break;
        }
        if (r < 0) { seterr(err, cap, "the model's graph is malformed"); graph_free(g); return -1; }
        if (!pass) {
            g->nodes = calloc(nn ? nn : 1, sizeof *g->nodes);
            g->inits = calloc(ni ? ni : 1, sizeof *g->inits);
            g->nn = nn;
            g->ni = ni;
            if (!g->nodes || !g->inits) { seterr(err, cap, "out of memory"); graph_free(g); return -1; }
        }
    }
    size_t nout = 0;
    for (size_t i = 0; i < g->nn; i++) nout += (size_t)g->nodes[i].nout;
    g->by_node = calloc(g->nn ? g->nn : 1, sizeof *g->by_node);
    g->by_out = calloc(nout ? nout : 1, sizeof *g->by_out);
    g->by_init = calloc(g->ni ? g->ni : 1, sizeof *g->by_init);
    if (!g->by_node || !g->by_out || !g->by_init) { seterr(err, cap, "out of memory"); graph_free(g); return -1; }
    for (size_t i = 0; i < g->nn; i++) {
        g->by_node[i].name = g->nodes[i].name;
        g->by_node[i].idx = (uint32_t)i;
        for (int o = 0; o < g->nodes[i].nout; o++) {
            g->by_out[g->nout].name = g->nodes[i].out[o];
            g->by_out[g->nout++].idx = (uint32_t)i;
        }
    }
    for (size_t i = 0; i < g->ni; i++) {
        g->by_init[i].name = g->inits[i].name;
        g->by_init[i].idx = (uint32_t)i;
    }
    qsort(g->by_node, g->nn, sizeof *g->by_node, key_cmp);
    qsort(g->by_out, g->nout, sizeof *g->by_out, key_cmp);
    qsort(g->by_init, g->ni, sizeof *g->by_init, key_cmp);
    /* What each constant is used as, for unexpected(). */
    for (size_t i = 0; i < g->nn; i++) {
        const gnode *nd = &g->nodes[i];
        int scales_mul = nd->name.n > 11 && memcmp(nd->name.p + nd->name.n - 11, "_scales_mul", 11) == 0;
        for (int k = 0; k < nd->nin; k++) {
            ginit *t = nd->in[k].n ? init_named(g, nd->in[k]) : NULL;
            if (!t) continue;
            if (scales_mul) t->weight = 1;
            if (!(span_is(nd->op, "Resize") && k == 2)) t->plain_use = 1;
        }
    }
    return 0;
}

/* The constant behind a value: itself, or through Reshape, Cast and Identity. */
static ginit *resolve(const graph *g, span value) {
    for (int hop = 0; hop < 8 && value.n; hop++) {
        ginit *t = init_named(g, value);
        if (t) return t;
        const gkey *k = key_find(g->by_out, g->nout, value);
        if (!k) return NULL;
        const gnode *pn = &g->nodes[k->idx];
        if (!(span_is(pn->op, "Reshape") || span_is(pn->op, "Cast") || span_is(pn->op, "Identity")) || pn->nin < 1)
            return NULL;
        value = pn->in[0];
    }
    return NULL;
}

static ginit *input_const(const graph *g, const gnode *n, int k) {
    return k < n->nin && n->in[k].n ? resolve(g, n->in[k]) : NULL;
}

/* ---- constants' values ----------------------------------------------------- */

static size_t init_count(const ginit *t) {
    size_t c = 1;
    for (int i = 0; i < t->ndim; i++) {
        if (t->dims[i] < 0 || (t->dims[i] && c > ((size_t)1 << 40) / (size_t)t->dims[i])) return (size_t)-1;
        c *= (size_t)t->dims[i];
    }
    return c;
}

static float half_to_float(uint16_t h) {
    uint32_t sign = (uint32_t)(h & 0x8000) << 16, exp = (h >> 10) & 0x1f, man = h & 0x3ff, bits;
    if (exp == 0x1f) bits = sign | 0x7f800000u | man << 13;
    else if (exp) bits = sign | (exp + 112) << 23 | man << 13;
    else if (!man) bits = sign;
    else {
        exp = 113;
        while (!(man & 0x400)) { man <<= 1; exp--; }
        bits = sign | exp << 23 | (man & 0x3ff) << 13;
    }
    float f;
    memcpy(&f, &bits, sizeof f);
    return f;
}

/* The typed-data field a tensor without raw_data keeps its values in: floats
 * (field 4) for f32, int32s (field 5) for the 8- and 16-bit types. */
static int typed_values(const ginit *t, size_t n, float *fv, int32_t *iv) {
    const uint8_t *p = t->body.p, *end = t->body.p + t->body.n;
    uint32_t f;
    int wt, r;
    uint64_t v;
    span s;
    size_t got = 0;
    uint32_t want = t->dtype == OT_F32 ? 4 : 5;
    while ((r = pb_next(&p, end, &f, &wt, &v, &s)) == 1) {
        if (f != want) continue;
        if (want == 4 && (wt == 5 || wt == 2)) {
            for (size_t i = 0; i + 4 <= s.n; i += 4, got++) {
                if (got >= n) return -1;
                uint32_t b = le32(s.p + i);
                memcpy(&fv[got], &b, 4);
            }
        } else if (want == 5 && (wt == 0 || wt == 2)) {
            const uint8_t *q = s.p, *qe = s.p + s.n;
            if (wt == 0) { if (got >= n) return -1; iv[got++] = (int32_t)(uint32_t)v; continue; }
            while (q < qe) {
                if (got >= n || pb_varint(&q, qe, &v) != 0) return -1;
                iv[got++] = (int32_t)(uint32_t)v;
            }
        }
    }
    return r < 0 || got != n ? -1 : 0;
}

/* A float or half constant's n values as f32. */
static int init_floats(const ginit *t, float *out, size_t n) {
    if (t->dtype != OT_F32 && t->dtype != OT_F16) return -1;
    size_t es = t->dtype == OT_F32 ? 4 : 2;
    if (t->raw.p) {
        if (t->raw.n != n * es) return -1;
        for (size_t i = 0; i < n; i++) {
            if (es == 4) {
                uint32_t b = le32(t->raw.p + 4 * i);
                memcpy(&out[i], &b, 4);
            } else out[i] = half_to_float(le16(t->raw.p + 2 * i));
        }
        return 0;
    }
    if (t->dtype == OT_F32) return typed_values(t, n, out, NULL);
    int32_t *iv = malloc((n ? n : 1) * sizeof *iv);
    if (!iv) return -1;
    int rc = typed_values(t, n, NULL, iv);
    for (size_t i = 0; rc == 0 && i < n; i++) out[i] = half_to_float((uint16_t)iv[i]);
    free(iv);
    return rc;
}

/* An int8 or uint8 constant's n values. */
static int init_ints(const ginit *t, int32_t *out, size_t n) {
    if (t->dtype != OT_I8 && t->dtype != OT_U8) return -1;
    if (t->raw.p) {
        if (t->raw.n != n) return -1;
        for (size_t i = 0; i < n; i++) out[i] = t->dtype == OT_I8 ? (int8_t)t->raw.p[i] : t->raw.p[i];
        return 0;
    }
    if (typed_values(t, n, NULL, out) != 0) return -1;
    for (size_t i = 0; i < n; i++) out[i] = t->dtype == OT_I8 ? (int8_t)out[i] : (uint8_t)out[i];
    return 0;
}

/* ---- attributes ------------------------------------------------------------ */

typedef struct { int found; int64_t i; float f; span s; int64_t ints[8]; int nints; } gattr;

static int node_attr(const gnode *n, const char *name, gattr *a) {
    memset(a, 0, sizeof *a);
    const uint8_t *p = n->body.p, *end = n->body.p + n->body.n;
    uint32_t f;
    int wt;
    uint64_t v;
    span s;
    while (pb_next(&p, end, &f, &wt, &v, &s) == 1) {
        if (f != 5 || wt != 2) continue;
        const uint8_t *q = s.p, *qe = s.p + s.n;
        span as;
        int is = 0;
        gattr t;
        memset(&t, 0, sizeof t);
        while (pb_next(&q, qe, &f, &wt, &v, &as) == 1) {
            if (f == 1 && wt == 2) is = span_is(as, name);
            else if (f == 2 && wt == 5) { uint32_t b = le32(as.p); memcpy(&t.f, &b, 4); }
            else if (f == 3 && wt == 0) t.i = (int64_t)v;
            else if (f == 4 && wt == 2) t.s = as;
            else if (f == 8 && wt == 0) { if (t.nints < 8) t.ints[t.nints++] = (int64_t)v; }
            else if (f == 8 && wt == 2) {
                const uint8_t *r = as.p, *re = as.p + as.n;
                while (r < re && pb_varint(&r, re, &v) == 0) if (t.nints < 8) t.ints[t.nints++] = (int64_t)v;
            }
        }
        if (is) {
            *a = t;
            a->found = 1;
            return 1;
        }
    }
    return 0;
}

static int64_t attr_int(const gnode *n, const char *name, int64_t dflt) {
    gattr a;
    return node_attr(n, name, &a) ? a.i : dflt;
}

/* A one-dimensional list attribute's element `i`, or `dflt` if it is absent;
 * -1000 if it is there with another length than `len`. */
static int64_t attr_at(const gnode *n, const char *name, int len, int i, int64_t dflt) {
    gattr a;
    if (!node_attr(n, name, &a)) return dflt;
    return a.nints == len ? a.ints[i] : -1000;
}

/* ---- checking a row -------------------------------------------------------- */

static const char *type_name(int t) {
    return t == OT_F32 ? "float" : t == OT_F16 ? "half" : t == OT_I8 ? "int8" : t == OT_U8 ? "uint8" : "other";
}

static void shape_text(char *b, size_t cap, int dtype, int ndim, const int64_t *d) {
    int k = snprintf(b, cap, "%s [", type_name(dtype));
    for (int i = 0; i < ndim && k > 0 && (size_t)k < cap; i++) k += snprintf(b + k, cap - (size_t)k, i ? ", %lld" : "%lld", (long long)d[i]);
    if (k > 0 && (size_t)k < cap) snprintf(b + k, cap - (size_t)k, "]");
}

static const char *row_label(const oc_voice_row *r) { return r->name ? r->name : "a norm"; }

/* The constant at row r's input of its node, of the row's type and shape. */
static ginit *row_source(const graph *g, const oc_voice_row *r, const gnode **np, char *err, size_t cap) {
    const gnode *n = node_named(g, r->node);
    if (!n) { seterr(err, cap, "%s: the model has no node %s", row_label(r), r->node); return NULL; }
    *np = n;
    if (r->kind == VK_EPS) return NULL;
    ginit *t = input_const(g, n, r->input);
    if (!t) { seterr(err, cap, "%s: input %d of %s is not a constant", row_label(r), r->input, r->node); return NULL; }
    int64_t want[4];
    for (int i = 0; i < 4; i++) want[i] = r->dims[i];
    int same = t->dtype == r->dtype && t->ndim == r->ndim && !t->external;
    for (int i = 0; same && i < r->ndim; i++) same = t->dims[i] == want[i];
    if (!same) {
        char a[96], b[96];
        shape_text(a, sizeof a, r->dtype, r->ndim, want);
        shape_text(b, sizeof b, t->dtype, t->ndim, t->dims);
        seterr(err, cap, "%s: input %d of %s is %s%s, not %s", row_label(r), r->input, r->node, b,
               t->external ? " stored outside the file" : "", a);
        return NULL;
    }
    t->used = 1;
    return t;
}

/* A scalar parameter of a quantised product: its value, through `iv` or `fv`. */
static int quant_scalar(const graph *g, const oc_voice_row *r, const gnode *n, int k, int dtype, int32_t *iv,
                        float *fv, char *err, size_t cap) {
    ginit *t = input_const(g, n, k);
    if (!t || t->dtype != dtype || init_count(t) != 1 ||
        (dtype == OT_F32 ? init_floats(t, fv, 1) : init_ints(t, iv, 1)) != 0) {
        seterr(err, cap, "%s: a quantisation parameter of %s (input %d of %.*s) is not a single %s", row_label(r),
               r->node, k, (int)n->name.n, (const char *)n->name.p, type_name(dtype));
        return -1;
    }
    t->used = 1;
    return 0;
}

/* The operator settings a row expects (geom), on a Conv, ConvInteger or
 * ConvTranspose. `groups` is the group count a depthwise one must have. */
static int check_geom(const oc_voice_row *r, const gnode *n, int64_t k, int64_t groups, char *err, size_t cap) {
    if (!r->geom[0]) return 0;
    int ok;
    if (span_is(n->op, "ConvTranspose")) {
        ok = attr_at(n, "strides", 1, 0, 1) == r->geom[0] && attr_at(n, "pads", 2, 0, 0) == r->geom[1] &&
             attr_at(n, "pads", 2, 1, 0) == r->geom[2] && attr_at(n, "output_padding", 1, 0, 0) == r->geom[3] &&
             attr_at(n, "dilations", 1, 0, 1) == 1;
    } else if (span_is(n->op, "Conv") || span_is(n->op, "ConvInteger")) {
        ok = attr_at(n, "strides", 1, 0, 1) == r->geom[0] && attr_at(n, "dilations", 1, 0, 1) == r->geom[1] &&
             attr_at(n, "pads", 2, 0, 0) == r->geom[2] && attr_at(n, "pads", 2, 1, 0) == r->geom[3];
    } else ok = 0;
    gattr a;
    if (ok && node_attr(n, "auto_pad", &a) && !span_is(a.s, "NOTSET")) ok = 0;
    if (ok && attr_at(n, "kernel_shape", 1, 0, k) != k) ok = 0;
    if (ok && attr_int(n, "group", 1) != groups) ok = 0;
    if (!ok) { seterr(err, cap, "%s: %s has other settings than the forward pass assumes", row_label(r), r->node); return -1; }
    return 0;
}

/* ---- the output's tensors ------------------------------------------------- */

typedef struct {
    const char         *name;
    const oc_voice_row *row;          /* NULL: a voice */
    const gnode        *node;
    ginit              *src;
    const uint8_t      *voice;        /* a style table's f32 bytes */
    int                 dtype, ndim, dims[4];
    float               scale, eps;
    int                 zp;
    uint64_t            bytes, off;
} entry;

static int r4(int n) { return (n + 3) / 4 * 4; }
static int r8(int n) { return (n + 7) / 8 * 8; }
static int r16(int n) { return (n + 15) / 16 * 16; }

static void set_dims(entry *e, int dtype, int nd, int a, int b, int c, int d) {
    e->dtype = dtype;
    e->ndim = nd;
    e->dims[0] = a; e->dims[1] = b; e->dims[2] = c; e->dims[3] = d;
    uint64_t n = 1;
    for (int i = 0; i < nd; i++) n *= (uint64_t)e->dims[i];
    e->bytes = n * (dtype == VOICE_F32 ? 4 : 1);
}

/* Check row r against the graph and work out what it stores. 0, or -1 with the
 * reason. A VK_NORM1 row stores nothing (e->name NULL). */
static int plan_row(const graph *g, const oc_voice_row *r, entry *e, char *err, size_t cap) {
    memset(e, 0, sizeof *e);
    e->row = r;
    e->name = r->name;
    const gnode *n = NULL;
    ginit *t = row_source(g, r, &n, err, cap);
    if (!n || (!t && r->kind != VK_EPS)) return -1;
    e->node = n;
    e->src = t;
    const int *d = r->dims;
    switch (r->kind) {
    case VK_F32: {
        if (r->dtype != OT_F32 && r->dtype != OT_F16) break;
        if (check_geom(r, n, r->ndim == 3 ? d[2] : 0, 1, err, cap) != 0) return -1;
        int kept[4], nk = 0;
        for (int i = 0; i < r->ndim; i++) if (d[i] != 1) kept[nk++] = d[i];
        if (!nk) kept[nk++] = 1;
        set_dims(e, VOICE_F32, nk, kept[0], nk > 1 ? kept[1] : 0, nk > 2 ? kept[2] : 0, nk > 3 ? kept[3] : 0);
        return 0;
    }
    case VK_EPS: {
        gattr a;
        e->eps = node_attr(n, "epsilon", &a) ? a.f : 1e-5f;
        set_dims(e, VOICE_F32, 1, 1, 0, 0, 0);
        return 0;
    }
    case VK_QMM: case VK_QCONV: {
        int conv = r->kind == VK_QCONV;
        if (!span_is(n->op, conv ? "ConvInteger" : "MatMulInteger") || r->ndim != (conv ? 3 : 2) ||
            r->dtype != (conv ? OT_U8 : OT_I8))
            break;
        int32_t zp;
        if (quant_scalar(g, r, n, 3, r->dtype, &zp, NULL, err, cap) != 0) return -1;
        char sm[256];
        snprintf(sm, sizeof sm, "%s_scales_mul", r->node);
        const gnode *s = node_named(g, sm);
        if (!s) { seterr(err, cap, "%s: the model has no node %s", r->name, sm); return -1; }
        if (quant_scalar(g, r, s, 1, OT_F32, NULL, &e->scale, err, cap) != 0) return -1;
        e->zp = zp;
        if (conv) {
            if (check_geom(r, n, d[2], 1, err, cap) != 0) return -1;
            set_dims(e, VOICE_U8, 3, r4(d[0]), d[2], r16(d[1]), 0);
        } else set_dims(e, VOICE_I8, 2, r4(d[1]), r16(d[0]), 0, 0);
        return 0;
    }
    case VK_LSTM_W: case VK_LSTM_R: case VK_LSTM_B: {
        if (!span_is(n->op, "DynamicQuantizeLSTM") || r->dir > 1) break;
        gattr dir;
        if (!node_attr(n, "direction", &dir) || !span_is(dir.s, "bidirectional")) break;
        int64_t H = attr_int(n, "hidden_size", 0);
        if (H <= 0 || H % 16 || attr_int(n, "input_forget", 0) != 0 || node_attr(n, "activations", &dir)) break;
        if (r->kind == VK_LSTM_B) {
            if (r->ndim != 2 || d[0] != 2 || d[1] != 8 * H || r->dtype != OT_F32) break;
            set_dims(e, VOICE_F32, 1, (int)(8 * H), 0, 0, 0);
            return 0;
        }
        int w = r->kind == VK_LSTM_W;
        if (r->ndim != 3 || d[0] != 2 || d[2] != 4 * H || (!w && d[1] != H) || r->dtype != OT_I8) break;
        /* Per direction: W's scale and zero point are inputs 8 and 9, R's 10 and 11. */
        ginit *sc = input_const(g, n, w ? 8 : 10), *zt = input_const(g, n, w ? 9 : 11);
        float sv[2];
        int32_t zv[2];
        if (!sc || !zt || sc->dtype != OT_F32 || zt->dtype != OT_I8 || init_count(sc) != 2 || init_count(zt) != 2 ||
            init_floats(sc, sv, 2) != 0 || init_ints(zt, zv, 2) != 0) {
            seterr(err, cap, "%s: %s has no per-direction scales and zero points", r->name, r->node);
            return -1;
        }
        sc->used = zt->used = 1;
        e->scale = sv[r->dir];
        e->zp = zv[r->dir];
        set_dims(e, VOICE_I8, 2, (int)(4 * H), w ? r16(d[1]) : (int)H, 0, 0);
        return 0;
    }
    case VK_MATF:
        if (r->ndim != 2 || !span_is(n->op, "MatMul")) break;
        set_dims(e, VOICE_F32, 2, d[0], r8(d[1]), 0, 0);
        return 0;
    case VK_CONVF:
        if (r->ndim != 3 || d[1] != 1 || !span_is(n->op, "Conv")) break;
        if (check_geom(r, n, d[2], 1, err, cap) != 0) return -1;
        set_dims(e, VOICE_F32, 2, d[2], r8(d[0]), 0, 0);
        return 0;
    case VK_CONVT:
        if (r->ndim != 3 || !r->geom[0] || d[2] % r->geom[0] || !span_is(n->op, "ConvTranspose")) break;
        if (check_geom(r, n, d[2], 1, err, cap) != 0) return -1;
        set_dims(e, VOICE_F32, 4, r->geom[0], d[2] / r->geom[0], d[0], r8(d[1]));
        return 0;
    case VK_CONVT_DW:
        if (r->ndim != 3 || d[1] != 1 || !span_is(n->op, "ConvTranspose")) break;
        if (check_geom(r, n, d[2], d[0], err, cap) != 0) return -1;
        set_dims(e, VOICE_F32, 2, d[2], d[0], 0, 0);
        return 0;
    case VK_NORM1: {
        ginit *b = input_const(g, n, 2);
        size_t c = init_count(t);
        if (!b || b->dtype != t->dtype || init_count(b) != c || r->dtype != OT_F32) break;
        float *v = malloc(2 * c * sizeof *v);
        int unit = v && init_floats(t, v, c) == 0 && init_floats(b, v + c, c) == 0;
        for (size_t i = 0; unit && i < c; i++) unit = v[i] == 1.0f && v[c + i] == 0.0f;
        free(v);
        gattr a;
        float eps = node_attr(n, "epsilon", &a) ? a.f : 1e-5f;
        if (!unit || eps != r->eps) {
            seterr(err, cap, "the norm %s has a scale, bias or epsilon the forward pass does not apply", r->node);
            return -1;
        }
        b->used = 1;
        e->name = NULL;
        return 0;
    }
    }
    seterr(err, cap, "%s: %s is not the operator the forward pass expects", row_label(r), r->node);
    return -1;
}

/* The tensor's bytes, laid out (voice_pack.h, VK_*), into out (e->bytes). */
static int fill(const entry *e, uint8_t *out) {
    const oc_voice_row *r = e->row;
    if (!r) { memcpy(out, e->voice, (size_t)e->bytes); return 0; }
    if (r->kind == VK_EPS) { memcpy(out, &e->eps, 4); return 0; }
    size_t n = init_count(e->src);
    const int *d = r->dims;
    if (r->kind == VK_QMM || r->kind == VK_QCONV || r->kind == VK_LSTM_W || r->kind == VK_LSTM_R) {
        int32_t *v = malloc(n * sizeof *v);
        if (!v || init_ints(e->src, v, n) != 0) { free(v); return -1; }
        memset(out, r->kind == VK_QCONV ? (uint8_t)e->zp : (uint8_t)(int8_t)e->zp, (size_t)e->bytes);
        if (r->kind == VK_QMM) {                        /* B [k][n] -> [c][i] */
            int K = d[0], N = d[1], kp = e->dims[1];
            for (int i = 0; i < K; i++)
                for (int c = 0; c < N; c++) out[(size_t)c * kp + i] = (uint8_t)(int8_t)v[(size_t)i * N + c];
        } else if (r->kind == VK_QCONV) {               /* W [co][ci][j] -> [co][j][ci] */
            int co = d[0], ci = d[1], k = d[2], cp = e->dims[2];
            for (int o = 0; o < co; o++)
                for (int i = 0; i < ci; i++)
                    for (int j = 0; j < k; j++)
                        out[((size_t)o * k + j) * cp + i] = (uint8_t)v[((size_t)o * ci + i) * k + j];
        } else {                                        /* [dir][in][4H] -> [4H][in] */
            int in = d[1], G = d[2], ip = e->dims[1];
            const int32_t *w = v + (size_t)r->dir * in * G;
            for (int i = 0; i < in; i++)
                for (int gi = 0; gi < G; gi++) out[(size_t)gi * ip + i] = (uint8_t)(int8_t)w[(size_t)i * G + gi];
        }
        free(v);
        return 0;
    }
    float *v = malloc(n * sizeof *v), *o = (float *)(void *)out;
    if (!v || init_floats(e->src, v, n) != 0) { free(v); return -1; }
    memset(out, 0, (size_t)e->bytes);
    switch (r->kind) {
    case VK_F32: memcpy(o, v, n * sizeof *v); break;
    case VK_LSTM_B: memcpy(o, v + (size_t)r->dir * d[1], (size_t)d[1] * sizeof *v); break;
    case VK_MATF:                                       /* [k][n] -> [k][n8] */
        for (int i = 0; i < d[0]; i++) memcpy(o + (size_t)i * e->dims[1], v + (size_t)i * d[1], (size_t)d[1] * sizeof *v);
        break;
    case VK_CONVF:                                      /* [co][1][k] -> [k][co8] */
        for (int c = 0; c < d[0]; c++)
            for (int j = 0; j < d[2]; j++) o[(size_t)j * e->dims[1] + c] = v[(size_t)c * d[2] + j];
        break;
    case VK_CONVT: {                                    /* [ci][co][k] -> [s][k/s][ci][co8] */
        int ci = d[0], co = d[1], k = d[2], s = r->geom[0], mt = k / s, cp = e->dims[3];
        for (int ph = 0; ph < s; ph++)
            for (int m = 0; m < mt; m++)
                for (int i = 0; i < ci; i++)
                    for (int c = 0; c < co; c++)
                        o[(((size_t)ph * mt + m) * ci + i) * cp + c] = v[((size_t)i * co + c) * k + ph + (mt - 1 - m) * s];
        break;
    }
    case VK_CONVT_DW:                                   /* [c][1][k] -> [k][c] */
        for (int c = 0; c < d[0]; c++)
            for (int j = 0; j < d[2]; j++) o[(size_t)j * d[0] + c] = v[(size_t)c * d[2] + j];
        break;
    default: free(v); return -1;
    }
    free(v);
    return 0;
}

/* Every constant that must be in the table and is not: an 8-bit one (every
 * quantised weight and zero point), a quantised product's scale, or a float or
 * half one of more than one value, other than a Resize's scales. */
static const ginit *unexpected(const graph *g) {
    for (size_t i = 0; i < g->ni; i++) {
        const ginit *t = &g->inits[i];
        if (t->used) continue;
        if (t->dtype == OT_I8 || t->dtype == OT_U8 || t->weight) return t;
        if ((t->dtype == OT_F32 || t->dtype == OT_F16) && init_count(t) > 1 && t->plain_use) return t;
    }
    return NULL;
}

/* ---- the voices ------------------------------------------------------------ */

/* The stored zip's members: each must be "<id>.npy" for one of the spec's
 * voices, once, a little-endian f32 table of style_rows x style_dim in C order. */
static int read_voices(const oc_voice_spec *spec, const uint8_t *z, size_t n, const uint8_t **tables, char *err,
                       size_t cap) {
    const uint8_t *p = z, *end = z + n;
    while (p + 30 <= end && le32(p) == 0x04034b50u) {
        uint16_t flags = le16(p + 6), method = le16(p + 8), nl = le16(p + 26), xl = le16(p + 28);
        uint64_t size = le32(p + 18);
        const uint8_t *name = p + 30, *extra = name + nl, *data = extra + xl;
        if (data > end || (flags & 0x08)) break;
        /* numpy writes zip64 members: the 32-bit size is 0xFFFFFFFF and the real
         * one is in the zip64 extra field (id 1: uncompressed, then compressed). */
        for (const uint8_t *x = extra; size == 0xFFFFFFFFu && x + 4 <= data;) {
            uint16_t id = le16(x), len = le16(x + 2);
            if (id == 1 && len >= 16 && x + 4 + 16 <= data) size = le32(x + 4) | (uint64_t)le32(x + 8) << 32;
            x += 4 + len;
        }
        if ((uint64_t)(end - data) < size) break;
        if (method != 0) { seterr(err, cap, "the voices: %.*s is compressed", (int)nl, (const char *)name); return -1; }
        size_t v = 0;
        for (; v < spec->nvoices; v++) {
            size_t il = strlen(spec->voices[v]);
            if (nl == il + 4 && memcmp(name, spec->voices[v], il) == 0 && memcmp(name + il, ".npy", 4) == 0) break;
        }
        if (v == spec->nvoices) { seterr(err, cap, "the voices: unexpected member %.*s", (int)nl, (const char *)name); return -1; }
        if (tables[v]) { seterr(err, cap, "the voices: %s twice", spec->voices[v]); return -1; }
        /* .npy: "\x93NUMPY", version, header length, a dict literal, data. */
        size_t hl = size >= 12 && memcmp(data, "\x93NUMPY", 6) == 0 ? (data[6] == 1 ? 10 + (size_t)le16(data + 8) : 12 + (size_t)le32(data + 8)) : 0;
        char hdr[256], shape[64];
        size_t off = data[6] == 1 ? 10 : 12;
        snprintf(hdr, sizeof hdr, "%.*s", hl > off ? (int)(hl - off < sizeof hdr ? hl - off : sizeof hdr - 1) : 0,
                 (const char *)data + off);
        snprintf(shape, sizeof shape, "'shape': (%d, %d)", spec->style_rows, spec->style_dim);
        if (!hl || hl > size || !strstr(hdr, "'descr': '<f4'") || !strstr(hdr, "'fortran_order': False") ||
            !strstr(hdr, shape) || size - hl != (uint64_t)spec->style_rows * spec->style_dim * 4) {
            seterr(err, cap, "the voices: %s is not a %d x %d float32 table", spec->voices[v], spec->style_rows,
                   spec->style_dim);
            return -1;
        }
        tables[v] = data + hl;
        p = data + size;
    }
    if (p + 4 <= end && le32(p) != 0x02014b50u && le32(p) != 0x06054b50u) {
        seterr(err, cap, "the voices are not a stored zip");
        return -1;
    }
    for (size_t v = 0; v < spec->nvoices; v++)
        if (!tables[v]) { seterr(err, cap, "the voices: no voice %s", spec->voices[v]); return -1; }
    return 0;
}

/* ---- writing --------------------------------------------------------------- */

static void put32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static void put64(uint8_t *p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = (uint8_t)(v >> (8 * i)); }
static uint64_t round64(uint64_t n) { return (n + 63) & ~(uint64_t)63; }

static int entry_cmp(const void *a, const void *b) {
    return strcmp(((const entry *)a)->name, ((const entry *)b)->name);
}

static int write_file(const char *out, entry *e, size_t ne, const uint8_t sha_m[32], const uint8_t sha_v[32],
                      size_t *written, char *err, size_t cap) {
    uint64_t data = round64(VOICE_HEADER + (uint64_t)ne * VOICE_ENTRY), off = data;
    for (size_t i = 0; i < ne; i++) {
        e[i].off = off;
        off = round64(off + e[i].bytes);
    }
    uint64_t size = e[ne - 1].off + e[ne - 1].bytes;
    uint8_t *head = calloc(1, (size_t)data);
    if (!head) { seterr(err, cap, "out of memory"); return -1; }
    memcpy(head, VOICE_MAGIC, 8);
    put32(head + 8, VOICE_VERSION);
    put32(head + 12, (uint32_t)ne);
    put32(head + 16, VOICE_HEADER);
    put32(head + 20, VOICE_ENTRY);
    put64(head + 24, data);
    put64(head + 32, size);
    memcpy(head + 40, sha_m, 32);
    memcpy(head + 72, sha_v, 32);
    for (size_t i = 0; i < ne; i++) {
        uint8_t *x = head + VOICE_HEADER + i * VOICE_ENTRY;
        memcpy(x, e[i].name, strlen(e[i].name));
        put32(x + 48, (uint32_t)e[i].dtype);
        put32(x + 52, (uint32_t)e[i].ndim);
        for (int d = 0; d < 4; d++) put32(x + 56 + 4 * d, (uint32_t)e[i].dims[d]);
        put64(x + 72, e[i].off);
        put64(x + 80, e[i].bytes);
        uint32_t s;
        memcpy(&s, &e[i].scale, 4);
        put32(x + 88, s);
        put32(x + 92, (uint32_t)e[i].zp);
    }
    char tmp[4200];
    if (snprintf(tmp, sizeof tmp, "%s.tmp", out) >= (int)sizeof tmp) { free(head); seterr(err, cap, "the path is too long"); return -1; }
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(head); seterr(err, cap, "cannot create %s", tmp); return -1; }
    int bad = fwrite(head, 1, (size_t)data, f) != (size_t)data;
    free(head);
    static const uint8_t zeros[64];
    uint64_t at = data;
    for (size_t i = 0; i < ne && !bad; i++) {
        if (e[i].off > at) bad = fwrite(zeros, 1, (size_t)(e[i].off - at), f) != (size_t)(e[i].off - at);
        uint8_t *buf = malloc((size_t)e[i].bytes);
        if (!buf || fill(&e[i], buf) != 0) {
            free(buf);
            fclose(f);
            unlink(tmp);
            seterr(err, cap, "%s: cannot read its values", e[i].name);
            return -1;
        }
        if (!bad) bad = fwrite(buf, 1, (size_t)e[i].bytes, f) != (size_t)e[i].bytes;
        free(buf);
        at = e[i].off + e[i].bytes;
    }
    if (fclose(f) != 0) bad = 1;
    if (bad || rename(tmp, out) != 0) {
        unlink(tmp);
        seterr(err, cap, "cannot write %s", out);
        return -1;
    }
    if (written) *written = (size_t)size;
    return 0;
}

int oc_voice_pack_mem(const oc_voice_spec *spec, const unsigned char *model, size_t model_len,
                      const unsigned char *voices, size_t voices_len, const char *out, size_t *written,
                      char *err, size_t errcap) {
    graph g;
    if (graph_parse(model, model_len, &g, err, errcap) != 0) return -1;
    size_t cap = spec->nrows + spec->nvoices, ne = 0;
    entry *e = calloc(cap ? cap : 1, sizeof *e);
    const uint8_t **tables = calloc(spec->nvoices ? spec->nvoices : 1, sizeof *tables);
    char (*vn)[VOICE_NAME_MAX] = NULL;           /* "voice.<id>" */
    int rc = -1;
    if (!e || !tables) { seterr(err, errcap, "out of memory"); goto done; }
    for (size_t i = 0; i < spec->nrows; i++) {
        if (plan_row(&g, &spec->rows[i], &e[ne], err, errcap) != 0) goto done;
        if (e[ne].name) ne++;
    }
    const ginit *x = unexpected(&g);
    if (x) {
        int64_t dd[MAX_DIM];
        memcpy(dd, x->dims, sizeof dd);
        char shape[96];
        shape_text(shape, sizeof shape, x->dtype, x->ndim, dd);
        seterr(err, errcap, "the model has a constant the forward pass does not know: %.*s, %s", (int)x->name.n,
               (const char *)x->name.p, shape);
        goto done;
    }
    if (read_voices(spec, voices, voices_len, tables, err, errcap) != 0) goto done;
    vn = calloc(spec->nvoices ? spec->nvoices : 1, sizeof *vn);
    if (!vn) { seterr(err, errcap, "out of memory"); goto done; }
    for (size_t v = 0; v < spec->nvoices; v++) {
        if (snprintf(vn[v], VOICE_NAME_MAX, "voice.%s", spec->voices[v]) >= VOICE_NAME_MAX) {
            seterr(err, errcap, "the voice id %s is too long", spec->voices[v]);
            goto done;
        }
        memset(&e[ne], 0, sizeof e[ne]);
        e[ne].name = vn[v];
        e[ne].voice = tables[v];
        set_dims(&e[ne], VOICE_F32, 2, spec->style_rows, spec->style_dim, 0, 0);
        ne++;
    }
    if (!ne) { seterr(err, errcap, "nothing to write"); goto done; }
    qsort(e, ne, sizeof *e, entry_cmp);
    for (size_t i = 0; i < ne; i++) {
        if (strlen(e[i].name) >= VOICE_NAME_MAX || !e[i].name[0]) { seterr(err, errcap, "the name %s is too long", e[i].name); goto done; }
        if (i && strcmp(e[i - 1].name, e[i].name) == 0) { seterr(err, errcap, "the name %s is used twice", e[i].name); goto done; }
    }
    uint8_t sm[32], sv[32];
    mbedtls_sha256(model, model_len, sm, 0);
    mbedtls_sha256(voices, voices_len, sv, 0);
    rc = write_file(out, e, ne, sm, sv, written, err, errcap);
done:
    free(e);
    free(tables);
    free(vn);
    graph_free(&g);
    return rc;
}

int oc_voice_pack(const char *model, const char *voices, const char *out, size_t *written, char *err, size_t errcap) {
    oc_tts_map m, v;
    if (oc_tts_map_open(model, &m) != 0) { seterr(err, errcap, "cannot read %s", model); return -1; }
    if (oc_tts_map_open(voices, &v) != 0) { oc_tts_map_close(&m); seterr(err, errcap, "cannot read %s", voices); return -1; }
    char why[400] = "";
    int rc = oc_voice_pack_mem(&OC_VOICE_SPEC, m.p, m.n, v.p, v.n, out, written, why, sizeof why);
    if (rc != 0) seterr(err, errcap, "%s, %s: %s", model, voices, why);
    oc_tts_map_close(&m);
    oc_tts_map_close(&v);
    return rc;
}

int oc_voice_pack_cli(const char *model, const char *voices, const char *out) {
    char err[600] = "";
    size_t n = 0;
    if (oc_voice_pack(model, voices, out, &n, err, sizeof err) != 0) {
        fprintf(stderr, "openchimed: %s\n", err);
        return 1;
    }
    fprintf(stderr, "openchimed: wrote %s (%zu bytes)\n", out, n);
    return 0;
}
