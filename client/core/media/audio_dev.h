/* The audio device layer (ARCH-110, docs/AUDIO.md §3.2, docs/VIDEO-MESSAGES.md
 * §3.3): device lists, capture and playback over miniaudio, with a lock-free
 * single-producer ring between each device callback and the media thread that
 * reads or writes it. The callbacks never allocate, lock or do I/O.
 *
 * Samples are signed 16-bit, interleaved. Captured samples are stamped from
 * oc_media_clock_us(), the clock video frames use.
 *
 * `OPENCHIME_TEST_AUDIO=synthetic` replaces the devices with a 440 Hz tone
 * source and a sink that consumes at real-time pace, so recording and playback
 * run in tests on a machine with no sound hardware. With it,
 * `OPENCHIME_TEST_MIC=<file.wav>` (16-bit PCM at the capture rate and channel
 * count) is what the synthetic microphone hears instead of the tone: the file
 * once, then silence, so voice input can be driven with real speech.
 * `OPENCHIME_TEST_AUDIO=mic-denied` is the synthetic devices with every capture
 * refused as OC_AUDIO_DENIED, as a microphone the operating system blocks is.
 * `OPENCHIME_TEST_TONE=<hz>` changes the synthetic microphone's tone. */
#ifndef OC_AUDIO_DEV_H
#define OC_AUDIO_DEV_H

#include <stddef.h>
#include <stdint.h>

enum {
    OC_AUDIO_OK       =  0,
    OC_AUDIO_DENIED   = -1,
    OC_AUDIO_NODEVICE = -2,
    OC_AUDIO_FAILED   = -3,
    /* The microphone is held by something else in this process: it has one
     * owner at a time -- a video recording, voice input or a call, never two
     * (ARCH-112). */
    OC_AUDIO_BUSY     = -4,
};

typedef struct {
    char id[520];            /* opaque (hex); pass to the open functions */
    char name[256];          /* UTF-8 */
    int  is_default;
} oc_audio_device;

typedef struct oc_audio_dev oc_audio_dev;

/* Capture (`capture` = 1) or playback devices, at most `cap`. Returns the count
 * or a negative OC_AUDIO_*. */
int oc_audio_list(int capture, oc_audio_device *out, int cap);

/* Open a device (NULL or "" for the default) at `rate` and `channels`; miniaudio
 * converts when the device runs otherwise. Both start running at once. NULL on
 * failure with `*err` set. */
oc_audio_dev *oc_audio_capture_open(const char *id, int rate, int channels, int *err);
oc_audio_dev *oc_audio_playback_open(const char *id, int rate, int channels, int *err);

/* The computer's own sound: what the output device (`output_id`, NULL for the
 * default) is playing, captured as it goes out -- WASAPI loopback. Read like a
 * capture device. It is not the microphone and takes nothing from its one owner.
 * A stretch where nothing played reads as silence at the right times.
 * Synthetic: a 660 Hz tone over quieter ones; `OPENCHIME_TEST_MIC_ECHO=1` makes
 * the synthetic microphone hear it back 20 ms later at half strength, over its
 * own tone, and `=only` makes that echo all it hears. */
oc_audio_dev *oc_audio_loopback_open(const char *output_id, int rate, int channels, int *err);

/* Capture: take up to `frames` frames. `*pts_us` receives the capture time of
 * the first one returned. Returns the frame count (0 when none are waiting). */
size_t oc_audio_capture_read(oc_audio_dev *d, int16_t *pcm, size_t frames, int64_t *pts_us);
/* Frames lost because the reader fell behind, since open. */
uint64_t oc_audio_capture_overruns(oc_audio_dev *d);

/* Playback: queue up to `frames` frames; returns how many fit. */
size_t oc_audio_playback_write(oc_audio_dev *d, const int16_t *pcm, size_t frames);
/* Frames queued but not yet handed to the device. */
size_t oc_audio_playback_queued(oc_audio_dev *d);
/* Frames the device has consumed since open (silence it played for an empty
 * ring does not count). The player's master clock. */
uint64_t oc_audio_playback_position(oc_audio_dev *d);
/* Drop everything queued (a seek). */
void oc_audio_playback_flush(oc_audio_dev *d);
/* Output gain 0..1 (mute is 0). */
void oc_audio_playback_volume(oc_audio_dev *d, float gain);

/* The far-end reference (docs/AUDIO.md §3.3, §6): everything every open playback
 * device has handed out, mixed to mono and resampled to 16 kHz, on the media
 * clock. Writes the `n` samples that were played from `start_us` onward into
 * `out` (silence where nothing was), for an echo canceller to align with a
 * capture frame stamped `start_us`. Returns how many devices contributed. */
#define OC_AUDIO_REF_RATE 16000
int oc_audio_reference(int64_t start_us, int16_t *out, size_t n);

/* Peak level of the most recent callback's samples, 0..32767 (the level meter). */
int oc_audio_level(oc_audio_dev *d);

void oc_audio_close(oc_audio_dev *d);

#endif
