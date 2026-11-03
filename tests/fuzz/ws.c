/* Harness: a browser's WebSocket frames (oc_ws_parse, daemon/http.c), read
 * one after another off the input as the I/O thread reads them. A frame's
 * masking key and payload lie inside the bytes it was parsed from, and a
 * frame consumes at least its header. */

#include "fuzz.h"
#include "http.h"

#define H "ws"

int oc_fuzz_ws(const uint8_t *data, size_t len) {
    size_t at = 0;
    while (at < len) {
        oc_ws_frame f;
        int rc = oc_ws_parse(data + at, len - at, OC_MAX_FRAME_SIZE + 64u, &f);
        if (rc == OC_WS_MORE || rc == OC_WS_REFUSE) break;
        if (rc != OC_WS_FRAME) OC_FUZZ_BROKEN(H, "an answer that is none of FRAME, MORE, REFUSE");
        if (f.whole < 6 || f.whole > len - at) OC_FUZZ_BROKEN(H, "a frame longer than the input");
        if (f.mask_off + 4 != f.payload_off || f.payload_off + f.len != f.whole)
            OC_FUZZ_BROKEN(H, "a frame whose parts do not add up");
        at += f.whole;
    }
    return 0;
}
