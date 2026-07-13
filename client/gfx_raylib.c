/*
 * OpenChime client — raylib implementation of the gfx.h seam (ARCH-63).
 * The only translation unit that includes raylib.
 */

#include "gfx.h"

#include <raylib.h>

static Color to_ray(gfx_color c) {
    Color r = { c.r, c.g, c.b, c.a };
    return r;
}

void gfx_init(int width, int height, const char *title) {
    SetTraceLogLevel(LOG_WARNING);
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(width, height, title);
    SetTargetFPS(60);
}

bool gfx_should_close(void) { return WindowShouldClose(); }
void gfx_close(void)        { CloseWindow(); }
void gfx_begin_frame(void)  { BeginDrawing(); }
void gfx_end_frame(void)    { EndDrawing(); }
int  gfx_width(void)        { return GetScreenWidth(); }
int  gfx_height(void)       { return GetScreenHeight(); }

void gfx_clear(gfx_color c) { ClearBackground(to_ray(c)); }

void gfx_fill_rect(gfx_rect r, gfx_color c) {
    DrawRectangleRec((Rectangle){ r.x, r.y, r.w, r.h }, to_ray(c));
}

void gfx_line(float x1, float y1, float x2, float y2, gfx_color c) {
    DrawLine((int)x1, (int)y1, (int)x2, (int)y2, to_ray(c));
}

void gfx_text(const char *s, float x, float y, int size, gfx_color c) {
    DrawText(s, (int)x, (int)y, size, to_ray(c));
}

int gfx_measure_text(const char *s, int size) {
    return MeasureText(s, size);
}

int   gfx_char_pressed(void)     { return GetCharPressed(); }
bool  gfx_enter_pressed(void)    { return IsKeyPressed(KEY_ENTER) || IsKeyPressedRepeat(KEY_ENTER); }
bool  gfx_backspace_pressed(void){ return IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE); }
float gfx_mouse_wheel(void)      { return GetMouseWheelMove(); }
