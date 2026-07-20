/*
 * OpenChime GUI (ARCH-80, take 2) — a self-rendered, cross-platform client over
 * the shared app-core (client/core), drawn with Clay (layout) on raylib (window
 * + GPU + text). One GUI codebase for Linux/Windows/macOS, replacing the native
 * per-platform widgets that couldn't reach a modern look. Built on Linux first.
 *
 * Two surfaces in one window: a centered login card, then — after AUTH_OK — a
 * paneled chat shell (channel sidebar · message list · members · composer). All
 * logic lives in client/core; this file is pure view + input.
 */

#define CLAY_IMPLEMENTATION
#include "clay.h"
#include "clay_renderer_raylib.c"   /* Raylib_MeasureText, Clay_Raylib_Initialize/Render/Close; pulls raylib.h */

#include "client.h"
#include "model.h"
#include "resolve.h"
#include "protocol.h"     /* OC_CHANNEL_KIND_DM, OC_PRESENCE_*, OC_LOGOUT_* */
#include "oc_port.h"      /* oc_localtime_r */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
/* chat */
static const Clay_Color SIDEBAR   = { 16, 14, 23, 255 };
static const Clay_Color MAIN_BG   = { 26, 23, 36, 255 };
static const Clay_Color ROW_HOVER = { 39, 35, 53, 255 };
static const Clay_Color ROW_SEL   = { 67, 56, 112, 255 };
static const Clay_Color CHIP_BG   = { 44, 40, 60, 255 };
static const Clay_Color CHIP_MINE = { 62, 58, 120, 255 };
static const Clay_Color HAIR      = { 40, 37, 54, 255 };
static const Clay_Color TRANSPARENT = { 0, 0, 0, 0 };
static const Clay_Color BADGE     = { 232, 74, 95, 255 };

/* ---- state ---------------------------------------------------------------- */
typedef enum { PH_FORM, PH_CONNECTING, PH_CHAT, PH_ERROR } Phase;

static char  f_ws[256]   = "127.0.0.1:8443";
static char  f_user[128] = "";
static char  f_pass[128] = "";
static int   g_focus     = 1;      /* login field: 0 ws, 1 user, 2 pass */
static Phase g_phase     = PH_FORM;
static char  g_status[192] = "";
static oc_client *g_cl   = NULL;
static float g_caret_t   = 0;
static int   g_caret_on  = 1;

static uint64_t g_focus_cid = 0;   /* open channel */
static char  g_compose[2048] = "";
static int   g_show_members  = 1;
static size_t g_last_total   = 0;  /* total messages last frame (auto-scroll trigger) */
static int   g_pin_bottom    = 1;

static Clay_String cs(const char *b, int n) {
    return (Clay_String){ .isStaticallyAllocated = false, .length = n, .chars = b };
}
/* csz: for strings that live until render (model-owned or static/global buffers). */
static Clay_String csz(const char *b) { return cs(b, (int)strlen(b)); }

/* Per-frame scratch arena: Clay_TEXT does not copy its string, and rendering
 * happens after the layout function returns, so any locally-built (stack) string
 * must be copied somewhere that survives until Clay_Raylib_Render. ct() copies
 * into this arena; it is reset at the top of each frame's layout build. */
static char   g_scratch[512 * 1024];
static size_t g_scratch_off;
static void   scratch_reset(void) { g_scratch_off = 0; }
static Clay_String ct(const char *s) {
    size_t len = strlen(s);
    if (g_scratch_off + len + 1 > sizeof g_scratch) g_scratch_off = 0;   /* best-effort wrap */
    char *d = g_scratch + g_scratch_off;
    memcpy(d, s, len + 1);
    g_scratch_off += len + 1;
    return cs(d, (int)len);
}

/* ---- text buffers --------------------------------------------------------- */
static void buf_append_cp(char *buf, size_t cap, int cp) {
    size_t n = strlen(buf);
    char tmp[4]; int len;
    if (cp < 0x80) { tmp[0] = (char)cp; len = 1; }
    else if (cp < 0x800) { tmp[0] = (char)(0xC0 | (cp >> 6)); tmp[1] = (char)(0x80 | (cp & 0x3F)); len = 2; }
    else if (cp < 0x10000) { tmp[0] = (char)(0xE0 | (cp >> 12)); tmp[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[2] = (char)(0x80 | (cp & 0x3F)); len = 3; }
    else { tmp[0] = (char)(0xF0 | (cp >> 18)); tmp[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); tmp[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); tmp[3] = (char)(0x80 | (cp & 0x3F)); len = 4; }
    if (n + (size_t)len < cap) { memcpy(buf + n, tmp, len); buf[n + len] = '\0'; }
}
static void buf_backspace(char *buf) {
    size_t n = strlen(buf);
    if (!n) return;
    do { n--; } while (n > 0 && (buf[n] & 0xC0) == 0x80);
    buf[n] = '\0';
}
static char *login_focused_buf(size_t *cap) {
    if (g_focus == 0) { *cap = sizeof f_ws;   return f_ws; }
    if (g_focus == 1) { *cap = sizeof f_user; return f_user; }
    *cap = sizeof f_pass; return f_pass;
}

/* ---- model helpers -------------------------------------------------------- */
static const oc_channel *find_chan(const oc_model *m, uint64_t cid) {
    for (size_t i = 0; i < m->n_channels; i++)
        if (m->channels[i].channel_id == cid) return &m->channels[i];
    return NULL;
}
static void hhmm(char *out, size_t cap, uint64_t server_ms) {
    time_t t = (time_t)(server_ms / 1000); struct tm tmv;
    if (oc_localtime_r(&t, &tmv)) strftime(out, cap, "%H:%M", &tmv);
    else snprintf(out, cap, "--:--");
}
static Clay_Color avatar_color(uint64_t id) {
    static const Clay_Color pal[8] = {
        { 239, 68, 68, 255 }, { 249, 115, 22, 255 }, { 234, 179, 8, 255 }, { 34, 197, 94, 255 },
        { 20, 184, 166, 255 }, { 59, 130, 246, 255 }, { 139, 92, 246, 255 }, { 236, 72, 153, 255 } };
    return pal[id % 8];
}
/* First (up to 2) initials of a display name into `out`. */
static void initials(const char *name, char *out, size_t cap) {
    size_t o = 0;
    int want = 1;
    if (name && name[0]) { out[o++] = (char)toupper((unsigned char)name[0]); }
    for (const char *p = name; *p && o + 1 < cap && want; p++)
        if (*p == ' ' && p[1]) { out[o++] = (char)toupper((unsigned char)p[1]); want = 0; }
    out[o] = '\0';
    if (!o) { snprintf(out, cap, "?"); }
}
static const char *presence_dot_color(uint8_t p, Clay_Color *c) {
    if (p == OC_PRESENCE_ONLINE) { *c = (Clay_Color){ 74, 222, 128, 255 }; return "on"; }
    if (p == OC_PRESENCE_AWAY)   { *c = (Clay_Color){ 250, 204, 21, 255 };  return "away"; }
    *c = (Clay_Color){ 110, 108, 128, 255 }; return "off";
}

/* ---- connect + chat entry ------------------------------------------------- */
static void enter_chat(void) {
    g_phase = PH_CHAT;
    SetWindowSize(1120, 720);
    oc_client_list_users(g_cl);
    oc_client_list_channels(g_cl);
    oc_client_set_presence(g_cl, OC_PRESENCE_ONLINE);
    const oc_model *m = oc_client_model(g_cl);
    for (size_t i = 0; i < m->n_channels; i++)
        if (m->channels[i].joined) { g_focus_cid = m->channels[i].channel_id; break; }
    if (!g_focus_cid && m->n_channels) g_focus_cid = m->channels[0].channel_id;
    if (g_focus_cid) { oc_client_backfill(g_cl, g_focus_cid); oc_client_mark_read(g_cl, g_focus_cid); }
}

static void do_submit(void) {
    if (g_phase == PH_CONNECTING) return;
    if (!f_ws[0])   { snprintf(g_status, sizeof g_status, "Enter a workspace (e.g. 127.0.0.1:8443)"); g_focus = 0; return; }
    if (!f_user[0]) { snprintf(g_status, sizeof g_status, "Enter a username"); g_focus = 1; return; }
    oc_endpoint ep;
    if (oc_resolve(f_ws, getenv("OPENCHIME_SUFFIX"), &ep) != OC_RESOLVE_OK) {
        snprintf(g_status, sizeof g_status, "Could not resolve '%s'", f_ws); g_focus = 0; g_phase = PH_ERROR; return;
    }
    char cred[264]; snprintf(cred, sizeof cred, "%s:%s", f_user, f_pass);
    g_cl = oc_client_start_secure(ep.host, ep.port, cred, NULL, NULL);
    if (!g_cl) { snprintf(g_status, sizeof g_status, "Failed to start client"); g_phase = PH_ERROR; return; }
    g_phase = PH_CONNECTING; snprintf(g_status, sizeof g_status, "Connecting...");
}

static void tick_connecting(void) {
    if (!g_cl) return;
    oc_client_tick(g_cl);
    const oc_model *m = oc_client_model(g_cl);
    if (m->authed) { enter_chat(); return; }
    if (m->last_error[0] && !m->connected) {
        snprintf(g_status, sizeof g_status, "%s", m->last_error);
        oc_client_stop(g_cl); g_cl = NULL; g_phase = PH_ERROR; return;
    }
    snprintf(g_status, sizeof g_status, "%s", m->connected ? "Authenticating..." : "Connecting...");
}

static void open_channel(uint64_t cid) {
    const oc_model *m = oc_client_model(g_cl);
    const oc_channel *c = find_chan(m, cid);
    if (!c) return;
    g_focus_cid = cid;
    if (!c->history_requested) oc_client_backfill(g_cl, cid);
    oc_client_mark_read(g_cl, cid);
    g_pin_bottom = 1;
}

/* Compact command dispatch — the visible-effect subset for this pass; the
 * remaining overlay-driven commands (/search /thread /prefs …) arrive with the
 * overlay render pass. Returns 1 if handled as a command. */
static int chat_command(const char *line) {
    const oc_model *m = oc_client_model(g_cl);
    if (line[0] != '/') return 0;
    if (strcmp(line, "/who") == 0)  { g_show_members = !g_show_members; return 1; }
    if (strcmp(line, "/list") == 0) { oc_client_list_channels(g_cl); return 1; }
    if (strcmp(line, "/away") == 0)   { oc_client_set_presence(g_cl, OC_PRESENCE_AWAY);   return 1; }
    if (strcmp(line, "/online") == 0) { oc_client_set_presence(g_cl, OC_PRESENCE_ONLINE); return 1; }
    if (strcmp(line, "/leave") == 0)  { oc_client_leave_channel(g_cl, g_focus_cid); return 1; }
    if (strcmp(line, "/logout") == 0) { oc_client_logout(g_cl, OC_LOGOUT_THIS); return 1; }
    if (strncmp(line, "/create ", 8) == 0) { const char *n = line + 8; while (*n == ' ') n++; if (*n) oc_client_create_channel(g_cl, n); return 1; }
    if (strncmp(line, "/join ", 6) == 0) {
        const char *n = line + 6; while (*n == ' ') n++;
        for (size_t i = 0; i < m->n_channels; i++)
            if (m->channels[i].name && strcmp(m->channels[i].name, n) == 0) { oc_client_join_channel(g_cl, m->channels[i].channel_id); open_channel(m->channels[i].channel_id); break; }
        return 1;
    }
    if (strncmp(line, "/dm ", 4) == 0) { const char *n = line + 4; while (*n == ' ') n++; uint64_t u = oc_model_user_id(m, n); if (u) oc_client_open_dm(g_cl, u); return 1; }
    if (strncmp(line, "/nick ", 6) == 0) { const char *n = line + 6; while (*n == ' ') n++; if (*n) oc_client_set_display_name(g_cl, n); return 1; }
    if (strncmp(line, "/upload ", 8) == 0) { const char *p = line + 8; while (*p == ' ') p++; if (*p) oc_client_upload(g_cl, g_focus_cid, p); return 1; }
    return 1;   /* swallow unknown slash commands rather than posting them */
}

static void compose_submit(void) {
    size_t n = strlen(g_compose);
    while (n && (g_compose[n-1] == ' ')) g_compose[--n] = '\0';
    if (!n) return;
    if (g_compose[0] == '/') chat_command(g_compose);
    else if (g_focus_cid) oc_client_send(g_cl, g_focus_cid, g_compose);
    g_compose[0] = '\0';
    g_pin_bottom = 1;
}

/* ---- login input ---------------------------------------------------------- */
static void login_input(void) {
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        if (Clay_PointerOver(CLAY_ID("field_ws")))    g_focus = 0;
        if (Clay_PointerOver(CLAY_ID("field_user")))  g_focus = 1;
        if (Clay_PointerOver(CLAY_ID("field_pass")))  g_focus = 2;
        if (Clay_PointerOver(CLAY_ID("connect_btn"))) do_submit();
    }
    if (IsKeyPressed(KEY_TAB)) {
        int dir = (IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT)) ? 2 : 1;
        g_focus = (g_focus + dir) % 3;
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) { do_submit(); return; }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) {
        size_t cap; buf_backspace(login_focused_buf(&cap)); if (g_phase == PH_ERROR) g_phase = PH_FORM;
    }
    int cp;
    while ((cp = GetCharPressed())) {
        if (cp < 0x20) continue;
        size_t cap; buf_append_cp(login_focused_buf(&cap), cap, cp); if (g_phase == PH_ERROR) g_phase = PH_FORM;
    }
}

/* ---- chat input ----------------------------------------------------------- */
static void chat_input(void) {
    oc_client_tick(g_cl);
    const oc_model *m = oc_client_model(g_cl);

    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) {
        for (size_t i = 0; i < m->n_channels; i++)
            if (Clay_PointerOver(CLAY_IDI("chan", (int)i))) { open_channel(m->channels[i].channel_id); }
        for (size_t i = 0; i < m->n_users; i++)
            if (Clay_PointerOver(CLAY_IDI("mem", (int)i))) { oc_client_open_dm(g_cl, m->users[i].user_id); }
    }
    if (IsKeyPressed(KEY_ENTER) || IsKeyPressed(KEY_KP_ENTER)) { compose_submit(); return; }
    if (IsKeyPressed(KEY_BACKSPACE) || IsKeyPressedRepeat(KEY_BACKSPACE)) buf_backspace(g_compose);
    int cp;
    while ((cp = GetCharPressed())) { if (cp >= 0x20) buf_append_cp(g_compose, sizeof g_compose, cp); }
    if (GetMouseWheelMove() != 0) g_pin_bottom = 0;   /* user took over scrolling */
}

/* ---- chat layout ---------------------------------------------------------- */
static void ui_avatar(uint64_t uid, const char *name, int size) {
    char ini[8]; initials(name, ini, sizeof ini);
    CLAY_AUTO_ID({ .backgroundColor = avatar_color(uid), .cornerRadius = CLAY_CORNER_RADIUS(size / 2.0f),
                   .layout = { .sizing = { CLAY_SIZING_FIXED((float)size), CLAY_SIZING_FIXED((float)size) },
                               .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } } }) {
        CLAY_TEXT(ct(ini), CLAY_TEXT_CONFIG({ .fontId = FONT_BOLD, .fontSize = (uint16_t)(size / 2 + 2), .textColor = TXT }));
    }
}

static void ui_message(const oc_model *m, const oc_msg *msg, int grouped) {
    const char *who = msg->author_name[0] ? msg->author_name
                    : (oc_model_user_name(m, msg->author_id)[0] ? oc_model_user_name(m, msg->author_id) : "?");
    char ts[16]; hhmm(ts, sizeof ts, msg->server_time);

    CLAY_AUTO_ID({ .layout = { .layoutDirection = CLAY_LEFT_TO_RIGHT, .sizing = { .width = CLAY_SIZING_GROW(0) },
                               .padding = { 0, 0, grouped ? 1 : 8, 0 }, .childGap = 12 } }) {
        /* gutter: avatar (group head) or spacer (grouped) */
        if (!grouped) ui_avatar(msg->author_id, who, 38);
        else CLAY_AUTO_ID({ .layout = { .sizing = { CLAY_SIZING_FIXED(38) } } }) {}

        CLAY_AUTO_ID({ .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = 3 } }) {
            if (!grouped)
                CLAY_AUTO_ID({ .layout = { .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 8, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                    CLAY_TEXT(csz(who), CLAY_TEXT_CONFIG({ .fontId = FONT_BOLD, .fontSize = 15, .textColor = TXT }));
                    CLAY_TEXT(ct(ts),  CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 12, .textColor = TXT_MUTE }));
                }
            if (msg->deleted)
                CLAY_TEXT(csz("(message deleted)"), CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 15, .textColor = TXT_MUTE }));
            else
                CLAY_TEXT(csz(msg->body ? msg->body : ""),
                          CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 15, .textColor = TXT, .lineHeight = 21 }));
            if (msg->edited && !msg->deleted)
                CLAY_TEXT(csz("edited"), CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 11, .textColor = TXT_MUTE }));

            for (uint8_t k = 0; k < msg->n_attach; k++) {
                char al[200];
                const oc_attachment *a = &msg->attach[k];
                snprintf(al, sizeof al, a->reclaimed ? "[file] %s  (no longer available)" : "[file] %s  (%llu bytes)  /download %llu",
                         a->filename, (unsigned long long)a->size, (unsigned long long)a->id);
                CLAY_AUTO_ID({ .backgroundColor = CHIP_BG, .cornerRadius = CLAY_CORNER_RADIUS(6),
                               .layout = { .padding = { 10, 10, 6, 6 } } }) {
                    CLAY_TEXT(ct(al), CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 13, .textColor = TXT_MUTE }));
                }
            }
            if (msg->n_reactions)
                CLAY_AUTO_ID({ .layout = { .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 6, .padding = { 0, 0, 2, 0 } } }) {
                    for (uint8_t k = 0; k < msg->n_reactions; k++) {
                        char rl[64]; snprintf(rl, sizeof rl, "%s %u", msg->reactions[k].emoji, msg->reactions[k].count);
                        CLAY_AUTO_ID({ .backgroundColor = msg->reactions[k].mine ? CHIP_MINE : CHIP_BG, .cornerRadius = CLAY_CORNER_RADIUS(10),
                                       .layout = { .padding = { 9, 9, 3, 3 } } }) {
                            CLAY_TEXT(ct(rl), CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 12, .textColor = TXT }));
                        }
                    }
                }
        }
    }
}

static Clay_RenderCommandArray build_chat_layout(void) {
    const oc_model *m = oc_client_model(g_cl);
    const oc_channel *fc = find_chan(m, g_focus_cid);
    const char *me = m->user_id ? oc_model_user_name(m, m->user_id) : "";

    scratch_reset();
    Clay_BeginLayout();

    CLAY(CLAY_ID("root"), { .backgroundColor = MAIN_BG,
        .layout = { .layoutDirection = CLAY_LEFT_TO_RIGHT, .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {

        /* ---- sidebar ---- */
        CLAY(CLAY_ID("sidebar"), { .backgroundColor = SIDEBAR,
            .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = { CLAY_SIZING_FIXED(232), CLAY_SIZING_GROW(0) },
                        .padding = { 12, 12, 14, 12 }, .childGap = 3 } }) {
            CLAY_AUTO_ID({ .layout = { .padding = { 6, 6, 2, 12 } } }) {
                CLAY_TEXT(csz("OpenChime"), CLAY_TEXT_CONFIG({ .fontId = FONT_BOLD, .fontSize = 19, .textColor = TXT }));
            }
            CLAY_AUTO_ID({ .layout = { .padding = { 6, 6, 2, 6 } } }) {
                CLAY_TEXT(csz("CHANNELS"), CLAY_TEXT_CONFIG({ .fontId = FONT_BOLD, .fontSize = 11, .textColor = TXT_MUTE, .letterSpacing = 1 }));
            }
            for (size_t i = 0; i < m->n_channels; i++) {
                const oc_channel *c = &m->channels[i];
                if (c->kind == OC_CHANNEL_KIND_DM) continue;
                int sel = (c->channel_id == g_focus_cid);
                int hov = Clay_PointerOver(CLAY_IDI("chan", (int)i));
                char label[128]; snprintf(label, sizeof label, "# %s", c->name ? c->name : "...");
                CLAY(CLAY_IDI("chan", (int)i), { .backgroundColor = sel ? ROW_SEL : (hov ? ROW_HOVER : TRANSPARENT),
                    .cornerRadius = CLAY_CORNER_RADIUS(6),
                    .layout = { .layoutDirection = CLAY_LEFT_TO_RIGHT, .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(30) },
                                .padding = { 10, 8, 0, 0 }, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 6 } }) {
                    CLAY_TEXT(ct(label), CLAY_TEXT_CONFIG({ .fontId = c->unread ? FONT_BOLD : FONT_BODY, .fontSize = 14,
                                                            .textColor = sel ? TXT : (c->unread ? TXT : TXT_MUTE) }));
                    if (c->unread > 0) {
                        CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) } } }) {}
                        char b[16]; snprintf(b, sizeof b, "%d", c->unread);
                        CLAY_AUTO_ID({ .backgroundColor = BADGE, .cornerRadius = CLAY_CORNER_RADIUS(9),
                                       .layout = { .padding = { 7, 7, 1, 1 }, .childAlignment = { CLAY_ALIGN_X_CENTER } } }) {
                            CLAY_TEXT(ct(b), CLAY_TEXT_CONFIG({ .fontId = FONT_BOLD, .fontSize = 11, .textColor = TXT }));
                        }
                    }
                }
            }
            /* DMs */
            CLAY_AUTO_ID({ .layout = { .padding = { 6, 6, 14, 6 } } }) {
                CLAY_TEXT(csz("DIRECT MESSAGES"), CLAY_TEXT_CONFIG({ .fontId = FONT_BOLD, .fontSize = 11, .textColor = TXT_MUTE, .letterSpacing = 1 }));
            }
            for (size_t i = 0; i < m->n_channels; i++) {
                const oc_channel *c = &m->channels[i];
                if (c->kind != OC_CHANNEL_KIND_DM) continue;
                int sel = (c->channel_id == g_focus_cid);
                int hov = Clay_PointerOver(CLAY_IDI("chan", (int)i));
                const char *pn = (c->peer_id == m->user_id) ? "you" : oc_model_user_name(m, c->peer_id);
                char label[128]; snprintf(label, sizeof label, "@ %s", pn[0] ? pn : "dm");
                CLAY(CLAY_IDI("chan", (int)i), { .backgroundColor = sel ? ROW_SEL : (hov ? ROW_HOVER : TRANSPARENT),
                    .cornerRadius = CLAY_CORNER_RADIUS(6),
                    .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(30) },
                                .padding = { 10, 8, 0, 0 }, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                    CLAY_TEXT(ct(label), CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 14, .textColor = sel ? TXT : TXT_MUTE }));
                }
            }
            /* spacer + self footer */
            CLAY_AUTO_ID({ .layout = { .sizing = { .height = CLAY_SIZING_GROW(0) } } }) {}
            CLAY_AUTO_ID({ .layout = { .layoutDirection = CLAY_LEFT_TO_RIGHT, .childGap = 8, .padding = { 6, 6, 8, 2 },
                                       .childAlignment = { .y = CLAY_ALIGN_Y_CENTER } } }) {
                ui_avatar(m->user_id, me[0] ? me : "?", 26);
                CLAY_TEXT(csz(me[0] ? me : "you"), CLAY_TEXT_CONFIG({ .fontId = FONT_BOLD, .fontSize = 13, .textColor = TXT }));
            }
        }

        /* ---- main column ---- */
        CLAY(CLAY_ID("main"), { .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) } } }) {
            /* header */
            char title[128];
            if (!fc) snprintf(title, sizeof title, "no channel");
            else if (fc->kind == OC_CHANNEL_KIND_DM) snprintf(title, sizeof title, "@ %s", oc_model_user_name(m, fc->peer_id));
            else snprintf(title, sizeof title, "# %s", fc->name ? fc->name : "...");
            CLAY(CLAY_ID("header"), { .backgroundColor = MAIN_BG,
                .border = { .color = HAIR, .width = { 0, 0, 0, 1, 0 } },
                .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(54) },
                            .padding = { 20, 16, 0, 0 }, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 10 } }) {
                CLAY_TEXT(ct(title), CLAY_TEXT_CONFIG({ .fontId = FONT_BOLD, .fontSize = 17, .textColor = TXT }));
            }
            /* message list (scrolls) */
            CLAY(CLAY_ID("msglist"), {
                .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
                            .padding = { 20, 20, 16, 12 }, .childGap = 2 },
                .clip = { .vertical = true, .childOffset = Clay_GetScrollOffset() } }) {
                if (fc && fc->n_msgs) {
                    for (size_t i = 0; i < fc->n_msgs; i++) {
                        const oc_msg *msg = &fc->msgs[i];
                        int grouped = i > 0 && fc->msgs[i-1].author_id == msg->author_id
                                    && !fc->msgs[i-1].deleted
                                    && (msg->server_time - fc->msgs[i-1].server_time) < 5*60*1000;
                        ui_message(m, msg, grouped);
                    }
                } else {
                    CLAY_AUTO_ID({ .layout = { .padding = { 0, 0, 8, 0 } } }) {
                        CLAY_TEXT(csz(fc ? "No messages yet - say hello." : "Select a channel."),
                                  CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 14, .textColor = TXT_MUTE }));
                    }
                }
            }
            /* composer */
            CLAY_AUTO_ID({ .layout = { .sizing = { .width = CLAY_SIZING_GROW(0) }, .padding = { 16, 16, 6, 14 } } }) {
                CLAY(CLAY_ID("composer"), { .backgroundColor = CARD, .cornerRadius = CLAY_CORNER_RADIUS(10),
                    .border = { .color = BORDER, .width = CLAY_BORDER_OUTSIDE(1) },
                    .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(48) },
                                .padding = { 16, 16, 0, 0 }, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 2 } }) {
                    if (g_compose[0])
                        CLAY_TEXT(csz(g_compose), CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 15, .textColor = TXT }));
                    else {
                        char ph[160]; snprintf(ph, sizeof ph, "Message %s", title);
                        CLAY_TEXT(ct(ph), CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 15, .textColor = TXT_MUTE }));
                    }
                    if (g_caret_on)
                        CLAY_AUTO_ID({ .backgroundColor = TXT, .layout = { .sizing = { CLAY_SIZING_FIXED(2), CLAY_SIZING_FIXED(20) } } }) {}
                }
            }
        }

        /* ---- members ---- */
        if (g_show_members) {
            CLAY(CLAY_ID("members"), { .backgroundColor = SIDEBAR,
                .border = { .color = HAIR, .width = { 1, 0, 0, 0, 0 } },
                .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = { CLAY_SIZING_FIXED(210), CLAY_SIZING_GROW(0) },
                            .padding = { 14, 12, 16, 12 }, .childGap = 2 } }) {
                CLAY_AUTO_ID({ .layout = { .padding = { 4, 4, 2, 10 } } }) {
                    CLAY_TEXT(csz("MEMBERS"), CLAY_TEXT_CONFIG({ .fontId = FONT_BOLD, .fontSize = 11, .textColor = TXT_MUTE, .letterSpacing = 1 }));
                }
                for (size_t i = 0; i < m->n_users; i++) {
                    const oc_member *u = &m->users[i];
                    int hov = Clay_PointerOver(CLAY_IDI("mem", (int)i));
                    Clay_Color dc; presence_dot_color(oc_model_presence_of(m, u->user_id), &dc);
                    CLAY(CLAY_IDI("mem", (int)i), { .backgroundColor = hov ? ROW_HOVER : TRANSPARENT, .cornerRadius = CLAY_CORNER_RADIUS(6),
                        .layout = { .layoutDirection = CLAY_LEFT_TO_RIGHT, .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(30) },
                                    .padding = { 8, 8, 0, 0 }, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 9 } }) {
                        CLAY_AUTO_ID({ .backgroundColor = dc, .cornerRadius = CLAY_CORNER_RADIUS(5),
                                       .layout = { .sizing = { CLAY_SIZING_FIXED(9), CLAY_SIZING_FIXED(9) } } }) {}
                        CLAY_TEXT(csz(u->name[0] ? u->name : "?"),
                                  CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 14, .textColor = u->disabled ? TXT_MUTE : TXT }));
                    }
                }
            }
        }
    }

    return Clay_EndLayout(GetFrameTime());
}

/* ---- login layout --------------------------------------------------------- */
static void ui_field(Clay_ElementId id, const char *label, const char *value, int focused, int masked, const char *placeholder) {
    CLAY_AUTO_ID({ .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = { .width = CLAY_SIZING_GROW(0) }, .childGap = 7 } }) {
        CLAY_TEXT(csz(label), CLAY_TEXT_CONFIG({ .fontId = FONT_BOLD, .fontSize = 12, .textColor = TXT_MUTE, .letterSpacing = 1 }));
        CLAY(id, { .backgroundColor = focused ? FIELD_FOC : FIELD, .cornerRadius = CLAY_CORNER_RADIUS(8),
                   .border = { .color = focused ? ACCENT : BORDER, .width = CLAY_BORDER_OUTSIDE(focused ? 2 : 1) },
                   .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(46) },
                               .padding = { 14, 14, 0, 0 }, .childAlignment = { .y = CLAY_ALIGN_Y_CENTER }, .childGap = 2 } }) {
            static char maskbuf[130];
            const char *shown = value; int has = value[0] != '\0';
            if (masked && has) { int n = 0; for (const char *p = value; *p; p++) if ((*p & 0xC0) != 0x80 && n < 128) maskbuf[n++] = '*'; maskbuf[n] = '\0'; shown = maskbuf; }
            if (has) CLAY_TEXT(csz(shown), CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 17, .textColor = TXT }));
            else     CLAY_TEXT(csz(placeholder), CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 17, .textColor = TXT_MUTE }));
            if (focused && g_caret_on)
                CLAY_AUTO_ID({ .backgroundColor = TXT, .layout = { .sizing = { CLAY_SIZING_FIXED(2), CLAY_SIZING_FIXED(20) } } }) {}
        }
    }
}
static Clay_RenderCommandArray build_login_layout(void) {
    Clay_BeginLayout();
    CLAY(CLAY_ID("root"), { .backgroundColor = BG,
        .layout = { .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) }, .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } } }) {
        CLAY(CLAY_ID("card"), { .backgroundColor = CARD, .cornerRadius = CLAY_CORNER_RADIUS(16),
            .border = { .color = BORDER, .width = CLAY_BORDER_OUTSIDE(1) },
            .layout = { .layoutDirection = CLAY_TOP_TO_BOTTOM, .sizing = { .width = CLAY_SIZING_FIXED(400) },
                        .padding = { 36, 36, 34, 34 }, .childGap = 18 } }) {
            CLAY_TEXT(csz("OpenChime"), CLAY_TEXT_CONFIG({ .fontId = FONT_BOLD, .fontSize = 30, .textColor = TXT }));
            CLAY_TEXT(csz("Sign in to your workspace"), CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 15, .textColor = TXT_MUTE }));
            CLAY_AUTO_ID({ .layout = { .sizing = { .height = CLAY_SIZING_FIXED(4) } } }) {}
            ui_field(CLAY_ID("field_ws"),   "WORKSPACE", f_ws,   g_focus == 0, 0, "domain or host:port");
            ui_field(CLAY_ID("field_user"), "USERNAME",  f_user, g_focus == 1, 0, "you");
            ui_field(CLAY_ID("field_pass"), "PASSWORD",  f_pass, g_focus == 2, 1, "password");
            int btn_hover = Clay_PointerOver(CLAY_ID("connect_btn"));
            int connecting = (g_phase == PH_CONNECTING);
            CLAY(CLAY_ID("connect_btn"), { .backgroundColor = connecting ? BORDER : (btn_hover ? ACCENT_HI : ACCENT),
                .cornerRadius = CLAY_CORNER_RADIUS(9),
                .layout = { .sizing = { .width = CLAY_SIZING_GROW(0), .height = CLAY_SIZING_FIXED(46) }, .childAlignment = { CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER } } }) {
                const char *bl = connecting ? "Connecting..." : "Connect";
                CLAY_TEXT(csz(bl), CLAY_TEXT_CONFIG({ .fontId = FONT_BOLD, .fontSize = 16, .textColor = TXT }));
            }
            if (g_status[0]) {
                Clay_Color sc = g_phase == PH_ERROR ? ERR_COL : TXT_MUTE;
                CLAY_TEXT(csz(g_status), CLAY_TEXT_CONFIG({ .fontId = FONT_BODY, .fontSize = 13, .textColor = sc }));
            }
        }
    }
    return Clay_EndLayout(GetFrameTime());
}

/* ---- fonts ---------------------------------------------------------------- */
static Font load_font(const char *path) {
    /* A compact codepoint set: ASCII + Latin-1 + common punctuation (– — ' ' " "
     * • …) + arrows. Color emoji are a separate atlas problem, deferred. */
    static int cps[600]; int n = 0;
    for (int c = 32;     c <= 126;    c++) cps[n++] = c;
    for (int c = 160;    c <= 255;    c++) cps[n++] = c;
    for (int c = 0x2010; c <= 0x2027; c++) cps[n++] = c;
    for (int c = 0x2190; c <= 0x2199; c++) cps[n++] = c;
    cps[n++] = 0x2713; cps[n++] = 0x2717; cps[n++] = 0x2022; cps[n++] = 0x2026;
    return LoadFontEx(path, 42, cps, n);
}

/* ---- clay error ----------------------------------------------------------- */
static void clay_err(Clay_ErrorData e) { fprintf(stderr, "clay: %.*s\n", (int)e.errorText.length, e.errorText.chars); }

int main(void) {
    const char *shot = getenv("OPENCHIME_GUI_SHOT");
    const char *fe   = getenv("OPENCHIME_GUI_FRAMES");
    const char *autologin = getenv("OPENCHIME_GUI_LOGIN");
    int max_frames = fe ? atoi(fe) : 0;

    /* Under WSLg, GLFW defaults to the Wayland backend, where character input
     * (GetCharPressed) is not delivered reliably — the mouse works but you can't
     * type. Force the X11 backend (XWayland), which handles the keyboard well.
     * Only under WSL; a native Wayland desktop keeps its native backend. */
    if (getenv("WSL_DISTRO_NAME") || getenv("WSL_INTEROP")) unsetenv("WAYLAND_DISPLAY");

    Clay_Raylib_Initialize(920, 640, "OpenChime", FLAG_WINDOW_RESIZABLE | FLAG_MSAA_4X_HINT | FLAG_VSYNC_HINT);
    SetTargetFPS(60);
    SetWindowFocused();   /* WSLg opens windows unfocused; grab keyboard focus */

    g_fonts[FONT_BODY] = load_font("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    g_fonts[FONT_BOLD] = load_font("/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf");
    SetTextureFilter(g_fonts[FONT_BODY].texture, TEXTURE_FILTER_BILINEAR);
    SetTextureFilter(g_fonts[FONT_BOLD].texture, TEXTURE_FILTER_BILINEAR);

    uint32_t mem = Clay_MinMemorySize();
    Clay_Arena arena = Clay_CreateArenaWithCapacityAndMemory(mem, malloc(mem));
    Clay_Initialize(arena, (Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() }, (Clay_ErrorHandler){ clay_err, 0 });
    Clay_SetMeasureTextFunction(Raylib_MeasureText, g_fonts);

    int frame = 0;
    while (!WindowShouldClose()) {
        g_caret_t += GetFrameTime();
        if (g_caret_t > 0.53f) { g_caret_t = 0; g_caret_on = !g_caret_on; }

        if (autologin && frame == 3 && g_phase == PH_FORM) {
            char tmp[256]; snprintf(tmp, sizeof tmp, "%s", autologin);
            char *colon = strchr(tmp, ':');
            if (colon) { *colon = '\0'; snprintf(f_user, sizeof f_user, "%s", tmp); snprintf(f_pass, sizeof f_pass, "%s", colon + 1); }
            do_submit();
        }

        /* A click must also grab OS keyboard focus — under WSLg/XWayland clicking
         * doesn't always transfer it, which leaves typing dead. */
        if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) && !IsWindowFocused()) SetWindowFocused();

        Clay_SetLayoutDimensions((Clay_Dimensions){ (float)GetScreenWidth(), (float)GetScreenHeight() });
        Vector2 mp = GetMousePosition();
        Clay_SetPointerState((Clay_Vector2){ mp.x, mp.y }, IsMouseButtonDown(MOUSE_BUTTON_LEFT));
        Clay_UpdateScrollContainers(true, (Clay_Vector2){ 0, GetMouseWheelMove() * 6 }, GetFrameTime());

        Clay_RenderCommandArray cmds;
        if (g_phase == PH_CHAT) {
            chat_input();
            SetMouseCursor(MOUSE_CURSOR_DEFAULT);
            cmds = build_chat_layout();
            /* auto-scroll to newest when pinned or on new messages */
            const oc_model *m = oc_client_model(g_cl);
            const oc_channel *fc = find_chan(m, g_focus_cid);
            size_t total = fc ? fc->n_msgs : 0;
            if (total != g_last_total) { g_pin_bottom = 1; g_last_total = total; }
            if (g_pin_bottom) {
                Clay_ScrollContainerData sd = Clay_GetScrollContainerData(CLAY_ID("msglist"));
                if (sd.found && sd.scrollPosition) {
                    float over = sd.contentDimensions.height - sd.scrollContainerDimensions.height;
                    sd.scrollPosition->y = over > 0 ? -over : 0;
                }
            }
        } else {
            if (g_phase == PH_CONNECTING) tick_connecting(); else login_input();
            SetMouseCursor(Clay_PointerOver(CLAY_ID("connect_btn")) ? MOUSE_CURSOR_POINTING_HAND : MOUSE_CURSOR_DEFAULT);
            cmds = build_login_layout();
        }

        BeginDrawing();
        ClearBackground((Color){ BG.r, BG.g, BG.b, 255 });
        Clay_Raylib_Render(cmds, g_fonts);
        EndDrawing();

        frame++;
        if (max_frames && frame >= max_frames) { if (shot) TakeScreenshot(shot); break; }
    }
    if (g_cl) oc_client_stop(g_cl);
    Clay_Raylib_Close();
    return 0;
}
