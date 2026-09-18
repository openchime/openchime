/*
 * OpenChime — voice-input recognition built into the daemon (REQ-296–300,
 * ARCH-112, docs/VOICE-INPUT.md §6).
 *
 * The Moonshine engine (stt_render.h). The code is in the binary; the model's
 * graphs and tokenizer are files in a data directory of their own, mapped when
 * the engine opens, so missing recognizer data turns voice input off without
 * touching read-aloud. Compiled only into a daemon built with voice input
 * (make STT=1, the default).
 */
#ifndef OC_STT_H
#define OC_STT_H

#include <stddef.h>

/* The language this daemon hears. */
#define OC_STT_LANG "en-US"

/* The model and the published files together, as the manifest and STT_INFO
 * name them. The files are pinned by SHA-256 in scripts/build_moonshine.sh;
 * change them and change this. */
#define OC_STT_MODEL_VERSION OC_STT_LANG "/moonshine-tiny-streaming/quantized_26_08_21"

#define OC_STT_DATA_SYSTEM_DIR "/usr/share/openchime/stt"

/* The files a data directory must hold for this build, relative to it. */
extern const char *const OC_STT_DATA_FILES[];

/* Find the recognizer's data directory and check it is the data this build
 * expects. 1 on success; 0 with a reason, and voice input should be absent. */
int oc_stt_data_ready(char *err, size_t errcap);

/* `openchimed --stt-manifest DIR`: hash the data files in DIR and write the
 * manifest this build accepts. Returns a process exit status. */
int oc_stt_manifest(const char *dir);

/* `openchimed --stt-hear IN.wav`: recognize a 16-bit mono WAV (any rate; it is
 * resampled to 16 kHz) with the model in this binary, printing the text on
 * stdout and timing on stderr. Returns a process exit status. */
int oc_stt_hear(const char *path);

#endif
