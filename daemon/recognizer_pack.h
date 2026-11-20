/*
 * OpenChime — the recognizer's data file, made from the model as it is
 * published (VOICE-INPUT.md, STTKIT.md): `openchimed --recognizer-pack <dir>
 * <out>` reads the six graphs, the tokenizer and the configuration from <dir>
 * and writes recognizer.bin (sttkit.h has the format); `openchimed
 * --recognizer-pack --list <dir>` prints every graph's inputs, outputs,
 * constants and nodes, which is how the forward pass was checked against the
 * model (STTKIT.md §3).
 *
 * The graphs are ONNX Runtime's .ort files: FlatBuffers, read here with no
 * library, bounds-checked at every step. The converter takes exactly the
 * tensors the forward pass needs, names them in sttkit's own terms and writes
 * each in the layout sttkit's kernels read. The same inputs give the same bytes
 * on every run and machine. Anything that is not exactly the expected model --
 * a file that is not one of these graphs, a tensor missing or of another shape
 * or type, an operator with other settings, a constant the table does not
 * account for -- is refused, naming the first difference. Plain C in every
 * build, so `make test` drives it.
 */
#ifndef OC_RECOGNIZER_PACK_H
#define OC_RECOGNIZER_PACK_H

#include <stddef.h>
#include <stdio.h>

/* The input files, in the order recognizer.bin records their SHA-256s. */
enum {
    RP_FRONTEND = 0,      /* frontend.model.ort    */
    RP_FRONTEND_WEIGHTS,  /* frontend.weights.ort  */
    RP_ENCODER,           /* encoder.ort           */
    RP_ADAPTER,           /* adapter.ort           */
    RP_CROSS_KV,          /* cross_kv.ort          */
    RP_DECODER,           /* decoder_kv.ort        */
    RP_TOKENIZER,         /* tokenizer.bin         */
    RP_CONFIG,            /* streaming_config.json */
    RP_FILES
};
extern const char *const OC_RECOGNIZER_INPUTS[RP_FILES];

/* Print one .ort graph's inputs, outputs, constants and nodes to `out`. 0, or
 * -1 with the reason in `err` (not an .ort file, or one that is damaged). */
int oc_recognizer_list_mem(const unsigned char *ort, size_t len, FILE *out, char *err, size_t errcap);

typedef struct { const unsigned char *data; size_t len; } oc_rp_blob;

/* Write recognizer.bin from the eight inputs' bytes to `out` (through a
 * temporary file beside it, renamed into place). 0, with the file's size in
 * *written (may be NULL); -1 with the reason in `err`. */
int oc_recognizer_pack_mem(const oc_rp_blob in[RP_FILES], const char *out, size_t *written, char *err,
                           size_t errcap);
/* The same from the files in `dir`. */
int oc_recognizer_pack(const char *dir, const char *out, size_t *written, char *err, size_t errcap);

/* `openchimed --recognizer-pack DIR OUT` and `--recognizer-pack --list DIR`:
 * say what was done or why not on stderr; the exit code. */
int oc_recognizer_pack_cli(int argc, char **argv);

#endif
