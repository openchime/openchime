/*
 * OpenChime client — graphics primitive seam (ARCH-63).
 *
 * A narrow immediate-mode 2D API (the openblocks gfx.h pattern). client/ui.c
 * and app code draw through this; only client/gfx_raylib.c includes raylib, so
 * a later iOS/Metal backend can swap the graphics stack by providing a second
 * implementation of this one header. No raylib types leak past this seam.
 */

#ifndef OC_GFX_H
#define OC_GFX_H

#include <stdbool.h>

typedef struct { float x, y, w, h; } gfx_rect;
typedef struct { unsigned char r, g, b, a; } gfx_color;

/* Window / frame lifecycle. */
void gfx_init(int width, int height, const char *title);
bool gfx_should_close(void);
void gfx_close(void);
void gfx_begin_frame(void);
void gfx_end_frame(void);
int  gfx_width(void);
int  gfx_height(void);

/* Primitives. */
void gfx_clear(gfx_color c);
void gfx_fill_rect(gfx_rect r, gfx_color c);
void gfx_line(float x1, float y1, float x2, float y2, gfx_color c);
void gfx_text(const char *s, float x, float y, int size, gfx_color c);
int  gfx_measure_text(const char *s, int size);

/* Input, funneled into plain values (no key-code leakage past this seam). The
 * message view/composer only needs these; richer input arrives with the real
 * UI phase. */
int   gfx_char_pressed(void);     /* next Unicode codepoint typed this frame, 0 if none */
bool  gfx_enter_pressed(void);
bool  gfx_backspace_pressed(void);
float gfx_mouse_wheel(void);      /* scroll delta this frame */

#endif /* OC_GFX_H */
