/* The device code's QR code as terminal text (tuikit/tk_qr.c, AUTH.md §8.11):
 * the grid qrcodegen encodes, two modules to a cell, light modules as blocks,
 * with its quiet zone -- checked where a QR code's shape is fixed, the corner
 * finder patterns; the encoding itself is qrcodegen's. */

#include "check.h"
#include "tk_qr.h"
#include "qrcodegen.h"

#include <stdio.h>
#include <string.h>

/* The cell at (col, row) of rendered text, as the string of its glyph. */
static void cell(const char *out, int col, int row, char *g, size_t cap) {
    const char *r = out;
    for (int i = 0; i < row && r; i++) { r = strchr(r, '\n'); if (r) r++; }
    g[0] = '\0';
    if (!r) return;
    for (int c = 0; *r && *r != '\n'; c++) {
        size_t n = (unsigned char)*r >= 0xE0 ? 3 : 1;
        if (c == col) { snprintf(g, cap, "%.*s", (int)n, r); return; }
        r += n;
    }
}

int run_tkqr_tests(void) {
    printf("test_tkqr: a URL's QR code as half-block text -- size, quiet zone, finder patterns\n");
    const char *url = "https://chat.example.com:8443/device?code=WDJB-MJHT";
    char out[16384]; int cols = 0, rows = 0;
    CHECK(tk_qr_render(url, out, sizeof out, &cols, &rows) == 0);
    uint8_t qr[qrcodegen_BUFFER_LEN_MAX], tmp[qrcodegen_BUFFER_LEN_MAX];
    CHECK(qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_LOW, qrcodegen_VERSION_MIN, qrcodegen_VERSION_MAX,
                               qrcodegen_Mask_AUTO, true));
    int size = qrcodegen_getSize(qr);
    CHECK(cols == size + 4 && rows == (size + 5) / 2);
    int lines = 1;
    for (const char *p = out; *p; p++) lines += *p == '\n';
    CHECK(lines == rows);
    char g[8];
    /* The quiet zone: the first row of cells is light top and bottom. */
    for (int c = 0; c < cols; c++) { cell(out, c, 0, g, sizeof g); CHECK(strcmp(g, "\xE2\x96\x88") == 0); }
    /* The top-left finder: its top edge (module row 2) and the row under it
     * (module row 3) are dark at column 2 -- a dark cell -- and dark over light
     * at column 3, inside the ring. */
    cell(out, 2, 1, g, sizeof g); CHECK(strcmp(g, " ") == 0);
    cell(out, 3, 1, g, sizeof g); CHECK(strcmp(g, "\xE2\x96\x84") == 0);
    /* ...and the quiet zone at the left edge stays light all the way down. */
    for (int r = 0; r < rows; r++) { cell(out, 0, r, g, sizeof g); CHECK(g[0] && strcmp(g, " ") != 0); }
    /* Too small a buffer is refused, never truncated. */
    CHECK(tk_qr_render(url, out, 64, &cols, &rows) == -1);
    return failures;
}
