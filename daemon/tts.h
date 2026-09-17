/*
 * OpenChime — read-aloud synthesis built into the daemon (REQ-291–295, ARCH-111,
 * docs/READ-ALOUD.md §4).
 *
 * The Kitten engine (tts_render.h). The code is in the binary; the voice model,
 * its voices and ttskit's pronunciation data are files in a data directory
 * (tts_data.h), mapped when the engine opens. Compiled only into a daemon built
 * with read-aloud (make TTS=1, the default).
 */
#ifndef OC_TTS_H
#define OC_TTS_H

#include <stddef.h>

/* The model version renders are cached under: the model, the pronunciation data
 * (by the first bytes of each file's SHA-256) and the speaking rate. Change any of
 * them and change this, so old renders are made again rather than mixed in. */
/* The language this daemon speaks, as a BCP 47 tag. One language is built in; the
 * pronunciation data carries the same tag and is refused if it disagrees, and every
 * voice is announced with it (ARCH-111). Adding a second language is adding another
 * engine beside this one, not editing this line. */
#define OC_TTS_LANG "en-US"

#define OC_TTS_MODEL_VERSION OC_TTS_LANG "/kitten-mini-0.8/ttskit-b1338cd8.51cf60b3/rate-1.2/style-chars/tail-quiet"

/* The files a data directory must hold for this build, relative to it. */
extern const char *const OC_TTS_DATA_FILES[];

/* Find read-aloud's data directory and check it is the data this build expects
 * (tts_data.h). On success the engine opens from it and 1 is returned; otherwise
 * 0 with a reason, and read-aloud should be treated as absent. */
int oc_tts_data_ready(char *err, size_t errcap);

/* `openchimed --tts-manifest DIR`: hash the data files in DIR and write the
 * manifest this build will accept -- the daemon writes it, so the version it
 * names is exactly the version compiled in. Returns a process exit status. */
int oc_tts_manifest(const char *dir);

/* `openchimed --tts-say VOICE TEXT OUT`: render TEXT with the model in this binary
 * through the same path the daemon's renders take, writing the audio-only MP4 (or,
 * for a name ending in .wav, the raw 24 kHz PCM) and reporting timing on stderr —
 * for checking a build by ear. Returns a process exit status. */
int oc_tts_say(const char *voice, const char *text, const char *path);

#endif
