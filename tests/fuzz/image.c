/* Harness: the image-preview decoders (ARCH-137, daemon/preview.c) -- the
 * daemon's first parser of untrusted media. The input is taken as an uploaded
 * file and made into previews under boxes small enough that a JPEG, PNG or WebP
 * of any size is decoded rather than passed as small, with the pixel cap low so
 * a header claiming a huge image is refused there. What must hold: the answer
 * is one of the statuses; a made preview has both rungs, each an image its own
 * header reads back at the size it claims; a refused one holds no rung. The
 * corpus is a small JPEG, PNG and WebP of each kind. */

#include "fuzz.h"
#include "preview.h"

#define H "image"

int oc_fuzz_image(const uint8_t *data, size_t len) {
    oc_preview_limits lim;
    oc_preview_limits_default(&lim);
    lim.inline_px = 16;
    lim.expanded_px = 40;
    lim.max_bytes = 1u << 20;
    lim.max_pixels = 1u << 20;
    uint32_t w, h;
    (void)oc_preview_probe(data, len, &w, &h);
    oc_preview_set s;
    oc_preview_status st = oc_preview_make(data, len, &lim, &s);
    if (st < OC_PREVIEW_MADE || st > OC_PREVIEW_NO_MEMORY) OC_FUZZ_BROKEN(H, "an answer that is no status");
    if (st == OC_PREVIEW_MADE) {
        for (int r = 0; r < OC_PREVIEW_RUNGS; r++) {
            if (!s.rung[r].data || !s.rung[r].len) OC_FUZZ_BROKEN(H, "a made preview without a rung");
            if (oc_preview_probe(s.rung[r].data, s.rung[r].len, &w, &h) != 0 ||
                w != s.rung[r].width || h != s.rung[r].height)
                OC_FUZZ_BROKEN(H, "a rung that is not the image it claims");
        }
    } else if (s.rung[0].data || s.rung[1].data) {
        OC_FUZZ_BROKEN(H, "a refused preview that holds a rung");
    }
    oc_preview_set_free(&s);
    return 0;
}
