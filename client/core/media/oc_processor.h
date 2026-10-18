/* The processor seam between capture and whatever consumes it (docs/AUDIO.md
 * §3.3): a vtable in the shape of oc_blob_backend, so an echo canceller is a
 * swap rather than a restructure. `process` receives the capture frame AND the
 * far-end frame played at the same instant (oc_audio_reference), which is what
 * lets a canceller align them; it rewrites the capture frame in place.
 *
 * Two processors: none, and speexdsp's linear acoustic echo canceller (AUDIO.md
 * §6.2). Voice input builds this first (ARCH-112); the call
 * client uses the same seam. */
#ifndef OC_PROCESSOR_H
#define OC_PROCESSOR_H

#include <stdint.h>

typedef struct {
    const char *name;
    void *(*open)(int sample_rate, int frame_samples);
    void  (*close)(void *p);
    /* Both directions, same frame, so a canceller can align them. */
    void  (*process)(void *p, int16_t *capture, const int16_t *playback, int frame_samples);
} oc_audio_processor;

extern const oc_audio_processor OC_PROCESSOR_NONE;
/* speexdsp's canceller, modelling up to OC_AEC_TAIL_MS of echo path: the
 * device's output and input buffering and the room together. */
#define OC_AEC_TAIL_MS 300
extern const oc_audio_processor OC_PROCESSOR_SPEEX;
/* The same canceller for full-band 48 kHz audio -- a screen recording's
 * microphone against the computer's sound (REQ-162). speexdsp run at 48 kHz
 * models the echo path with three times the taps and, measured in the ERLE
 * harness, keeps too little of a voice talking over it; so the canceller runs at
 * 16 kHz, where it is proven, and the echo it finds there is taken out of the
 * full-band microphone, which keeps everything above 8 kHz. Frames are 48 kHz
 * and a multiple of 3; the output is 1 ms later than the input. */
extern const oc_audio_processor OC_PROCESSOR_SPEEX_48K;

#endif
