/* VP9 decode over libvpx, shared by the clients' player and call engine and the
 * daemon's export of a video message (ARCH-110, ARCH-135). The encoders stay the
 * clients' (client/core/media/oc_codec.h). */
#ifndef OC_VP9DEC_H
#define OC_VP9DEC_H

#include "oc_frame.h"

#include <stddef.h>
#include <stdint.h>

typedef struct oc_vp9dec oc_vp9dec;

/* A decoder using up to `threads` threads (at least 1). */
oc_vp9dec *oc_vp9dec_open_threads(int threads);
/* The same, with one thread fewer than the machine has cores, at most four. */
oc_vp9dec *oc_vp9dec_open(void);
/* Decode one sample. On success `out` points at the decoder's own frame
 * (valid until the next call) and 0 is returned; 1 means no frame this time;
 * -1 an error. `out->pts_us` is set to `pts_us`. */
int  oc_vp9dec_decode(oc_vp9dec *d, const uint8_t *data, size_t len, int64_t pts_us, oc_frame *out);
void oc_vp9dec_close(oc_vp9dec *d);

#endif
