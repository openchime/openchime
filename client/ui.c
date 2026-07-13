#include "ui.h"
#include "gfx.h"

#include <stdio.h>

void oc_ui_draw(const oc_app *a) {
    const gfx_color bg   = { 24, 24, 28, 255 };
    const gfx_color bar  = { 36, 36, 44, 255 };
    const gfx_color comp = { 30, 30, 38, 255 };
    const gfx_color text = { 230, 230, 235, 255 };
    const gfx_color dim  = { 150, 150, 160, 255 };

    int W = gfx_width(), H = gfx_height();
    gfx_clear(bg);

    /* Status bar. */
    gfx_fill_rect((gfx_rect){ 0, 0, (float)W, 28 }, bar);
    gfx_text(a->status, 10, 6, 16, a->connected ? text : dim);

    /* Message list (bottom-anchored via scroll). */
    int y = 40 + (int)a->scroll;
    for (size_t i = 0; i < a->n_msgs; i++) {
        char line[600];
        snprintf(line, sizeof line, "u%llu:  %s",
                 (unsigned long long)a->msgs[i].author_id,
                 a->msgs[i].body ? a->msgs[i].body : "");
        if (y > 28 && y < H - 36) gfx_text(line, 10, (float)y, 16, text);
        y += 22;
    }

    /* Composer. */
    int cy = H - 32;
    gfx_fill_rect((gfx_rect){ 0, (float)cy, (float)W, 32 }, comp);
    char c[560];
    snprintf(c, sizeof c, "> %s", a->composer);
    gfx_text(c, 10, (float)cy + 8, 16, text);
}
