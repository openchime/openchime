/*
 * OpenChime — the local summary model, fetched on first use (REQ-310, ARCH-116,
 * docs/SUMMARIES.md §6).
 *
 * The model is data, not code, and is never shipped in a package: when an
 * operator turns summaries on, the daemon fetches the one model it was built
 * for, from a pinned address, checks the file against a pinned SHA-256, and
 * keeps it beside the database. Later starts find it there. A file that is
 * missing, partial or altered is fetched again; one that cannot be fetched
 * leaves summaries off.
 */
#ifndef OC_SUM_FETCH_H
#define OC_SUM_FETCH_H

#include <stddef.h>
#include <stdint.h>

/* The model this daemon summarizes with: open-licensed (Apache-2.0), a 4-bit
 * GGUF file. */
#define OC_SUM_MODEL_NAME   "qwen3.5-2b-q4_k_m"
#define OC_SUM_MODEL_FILE   "Qwen3.5-2B-Q4_K_M.gguf"
#define OC_SUM_MODEL_URL    "https://huggingface.co/unsloth/Qwen3.5-2B-GGUF/resolve/main/Qwen3.5-2B-Q4_K_M.gguf"
#define OC_SUM_MODEL_SHA256 "aaf42c8b7c3cab2bf3d69c355048d4a0ee9973d48f16c731c0520ee914699223"
#define OC_SUM_MODEL_BYTES  1280835840ull

/* Make sure the model is in `dir` (created if need be): its path into `out`.
 * `url` and `sha256_hex` are normally the pinned ones above (a test passes its
 * own). `stop`, once set, abandons a fetch. 0, or -1 with a reason. Blocks for
 * as long as a fetch takes. */
int oc_sum_model_ensure(const char *dir, const char *file, const char *url, const char *sha256_hex,
                        uint64_t max_bytes, const volatile int *stop, char *out, size_t cap, char *err,
                        size_t errcap);

/* Whether this CPU can run the local model. On x86-64 it is built for
 * x86-64-v3 (scripts/build_llamacpp.sh), and a CPU without AVX2, FMA, F16C or
 * BMI2 would stop the whole daemon at the first such instruction; elsewhere
 * every CPU can. `supports` answers for a feature name ("avx2", ...); NULL asks
 * the CPU itself (a test passes its own). 1, or 0 with a reason. */
int oc_sum_cpu_ok(int (*supports)(const char *feature), char *err, size_t errcap);

/* `dir` for a database at `db_path`: "<its directory>/summary". */
void oc_sum_model_dir(const char *db_path, char *out, size_t cap);

#endif
