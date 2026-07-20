/*
 * OpenChime GUI (ARCH-80, take 2) — a self-rendered, cross-platform client over
 * the shared app-core (client/core), drawn with Clay (layout) on raylib (window
 * + GPU + text). One GUI codebase for Linux/Windows/macOS, replacing the native
 * per-platform widgets that couldn't reach a modern look. Built on Linux first.
 *
 * This file is the login surface: a centered card with Workspace / Username /
 * Password fields and a Connect button, wired to oc_resolve +
 * oc_client_start_secure. The chat surface lands next in the same window.
 */

#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "clay_renderer_raylib.c"   /* Raylib_MeasureText, Clay_Raylib_Initialize/Render/Close; pulls raylib.h */

#include "client.h"
#include "model.h"
#include "resolve.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- fonts + palette ------------------------------------------------------ */
enum { FONT_BODY = 0, FONT_BOLD = 1, FONT_COUNT };
static Font g_fonts[FONT_COUNT];

static const Clay_Color BG        = { 20, 17, 28, 255 };
static const Clay_Color CARD      = { 36, 33, 48, 255 };
static const Clay_Color FIELD     = { 28, 25, 38, 255 };
static const Clay_Color FIELD_FOC = { 33, 30, 46, 255 };
static const Clay_Color ACCENT    = { 99, 102, 241, 255 };   /* indigo-500 */
static const Clay_Color ACCENT_HI = { 129, 132, 248, 255 };
static const Clay_Color TXT       = { 236, 236, 242, 255 };
static const Clay_Color TXT_MUTE  = { 148, 146, 168, 255 };
static const Clay_Color BORDER    = { 60, 56, 78, 255 };
static const Clay_Color ERR_COL   = { 248, 113, 113, 255 };
static const Clay_Color OK_COL    = { 74, 222, 128, 255 };

/* ---- login state ---------------------------------------------------------- */
typedef enum { PH_FORM, PH_CONNECTING, PH_DONE, PH_ERROR } Phase;

static char  f_ws[256]   = "127.0.0.1:8443";
static char  f_user[128] = "";
static char  f_pass[128] = "";
static int   g_focus     = 1;      /* 0 = ws, 1 = user, 2 = pass */
static Phase g_phase     = PH_FORM;
static char  g_status[192] = "";
static oc_client *g_cl   = NULL;
static float g_caret_t   = 0;
static int   g_caret_on  = 1;

/* Non-owning Clay_String over a live buffer (buffers are static, so valid
 * through EndLayout + render). */
static Clay_String cs(const char *b, int n) {
    return (Clay_String){ .isStaticallyAllocated = false, .length = n, .chars = b };
}

/* Append a Unicode codepoint (UTF-8) to a fixed buffer. */
static void buf_append_cp(char *buf, size_t cap, int cp) {
    size_t n = strlen(buf);
    char tmp[4]; int len = 0;
    if (cp < 0x80) { tmp[0] = (char)cp; len = 1; }
    else if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); len = 2; }
    else if (cp < 0x10000) { tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[2] = (char)(0x80 | (cp & 0x3F)); len = 3; }
    else { tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); len = 4; }
    if (n + (size_t)len < cap) { memcpy(buf + n, tmp, len); buf[n + len] = '\0'; }
}
/* Delete the last UTF-8 codepoint. */
static void buf_backspace(char *buf) {
    size_t n = strlen(buf);
    if (!n) return;
    do { n--; } while (n > 0 && (buf[n] & 0xC0) == 0x80);
    buf[n] = '\0';
}

static char *focused_buf(size_t *cap) {
    if (g_focus == 0) { *cap = sizeof f_ws;   return f_ws; }
    if (g_focus == 1) { *cap = sizeof f_user; return f_user; }
    *cap = sizeof f_pass; return f_pass;
}

/* ---- connect --------------------------------------------------------------- */
static void do_submit(void) {
    if (g_phase == PH_CONNECTING) return;
    if (!f_ws[0])   { snprintf(g_status, sizeof g_status, "Enter a workspace (e.g. 127.0.0.1:8443)"); g_focus = 0; return; }
    if (!f_user[0]) { snprintf(g_status, sizeof g_status, "Enter a username"); g_focus = 1; return; }

    oc_endpoint ep;
    oc_resolve_status st = oc_resolve(f_ws, getenv("OPENCHIME_SUFFIX"), &ep);
    if (st != OC_RESOLVE_OK) {
        snprintf(g_status, sizeof g_status, "Could not resolve '%s'", f_ws);
        g_focus = 0; g_phase = PH_ERROR; return;
    }
    char cred[264];
    snprintf(cred, sizeof cred, "%s:%s", f_user, f_pass);
    g_cl = oc_client_start_secure(ep.host, ep.port, cred, NULL, NULL);
    if (!g_cl) { snprintf(g_status, sizeof g_status, "Failed to start client"); g_phase = PH_ERROR; return; }
    g_phase = PH_CONNECTING;
    snprintf(g_status, sizeof g_status, "Connecting...");
}

static void tick_connecting(void) {
    if (!g_cl) return;
    oc_client_tick(g_cl);
    const oc_model *m = oc_client_model(g_cl);
    if (m->authed) {
        const char *me = m->user_id ? oc_model_user_name(m, m->user_id) : "";
        snprintf(g_status, sizeof g_status, "Signed in as %s  \xC2\xB7  %zu channel%s",
                 me[0] ? me : "you", m->n_channels, m->n_channels == 1 ? "" : "s");
        g_phase = PH_DONE;
        return;
    }
    if (m->last_error[0] && !m->connected) {
        snprintf(g_status, sizeof g_status, "%s", m->last_error);
        oc_client_stop(g_cl); g_cl = NULL;
        g_phase = PH_ERROR;
        return;
    }
    snprintf(g_status, sizeof g_status, "%s", m->connected ? "Authenticating..." : "Connecting...");
}

/* ---- input ---------------------------------------------------------------- */
static void set_focus_from_click(void) {
    if (Clay_PointerOver(CLAY_ID("field_ws")))   g_focus = 0;
    if (Clay_PointerOver(CLAY_ID("field_user"))) g_focus = 1;
    if (Clay_PointerOver(CLAY_ID("field_pass"))) g_focus = 2;
    if (Clay_PointerOver(CLAY_ID("connect_btn"))) do_submit();
}

static void process_input(void) {
    if (g_phase == PH_CONNECTING) { tick_connecting(); return; }
    if (g_phase == PH_DONE) return;

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) set_focus_from_click();

    if (IsKeyPressed(KEY_TAB)) {
        int dir = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? 2 : 1;
        g_focus = (g_focus + dir) % 3;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) { do_submit(); return; }

    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        size_t cap; buf_backspace(focused_buf(&cap));
        if (g_phase == PH_ERROR) g_phase = PH_FORM;
    }
    int cp;
    while ((cp = GetCharPressed()) != 0) {
        if (cp < 0x20) continue;
        size_t cap; buf_append_cp(focused_buf(&cap), cap, cp);
        if (g_phase == PH_ERROR) g_phase = PH_FORM;
    }
}

/* ---- layout --------------------------------------------------------------- */
static void ui_field(Clay_ElementId id, const char *label, const char *value,
                     int focused, int masked, const char *placeholder) {
    CLAY_AUTO_ID({ .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM,
                               .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = 7 } }) {
        CLAY_TEXT(cs(label, (int)strlen(label)),
                  CLAY_TEXT_CONFIG({ .fontId = FONT_BOLD, .fontSize = 12, .textColor = TXT_MUTE, .letterSpacing = 1 }));
        CLAY(id, { .backgroundColor = focused ? FIELD_FOC : FIELD,
                   .cornerRadius = CLAY_CORNER_RADIUS(8),
                   .border = { .color = focused ? ACCENT : BORDER, .width = CLAY_BORDER_OUTSIDE(focused ? 2 : 1) },
                   .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(46) },
                               .padding = { 14, 14, 0, 0 }, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 2 } }) {
            static char maskbuf[130];
            const char *shown = value;
            int has = value[0] != '\0';
            if (masked && has) {
                int n = 0; for (const char *p = value; *p; p++) if ((*p & 0xC0) != 0x80 && n < 128) maskbuf[n++] = '*';
                maskbuf[n] = '\0'; shown = maskbuf;
            }
            if (has)
                CLAY_TEXT(cs(shown, (int)strlen(shown)),
                          CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 17, .textColor = TXT }));
            else
                CLAY_TEXT(cs(placeholder, (int)strlen(placeholder)),
                          CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 17, .textColor = TXT_MUTE }));
            if (focused && g_caret_on)
                CLAY_AUTO_ID({ .backgroundColor = TXT,
                               .layout = { .sizing = { .width = CLAY_SIZING_FIXED(2), .height = CLAY_SIZING_FIXED(20) } } }) {}
        }
    }
}

static Clay_RenderCommandArray build_layout(void) {
    Clay_BeginLayout();

    CLAY(CLAY_ID("root"), {
        .backgroundColor = BG,
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                    .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } } }) {

        CLAY(CLAY_ID("card"), {
            .backgroundColor = CARD,
            .cornerRadius = CLAY_CORNER_RADIUS(16),
            .border = { .color = BORDER, .width = CLAY_BORDER_OUTSIDE(1) },
            .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM,
                        .sizing = { .width = CLAY_SIZING_FIXED(400) },
                        .padding = { 36, 36, 34, 34 }, .childGap = 18 } }) {

            CLAY_TEXT(CLAY_STRING("OpenChime"),
                      CLAY_TEXT_CONFIG({ .fontId = FONT_BOLD, .fontSize = 30, .textColor = TXT }));
            CLAY_TEXT(CLAY_STRING("Sign in to your workspace"),
                      CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 15, .textColor = TXT_MUTE }));

            CLAY_AUTO_ID({ .layout = { .sizing = { .height = CLAY_SIZING_FIXED(4) } } }) {}

            ui_field(CLAY_ID("field_ws"),   "WORKSPACE", f_ws,   g_focus == 0, 0, "domain or host:port");
            ui_field(CLAY_ID("field_user"), "USERNAME",  f_user, g_focus == 1, 0, "you");
            ui_field(CLAY_ID("field_pass"), "PASSWORD",  f_pass, g_focus == 2, 1, "password");

            int connecting = (g_phase == PH_CONNECTING);
            int btn_hover = Clay_PointerOver(CLAY_ID("connect_btn"));
            CLAY(CLAY_ID("connect_btn"), {
                .backgroundColor = connecting ? BORDER : (btn_hover ? ACCENT_HI : ACCENT),
                .cornerRadius = CLAY_CORNER_RADIUS(9),
                .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(46) },
                            .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } } }) {
                const char *blabel = connecting ? "Connecting..." : "Connect";
                CLAY_TEXT(cs(blabel, (int)strlen(blabel)),
                          CLAY_TEXT_CONFIG({ .fontId = FONT_BOLD, .fontSize = 16, .textColor = TXT }));
            }

            if (g_status[0]) {
                Clay_Color sc = g_phase == PH_ERROR ? ERR_COL : g_phase == PH_DONE ? OK_COL : TXT_MUTE;
                CLAY_TEXT(cs(g_status, (int)strlen(g_status)),
                          CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 13, .textColor = sc }));
            }
        }
    }

    return Clay_EndLayout(GetFrameTime());
}

/* ---- clay error handler --------------------------------------------------- */
static void clay_err(Clay_ErrorData e) { fprintf(stderr, "clay: %.*s\n", (int)e.errorText.length, e.errorText.chars); }

int main(void) {
    /* Screenshot / smoke: OPENCHIME_GUI_SHOT=<png> + OPENCHIME_GUI_FRAMES=<n>
     * renders n frames, saves a screenshot, and exits — lets the UI be verified
     * headlessly. Unset = run interactively until the window is closed. */
    const char *shot = getenv("OPENCHIME_GUI_SHOT");
    const char *fe   = getenv("OPENCHIME_GUI_FRAMES");
    int max_frames = fe ? atoi(fe) : 0;

    Clay_Raylib_Initialize(920, 640, "OpenChime",
                           FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    SetTargetFPS(60);

    g_fonts[FONT_BODY] = LoadFontEx("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", 48, 0, 400);
    g_fonts[FONT_BOLD] = LoadFontEx("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf", 48, 0, 400);
    SetTextureFilter(g_fonts[FONT_BODY].texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(g_fonts[FONT_BOLD].texture, TEXTURE_FILTER_BILINEAR);

    uint32_t mem = Clay_MinMemorySize();
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(mem, malloc(mem));
    Clay_Initialize(arena, (Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() },
                    (Clay_ErrorHandler){ clay_err, 0 });
    Clay_SetMeasureTextFunction(Raylib_MeasureText, g_fonts);

    /* Dev/verify hook: OPENCHIME_GUI_LOGIN="user:pass" pre-fills and submits so
     * the connect→auth path can be screenshotted headlessly. */
    const char *autologin = getenv("OPENCHIME_GUI_LOGIN");

    int frame = 0;
    while (!WindowShouldClose()) {
        if (autologin && frame == 3) {
            char tmp[256]; snprintf(tmp, sizeof tmp, "%s", autologin);
            char *colon = strchr(tmp, ':');
            if (colon) { *colon = '\0'; snprintf(f_user, sizeof f_user, "%s", tmp); snprintf(f_pass, sizeof f_pass, "%s", colon + 1); }
            do_submit();
        }
        g_caret_t += GetFrameTime();
        if (g_caret_t > 0.53f) { g_caret_t = 0; g_caret_on = !g_caret_on; }

        Clay_SetLayoutDimensions((Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() });
        Vector2 mp = GetMousePosition();
        Clay_SetPointerState((Clay_Vector2){ mp.x, mp.y }, IsMouseButtonDown(MOUSE_BUTTON_LEFT));
        Clay_UpdateScrollContainers(true, (Clay_Vector2){ 0, GetMouseWheelMove() * 4 }, GetFrameTime());
        SetMouseCursor(Clay_PointerOver(CLAY_ID("connect_btn")) ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_DEFAULT);

        process_input();
        Clay_RenderCommandArray cmds = build_layout();

        BeginDrawing();
        ClearBackground((Color){ BG.r, BG.g, BG.b, 255 });
        Clay_Raylib_Render(cmds, g_fonts);
        EndDrawing();

        if (max_frames && ++frame >= max_frames) {
            if (shot) TakeScreenshot(shot);
            break;
        }
    }
    if (g_cl) oc_client_stop(g_cl);
    Clay_Raylib_Close();
    return 0;
}
