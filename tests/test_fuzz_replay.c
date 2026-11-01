/* The fuzzing tier's corpora and reproducers, replayed (docs/TESTING.md,
 * "Fuzzing"): every file under tests/fuzz/corpus/<harness>/ and
 * tests/fuzz/crashes/<harness>/ through its harness. A crash the fuzzer found is
 * checked in under crashes/, and fails here until it is fixed. A harness aborts
 * on a broken contract, so a failure here is the process stopping, with the
 * file it was on printed first. A missing crashes/ directory is no files. */

#include "check.h"
#include "testenv.h"
#include "fuzz/fuzz.h"

#include <dirent.h>
#include <string.h>

static int replay_dir(const char *rel, int (*harness)(const uint8_t *, size_t), int must_exist) {
    char path[PATH_MAX];
    oc_test_tree_path(rel, path, sizeof path);
    DIR *d = opendir(path);
    if (!d) {
        if (must_exist) { printf("  FAIL %s is missing\n", rel); failures++; }
        return 0;
    }
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (e->d_name[0] == '.') continue;
        char file[PATH_MAX + 256];
        snprintf(file, sizeof file, "%s/%s", path, e->d_name);
        FILE *f = fopen(file, "rb");
        if (!f) { printf("  FAIL cannot read %s\n", file); failures++; continue; }
        static uint8_t buf[1u << 20];
        size_t len = fread(buf, 1, sizeof buf, f);
        fclose(f);
        fflush(stdout);                     /* what ran before, should the harness stop here */
        if (harness(buf, len) != 0) { printf("  FAIL %s\n", file); failures++; }
        n++;
    }
    closedir(d);
    return n;
}

int run_fuzz_replay_tests(void) {
    static const struct { const char *name; int (*fn)(const uint8_t *, size_t); } H[] = {
        { "frame", oc_fuzz_frame },
        { "framebuf", oc_fuzz_framebuf },
    };
    int total = 0;
    for (size_t i = 0; i < sizeof H / sizeof H[0]; i++) {
        char rel[128];
        snprintf(rel, sizeof rel, "tests/fuzz/corpus/%s", H[i].name);
        int n = replay_dir(rel, H[i].fn, 1);
        CHECK(n > 0);                              /* a corpus that is there and not empty */
        snprintf(rel, sizeof rel, "tests/fuzz/crashes/%s", H[i].name);
        total += n + replay_dir(rel, H[i].fn, 0);
    }
    printf("test_fuzz_replay: %d corpus and reproducer files through their harnesses\n", total);
    return failures;
}
