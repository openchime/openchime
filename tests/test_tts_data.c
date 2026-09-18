/* Tests for finding and checking read-aloud's data directory (daemon/tts_data.c;
 * REQ-295, ARCH-111). What they prove: an explicitly configured directory is used
 * or refused, never silently replaced; unset, the system directory wins over the
 * one beside the executable; and a manifest check refuses a directory from
 * another build, an altered or missing file, a required file left unlisted, a
 * malformed line and a path that would leave the directory. */
#include "check.h"
#include "tts_data.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void put(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    if (f) { fputs(text, f); fclose(f); }
}

static int contains(const char *s, const char *needle) { return s && strstr(s, needle) != NULL; }

int run_tts_data_tests(void) {
    printf("test_tts_data: data directory precedence, explicit directory refused not replaced, manifest version, hashes, required files, malformed and escaping names\n");

    char root[] = "/tmp/oc-ttsdata-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char sys[256], exe[256], exev[256], env[256], out[512], err[256];
    snprintf(sys, sizeof sys, "%s/system", root);
    snprintf(exe, sizeof exe, "%s/bin", root);
    snprintf(exev, sizeof exev, "%s/bin/voices", root);
    snprintf(env, sizeof env, "%s/configured", root);
    mkdir(sys, 0700); mkdir(exe, 0700); mkdir(exev, 0700); mkdir(env, 0700);

    /* Configured and present: used. */
    CHECK(oc_tts_data_dir(env, sys, exe, out, sizeof out, err, sizeof err) == 1 && strcmp(out, env) == 0);
    /* Configured and absent: refused, even though a system directory exists. */
    char gone[256]; snprintf(gone, sizeof gone, "%s/nope", root);
    err[0] = '\0';
    CHECK(oc_tts_data_dir(gone, sys, exe, out, sizeof out, err, sizeof err) == 0 && contains(err, "not a directory"));
    /* Unset: the system directory first... */
    CHECK(oc_tts_data_dir(NULL, sys, exe, out, sizeof out, err, sizeof err) == 1 && strcmp(out, sys) == 0);
    CHECK(oc_tts_data_dir("", sys, exe, out, sizeof out, err, sizeof err) == 1 && strcmp(out, sys) == 0);
    /* ...then beside the executable. */
    char nosys[256]; snprintf(nosys, sizeof nosys, "%s/nosys", root);
    CHECK(oc_tts_data_dir(NULL, nosys, exe, out, sizeof out, err, sizeof err) == 1 && strcmp(out, exev) == 0);
    /* Neither: disabled, with a reason. */
    err[0] = '\0';
    CHECK(oc_tts_data_dir(NULL, nosys, nosys, out, sizeof out, err, sizeof err) == 0 && contains(err, "no data"));

    /* The running test binary has a directory. */
    CHECK(oc_tts_exe_dir(out, sizeof out) == 1 && out[0] == '/');

    /* A data directory and its manifest. */
    char data[256], sub[256], a[300], b[300];
    snprintf(data, sizeof data, "%s/data", root);
    snprintf(sub, sizeof sub, "%s/data/en-US", root);
    mkdir(data, 0700); mkdir(sub, 0700);
    snprintf(a, sizeof a, "%s/model.ort", data);
    snprintf(b, sizeof b, "%s/en-US/lexicon.bin", data);
    put(a, "model bytes");
    put(b, "lexicon bytes");
    const char *const files[] = { "model.ort", "en-US/lexicon.bin", NULL };
    CHECK(oc_tts_data_write_manifest(data, "en-US/test-1", files, err, sizeof err) == 0);

    /* The directory it was written for verifies. */
    CHECK(oc_tts_data_verify(data, "en-US/test-1", files, err, sizeof err) == 0);

    /* Another build's data is refused by name. */
    err[0] = '\0';
    CHECK(oc_tts_data_verify(data, "en-US/test-2", files, err, sizeof err) == -1 && contains(err, "is for en-US/test-1"));

    /* A required file the manifest does not list is refused: a manifest over half
     * the data would otherwise vouch for the whole. */
    const char *const more[] = { "model.ort", "en-US/lexicon.bin", "voices.npz", NULL };
    err[0] = '\0';
    CHECK(oc_tts_data_verify(data, "en-US/test-1", more, err, sizeof err) == -1 && contains(err, "does not list voices.npz"));

    /* An altered file is refused. */
    put(b, "lexicon bytes, edited");
    err[0] = '\0';
    CHECK(oc_tts_data_verify(data, "en-US/test-1", files, err, sizeof err) == -1 && contains(err, "does not match"));
    put(b, "lexicon bytes");
    CHECK(oc_tts_data_verify(data, "en-US/test-1", files, err, sizeof err) == 0);

    /* A missing file is refused. */
    unlink(a);
    err[0] = '\0';
    CHECK(oc_tts_data_verify(data, "en-US/test-1", files, err, sizeof err) == -1 && contains(err, "missing"));
    put(a, "model bytes");

    /* A manifest naming a path outside the directory is refused, not followed. */
    char man[300]; snprintf(man, sizeof man, "%s/manifest", data);
    put(man, "openchime-tts-data 1\nversion en-US/test-1\n"
             "sha256 0000000000000000000000000000000000000000000000000000000000000000 ../escape\n");
    err[0] = '\0';
    CHECK(oc_tts_data_verify(data, "en-US/test-1", NULL, err, sizeof err) == -1 && contains(err, "malformed"));

    /* Not a manifest at all, and no manifest. */
    put(man, "something else\n");
    CHECK(oc_tts_data_verify(data, "en-US/test-1", files, err, sizeof err) == -1);
    unlink(man);
    err[0] = '\0';
    CHECK(oc_tts_data_verify(data, "en-US/test-1", files, err, sizeof err) == -1 && contains(err, "no manifest"));

    /* A mapped file is the file. */
    oc_tts_map m;
    CHECK(oc_tts_map_open(a, &m) == 0 && m.n == strlen("model bytes") && memcmp(m.p, "model bytes", m.n) == 0);
    oc_tts_map_close(&m);
    CHECK(oc_tts_map_open(gone, &m) == -1);

    char cmd[512];
    snprintf(cmd, sizeof cmd, "rm -rf %s", root);
    CHECK(system(cmd) == 0);
    return failures;
}
