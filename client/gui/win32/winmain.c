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
#include <richedit.h>         /* MSFTEDIT_CLASS composer */
#include <commdlg.h>          /* GetSaveFileNameW (attachment download) */
#include <dwmapi.h>           /* DwmSetWindowAttribute (dark title bar) */
#include <d2d1.h>
#include <dwrite.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "client.h"
#include "model.h"
#include "resolve.h"
#include "store.h"            /* peek a stored session token (skip the login box) */
#include "oc_port.h"          /* oc_mkdir, oc_localtime_r */
#include "protocol.h"         /* OC_CHANNEL_KIND_DM, OC_PRESENCE_* */
#include "theme.h"
#include "icons.h"            /* baked Lucide vector icons (cross-platform) */

/* mingw ships IID_ID2D1Factory in libuuid but not IID_IDWriteFactory; define it
 * locally so we don't depend on the toolchain's GUID table for DWrite. */
static const GUID OC_IID_IDWriteFactory =
    { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };

#define TIMER_TICK 1

/* Layout metrics (device pixels; per-monitor DPI is a later phase). */
#define RAIL_W      70.0f     /* pixel-matched to the Slack rail reference */
#define SIDEBAR_W   250.0f
#define HEADER_H    56.0f
#define COMPOSER_H  86.0f
#define ROW_H       32.0f     /* a sidebar channel row */
#define AVA         36.0f     /* transcript avatar diameter */
#define LINE_H      19.0f     /* an extra (reaction/attach/thread) line */
#define MEMBERS_W   220.0f    /* the (toggleable) right-hand members pane */

/* Primary views selected by the left-nav rail (Slack-style). HOME/DMS/ADMIN are
 * real today; ACTIVITY/FILES/LATER/NOTIFICATIONS are stubs until their features
 * land. Rail item `act` values <0 are special (switcher / new / profile / more). */
enum { VIEW_HOME = 0, VIEW_DMS, VIEW_ACTIVITY, VIEW_FILES, VIEW_LATER,
       VIEW_ADMIN, VIEW_NOTIFICATIONS, VIEW_COUNT };
enum { NAV_SWITCHER = -2, NAV_NEW = -3, NAV_PROFILE = -4, NAV_MORE = -5 };
#define BODY_DIP    15.0f     /* message + composer text size (shared, so they match) */

/* Common reactions offered by the message context menu. */
static const char *REACT_EMO[6] = {
    "\xF0\x9F\x91\x8D", "\xE2\x9D\xA4\xEF\xB8\x8F", "\xF0\x9F\x98\x82",
    "\xF0\x9F\x8E\x89", "\xF0\x9F\x98\xAE", "\xF0\x9F\x98\xA2"
};

/* ---- app state ----------------------------------------------------------- */

static oc_client *g_client;
static char       g_cred[264];
static char       g_host[256];
static int        g_port;
static char       g_cur_ws[256];        /* the workspace string we connected with */

static ID2D1Factory          *g_factory;
static IDWriteFactory        *g_dwrite;
static ID2D1HwndRenderTarget *g_rt;
static ID2D1SolidColorBrush  *g_brush;      /* one reusable brush; recolored per draw */
static ID2D1SolidColorBrush  *g_brush2;     /* faint brush for inline "(edited)" effect */

static IDWriteTextFormat *g_hdr;    /* channel title */
static IDWriteTextFormat *g_name;   /* message author (semibold) */
static IDWriteTextFormat *g_time;   /* timestamp (trailing-aligned) */
static IDWriteTextFormat *g_body;   /* message body (wrapping) */
static IDWriteTextFormat *g_ui;     /* sidebar rows / composer */
static IDWriteTextFormat *g_ui_b;   /* unread sidebar rows (semibold) */
static IDWriteTextFormat *g_small;  /* subtitles / meta lines */
static IDWriteTextFormat *g_ava;    /* avatar initial (centered) */
static IDWriteTextFormat *g_rail;   /* tiny rail item labels (centered) */
/* Lucide vector icons: geometry cached once (device-independent, from the factory,
 * so it survives render-target recreation and works for both paint and shots). */
static ID2D1PathGeometry *g_icon_geo[OC_ICON_COUNT];
static ID2D1StrokeStyle  *g_icon_stroke;   /* round cap/join, matching Lucide */

static uint64_t g_sel;              /* selected channel id (0 = none) */
static float    g_scroll;           /* px scrolled up from the bottom of the transcript */
static float    g_scroll_max;       /* recomputed each paint, for input clamping */
static uint64_t g_hover_mid;        /* transcript message under the cursor (0 = none) */

/* Custom transcript scrollbar (drawn over the D2D surface). Geometry is captured
 * each paint so the mouse handlers can hit-test and drag the thumb. */
static D2D1_RECT_F g_sbar_thumb;    /* thumb hit-box (empty when not scrollable) */
static float    g_sbar_track_top;   /* track origin + travel range, for drag mapping */
static float    g_sbar_travel;
static int      g_sbar_drag;        /* dragging the scrollbar thumb */
static float    g_sbar_grab;        /* cursor offset within the thumb at grab */
static uint8_t  g_post_auth;        /* one-shot post-auth roster/channel refresh */

static uint64_t g_backfilled[512];  /* channels we've already asked history for */
static int      g_n_backfilled;

/* The composer is a native RichEdit child (IME / selection / clipboard / undo);
 * we subclass it to make Enter send and Shift+Enter insert a newline. */
static HWND     g_re;
static WNDPROC  g_re_oldproc;
static DWORD    g_last_typing;

/* Sidebar row hit-boxes, captured during paint for WM_LBUTTONDOWN. */
static struct { float top, bot; uint64_t cid; } g_rows[512];
static int g_n_rows;

/* Transcript message hit-boxes (context menu + text selection). bx/by = the body
 * layout's top-left; cw = its wrap width — enough to re-hit-test on mouse events. */
static struct { float top, bot, bx, by, cw; uint64_t mid; } g_msgrows[600];
static int g_n_msgrows;

/* Transcript text selection (DirectWrite hit-testing over the custom surface).
 * Anchor/focus are (message id, UTF-16 offset); order is resolved at draw time. */
static uint64_t g_sel_a_mid, g_sel_f_mid;
static uint32_t g_sel_a_pos, g_sel_f_pos;
static int      g_has_sel;      /* a (possibly empty) selection exists */
static int      g_selecting;    /* left button held, dragging a selection */

/* Members-pane row hit-boxes. */
static struct { float top, bot; uint64_t uid; } g_memrows[256];
static int g_n_memrows;

/* Search-overlay result hit-boxes (row -> its channel). */
static struct { float top, bot; uint64_t cid; } g_searchrows[128];
static int g_n_searchrows;

/* Webhook-overlay row hit-boxes (row -> webhook id, for delete). */
static struct { float top, bot; uint64_t wid; } g_webrows[64];
static int g_n_webrows;

static int      g_show_members = 1;     /* members pane visible */
static D2D1_RECT_F g_members_btn;       /* header toggle hit-box */
static D2D1_RECT_F g_ws_hdr_btn;        /* channel-column workspace header (opens ws menu) */
static D2D1_RECT_F g_hdr_gear, g_hdr_compose;   /* header settings + compose buttons */
static HWND     g_find;                 /* "Find a conversation" filter box (native EDIT) */
static char     g_find_filter[64];      /* current filter text (lowercased) */
static HBRUSH   g_find_brush;           /* dark bg for the find box */
static D2D1_RECT_F g_rail_btn;          /* workspace-avatar hit-box (app menu) */
static int g_view = VIEW_HOME;          /* current primary view (rail selection) */
static int g_nav_hover = -100;          /* rail item under the cursor (act value) */
/* Rail item hit-boxes, captured during paint. `act` >=0 is a VIEW_*, <0 a NAV_*. */
static struct { float top, bot; int act; } g_navrows[16];
static int g_n_navrows;
/* Overflow "More": items that didn't fit, revealed in a flyout. */
static int g_more_open;
static float g_more_y;                   /* the More item's top, to anchor the flyout */
static struct { int act, icon; const char *label; } g_more[8];
static int g_n_more;
static struct { float top, bot; int act; } g_moreflyrows[8];
static int g_n_moreflyrows;

/* ---- custom D2D dropdown menus (workspace / profile / new / switcher) -------
 * One reusable floating menu replaces the old TrackPopupMenu. An opener fills
 * g_mi[] + anchor; draw_menu() renders it last so it floats; on_click routes to
 * menu_dispatch(). Item kinds: ITEM (icon+label+cmd), SECTION (faint header),
 * SEP. MENU_WS/MENU_SWITCHER also draw a workspace header block on top. */
enum { MENU_NONE = 0, MENU_WS, MENU_PROFILE, MENU_NEW, MENU_SWITCHER };
enum { MK_ITEM = 0, MK_SECTION, MK_SEP };
struct menuitem { int kind, cmd, icon, danger; char label[72]; };
static int   g_menu;                     /* which menu is open (MENU_NONE = none) */
static struct menuitem g_mi[28];
static int   g_n_mi;
static float g_menu_x, g_menu_y, g_menu_w;   /* panel top-left + width */
static int   g_menu_hover = -1;              /* hovered item index */
static int   g_menu_headerblock;             /* draw the workspace header on top */
static struct { float top, bot; int cmd; } g_mirows[28];
static int   g_n_mirows;
/* Workspace switcher list (built from the store book at open time). */
static struct { char ws[256], label[80]; int current; } g_sw[16];
static int   g_n_sw;

static D2D1_RECT_F g_attach_btn;        /* composer attach (+) hit-box */
static D2D1_RECT_F g_send_btn;          /* composer send-button hit-box */
static uint64_t g_edit_msg;             /* non-zero => composer is editing this message */

/* Test/automation hook: when OPENCHIME_TEST_DIR is set, the app polls <dir>/cmd
 * each tick and can render itself to a file — a display-independent way to drive
 * and screenshot the client from WSL/CI without screen-scraping. */
static char g_test_dir[512];

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

static ID2D1Brush *paint_alpha(uint32_t rgb, float a) {
    D2D1_COLOR_F c = col(rgb); c.a = a;
    ID2D1SolidColorBrush_SetColor(g_brush, &c);
    return (ID2D1Brush *)g_brush;
}

static D2D1_RECT_F rf(float l, float t, float r, float b) {
    D2D1_RECT_F x = { l, t, r, b }; return x;
}

/* theme.h stores 0xRRGGBB; GDI/Win32 want a COLORREF (0x00BBGGRR). */
#define OCRGB(x) RGB(((x) >> 16) & 0xff, ((x) >> 8) & 0xff, (x) & 0xff)

/* Ask DWM for a dark window caption (native title bar matching the shell).
 * Attr 20 = DWMWA_USE_IMMERSIVE_DARK_MODE on Win10 1903+, 19 on 1809. */
static void apply_dark_titlebar(HWND h) {
    BOOL dark = TRUE;
    if (FAILED(DwmSetWindowAttribute(h, 20, &dark, sizeof dark)))
        DwmSetWindowAttribute(h, 19, &dark, sizeof dark);
}

static void fill(ID2D1RenderTarget *rt, D2D1_RECT_F r, uint32_t rgb) {
    ID2D1RenderTarget_FillRectangle(rt, &r, paint_with(rgb));
}

static void fill_round(ID2D1RenderTarget *rt, D2D1_RECT_F r, float rad, uint32_t rgb) {
    D2D1_ROUNDED_RECT rr = { r, rad, rad };
    ID2D1RenderTarget_FillRoundedRectangle(rt, &rr, paint_with(rgb));
}

/* Rounded fill with alpha — for translucent overlays (Slack-style selection). */
static void fill_round_a(ID2D1RenderTarget *rt, D2D1_RECT_F r, float rad, uint32_t rgb, float a) {
    D2D1_ROUNDED_RECT rr = { r, rad, rad };
    ID2D1RenderTarget_FillRoundedRectangle(rt, &rr, paint_alpha(rgb, a));
}

static void stroke_round(ID2D1RenderTarget *rt, D2D1_RECT_F r, float rad,
                         uint32_t rgb, float w) {
    D2D1_ROUNDED_RECT rr = { r, rad, rad };
    ID2D1RenderTarget_DrawRoundedRectangle(rt, &rr, paint_with(rgb), w, NULL);
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

/* Build (once) a stroked path geometry for a Lucide icon in its 24x24 space. */
static ID2D1PathGeometry *icon_geo(int id) {
    if (id < 0 || id >= OC_ICON_COUNT) return NULL;
    if (g_icon_geo[id]) return g_icon_geo[id];
    ID2D1PathGeometry *geo = NULL;
    if (FAILED(ID2D1Factory_CreatePathGeometry(g_factory, &geo)) || !geo) return NULL;
    ID2D1GeometrySink *sink = NULL;
    if (FAILED(ID2D1PathGeometry_Open(geo, &sink)) || !sink) {
        ID2D1PathGeometry_Release(geo); return NULL;
    }
    const oc_icon *ic = &OC_ICONS[id];
    int open = 0;
    for (int i = 0; i < ic->n; i++) {
        const oc_icon_seg *s = &ic->segs[i];
        if (s->op == 'M') {
            if (open) ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_OPEN);
            D2D1_POINT_2F p = { s->x0, s->y0 };
            ID2D1GeometrySink_BeginFigure(sink, p, D2D1_FIGURE_BEGIN_HOLLOW);   /* stroked */
            open = 1;
        } else if (s->op == 'L') {
            D2D1_POINT_2F p = { s->x0, s->y0 };
            ID2D1GeometrySink_AddLine(sink, p);
        } else if (s->op == 'C') {
            D2D1_BEZIER_SEGMENT b = { { s->x0, s->y0 }, { s->x1, s->y1 }, { s->x2, s->y2 } };
            ID2D1GeometrySink_AddBezier(sink, &b);
        } else if (s->op == 'Z') {
            if (open) { ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_CLOSED); open = 0; }
        }
    }
    if (open) ID2D1GeometrySink_EndFigure(sink, D2D1_FIGURE_END_OPEN);
    ID2D1GeometrySink_Close(sink);
    ID2D1GeometrySink_Release(sink);
    g_icon_geo[id] = geo;
    return geo;
}

/* Draw a Lucide icon stroked (2px, round caps) fitted + centered in `box`. */
static void draw_lucide(ID2D1RenderTarget *rt, int id, D2D1_RECT_F box, uint32_t rgb) {
    ID2D1PathGeometry *geo = icon_geo(id);
    if (!geo) return;
    float bw = box.right - box.left, bh = box.bottom - box.top;
    float side = bw < bh ? bw : bh, s = side / OC_ICON_VIEWBOX;
    D2D1_MATRIX_3X2_F m = {{{ s, 0, 0, s,
                           box.left + (bw - side) / 2, box.top + (bh - side) / 2 }}};
    ID2D1RenderTarget_SetTransform(rt, &m);
    ID2D1RenderTarget_DrawGeometry(rt, (ID2D1Geometry *)geo, paint_with(rgb), 2.0f, g_icon_stroke);
    D2D1_MATRIX_3X2_F ident = {{{ 1, 0, 0, 1, 0, 0 }}};
    ID2D1RenderTarget_SetTransform(rt, &ident);
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
    g_body  = mk_fmt(UI, BODY_DIP, DWRITE_FONT_WEIGHT_NORMAL,  DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_PARAGRAPH_ALIGNMENT_NEAR,   1);
    g_ui    = mk_fmt(UI, 14.5f, DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0);
    g_ui_b  = mk_fmt(UI, 14.5f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0);
    g_small = mk_fmt(UI, 12.5f, DWRITE_FONT_WEIGHT_NORMAL,    DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0);
    g_ava   = mk_fmt(UI, 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER,   DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0);
    g_rail  = mk_fmt(UI, 9.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0);
    /* Round-capped stroke style for the Lucide line icons. */
    D2D1_STROKE_STYLE_PROPERTIES ssp;
    ZeroMemory(&ssp, sizeof ssp);
    ssp.startCap = ssp.endCap = ssp.dashCap = D2D1_CAP_STYLE_ROUND;
    ssp.lineJoin = D2D1_LINE_JOIN_ROUND;
    ssp.miterLimit = 10.0f; ssp.dashStyle = D2D1_DASH_STYLE_SOLID;
    ID2D1Factory_CreateStrokeStyle(g_factory, &ssp, NULL, 0, &g_icon_stroke);
    /* Roomier line height on message bodies for readability (Slack-like). */
    if (g_body)
        IDWriteTextFormat_SetLineSpacing(g_body, DWRITE_LINE_SPACING_METHOD_UNIFORM, 22.0f, 16.5f);
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
        D2D1_COLOR_F faint = col(OC_COL_FAINT);
        ID2D1RenderTarget_CreateSolidColorBrush((ID2D1RenderTarget *)g_rt, &faint, NULL, &g_brush2);
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

static const oc_msg *find_msg(const oc_channel *c, uint64_t mid);   /* defined below */

/* Our own tenant role (for gating admin actions); OC_ROLE_MEMBER if unknown. */
static uint8_t self_role(const oc_model *m) {
    for (size_t i = 0; i < m->n_users; i++)
        if (m->users[i].user_id == m->user_id) return m->users[i].role;
    return OC_ROLE_MEMBER;
}

static const char *role_label(uint8_t role) {
    return role == OC_ROLE_OWNER ? "owner" : role == OC_ROLE_ADMIN ? "admin" : "";
}

static int already_backfilled(uint64_t cid) {
    for (int i = 0; i < g_n_backfilled; i++) if (g_backfilled[i] == cid) return 1;
    return 0;
}

static void close_overlays(void) {
    const oc_model *mm = model();
    if (!mm) return;
    if (mm->thread_open)    oc_client_close_thread(g_client);
    if (mm->search_open)    oc_client_close_search(g_client);
    if (mm->reactlist_open) oc_client_close_reactions(g_client);
    if (mm->weblist_open)   oc_client_close_webhooks(g_client);
    if (mm->storage_open)   oc_client_toggle_storage(g_client, 0);
    if (mm->audit_open)     oc_client_toggle_audit(g_client, 0);
}

static int any_overlay(const oc_model *m) {
    return m && (m->thread_open || m->search_open || m->reactlist_open ||
                 m->weblist_open || m->storage_open || m->audit_open);
}

static void select_channel(uint64_t cid) {
    if (!g_client || !cid) return;
    close_overlays();
    g_has_sel = 0;                       /* drop any transcript text selection */
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

#define RAIL_IH 68.0f     /* rail item pitch — pixel-matched to Slack (center-to-center) */

static void rail_hit(float top, float bot, int act) {
    if (g_n_navrows < (int)(sizeof g_navrows / sizeof g_navrows[0])) {
        g_navrows[g_n_navrows].top = top; g_navrows[g_n_navrows].bot = bot;
        g_navrows[g_n_navrows].act = act; g_n_navrows++;
    }
}

static void ws_display_name(const oc_model *m, char *out, size_t cap);   /* fwd */

/* One rail item: a Lucide icon + tiny label. Matches Slack: selected is a
 * subtle translucent-white rounded square with a WHITE icon; unselected icons +
 * labels are bright (not muted); hover is a fainter overlay. */
static void rail_item(ID2D1RenderTarget *rt, float y, int icon, const char *label, int act) {
    int selected = (act >= 0 && act == g_view);
    int hovered  = (act == g_nav_hover);
    /* Slack spec: 36x36 rounded square (radius ~10), 20px icon centered, label
     * below; item pitch RAIL_IH. */
    float cx = RAIL_W / 2;
    D2D1_RECT_F sq = rf(cx - 18, y + 6, cx + 18, y + 42);          /* 36x36 */
    uint32_t icon_col = OC_COL_RAIL_ICON;
    if (selected)     { fill_round_a(rt, sq, 10.0f, 0xFFFFFF, 0.16f); icon_col = 0xFFFFFF; }
    else if (hovered) { fill_round_a(rt, sq, 10.0f, 0xFFFFFF, 0.08f); icon_col = 0xFFFFFF; }
    draw_lucide(rt, icon, rf(cx - 10, y + 14, cx + 10, y + 34), icon_col);   /* 20px */
    if (label) draw_text(rt, label, g_rail, rf(0, y + 45, RAIL_W, y + 61),
                         selected ? 0xFFFFFF : OC_COL_RAIL_ICON);
    rail_hit(y, y + RAIL_IH, act);
}

/* The main nav items (the user's list). "More" is synthesized only on overflow. */
static const struct { int act; int icon; const char *label; int admin; } RAIL_ITEMS[] = {
    { VIEW_HOME,     OC_ICON_HOME,     "Home",     0 },
    { VIEW_DMS,      OC_ICON_DMS,      "DMs",      0 },
    { VIEW_ACTIVITY, OC_ICON_ACTIVITY, "Activity", 0 },
    { VIEW_FILES,    OC_ICON_FILE,     "Files",    0 },
    { VIEW_LATER,    OC_ICON_BOOKMARK, "Later",    0 },
    { VIEW_ADMIN,    OC_ICON_SETTINGS, "Admin",    1 },
};
#define RAIL_N_ITEMS ((int)(sizeof RAIL_ITEMS / sizeof RAIL_ITEMS[0]))

static void draw_rail(ID2D1RenderTarget *rt, const oc_model *m, float h) {
    fill(rt, rf(0, 0, RAIL_W, h), OC_COL_RAIL);
    g_n_navrows = 0;

    /* Top: workspace-switcher — a 36x36 rounded square (Slack spec) with the
     * workspace's initial (its display name, not the host). */
    float cx = RAIL_W / 2;
    D2D1_RECT_F av = rf(cx - 18, 14, cx + 18, 50);
    g_rail_btn = av;
    fill_round(rt, av, 12.0f, OC_COL_ACCENT);
    char wsn[80]; ws_display_name(m, wsn, sizeof wsn);
    char init[2] = { (char)(wsn[0] ? (wsn[0] >= 'a' && wsn[0] <= 'z' ? wsn[0] - 32 : wsn[0]) : 'O'), 0 };
    draw_text(rt, init, g_ava, av, 0xFFFFFF);
    rail_hit(av.top - 6, av.bottom + 6, NAV_SWITCHER);
    fill(rt, rf(14, 58, RAIL_W - 14, 59), OC_COL_BORDER);   /* divider */

    /* Gate admin-only items, then overflow the tail into "More" when there isn't
     * enough vertical space (fold Admin first, up toward Home; Home never folds). */
    int vis[RAIL_N_ITEMS], nv = 0;
    for (int i = 0; i < RAIL_N_ITEMS; i++)
        if (!RAIL_ITEMS[i].admin || (m && self_role(m) >= OC_ROLE_ADMIN)) vis[nv++] = i;

    float y = 64;                                   /* below the workspace + divider */
    float by = h - 3 * RAIL_IH - 6;                 /* bottom cluster top */
    int maxfit = (int)((by - y) / RAIL_IH);
    if (maxfit < 1) maxfit = 1;
    int shown = nv, overflow = 0;
    if (nv > maxfit) { shown = maxfit - 1; if (shown < 1) shown = 1; overflow = 1; }

    g_n_more = 0;
    for (int k = 0; k < shown; k++) {
        rail_item(rt, y, RAIL_ITEMS[vis[k]].icon, RAIL_ITEMS[vis[k]].label, RAIL_ITEMS[vis[k]].act);
        y += RAIL_IH;
    }
    if (overflow) {
        for (int k = shown; k < nv && g_n_more < (int)(sizeof g_more / sizeof g_more[0]); k++) {
            g_more[g_n_more].act = RAIL_ITEMS[vis[k]].act;
            g_more[g_n_more].icon = RAIL_ITEMS[vis[k]].icon;
            g_more[g_n_more].label = RAIL_ITEMS[vis[k]].label;
            g_n_more++;
        }
        g_more_y = y;
        rail_item(rt, y, OC_ICON_ELLIPSIS, "More", NAV_MORE);
        y += RAIL_IH;
    } else {
        g_more_open = 0;   /* nothing folded — no flyout */
    }

    /* Bottom cluster (elastic spacer above): New, Alerts, Profile. */
    rail_item(rt, by,               OC_ICON_PLUS, "New",    NAV_NEW);
    rail_item(rt, by + RAIL_IH,     OC_ICON_BELL, "Alerts", VIEW_NOTIFICATIONS);
    /* Profile: a colored initial avatar for the signed-in user. */
    {
        float py = by + 2 * RAIL_IH;
        if (NAV_PROFILE == g_nav_hover)
            fill_round_a(rt, rf(cx - 18, py + 6, cx + 18, py + 42), 10.0f, 0xFFFFFF, 0.08f);
        const char *nm = m ? oc_model_user_name(m, m->user_id) : "";
        char pi[2] = { (char)((nm && nm[0]) ? (nm[0] >= 'a' && nm[0] <= 'z' ? nm[0] - 32 : nm[0]) : 'U'), 0 };
        D2D1_ELLIPSE e = { { cx, py + 24 }, 15, 15 };
        ID2D1RenderTarget_FillEllipse(rt, &e, paint_with(OC_COL_ACCENT_DIM));
        draw_text(rt, pi, g_ava, rf(0, py + 9, RAIL_W, py + 39), 0xFFFFFF);
        draw_text(rt, "You", g_rail, rf(0, py + 45, RAIL_W, py + 61), OC_COL_RAIL_ICON);
        rail_hit(py, py + RAIL_IH, NAV_PROFILE);
    }
}

/* The "More" overflow flyout: a floating list of the folded rail items, anchored
 * beside the More icon. Drawn last so it floats over the pane. */
static void draw_more_flyout(ID2D1RenderTarget *rt) {
    g_n_moreflyrows = 0;
    if (!g_more_open || g_n_more == 0) return;
    float rowh = 40, w = 196, pad = 6;
    float x0 = RAIL_W + 6, y0 = g_more_y;
    D2D1_RECT_F panel = rf(x0, y0, x0 + w, y0 + g_n_more * rowh + 2 * pad);
    fill_round(rt, rf(panel.left + 2, panel.top + 3, panel.right + 2, panel.bottom + 3), 12.0f,
               OC_COL_RAIL);                                  /* soft shadow */
    fill_round(rt, panel, 12.0f, OC_COL_INPUT);
    stroke_round(rt, panel, 12.0f, OC_COL_BORDER, 1.0f);
    float y = y0 + pad;
    for (int i = 0; i < g_n_more; i++) {
        int hovered = (g_more[i].act == g_nav_hover);
        if (hovered) fill_round(rt, rf(x0 + 4, y + 2, panel.right - 4, y + rowh - 2), 8.0f, OC_COL_HOVER);
        draw_lucide(rt, g_more[i].icon, rf(x0 + 12, y + 9, x0 + 34, y + 31), OC_COL_TEXT);
        draw_text(rt, g_more[i].label, g_ui, rf(x0 + 44, y, panel.right - 8, y + rowh), OC_COL_TEXT);
        g_moreflyrows[g_n_moreflyrows].top = y;
        g_moreflyrows[g_n_moreflyrows].bot = y + rowh;
        g_moreflyrows[g_n_moreflyrows].act = g_more[i].act;
        g_n_moreflyrows++;
        y += rowh;
    }
}

/* ---- custom dropdown menus ----------------------------------------------- */

static void mi_add(int kind, int cmd, const char *label, int danger) {
    if (g_n_mi >= (int)(sizeof g_mi / sizeof g_mi[0])) return;
    struct menuitem *it = &g_mi[g_n_mi++];
    it->kind = kind; it->cmd = cmd; it->icon = -1; it->danger = danger;
    it->label[0] = 0;
    if (label) snprintf(it->label, sizeof it->label, "%s", label);
}
#define mi_item(cmd, label)    mi_add(MK_ITEM, (cmd), (label), 0)
#define mi_item_d(cmd, label)  mi_add(MK_ITEM, (cmd), (label), 1)
#define mi_section(label)      mi_add(MK_SECTION, 0, (label), 0)
#define mi_sep()               mi_add(MK_SEP, 0, NULL, 0)

/* Workspace display name, or the host's leading label as a fallback. */
static void ws_display_name(const oc_model *m, char *out, size_t cap) {
    const char *nm = m ? oc_model_workspace_name(m) : "";
    if (nm && nm[0]) { snprintf(out, cap, "%s", nm); return; }
    size_t i = 0;
    while (g_host[i] && g_host[i] != '.' && g_host[i] != ':' && i < cap - 1) { out[i] = g_host[i]; i++; }
    out[i] = 0;
    if (!out[0]) snprintf(out, cap, "OpenChime");
}

static void ws_mode_line(const oc_model *m, char *out, size_t cap) {
    const char *dn = m ? oc_model_deployment_name(m) : "standalone";
    char cap_dn[24]; snprintf(cap_dn, sizeof cap_dn, "%s", dn);
    if (cap_dn[0] >= 'a' && cap_dn[0] <= 'z') cap_dn[0] -= 32;
    uint32_t mu = m ? oc_model_max_users(m) : 0;
    if (mu > 0) snprintf(out, cap, "%s \xC2\xB7 %u users", cap_dn, mu);
    else        snprintf(out, cap, "%s", cap_dn);
}

static float menu_item_h(int kind) { return kind == MK_ITEM ? 36.0f : kind == MK_SECTION ? 24.0f : 11.0f; }

static void draw_menu(ID2D1RenderTarget *rt) {
    g_n_mirows = 0;
    if (!g_menu) return;
    const oc_model *m = model();
    float pad = 6, x = g_menu_x, y = g_menu_y, w = g_menu_w;
    float hh = g_menu_headerblock ? 66.0f : 0.0f;
    float total = pad * 2 + hh;
    for (int i = 0; i < g_n_mi; i++) total += menu_item_h(g_mi[i].kind);
    D2D1_RECT_F panel = rf(x, y, x + w, y + total);
    fill_round(rt, rf(panel.left + 2, panel.top + 4, panel.right + 2, panel.bottom + 4), 12.0f, OC_COL_RAIL);
    fill_round(rt, panel, 12.0f, OC_COL_INPUT);
    stroke_round(rt, panel, 12.0f, OC_COL_BORDER, 1.0f);

    float cy = y + pad;
    if (g_menu_headerblock) {
        D2D1_RECT_F av = rf(x + 14, cy + 10, x + 54, cy + 50);
        fill_round(rt, av, 10.0f, OC_COL_ACCENT);
        char nm[80]; ws_display_name(m, nm, sizeof nm);
        char init[2] = { (char)(nm[0] ? (nm[0] >= 'a' && nm[0] <= 'z' ? nm[0] - 32 : nm[0]) : 'O'), 0 };
        draw_text(rt, init, g_ava, av, 0xFFFFFF);
        draw_text(rt, nm, g_name, rf(x + 64, cy + 9, panel.right - 12, cy + 30), OC_COL_TEXT);
        char hostline[288]; snprintf(hostline, sizeof hostline, "%s:%d", g_host, g_port);
        draw_text(rt, hostline, g_small, rf(x + 64, cy + 28, panel.right - 12, cy + 45), OC_COL_MUTED);
        char mode[64]; ws_mode_line(m, mode, sizeof mode);
        draw_text(rt, mode, g_small, rf(x + 64, cy + 44, panel.right - 12, cy + 61), OC_COL_FAINT);
        cy += hh;
        fill(rt, rf(x + 8, cy, panel.right - 8, cy + 1), OC_COL_BORDER);
    }
    for (int i = 0; i < g_n_mi; i++) {
        float ih = menu_item_h(g_mi[i].kind);
        if (g_mi[i].kind == MK_SEP) {
            fill(rt, rf(x + 10, cy + ih / 2, panel.right - 10, cy + ih / 2 + 1), OC_COL_BORDER);
        } else if (g_mi[i].kind == MK_SECTION) {
            draw_text(rt, g_mi[i].label, g_small, rf(x + 16, cy + 5, panel.right - 10, cy + ih), OC_COL_FAINT);
        } else {
            if (g_menu_hover == i)
                fill_round(rt, rf(x + 5, cy + 2, panel.right - 5, cy + ih - 2), 7.0f, OC_COL_HOVER);
            uint32_t col = g_mi[i].danger ? OC_COL_DANGER : OC_COL_TEXT;
            draw_text(rt, g_mi[i].label, g_ui, rf(x + 16, cy, panel.right - 12, cy + ih), col);
            if (g_n_mirows < (int)(sizeof g_mirows / sizeof g_mirows[0])) {
                g_mirows[g_n_mirows].top = cy; g_mirows[g_n_mirows].bot = cy + ih;
                g_mirows[g_n_mirows].cmd = g_mi[i].cmd; g_n_mirows++;
            }
        }
        cy += ih;
    }
}

static void draw_sidebar(ID2D1RenderTarget *rt, const oc_model *m, float h) {
    fill(rt, rf(RAIL_W, 0, RAIL_W + SIDEBAR_W, h), OC_COL_SIDEBAR);

    float x1 = RAIL_W + SIDEBAR_W - 12;
    /* Header: workspace name + chevron (opens ws menu), with settings + compose
     * icon buttons on the right (Slack channel-column header). */
    g_hdr_compose = rf(x1 - 24, 16, x1, 40);
    g_hdr_gear    = rf(x1 - 54, 16, x1 - 30, 40);
    draw_lucide(rt, OC_ICON_SQUARE_PEN, g_hdr_compose, OC_COL_MUTED);
    draw_lucide(rt, OC_ICON_SETTINGS,   g_hdr_gear,    OC_COL_MUTED);
    g_ws_hdr_btn = rf(RAIL_W, 0, g_hdr_gear.left - 4, HEADER_H);
    char wsname[80]; ws_display_name(m, wsname, sizeof wsname);
    draw_text(rt, wsname, g_hdr, rf(RAIL_W + 16, 0, g_hdr_gear.left - 22, HEADER_H), OC_COL_TEXT);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_TRAILING);
    draw_text(rt, "\xE2\x96\xBE", g_small, rf(RAIL_W + 16, 2, g_hdr_gear.left - 8, HEADER_H), OC_COL_MUTED);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);

    /* "Find a conversation" box — a rounded container with a search glyph; the
     * native EDIT child (g_find) sits inside and live-filters the list. */
    D2D1_RECT_F fb = rf(RAIL_W + 10, HEADER_H + 6, RAIL_W + SIDEBAR_W - 10, HEADER_H + 36);
    fill_round(rt, fb, 8.0f, OC_COL_INPUT);
    stroke_round(rt, fb, 8.0f, OC_COL_BORDER, 1.0f);
    draw_lucide(rt, OC_ICON_SEARCH, rf(fb.left + 8, fb.top + 7, fb.left + 24, fb.top + 23), OC_COL_MUTED);

    /* "Channels" section label. */
    D2D1_RECT_F sec = rf(RAIL_W + 16, HEADER_H + 46, RAIL_W + SIDEBAR_W - 12, HEADER_H + 68);
    draw_text(rt, "CHANNELS", g_small, sec, OC_COL_FAINT);

    float y = HEADER_H + 72;
    g_n_rows = 0;
    for (size_t i = 0; i < m->n_channels; i++) {
        const oc_channel *c = &m->channels[i];
        if (!c->name || !c->name[0]) continue;             /* unnamed = not listed yet */
        if (g_find_filter[0]) {                            /* live filter */
            char low[160]; size_t k = 0;
            for (; c->name[k] && k < sizeof low - 1; k++)
                low[k] = (char)(c->name[k] >= 'A' && c->name[k] <= 'Z' ? c->name[k] + 32 : c->name[k]);
            low[k] = 0;
            if (!strstr(low, g_find_filter)) continue;
        }
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

/* Two messages group (Slack-style) when same author, close in time, and neither
 * is a tombstone — the follow-up drops its avatar + name/time header. */
static int groups_with(const oc_msg *prev, const oc_msg *cur) {
    if (!prev) return 0;
    if (prev->author_id != cur->author_id) return 0;
    if (prev->deleted || cur->deleted) return 0;
    uint64_t dt = cur->server_time > prev->server_time
                ? cur->server_time - prev->server_time : 0;
    return dt <= 5u * 60u * 1000u;                     /* within 5 minutes */
}

/* The literal text of a message body (tombstone / empty handled). */
static const char *body_text(const oc_msg *msg) {
    return msg->deleted ? "(message deleted)"
         : (msg->body && msg->body[0]) ? msg->body : " ";
}

/* A DirectWrite layout for a message body wrapped to `cw`; *wlen (optional) gets
 * the UTF-16 length — the unit HitTest positions are expressed in. */
static IDWriteTextLayout *body_layout(const oc_msg *msg, float cw, UINT32 *wlen) {
    WCHAR w[2048];
    int n = to_w(body_text(msg), w, 2048);
    if (n < 1) n = 1;
    if (wlen) *wlen = (UINT32)n;             /* selection/copy span the body only */

    /* Append a faint inline "(edited)" so it flows after the last word instead of
     * colliding with a header line that grouped messages don't have. */
    UINT32 edit_at = (UINT32)n, edit_len = 0;
    if (msg->edited && !msg->deleted) {
        const WCHAR *suf = L"  (edited)";
        int sl = lstrlenW(suf);
        if (n + sl < 2048) { memcpy(w + n, suf, (size_t)sl * sizeof(WCHAR)); n += sl; edit_len = (UINT32)sl; }
    }
    IDWriteTextLayout *layout = NULL;
    IDWriteFactory_CreateTextLayout(g_dwrite, w, (UINT32)n, g_body, cw, 4000.0f, &layout);
    if (layout && edit_len && g_brush2) {
        DWRITE_TEXT_RANGE tr = { edit_at, edit_len };
        IDWriteTextLayout_SetDrawingEffect(layout, (IUnknown *)g_brush2, tr);
    }
    return layout;
}

/* Vertical layout of a message block. An ungrouped message gets an even top
 * margin (so the avatar/name isn't jammed against the block top) matching the
 * bottom pad; grouped continuations stay tight. */
#define MSG_TOP(g)   ((g) ? 4.0f  : 12.0f)   /* margin above avatar/name */
#define MSG_NAME(g)  ((g) ? 0.0f  : 20.0f)   /* header (name/time) line height */
#define MSG_BOT(g)   ((g) ? 6.0f  : 12.0f)   /* margin below the block */
#define MSG_BODY_DY(g) (MSG_TOP(g) + MSG_NAME(g))   /* block top -> body top */

/* A message's rendered height for a given content width (creates + returns the
 * body layout so the draw pass can reuse it; *wlen gets its UTF-16 length). */
static float msg_height(const oc_msg *msg, float content_w, int grouped,
                        IDWriteTextLayout **out_body, UINT32 *wlen) {
    IDWriteTextLayout *layout = body_layout(msg, content_w, wlen);
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
    return MSG_BODY_DY(grouped) + body_h + (float)extra * LINE_H + MSG_BOT(grouped);
}

static void draw_message(ID2D1RenderTarget *rt, const oc_model *m, const oc_msg *msg,
                         IDWriteTextLayout *body, float x0, float y, float content_w,
                         int grouped) {
    static const uint32_t AVPAL[6] =
        { 0x2563EB, 0x3BA55D, 0xD9A441, 0xB05CCB, 0xE0725A, 0x2FA5A5 };
    float ax = x0, tx = x0 + AVA + 12;

    if (!grouped) {
        float ty = y + MSG_TOP(grouped);        /* content sits below the top margin */
        /* Avatar: colored circle with the author's initial. */
        const char *nm = msg->author_name[0] ? msg->author_name : oc_model_user_name(m, msg->author_id);
        if (!nm || !nm[0]) nm = "user";
        D2D1_ELLIPSE e = { { ax + AVA / 2, ty + AVA / 2 }, AVA / 2, AVA / 2 };
        ID2D1RenderTarget_FillEllipse(rt, &e, paint_with(AVPAL[msg->author_id % 6]));
        char ini[2] = { (char)(nm[0] >= 'a' && nm[0] <= 'z' ? nm[0] - 32 : nm[0]), 0 };
        draw_text(rt, ini, g_ava, rf(ax, ty, ax + AVA, ty + AVA), 0xFFFFFF);

        /* Author + timestamp on the header line. */
        D2D1_RECT_F hl = rf(tx, ty, x0 + content_w + AVA + 12, ty + 20);
        draw_text(rt, nm, g_name, hl, OC_COL_TEXT);
        if (msg->server_time) {
            time_t t = (time_t)(msg->server_time / 1000);
            struct tm tv; char when[16] = "";
            if (oc_localtime_r(&t, &tv)) strftime(when, sizeof when, "%H:%M", &tv);
            draw_text(rt, when, g_time, hl, OC_COL_FAINT);
        }
    }

    /* Body. */
    float by = y + MSG_BODY_DY(grouped);
    if (body) {
        D2D1_POINT_2F org = { tx, by };
        uint32_t bcol = msg->deleted ? OC_COL_FAINT : OC_COL_TEXT;
        ID2D1RenderTarget_DrawTextLayout(rt, org, body, paint_with(bcol),
                                         D2D1_DRAW_TEXT_OPTIONS_NONE);
        DWRITE_TEXT_METRICS tm;
        if (SUCCEEDED(IDWriteTextLayout_GetMetrics(body, &tm))) by += tm.height;
        else by += 18;
    }
    /* "(edited)" is drawn inline by body_layout (faint, after the last word). */

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

#define SEP_H 30.0f     /* a date-divider row */

static int pt_in(D2D1_RECT_F r, int x, int y) {
    return (float)x >= r.left && (float)x < r.right &&
           (float)y >= r.top  && (float)y < r.bottom;
}

/* Two messages fall on the same local calendar day? */
static int same_day(uint64_t a_ms, uint64_t b_ms) {
    time_t ta = (time_t)(a_ms / 1000), tb = (time_t)(b_ms / 1000);
    struct tm va, vb;
    if (!oc_localtime_r(&ta, &va) || !oc_localtime_r(&tb, &vb)) return 1;
    return va.tm_year == vb.tm_year && va.tm_yday == vb.tm_yday;
}

/* "Today" / "Yesterday" / "Friday, July 25" for a date divider. */
static void day_label(uint64_t ms, char *out, size_t cap) {
    time_t t = (time_t)(ms / 1000), now = time(NULL), yst = now - 86400;
    struct tm tv, nv, yv;
    if (!oc_localtime_r(&t, &tv)) { if (cap) out[0] = 0; return; }
    if (oc_localtime_r(&now, &nv) &&
        tv.tm_year == nv.tm_year && tv.tm_yday == nv.tm_yday) {
        snprintf(out, cap, "Today"); return;
    }
    if (oc_localtime_r(&yst, &yv) &&
        tv.tm_year == yv.tm_year && tv.tm_yday == yv.tm_yday) {
        snprintf(out, cap, "Yesterday"); return;
    }
    strftime(out, cap, "%A, %B %d", &tv);
}

/* A centered date label flanked by hairlines, filling a SEP_H band at `y`. */
static void draw_day_sep(ID2D1RenderTarget *rt, uint64_t ms, D2D1_RECT_F reg, float y) {
    char lbl[64]; day_label(ms, lbl, sizeof lbl);
    if (!lbl[0]) return;
    float cy = y + SEP_H / 2, cx = (reg.left + reg.right) / 2;
    WCHAR w[64]; int n = to_w(lbl, w, 64);
    IDWriteTextLayout *l = NULL; float tw = 60;
    IDWriteFactory_CreateTextLayout(g_dwrite, w, (UINT32)n, g_small,
                                    reg.right - reg.left, SEP_H, &l);
    if (l) {
        DWRITE_TEXT_METRICS tm;
        if (SUCCEEDED(IDWriteTextLayout_GetMetrics(l, &tm)))
            tw = tm.widthIncludingTrailingWhitespace;
        IDWriteTextLayout_Release(l);
    }
    float gap = tw / 2 + 14;
    fill(rt, rf(reg.left + 40, cy, cx - gap, cy + 1), OC_COL_BORDER);
    fill(rt, rf(cx + gap, cy, reg.right - 40, cy + 1), OC_COL_BORDER);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(rt, lbl, g_small, rf(reg.left, y + 5, reg.right, y + SEP_H - 3), OC_COL_MUTED);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
}

/* Bottom-pinned, wheel-scrolled render of a message array into `reg`. When
 * `capture` is set, records per-message hit-boxes for the context menu. */
static void draw_msglist(ID2D1RenderTarget *rt, const oc_model *m,
                         const oc_msg *msgs, size_t nmsgs, D2D1_RECT_F reg, int capture) {
    float pad = 20.0f;
    float x0 = reg.left + pad;
    float content_w = (reg.right - pad) - (x0 + AVA + 12);
    if (content_w < 80) content_w = 80;

    size_t n = nmsgs, first = 0;
    enum { CAP = 600 };
    static IDWriteTextLayout *layouts[CAP];
    static float heights[CAP];
    static uint32_t wlens[CAP];
    static uint8_t grouped[CAP];
    static uint8_t sep[CAP];       /* a date divider precedes this message */
    if (n > CAP) { first = n - CAP; n = CAP; }

    float total = 0;
    for (size_t i = 0; i < n; i++) {
        /* Date dividers only in the main transcript, not the thread/search panes. */
        sep[i] = (uint8_t)(capture && (i == 0 ||
                 !same_day(msgs[first + i - 1].server_time, msgs[first + i].server_time)));
        grouped[i] = (uint8_t)(!sep[i] && i > 0 &&
                     groups_with(&msgs[first + i - 1], &msgs[first + i]));
        heights[i] = msg_height(&msgs[first + i], content_w, grouped[i], &layouts[i], &wlens[i]);
        total += heights[i] + (sep[i] ? SEP_H : 0);
    }

    float visible = reg.bottom - reg.top;
    g_scroll_max = total > visible ? total - visible : 0;
    if (g_scroll > g_scroll_max) g_scroll = g_scroll_max;
    if (g_scroll < 0) g_scroll = 0;

    /* Resolve the selection into an ordered index range within this list. */
    long sel_lo = -1, sel_hi = -1; uint32_t sel_lo_pos = 0, sel_hi_pos = 0;
    if (capture && g_has_sel) {
        long ai = -1, fi = -1;
        for (size_t i = 0; i < n; i++) {
            if (msgs[first + i].message_id == g_sel_a_mid) ai = (long)i;
            if (msgs[first + i].message_id == g_sel_f_mid) fi = (long)i;
        }
        if (ai >= 0 && fi >= 0) {
            if (ai < fi || (ai == fi && g_sel_a_pos <= g_sel_f_pos)) {
                sel_lo = ai; sel_lo_pos = g_sel_a_pos; sel_hi = fi; sel_hi_pos = g_sel_f_pos;
            } else {
                sel_lo = fi; sel_lo_pos = g_sel_f_pos; sel_hi = ai; sel_hi_pos = g_sel_a_pos;
            }
        }
    }

    float y = (reg.bottom - total) + g_scroll;     /* g_scroll 0 => newest pinned to bottom */

    ID2D1RenderTarget_PushAxisAlignedClip(rt, &reg, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    if (capture) g_n_msgrows = 0;
    for (size_t i = 0; i < n; i++) {
        if (sep[i]) {
            if (y + SEP_H >= reg.top && y <= reg.bottom)
                draw_day_sep(rt, msgs[first + i].server_time, reg, y);
            y += SEP_H;
        }
        if (y + heights[i] >= reg.top && y <= reg.bottom) {
            float bx = x0 + AVA + 12, by = y + MSG_BODY_DY(grouped[i]);
            /* Hover highlight behind the whole row (main transcript only). */
            if (capture && !g_selecting && g_hover_mid == msgs[first + i].message_id)
                fill(rt, rf(reg.left, y, reg.right, y + heights[i]), OC_COL_HOVER);
            /* Selection highlight — drawn under the text so it stays readable. */
            if (sel_lo >= 0 && (long)i >= sel_lo && (long)i <= sel_hi && layouts[i]) {
                uint32_t s = ((long)i == sel_lo) ? sel_lo_pos : 0;
                uint32_t e = ((long)i == sel_hi) ? sel_hi_pos : wlens[i];
                if (e > s) {
                    DWRITE_HIT_TEST_METRICS hm[64]; UINT32 got = 0;
                    if (SUCCEEDED(IDWriteTextLayout_HitTestTextRange(
                            layouts[i], s, e - s, bx, by, hm, 64, &got)))
                        for (UINT32 r = 0; r < got; r++) {
                            D2D1_RECT_F hr = rf(hm[r].left, hm[r].top,
                                                hm[r].left + hm[r].width, hm[r].top + hm[r].height);
                            ID2D1RenderTarget_FillRectangle(rt, &hr, paint_alpha(OC_COL_ACCENT, 0.32f));
                        }
                }
            }
            draw_message(rt, m, &msgs[first + i], layouts[i], x0, y, content_w, grouped[i]);
            if (capture && g_n_msgrows < (int)(sizeof g_msgrows / sizeof g_msgrows[0])) {
                g_msgrows[g_n_msgrows].top = y;
                g_msgrows[g_n_msgrows].bot = y + heights[i];
                g_msgrows[g_n_msgrows].bx = bx;
                g_msgrows[g_n_msgrows].by = by;
                g_msgrows[g_n_msgrows].cw = content_w;
                g_msgrows[g_n_msgrows].mid = msgs[first + i].message_id;
                g_n_msgrows++;
            }
        }
        y += heights[i];
        if (layouts[i]) IDWriteTextLayout_Release(layouts[i]);
        layouts[i] = NULL;
    }

    /* Custom scrollbar for the main transcript. g_scroll 0 => pinned bottom => thumb
     * at the bottom of the track; g_scroll_max => scrolled to top => thumb at top. */
    if (capture) {
        if (g_scroll_max > 0.5f && total > 0) {
            float track_top = reg.top + 4, track_h = visible - 8;
            float thumb_h = visible / total * track_h;
            if (thumb_h < 30) thumb_h = 30;
            if (thumb_h > track_h) thumb_h = track_h;
            float travel = track_h - thumb_h;
            float thumb_top = track_top + (1.0f - g_scroll / g_scroll_max) * travel;
            float sx = reg.right - 10;
            g_sbar_track_top = track_top; g_sbar_travel = travel;
            g_sbar_thumb = rf(sx, thumb_top, sx + 6, thumb_top + thumb_h);
            fill_round(rt, g_sbar_thumb, 3.0f, OC_COL_FAINT);
        } else {
            g_sbar_thumb = rf(0, 0, 0, 0);
        }
    }
    ID2D1RenderTarget_PopAxisAlignedClip(rt);
}

/* An overlay title bar; returns the region below it for the overlay body. */
static D2D1_RECT_F overlay_header(ID2D1RenderTarget *rt, D2D1_RECT_F reg, const char *title) {
    fill(rt, rf(reg.left, reg.top, reg.right, reg.top + 34), OC_COL_HEADER);
    draw_text(rt, title, g_name, rf(reg.left + 20, reg.top, reg.right - 130, reg.top + 34), OC_COL_TEXT);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_TRAILING);
    draw_text(rt, "Esc to close", g_small, rf(reg.left + 20, reg.top, reg.right - 16, reg.top + 34), OC_COL_MUTED);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
    fill(rt, rf(reg.left, reg.top + 33, reg.right, reg.top + 34), OC_COL_BORDER);
    return rf(reg.left, reg.top + 34, reg.right, reg.bottom);
}

static void overlay_empty(ID2D1RenderTarget *rt, D2D1_RECT_F body, const char *text) {
    IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(rt, text, g_ui, body, OC_COL_MUTED);
    IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_LEADING);
}

static void draw_thread(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    D2D1_RECT_F body = overlay_header(rt, reg, "Thread");
    const oc_channel *pc = oc_model_channel((oc_model *)m, m->thread_channel);
    const oc_msg *parent = find_msg(pc, m->thread_parent);
    float top = body.top + 6;
    if (parent) {
        float pad = 20.0f, x0 = body.left + pad;
        float cw = (body.right - pad) - (x0 + AVA + 12); if (cw < 80) cw = 80;
        IDWriteTextLayout *pl = NULL;
        float ph = msg_height(parent, cw, 0, &pl, NULL);
        if (ph > 120) ph = 120;
        D2D1_RECT_F pband = rf(body.left, top, body.right, top + ph);
        ID2D1RenderTarget_PushAxisAlignedClip(rt, &pband, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
        draw_message(rt, m, parent, pl, x0, top, cw, 0);
        ID2D1RenderTarget_PopAxisAlignedClip(rt);
        if (pl) IDWriteTextLayout_Release(pl);
        top += ph + 4;
    }
    char rc[48];
    snprintf(rc, sizeof rc, "%zu %s", m->n_thread_msgs, m->n_thread_msgs == 1 ? "reply" : "replies");
    fill(rt, rf(body.left, top, body.right, top + 1), OC_COL_BORDER);
    draw_text(rt, rc, g_small, rf(body.left + 20, top + 2, body.right - 16, top + 22), OC_COL_FAINT);
    D2D1_RECT_F replies = rf(body.left, top + 24, body.right, body.bottom);
    if (m->n_thread_msgs == 0) overlay_empty(rt, replies, "No replies yet — reply below.");
    else draw_msglist(rt, m, m->thread_msgs, m->n_thread_msgs, replies, 0);
}

static void draw_search(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    char title[192];
    snprintf(title, sizeof title, "Search: %s", m->search_query);
    D2D1_RECT_F body = overlay_header(rt, reg, title);
    g_n_searchrows = 0;
    if (m->n_search == 0) { overlay_empty(rt, body, "No matches."); return; }

    ID2D1RenderTarget_PushAxisAlignedClip(rt, &body, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float y = body.top + 6, rowh = 54;
    for (size_t i = 0; i < m->n_search && y < body.bottom; i++) {
        const oc_search_result *r = &m->search_results[i];
        const char *au = oc_model_user_name(m, r->author_id);
        const oc_channel *ch = oc_model_channel((oc_model *)m, r->channel_id);
        char head[160];
        char when[16] = "";
        if (r->server_time) { time_t t = (time_t)(r->server_time / 1000); struct tm tv;
            if (oc_localtime_r(&t, &tv)) strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tv); }
        snprintf(head, sizeof head, "%s  ·  %s  ·  %s",
                 (au && au[0]) ? au : "user", (ch && ch->name) ? ch->name : "channel", when);
        draw_text(rt, head, g_small, rf(body.left + 20, y, body.right - 16, y + 20), OC_COL_MUTED);
        draw_text(rt, r->snippet ? r->snippet : "", g_ui,
                  rf(body.left + 20, y + 20, body.right - 16, y + 46), OC_COL_TEXT);
        fill(rt, rf(body.left + 20, y + rowh - 1, body.right - 16, y + rowh), OC_COL_BORDER);
        if (g_n_searchrows < (int)(sizeof g_searchrows / sizeof g_searchrows[0])) {
            g_searchrows[g_n_searchrows].top = y; g_searchrows[g_n_searchrows].bot = y + rowh;
            g_searchrows[g_n_searchrows].cid = r->channel_id; g_n_searchrows++;
        }
        y += rowh;
    }
    ID2D1RenderTarget_PopAxisAlignedClip(rt);
}

static void human_bytes(uint64_t b, char *out, size_t cap) {
    const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)b; int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; i++; }
    snprintf(out, cap, i == 0 ? "%.0f %s" : "%.1f %s", v, u[i]);
}

static void draw_kv(ID2D1RenderTarget *rt, D2D1_RECT_F body, float *y,
                    const char *k, const char *v, uint32_t vcol) {
    draw_text(rt, k, g_ui, rf(body.left + 24, *y, body.left + 240, *y + 26), OC_COL_MUTED);
    draw_text(rt, v, g_ui, rf(body.left + 240, *y, body.right - 20, *y + 26), vcol);
    *y += 28;
}

static void draw_storage(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    D2D1_RECT_F body = overlay_header(rt, reg, "Storage usage");
    if (!m->storage_have) { overlay_empty(rt, body, "Loading…"); return; }
    const oc_storage_view *s = &m->storage;
    char v[64]; float y = body.top + 12;
    human_bytes(s->total_bytes, v, sizeof v);   draw_kv(rt, body, &y, "Total disk", v, OC_COL_TEXT);
    human_bytes(s->avail_bytes, v, sizeof v);
    draw_kv(rt, body, &y, "Available", v, s->under_pressure ? OC_COL_DANGER : OC_COL_TEXT);
    human_bytes(s->attach_bytes, v, sizeof v);  draw_kv(rt, body, &y, "Attachments", v, OC_COL_TEXT);
    snprintf(v, sizeof v, "%llu", (unsigned long long)s->attach_count);
    draw_kv(rt, body, &y, "Attachment count", v, OC_COL_TEXT);
    human_bytes(s->reserve_bytes, v, sizeof v);  draw_kv(rt, body, &y, "DB reserve", v, OC_COL_TEXT);
    snprintf(v, sizeof v, "%s%s", s->evict_enabled ? "on" : "off",
             s->max_age_days ? "" : "");
    draw_kv(rt, body, &y, "Eviction", v, OC_COL_TEXT);
    snprintf(v, sizeof v, "%llu orphan · %llu expired · %llu evicted",
             (unsigned long long)s->rec_orphan, (unsigned long long)s->rec_expired,
             (unsigned long long)s->rec_evicted);
    draw_kv(rt, body, &y, "Reclaimed", v, OC_COL_TEXT);
    if (s->under_pressure)
        draw_text(rt, "⚠ Under storage pressure — uploads may be refused.", g_ui,
                  rf(body.left + 24, y + 6, body.right - 20, y + 34), OC_COL_DANGER);
}

static const char *audit_family(uint8_t f) {
    return f == 1 ? "admin" : f == 2 ? "account" : f == 3 ? "security" : f == 4 ? "moderation" : "";
}

static void draw_audit(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    D2D1_RECT_F body = overlay_header(rt, reg, "Audit log");
    if (m->n_audit == 0) { overlay_empty(rt, body, "No audit entries."); return; }
    ID2D1RenderTarget_PushAxisAlignedClip(rt, &body, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float y = body.top + 6, rowh = 46;
    for (size_t i = 0; i < m->n_audit && y < body.bottom; i++) {
        const oc_audit_view *a = &m->audit[i];
        char when[20] = "";
        if (a->at_ms) { time_t t = (time_t)(a->at_ms / 1000); struct tm tv;
            if (oc_localtime_r(&t, &tv)) strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tv); }
        char head[220];
        snprintf(head, sizeof head, "%s  ·  %s  ·  %s%s%s", when,
                 a->actor_name[0] ? a->actor_name : "system", a->action,
                 a->target[0] ? " → " : "", a->target);
        draw_text(rt, head, g_ui, rf(body.left + 20, y, body.right - 90, y + 24),
                  a->outcome ? OC_COL_TEXT : OC_COL_DANGER);
        char meta[160];
        snprintf(meta, sizeof meta, "%s%s%s", audit_family(a->family),
                 a->detail[0] ? " · " : "", a->detail);
        draw_text(rt, meta, g_small, rf(body.left + 20, y + 24, body.right - 20, y + 44), OC_COL_MUTED);
        fill(rt, rf(body.left + 20, y + rowh - 1, body.right - 20, y + rowh), OC_COL_BORDER);
        y += rowh;
    }
    ID2D1RenderTarget_PopAxisAlignedClip(rt);
}

static void draw_weblist(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    const oc_channel *c = oc_model_channel((oc_model *)m, m->weblist_channel);
    char title[160];
    snprintf(title, sizeof title, "Webhooks — %s", (c && c->name) ? c->name : "channel");
    D2D1_RECT_F body = overlay_header(rt, reg, title);
    g_n_webrows = 0;
    if (m->n_webhooks == 0) {
        overlay_empty(rt, body, "No webhooks. Right-click the channel → Create webhook.");
        return;
    }
    draw_text(rt, "Click a webhook to delete it.", g_small,
              rf(body.left + 20, body.top + 4, body.right - 16, body.top + 24), OC_COL_FAINT);
    float y = body.top + 28;
    for (size_t i = 0; i < m->n_webhooks && y < body.bottom; i++) {
        const oc_webhook_view *wv = &m->webhooks[i];
        char row[160];
        snprintf(row, sizeof row, "%s%s", wv->label, wv->disabled ? "  (disabled)" : "");
        draw_text(rt, row, g_ui, rf(body.left + 20, y, body.right - 16, y + 30),
                  wv->disabled ? OC_COL_MUTED : OC_COL_TEXT);
        if (g_n_webrows < (int)(sizeof g_webrows / sizeof g_webrows[0])) {
            g_webrows[g_n_webrows].top = y; g_webrows[g_n_webrows].bot = y + 30;
            g_webrows[g_n_webrows].wid = wv->webhook_id; g_n_webrows++;
        }
        y += 32;
    }
}

static void draw_reactlist(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    D2D1_RECT_F body = overlay_header(rt, reg, "Who reacted");
    if (m->n_reactors == 0) { overlay_empty(rt, body, "No reactions."); return; }
    float y = body.top + 6;
    for (size_t i = 0; i < m->n_reactors && y < body.bottom; i++) {
        const oc_reactor_row *rr = &m->reactors[i];
        draw_text(rt, rr->emoji, g_ui, rf(body.left + 20, y, body.left + 60, y + 30), OC_COL_TEXT);
        const char *nm = oc_model_user_name(m, rr->user_id);
        draw_text(rt, (nm && nm[0]) ? nm : "user", g_ui,
                  rf(body.left + 64, y, body.right - 16, y + 30), OC_COL_TEXT);
        y += 30;
    }
}

static void draw_transcript(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    if (!m->authed) {
        const char *msg = !m->connected ? (m->last_error[0] ? m->last_error : "connecting…")
                                        : "signing in…";
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, msg, g_ui, reg, OC_COL_MUTED);
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_LEADING);
        return;
    }
    if (m->thread_open)    { draw_thread(rt, m, reg);    return; }
    if (m->search_open)    { draw_search(rt, m, reg);    return; }
    if (m->reactlist_open) { draw_reactlist(rt, m, reg); return; }
    if (m->weblist_open)   { draw_weblist(rt, m, reg);   return; }
    if (m->storage_open)   { draw_storage(rt, m, reg);   return; }
    if (m->audit_open)     { draw_audit(rt, m, reg);     return; }

    const oc_channel *c = g_sel ? oc_model_channel((oc_model *)m, g_sel) : NULL;
    if (!c) {
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, "Select a channel to start reading.", g_ui, reg, OC_COL_MUTED);
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_LEADING);
        return;
    }
    if (c->n_msgs == 0) {
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, "No messages yet — say hello.", g_ui, reg, OC_COL_MUTED);
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_LEADING);
        return;
    }

    /* Seen-by footer (REQ-090): who (besides us) has read the last message. */
    uint64_t seen[8];
    size_t ns = oc_model_seen_by(m, g_sel, c->msgs[c->n_msgs - 1].message_id,
                                 m->user_id, seen, 8);
    if (ns > 0) {
        char line[224];
        int off = snprintf(line, sizeof line, "\xE2\x9C\x93 Seen by ");
        size_t show = ns < 3 ? ns : 3;
        for (size_t i = 0; i < show && off < (int)sizeof line - 32; i++) {
            const char *nm = oc_model_user_name(m, seen[i]);
            off += snprintf(line + off, sizeof line - off, "%s%s",
                            i ? ", " : "", (nm && nm[0]) ? nm : "someone");
        }
        if (ns > show) off += snprintf(line + off, sizeof line - off, " +%zu", ns - show);
        D2D1_RECT_F lr = reg; lr.bottom -= 20;
        draw_msglist(rt, m, c->msgs, c->n_msgs, lr, 1);
        draw_text(rt, line, g_small, rf(reg.left + 72, reg.bottom - 20, reg.right - 20, reg.bottom),
                  OC_COL_FAINT);
        return;
    }
    draw_msglist(rt, m, c->msgs, c->n_msgs, reg, 1);
}

/* ---- header + composer --------------------------------------------------- */

static void draw_header(ID2D1RenderTarget *rt, const oc_model *m, float x0, float w) {
    fill(rt, rf(x0, 0, x0 + w, HEADER_H), OC_COL_HEADER);
    fill(rt, rf(x0, HEADER_H - 1, x0 + w, HEADER_H), OC_COL_BORDER);
    const oc_channel *c = g_sel ? oc_model_channel((oc_model *)m, g_sel) : NULL;
    char title[160] = "OpenChime";
    if (c) channel_label(m, c, title, sizeof title);

    /* Someone typing in this channel? -> a subtitle under the title. */
    char typing[192] = "";
    if (c) {
        uint64_t who[4];
        size_t nt = oc_model_typing(m, g_sel, m->user_id, who, 4);
        if (nt == 1) {
            const char *n1 = oc_model_user_name(m, who[0]);
            snprintf(typing, sizeof typing, "%s is typing…", (n1 && n1[0]) ? n1 : "someone");
        } else if (nt == 2) {
            const char *n1 = oc_model_user_name(m, who[0]), *n2 = oc_model_user_name(m, who[1]);
            snprintf(typing, sizeof typing, "%s and %s are typing…",
                     (n1 && n1[0]) ? n1 : "someone", (n2 && n2[0]) ? n2 : "someone");
        } else if (nt > 2) {
            snprintf(typing, sizeof typing, "several people are typing…");
        }
    }
    if (typing[0]) {
        draw_text(rt, title, g_hdr, rf(x0 + 20, 6, x0 + w - 240, 34), OC_COL_TEXT);
        draw_text(rt, typing, g_small, rf(x0 + 20, 32, x0 + w - 240, HEADER_H - 6), OC_COL_ACCENT);
    } else {
        draw_text(rt, title, g_hdr, rf(x0 + 20, 0, x0 + w - 240, HEADER_H), OC_COL_TEXT);
    }

    /* Members toggle (right). */
    g_members_btn = rf(x0 + w - 16 - 96, 12, x0 + w - 16, HEADER_H - 12);
    if (g_show_members) fill_round(rt, g_members_btn, 6.0f, OC_COL_SELECT);
    IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(rt, "Members", g_ui, g_members_btn, g_show_members ? OC_COL_TEXT : OC_COL_MUTED);
    IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_LEADING);

    const char *status = !m->connected ? "offline" : !m->authed ? "signing in" : "connected";
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_TRAILING);
    draw_text(rt, status, g_small, rf(x0 + 20, 0, g_members_btn.left - 12, HEADER_H),
              m->authed ? OC_COL_ONLINE : OC_COL_MUTED);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
}

static void draw_members(ID2D1RenderTarget *rt, const oc_model *m, float W, float H) {
    float x0 = W - MEMBERS_W;
    fill(rt, rf(x0, 0, W, H), OC_COL_SIDEBAR);
    fill(rt, rf(x0, 0, x0 + 1, H), OC_COL_BORDER);

    draw_text(rt, "MEMBERS", g_small, rf(x0 + 16, 10, W - 12, 34), OC_COL_FAINT);
    float y = 40;
    g_n_memrows = 0;
    for (size_t i = 0; i < m->n_users; i++) {
        const oc_member *u = &m->users[i];
        if (u->disabled) continue;
        if (y > H) break;
        uint8_t pr = oc_model_presence_of(m, u->user_id);
        uint32_t dot = pr == OC_PRESENCE_ONLINE ? OC_COL_ONLINE
                     : pr == OC_PRESENCE_AWAY   ? OC_COL_AWAY : OC_COL_FAINT;
        D2D1_ELLIPSE e = { { x0 + 22, y + ROW_H / 2 }, 4.5f, 4.5f };
        ID2D1RenderTarget_FillEllipse(rt, &e, paint_with(dot));
        draw_text(rt, u->name[0] ? u->name : "user", g_ui,
                  rf(x0 + 34, y, W - 60, y + ROW_H), OC_COL_TEXT);
        const char *rl = role_label(u->role);
        if (rl[0]) {
            IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_TRAILING);
            draw_text(rt, rl, g_small, rf(x0 + 34, y, W - 14, y + ROW_H), OC_COL_FAINT);
            IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        if (g_n_memrows < (int)(sizeof g_memrows / sizeof g_memrows[0])) {
            g_memrows[g_n_memrows].top = y; g_memrows[g_n_memrows].bot = y + ROW_H;
            g_memrows[g_n_memrows].uid = u->user_id; g_n_memrows++;
        }
        y += ROW_H;
    }
}

/* The composer region behind the RichEdit child (which paints its own box),
 * plus an attach (+) button to the left of the input. */
static void draw_composer(ID2D1RenderTarget *rt, float x0, float w, float h) {
    float top = h - COMPOSER_H;
    fill(rt, rf(x0, top, x0 + w, h), OC_COL_BASE);

    /* A bordered, rounded input container the composer lives inside (Slack-style),
     * so the field reads as a real control rather than a naked line of text. */
    float bx0 = x0 + 20, bx1 = x0 + w - 20, by0 = top + 12, by1 = h - 16;
    fill_round(rt, rf(bx0, by0, bx1, by1), 10.0f, OC_COL_INPUT);
    stroke_round(rt, rf(bx0, by0, bx1, by1), 10.0f, OC_COL_BORDER, 1.0f);

    float boxh = by1 - by0, sq = 34.0f, cy = by0 + (boxh - sq) / 2;

    /* Attach (+) on the left. */
    g_attach_btn = rf(bx0 + 6, cy, bx0 + 6 + sq, cy + sq);
    IDWriteTextFormat_SetTextAlignment(g_hdr, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(rt, "+", g_hdr, rf(g_attach_btn.left, g_attach_btn.top - 2,
                                 g_attach_btn.right, g_attach_btn.bottom), OC_COL_MUTED);

    /* Send on the right — accent when there's text to send, muted when empty. */
    int has_text = g_re && GetWindowTextLengthW(g_re) > 0;
    g_send_btn = rf(bx1 - 6 - sq, cy, bx1 - 6, cy + sq);
    fill_round(rt, g_send_btn, 8.0f, has_text ? OC_COL_ACCENT : OC_COL_INPUT);
    if (!has_text) stroke_round(rt, g_send_btn, 8.0f, OC_COL_BORDER, 1.0f);
    draw_text(rt, "\xE2\x86\x91", g_hdr, rf(g_send_btn.left, g_send_btn.top - 2,
                                 g_send_btn.right, g_send_btn.bottom),
              has_text ? 0xFFFFFF : OC_COL_FAINT);
    IDWriteTextFormat_SetTextAlignment(g_hdr, DWRITE_TEXT_ALIGNMENT_LEADING);
}

/* ---- paint --------------------------------------------------------------- */

/* Draw the whole UI into `rt` (window RT for painting, or a DC RT for test
 * shots). Caller wraps this in BeginDraw/EndDraw; brushes must belong to `rt`. */
/* Views that show the channel sidebar + transcript + composer. */
static int view_has_sidebar(void) { return g_view == VIEW_HOME || g_view == VIEW_DMS; }

/* A full-pane placeholder for views whose backing feature isn't built yet. */
static void draw_stub_view(ID2D1RenderTarget *rt, D2D1_RECT_F reg,
                           const char *title, const char *sub) {
    fill(rt, rf(reg.left, reg.top, reg.right, reg.top + HEADER_H), OC_COL_HEADER);
    draw_text(rt, title, g_hdr, rf(reg.left + 20, reg.top, reg.right - 20, reg.top + HEADER_H), OC_COL_TEXT);
    fill(rt, rf(reg.left, reg.top + HEADER_H - 1, reg.right, reg.top + HEADER_H), OC_COL_BORDER);
    float cy = (reg.top + HEADER_H + reg.bottom) / 2;
    IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(rt, sub, g_ui, rf(reg.left, cy - 12, reg.right, cy + 14), OC_COL_MUTED);
    IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_LEADING);
}

static void render_scene(ID2D1RenderTarget *rt, const oc_model *m, float W, float H) {
    /* Start from a known identity transform — draw_lucide sets a scale/translate
     * transform per icon and resets it, but reset defensively here so a leaked
     * transform can never distort the menu/avatar chrome (guards the malformed
     * workspace-avatar glitch). */
    D2D1_MATRIX_3X2_F ident = {{{ 1, 0, 0, 1, 0, 0 }}};
    ID2D1RenderTarget_SetTransform(rt, &ident);
    /* Grayscale AA: on a dark UI, ClearType subpixel fringing tints thin text
     * (visible color speckle on the rail labels); grayscale is cleaner. */
    ID2D1RenderTarget_SetTextAntialiasMode(rt, D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    D2D1_COLOR_F base = col(OC_COL_BASE);
    ID2D1RenderTarget_Clear(rt, &base);
    if (!m) return;
    ensure_selection(m);
    draw_rail(rt, m, H);

    if (view_has_sidebar()) {
        float main_x = RAIL_W + SIDEBAR_W;
        float members = (g_show_members && m->authed) ? MEMBERS_W : 0;
        float main_r = W - members, main_w = main_r - main_x;
        draw_sidebar(rt, m, H);
        draw_header(rt, m, main_x, main_w);
        draw_transcript(rt, m, rf(main_x, HEADER_H, main_r, H - COMPOSER_H));
        draw_composer(rt, main_x, main_w, H);
        if (members > 0) draw_members(rt, m, W, H);
        else g_n_memrows = 0;
    } else {
        D2D1_RECT_F reg = rf(RAIL_W, 0, W, H);
        switch (g_view) {
            case VIEW_ACTIVITY:      draw_stub_view(rt, reg, "Activity", "Activity feed \xE2\x80\x94 coming soon"); break;
            case VIEW_FILES:         draw_stub_view(rt, reg, "Files", "File browser \xE2\x80\x94 coming soon"); break;
            case VIEW_LATER:         draw_stub_view(rt, reg, "Later", "Saved items \xE2\x80\x94 coming soon"); break;
            case VIEW_NOTIFICATIONS: draw_stub_view(rt, reg, "Notifications", "Notifications \xE2\x80\x94 coming soon"); break;
            case VIEW_ADMIN:         draw_stub_view(rt, reg, "Admin", "Storage & audit \xE2\x80\x94 open from the workspace menu"); break;
            default:                 draw_stub_view(rt, reg, "OpenChime", ""); break;
        }
        g_n_memrows = 0;
    }
    draw_more_flyout(rt);   /* floats over the pane when open */
    draw_menu(rt);          /* dropdown menus float on top of everything */
}

static void paint(HWND hwnd) {
    d2d_ensure_rt(hwnd);
    if (!g_rt || !g_brush) return;
    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)g_rt;
    RECT rc; GetClientRect(hwnd, &rc);
    float W = (float)(rc.right - rc.left), H = (float)(rc.bottom - rc.top);
    const oc_model *m = model();

    ID2D1RenderTarget_BeginDraw(rt);
    render_scene(rt, m, W, H);

    HRESULT hr = ID2D1RenderTarget_EndDraw(rt, NULL, NULL);
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        if (g_brush) { ID2D1SolidColorBrush_Release(g_brush); g_brush = NULL; }
        if (g_brush2) { ID2D1SolidColorBrush_Release(g_brush2); g_brush2 = NULL; }
        ID2D1HwndRenderTarget_Release(g_rt);
        g_rt = NULL;
    }
}

/* ---- composer (native RichEdit) ------------------------------------------ */

/* Send the composer's text to the selected channel and clear it. */
static void composer_send(void) {
    if (!g_re || !g_client || !g_sel) return;
    int wlen = GetWindowTextLengthW(g_re);
    if (wlen <= 0) return;
    WCHAR *w = (WCHAR *)malloc((size_t)(wlen + 1) * sizeof(WCHAR));
    if (!w) return;
    GetWindowTextW(g_re, w, wlen + 1);
    int blen = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    char *b = (char *)malloc((size_t)(blen > 0 ? blen : 1));
    if (b) {
        WideCharToMultiByte(CP_UTF8, 0, w, -1, b, blen, NULL, NULL);
        /* Drop a trailing newline the RichEdit may append; skip empty/whitespace. */
        int nonspace = 0;
        for (char *p = b; *p; p++) if (*p != '\r' && *p != '\n' && *p != ' ' && *p != '\t') { nonspace = 1; break; }
        if (nonspace) {
            const oc_model *mm = model();
            if (g_edit_msg)
                oc_client_edit(g_client, g_sel, g_edit_msg, b);
            else if (mm && mm->thread_open)
                oc_client_reply(g_client, mm->thread_channel, mm->thread_parent, b);
            else
                oc_client_send(g_client, g_sel, b);
            g_scroll = 0;
        }
        free(b);
    }
    free(w);
    g_edit_msg = 0;
    SetWindowTextW(g_re, L"");
}

static void composer_begin_edit(const oc_msg *msg) {
    if (!g_re || !msg || msg->deleted) return;
    g_edit_msg = msg->message_id;
    WCHAR w[2048];
    to_w(msg->body ? msg->body : "", w, 2048);
    SetWindowTextW(g_re, w);
    SendMessageW(g_re, EM_SETSEL, (WPARAM)-2, -1);      /* caret to end */
    SetFocus(g_re);
}

static void composer_cancel_edit(void) {
    g_edit_msg = 0;
    if (g_re) SetWindowTextW(g_re, L"");
}

/* Subclass proc: Enter sends, Shift+Enter inserts a newline. */
static LRESULT CALLBACK re_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if ((msg == WM_KEYDOWN || msg == WM_CHAR) && wp == VK_RETURN
        && !(GetKeyState(VK_SHIFT) & 0x8000)) {
        if (msg == WM_KEYDOWN) composer_send();
        return 0;                                   /* eat both so no newline/bell */
    }
    if ((msg == WM_KEYDOWN || msg == WM_CHAR) && wp == VK_ESCAPE) {
        if (any_overlay(model())) {
            if (msg == WM_KEYDOWN) close_overlays();
            return 0;
        }
        if (g_edit_msg) { if (msg == WM_KEYDOWN) composer_cancel_edit(); return 0; }
    }
    return CallWindowProcW(g_re_oldproc, hwnd, msg, wp, lp);
}

/* Position the RichEdit over the composer region for the current window size. */
static void layout_find(HWND hwnd);   /* fwd */

static void layout_composer(HWND hwnd) {
    layout_find(hwnd);
    if (!g_re) return;
    /* The composer only belongs to transcript views; hide it elsewhere. */
    if (!view_has_sidebar()) { ShowWindow(g_re, SW_HIDE); return; }
    ShowWindow(g_re, SW_SHOW);
    RECT rc; GetClientRect(hwnd, &rc);
    const oc_model *m = model();
    float members = (g_show_members && m && m->authed) ? MEMBERS_W : 0;
    float main_x = RAIL_W + SIDEBAR_W;
    /* Inside the composer box, between the attach (+) and send buttons. */
    float bx0 = main_x + 20, bx1 = (rc.right - members) - 20;
    float by0 = rc.bottom - COMPOSER_H + 12, by1 = rc.bottom - 16, boxh = by1 - by0;
    int x = (int)(bx0 + 48), r = (int)(bx1 - 48);
    int reh = 24, top = (int)(by0 + (boxh - reh) / 2);
    MoveWindow(g_re, x, top, r - x, reh, TRUE);
}

/* Position the "Find a conversation" EDIT inside its sidebar box (transcript
 * views only). */
static void layout_find(HWND hwnd) {
    (void)hwnd;   /* the sidebar has a fixed width; geometry is constant */
    if (!g_find) return;
    if (!view_has_sidebar()) { ShowWindow(g_find, SW_HIDE); return; }
    ShowWindow(g_find, SW_SHOW);
    int x = (int)(RAIL_W + 10 + 28), r = (int)(RAIL_W + SIDEBAR_W - 10 - 8);
    int top = (int)(HEADER_H + 6 + 6), hgt = 18;
    MoveWindow(g_find, x, top, r - x, hgt, TRUE);
}

static void find_create(HWND parent) {
    g_find = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, parent,
        (HMENU)(INT_PTR)0xF1, GetModuleHandleW(NULL), NULL);
    if (!g_find) return;
    SendMessageW(g_find, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    SendMessageW(g_find, EM_SETCUEBANNER, TRUE, (LPARAM)L"Find a conversation…");
    layout_find(parent);
}

static void composer_create(HWND parent) {
    g_re = CreateWindowExW(0, MSFTEDIT_CLASS, L"",
        WS_CHILD | WS_VISIBLE | ES_MULTILINE | ES_AUTOVSCROLL,
        0, 0, 10, 10, parent, NULL, GetModuleHandleW(NULL), NULL);
    if (!g_re) return;
    SendMessageW(g_re, EM_SETBKGNDCOLOR, 0, (LPARAM)OCRGB(OC_COL_INPUT));
    CHARFORMAT2W cf; ZeroMemory(&cf, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_COLOR | CFM_FACE | CFM_SIZE;
    cf.crTextColor = OCRGB(OC_COL_TEXT);
    /* Match g_body exactly: DIP -> twips is x15 (72/96 pt-per-DIP, 20 twips/pt). */
    cf.yHeight = (LONG)(BODY_DIP * 15.0f);
    lstrcpynW(cf.szFaceName, L"Segoe UI", LF_FACESIZE);
    SendMessageW(g_re, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
    SendMessageW(g_re, EM_SETEVENTMASK, 0, ENM_CHANGE);
    /* A little inner margin so text isn't jammed against the edge. */
    SendMessageW(g_re, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(12, 12));
    g_re_oldproc = (WNDPROC)SetWindowLongPtrW(g_re, GWLP_WNDPROC, (LONG_PTR)re_proc);
    layout_composer(parent);
    SetFocus(g_re);
}

/* ---- context menus + actions --------------------------------------------- */

static const oc_msg *find_msg(const oc_channel *c, uint64_t mid) {
    if (!c) return NULL;
    for (size_t i = 0; i < c->n_msgs; i++)
        if (c->msgs[i].message_id == mid) return &c->msgs[i];
    return NULL;
}

/* Is `emoji` one of our own reactions on this message? (drives react toggle) */
static int reaction_is_mine(const oc_msg *msg, const char *emoji) {
    for (int i = 0; i < msg->n_reactions; i++)
        if (msg->reactions[i].mine && strcmp(msg->reactions[i].emoji, emoji) == 0) return 1;
    return 0;
}

static void copy_to_clipboard(HWND hwnd, const char *utf8) {
    if (!utf8 || !OpenClipboard(hwnd)) return;
    EmptyClipboard();
    int wl = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (size_t)wl * sizeof(WCHAR));
    if (h) {
        WCHAR *p = (WCHAR *)GlobalLock(h);
        MultiByteToWideChar(CP_UTF8, 0, utf8, -1, p, wl);
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

static void download_attachment(HWND hwnd, const oc_attachment *a) {
    WCHAR file[MAX_PATH]; file[0] = 0;
    MultiByteToWideChar(CP_UTF8, 0, a->filename, -1, file, MAX_PATH);
    OPENFILENAMEW ofn; ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameW(&ofn)) {
        char path[1024];
        WideCharToMultiByte(CP_UTF8, 0, file, -1, path, sizeof path, NULL, NULL);
        oc_client_download(g_client, a->id, path);
    }
}

static WCHAR *wmenu(const char *utf8, WCHAR *buf, int cap) { to_w(utf8, buf, cap); return buf; }

/* Pick a local file and upload it to the selected channel. */
static void upload_file(HWND hwnd) {
    if (!g_client || !g_sel) return;
    WCHAR file[MAX_PATH]; file[0] = 0;
    OPENFILENAMEW ofn; ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hwnd;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        char path[1024];
        WideCharToMultiByte(CP_UTF8, 0, file, -1, path, sizeof path, NULL, NULL);
        oc_client_upload(g_client, g_sel, path);
    }
}

static void show_msg_menu(HWND hwnd, const oc_model *m, uint64_t mid, int sx, int sy) {
    const oc_channel *c = oc_model_channel((oc_model *)m, g_sel);
    const oc_msg *msg = find_msg(c, mid);
    if (!msg) return;
    WCHAR wb[128];
    HMENU menu = CreatePopupMenu();
    HMENU react = CreatePopupMenu();
    for (int i = 0; i < 6; i++)
        AppendMenuW(react, MF_STRING, (UINT_PTR)(1 + i), wmenu(REACT_EMO[i], wb, 128));
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)react, L"React");
    for (int i = 0; i < msg->n_attach; i++) {
        char lbl[200];
        snprintf(lbl, sizeof lbl, "Download %s", msg->attach[i].filename);
        AppendMenuW(menu, MF_STRING | (msg->attach[i].reclaimed ? MF_GRAYED : 0),
                    (UINT_PTR)(30 + i), wmenu(lbl, wb, 128));
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    if (!msg->deleted)
        AppendMenuW(menu, MF_STRING, 100, msg->reply_count ? L"Open thread" : L"Reply in thread");
    if (msg->n_reactions) AppendMenuW(menu, MF_STRING, 102, L"Who reacted");
    AppendMenuW(menu, MF_STRING, 20, L"Copy text");
    int own = (msg->author_id == m->user_id);
    int canmod = own || self_role(m) >= OC_ROLE_ADMIN;
    if (own && !msg->deleted)    AppendMenuW(menu, MF_STRING, 21, L"Edit");
    if (canmod && !msg->deleted) AppendMenuW(menu, MF_STRING, 22, L"Delete");

    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, hwnd, NULL);
    DestroyMenu(menu);
    if (cmd >= 1 && cmd <= 6) {
        const char *e = REACT_EMO[cmd - 1];
        oc_client_react(g_client, g_sel, mid, e, reaction_is_mine(msg, e) ? 0 : 1);
    } else if (cmd == 20) {
        copy_to_clipboard(hwnd, msg->body ? msg->body : "");
    } else if (cmd == 21) {
        composer_begin_edit(msg);
    } else if (cmd == 22) {
        oc_client_delete(g_client, g_sel, mid);
    } else if (cmd == 100) {
        g_scroll = 0; oc_client_open_thread(g_client, g_sel, mid);
    } else if (cmd == 102) {
        g_scroll = 0; oc_client_list_reactions(g_client, g_sel, mid);
    } else if (cmd >= 30 && cmd - 30 < msg->n_attach) {
        download_attachment(hwnd, &msg->attach[cmd - 30]);
    }
}

static void show_member_menu(HWND hwnd, const oc_model *m, uint64_t uid, int sx, int sy) {
    if (uid == m->user_id) return;
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 1, L"Message");
    uint8_t me = self_role(m);
    if (me >= OC_ROLE_ADMIN) {
        HMENU roles = CreatePopupMenu();
        AppendMenuW(roles, MF_STRING, 10, L"Member");
        AppendMenuW(roles, MF_STRING, 11, L"Admin");
        if (me >= OC_ROLE_OWNER) AppendMenuW(roles, MF_STRING, 12, L"Owner");
        AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)roles, L"Set role");
        AppendMenuW(menu, MF_STRING, 13, L"Remove from workspace");
    }
    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, hwnd, NULL);
    DestroyMenu(menu);
    switch (cmd) {
    case 1:  oc_client_open_dm(g_client, uid); break;
    case 10: oc_client_set_role(g_client, uid, OC_ROLE_MEMBER); break;
    case 11: oc_client_set_role(g_client, uid, OC_ROLE_ADMIN); break;
    case 12: oc_client_set_role(g_client, uid, OC_ROLE_OWNER); break;
    case 13: oc_client_remove_user(g_client, uid); break;
    default: break;
    }
}

/* ---- input --------------------------------------------------------------- */

static void show_channel_menu(HWND hwnd, const oc_model *m, uint64_t cid, int sx, int sy);
static void open_ws_menu(HWND hwnd);
static void open_profile_menu(HWND hwnd);
static void open_new_menu(HWND hwnd);
static void open_switcher(HWND hwnd);
static void menu_dispatch(HWND hwnd, int cmd);

static int in_rect(D2D1_RECT_F r, int x, int y) {
    return (float)x >= r.left && (float)x <= r.right && (float)y >= r.top && (float)y <= r.bottom;
}

/* Returns 1 if the click hit a control/row (so the caller won't start a text
 * selection), 0 if it fell through to the transcript. */
static int on_click(HWND hwnd, int x, int y) {
    /* An open dropdown menu takes clicks first: a row runs its command; a click
     * anywhere else dismisses it. */
    if (g_menu) {
        for (int i = 0; i < g_n_mirows; i++)
            if ((float)y >= g_mirows[i].top && (float)y < g_mirows[i].bot &&
                (float)x >= g_menu_x && (float)x < g_menu_x + g_menu_w) {
                int cmd = g_mirows[i].cmd; g_menu = MENU_NONE; g_menu_hover = -1;
                menu_dispatch(hwnd, cmd); return 1;
            }
        g_menu = MENU_NONE; g_menu_hover = -1; return 1;
    }
    /* The "More" overflow flyout takes clicks next. */
    if (g_more_open) {
        for (int i = 0; i < g_n_moreflyrows; i++)
            if ((float)y >= g_moreflyrows[i].top && (float)y < g_moreflyrows[i].bot &&
                (float)x >= RAIL_W && (float)x < RAIL_W + 6 + 196) {
                g_view = g_moreflyrows[i].act; g_more_open = 0; layout_composer(hwnd);
                return 1;
            }
        g_more_open = 0;
        return 1;
    }
    /* Left-nav rail: switch the primary view (VIEW_*), or open a dropdown for the
     * special icons (switcher / new / profile) / the More overflow. */
    if ((float)x < RAIL_W) {
        for (int i = 0; i < g_n_navrows; i++)
            if ((float)y >= g_navrows[i].top && (float)y < g_navrows[i].bot) {
                int act = g_navrows[i].act;
                if (act >= 0)                  { g_view = act; layout_composer(hwnd); }
                else if (act == NAV_MORE)      { g_more_open = !g_more_open; }
                else if (act == NAV_SWITCHER)  { open_switcher(hwnd); }
                else if (act == NAV_NEW)       { open_new_menu(hwnd); }
                else if (act == NAV_PROFILE)   { open_profile_menu(hwnd); }
                return 1;
            }
        return 1;   /* clicks in the rail gutter do nothing, but are swallowed */
    }
    /* Everything below is only meaningful in the transcript views. */
    if (!view_has_sidebar()) return 1;

    /* Header buttons + workspace header. */
    if (in_rect(g_hdr_gear, x, y))    { open_ws_menu(hwnd); return 1; }
    if (in_rect(g_hdr_compose, x, y)) { open_new_menu(hwnd); return 1; }
    if (in_rect(g_ws_hdr_btn, x, y))  { open_ws_menu(hwnd); return 1; }
    /* Members toggle. */
    if (in_rect(g_members_btn, x, y)) {
        g_show_members = !g_show_members;
        layout_composer(hwnd);
        return 1;
    }
    /* Composer attach (+) and send buttons. */
    if (in_rect(g_attach_btn, x, y)) { upload_file(hwnd); return 1; }
    if (in_rect(g_send_btn, x, y))   { composer_send(); return 1; }
    /* Overlay row clicks (main area only). */
    {
        const oc_model *mm = model();
        if (mm && x > RAIL_W + SIDEBAR_W) {
            if (mm->search_open)
                for (int i = 0; i < g_n_searchrows; i++)
                    if ((float)y >= g_searchrows[i].top && (float)y < g_searchrows[i].bot) {
                        select_channel(g_searchrows[i].cid);   /* also closes the overlay */
                        return 1;
                    }
            if (mm->weblist_open)
                for (int i = 0; i < g_n_webrows; i++)
                    if ((float)y >= g_webrows[i].top && (float)y < g_webrows[i].bot) {
                        if (MessageBoxW(hwnd, L"Delete this webhook?", L"Confirm",
                                        MB_YESNO | MB_ICONWARNING) == IDYES)
                            oc_client_delete_webhook(g_client, g_webrows[i].wid);
                        return 1;
                    }
        }
    }
    /* Sidebar channel rows. */
    if (x >= RAIL_W && x <= RAIL_W + SIDEBAR_W) {
        for (int i = 0; i < g_n_rows; i++)
            if ((float)y >= g_rows[i].top && (float)y < g_rows[i].bot) {
                if (g_rows[i].cid != g_sel) select_channel(g_rows[i].cid);
                return 1;
            }
        return 1;
    }
    /* Members-pane rows: click opens a DM. */
    for (int i = 0; i < g_n_memrows; i++)
        if ((float)y >= g_memrows[i].top && (float)y < g_memrows[i].bot) {
            oc_client_open_dm(g_client, g_memrows[i].uid);
            return 1;
        }
    return 0;
}

/* ---- transcript text selection (DirectWrite hit-testing) ----------------- */

static int msgrow_at(int y) {
    for (int i = 0; i < g_n_msgrows; i++)
        if ((float)y >= g_msgrows[i].top && (float)y < g_msgrows[i].bot) return i;
    return -1;
}

static int msgrow_clamp(int y) {
    int r = msgrow_at(y);
    if (r >= 0 || g_n_msgrows == 0) return r;
    return (float)y < g_msgrows[0].top ? 0 : g_n_msgrows - 1;
}

/* Map a client point over row `ri` to a UTF-16 offset in that message's body. */
static uint32_t hit_pos(int ri, int x, int y) {
    const oc_model *m = model();
    if (!m || !g_sel) return 0;
    const oc_channel *c = oc_model_channel((oc_model *)m, g_sel);
    const oc_msg *msg = find_msg(c, g_msgrows[ri].mid);
    if (!msg) return 0;
    IDWriteTextLayout *l = body_layout(msg, g_msgrows[ri].cw, NULL);
    if (!l) return 0;
    float lx = (float)x - g_msgrows[ri].bx, ly = (float)y - g_msgrows[ri].by;
    if (lx < 0) lx = 0;
    if (ly < 0) ly = 0;
    BOOL trailing = FALSE, inside = FALSE; DWRITE_HIT_TEST_METRICS htm;
    IDWriteTextLayout_HitTestPoint(l, lx, ly, &trailing, &inside, &htm);
    uint32_t pos = htm.textPosition + (trailing ? 1u : 0u);
    IDWriteTextLayout_Release(l);
    return pos;
}

static int selection_start(HWND hwnd, int x, int y) {
    int r = msgrow_at(y);
    if (r < 0) { g_has_sel = 0; return 0; }
    uint32_t pos = hit_pos(r, x, y);
    g_sel_a_mid = g_sel_f_mid = g_msgrows[r].mid;
    g_sel_a_pos = g_sel_f_pos = pos;
    g_has_sel = 1; g_selecting = 1; g_hover_mid = 0;
    SetCapture(hwnd); SetFocus(hwnd);      /* take focus so Ctrl+C reaches us */
    return 1;
}

static void selection_update(int x, int y) {
    if (!g_selecting) return;
    int r = msgrow_clamp(y);
    if (r < 0) return;
    g_sel_f_mid = g_msgrows[r].mid;
    g_sel_f_pos = hit_pos(r, x, y);
}

static void selection_end(void) {
    if (!g_selecting) return;
    g_selecting = 0;
    ReleaseCapture();
    if (g_sel_a_mid == g_sel_f_mid && g_sel_a_pos == g_sel_f_pos) g_has_sel = 0;
}

/* Copy the current transcript selection to the clipboard as Unicode text. */
static void copy_selection(HWND hwnd) {
    const oc_model *m = model();
    if (!m || !g_has_sel || !g_sel) return;
    const oc_channel *c = oc_model_channel((oc_model *)m, g_sel);
    if (!c) return;
    long ai = -1, fi = -1;
    for (size_t k = 0; k < c->n_msgs; k++) {
        if (c->msgs[k].message_id == g_sel_a_mid) ai = (long)k;
        if (c->msgs[k].message_id == g_sel_f_mid) fi = (long)k;
    }
    if (ai < 0 || fi < 0) return;
    long lo, hi; uint32_t lop, hip;
    if (ai < fi || (ai == fi && g_sel_a_pos <= g_sel_f_pos)) { lo = ai; lop = g_sel_a_pos; hi = fi; hip = g_sel_f_pos; }
    else                                                     { lo = fi; lop = g_sel_f_pos; hi = ai; hip = g_sel_a_pos; }

    static WCHAR buf[16384]; size_t bl = 0;
    for (long gi = lo; gi <= hi && bl < 16350; gi++) {
        WCHAR w[2048];
        int wn = to_w(body_text(&c->msgs[gi]), w, 2048);
        if (wn < 0) wn = 0;
        uint32_t s = (gi == lo) ? lop : 0;
        uint32_t e = (gi == hi) ? hip : (uint32_t)wn;
        if (e > (uint32_t)wn) e = (uint32_t)wn;
        if (s > e) s = e;
        if (gi > lo && bl + 2 < 16350) { buf[bl++] = L'\r'; buf[bl++] = L'\n'; }
        for (uint32_t k = s; k < e && bl < 16350; k++) buf[bl++] = w[k];
    }
    buf[bl] = 0;
    if (bl == 0 || !OpenClipboard(hwnd)) return;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (bl + 1) * sizeof(WCHAR));
    if (h) {
        WCHAR *p = (WCHAR *)GlobalLock(h);
        memcpy(p, buf, (bl + 1) * sizeof(WCHAR));
        GlobalUnlock(h);
        SetClipboardData(CF_UNICODETEXT, h);
    }
    CloseClipboard();
}

static void on_rclick(HWND hwnd, int x, int y) {
    const oc_model *m = model();
    if (!m) return;
    POINT pt = { x, y };
    ClientToScreen(hwnd, &pt);
    /* Sidebar channel rows -> channel menu. */
    if (x >= RAIL_W && x <= RAIL_W + SIDEBAR_W) {
        for (int i = 0; i < g_n_rows; i++)
            if ((float)y >= g_rows[i].top && (float)y < g_rows[i].bot) {
                show_channel_menu(hwnd, m, g_rows[i].cid, pt.x, pt.y);
                return;
            }
        return;
    }
    /* Members pane first (it overlaps the right edge). */
    for (int i = 0; i < g_n_memrows; i++)
        if ((float)y >= g_memrows[i].top && (float)y < g_memrows[i].bot) {
            show_member_menu(hwnd, m, g_memrows[i].uid, pt.x, pt.y);
            return;
        }
    for (int i = 0; i < g_n_msgrows; i++)
        if ((float)y >= g_msgrows[i].top && (float)y < g_msgrows[i].bot) {
            show_msg_menu(hwnd, m, g_msgrows[i].mid, pt.x, pt.y);
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

/* Most-recently-used workspace from the book — for silent reconnect + prefill. */
struct last_ws { char ws[256], user[128]; uint64_t at; int found; };
static void last_ws_cb(void *ctx, const char *workspace, const char *label,
                       const char *username, uint64_t last_used_ms) {
    (void)label;
    struct last_ws *l = ctx;
    if (l->found && last_used_ms < l->at) return;
    l->found = 1; l->at = last_used_ms;
    snprintf(l->ws,   sizeof l->ws,   "%s", workspace ? workspace : "");
    snprintf(l->user, sizeof l->user, "%s", username  ? username  : "");
}
static int pick_last_workspace(char *ws, size_t wscap, char *user, size_t ucap) {
    const char *sp = store_path();
    oc_store *s = sp ? oc_store_open(sp) : NULL;
    if (!s) return 0;
    struct last_ws l; memset(&l, 0, sizeof l);
    oc_store_workspace_each(s, last_ws_cb, &l);
    oc_store_close(s);
    if (!l.found || !l.ws[0]) return 0;
    snprintf(ws, wscap, "%s", l.ws);
    if (user) snprintf(user, ucap, "%s", l.user);
    return 1;
}

/* A still-valid session token stored for `ws`? Then the net thread can reconnect
 * silently and we skip the login dialog entirely (uniform with the TUI). */
static int have_stored_token(const char *ws) {
    oc_endpoint ep;
    if (oc_resolve(ws, getenv("OPENCHIME_SUFFIX"), &ep) != OC_RESOLVE_OK) return 0;
    const char *sp = store_path();
    oc_store *s = sp ? oc_store_open(sp) : NULL;
    if (!s) return 0;
    char inst[288]; snprintf(inst, sizeof inst, "%s:%d", ep.host, ep.port);
    uint8_t tok[OC_SESSION_TOKEN_LEN];
    int has = oc_store_load_session(s, inst, tok, NULL, (uint64_t)time(NULL) * 1000);
    oc_store_close(s);
    return has;
}

/* Record a workspace in the book so the next launch can reconnect to it. A NULL
 * username preserves the stored one (a silent reconnect carries no credential). */
static void remember_workspace(const char *ws, const char *user) {
    const char *sp = store_path();
    oc_store *s = sp ? oc_store_open(sp) : NULL;
    if (!s) return;
    oc_store_workspace_remember(s, ws, ws, user, (uint64_t)time(NULL) * 1000);
    oc_store_close(s);
}

static void connect_start(const char *ws, const char *cred) {
    oc_endpoint ep;
    if (oc_resolve(ws, getenv("OPENCHIME_SUFFIX"), &ep) != OC_RESOLVE_OK) {
        snprintf(g_host, sizeof g_host, "%s", "?");
        return;
    }
    snprintf(g_host, sizeof g_host, "%s", ep.host);
    g_port = ep.port;
    snprintf(g_cur_ws, sizeof g_cur_ws, "%s", ws);
    snprintf(g_cred, sizeof g_cred, "%s", cred);
    g_client = oc_client_start_secure(g_host, g_port, g_cred, store_path(), NULL);

    /* Remember this workspace (+ the username, parsed off "user:pass") so the next
     * launch reconnects silently via the stored session token. */
    char user[128] = ""; const char *colon = strchr(cred, ':');
    if (colon && colon > cred) {
        size_t n = (size_t)(colon - cred); if (n >= sizeof user) n = sizeof user - 1;
        memcpy(user, cred, n); user[n] = 0;
    }
    remember_workspace(ws, user[0] ? user : NULL);
}

/* ---- login dialog -------------------------------------------------------- */

static HWND g_lg_ws, g_lg_user, g_lg_pass;
static int  g_lg_result;                 /* -1 pending, 0 cancel, 1 connect */
static int  g_lg_done;                    /* ends the modal pump (no WM_QUIT) */
static HBRUSH g_lg_bg, g_lg_input;        /* dark-theme brushes (freed after pump) */

static void lg_set_font(HWND w) {
    SendMessageW(w, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

static HWND lg_edit(HWND parent, HINSTANCE inst, int x, int y, int w, const WCHAR *init, DWORD extra) {
    /* Borderless flat field — the lighter input brush delineates it on the
     * dark backdrop (WS_EX_CLIENTEDGE draws a white sunken border, which jars). */
    HWND e = CreateWindowExW(0, L"EDIT", init,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | extra,
        x, y, w, 26, parent, NULL, inst, NULL);
    lg_set_font(e);
    return e;
}

/* Owner-draw a rounded, themed push button (accent for the default action). */
static void lg_draw_button(LPDRAWITEMSTRUCT d) {
    int pressed = (d->itemState & ODS_SELECTED) != 0;
    int accent  = (d->CtlID == IDOK);
    COLORREF bg = accent ? OCRGB(pressed ? OC_COL_ACCENT_DIM : OC_COL_ACCENT)
                         : OCRGB(pressed ? OC_COL_BORDER     : OC_COL_HOVER);
    COLORREF edge = accent ? bg : OCRGB(OC_COL_BORDER);
    HBRUSH b = CreateSolidBrush(bg);
    HPEN  pen = CreatePen(PS_SOLID, 1, edge);
    HGDIOBJ ob = SelectObject(d->hDC, b), op = SelectObject(d->hDC, pen);
    RoundRect(d->hDC, d->rcItem.left, d->rcItem.top, d->rcItem.right, d->rcItem.bottom, 8, 8);
    SelectObject(d->hDC, GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(d->hDC, TRANSPARENT);
    SetTextColor(d->hDC, accent ? RGB(255, 255, 255) : OCRGB(OC_COL_TEXT));
    WCHAR t[32]; GetWindowTextW(d->hwndItem, t, 32);
    DrawTextW(d->hDC, t, -1, &d->rcItem, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    SelectObject(d->hDC, ob); SelectObject(d->hDC, op);
    DeleteObject(b); DeleteObject(pen);
}

static LRESULT CALLBACK login_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        /* Ends the pump via g_lg_done — never PostQuitMessage here, or the
         * WM_QUIT would also kill the main window loop that runs next. */
        if (LOWORD(wp) == IDOK)     { g_lg_result = 1; g_lg_done = 1; return 0; }
        if (LOWORD(wp) == IDCANCEL) { g_lg_result = 0; g_lg_done = 1; return 0; }
        return 0;
    case WM_DRAWITEM:
        lg_draw_button((LPDRAWITEMSTRUCT)lp);
        return TRUE;
    case WM_CTLCOLORSTATIC:
        SetBkColor((HDC)wp, OCRGB(OC_COL_BASE));
        SetTextColor((HDC)wp, OCRGB(OC_COL_MUTED));
        return (LRESULT)g_lg_bg;
    case WM_CTLCOLOREDIT:
        SetBkColor((HDC)wp, OCRGB(OC_COL_HOVER));
        SetTextColor((HDC)wp, OCRGB(OC_COL_TEXT));
        return (LRESULT)g_lg_input;
    case WM_CLOSE:   g_lg_result = 0; g_lg_done = 1; return 0;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* Modal-ish login window (runs its own pump). Returns 1 with ws/cred filled on
 * Connect, 0 on Cancel. */
static int login_dialog(HINSTANCE inst, char *ws, size_t wscap, char *cred, size_t credcap,
                        const char *pre_ws, const char *pre_user) {
    g_lg_bg    = CreateSolidBrush(OCRGB(OC_COL_BASE));
    g_lg_input = CreateSolidBrush(OCRGB(OC_COL_HOVER));
    WCHAR w_ws[256], w_user[128];
    to_w((pre_ws && pre_ws[0]) ? pre_ws : "127.0.0.1:8443", w_ws, 256);
    to_w((pre_user && pre_user[0]) ? pre_user : "alice", w_user, 128);

    WNDCLASSW wc; memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = login_proc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    wc.hbrBackground = g_lg_bg;
    wc.lpszClassName = L"OpenChimeLogin";
    RegisterClassW(&wc);

    int W = 380, H = 268;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - W) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - H) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"OpenChimeLogin", L"Sign in to OpenChime",
        WS_POPUP | WS_CAPTION | WS_SYSMENU, sx, sy, W, H, NULL, NULL, inst, NULL);
    if (!dlg) return 0;
    apply_dark_titlebar(dlg);

    HWND title = CreateWindowExW(0, L"STATIC", L"OpenChime", WS_CHILD | WS_VISIBLE,
        26, 20, 300, 26, dlg, NULL, inst, NULL);
    SendMessageW(title, WM_SETFONT, (WPARAM)CreateFontW(
        22, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0,
        CLEARTYPE_QUALITY, 0, L"Segoe UI"), TRUE);

    struct { const WCHAR *label; int y; } rows[3] = {
        { L"Workspace", 62 }, { L"Username", 104 }, { L"Password", 146 } };
    for (int i = 0; i < 3; i++) {
        HWND s = CreateWindowExW(0, L"STATIC", rows[i].label, WS_CHILD | WS_VISIBLE,
            26, rows[i].y + 5, 96, 20, dlg, NULL, inst, NULL);
        lg_set_font(s);
    }
    g_lg_ws   = lg_edit(dlg, inst, 128, 62,  226, w_ws, 0);
    g_lg_user = lg_edit(dlg, inst, 128, 104, 226, w_user, 0);
    g_lg_pass = lg_edit(dlg, inst, 128, 146, 226, L"", ES_PASSWORD);

    HWND ok = CreateWindowExW(0, L"BUTTON", L"Connect",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON | BS_OWNERDRAW, 176, 196, 84, 32,
        dlg, (HMENU)IDOK, inst, NULL);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 270, 196, 84, 32,
        dlg, (HMENU)IDCANCEL, inst, NULL);
    lg_set_font(ok); lg_set_font(cancel);

    ShowWindow(dlg, SW_SHOW);
    SetFocus(g_lg_pass);
    g_lg_result = -1;
    g_lg_done = 0;

    /* Flag-terminated modal pump — no WM_QUIT, so the main loop survives. */
    MSG m;
    while (!g_lg_done && GetMessageW(&m, NULL, 0, 0) > 0) {
        if (!IsDialogMessageW(dlg, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }
    }
    int ok_pressed = (g_lg_result == 1);
    if (ok_pressed) {
        WCHAR wws[256], wu[128], wp[128];
        GetWindowTextW(g_lg_ws, wws, 256);
        GetWindowTextW(g_lg_user, wu, 128);
        GetWindowTextW(g_lg_pass, wp, 128);
        char u[192], p[192];
        WideCharToMultiByte(CP_UTF8, 0, wws, -1, ws, (int)wscap, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, wu, -1, u, sizeof u, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, wp, -1, p, sizeof p, NULL, NULL);
        snprintf(cred, credcap, "%s:%s", u, p);
    }
    if (IsWindow(dlg)) DestroyWindow(dlg);
    DeleteObject(g_lg_bg);    g_lg_bg = NULL;
    DeleteObject(g_lg_input); g_lg_input = NULL;
    return ok_pressed && ws[0] != 0;
}

/* ---- generic single-field modal prompt ----------------------------------- */

static HWND g_pr_edit;
static int  g_pr_result, g_pr_done;

static LRESULT CALLBACK prompt_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COMMAND:
        if (LOWORD(wp) == IDOK)     { g_pr_result = 1; g_pr_done = 1; return 0; }
        if (LOWORD(wp) == IDCANCEL) { g_pr_result = 0; g_pr_done = 1; return 0; }
        return 0;
    case WM_CLOSE: g_pr_result = 0; g_pr_done = 1; return 0;
    default: return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

/* Modal one-line input. Returns 1 with `out` filled on OK, 0 on Cancel. */
static int text_prompt(HWND owner, const char *title, const char *label,
                       const char *initial, char *out, size_t cap, int password) {
    HINSTANCE inst = GetModuleHandleW(NULL);
    static int registered;
    if (!registered) {
        WNDCLASSW wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = prompt_proc; wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"OcPrompt";
        RegisterClassW(&wc); registered = 1;
    }
    WCHAR wtitle[128], wlabel[128], winit[512];
    to_w(title, wtitle, 128); to_w(label, wlabel, 128); to_w(initial ? initial : "", winit, 512);

    int W = 380, H = 176;
    RECT orc; GetWindowRect(owner, &orc);
    int sx = orc.left + ((orc.right - orc.left) - W) / 2;
    int sy = orc.top + ((orc.bottom - orc.top) - H) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"OcPrompt", wtitle,
        WS_POPUP | WS_CAPTION | WS_SYSMENU, sx, sy, W, H, owner, NULL, inst, NULL);
    if (!dlg) return 0;

    HWND s = CreateWindowExW(0, L"STATIC", wlabel, WS_CHILD | WS_VISIBLE,
        18, 16, 344, 20, dlg, NULL, inst, NULL);
    lg_set_font(s);
    g_pr_edit = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", winit,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL | (password ? ES_PASSWORD : 0),
        18, 42, 344, 24, dlg, NULL, inst, NULL);
    lg_set_font(g_pr_edit);
    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, 178, 92, 84, 28,
        dlg, (HMENU)IDOK, inst, NULL);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, 272, 92, 84, 28, dlg, (HMENU)IDCANCEL, inst, NULL);
    lg_set_font(ok); lg_set_font(cancel);

    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    SetFocus(g_pr_edit);
    SendMessageW(g_pr_edit, EM_SETSEL, 0, -1);
    g_pr_result = -1; g_pr_done = 0;

    MSG m;
    while (!g_pr_done && GetMessageW(&m, NULL, 0, 0) > 0)
        if (!IsDialogMessageW(dlg, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }

    if (g_pr_result == 1) {
        WCHAR w[512]; GetWindowTextW(g_pr_edit, w, 512);
        WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (IsWindow(dlg)) DestroyWindow(dlg);
    return g_pr_result == 1;
}

/* ---- app menu (workspace avatar) + channel menu -------------------------- */

static int g_logging_out;
static int g_await_invite;      /* show the minted invite token once it arrives */
static int g_await_webhook;     /* show the minted webhook token once it arrives */

static float menu_total_height(void) {
    float t = 12 + (g_menu_headerblock ? 66 : 0);
    for (int i = 0; i < g_n_mi; i++) t += menu_item_h(g_mi[i].kind);
    return t;
}

/* Re-point the single client at another workspace (true N-hosting is a later
 * phase). Empty cred = silent reconnect via the stored session token. */
static void switch_workspace(HWND hwnd, const char *ws, const char *cred) {
    if (g_client) { oc_client_stop(g_client); g_client = NULL; }
    g_sel = 0; g_view = VIEW_HOME; g_scroll = 0; g_post_auth = 0;
    g_n_backfilled = 0; g_has_sel = 0; g_more_open = 0; g_menu = MENU_NONE;
    connect_start(ws, cred ? cred : "");
    layout_composer(hwnd);
}

static void sw_book_cb(void *ctx, const char *workspace, const char *label,
                       const char *username, uint64_t last) {
    (void)ctx; (void)username; (void)last;
    if (g_n_sw >= (int)(sizeof g_sw / sizeof g_sw[0])) return;
    snprintf(g_sw[g_n_sw].ws, sizeof g_sw[g_n_sw].ws, "%s", workspace ? workspace : "");
    snprintf(g_sw[g_n_sw].label, sizeof g_sw[g_n_sw].label, "%s",
             (label && label[0]) ? label : (workspace ? workspace : "?"));
    g_sw[g_n_sw].current = (strcmp(g_sw[g_n_sw].ws, g_cur_ws) == 0);
    g_n_sw++;
}

static void open_ws_menu(HWND hwnd) {
    (void)hwnd;
    const oc_model *m = model();
    int admin = m && self_role(m) >= OC_ROLE_ADMIN;
    g_n_mi = 0;
    if (admin) { mi_item(40, "Invite people as member"); mi_item(41, "Invite people as admin"); mi_sep(); }
    mi_item(70, "Preferences");
    if (admin) { mi_section("TOOLS & SETTINGS"); mi_item(60, "Storage usage"); mi_item(61, "Audit log"); }
    mi_sep();
    mi_item(2, "Reconnect now");
    mi_item_d(3, "Sign out");
    mi_item_d(5, "Sign out everywhere");
    g_menu = MENU_WS; g_menu_headerblock = 1; g_menu_hover = -1;
    g_menu_x = RAIL_W + 8; g_menu_y = HEADER_H - 6; g_menu_w = 268;
}

static void open_profile_menu(HWND hwnd) {
    g_n_mi = 0;
    mi_item(10, "Set status: Online");
    mi_item(11, "Set status: Away");
    mi_item(50, "Do not disturb\xE2\x80\xA6");
    mi_sep();
    mi_item(30, "Change display name\xE2\x80\xA6");
    mi_item(31, "Change password\xE2\x80\xA6");
    g_menu = MENU_PROFILE; g_menu_headerblock = 0; g_menu_hover = -1; g_menu_w = 224;
    RECT rc; GetClientRect(hwnd, &rc);
    float profile_top = rc.bottom - 3 * RAIL_IH - 6 + 2 * RAIL_IH;
    g_menu_x = RAIL_W + 8;
    g_menu_y = profile_top - menu_total_height();
    if (g_menu_y < 8) g_menu_y = 8;
}

static void open_new_menu(HWND hwnd) {
    g_n_mi = 0;
    mi_item(1, "New channel\xE2\x80\xA6");
    mi_item(6, "New direct message\xE2\x80\xA6");
    mi_item(7, "Upload a file\xE2\x80\xA6");
    mi_sep();
    mi_item(4, "Search messages\xE2\x80\xA6");
    g_menu = MENU_NEW; g_menu_headerblock = 0; g_menu_hover = -1; g_menu_w = 224;
    RECT rc; GetClientRect(hwnd, &rc);
    float new_top = rc.bottom - 3 * RAIL_IH - 6;
    g_menu_x = RAIL_W + 8;
    g_menu_y = new_top - menu_total_height();
    if (g_menu_y < 8) g_menu_y = 8;
}

static void open_switcher(HWND hwnd) {
    (void)hwnd;
    g_n_sw = 0;
    const char *sp = store_path();
    oc_store *s = sp ? oc_store_open(sp) : NULL;
    if (s) { oc_store_workspace_each(s, sw_book_cb, NULL); oc_store_close(s); }
    g_n_mi = 0;
    mi_section("WORKSPACES");
    for (int i = 0; i < g_n_sw; i++) {
        char lbl[110];
        snprintf(lbl, sizeof lbl, "%s%s", g_sw[i].label, g_sw[i].current ? "  \xE2\x9C\x93" : "");
        mi_item(100 + i, lbl);
    }
    if (g_n_sw == 0) mi_item(-1, "(no remembered workspaces)");
    mi_sep();
    mi_item(80, "Add a workspace\xE2\x80\xA6");
    g_menu = MENU_SWITCHER; g_menu_headerblock = 0; g_menu_hover = -1;
    g_menu_x = RAIL_W + 8; g_menu_y = 12; g_menu_w = 244;
}

static void menu_dispatch(HWND hwnd, int cmd) {
    const oc_model *m = model();
    switch (cmd) {
    case 1: { char name[80];
        if (text_prompt(hwnd, "New channel", "Channel name:", "", name, sizeof name, 0) && name[0])
            oc_client_create_channel(g_client, name);
        break; }
    case 2:  oc_client_reconnect(g_client); break;
    case 3:  oc_client_logout(g_client, OC_LOGOUT_THIS); g_logging_out = 1; break;
    case 5:  oc_client_logout(g_client, OC_LOGOUT_ALL);  g_logging_out = 1; break;
    case 4: { char q[128];
        if (text_prompt(hwnd, "Search", "Search messages:", "", q, sizeof q, 0) && q[0]) {
            g_view = VIEW_HOME; g_scroll = 0; oc_client_search(g_client, q); } break; }
    case 6: { char u[64];
        if (m && text_prompt(hwnd, "New direct message", "Username:", "", u, sizeof u, 0) && u[0]) {
            uint64_t id = oc_model_user_id(m, u);
            if (id) { g_view = VIEW_HOME; oc_client_open_dm(g_client, id); } } break; }
    case 7:  g_view = VIEW_HOME; upload_file(hwnd); break;
    case 10: oc_client_set_presence(g_client, OC_PRESENCE_ONLINE); break;
    case 11: oc_client_set_presence(g_client, OC_PRESENCE_AWAY); break;
    case 30: { char name[64]; const char *cur = m ? oc_model_user_name(m, m->user_id) : "";
        if (text_prompt(hwnd, "Display name", "New display name:", cur ? cur : "", name, sizeof name, 0) && name[0])
            oc_client_set_display_name(g_client, name);
        break; }
    case 31: { char oldp[128], newp[128];
        if (text_prompt(hwnd, "Change password", "Current password:", "", oldp, sizeof oldp, 1) &&
            text_prompt(hwnd, "Change password", "New password:", "", newp, sizeof newp, 1) && newp[0])
            oc_client_change_password(g_client, oldp, newp);
        break; }
    case 40: oc_client_invite_user(g_client, OC_ROLE_MEMBER); g_await_invite = 1; break;
    case 41: oc_client_invite_user(g_client, OC_ROLE_ADMIN);  g_await_invite = 1; break;
    case 60: g_view = VIEW_HOME; close_overlays(); oc_client_toggle_storage(g_client, 1); oc_client_storage_status(g_client); break;
    case 61: g_view = VIEW_HOME; close_overlays(); oc_client_toggle_audit(g_client, 1); oc_client_audit_query(g_client, 0); break;
    case 50: { char win[32];
        if (text_prompt(hwnd, "Do not disturb", "Window HH:MM-HH:MM (blank = off):", "", win, sizeof win, 0)) {
            int sh, sm, eh, em;
            if (win[0] && sscanf(win, "%d:%d-%d:%d", &sh, &sm, &eh, &em) == 4)
                oc_client_set_dnd(g_client, 1, (uint16_t)(sh * 60 + sm), (uint16_t)(eh * 60 + em));
            else oc_client_set_dnd(g_client, 0, 0, 0); } break; }
    case 70: MessageBoxW(hwnd, L"Preferences \xE2\x80\x94 coming soon.", L"Preferences", MB_OK | MB_ICONINFORMATION); break;
    case 80: { HINSTANCE inst = GetModuleHandleW(NULL); char aws[256], acred[264];
        if (login_dialog(inst, aws, sizeof aws, acred, sizeof acred, NULL, NULL))
            switch_workspace(hwnd, aws, acred);
        break; }
    default:
        if (cmd >= 100 && cmd - 100 < g_n_sw && !g_sw[cmd - 100].current)
            switch_workspace(hwnd, g_sw[cmd - 100].ws, "");
        break;
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

static void show_channel_menu(HWND hwnd, const oc_model *m, uint64_t cid, int sx, int sy) {
    const oc_channel *c = oc_model_channel((oc_model *)m, cid);
    if (!c) return;
    HMENU menu = CreatePopupMenu();
    if (!c->joined && c->is_public) {
        AppendMenuW(menu, MF_STRING, 1, L"Join channel");
    } else {
        AppendMenuW(menu, MF_STRING, 2, L"Mark as read");
        HMENU notify = CreatePopupMenu();
        AppendMenuW(notify, MF_STRING | (c->notify_level == OC_NOTIFY_ALL ? MF_CHECKED : 0),      20, L"All messages");
        AppendMenuW(notify, MF_STRING | (c->notify_level == OC_NOTIFY_MENTIONS ? MF_CHECKED : 0), 21, L"Mentions only");
        AppendMenuW(notify, MF_STRING | (c->notify_level == OC_NOTIFY_NONE ? MF_CHECKED : 0),     22, L"Nothing");
        AppendMenuW(menu, MF_POPUP, (UINT_PTR)notify, L"Notifications");
        if (c->kind != OC_CHANNEL_KIND_DM) {
            AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
            AppendMenuW(menu, MF_STRING, 4, L"Webhooks…");
            AppendMenuW(menu, MF_STRING, 5, L"Create webhook…");
            AppendMenuW(menu, MF_STRING, 3, L"Leave channel");
        }
    }
    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, hwnd, NULL);
    DestroyMenu(menu);
    switch (cmd) {
    case 1:  oc_client_join_channel(g_client, cid); break;
    case 2:  oc_client_mark_read(g_client, cid); break;
    case 3:  oc_client_leave_channel(g_client, cid); break;
    case 4:  close_overlays(); oc_client_webhooks(g_client, cid); break;
    case 5: {
        char label[64];
        if (text_prompt(hwnd, "Create webhook", "Webhook label:", "", label, sizeof label, 0) && label[0]) {
            oc_client_create_webhook(g_client, cid, label);
            g_await_webhook = 1;
        }
        break;
    }
    case 20: oc_client_set_notify_pref(g_client, cid, OC_NOTIFY_ALL); break;
    case 21: oc_client_set_notify_pref(g_client, cid, OC_NOTIFY_MENTIONS); break;
    case 22: oc_client_set_notify_pref(g_client, cid, OC_NOTIFY_NONE); break;
    default: break;
    }
}

/* ---- test/automation hook ------------------------------------------------ */

/* Write a top-down 32bpp BGRA buffer as a BMP (no encoder deps; PIL reads it). */
static int write_bmp(const char *path, int w, int h, const void *bgra) {
    uint32_t rowbytes = (uint32_t)w * 4, imgsize = rowbytes * (uint32_t)h, off = 54;
    uint8_t fh[14] = {0}, ih[40] = {0};
    uint32_t total = off + imgsize; int32_t bw = w, bh = -h; uint16_t planes = 1, bpp = 32;
    fh[0] = 'B'; fh[1] = 'M';
    memcpy(fh + 2, &total, 4); memcpy(fh + 10, &off, 4);
    uint32_t hsz = 40; memcpy(ih, &hsz, 4);
    memcpy(ih + 4, &bw, 4); memcpy(ih + 8, &bh, 4);
    memcpy(ih + 12, &planes, 2); memcpy(ih + 14, &bpp, 2);
    memcpy(ih + 20, &imgsize, 4);
    FILE *f = fopen(path, "wb"); if (!f) return 0;
    fwrite(fh, 1, 14, f); fwrite(ih, 1, 40, f); fwrite(bgra, 1, imgsize, f);
    fclose(f); return 1;
}

/* Render the whole UI to a BMP via a Direct2D DC render target — independent of
 * the display being awake/unlocked (the whole point). The RichEdit composer is a
 * native child and won't appear; everything D2D-drawn does. */
static int test_shot(HWND hwnd, const char *path) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return 0;
    HDC screen = GetDC(NULL), mem = CreateCompatibleDC(screen);
    BITMAPINFO bi; ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w; bi.bmiHeader.biHeight = -h;   /* top-down */
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32; bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    int ok = 0;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        D2D1_RENDER_TARGET_PROPERTIES props; ZeroMemory(&props, sizeof props);
        props.type = D2D1_RENDER_TARGET_TYPE_DEFAULT;
        props.pixelFormat.format = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        props.dpiX = 96; props.dpiY = 96;
        props.usage = D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE;
        ID2D1DCRenderTarget *dcrt = NULL;
        if (SUCCEEDED(ID2D1Factory_CreateDCRenderTarget(g_factory, &props, &dcrt)) && dcrt) {
            RECT bind = { 0, 0, w, h };
            ID2D1DCRenderTarget_BindDC(dcrt, mem, &bind);
            ID2D1RenderTarget *rt = (ID2D1RenderTarget *)dcrt;
            /* Brushes are RT-specific; swap the globals to ones on this RT. */
            ID2D1SolidColorBrush *sb = g_brush, *sb2 = g_brush2;
            D2D1_COLOR_F white = col(0xFFFFFF), faint = col(OC_COL_FAINT);
            g_brush = NULL; g_brush2 = NULL;
            ID2D1RenderTarget_CreateSolidColorBrush(rt, &white, NULL, &g_brush);
            ID2D1RenderTarget_CreateSolidColorBrush(rt, &faint, NULL, &g_brush2);
            ID2D1RenderTarget_BeginDraw(rt);
            render_scene(rt, model(), (float)w, (float)h);
            if (SUCCEEDED(ID2D1RenderTarget_EndDraw(rt, NULL, NULL))) ok = 1;
            if (g_brush) ID2D1SolidColorBrush_Release(g_brush);
            if (g_brush2) ID2D1SolidColorBrush_Release(g_brush2);
            g_brush = sb; g_brush2 = sb2;
            ID2D1DCRenderTarget_Release(dcrt);
            GdiFlush();
            if (ok) ok = write_bmp(path, w, h, bits);
        }
        SelectObject(mem, old);
    }
    if (dib) DeleteObject(dib);
    DeleteDC(mem); ReleaseDC(NULL, screen);
    return ok;
}

static void test_ack(const char *msg) {
    char p[600]; snprintf(p, sizeof p, "%s\\ack", g_test_dir);
    FILE *f = fopen(p, "wb"); if (f) { fputs(msg ? msg : "ok", f); fclose(f); }
}

static void test_dump(const char *path) {
    const oc_model *m = model();
    FILE *f = fopen(path, "wb"); if (!f) return;
    if (!m) { fprintf(f, "no model\n"); fclose(f); return; }
    fprintf(f, "authed=%d connected=%d sel=%llu members=%d\n",
            m->authed, m->connected, (unsigned long long)g_sel, g_show_members);
    fprintf(f, "workspace name=\"%s\" deployment=%s max_users=%u\n",
            oc_model_workspace_name(m), oc_model_deployment_name(m), oc_model_max_users(m));
    fprintf(f, "channels=%zu\n", m->n_channels);
    for (size_t i = 0; i < m->n_channels; i++) {
        const oc_channel *c = &m->channels[i];
        if (!c->name || !c->name[0]) continue;
        fprintf(f, "  ch %llu \"%s\" unread=%d msgs=%zu%s\n",
                (unsigned long long)c->channel_id, c->name, c->unread,
                c->n_msgs, c->channel_id == g_sel ? " *" : "");
    }
    fclose(f);
}

/* Poll <OPENCHIME_TEST_DIR>/cmd for one command per tick; write <dir>/ack. */
static void test_poll(HWND hwnd) {
    if (!g_test_dir[0]) return;
    char cmdpath[600]; snprintf(cmdpath, sizeof cmdpath, "%s\\cmd", g_test_dir);
    FILE *f = fopen(cmdpath, "rb"); if (!f) return;
    char buf[1024]; size_t n = fread(buf, 1, sizeof buf - 1, f); buf[n] = 0; fclose(f);
    remove(cmdpath);
    while (n && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;

    char verb[32] = {0}; size_t i = 0;
    while (buf[i] && buf[i] != ' ' && i < sizeof verb - 1) { verb[i] = buf[i]; i++; }
    verb[i] = 0;
    const char *arg = (buf[i] == ' ') ? buf + i + 1 : "";
    const oc_model *m = model();

    if (!strcmp(verb, "shot")) {
        test_ack(test_shot(hwnd, arg) ? "ok" : "err");
    } else if (!strcmp(verb, "send")) {
        if (g_re) { WCHAR w[1024]; to_w(arg, w, 1024); SetWindowTextW(g_re, w); composer_send(); }
        test_ack("ok");
    } else if (!strcmp(verb, "channel")) {
        if (m) for (size_t k = 0; k < m->n_channels; k++)
            if (m->channels[k].name && !strcmp(m->channels[k].name, arg)) {
                select_channel(m->channels[k].channel_id); break;
            }
        test_ack("ok");
    } else if (!strcmp(verb, "click")) {
        int x = 0, y = 0; sscanf(arg, "%d %d", &x, &y); on_click(hwnd, x, y); test_ack("ok");
    } else if (!strcmp(verb, "rclick")) {
        int x = 0, y = 0; sscanf(arg, "%d %d", &x, &y); on_rclick(hwnd, x, y); test_ack("ok");
    } else if (!strcmp(verb, "members")) {
        g_show_members = !g_show_members; layout_composer(hwnd); test_ack("ok");
    } else if (!strcmp(verb, "scroll")) {
        int d = 0; sscanf(arg, "%d", &d); g_scroll += (float)d;
        if (g_scroll < 0) g_scroll = 0;
        if (g_scroll > g_scroll_max) g_scroll = g_scroll_max;
        test_ack("ok");
    } else if (!strcmp(verb, "size")) {
        int w = 0, h = 0; sscanf(arg, "%d %d", &w, &h);
        if (w > 0 && h > 0) SetWindowPos(hwnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
        test_ack("ok");
    } else if (!strcmp(verb, "find")) {
        snprintf(g_find_filter, sizeof g_find_filter, "%s", arg);
        for (char *p = g_find_filter; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
        test_ack("ok");
    } else if (!strcmp(verb, "dump")) {
        test_dump(arg); test_ack("ok");
    } else {
        test_ack("unknown");
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        composer_create(hwnd);
        find_create(hwnd);
        DragAcceptFiles(hwnd, TRUE);              /* drop files anywhere to upload */
        SetTimer(hwnd, TIMER_TICK, 30, NULL);
        return 0;
    case WM_DROPFILES: {
        HDROP drop = (HDROP)wp;
        UINT nf = DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0);
        for (UINT i = 0; i < nf && g_client && g_sel; i++) {
            WCHAR wf[MAX_PATH];
            if (DragQueryFileW(drop, i, wf, MAX_PATH)) {
                char path[1024];
                WideCharToMultiByte(CP_UTF8, 0, wf, -1, path, sizeof path, NULL, NULL);
                oc_client_upload(g_client, g_sel, path);
            }
        }
        DragFinish(drop);
        return 0;
    }
    case WM_TIMER:
        if (wp == TIMER_TICK && g_client) {
            oc_client_tick(g_client);
            const oc_model *m = oc_client_model(g_client);
            if (m->authed && !g_post_auth) {          /* one-shot: identify the bucket + pull state */
                oc_client_set_client_type(g_client, "gui");   /* our own settings bucket, not tui's */
                oc_client_list_settings(g_client);
                oc_client_list_users(g_client);
                oc_client_list_channels(g_client);
                g_post_auth = 1;
                layout_composer(hwnd);   /* members pane now shows — re-fit the composer */
            }
            if (g_logging_out && !m->connected) {     /* logout frame sent + server dropped us */
                PostMessageW(hwnd, WM_CLOSE, 0, 0);
                g_logging_out = 0;
            }
            if (g_await_invite && m->invite_token[0]) {   /* show the minted token once */
                WCHAR msgw[256]; char line[256];
                snprintf(line, sizeof line,
                         "Invite token (share once — it is not shown again):\n\n%s", m->invite_token);
                to_w(line, msgw, 256);
                g_await_invite = 0;
                MessageBoxW(hwnd, msgw, L"Invite created", MB_OK | MB_ICONINFORMATION);
            }
            if (g_await_webhook && m->webhook_token[0]) {  /* show the minted webhook token once */
                WCHAR msgw[256]; char line[256];
                snprintf(line, sizeof line,
                         "Webhook token (shown once — POST to /webhook/<token>):\n\n%s", m->webhook_token);
                to_w(line, msgw, 256);
                g_await_webhook = 0;
                MessageBoxW(hwnd, msgw, L"Webhook created", MB_OK | MB_ICONINFORMATION);
            }
            InvalidateRect(hwnd, NULL, FALSE);
        }
        if (wp == TIMER_TICK) test_poll(hwnd);   /* automation channel (no-op unless enabled) */
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps; BeginPaint(hwnd, &ps);
        paint(hwnd);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        d2d_resize(hwnd);
        layout_composer(hwnd);
        return 0;
    case WM_COMMAND:
        if (g_re && (HWND)lp == g_re && HIWORD(wp) == EN_CHANGE) {
            DWORD now = GetTickCount();
            if (g_client && g_sel && now - g_last_typing > 2000) {
                oc_client_typing(g_client, g_sel);
                g_last_typing = now;
            }
        }
        if (g_find && (HWND)lp == g_find && HIWORD(wp) == EN_CHANGE) {
            WCHAR w[64]; GetWindowTextW(g_find, w, 64);
            char b[128]; WideCharToMultiByte(CP_UTF8, 0, w, -1, b, sizeof b, NULL, NULL);
            for (char *p = b; *p; p++) if (*p >= 'A' && *p <= 'Z') *p += 32;
            snprintf(g_find_filter, sizeof g_find_filter, "%s", b);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 0;
    case WM_CTLCOLOREDIT:
        if ((HWND)lp == g_find) {
            SetBkColor((HDC)wp, OCRGB(OC_COL_INPUT));
            SetTextColor((HDC)wp, OCRGB(OC_COL_TEXT));
            if (!g_find_brush) g_find_brush = CreateSolidBrush(OCRGB(OC_COL_INPUT));
            return (LRESULT)g_find_brush;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_MOUSEWHEEL:
        g_scroll += (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 48.0f;
        if (g_scroll < 0) g_scroll = 0;
        if (g_scroll > g_scroll_max) g_scroll = g_scroll_max;
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_LBUTTONDOWN: {
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        if (!any_overlay(model()) && pt_in(g_sbar_thumb, mx, my)) {
            g_sbar_drag = 1; g_sbar_grab = (float)my - g_sbar_thumb.top;
            SetCapture(hwnd);
        } else if (!on_click(hwnd, mx, my) && !any_overlay(model())) {
            selection_start(hwnd, mx, my);
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int mx = GET_X_LPARAM(lp), my = GET_Y_LPARAM(lp);
        if (g_sbar_drag) {
            if (g_sbar_travel > 0) {
                float off = (float)my - g_sbar_grab - g_sbar_track_top;
                if (off < 0) off = 0;
                if (off > g_sbar_travel) off = g_sbar_travel;
                g_scroll = (1.0f - off / g_sbar_travel) * g_scroll_max;
            }
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (g_selecting) {
            selection_update(mx, my);
            InvalidateRect(hwnd, NULL, FALSE);
        } else if (g_menu) {
            /* Dropdown-menu hover. */
            int h = -1;
            for (int i = 0; i < g_n_mirows; i++)
                if ((float)my >= g_mirows[i].top && (float)my < g_mirows[i].bot &&
                    (float)mx >= g_menu_x && (float)mx < g_menu_x + g_menu_w) {
                    /* map row index back to item index for hover highlight */
                    h = i; break;
                }
            /* g_menu_hover indexes g_mi[]; approximate by matching the row's cmd. */
            int hi = -1;
            if (h >= 0) for (int i = 0, r = 0; i < g_n_mi; i++)
                if (g_mi[i].kind == MK_ITEM) { if (r == h) { hi = i; break; } r++; }
            if (hi != g_menu_hover) { g_menu_hover = hi; InvalidateRect(hwnd, NULL, FALSE); }
        } else if ((float)mx < RAIL_W) {
            /* Rail hover. */
            int a = -100;
            for (int i = 0; i < g_n_navrows; i++)
                if ((float)my >= g_navrows[i].top && (float)my < g_navrows[i].bot) { a = g_navrows[i].act; break; }
            if (a != g_nav_hover) { g_nav_hover = a; InvalidateRect(hwnd, NULL, FALSE); }
        } else {
            if (g_nav_hover != -100) { g_nav_hover = -100; InvalidateRect(hwnd, NULL, FALSE); }
            int r = (any_overlay(model()) || !view_has_sidebar()) ? -1 : msgrow_at(my);
            uint64_t h = r >= 0 ? g_msgrows[r].mid : 0;
            if (h != g_hover_mid) { g_hover_mid = h; InvalidateRect(hwnd, NULL, FALSE); }
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_sbar_drag) { g_sbar_drag = 0; ReleaseCapture(); InvalidateRect(hwnd, NULL, FALSE); }
        else if (g_selecting) { selection_end(); InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_KEYDOWN:
        if (wp == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) { copy_selection(hwnd); return 0; }
        if (wp == VK_ESCAPE && g_menu) { g_menu = MENU_NONE; g_menu_hover = -1; InvalidateRect(hwnd, NULL, FALSE); return 0; }
        if (wp == VK_ESCAPE && g_more_open) { g_more_open = 0; InvalidateRect(hwnd, NULL, FALSE); return 0; }
        if (wp == VK_ESCAPE && g_has_sel) { g_has_sel = 0; InvalidateRect(hwnd, NULL, FALSE); return 0; }
        return 0;
    case WM_GETMINMAXINFO: {
        /* Item 4: never shrink below fitting the workspace icon + Home + More +
         * New + Alerts + Profile (the overflow floor). */
        MINMAXINFO *mmi = (MINMAXINFO *)lp;
        RECT wr, cr; GetWindowRect(hwnd, &wr); GetClientRect(hwnd, &cr);
        int frameH = (int)((wr.bottom - wr.top) - (cr.bottom - cr.top));
        int frameW = (int)((wr.right - wr.left) - (cr.right - cr.left));
        if (frameH < 0 || frameH > 200) frameH = 40;
        if (frameW < 0 || frameW > 200) frameW = 16;
        int minClientH = (int)(64 + 2 * RAIL_IH + 3 * RAIL_IH + 12);  /* start+Home+More+cluster */
        mmi->ptMinTrackSize.y = minClientH + frameH;
        mmi->ptMinTrackSize.x = 640 + frameW;
        return 0;
    }
    case WM_RBUTTONDOWN:
        on_rclick(hwnd, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
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

    /* Automation hook: OPENCHIME_TEST_DIR enables the file command channel. */
    { const char *td = getenv("OPENCHIME_TEST_DIR");
      if (td && td[0]) snprintf(g_test_dir, sizeof g_test_dir, "%s", td); }

    /* Credential resolution, uniform with the TUI:
     *   1. Dev quick-launch `openchime.exe <workspace> <user:pass>` — connect directly.
     *   2. A remembered workspace with a still-valid session token — reconnect
     *      silently (no dialog); the net thread reuses the stored token.
     *   3. Otherwise the login dialog, pre-filled with the last workspace + user. */
    static char aws[256], acred[264];
    const char *ws = aws, *cred = acred;
    int argc = 0; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 3) {
        WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, aws, sizeof aws, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, acred, sizeof acred, NULL, NULL);
    } else {
        char last_ws[256] = "", last_user[128] = "";
        int have_last = pick_last_workspace(last_ws, sizeof last_ws, last_user, sizeof last_user);
        if (have_last && have_stored_token(last_ws)) {
            snprintf(aws, sizeof aws, "%s", last_ws);   /* silent reconnect: cred stays "" */
        } else if (!login_dialog(inst, aws, sizeof aws, acred, sizeof acred,
                                 have_last ? last_ws : NULL, have_last ? last_user : NULL)) {
            if (argv) LocalFree(argv);
            return 0;                     /* cancelled */
        }
    }
    if (argv) LocalFree(argv);

    LoadLibraryW(L"Msftedit.dll");        /* registers MSFTEDIT_CLASS (RICHEDIT50W) */
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
                    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                    1120, 820, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    apply_dark_titlebar(hwnd);
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
    if (g_rail)  IDWriteTextFormat_Release(g_rail);
    for (int i = 0; i < OC_ICON_COUNT; i++)
        if (g_icon_geo[i]) ID2D1PathGeometry_Release(g_icon_geo[i]);
    if (g_icon_stroke) ID2D1StrokeStyle_Release(g_icon_stroke);
    if (g_brush) ID2D1SolidColorBrush_Release(g_brush);
    if (g_brush2) ID2D1SolidColorBrush_Release(g_brush2);
    if (g_rt)     ID2D1HwndRenderTarget_Release(g_rt);
    if (g_dwrite) IDWriteFactory_Release(g_dwrite);
    if (g_factory) ID2D1Factory_Release(g_factory);
    return 0;
}
