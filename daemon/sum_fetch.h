/*
 * OpenChime — the local summary model, made by the daemon at the first start
 * with summaries on (REQ-310, ARCH-116, docs/details/SUMMARIES.md §6).
 *
 * The model is data, not code, and is never shipped in a package. When an
 * operator turns summaries on, the daemon's next start fetches Qwen's release
 * of the model it was built for -- its configuration, tokenizer, licence and
 * weights, at one pinned commit of the repository, each file checked against a
 * pinned SHA-256 -- and converts it into summary.bin (summary_pack.h) beside
 * the database. The conversion is deterministic, so its result is checked
 * against a pinned SHA-256 too; the source files are then deleted and the
 * licence kept. Later starts find summary.bin and its marker and fetch
 * nothing. A file that is missing, partial or altered is made again; one that
 * cannot be made leaves summaries off.
 */
#ifndef OC_SUM_FETCH_H
#define OC_SUM_FETCH_H

#include <stddef.h>
#include <stdint.h>

/* The model this daemon summarizes with: Qwen3.5 2B (Apache-2.0), from Qwen's
 * own repository at one commit, converted by summary_pack.c. Its name is part
 * of every summary's version. */
#define OC_SUM_MODEL_NAME  "qwen3.5-2b-sumkit-1"
#define OC_SUM_MODEL_FILE  "summary.bin"
#define OC_SUM_SOURCE_COMMIT           "15852e8c16360a2fea060d615a32b45270f8a8fc"
#define OC_SUM_SOURCE_URL              "https://huggingface.co/Qwen/Qwen3.5-2B/resolve/" OC_SUM_SOURCE_COMMIT "/"
#define OC_SUM_SOURCE_CONFIG_SHA256    "ed1c1723241f23f7f4e23430759cbd7dcfb4103cbdfe052bfe7626b57c2615b4"
#define OC_SUM_SOURCE_TOKENIZER_SHA256 "5f9e4d4901a92b997e463c1f46055088b6cca5ca61a6522d1b9f64c4bb81cb42"
#define OC_SUM_SOURCE_LICENSE_SHA256   "bbedc3fda3305820b977265f01b8619d87570a6739de3a5582c3464840f1e57a"
#define OC_SUM_SOURCE_WEIGHTS_SHA256   "aa33250c4fc64891ddfaba3a314fd9542ea371843c387178b425fbcc5ed680b1"
/* summary.bin as --summary-pack makes it from those: the same bytes on every machine */
#define OC_SUM_PACK_SHA256             "c982e72f2e47f7d4e71cc5e977444c4641dcbefe2bb8ebeef6815374c19bf044"
#define OC_SUM_PACK_BYTES              1399305116ull

typedef struct {
    const char *name;           /* in the repository */
    const char *save_as;        /* in the source folder: summary_pack.h's names, and LICENSE */
    const char *sha256;         /* hex */
    uint64_t    max_bytes;
} oc_sum_source_file;

typedef struct {
    const char               *base_url;    /* ending in '/' */
    const oc_sum_source_file *files;
    int                       n_files;
    const char               *pack_sha256; /* summary.bin's, hex */
} oc_sum_source;

/* The pinned release and the summary.bin it converts to. */
extern const oc_sum_source OC_SUM_SOURCE;

/* Make sure summary.bin is in `dir` (created if need be): its path into `out`.
 * When it is not there, the source's files are fetched into `dir`/source and
 * each checked, converted (summary_pack.h), the result checked, the sources
 * deleted and the licence kept as `dir`/LICENSE. `pause` (may be NULL) is
 * asked between the conversion's tensors: non-zero abandons it. `stop`, once
 * set, abandons a fetch. 0, or -1 with a reason; on failure nothing partial is
 * left behind. Blocks for as long as it takes. */
int oc_sum_model_ensure(const char *dir, const oc_sum_source *src, const volatile int *stop,
                        int (*pause)(void *ctx), void *pause_ctx, char *out, size_t cap, char *err, size_t errcap);

/* `dir` for a database at `db_path`: "<its directory>/summary". */
void oc_sum_model_dir(const char *db_path, char *out, size_t cap);

#endif
