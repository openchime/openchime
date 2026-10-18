/* A dictation session (REQ-296-300, ARCH-112, docs/VOICE-INPUT.md): the
 * microphone open for one conversation, in one mode, until stopped.
 *
 * A capture thread reads 20 ms frames at 16 kHz, cancels the client's own
 * playback out of them (the speexdsp processor against the device layer's
 * far-end reference), finds where each utterance ends and sends it to the
 * daemon (oc_client_stt_send). In free talk the daemon posts each one; in push
 * to talk the words come back for the composer. Nothing is written to disk. */
#ifndef OC_DICTATE_H
#define OC_DICTATE_H

#include <stddef.h>
#include <stdint.h>

#include "client.h"

enum {
    OC_DICTATE_OK       =  0,
    OC_DICTATE_DENIED   = -1,   /* the operating system blocks the microphone */
    OC_DICTATE_NODEVICE = -2,
    OC_DICTATE_BUSY     = -3,   /* a recording (or a call) holds the microphone */
    OC_DICTATE_FAILED   = -4,
};

typedef struct oc_dictate oc_dictate;

/* Open the microphone (`mic_id` NULL for the default) and start. `mode` is
 * OC_STT_MODE_PTT or OC_STT_MODE_FREE; `max_ms` the daemon's segment cap
 * (STT_INFO). NULL on failure with *err set. */
oc_dictate *oc_dictate_start(oc_client *c, uint8_t mode, uint64_t channel_id, uint64_t thread_root,
                             const char *mic_id, uint32_t max_ms, int *err);

/* Stop and close the microphone. `send_rest` 1 sends what was being said (push
 * to talk let go); 0 drops it (cancelled). Frees the session. */
void oc_dictate_stop(oc_dictate *d, int send_rest);

uint8_t  oc_dictate_mode(const oc_dictate *d);
uint64_t oc_dictate_channel(const oc_dictate *d);
uint64_t oc_dictate_thread_root(const oc_dictate *d);
/* The microphone's peak level, 0..32767, after echo cancellation. */
int      oc_dictate_level(const oc_dictate *d);
/* Whether the detector hears speech right now. */
int      oc_dictate_speaking(const oc_dictate *d);
/* Segments sent so far. */
uint32_t oc_dictate_sent(const oc_dictate *d);

#endif
