/* The one frame format of a video message (ARCH-110, docs/details/VIDEO-MESSAGES.md §3.1):
 * planar I420, BT.709 limited range, even dimensions. Shared because the clients
 * capture, encode and play it, and the daemon decodes it to export a video
 * message (ARCH-135). The rest of the media types are the clients' (oc_media.h). */
#ifndef OC_FRAME_H
#define OC_FRAME_H

#include <stdint.h>

typedef struct {
    int      width, height;      /* even, > 0 */
    int64_t  pts_us;             /* oc_media_clock_us() when captured */
    uint8_t *plane[3];           /* Y, U, V */
    int      stride[3];          /* bytes per row, >= plane width */
} oc_frame;

#endif
