/*
 * OpenChime — read-aloud's embedded data (ARCH-111): the Kitten model in ONNX
 * Runtime's .ort format, its voices, and ttskit's dictionary and guesser, each as
 * bytes inside the openchimed binary (tts_embed.S). A trailing NUL follows each,
 * outside its length.
 */
#ifndef OC_TTS_EMBED_H
#define OC_TTS_EMBED_H

#include <stddef.h>

extern const unsigned char oc_tts_kitten_ort[], oc_tts_kitten_ort_end[];
extern const unsigned char oc_tts_kitten_voices[], oc_tts_kitten_voices_end[];
extern const unsigned char oc_tts_lexicon[], oc_tts_lexicon_end[];
extern const unsigned char oc_tts_guesses[], oc_tts_guesses_end[];

#define OC_TTS_BLOB_LEN(name) ((size_t)(name##_end - name))

#endif
