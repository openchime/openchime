/* OpenChime — read-aloud's data, embedded in openchimed (ARCH-111, tts_embed.h).
 *
 * The voice model and the pronunciation data become read-only sections of the
 * executable, so the daemon is one file with nothing to install beside it. The
 * kernel pages them in from the binary as they are read, exactly as it would a
 * mapped file, and can drop them again: embedding costs disk, not memory.
 *
 * Paths are relative to the repository root, where make runs. The model is built
 * by scripts/build_kitten.sh; the ttskit data is committed. */

#define BLOB(name, path)                         \
    .section .rodata.oc_tts, "a", %progbits;     \
    .balign 64;                                  \
    .globl name;                                 \
name:                                            \
    .incbin path;                                \
    .globl name##_end;                           \
name##_end:                                      \
    .byte 0

BLOB(oc_tts_kitten_ort, "build/kitten/kitten.ort")
BLOB(oc_tts_kitten_voices, "build/kitten/voices.npz")
BLOB(oc_tts_lexicon, "ttskit/data/lexicon.bin")
BLOB(oc_tts_guesses, "ttskit/data/guesses.bin")

/* Not an executable stack. */
.section .note.GNU-stack, "", %progbits
