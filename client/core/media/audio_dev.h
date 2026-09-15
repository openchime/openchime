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
 * run in tests on a machine with no sound hardware. */
#ifndef OC_AUDIO_DEV_H
#define OC_AUDIO_DEV_H

#include <stddef.h>
#include <stdint.h>

enum {
    OC_AUDIO_OK       =  0,
    OC_AUDIO_DENIED   = -1,
    OC_AUDIO_NODEVICE = -2,
    OC_AUDIO_FAILED   = -3,
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

/* Peak level of the most recent callback's samples, 0..32767 (the level meter). */
int oc_audio_level(oc_audio_dev *d);

void oc_audio_close(oc_audio_dev *d);

#endif
