/* Opus decode over libopus, shared by the clients' player and call engine and the
 * daemon's export of a video message (ARCH-110, ARCH-135). The encoders stay the
 * clients' (client/core/media/oc_codec.h). */
#ifndef OC_OPUSDEC_H
#define OC_OPUSDEC_H

#include <stddef.h>
#include <stdint.h>

#define OC_OPUS_RATE        48000

typedef struct oc_opusdec oc_opusdec;

oc_opusdec *oc_opusdec_open(int channels);
/* Decode one packet into mono PCM at OC_OPUS_RATE (a stereo stream is
 * downmixed). Returns the number of samples written (at most `cap`) or -1. */
int  oc_opusdec_decode(oc_opusdec *d, const uint8_t *data, size_t len, int16_t *pcm, int cap);
void oc_opusdec_close(oc_opusdec *d);

/* A mono decoder at `rate`. */
oc_opusdec *oc_opusdec_open_rate(int rate);
/* One frame of `frame` samples: `data` decoded; or, with `fec`, the frame
 * BEFORE `data` rebuilt from the redundancy `data` carries; or, with `data`
 * NULL, a frame concealed from what came before (PLC). Returns samples or -1. */
int  oc_opusdec_decode_frame(oc_opusdec *d, const uint8_t *data, size_t len, int fec,
                             int16_t *pcm, int frame);

#endif
