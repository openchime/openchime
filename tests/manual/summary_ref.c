/* MANUAL: makes the summary model's references, the fixtures in
 * tests/data/summary/ that sumkit's own run of the model is compared against
 * (docs/SUMKIT.md, `sum_check parity` and `sum_check kl`).
 *
 * Deliberately NOT part of any suite or Makefile target: it runs the model with
 * llama.cpp, which nothing in the build links. Its outputs are committed; rerun
 * it only to remake them, and then commit what it writes.
 *
 * The model is Qwen's published release (docs/VENDORS.md: the pinned commit and
 * SHA-256s), unquantised: converted to a bfloat16 GGUF by llama.cpp's own
 * converter, so the reference and sumkit's `--full` file hold the same weights.
 * With llama.cpp checked out and built (cmake, the llama library and the
 * llama-perplexity tool) in LLAMA, and its converter's Python requirements in
 * the environment:
 *
 *     python3 LLAMA/convert_hf_to_gguf.py SRC --outtype bf16 --outfile /tmp/qwen-bf16.gguf
 *     cc -std=c99 -D_GNU_SOURCE -O2 -Wall -Wextra -Werror -I"LLAMA/include" -I"LLAMA/ggml/include" \
 *        -Idaemon -Ithird_party/jsmn tests/manual/summary_ref.c daemon/sum_core.c daemon/json.c \
 *        -L"LLAMA/build/bin" -lllama -lggml -lggml-base -Wl,-rpath,"LLAMA/build/bin" -lm -o build/summary_ref
 *     build/summary_ref /tmp/qwen-bf16.gguf tests/data/summary
 *
 * and then the baseline the quantisation is held to, Unsloth's Q4_K_M file
 * (the one the daemon ran before sumkit) against the same unquantised model,
 * over the held-out text this writes:
 *
 *     LLAMA/build/bin/llama-perplexity -m /tmp/qwen-bf16.gguf -f tests/data/summary/kl.txt -c 512 \
 *        --kl-divergence-base /tmp/kl-base.bin
 *     LLAMA/build/bin/llama-perplexity -m Qwen3.5-2B-Q4_K_M.gguf -c 512 --kl-divergence-base /tmp/kl-base.bin \
 *        --kl-divergence
 *     build/summary_ref --kl <its Mean KLD> tests/data/summary
 *
 * The prompts are the daemon's own (daemon/sum_core.c's builders) over the
 * synthetic channels in tests/data/summary/channels/, written for the tests;
 * no real chat is used. The model's chat format, the empty thought and the
 * tokenization (special tokens read as such) are the daemon's (sum_llama.c);
 * the cache is at 8 bits with flash attention, as there (the context is 4,096
 * tokens, which every prompt here fits); each answer is greedy inside its grammar after DRY at the daemon's
 * values.
 *
 * Files written to the output folder, per prompt <name>:
 *   <name>.system, <name>.user, <name>.grammar   the prompt as the daemon builds it
 *   <name>.prompt       the templated prompt's token ids, one a line
 *   <name>.steps        one line a step of the answer (at most 32): the token taken, the
 *                       step's largest logit's token and how far above the
 *                       second largest it is (the logits before the grammar)
 *   <name>.logits0.f32  the first step's logits, float32 little-endian
 * and tokens.txt (one line a text: its ids, a tab, the text with \n, \t and \\
 * escaped), kl.txt (the held-out text), kl.ids (its tokens as llama-perplexity
 * reads it) and `manifest`. Run twice on one machine it writes the same bytes. */
#include "llama.h"
#include "jsmn.h"            /* the implementation, for daemon/json.c */
#include "sum_core.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CTX       4096
#define MAX_STEPS 32
#define BATCH     128

static void die(const char *what) { fprintf(stderr, "summary_ref: %s\n", what); exit(1); }

/* A channel file: "Name: text" lines, replies indented two spaces, '#' comments. */
static void read_channel(const char *path, oc_sum_lines *l) {
    FILE *f = fopen(path, "r");
    if (!f) die(path);
    char line[4096];
    int64_t id = 0;
    while (fgets(line, sizeof line, f)) {
        size_t n = strlen(line);
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n || line[0] == '#') continue;
        int indent = line[0] == ' ';
        id++;
        if (oc_sum_lines_add(l, line + (indent ? 2 : 0), indent, NULL, &id, 1)) die("out of memory");
    }
    fclose(f);
}

static void write_file(const char *dir, const char *name, const char *ext, const void *p, size_t n) {
    char path[1200];
    snprintf(path, sizeof path, "%s/%s%s", dir, name, ext);
    FILE *f = fopen(path, "wb");
    if (!f || fwrite(p, 1, n, f) != n || fclose(f)) die(path);
}

static const char *think_close(const char *tmpl) {
    return tmpl && strstr(tmpl, "<think>") ? "<think>\n\n</think>\n\n" : "";
}

static int tokenize(const struct llama_vocab *v, const char *text, int add_special, int parse_special,
                    llama_token **out) {
    int len = (int)strlen(text), cap = len + 16;
    *out = malloc((size_t)cap * sizeof **out);
    int n = llama_tokenize(v, text, len, *out, cap, add_special != 0, parse_special != 0);
    if (n < 0) die("tokenize");
    return n;
}

static void feed(struct llama_context *ctx, llama_token *t, int n) {
    for (int at = 0; at < n; at += BATCH) {
        int k = n - at < BATCH ? n - at : BATCH;
        if (llama_decode(ctx, llama_batch_get_one(t + at, k))) die("decode");
    }
}

static void one_prompt(struct llama_model *m, const char *dir, const char *name, const char *system,
                       const char *user, const char *grammar, FILE *manifest) {
    const struct llama_vocab *v = llama_model_get_vocab(m);
    const char *tmpl = llama_model_chat_template(m, NULL);
    struct llama_context_params cp = llama_context_default_params();
    cp.n_ctx = CTX;
    cp.n_batch = cp.n_ubatch = BATCH;
    cp.n_threads = cp.n_threads_batch = 8;
    cp.flash_attn_type = LLAMA_FLASH_ATTN_TYPE_ENABLED;
    cp.type_k = cp.type_v = GGML_TYPE_Q8_0;
    struct llama_context *ctx = llama_init_from_model(m, cp);
    if (!ctx) die("context");
    llama_chat_message msgs[2] = { { "system", system }, { "user", user } };
    size_t cap = 2 * (strlen(system) + strlen(user)) + 1024;
    char *prompt = malloc(cap + 64);
    int pl = llama_chat_apply_template(tmpl, msgs, 2, true, prompt, (int32_t)cap);
    if (pl < 0 || (size_t)pl >= cap) die("template");
    strcpy(prompt + pl, think_close(tmpl));
    llama_token *tok;
    int nt = tokenize(v, prompt, 1, 1, &tok);
    write_file(dir, name, ".system", system, strlen(system));
    write_file(dir, name, ".user", user, strlen(user));
    write_file(dir, name, ".grammar", grammar, strlen(grammar));
    char path[1200];
    snprintf(path, sizeof path, "%s/%s.prompt", dir, name);
    FILE *f = fopen(path, "w");
    for (int i = 0; i < nt; i++) fprintf(f, "%d\n", tok[i]);
    fclose(f);
    feed(ctx, tok, nt);

    int nv = llama_vocab_n_tokens(v);
    struct llama_sampler *s = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(s, llama_sampler_init_grammar(v, grammar, "root"));
    static const char *breakers[] = { ":", "\"", "*" };
    llama_sampler_chain_add(s, llama_sampler_init_dry(v, 0.8f, 1.75f, 2, CTX, breakers, 3));
    llama_sampler_chain_add(s, llama_sampler_init_greedy());
    snprintf(path, sizeof path, "%s/%s.steps", dir, name);
    FILE *st = fopen(path, "w");
    int steps = 0;
    for (; steps < MAX_STEPS; steps++) {
        const float *lg = llama_get_logits_ith(ctx, -1);
        if (steps == 0) write_file(dir, name, ".logits0.f32", lg, (size_t)nv * sizeof *lg);
        int a = 0, b = -1;
        for (int i = 1; i < nv; i++) {
            if (lg[i] > lg[a]) { b = a; a = i; }
            else if (b < 0 || lg[i] > lg[b]) b = i;
        }
        llama_token t = llama_sampler_sample(s, ctx, -1);
        fprintf(st, "%d %d %.6f\n", t, a, (double)(lg[a] - lg[b]));
        fflush(st);
        if (llama_vocab_is_eog(v, t)) { steps++; break; }
        if (llama_decode(ctx, llama_batch_get_one(&t, 1))) die("decode");
    }
    fclose(st);
    fprintf(manifest, "%s %d %d\n", name, nt, steps);
    llama_sampler_free(s);
    free(tok);
    free(prompt);
    llama_free(ctx);
}

static void escape(FILE *f, const char *s) {
    for (; *s; s++) {
        if (*s == '\n') fputs("\\n", f);
        else if (*s == '\t') fputs("\\t", f);
        else if (*s == '\\') fputs("\\\\", f);
        else fputc(*s, f);
    }
}

/* Texts the tokenizer must split as the reference does: accents, other
 * scripts, digits, whitespace runs, contractions, the chat format's tokens. */
static const char *const TOKEN_TEXTS[] = {
    "Hello world",
    "Invoices over 10,000 still show the old tax rate (ticket 4471).",
    "Café crème, naïve résumé, Zürich, São Paulo, Kraków.",
    "Привет, мир! 你好，世界。 こんにちは。 مرحبا 🙂🚀",
    "  two leading spaces,\tone tab,   three spaces\n\n\nthree newlines \r\n end",
    "I'm sure they'll say it's what we'd've done; DON'T SHOUT'S",
    "3.18.4 v2.0 p99=180ms 2,400req/s 08:40 1e-6 -42 no.4489 @dev",
    "<|im_start|>user\nWrite notes.<|im_end|>\n<|im_start|>assistant\n<think>\n\n</think>\n\n",
    "- [12][15] decision | export endpoint | Jonas fixed it.\n## Launch plan\n",
};

int main(int argc, char **argv) {
    if (argc == 4 && !strcmp(argv[1], "--kl")) {
        char path[1200];
        snprintf(path, sizeof path, "%s/manifest", argv[3]);
        FILE *m = fopen(path, "a");
        if (!m) die(path);
        fprintf(m, "kl_unsloth_q4_k_m %s\n", argv[2]);
        fclose(m);
        return 0;
    }
    if (argc != 3) die("usage: summary_ref MODEL.gguf OUTDIR | summary_ref --kl MEAN_KLD OUTDIR");
    const char *dir = argv[2];
    llama_backend_init();
    struct llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = 0;
    struct llama_model *m = llama_model_load_from_file(argv[1], mp);
    if (!m) die("cannot load the model");
    const struct llama_vocab *v = llama_model_get_vocab(m);

    char path[1200];
    snprintf(path, sizeof path, "%s/manifest", dir);
    FILE *man = fopen(path, "w");
    if (!man) die(path);
    fprintf(man, "# The summary model, unquantised, as the reference runs it (tests/manual/summary_ref.c).\n"
                 "# <name> <prompt tokens> <answer steps>, then kl_tokens, kl_chunk, kl_unsloth_q4_k_m\n");

    oc_sum_lines sup = { 0 }, lau = { 0 }, ops = { 0 }, pla = { 0 };
    snprintf(path, sizeof path, "%s/channels/support.chat", dir);
    read_channel(path, &sup);
    snprintf(path, sizeof path, "%s/channels/launch.chat", dir);
    read_channel(path, &lau);
    snprintf(path, sizeof path, "%s/channels/ops.chat", dir);
    read_channel(path, &ops);
    snprintf(path, sizeof path, "%s/channels/planning.chat", dir);
    read_channel(path, &pla);

    oc_sum_buf pr = { 0 }, gr = { 0 };
    char intro[256];
    /* p01: notes on #support */
    snprintf(intro, sizeof intro, SUM_INTRO_MESSAGES, "#support");
    if (oc_sum_notes_prompt(intro, &sup, oc_sum_notes_for(oc_sum_lines_tokens(&sup), sup.n), &pr, &gr)) die("p01");
    one_prompt(m, dir, "p01", OC_SUM_SYSTEM, pr.p, gr.p, man);
    oc_sum_buf_free(&pr); oc_sum_buf_free(&gr);
    /* p02: the summary of #launch */
    oc_sum_caps caps;
    oc_sum_caps_for(oc_sum_lines_words(&lau), &caps);
    snprintf(intro, sizeof intro, SUM_INTRO_MESSAGES, "#launch");
    if (oc_sum_final_prompt(intro, &lau, &caps, &pr, &gr)) die("p02");
    one_prompt(m, dir, "p02", OC_SUM_SYSTEM, pr.p, gr.p, man);
    oc_sum_buf_free(&pr); oc_sum_buf_free(&gr);
    /* p03: a rewrite */
    if (oc_sum_rewrite_prompt("[3] The migration ran on the staging copy in 47 minutes and should take about 70 on "
                              "production data, so the team split it: the index rebuild runs the day before at 18:00 "
                              "while users are still on 1.9, leaving about 25 minutes of downtime.", 20, &pr, &gr))
        die("p03");
    one_prompt(m, dir, "p03", OC_SUM_SYSTEM, pr.p, gr.p, man);
    oc_sum_buf_free(&pr); oc_sum_buf_free(&gr);

    /* The tokenizer's texts. */
    snprintf(path, sizeof path, "%s/tokens.txt", dir);
    FILE *tf = fopen(path, "w");
    for (size_t i = 0; i < sizeof TOKEN_TEXTS / sizeof *TOKEN_TEXTS; i++) {
        llama_token *t;
        int n = tokenize(v, TOKEN_TEXTS[i], 0, 1, &t);
        for (int k = 0; k < n; k++) fprintf(tf, "%s%d", k ? "," : "", t[k]);
        fputc('\t', tf);
        escape(tf, TOKEN_TEXTS[i]);
        fputc('\n', tf);
        free(t);
    }
    fclose(tf);

    /* The held-out text: the prompts the daemon would build over the two
     * held-out channels, as plain text. */
    oc_sum_buf kl = { 0 };
    snprintf(intro, sizeof intro, SUM_INTRO_MESSAGES, "#ops");
    oc_sum_notes_prompt(intro, &ops, oc_sum_notes_for(oc_sum_lines_tokens(&ops), ops.n), &pr, &gr);
    oc_sum_buf_printf(&kl, "%s\n%s\n", OC_SUM_SYSTEM, pr.p);
    oc_sum_buf_free(&pr); oc_sum_buf_free(&gr);
    oc_sum_caps_for(oc_sum_lines_words(&ops), &caps);
    oc_sum_final_prompt(intro, &ops, &caps, &pr, &gr);
    oc_sum_buf_printf(&kl, "%s\n", pr.p);
    oc_sum_buf_free(&pr); oc_sum_buf_free(&gr);
    snprintf(intro, sizeof intro, SUM_INTRO_MESSAGES, "#planning");
    oc_sum_notes_prompt(intro, &pla, oc_sum_notes_for(oc_sum_lines_tokens(&pla), pla.n), &pr, &gr);
    oc_sum_buf_printf(&kl, "%s\n", pr.p);
    oc_sum_buf_free(&pr); oc_sum_buf_free(&gr);
    oc_sum_caps_for(oc_sum_lines_words(&pla), &caps);
    oc_sum_final_prompt(intro, &pla, &caps, &pr, &gr);
    oc_sum_buf_printf(&kl, "%s", pr.p);
    oc_sum_buf_free(&pr); oc_sum_buf_free(&gr);
    if (kl.oom) die("out of memory");
    write_file(dir, "kl", ".txt", kl.p, kl.n);
    llama_token *kt;
    int kn = tokenize(v, kl.p, 1, 0, &kt);       /* as llama-perplexity reads it */
    snprintf(path, sizeof path, "%s/kl.ids", dir);
    FILE *kf = fopen(path, "w");
    for (int i = 0; i < kn; i++) fprintf(kf, "%d\n", kt[i]);
    fclose(kf);
    fprintf(man, "kl_tokens %d\nkl_chunk 512\n", kn);
    fclose(man);
    free(kt);
    oc_sum_buf_free(&kl);
    oc_sum_lines_free(&sup); oc_sum_lines_free(&lau); oc_sum_lines_free(&ops); oc_sum_lines_free(&pla);
    llama_model_free(m);
    fprintf(stderr, "summary_ref: wrote %s (%d held-out tokens)\n", dir, kn);
    return 0;
}
