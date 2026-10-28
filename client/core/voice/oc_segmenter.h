/* Where an utterance ends (ARCH-112, docs/VOICE-INPUT.md §4): 20 ms frames of
 * 16 kHz mono in, whole segments of speech out, over libfvad's per-frame
 * speech/not-speech answer.
 *
 * Free talk (`held` 0): a segment opens on a short run of speech, keeps a
 * pre-roll so the first syllable survives, and closes after a pause long enough
 * not to be a breath; one holding too little speech is dropped. Push to talk
 * (`held` 1): every frame belongs to the segment until the key is let go
 * (oc_seg_finish); a press with too little speech in it is dropped. Either way a
 * segment reaching the cap is cut at its last pause, or at the cap if there was
 * none, and the rest carries on as the next. */
#ifndef OC_SEGMENTER_H
#define OC_SEGMENTER_H

#include <stddef.h>
#include <stdint.h>

#define OC_SEG_RATE  16000
#define OC_SEG_FRAME 320                /* 20 ms */

/* The shape of a segment, in frames (tuned by ear against recordings; the
 * reasoning is in VOICE-INPUT.md §4). */
#define OC_SEG_ONSET_FRAMES     3       /* 60 ms of speech opens a segment */
#define OC_SEG_PREROLL_FRAMES   15      /* 300 ms kept from before the onset */
#define OC_SEG_END_FRAMES       35      /* 700 ms of quiet closes it */
#define OC_SEG_TAIL_FRAMES      10      /* 200 ms of that quiet is kept */
#define OC_SEG_PAUSE_FRAMES     10      /* a 200 ms gap is a place to cut at the cap */
#define OC_SEG_MIN_SPEECH       20      /* free talk: under 400 ms of speech is noise */
#define OC_SEG_MIN_SPEECH_HELD  8       /* push to talk: 160 ms -- "yes" is a word */
#define OC_SEG_WARMUP_FRAMES    10      /* libfvad's first 200 ms are not to be believed */

typedef struct oc_segmenter oc_segmenter;

/* `max_samples` is the cap (the daemon's max_segment_ms at 16 kHz). NULL on
 * failure. `vad_mode` is libfvad's aggressiveness, 0..3. */
oc_segmenter *oc_seg_new(int held, size_t max_samples, int vad_mode);
void          oc_seg_free(oc_segmenter *s);

/* One frame of OC_SEG_FRAME samples. Returns 1 when a segment is ready. */
int oc_seg_push(oc_segmenter *s, const int16_t *frame);
/* The input has ended (push to talk let go). Returns 1 when a segment is ready. */
int oc_seg_finish(oc_segmenter *s);
/* The ready segment: malloc'd samples the caller frees, and their count; NULL
 * when none is ready. */
int16_t *oc_seg_take(oc_segmenter *s, size_t *samples);

/* Whether the detector heard speech in the last frame (the live mark). */
int oc_seg_speaking(const oc_segmenter *s);

#endif
