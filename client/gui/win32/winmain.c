/*
 * OpenChime Win32 GUI client (ARCH-80/82) — pure C, over the shared app-core
 * (client/core, ARCH-74). A native window drawn with Direct2D + DirectWrite:
 * a slim left rail, a live channel sidebar, a scrollable message transcript,
 * and a working composer. Deep-blue accent on a dark neutral shell.
 *
 *   openchime.exe [<workspace> <user:pass>]   (defaults: 127.0.0.1:8443 alice:pw)
 *
 * The core owns the network thread + the view-model; this frontend is pure view
 * + input, ticked on a WM_TIMER (~30 ms, the GUI analogue of the TUI poll loop):
 * tick -> read oc_client_model() -> D2D-draw -> dispatch intents, all on the UI
 * thread. No product logic lives here (the core owns it). Native controls (a
 * RichEdit composer, menus, dialogs) and the members pane land in later phases.
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS            /* C-style COM: Interface_Method(obj, ...) */
#include <windows.h>
#include <windowsx.h>         /* GET_X_LPARAM / GET_Y_LPARAM */
#include <shellapi.h>         /* CommandLineToArgvW */
#include <d2d1.h>
#include <dwrite.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "client.h"
#include "model.h"
#include "resolve.h"
#include "oc_port.h"          /* oc_mkdir, oc_localtime_r */
#include "protocol.h"         /* OC_CHANNEL_KIND_DM, OC_PRESENCE_* */
#include "theme.h"

/* mingw ships IID_ID2D1Factory in libuuid but not IID_IDWriteFactory; define it
 * locally so we don't depend on the toolchain's GUID table for DWrite. */
static const GUID OC_IID_IDWriteFactory =
    { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };

#define TIMER_TICK 1

/* Layout metrics (device pixels; per-monitor DPI is a later phase). */
#define RAIL_W      64.0f
#define SIDEBAR_W   250.0f
#define HEADER_H    56.0f
#define COMPOSER_H  72.0f
#define ROW_H       32.0f     /* a sidebar channel row */
#define AVA         36.0f     /* transcript avatar diameter */
#define LINE_H      19.0f     /* an extra (reaction/attach/thread) line */

/* ---- app state ----------------------------------------------------------- */

static oc_client *g_client;
static char       g_cred[264];
static char       g_host[256];
static int        g_port;

static ID2D1Factory          *g_factory;
static IDWriteFactory        *g_dwrite;
static ID2D1HwndRenderTarget *g_rt;
static ID2D1SolidColorBrush  *g_brush;      /* one reusable brush; recolored per draw */

static IDWriteTextFormat *g_hdr;    /* channel title */
static IDWriteTextFormat *g_name;   /* message author (semibold) */
static IDWriteTextFormat *g_time;   /* timestamp (trailing-aligned) */
static IDWriteTextFormat *g_body;   /* message body (wrapping) */
static IDWriteTextFormat *g_ui;     /* sidebar rows / composer */
static IDWriteTextFormat *g_ui_b;   /* unread sidebar rows (semibold) */
static IDWriteTextFormat *g_small;  /* subtitles / meta lines */
static IDWriteTextFormat *g_ava;    /* avatar initial (centered) */

static uint64_t g_sel;              /* selected channel id (0 = none) */
static float    g_scroll;           /* px scrolled up from the bottom of the transcript */
static float    g_scroll_max;       /* recomputed each paint, for input clamping */
static uint8_t  g_post_auth;        /* one-shot post-auth roster/channel refresh */

static uint64_t g_backfilled[512];  /* channels we've already asked history for */
static int      g_n_backfilled;

/* Composer input (UTF-8) + a pending UTF-16 high surrogate from WM_CHAR. */
static char     g_input[4000];
static size_t   g_inlen;
static WCHAR    g_hi_surrogate;
static DWORD    g_last_typing;

/* Sidebar row hit-boxes, captured during paint for WM_LBUTTONDOWN. */
static struct { float top, bot; uint64_t cid; } g_rows[512];
static int g_n_rows;

/* ---- small helpers ------------------------------------------------------- */

static D2D1_COLOR_F col(uint32_t rgb) {
    D2D1_COLOR_F c;
    c.r = ((rgb >> 16) & 0xff) / 255.0f;
    c.g = ((rgb >> 8) & 0xff) / 255.0f;
    c.b = (rgb & 0xff) / 255.0f;
    c.a = 1.0f;
    return c;
}

static ID2D1Brush *paint_with(uint32_t rgb) {
    D2D1_COLOR_F c = col(rgb);
    ID2D1SolidColorBrush_SetColor(g_brush, &c);
    return (ID2D1Brush *)g_brush;
}

static D2D1_RECT_F rf(float l, float t, float r, float b) {
    D2D1_RECT_F x = { l, t, r, b }; return x;
}

static void fill(ID2D1RenderTarget *rt, D2D1_RECT_F r, uint32_t rgb) {
    ID2D1RenderTarget_FillRectangle(rt, &r, paint_with(rgb));
}

static void fill_round(ID2D1RenderTarget *rt, D2D1_RECT_F r, float rad, uint32_t rgb) {
    D2D1_ROUNDED_RECT rr = { r, rad, rad };
    ID2D1RenderTarget_FillRoundedRectangle(rt, &rr, paint_with(rgb));
}

/* UTF-8 -> UTF-16 into caller buffer; returns character count (no NUL). */
static int to_w(const char *s, WCHAR *out, int cap) {
    if (!s) { out[0] = 0; return 0; }
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
    return n > 0 ? n - 1 : 0;
}

static void draw_text(ID2D1RenderTarget *rt, const char *s, IDWriteTextFormat *fmt,
                      D2D1_RECT_F r, uint32_t rgb) {
    WCHAR w[1024];
    int n = to_w(s, w, 1024);
    if (n <= 0) return;
    ID2D1RenderTarget_DrawText(rt, w, (UINT32)n, fmt, &r, paint_with(rgb),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

/* ---- Direct2D / DirectWrite setup ---------------------------------------- */

static IDWriteTextFormat *mk_fmt(const WCHAR *family, float size, DWRITE_FONT_WEIGHT wt,
                                 DWRITE_TEXT_ALIGNMENT ta, DWRITE_PARAGRAPH_ALIGNMENT pa,
                                 int wrap) {
    IDWriteTextFormat *f = NULL;
    IDWriteFactory_CreateTextFormat(g_dwrite, family, NULL, wt, DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
    if (f) {
        IDWriteTextFormat_SetTextAlignment(f, ta);
        IDWriteTextFormat_SetParagraphAlignment(f, pa);
        IDWriteTextFormat_SetWordWrapping(f, wrap ? DWRITE_WORD_WRAPPING_WRAP
                                                  : DWRITE_WORD_WRAPPING_NO_WRAP);
    }
    return f;
}

static void d2d_init(void) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory, NULL,
                      (void **)&g_factory);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &OC_IID_IDWriteFactory,
                        (IUnknown **)&g_dwrite);
    if (!g_dwrite) return;
    const WCHAR *UI = L"Segoe UI";
    g_hdr   = mk_fmt(UI, 17.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0);
    g_name  = mk_fmt(UI, 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0);
    g_time  = mk_fmt(UI, 12.0f, DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0);
    g_body  = mk_fmt(UI, 15.0f, DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_PARAGRAPH_ALIGNMENT_NEAR,   1);
    g_ui    = mk_fmt(UI, 14.5f, DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0);
    g_ui_b  = mk_fmt(UI, 14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0);
    g_small = mk_fmt(UI, 12.5f, DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0);
    g_ava   = mk_fmt(UI, 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER,   DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0);
}

static void d2d_ensure_rt(HWND hwnd) {
    if (g_rt || !g_factory) return;
    RECT rc; GetClientRect(hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES rtp;
    ZeroMemory(&rtp, sizeof rtp);
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp;
    hp.hwnd = hwnd;
    hp.pixelSize.width  = (UINT32)(rc.right - rc.left);
    hp.pixelSize.height = (UINT32)(rc.bottom - rc.top);
    hp.presentOptions = D2D1_PRESENT_OPTIONS_NONE;
    if (SUCCEEDED(ID2D1Factory_CreateHwndRenderTarget(g_factory, &rtp, &hp, &g_rt))) {
        D2D1_COLOR_F white = col(0xFFFFFF);
        ID2D1RenderTarget_CreateSolidColorBrush((ID2D1RenderTarget *)g_rt, &white, NULL, &g_brush);
    }
}

static void d2d_resize(HWND hwnd) {
    if (!g_rt) return;
    RECT rc; GetClientRect(hwnd, &rc);
    D2D1_SIZE_U s = { (UINT32)(rc.right - rc.left), (UINT32)(rc.bottom - rc.top) };
    ID2D1HwndRenderTarget_Resize(g_rt, &s);
}

/* ---- model access + intents ---------------------------------------------- */

static const oc_model *model(void) { return g_client ? oc_client_model(g_client) : NULL; }

static int already_backfilled(uint64_t cid) {
    for (int i = 0; i < g_n_backfilled; i++) if (g_backfilled[i] == cid) return 1;
    return 0;
}

static void select_channel(uint64_t cid) {
    if (!g_client || !cid) return;
    g_sel = cid;
    g_scroll = 0;
    if (!already_backfilled(cid)) {
        oc_client_backfill(g_client, cid);
        if (g_n_backfilled < (int)(sizeof g_backfilled / sizeof g_backfilled[0]))
            g_backfilled[g_n_backfilled++] = cid;
    }
    oc_client_mark_read(g_client, cid);
}

/* A channel's display name into `out` ("# general" / "@ bob"). */
static void channel_label(const oc_model *m, const oc_channel *c, char *out, size_t cap) {
    if (c->kind == OC_CHANNEL_KIND_DM) {
        const char *pn = (c->peer_id == m->user_id) ? "you" : oc_model_user_name(m, c->peer_id);
        snprintf(out, cap, "@ %s", (pn && pn[0]) ? pn : "dm");
    } else {
        snprintf(out, cap, "%s %s", c->is_public ? "#" : "\xF0\x9F\x94\x92", /* # or lock */
                 c->name ? c->name : "channel");
    }
}

/* Pick a sensible default channel once the list arrives. */
static void ensure_selection(const oc_model *m) {
    if (g_sel) {
        if (oc_model_channel((oc_model *)m, g_sel)) return;   /* still valid */
        g_sel = 0;
    }
    for (size_t i = 0; i < m->n_channels; i++)
        if (m->channels[i].name && m->channels[i].name[0]) {
            select_channel(m->channels[i].channel_id);
            return;
        }
}

/* ---- rail + sidebar ------------------------------------------------------ */

static void draw_rail(ID2D1RenderTarget *rt, float h) {
    fill(rt, rf(0, 0, RAIL_W, h), OC_COL_RAIL);
    /* Workspace avatar: an accent rounded square with the host's initial. */
    D2D1_RECT_F av = rf(14, 16, RAIL_W - 14, 16 + (RAIL_W - 28));
    fill_round(rt, av, 12.0f, OC_COL_ACCENT);
    char init[2] = { (char)(g_host[0] ? (g_host[0] >= 'a' && g_host[0] <= 'z'
                        ? g_host[0] - 32 : g_host[0]) : 'O'), 0 };
    draw_text(rt, init, g_ava, av, 0xFFFFFF);
}

static void draw_sidebar(ID2D1RenderTarget *rt, const oc_model *m, float h) {
    fill(rt, rf(RAIL_W, 0, RAIL_W + SIDEBAR_W, h), OC_COL_SIDEBAR);

    /* Workspace header. */
    D2D1_RECT_F hdr = rf(RAIL_W + 16, 0, RAIL_W + SIDEBAR_W - 12, HEADER_H);
    draw_text(rt, g_host[0] ? g_host : "OpenChime", g_hdr, hdr, OC_COL_TEXT);

    /* "Channels" section label. */
    D2D1_RECT_F sec = rf(RAIL_W + 16, HEADER_H, RAIL_W + SIDEBAR_W - 12, HEADER_H + 24);
    draw_text(rt, "CHANNELS", g_small, sec, OC_COL_FAINT);

    float y = HEADER_H + 26;
    g_n_rows = 0;
    for (size_t i = 0; i < m->n_channels; i++) {
        const oc_channel *c = &m->channels[i];
        if (!c->name || !c->name[0]) continue;             /* unnamed = not listed yet */
        if (y > h) break;
        float x0 = RAIL_W + 8, x1 = RAIL_W + SIDEBAR_W - 8;
        int selected = (c->channel_id == g_sel);
        int unread = c->unread > 0;

        if (selected)
            fill_round(rt, rf(x0, y + 2, x1, y + ROW_H - 2), 6.0f, OC_COL_SELECT);

        char label[160];
        channel_label(m, c, label, sizeof label);
        D2D1_RECT_F lr = rf(x0 + 10, y, x1 - 44, y + ROW_H);
        uint32_t fg = selected ? OC_COL_TEXT : (unread ? OC_COL_TEXT : OC_COL_MUTED);
        draw_text(rt, label, unread ? g_ui_b : g_ui, lr, fg);

        if (unread) {
            char badge[16]; snprintf(badge, sizeof badge, "%d", c->unread);
            D2D1_RECT_F br = rf(x1 - 40, y + 6, x1 - 10, y + ROW_H - 6);
            fill_round(rt, br, 9.0f, OC_COL_ACCENT);
            IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
            draw_text(rt, badge, g_small, br, 0xFFFFFF);
            IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
        }

        if (g_n_rows < (int)(sizeof g_rows / sizeof g_rows[0])) {
            g_rows[g_n_rows].top = y; g_rows[g_n_rows].bot = y + ROW_H;
            g_rows[g_n_rows].cid = c->channel_id; g_n_rows++;
        }
        y += ROW_H;
    }
}

/* ---- transcript ---------------------------------------------------------- */

/* A message's rendered height for a given content width (creates + returns the
 * body layout so the draw pass can reuse it). */
static float msg_height(const oc_msg *msg, float content_w, IDWriteTextLayout **out_body) {
    const char *body = msg->deleted ? "(message deleted)"
                     : (msg->body && msg->body[0]) ? msg->body : " ";
    WCHAR w[2048];
    int n = to_w(body, w, 2048);
    IDWriteTextLayout *layout = NULL;
    IDWriteFactory_CreateTextLayout(g_dwrite, w, (UINT32)(n > 0 ? n : 1), g_body,
                                    content_w, 4000.0f, &layout);
    float body_h = 18.0f;
    if (layout) {
        DWRITE_TEXT_METRICS tm;
        if (SUCCEEDED(IDWriteTextLayout_GetMetrics(layout, &tm))) body_h = tm.height;
    }
    *out_body = layout;

    int extra = 0;
    if (msg->n_reactions) extra++;
    extra += msg->n_attach;
    if (msg->reply_count) extra++;
    return 22.0f + body_h + (float)extra * LINE_H + 12.0f;   /* header + body + meta + pad */
}

static void draw_message(ID2D1RenderTarget *rt, const oc_model *m, const oc_msg *msg,
                         IDWriteTextLayout *body, float x0, float y, float content_w) {
    static const uint32_t AVPAL[6] =
        { 0x2563EB, 0x3BA55D, 0xD9A441, 0xB05CCB, 0xE0725A, 0x2FA5A5 };
    float ax = x0, tx = x0 + AVA + 12;

    /* Avatar: colored circle with the author's initial. */
    const char *nm = msg->author_name[0] ? msg->author_name : oc_model_user_name(m, msg->author_id);
    if (!nm || !nm[0]) nm = "user";
    D2D1_ELLIPSE e = { { ax + AVA / 2, y + AVA / 2 }, AVA / 2, AVA / 2 };
    ID2D1RenderTarget_FillEllipse(rt, &e, paint_with(AVPAL[msg->author_id % 6]));
    char ini[2] = { (char)(nm[0] >= 'a' && nm[0] <= 'z' ? nm[0] - 32 : nm[0]), 0 };
    draw_text(rt, ini, g_ava, rf(ax, y, ax + AVA, y + AVA), 0xFFFFFF);

    /* Author + timestamp on the header line. */
    D2D1_RECT_F hl = rf(tx, y, x0 + content_w + AVA + 12, y + 20);
    draw_text(rt, nm, g_name, hl, OC_COL_TEXT);
    if (msg->server_time) {
        time_t t = (time_t)(msg->server_time / 1000);
        struct tm tv; char when[16] = "";
        if (oc_localtime_r(&t, &tv)) strftime(when, sizeof when, "%H:%M", &tv);
        draw_text(rt, when, g_time, hl, OC_COL_FAINT);
    }

    /* Body. */
    float by = y + 22;
    if (body) {
        D2D1_POINT_2F org = { tx, by };
        uint32_t bcol = msg->deleted ? OC_COL_FAINT : OC_COL_TEXT;
        ID2D1RenderTarget_DrawTextLayout(rt, org, body, paint_with(bcol),
                                         D2D1_DRAW_TEXT_OPTIONS_NONE);
        DWRITE_TEXT_METRICS tm;
        if (SUCCEEDED(IDWriteTextLayout_GetMetrics(body, &tm))) by += tm.height;
        else by += 18;
    }
    if (msg->edited && !msg->deleted)
        draw_text(rt, " (edited)", g_time, rf(tx, y, x0 + content_w + AVA + 12, y + 20), OC_COL_FAINT);

    /* Meta lines: reactions, attachments, thread. */
    if (msg->n_reactions) {
        char line[256] = ""; size_t off = 0;
        for (int i = 0; i < msg->n_reactions && off < sizeof line - 32; i++)
            off += (size_t)snprintf(line + off, sizeof line - off, "%s %u   ",
                                    msg->reactions[i].emoji, msg->reactions[i].count);
        draw_text(rt, line, g_small, rf(tx, by, x0 + content_w + AVA + 12, by + LINE_H), OC_COL_MUTED);
        by += LINE_H;
    }
    for (int i = 0; i < msg->n_attach; i++) {
        char line[200];
        if (msg->attach[i].reclaimed)
            snprintf(line, sizeof line, "\xF0\x9F\x93\x8E %s (no longer available)", msg->attach[i].filename);
        else
            snprintf(line, sizeof line, "\xF0\x9F\x93\x8E %s", msg->attach[i].filename);
        draw_text(rt, line, g_small, rf(tx, by, x0 + content_w + AVA + 12, by + LINE_H), OC_COL_ACCENT);
        by += LINE_H;
    }
    if (msg->reply_count) {
        char line[64];
        snprintf(line, sizeof line, "\xE2\x86\xB3 %u %s", msg->reply_count,
                 msg->reply_count == 1 ? "reply" : "replies");
        draw_text(rt, line, g_small, rf(tx, by, x0 + content_w + AVA + 12, by + LINE_H), OC_COL_ACCENT);
    }
}

static void draw_transcript(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    const oc_channel *c = g_sel ? oc_model_channel((oc_model *)m, g_sel) : NULL;

    if (!m->authed || !c) {
        const char *msg = !m->connected ? (m->last_error[0] ? m->last_error : "connecting…")
                        : !m->authed    ? "signing in…"
                                        : "Select a channel to start reading.";
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, msg, g_ui, reg, OC_COL_MUTED);
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_LEADING);
        return;
    }
    if (c->n_msgs == 0) {
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, "No messages yet — say hello.", g_ui, reg, OC_COL_MUTED);
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_LEADING);
        return;
    }

    float pad = 20.0f;
    float x0 = reg.left + pad;
    float content_w = (reg.right - pad) - (x0 + AVA + 12);
    if (content_w < 80) content_w = 80;

    /* Render at most the most recent N messages (bounds the per-frame layouts). */
    size_t n = c->n_msgs, first = 0;
    enum { CAP = 600 };
    static IDWriteTextLayout *layouts[CAP];
    static float heights[CAP];
    if (n > CAP) { first = n - CAP; n = CAP; }

    float total = 0;
    for (size_t i = 0; i < n; i++) {
        heights[i] = msg_height(&c->msgs[first + i], content_w, &layouts[i]);
        total += heights[i];
    }

    float visible = reg.bottom - reg.top;
    g_scroll_max = total > visible ? total - visible : 0;
    if (g_scroll > g_scroll_max) g_scroll = g_scroll_max;
    if (g_scroll < 0) g_scroll = 0;

    float y = (reg.bottom - total) + g_scroll;     /* g_scroll 0 => newest pinned to bottom */

    ID2D1RenderTarget_PushAxisAlignedClip(rt, &reg, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    for (size_t i = 0; i < n; i++) {
        if (y + heights[i] >= reg.top && y <= reg.bottom)
            draw_message(rt, m, &c->msgs[first + i], layouts[i], x0, y, content_w);
        y += heights[i];
        if (layouts[i]) IDWriteTextLayout_Release(layouts[i]);
        layouts[i] = NULL;
    }
    ID2D1RenderTarget_PopAxisAlignedClip(rt);
}

/* ---- header + composer --------------------------------------------------- */

static void draw_header(ID2D1RenderTarget *rt, const oc_model *m, float x0, float w) {
    fill(rt, rf(x0, 0, x0 + w, HEADER_H), OC_COL_HEADER);
    fill(rt, rf(x0, HEADER_H - 1, x0 + w, HEADER_H), OC_COL_BORDER);
    const oc_channel *c = g_sel ? oc_model_channel((oc_model *)m, g_sel) : NULL;
    char title[160] = "OpenChime";
    if (c) channel_label(m, c, title, sizeof title);
    draw_text(rt, title, g_hdr, rf(x0 + 20, 0, x0 + w - 160, HEADER_H), OC_COL_TEXT);

    const char *status = !m->connected ? "offline" : !m->authed ? "signing in" : "connected";
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_TRAILING);
    draw_text(rt, status, g_small, rf(x0 + 20, 0, x0 + w - 20, HEADER_H),
              m->authed ? OC_COL_ONLINE : OC_COL_MUTED);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
}

static void draw_composer(ID2D1RenderTarget *rt, const oc_model *m, float x0, float w, float h) {
    float top = h - COMPOSER_H;
    fill(rt, rf(x0, top, x0 + w, h), OC_COL_BASE);
    D2D1_RECT_F box = rf(x0 + 16, top + 12, x0 + w - 16, h - 14);
    fill_round(rt, box, 8.0f, OC_COL_SIDEBAR);
    D2D1_RECT_F inner = rf(box.left + 14, box.top, box.right - 14, box.bottom);

    int can = m->authed && g_sel;
    if (g_inlen == 0) {
        draw_text(rt, can ? "Message… (Enter to send)" : "Connect to a channel to chat",
                  g_ui, inner, OC_COL_FAINT);
    } else {
        /* Text + a simple blinking block caret. */
        char shown[4008];
        int caret = (GetTickCount() / 500) % 2;
        snprintf(shown, sizeof shown, "%s%s", g_input, caret ? "\xE2\x96\x8F" : "");
        draw_text(rt, shown, g_ui, inner, OC_COL_TEXT);
    }
}

/* ---- paint --------------------------------------------------------------- */

static void paint(HWND hwnd) {
    d2d_ensure_rt(hwnd);
    if (!g_rt || !g_brush) return;
    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)g_rt;
    RECT rc; GetClientRect(hwnd, &rc);
    float W = (float)(rc.right - rc.left), H = (float)(rc.bottom - rc.top);
    const oc_model *m = model();

    ID2D1RenderTarget_BeginDraw(rt);
    D2D1_COLOR_F base = col(OC_COL_BASE);
    ID2D1RenderTarget_Clear(rt, &base);

    if (m) {
        ensure_selection(m);
        float main_x = RAIL_W + SIDEBAR_W, main_w = W - main_x;
        draw_rail(rt, H);
        draw_sidebar(rt, m, H);
        draw_header(rt, m, main_x, main_w);
        draw_transcript(rt, m, rf(main_x, HEADER_H, W, H - COMPOSER_H));
        draw_composer(rt, m, main_x, main_w, H);
    }

    HRESULT hr = ID2D1RenderTarget_EndDraw(rt, NULL, NULL);
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        if (g_brush) { ID2D1SolidColorBrush_Release(g_brush); g_brush = NULL; }
        ID2D1HwndRenderTarget_Release(g_rt);
        g_rt = NULL;
    }
}

/* ---- input --------------------------------------------------------------- */

static void input_append_cp(unsigned cp) {
    char b[4]; int n;
    if (cp < 0x80)            { b[0] = (char)cp; n = 1; }
    else if (cp < 0x800)      { b[0] = (char)(0xC0 | (cp >> 6)); b[1] = (char)(0x80 | (cp & 0x3F)); n = 2; }
    else if (cp < 0x10000)    { b[0] = (char)(0xE0 | (cp >> 12)); b[1] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[2] = (char)(0x80 | (cp & 0x3F)); n = 3; }
    else                      { b[0] = (char)(0xF0 | (cp >> 18)); b[1] = (char)(0x80 | ((cp >> 12) & 0x3F)); b[2] = (char)(0x80 | ((cp >> 6) & 0x3F)); b[3] = (char)(0x80 | (cp & 0x3F)); n = 4; }
    if (g_inlen + (size_t)n >= sizeof g_input) return;
    memcpy(g_input + g_inlen, b, (size_t)n);
    g_inlen += (size_t)n;
    g_input[g_inlen] = 0;
}

static void input_backspace(void) {
    if (g_inlen == 0) return;
    size_t i = g_inlen - 1;
    while (i > 0 && (g_input[i] & 0xC0) == 0x80) i--;   /* back over UTF-8 continuation bytes */
    g_inlen = i;
    g_input[g_inlen] = 0;
}

static void input_send(void) {
    if (!g_client || !g_sel || g_inlen == 0) return;
    oc_client_send(g_client, g_sel, g_input);
    g_inlen = 0; g_input[0] = 0; g_scroll = 0;
}

static void on_char(WPARAM wp) {
    WCHAR u = (WCHAR)wp;
    unsigned cp;
    if (u >= 0xD800 && u <= 0xDBFF) { g_hi_surrogate = u; return; }   /* await low surrogate */
    if (u >= 0xDC00 && u <= 0xDFFF && g_hi_surrogate) {
        cp = 0x10000u + (((unsigned)g_hi_surrogate - 0xD800u) << 10) + ((unsigned)u - 0xDC00u);
        g_hi_surrogate = 0;
    } else { cp = u; g_hi_surrogate = 0; }

    if (cp == 0x0D)      input_send();          /* Enter */
    else if (cp == 0x08) input_backspace();     /* Backspace */
    else if (cp == 0x1B) { g_inlen = 0; g_input[0] = 0; }  /* Esc clears */
    else if (cp >= 0x20) {
        input_append_cp(cp);
        DWORD now = GetTickCount();
        if (g_client && g_sel && now - g_last_typing > 2000) {
            oc_client_typing(g_client, g_sel);
            g_last_typing = now;
        }
    }
}

static void on_click(int x, int y) {
    if (x < RAIL_W || x > RAIL_W + SIDEBAR_W) return;
    for (int i = 0; i < g_n_rows; i++)
        if ((float)y >= g_rows[i].top && (float)y < g_rows[i].bot) {
            if (g_rows[i].cid != g_sel) select_channel(g_rows[i].cid);
            return;
        }
}

/* ---- core wiring ---------------------------------------------------------- */

static const char *store_path(void) {
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) base = getenv("APPDATA");
    if (!base || !base[0]) return NULL;
    static char path[1024];
    char dir[900];
    snprintf(dir, sizeof dir, "%s\\openchime", base);
    oc_mkdir(dir);
    snprintf(path, sizeof path, "%s\\state.db", dir);
    return path;
}

static void connect_start(const char *ws, const char *cred) {
    oc_endpoint ep;
    if (oc_resolve(ws, getenv("OPENCHIME_SUFFIX"), &ep) != OC_RESOLVE_OK) {
        snprintf(g_host, sizeof g_host, "%s", "?");
        return;
    }
    snprintf(g_host, sizeof g_host, "%s", ep.host);
    g_port = ep.port;
    snprintf(g_cred, sizeof g_cred, "%s", cred);
    g_client = oc_client_start_secure(g_host, g_port, g_cred, store_path(), NULL);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        SetTimer(hwnd, TIMER_TICK, 30, NULL);
        return 0;
    case WM_TIMER:
        if (wp == TIMER_TICK && g_client) {
            oc_client_tick(g_client);
            const oc_model *m = oc_client_model(g_client);
            if (m->authed && !g_post_auth) {          /* one-shot: pull roster + channels */
                oc_client_list_users(g_client);
                oc_client_list_channels(g_client);
                g_post_auth = 1;
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps);
        paint(hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        d2d_resize(hwnd);
        return 0;
    case WM_MOUSEWHEEL:
        g_scroll += (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 48.0f;
        if (g_scroll < 0) g_scroll = 0;
        if (g_scroll > g_scroll_max) g_scroll = g_scroll_max;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
        on_click(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_CHAR:
        on_char(wp);
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_TICK);
        if (g_client) { oc_client_stop(g_client); g_client = NULL; }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE prev, LPWSTR cmdline, int show) {
    (void)prev; (void)cmdline;

    const char *ws = "127.0.0.1:8443", *cred = "alice:pw";
    int argc = 0; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    static char aws[256], acred[264];
    if (argv && argc >= 3) {
        WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, aws, sizeof aws, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, acred, sizeof acred, NULL, NULL);
        ws = aws; cred = acred;
    }
    if (argv) LocalFree(argv);

    d2d_init();
    connect_start(ws, cred);

    WNDCLASSW wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.lpszClassName = L"OpenChimeWin";
    if (!RegisterClassW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, L"OpenChimeWin", L"OpenChime",
                    WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1080, 720,
                    NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_hdr)   IDWriteTextFormat_Release(g_hdr);
    if (g_name)  IDWriteTextFormat_Release(g_name);
    if (g_time)  IDWriteTextFormat_Release(g_time);
    if (g_body)  IDWriteTextFormat_Release(g_body);
    if (g_ui)    IDWriteTextFormat_Release(g_ui);
    if (g_ui_b)  IDWriteTextFormat_Release(g_ui_b);
    if (g_small) IDWriteTextFormat_Release(g_small);
    if (g_ava)   IDWriteTextFormat_Release(g_ava);
    if (g_brush) ID2D1SolidColorBrush_Release(g_brush);
    if (g_rt)     ID2D1HwndRenderTarget_Release(g_rt);
    if (g_dwrite) IDWriteFactory_Release(g_dwrite);
    if (g_factory) ID2D1Factory_Release(g_factory);
    return 0;
}
