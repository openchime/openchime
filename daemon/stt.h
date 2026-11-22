/*
 * OpenChime — voice-input recognition built into the daemon (REQ-296–300,
 * ARCH-112, docs/details/VOICE-INPUT.md §6).
 *
 * The recognizer engine (stt_render.h), run by sttkit in plain C
 * (docs/details/STTKIT.md). The code is in the binary; the model is one file,
 * recognizer.bin, in a data directory of its own, mapped when the engine opens,
 * so missing recognizer data turns voice input off without touching
 * read-aloud. Compiled only into a daemon built with voice input (make STT=1).
 */
#ifndef OC_STT_H
#define OC_STT_H

#include <stddef.h>

/* The language this daemon hears. */
#define OC_STT_LANG "en-US"

/* The model, as the manifest and STT_INFO name it. recognizer.bin is made from
 * the published files docs/details/VENDORS.md pins; a different model, or a different
 * forward pass that hears differently, changes this. */
#define OC_STT_MODEL_VERSION OC_STT_LANG "/recognizer/1"

#define OC_STT_DATA_SYSTEM_DIR "/usr/share/openchime/stt"

/* The files a data directory must hold for this build, relative to it. */
extern const char *const OC_STT_DATA_FILES[];

/* Find the recognizer's data directory and check it is the data this build
 * expects. 1 on success; 0 with a reason, and voice input should be absent. */
int oc_stt_data_ready(char *err, size_t errcap);

/* `openchimed --stt-manifest DIR` (main.c, tts_data.h): hash the data files in DIR and write the
 * manifest this build accepts. Returns a process exit status. */

/* `openchimed --stt-hear IN.wav`: recognize a 16-bit mono WAV (any rate; it is
 * resampled to 16 kHz) with the model in this binary, printing the text on
 * stdout and timing on stderr. Returns a process exit status. */
int oc_stt_hear(const char *path);

#endif
