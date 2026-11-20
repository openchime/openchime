/* A small model of the summary model's family, written as Qwen publishes one:
 * config.json, tokenizer.json, model.safetensors and LICENSE in a directory, for the
 * converter, sumkit and the fetch to be tested on without the real 4.5 GB
 * (tests/test_sumkit.c, tests/test_summary.c). Two layers, one of each kind,
 * at sizes of its own; its tokenizer knows the 256 bytes, a few merges and the
 * chat format's tokens. Random weights from a fixed seed. */
#ifndef OC_SUMKIT_FIXTURE_H
#define OC_SUMKIT_FIXTURE_H

#include <stddef.h>

enum {
    SKF_NONE = 0,
    SKF_MISSING = 1,   /* one tensor left out */
    SKF_EXTRA = 2,     /* a text-model tensor the configuration does not imply */
    SKF_SHAPE = 4,     /* one tensor of another shape */
};

/* Write the model into `dir`, changed by `flags`. 0, or -1. */
int oc_test_sumkit_model(const char *dir, int flags);

/* The fixture's dimensions, for the tests that check them. */
#define SKF_EMBD   256
#define SKF_FF     512
#define SKF_VOCAB  320
#define SKF_LAYERS 2

#endif
