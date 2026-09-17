/*
 * OpenChime — where read-aloud's data lives, and whether it is the data this
 * daemon was built for (REQ-295, ARCH-111, READ-ALOUD.md §4).
 *
 * The voice model, its voices and ttskit's pronunciation data are files beside
 * the daemon rather than bytes inside it. Nothing here needs ONNX Runtime, so the
 * tests link it directly.
 */
#ifndef OC_TTS_DATA_H
#define OC_TTS_DATA_H

#include <stddef.h>

/* The default for a packaged install: read-only content, which does not belong
 * in /data, the writable volume every other daemon path defaults into. */
#define OC_TTS_DATA_SYSTEM_DIR "/usr/share/openchime/voices"
#define OC_TTS_DATA_MANIFEST   "manifest"

/* Choose the data directory. `env` (OPENCHIME_TTS_DATA_DIR), when set, is used
 * if it is a directory and is otherwise a failure -- an operator who names a
 * directory means that one, and quietly using another would be a surprise.
 * Unset, the first of `system_dir` and `exe_dir/voices` that is a directory is
 * chosen; either may be NULL. Writes the path to `out` and returns 1, or returns
 * 0 with a reason in `err`. */
int oc_tts_data_dir(const char *env, const char *system_dir, const char *exe_dir,
                    char *out, size_t cap, char *err, size_t errcap);

/* The directory holding the running executable, from /proc/self/exe. 1 or 0. */
int oc_tts_exe_dir(char *out, size_t cap);

/* Whether `dir` holds the data for `version`. Its manifest must name `version`
 * exactly and list every one of `required` (relative paths) with a SHA-256 that
 * the file on disk matches. 0 when it does; -1 with a reason otherwise. A data
 * directory from another build, a partial copy or an altered file fails here, at
 * startup, rather than producing wrong speech -- the loose files can disagree
 * with the code in a way embedded bytes never could. */
int oc_tts_data_verify(const char *dir, const char *version, const char *const *required,
                       char *err, size_t errcap);

/* Write `dir`'s manifest for `version` over `files`, hashing each. 0 or -1. */
int oc_tts_data_write_manifest(const char *dir, const char *version, const char *const *files,
                               char *err, size_t errcap);

/* A file mapped read-only, as the engine consumes it in place. */
typedef struct { const unsigned char *p; size_t n; } oc_tts_map;
int  oc_tts_map_open(const char *path, oc_tts_map *m);
void oc_tts_map_close(oc_tts_map *m);

#endif
