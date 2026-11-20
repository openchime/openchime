/* The small model sumkit and its converter are tested on (sumkit_fixture.h). */
#include "sumkit_fixture.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t g_seed;
static uint32_t rnd(void) {
    g_seed ^= g_seed << 13;
    g_seed ^= g_seed >> 17;
    g_seed ^= g_seed << 5;
    return g_seed;
}
static float rnd_f(float lo, float hi) { return lo + (hi - lo) * (float)(rnd() >> 8) / 16777216.0f; }

static int write_text(const char *dir, const char *name, const char *text) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    int bad = fputs(text, f) < 0;
    return fclose(f) || bad ? -1 : 0;
}

/* GPT-2's byte-level character for byte b, as UTF-8 in a JSON string. */
static int byte_char(int b, char *out) {
    int printable = (b >= 33 && b <= 126) || (b >= 161 && b <= 172) || (b >= 174 && b <= 255);
    unsigned cp;
    if (printable) cp = (unsigned)b;
    else {
        int n = 0;
        for (int x = 0; x < b; x++)
            if (!((x >= 33 && x <= 126) || (x >= 161 && x <= 172) || (x >= 174 && x <= 255))) n++;
        cp = 256u + (unsigned)n;
    }
    if (cp == '"' || cp == '\\') return sprintf(out, "\\%c", (char)cp);
    if (cp < 0x80) return sprintf(out, "%c", (char)cp);
    return sprintf(out, "%c%c", (char)(0xc0 | cp >> 6), (char)(0x80 | (cp & 0x3f)));
}

static const char *const MERGES[][2] = { { "h", "e" }, { "l", "l" }, { "he", "ll" }, { "hell", "o" },
                                         { "\xc4\xa0", "w" }, { "o", "r" }, { "\xc4\xa0w", "or" } };
static const char *const ADDED[] = { "<|endoftext|>", "<|im_start|>", "<|im_end|>", "<think>", "</think>" };

static int write_tokenizer(const char *dir) {
    size_t cap = 1 << 16, n = 0;
    char *s = malloc(cap), ch[8];
    if (!s) return -1;
#define P(...) (n += (size_t)snprintf(s + n, cap - n, __VA_ARGS__))
    P("{\"version\":\"1.0\",\"added_tokens\":[");
    int nm = (int)(sizeof MERGES / sizeof *MERGES), na = (int)(sizeof ADDED / sizeof *ADDED);
    for (int i = 0; i < na; i++)
        P("%s{\"id\":%d,\"content\":\"%s\",\"single_word\":false,\"special\":%s}", i ? "," : "", 256 + nm + i, ADDED[i],
          i < 3 ? "true" : "false");
    P("],\"normalizer\":{\"type\":\"NFC\"},\"pre_tokenizer\":{\"type\":\"Sequence\",\"pretokenizers\":[{\"type\":"
      "\"Split\",\"pattern\":{\"Regex\":\"(?i:'s|'t|'re|'ve|'m|'ll|'d)|[^\\\\r\\\\n\\\\p{L}\\\\p{N}]?[\\\\p{L}\\\\p{M}]+"
      "|\\\\p{N}| ?[^\\\\s\\\\p{L}\\\\p{M}\\\\p{N}]+[\\\\r\\\\n]*|\\\\s*[\\\\r\\\\n]+|\\\\s+(?!\\\\S)|\\\\s+\"},\"behavior\":"
      "\"Isolated\",\"invert\":false},{\"type\":\"ByteLevel\",\"add_prefix_space\":false,\"trim_offsets\":false,"
      "\"use_regex\":false}]},\"post_processor\":null,\"decoder\":{\"type\":\"ByteLevel\"},\"model\":{\"type\":\"BPE\","
      "\"dropout\":null,\"unk_token\":null,\"byte_fallback\":false,\"ignore_merges\":false,\"vocab\":{");
    for (int b = 0; b < 256; b++) {
        byte_char(b, ch);
        P("%s\"%s\":%d", b ? "," : "", ch, b);
    }
    for (int i = 0; i < nm; i++) P(",\"%s%s\":%d", MERGES[i][0], MERGES[i][1], 256 + i);
    P("},\"merges\":[");
    for (int i = 0; i < nm; i++) P("%s\"%s %s\"", i ? "," : "", MERGES[i][0], MERGES[i][1]);
    P("]}}");
#undef P
    int rc = n < cap ? write_text(dir, "tokenizer.json", s) : -1;
    free(s);
    return rc;
}

typedef struct { char name[128]; int f32; unsigned dims[3], nd; } tdesc;

static int add(tdesc *t, int n, const char *name, int f32, unsigned a, unsigned b, unsigned c) {
    snprintf(t[n].name, sizeof t[n].name, "%s", name);
    t[n].f32 = f32;
    t[n].dims[0] = a; t[n].dims[1] = b; t[n].dims[2] = c;
    t[n].nd = c ? 3 : b ? 2 : 1;
    return n + 1;
}

int oc_test_sumkit_model(const char *dir, int flags) {
    g_seed = 0x5eed1234u;
    char cfg[2048];
    snprintf(cfg, sizeof cfg,
             "{\"architectures\":[\"Qwen3_5ForConditionalGeneration\"],\"model_type\":\"qwen3_5\","
             "\"tie_word_embeddings\":true,\"text_config\":{\"attn_output_gate\":true,\"head_dim\":64,"
             "\"hidden_size\":%d,\"intermediate_size\":%d,\"layer_types\":[\"linear_attention\",\"full_attention\"],"
             "\"linear_conv_kernel_dim\":4,\"linear_key_head_dim\":32,\"linear_num_key_heads\":2,"
             "\"linear_num_value_heads\":4,\"linear_value_head_dim\":32,\"model_type\":\"qwen3_5_text\","
             "\"num_attention_heads\":2,\"num_hidden_layers\":%d,\"num_key_value_heads\":1,\"rms_norm_eps\":1e-06,"
             "\"tie_word_embeddings\":true,\"vocab_size\":%d,\"rope_parameters\":{\"mrope_interleaved\":true,"
             "\"rope_type\":\"default\",\"rope_theta\":10000000,\"partial_rotary_factor\":0.25}},"
             "\"vision_config\":{\"depth\":1}}",
             SKF_EMBD, SKF_FF, SKF_LAYERS, SKF_VOCAB);
    if (write_text(dir, "config.json", cfg) || write_tokenizer(dir) ||
        write_text(dir, "LICENSE", "Apache License, Version 2.0 (the fixture's stand-in)\n"))
        return -1;

    tdesc t[64];
    int n = 0;
    char nm[128];
    const char *lm = "model.language_model.";
#define L(i, s) (snprintf(nm, sizeof nm, "%slayers.%d.%s", lm, i, s), nm)
    snprintf(nm, sizeof nm, "%sembed_tokens.weight", lm);
    n = add(t, n, nm, 0, SKF_VOCAB, SKF_EMBD, 0);
    snprintf(nm, sizeof nm, "%snorm.weight", lm);
    n = add(t, n, nm, 0, SKF_EMBD, 0, 0);
    for (int l = 0; l < SKF_LAYERS; l++) {
        n = add(t, n, L(l, "input_layernorm.weight"), 0, SKF_EMBD, 0, 0);
        n = add(t, n, L(l, "post_attention_layernorm.weight"), 0, SKF_EMBD, 0, 0);
        n = add(t, n, L(l, "mlp.gate_proj.weight"), 0, SKF_FF, SKF_EMBD, 0);
        n = add(t, n, L(l, "mlp.up_proj.weight"), 0, SKF_FF, SKF_EMBD, 0);
        n = add(t, n, L(l, "mlp.down_proj.weight"), 0, SKF_EMBD, (flags & SKF_SHAPE) && l == 0 ? SKF_FF - 1 : SKF_FF, 0);
    }
    /* layer 0, linear: 2 key heads of 32, 4 value heads of 32 */
    n = add(t, n, L(0, "linear_attn.in_proj_qkv.weight"), 0, 2 * 64 + 128, SKF_EMBD, 0);
    n = add(t, n, L(0, "linear_attn.in_proj_z.weight"), 0, 128, SKF_EMBD, 0);
    n = add(t, n, L(0, "linear_attn.in_proj_a.weight"), 0, 4, SKF_EMBD, 0);
    n = add(t, n, L(0, "linear_attn.in_proj_b.weight"), 0, 4, SKF_EMBD, 0);
    n = add(t, n, L(0, "linear_attn.conv1d.weight"), 0, 256, 1, 4);
    n = add(t, n, L(0, "linear_attn.dt_bias"), 0, 4, 0, 0);
    n = add(t, n, L(0, "linear_attn.A_log"), 1, 4, 0, 0);
    n = add(t, n, L(0, "linear_attn.norm.weight"), 1, 32, 0, 0);
    if (!(flags & SKF_MISSING)) n = add(t, n, L(0, "linear_attn.out_proj.weight"), 0, SKF_EMBD, 128, 0);
    /* layer 1, attention: 2 heads of 64, 1 key/value head */
    n = add(t, n, L(1, "self_attn.q_proj.weight"), 0, 2 * 2 * 64, SKF_EMBD, 0);
    n = add(t, n, L(1, "self_attn.k_proj.weight"), 0, 64, SKF_EMBD, 0);
    n = add(t, n, L(1, "self_attn.v_proj.weight"), 0, 64, SKF_EMBD, 0);
    n = add(t, n, L(1, "self_attn.o_proj.weight"), 0, SKF_EMBD, 128, 0);
    n = add(t, n, L(1, "self_attn.q_norm.weight"), 0, 64, 0, 0);
    n = add(t, n, L(1, "self_attn.k_norm.weight"), 0, 64, 0, 0);
    if (flags & SKF_EXTRA) n = add(t, n, L(1, "self_attn.extra.weight"), 0, 8, 0, 0);
    /* what the converter skips */
    n = add(t, n, "model.visual.blocks.0.attn.qkv.weight", 0, 4, 4, 0);
    n = add(t, n, "mtp.fc.weight", 0, 4, 8, 0);
#undef L

    /* The header, then each tensor's bytes. */
    size_t hcap = 16384, hn = 0, off = 0;
    char *h = malloc(hcap);
    if (!h) return -1;
    hn += (size_t)snprintf(h + hn, hcap - hn, "{\"__metadata__\":{\"format\":\"pt\"}");
    for (int i = 0; i < n; i++) {
        size_t count = t[i].dims[0] * (t[i].nd > 1 ? t[i].dims[1] : 1) * (t[i].nd > 2 ? t[i].dims[2] : 1);
        size_t bytes = count * (t[i].f32 ? 4 : 2);
        hn += (size_t)snprintf(h + hn, hcap - hn, ",\"%s\":{\"dtype\":\"%s\",\"shape\":[%u", t[i].name, t[i].f32 ? "F32" : "BF16", t[i].dims[0]);
        for (unsigned d = 1; d < t[i].nd; d++) hn += (size_t)snprintf(h + hn, hcap - hn, ",%u", t[i].dims[d]);
        hn += (size_t)snprintf(h + hn, hcap - hn, "],\"data_offsets\":[%zu,%zu]}", off, off + bytes);
        off += bytes;
    }
    hn += (size_t)snprintf(h + hn, hcap - hn, "}");
    while (hn % 8) h[hn++] = ' ';
    char path[1200];
    snprintf(path, sizeof path, "%s/model.safetensors", dir);
    FILE *f = fopen(path, "wb");
    int bad = !f;
    uint8_t len8[8];
    for (int i = 0; i < 8; i++) len8[i] = (uint8_t)((uint64_t)hn >> (8 * i));
    if (!bad) bad = fwrite(len8, 1, 8, f) != 8 || fwrite(h, 1, hn, f) != hn;
    for (int i = 0; i < n && !bad; i++) {
        size_t count = t[i].dims[0] * (t[i].nd > 1 ? t[i].dims[1] : 1) * (t[i].nd > 2 ? t[i].dims[2] : 1);
        int is_norm = strstr(t[i].name, "norm.weight") && !strstr(t[i].name, "linear_attn.norm");
        for (size_t k = 0; k < count && !bad; k++) {
            float v = strstr(t[i].name, "A_log") ? rnd_f(-1, 1) : is_norm ? rnd_f(-0.2f, 0.2f) : rnd_f(-0.08f, 0.08f);
            if (t[i].f32) bad = fwrite(&v, 4, 1, f) != 1;
            else {
                union { float f; uint32_t u; } u = { .f = v };
                uint8_t b2[2] = { (uint8_t)(u.u >> 16), (uint8_t)(u.u >> 24) };
                bad = fwrite(b2, 1, 2, f) != 2;
            }
        }
    }
    if (f && fclose(f)) bad = 1;
    free(h);
    return bad ? -1 : 0;
}
