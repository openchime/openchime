/* The audio device layer's platform seam (audio_dev.h, docs/AUDIO.md §3.2).
 * audio_dev.c owns everything that is the same everywhere -- the rings, the
 * timestamps, the far-end reference, the microphone's one owner, the
 * synthetic test devices -- and a backend owns the devices: how they are
 * listed and opened, and the callbacks that move samples. Two backends:
 * miniaudio (audio_ma.c: Windows, Linux, macOS, and the phones when they
 * come) and Web Audio (client/gui/platform/web/audio_web.c). Each platform's
 * client links exactly one, named by oc_audio_platform_backend().
 *
 * A backend's device callback hands samples to the two functions below and
 * does nothing else: no allocation, no lock, no I/O (they are real-time safe
 * and so must the callback be). */
#ifndef OC_AUDIO_BACKEND_H
#define OC_AUDIO_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#include "audio_dev.h"

typedef struct {
    /* As oc_audio_list. */
    int   (*list)(int capture, oc_audio_device *out, int cap);
    /* Open and start the device `d` describes (oc_audio_dev_rate/channels;
     * `capture`, `loopback` as the open functions), delivering through
     * oc_audio_dev_capture_in / oc_audio_dev_playback_out. 0, or a negative
     * OC_AUDIO_*. The backend keeps what it needs in oc_audio_dev_impl. */
    int   (*open)(oc_audio_dev *d, int capture, int loopback, const char *id);
    void  (*close)(oc_audio_dev *d);
} oc_audio_backend;

const oc_audio_backend *oc_audio_platform_backend(void);

/* What the device is, for the backend. */
int    oc_audio_dev_rate(const oc_audio_dev *d);
int    oc_audio_dev_channels(const oc_audio_dev *d);
void **oc_audio_dev_impl(oc_audio_dev *d);

/* `frames` interleaved frames the device captured: into the ring, stamped. */
void   oc_audio_dev_capture_in(oc_audio_dev *d, const int16_t *pcm, size_t frames);
/* Fill `frames` interleaved frames for the device to play: from the ring,
 * with the gain applied and silence where it is empty. */
void   oc_audio_dev_playback_out(oc_audio_dev *d, int16_t *pcm, size_t frames);

#endif
