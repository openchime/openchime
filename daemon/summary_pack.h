/*
 * OpenChime — the summary model's data file, made from the model as Qwen
 * publishes it (SUMMARIES.md §6, SUMKIT.md): `openchimed --summary-pack DIR OUT`
 * reads config.json, tokenizer.json and model.safetensors from DIR and writes
 * summary.bin (sumkit.h has the format); `--summary-pack --full DIR OUT` writes
 * the same model unquantised, for the checks (sum_check).
 *
 * The converter takes the text model's tensors and nothing else (the vision
 * tower and the multi-token-prediction layer are skipped), names them in
 * sumkit's terms, applies the transforms the model's own code applies at load
 * (the norms' 1 + weight, A = -exp(A_log)), and writes each in the type the
 * quantisation plan gives it, rows quantised with ggml's reference algorithms
 * (no importance matrix). It holds a row at a time, never a whole tensor or the
 * whole file. The same inputs give the same bytes on every run and machine
 * (compiled with -ffp-contract=off, as the kits are). Anything that is not the
 * expected model -- a dimension it cannot run, a tensor missing, unexpected or
 * of another shape, a tokenizer of another kind -- is refused, naming the first
 * difference.
 */
#ifndef OC_SUMMARY_PACK_H
#define OC_SUMMARY_PACK_H

#include <stddef.h>
#include <stdint.h>

/* The input files, in the order summary.bin records their SHA-256s. */
enum { SP_CONFIG = 0, SP_TOKENIZER, SP_WEIGHTS, SP_FILES };
extern const char *const OC_SUMMARY_INPUTS[SP_FILES];

/* Write summary.bin from the inputs in `dir` to `out` (through a temporary
 * file beside it, renamed into place), quantised, or unquantised when `full`.
 * `sha` (may be NULL): the inputs' SHA-256s when the caller already has them,
 * so they are not read again. `pause` (may be NULL) is asked between tensors:
 * non-zero abandons the conversion. 0, with the file's size in *written (may be
 * NULL); -1 with the reason in `err`. */
int oc_summary_pack(const char *dir, const char *out, int full, const uint8_t (*sha)[32],
                    int (*pause)(void *ctx), void *pause_ctx, uint64_t *written, char *err, size_t errcap);

/* `openchimed --summary-pack [--full] DIR OUT`: says what was done or why not
 * on stderr; the exit code. */
int oc_summary_pack_cli(int argc, char **argv);

#endif
