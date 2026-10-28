#include "tk_qr.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "qrcodegen.h"

#define QUIET 2   /* modules of light around the code: scanners want some */

int tk_qr_render(const char *text, char *out, size_t cap, int *cols, int *rows) {
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    if (!text || !qrcodegen_encodeText(text, tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN,
                                       qrcodegen_VERSION_MAX, qrcodegen_Mask_AUTO, true))
        return -1;
    int size = qrcodegen_getSize(qr), full = size + 2 * QUIET;
    *cols = full;
    *rows = (full + 1) / 2;
    size_t o = 0;
    for (int y = 0; y < full; y += 2) {
        for (int x = 0; x < full; x++) {
            /* Light is a module outside the code, or one the code leaves light. */
            int mx = x - QUIET, my = y - QUIET;
            int top = !(mx >= 0 && my >= 0 && mx < size && my < size && qrcodegen_getModule(qr, mx, my));
            int bot = y + 1 >= full ? 0 :
                      !(mx >= 0 && my + 1 >= 0 && mx < size && my + 1 < size && qrcodegen_getModule(qr, mx, my + 1));
            const char *c = top && bot ? "\xE2\x96\x88" : top ? "\xE2\x96\x80" : bot ? "\xE2\x96\x84" : " ";
            size_t n = strlen(c);
            if (o + n + 2 > cap) return -1;
            memcpy(out + o, c, n);
            o += n;
        }
        out[o++] = y + 2 < full ? '\n' : '\0';
    }
    out[o < cap ? o : cap - 1] = '\0';
    return 0;
}
