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
#include <shellapi.h>         /* CommandLineToArgvW, Shell_NotifyIconW (WIN-18) */
#include <richedit.h>         /* MSFTEDIT_CLASS composer */
#include <commdlg.h>          /* GetSaveFileNameW (attachment download) */
#include <dwmapi.h>           /* DwmSetWindowAttribute (dark title bar) */
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>       /* WIC: decode an inline image from memory (WIN-17) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wchar.h>
#include <wctype.h>

#include "client.h"
#include "secret_os.h"
#include "model.h"
#include "complete.h"   /* shared @user / #channel / :emoji: completion */
#include "mention.h"    /* the same @mention scanner the daemon resolves with */
#include "resolve.h"
#include "store.h"            /* peek a stored session token (skip the login box) */
#include "oc_port.h"          /* oc_mkdir, oc_localtime_r */
#include "protocol.h"         /* OC_CHANNEL_KIND_DM, OC_PRESENCE_* */
#include "openchime_res.h"    /* IDI_APPICON */
#include "theme.h"
#include "icons.h"            /* baked Lucide vector icons (cross-platform) */

/* mingw ships IID_ID2D1Factory in libuuid but not IID_IDWriteFactory; define it
 * locally so we don't depend on the toolchain's GUID table for DWrite. */
static const GUID OC_IID_IDWriteFactory =
    { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };

#define TIMER_TICK 1

/* Layout metrics (device pixels; per-monitor DPI is a later phase). */
/* ---- DPI ------------------------------------------------------------------
 * Every layout constant and hit-box below is in DIPs (1/96"), not device pixels.
 * Direct2D and DirectWrite already work that way, so telling the render target
 * the window's DPI scales the entire drawn UI for free. Two boundaries do NOT
 * scale themselves and must be converted by hand:
 *
 *   native children  positioned with MoveWindow, which takes device pixels
 *   mouse messages   delivered in device pixels
 *
 * Without any of this the process is DPI-unaware and Windows bitmap-stretches
 * the whole window on a scaled display — the app shipped blurry on any modern
 * laptop. */
static UINT g_dpi = 96;
/* Loaded dynamically: both are Windows 10 1607/1703, newer than the _WIN32_WINNT
 * this builds against, and neither is worth raising the floor for. Missing them
 * simply means the older SetProcessDPIAware path and a fixed 96. */
static void dpi_declare_awareness(void) {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef BOOL (WINAPI *setctx_t)(HANDLE);
    setctx_t setctx = u32 ? (setctx_t)(void *)GetProcAddress(u32, "SetProcessDpiAwarenessContext") : NULL;
    /* -4 == DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2: per-monitor, and the
     * system scales the non-client area (title bar) to match. */
    if (setctx && setctx((HANDLE)-4)) return;
    SetProcessDPIAware();          /* system-DPI aware: right on one monitor */
}

static UINT dpi_for_window(HWND hwnd) {
    HMODULE u32 = GetModuleHandleW(L"user32.dll");
    typedef UINT (WINAPI *getdpi_t)(HWND);
    getdpi_t getdpi = u32 ? (getdpi_t)(void *)GetProcAddress(u32, "GetDpiForWindow") : NULL;
    if (getdpi) { UINT d = getdpi(hwnd); if (d >= 48 && d <= 480) return d; }
    HDC dc = GetDC(hwnd);
    UINT d = dc ? (UINT)GetDeviceCaps(dc, LOGPIXELSX) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return (d >= 48 && d <= 480) ? d : 96;
}
#define PX(v)   ((int)((float)(v) * (float)g_dpi / 96.0f + 0.5f))   /* DIP -> device */
#define DIPF(v) ((float)(v) * 96.0f / (float)g_dpi)                 /* device -> DIP */

#define RAIL_W      70.0f     /* pixel-matched to the Slack rail reference */
#define SIDEBAR_W   250.0f
#define HEADER_H    56.0f
/* The channel tab strip under the header (WIN-37). Slack's shape: the channel
 * name and its controls on one row, and the channel's own sub-views on the next.
 * It also fixes something structural — Pins and Files used to replace the
 * transcript with only "Esc to close" as the way back, so there was no standing
 * sense of where you were. */
#define TABBAR_H    34.0f
/* One row at rest: attach, emoji, the text field, and send on the right. The
 * box grows downward as the message wraps, to COMPOSER_MAX_LINES, with the
 * buttons staying on the bottom line — so a tall composer reads as the same
 * control that grew, not a different arrangement.
 *
 *   margin-top | pad | max(text, button) | pad | margin-bottom
 */
#define COMPOSER_MT     12.0f    /* above the box */
#define COMPOSER_MB     16.0f    /* below it */
#define COMPOSER_PAD    12.0f    /* box inner */
#define COMPOSER_BTN    34.0f    /* the square buttons */
#define COMPOSER_LINE   20.0f    /* one wrapped line of text */
#define COMPOSER_MAX_LINES 4
#define COMPOSER_CHROME (COMPOSER_MT + COMPOSER_PAD * 2 + COMPOSER_MB)
/* Resting height: one line, but never shorter than the buttons need. */
#define COMPOSER_H      (COMPOSER_CHROME + COMPOSER_BTN)
static float g_composer_h = COMPOSER_H;

/* The box's inner height — what the text and the buttons share. */
static float composer_inner_h(void) { return g_composer_h - COMPOSER_CHROME; }
#define ROW_H       32.0f     /* a sidebar channel row */
#define AVA         36.0f     /* transcript avatar diameter */
#define LINE_H      19.0f     /* an extra (reaction/attach/thread) line */
/* The right-hand CONTEXT pane. It holds what is true about *people* in this
 * conversation — the member list, and a person's card when you open one. Your
 * OWN account (preferences, shortcuts, workspaces, notification settings) is a
 * modal instead: those interrupt, they are not context for what you are reading.
 * 300 rather than the original 220 because a profile does not fit in 220 and
 * will fit less as REQ-240/241 add fields. */
#define MEMBERS_W   300.0f

/* Primary views selected by the left-nav rail (Slack-style). HOME/DMS/ADMIN are
 * real today; ACTIVITY/FILES/LATER/NOTIFICATIONS are stubs until their features
 * land. Rail item `act` values <0 are special (switcher / new / profile / more). */
enum { VIEW_HOME = 0, VIEW_DMS, VIEW_ACTIVITY, VIEW_FILES, VIEW_LATER,
       VIEW_ADMIN, VIEW_COUNT,
       /* Not a rail destination: the sign-in screen, which owns the whole window
        * (no rail, no sidebar, no composer) until there is a session. */
       VIEW_SIGNIN = 100 };
enum { NAV_SWITCHER = -2, NAV_NEW = -3, NAV_PROFILE = -4, NAV_MORE = -5 };
#define BODY_DIP    15.0f     /* message + composer text size (shared, so they match) */

/* Per-user avatar tints, shared by the transcript and the sidebar's DM rows so
 * one person is the same colour everywhere. */
/* Avatar tints. Deliberately contains NO green and NO amber: those are the
 * presence colours, and two entries here used to be byte-identical to them
 * (0x3BA55D == online, 0xD9A441 == away). A user whose id hashed to one got an
 * avatar the exact colour of the status dot drawn on its corner, so the dot
 * vanished. Colour means one thing at a time. */
static const uint32_t AVPAL[6] =
    { 0x2563EB, 0x8B5CF6, 0xEC4899, 0xE0725A, 0x0EA5E9, 0x64748B };

static ID2D1Brush *paint_with(uint32_t rgb);   /* fwd */

/* A presence dot with a ring in whatever surface it sits on. The ring is what
 * makes it legible against ANY avatar colour rather than just the ones we
 * happened to pick — a tint close to the status colour would otherwise swallow
 * it again the moment the palette changed. */
/* Connection state as a dot (WIN-64): filled when live, hollow when not.
 *
 * This started as THREE states — live / reconnecting / down — which sounded
 * better than it rendered. Measured against a killed daemon, the dot flickered:
 * the model reports a scheduled retry most of the time but nothing at all during
 * each dial attempt, so a "reconnecting" tint blinked to "down" every few
 * seconds. A 9px dot that blinks reads as broken, and the distinction was never
 * worth much anyway — the client retries forever, so "down" and "reconnecting"
 * are the same situation seen at different instants.
 *
 * The hollow ring is drawn in the ACCENT rather than a dead grey because it is
 * the honest reading: we are always trying. The detail — why, and how long until
 * the next attempt — belongs to the connection banner (WIN-1), which has room
 * for words and a Retry button.
 *
 * A widget rather than an inline blob because the rail's per-workspace avatars
 * want exactly this once N workspaces are held at once (REQ-012-015). */
static void draw_conn_dot(ID2D1RenderTarget *rt, float cx, float cy, float r, int live) {
    D2D1_ELLIPSE e = { { cx, cy }, r, r };
    if (live) ID2D1RenderTarget_FillEllipse(rt, &e, paint_with(OC_COL_ONLINE));
    else      ID2D1RenderTarget_DrawEllipse(rt, &e, paint_with(OC_COL_ACCENT), 1.8f, NULL);
}

static void draw_presence_dot(ID2D1RenderTarget *rt, float cx, float cy, float r,
                              uint8_t presence, uint32_t surface) {
    uint32_t c = presence == OC_PRESENCE_ONLINE ? OC_COL_ONLINE
               : presence == OC_PRESENCE_AWAY   ? OC_COL_AWAY : OC_COL_FAINT;
    D2D1_ELLIPSE ring = { { cx, cy }, r + 1.6f, r + 1.6f };
    ID2D1RenderTarget_FillEllipse(rt, &ring, paint_with(surface));
    D2D1_ELLIPSE dot = { { cx, cy }, r, r };
    ID2D1RenderTarget_FillEllipse(rt, &dot, paint_with(c));
    /* Offline reads as an outline, so "not here" is not just a dim fill. */
    if (presence != OC_PRESENCE_ONLINE && presence != OC_PRESENCE_AWAY) {
        D2D1_ELLIPSE in = { { cx, cy }, r - 1.3f, r - 1.3f };
        ID2D1RenderTarget_FillEllipse(rt, &in, paint_with(surface));
    }
}

/* Typed modal form fields (WIN-21); form_dialog() is defined further down. */
enum { FF_TEXT = 0, FF_PASSWORD, FF_CHECK, FF_CHOICE };

typedef struct {
    int         kind;
    const char *label;
    const char *hint;              /* FF_CHOICE: "a|b|c"; else an optional sub-label */
    /* 256 so a full-length channel topic fits (OC_MAX_TOPIC is 250) — at 192 the
     * dialog silently truncated the very value it was editing. */
    char        value[256];        /* in: initial; out: the result (FF_CHECK/CHOICE: "0".."n") */
} oc_field;
static int form_dialog(HWND owner, const char *title, oc_field *f, int n);

/* The quick reactions offered inline by the message menu (WIN-28). Shortcodes,
 * not literals, because they are stored as a preference and a shortcode is what
 * a user can reasonably be asked to type; oc_emoji_by_name() resolves them
 * through the same catalogue the picker uses, so an unknown name simply drops
 * out instead of writing a broken glyph. */
#define QUICK_DEFAULT "+1,heart,joy,tada,eyes,cry"
static char  g_quick_names[160] = QUICK_DEFAULT;
static const char *REACT_EMO[6];
static int   g_n_quick;

static void quick_rebuild(void) {
    g_n_quick = 0;
    const char *p = g_quick_names;
    while (*p && g_n_quick < 6) {
        char name[40]; size_t n = 0;
        while (*p == ' ' || *p == ',') p++;
        while (*p && *p != ',' && n + 1 < sizeof name) name[n++] = *p++;
        name[n] = '\0';
        while (n && name[n - 1] == ' ') name[--n] = '\0';
        if (!n) continue;
        const char *e = oc_emoji_by_name(name);
        if (e) REACT_EMO[g_n_quick++] = e;
    }
    if (g_n_quick == 0) {          /* never leave the menu with no reactions */
        snprintf(g_quick_names, sizeof g_quick_names, "%s", QUICK_DEFAULT);
        const char *d[6] = { "+1", "heart", "joy", "tada", "eyes", "cry" };
        for (int i = 0; i < 6; i++) REACT_EMO[g_n_quick++] = oc_emoji_by_name(d[i]);
    }
}

/* ---- app state ----------------------------------------------------------- */

static oc_client *g_client;
/* The OS credential store (Windows Credential Manager), opened once at startup.
 * NULL means this machine has none — the core then persists no session token at
 * all rather than writing one to the plaintext SQLite file (secret_os.h). */
static oc_secret *g_secret;
static char       g_cred[264];
static char       g_host[256];
static int        g_port;
static char       g_cur_ws[256];        /* the workspace string we connected with */

static ID2D1Factory          *g_factory;
static IDWriteFactory        *g_dwrite;
static ID2D1HwndRenderTarget *g_rt;
static ID2D1SolidColorBrush  *g_brush;      /* one reusable brush; recolored per draw */
static ID2D1SolidColorBrush  *g_brush2;     /* faint brush for inline "(edited)" effect */
static ID2D1SolidColorBrush  *g_brush3;     /* accent brush for @mention spans */

static IDWriteTextFormat *g_hdr;    /* channel title */
static IDWriteTextFormat *g_name;   /* message author (semibold) */
static IDWriteTextFormat *g_time;   /* timestamp (trailing-aligned) */
static IDWriteTextFormat *g_body;   /* message body (wrapping) */
static IDWriteTextFormat *g_ui;     /* sidebar rows / composer */
static IDWriteTextFormat *g_ui_b;   /* unread sidebar rows (semibold) */
static IDWriteTextFormat *g_small;  /* subtitles / meta lines (no wrap) */
static IDWriteTextFormat *g_small_w;/* the same, wrapping — for paragraphs of explanation */
static IDWriteTextFormat *g_ava;    /* avatar initial (centered) */
static IDWriteTextFormat *g_rail;   /* tiny rail item labels (centered) */
static IDWriteTextFormat *g_emoji;   /* Segoe UI Emoji, picker cells (22px) */
static IDWriteTextFormat *g_emoji_s; /* the same, sized for reaction chips */
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
/* Sidebar rows come from the shared core helper (oc_model_sidebar), so the GUI
 * and the TUI group, filter and sort identically. A row is either a section
 * header or a conversation. */
static struct { float top, bot; uint64_t cid; int header; int sec; } g_rows[512];
static oc_sidebar_opts g_sb;          /* per-section sort/filter/collapse */
static float g_sb_scroll, g_sb_content, g_sb_view;
static int   g_sb_hover_sec = -1;     /* header hovered -> reveal its kebab */
static D2D1_RECT_F g_sb_kebab;
static int   g_sb_menu_sec = -1;
static int   g_sb_settings_pending;   /* waiting for the synced prefs after auth */
static void open_section_menu(HWND hwnd, int sec);
static void sidebar_opts_save(void);
static void sidebar_opts_load(const oc_model *m);
#define SB_SETTING_KEY "sidebar"
#define PREFS_SETTING_KEY "prefs"

/* Preferences (WIN-9, REQ-261). Client-side display choices, synced through the
 * daemon's `gui` settings bucket so they follow the account to another machine —
 * a client writes no files (ARCH-88), so this bucket is the only place they can
 * live. Server-side behaviour (per-channel notification level, DND) stays on its
 * own surfaces; this pane is deliberately only what the client itself decides. */
static int g_prefs_open;
/* Profile pane (WIN-10). Viewing a person was impossible from anywhere: clicking
 * a member opened a DM and that was the whole interaction. Richer fields (avatar
 * image, email, timezone, title) are REQ-240 / WIN-47; this shows what the
 * roster actually knows today rather than inventing placeholders for them. */
static uint64_t g_profile_uid;
static D2D1_RECT_F g_prof_dm_btn;
static int g_pref_time24    = 1;   /* 24-hour timestamps */
static int g_pref_members   = 1;   /* members pane shown by default */
static int g_pref_daysep    = 1;   /* date dividers in the transcript */
static int g_prefs_pending;        /* fold the synced values in once they land */
/* Window placement, remembered across runs. It rides in the same synced bucket
 * as the other preferences because a client writes no files (ARCH-88) — which
 * does mean it is stored per workspace rather than per machine. Acceptable: the
 * alternative is not remembering it at all.
 *
 * Stored in DEVICE pixels, since that is what SetWindowPos takes; a move to a
 * differently-scaled monitor is handled by WM_DPICHANGED afterwards. */
static int g_win_x = -1, g_win_y, g_win_w, g_win_h, g_win_max;
static int g_geom_applied;         /* only restore once per run */
static ULONGLONG g_geom_deadline;  /* show anyway if the bucket never lands */
static ULONGLONG g_geom_dirty_at;  /* geometry changed; save once it settles */

/* Read the window's placement into the globals. Returns 1 if it changed.
 * Separate from saving because WM_EXITSIZEMOVE only fires for INTERACTIVE
 * drags — a programmatic move never sends it, and the geometry would then be
 * whatever it was at the last drag. */
static int geom_capture(HWND hwnd) {
    WINDOWPLACEMENT wp; wp.length = sizeof wp;
    if (!GetWindowPlacement(hwnd, &wp)) return 0;
    if (wp.showCmd == SW_SHOWMINIMIZED) return 0;   /* never persist minimised */
    int mx = (wp.showCmd == SW_SHOWMAXIMIZED);
    RECT *n = &wp.rcNormalPosition;
    int w = n->right - n->left, h = n->bottom - n->top;
    if (mx == g_win_max && n->left == g_win_x && n->top == g_win_y &&
        w == g_win_w && h == g_win_h) return 0;
    g_win_max = mx; g_win_x = n->left; g_win_y = n->top; g_win_w = w; g_win_h = h;
    return 1;
}
static struct { D2D1_RECT_F r; int row, val; } g_pref_hits[16];
static int g_n_pref_hits;
static int g_n_rows;

/* Transcript message hit-boxes (context menu + text selection). bx/by = the body
 * layout's top-left; cw = its wrap width — enough to re-hit-test on mouse events. */
typedef struct { float top, bot, bx, by, cw; uint64_t mid; } oc_msgrow;
static oc_msgrow g_msgrows[600];
static int g_n_msgrows;
/* The same, for the thread pane (WIN-15) — replies were read-only because the
 * pane recorded no hit-boxes at all. Its own scroll offset, so opening a thread
 * does not disturb where the transcript was. */
static oc_msgrow g_thrrows[200];
static int g_n_thrrows;
static float g_thr_scroll, g_thr_scroll_max;

/* WIN-14: the read marker as it stood when this channel was opened. It has to be
 * snapshotted, because entering a channel acks it — read live, the divider would
 * vanish in the same frame it appeared. Cleared when you leave or send. */
/* WIN-27: per-channel drafts, in memory for the life of the process. ARCH-88
 * leaves two options — memory-only, or server-synced through the settings
 * bucket — and this is the cheap half: a half-typed message survives a channel
 * switch, not a restart. Cross-restart drafts need the synced route. */
enum { DRAFT_MAX = 24 };
static struct { uint64_t cid; WCHAR text[1024]; } g_drafts[DRAFT_MAX];
static int g_n_drafts;

/* ---- OS notifications (WIN-18, REQ-138) ------------------------------------
 * Shell_NotifyIconW with NIF_INFO rather than WinRT toasts: the client is pure C
 * (ARCH-82), and WinRT's ToastNotificationManager needs a C++/WinRT projection
 * plus a registered AppUserModelID. On Windows 10+ a balloon is rendered by the
 * same toast system anyway, so the user-visible result is a real OS toast.
 *
 * The decision of WHETHER to notify is the server's model rendered locally
 * (ARCH-72): the channel's notify level and the user's DND window, both already
 * synced into oc_model. This adds no server surface.
 *
 * Nothing is raised while the window is foreground — a notification for a
 * message you are looking at is noise. */
static int to_w(const char *s, WCHAR *out, int cap);   /* fwd */

enum { NOTIFY_OFF = 0, NOTIFY_COUNT = 1, NOTIFY_FULL = 2 };
static int  g_pref_notify = NOTIFY_FULL;
static int  g_tray_live;
/* Per-channel high-water as of the last tick. A message is "new" when a
 * channel's mark advances, which is the same signal the unread count uses —
 * cheaper and less fragile than trying to intercept events on their way in. */
/* Keyed by (workspace slot, channel) — channel ids are per-workspace, so with
 * several connected they collide and one workspace would mask another's mail. */
static struct { int slot; uint64_t cid, seen; } g_notify_hw[128];
static int  g_n_notify_hw;
static int  g_notify_primed;   /* the first tick after auth only records */
static NOTIFYICONDATAW g_tray;
#define TRAY_UID 1

/* Is `t` (minutes since midnight) inside the DND window? Windows may wrap past
 * midnight, which is why this is not a plain range test. */
static int in_dnd_window(uint16_t t, uint16_t start, uint16_t end) {
    if (start == end) return 0;
    return (start < end) ? (t >= start && t < end) : (t >= start || t < end);
}

static int dnd_active(const oc_model *m) {
    if (!m || !m->dnd_enabled) return 0;
    time_t now = time(NULL); struct tm tv;
    if (!oc_localtime_r(&now, &tv)) return 0;
    return in_dnd_window((uint16_t)(tv.tm_hour * 60 + tv.tm_min), m->dnd_start_min, m->dnd_end_min);
}

static void tray_init(HWND hwnd) {
    memset(&g_tray, 0, sizeof g_tray);
    g_tray.cbSize = sizeof g_tray;
    g_tray.hWnd = hwnd;
    g_tray.uID = TRAY_UID;
    g_tray.uFlags = NIF_ICON | NIF_TIP;
    g_tray.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APPICON));
    if (!g_tray.hIcon) g_tray.hIcon = LoadIconW(NULL, IDI_APPLICATION);
    lstrcpynW(g_tray.szTip, L"OpenChime", 128);
    g_tray_live = Shell_NotifyIconW(NIM_ADD, &g_tray) ? 1 : 0;
}

static void tray_done(void) {
    if (g_tray_live) { Shell_NotifyIconW(NIM_DELETE, &g_tray); g_tray_live = 0; }
}

static int g_toasts_raised;      /* observable by the harness; see test_dump */

static void notify_toast(const char *title, const char *body) {
    if (!g_tray_live) return;
    g_toasts_raised++;
    g_tray.uFlags = NIF_INFO;
    g_tray.dwInfoFlags = NIIF_NONE;
    to_w(title, g_tray.szInfoTitle, 64);
    to_w(body,  g_tray.szInfo, 256);
    Shell_NotifyIconW(NIM_MODIFY, &g_tray);
}

/* ---- inline image thumbnails (WIN-17) -------------------------------------
 * Attachments rendered as text lines only. The bytes are fetched INTO MEMORY
 * (oc_client_fetch_attachment) and decoded with WIC — no temp file, because a
 * client stores nothing locally (ARCH-88) and a scratch file for rendering is
 * exactly the cache that decision removed.
 *
 * The decoded bitmaps belong to the render target, so the cache is dropped
 * whenever the RT is recreated. */
#define THUMB_H 160.0f
#define THUMB_W 260.0f
enum { THUMB_CACHE = 32 };
/* Dimensions are captured at decode time from WIC rather than read back with
 * ID2D1Bitmap_GetSize(). That method returns a struct by value and mingw's C
 * binding disagrees with the callee about how: the size is written through a
 * bogus hidden return pointer, which landed inside THIS array and clobbered an
 * id. Every lookup then missed, and every miss triggered another fetch — the
 * image rendered once and then flickered back to "loading" for good.
 * IWICBitmapSource_GetSize takes explicit out-params and has no such hazard. */
static struct { uint64_t id; ID2D1Bitmap *bmp; UINT w, h; } g_thumbs[THUMB_CACHE];
static int      g_n_thumbs;
/* A D2D bitmap belongs to the render target that created it; drawing it into
 * another one fails the whole frame, and the test harness renders the same scene
 * into a DC target. An explicit flag rather than comparing render-target
 * pointers: the comparison silently missed on the real window target too, which
 * turned every cache lookup into a miss and every miss into another fetch — a
 * decode loop that filled the cache with copies of the same image. */
static int g_thumbs_off;
static uint64_t g_thumb_missing[THUMB_CACHE];   /* asked for, nothing came back */
static int      g_n_thumb_missing;
static uint64_t g_thumb_pending;                /* one fetch in flight */
/* Click-to-expand: the thumbnails on screen, and the one being viewed full
 * size. The bitmap is already decoded at native resolution — the transcript
 * merely draws it small — so expanding costs nothing but a bigger destination
 * rect. */
static struct { D2D1_RECT_F r; uint64_t id; } g_thumb_hits[32];
static int      g_n_thumb_hits;
static uint64_t g_lightbox;
/* Slack's pattern: the image itself is the click target for a bigger view, and
 * saving it is a button that appears on hover — so the affordance is there when
 * you look for it and out of the way when you are reading. */
static uint64_t g_thumb_hover;
static struct { D2D1_RECT_F r; int attach_ix; uint64_t mid; } g_thumb_dl[32];
static int      g_n_thumb_dl;
static ULONGLONG g_thumb_deadline;
static IWICImagingFactory *g_wic;

/* Only what WIC will actually decode, and only from the server's declared mime —
 * guessing from the filename would mean fetching whatever someone chose to call
 * a .png. */
static int mime_is_image(const char *mime) {
    return mime && (strcmp(mime, "image/png") == 0 || strcmp(mime, "image/jpeg") == 0 ||
                    strcmp(mime, "image/gif") == 0 || strcmp(mime, "image/bmp") == 0 ||
                    strcmp(mime, "image/webp") == 0);
}

static ID2D1Bitmap *thumb_get(ID2D1RenderTarget *rt, uint64_t id, UINT *w, UINT *h) {
    (void)rt;
    if (g_thumbs_off) return NULL;
    for (int i = 0; i < g_n_thumbs; i++)
        if (g_thumbs[i].id == id) {
            if (w) *w = g_thumbs[i].w;
            if (h) *h = g_thumbs[i].h;
            return g_thumbs[i].bmp;
        }
    return NULL;
}
static int thumb_failed(uint64_t id) {
    for (int i = 0; i < g_n_thumb_missing; i++) if (g_thumb_missing[i] == id) return 1;
    return 0;
}
static void thumbs_drop(void) {
    for (int i = 0; i < g_n_thumbs; i++)
        if (g_thumbs[i].bmp) ID2D1Bitmap_Release(g_thumbs[i].bmp);
    g_n_thumbs = 0;
}

/* WIN-16: paging older history. One request in flight at a time, and a
 * remembered "we reached the top" so we stop asking a channel that has no more. */
static uint64_t g_hist_pending_chan, g_hist_before;
static ULONGLONG g_hist_deadline;
static uint64_t g_hist_exhausted[32];
static int g_n_hist_exhausted;

static uint64_t g_unread_from;
static uint64_t g_unread_chan;
static D2D1_RECT_F g_unread_jump;      /* "N new" affordance in the header */
static int g_unread_count;

/* Transcript text selection (DirectWrite hit-testing over the custom surface).
 * Anchor/focus are (message id, UTF-16 offset); order is resolved at draw time. */
static uint64_t g_sel_a_mid, g_sel_f_mid;
static uint32_t g_sel_a_pos, g_sel_f_pos;
static int      g_has_sel;      /* a (possibly empty) selection exists */
static int      g_selecting;    /* left button held, dragging a selection */

/* Members-pane row hit-boxes. The full rect, not just top/bot: testing y alone
 * made every click at that height — right across the transcript — open a
 * profile, which is how the profile pane kept appearing unbidden. */
static struct { D2D1_RECT_F r; uint64_t uid; } g_memrows[256];
static int g_n_memrows;

/* Search-overlay result hit-boxes (row -> its channel AND message). */
static struct { float top, bot; uint64_t cid, mid; } g_searchrows[128];
static int g_n_searchrows;

/* WIN-3: jump-to-message. A search hit names a message, not just a channel, so
 * clicking one arms a jump: the transcript scrolls that message into view and
 * flashes it. The jump survives a few frames because the channel's history is
 * usually still in flight when the click lands — `g_jump_deadline` is what turns
 * "not loaded yet" into an honest failure instead of a silent no-op. */
static uint64_t  g_jump_mid;            /* message to scroll to, 0 = none */
static ULONGLONG g_jump_deadline;       /* GetTickCount64 by which it must appear */
static uint64_t  g_flash_mid;           /* message to tint */
static ULONGLONG g_flash_until;
/* Webhook-overlay row hit-boxes (row -> webhook id, for delete). */
static struct { float top, bot; uint64_t wid; } g_webrows[64];
static int g_n_webrows;

/* Audit family filter (WIN-19): 0 = all, else the OC audit family id. */
static int g_audit_family;
static D2D1_RECT_F g_audit_filters[5];
static int g_n_audit_filters;
static uint64_t g_audit_oldest;     /* the oldest entry paged in, for load-older */

/* Command palette (WIN-11). Every action on the rail and in the menus is
 * mouse-only today; this is the same catalogue reached by keyboard, plus
 * channel and DM quick-switch so Ctrl+K also answers "take me to X". */
static int   g_pal_open, g_pal_sel;
static HWND  g_pal_edit;
static D2D1_RECT_F g_pal_panel, g_pal_box;
/* A palette hit is either a menu command or a channel to select, never both. */
static struct { D2D1_RECT_F r; int cmd; uint64_t cid; } g_pal_rows[12];
static int   g_n_pal_rows;

/* Workspaces manager. The switcher is for going somewhere; this is for managing
 * what is on the device — including the only way to REMOVE a workspace, which
 * REQ-012 requires and the GUI had no surface for at all (the TUI had one). */
static int g_wsmgr_open;
static struct { D2D1_RECT_F r; int row, act; } g_wsmgr_hits[48];
static int g_n_wsmgr_hits;
enum { WSM_GO = 0, WSM_SIGNOUT, WSM_FORGET };

/* Notification-prefs review (WIN-12) + the shortcut sheet (WIN-25). */
static int g_notify_open, g_keys_open;
static struct { D2D1_RECT_F r; uint64_t cid; uint8_t level; } g_notify_hits[128];
static int g_n_notify_hits;

static int      g_show_members = 1;     /* members pane visible */
static D2D1_RECT_F g_members_btn;       /* header toggle hit-box */
enum { TAB_MESSAGES = 0, TAB_FILES, TAB_PINS, TAB_ABOUT, TAB_COUNT };
static int g_tab;                       /* the selected channel tab (WIN-37) */
static D2D1_RECT_F g_tab_r[TAB_COUNT];  /* tab hit-boxes */
static D2D1_RECT_F g_memchip;           /* header member-count chip */
static D2D1_RECT_F g_ws_dot;            /* workspace connection dot (WIN-64) */
static int g_tab_hover = -1;
/* Reaction-chip hit-boxes, rebuilt every frame like the thumbnail ones. */
static struct { D2D1_RECT_F r; uint64_t mid; char emoji[40]; uint8_t mine; } g_chips[128];
static int g_n_chips;
static D2D1_RECT_F g_about_topic, g_about_rename, g_about_archive, g_about_hooks;
static struct { D2D1_RECT_F row, dl; int ix; } g_filerows[64];
static int g_n_filerows;
static uint64_t g_hover_filerow;
/* Rows of the open pins overlay: click jumps to the message, the trailing
 * button unpins it. */
static struct { D2D1_RECT_F row, unpin; uint64_t mid; } g_pinrows[64];
static int g_n_pinrows;
static uint64_t g_hover_pinrow;
static D2D1_RECT_F g_ws_hdr_btn;        /* channel-column workspace header (opens ws menu) */
static D2D1_RECT_F g_hdr_gear, g_hdr_compose;   /* header settings + compose buttons */
static HWND     g_find;                 /* "Find a conversation" filter box (native EDIT) */
static HWND     g_srch;                 /* search-overlay query box (WIN-4, native EDIT) */

/* Composer autocomplete (WIN-7). The candidate list is rebuilt from the text up
 * to the caret on every change; the popover renders above the composer and the
 * RichEdit subclass steals the navigation keys while it is open. */
/* Emoji picker (WIN-8). One panel serves two callers: with g_pick_mid == 0 it
 * inserts into the composer, otherwise it reacts to that message — the same
 * catalogue either way, so the reaction set is no longer six hardcoded glyphs. */
static int      g_pick_open;
static uint64_t g_pick_mid;
static HWND     g_pick_edit;        /* native search box */
static float    g_pick_scroll;
static D2D1_RECT_F g_pick_panel, g_pick_box;
static struct { D2D1_RECT_F r; const char *emoji; } g_pick_cells[256];
static int      g_n_pick_cells;

enum { AC_MAX = 8 };
static oc_completion g_ac[AC_MAX];
static int   g_n_ac, g_ac_sel, g_ac_kind;
static D2D1_RECT_F g_ac_rows[AC_MAX];   /* hit-boxes, captured at paint */
static D2D1_RECT_F g_ac_panel;
static float    g_srch_scroll;          /* search-results scroll offset, px */
static float    g_srch_max;             /* its maximum, computed at paint */
static D2D1_RECT_F g_srch_box;          /* the query field's chrome, for layout_search */
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
enum { MENU_NONE = 0, MENU_WS, MENU_PROFILE, MENU_NEW, MENU_SWITCHER, MENU_SECTION };
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
enum { WS_MAX = 8 };
typedef struct {
    oc_client *client;
    char     ws[256];
    char     cred[320];
    char     host[256];
    int      port;
    /* Per-workspace view state, swapped with the active globals. */
    uint64_t sel;
    float    scroll;
    int      post_auth;
    uint64_t backfilled[64];
    int      n_backfilled;
} oc_ws_slot;
static oc_ws_slot g_wss[WS_MAX];
static int g_n_wss, g_ws_active = -1;
static int ws_find(const char *ws);   /* fwd */
static int g_logging_out;            /* a LOGOUT is in flight */
static int g_forget_after_logout;    /* remove the entry once that sign-out lands */
static void switch_workspace(HWND hwnd, const char *ws, const char *cred);   /* fwd */
static void signin_begin_known(HWND hwnd, const char *ws, const char *user); /* fwd */
static void sw_book_load(void);      /* fwd */
static void ws_forget(const char *ws);   /* fwd */

static struct { char ws[256], label[80], user[80]; int current; } g_sw[16];
static int   g_n_sw;

static D2D1_RECT_F g_attach_btn;        /* composer attach (+) hit-box */
static D2D1_RECT_F g_send_btn;          /* composer send-button hit-box */
static D2D1_RECT_F g_emoji_btn;         /* composer emoji-picker hit-box (WIN-8) */
static D2D1_RECT_F g_at_btn;            /* composer mention button */
static uint64_t g_edit_msg;             /* non-zero => composer is editing this message */

/* ---- sign-in view (WIN-2, REQ-263/020) -------------------------------------
 * Slack signs in in two steps — workspace address first, credentials once the
 * workspace is known — and does it *in the app window*. We match that shape,
 * not Slack's credential mechanism: their email/magic-code/SSO path has no
 * OpenChime equivalent (no mail path; local auth is username+password, ARCH-59;
 * no SAML by design, REQ-027). The step-1 field is exactly OpenChime's DNS
 * workspace resolution (REQ-010/011), which is why the split fits so naturally.
 *
 * This replaces a pre-window GDI popup that could not show an error, could not
 * retry, and could not be returned to after sign-out. The behaviour mirrors the
 * TUI's proven run_login() loop (client/tui/main.c) so the two clients agree on
 * wording and on what happens after a failure — but the TUI can block in
 * await_auth() and a Win32 UI thread cannot, so the wait is a state machine
 * driven from the WM_TIMER tick instead. */
#define SI_W        380.0f        /* card width */
#define SI_TIMEOUT  20000         /* ms before we stop waiting for auth */
static int   g_si_step = 1;       /* 1 = workspace, 2 = credentials */
/* Render the card over the live shell rather than instead of it — true whenever
 * signing in while another workspace is still connected. */
static int   g_si_overlay;
/* The client being signed in, kept SEPARATE from the active one. Signing in to
 * an additional workspace must not disturb the one you are already in: it stays
 * connected and on screen behind the card for the whole attempt, and is only
 * parked once the new workspace actually authenticates. On failure or cancel
 * nothing about it has changed. */
static oc_client *g_si_client;
/* An invite token entered on step 2 (WIN-32). Non-empty turns the next attempt
 * into a redeem: the account is created and signed in together. */
static char  g_si_invite[128];
static D2D1_RECT_F g_si_invite_link;
static D2D1_RECT_F g_si_cancel;      /* overlay sign-in: back to the live workspace */
static char  g_si_ws[256];        /* the workspace string as typed */
static char  g_si_host[256];      /* resolved host (step 1 output) */
static int   g_si_port;
static char  g_si_err[192];       /* inline error under the active field */
static int   g_si_remember = 1;   /* gates whether the session token is persisted */
/* Step 1 has two modes. The default is the hosted case — type just `acme` and
 * the field shows a fixed `.openchime.io` suffix, exactly as Slack shows
 * `.slack.com` — because nearly every workspace is a managed one. "Advanced
 * options" reveals the self-hosted case: a full domain, host:port, or an IP.
 * Both end up in the same oc_resolve() call; only the chrome differs, since a
 * dotted name or an explicit :port passes through suffixing untouched. */
static int   g_si_advanced;
static D2D1_RECT_F g_si_adv_link;
static int   g_si_connecting;     /* awaiting auth: fields hidden, spinner text */
static ULONGLONG g_si_started;    /* GetTickCount64 when the attempt began */
static HWND  g_si_e_ws, g_si_e_user, g_si_e_pass;   /* native EDIT children */
static D2D1_RECT_F g_si_btn, g_si_remember_box, g_si_back;   /* hit-boxes */
/* Defined with the rest of the flow, below the core wiring they depend on. */
static void signin_submit(HWND hwnd);
static void signin_cancel(HWND hwnd);
static void signin_back(HWND hwnd);
static void signin_set_advanced(HWND hwnd, int on);
static void signin_begin(HWND hwnd, const char *ws, const char *user);
static void signin_poll(HWND hwnd);
static void layout_signin(HWND hwnd);
typedef struct { float x0, y0, w, h, fx, fw, fields_y; } si_geom;
static si_geom si_layout(float W, float H);

/* ---- failure surface: toasts + connection banner (WIN-1, REQ-263) ----------
 * Until now the only way a failure reached the user was `last_error` drawn into
 * an *empty* transcript — so a failed send, a rate-limit, or a storage refusal
 * on a working connection was silent. Two surfaces fix that:
 *
 *   toast   — transient, bottom-right of the main pane, auto-expiring, click to
 *             dismiss. For things that just happened.
 *   banner  — persistent strip under the header while the connection is down,
 *             carrying the reason and a Retry now button. For a state.
 *
 * The core has no "an error occurred" callback, so we detect errors by watching
 * the model's sticky `last_error` change between ticks. Known limit: the same
 * text arriving twice in a row is one toast (we refresh its timer instead of
 * stacking a duplicate), because there is nothing in the model to tell the two
 * apart. A monotonic error counter in oc_model would remove that; it is a core
 * change, deliberately out of this item's scope. */
#define TOAST_MAX      4
#define TOAST_MS       6000       /* long enough to read a sentence, short enough not to nag */
#define TOAST_W        340.0f
#define TOAST_H        52.0f
#define TOAST_GAP      10.0f
#define BANNER_H       34.0f
static struct {
    char      text[192];
    ULONGLONG born;               /* GetTickCount64 at push */
    int       danger;             /* 1 = failure (red), 0 = neutral notice */
} g_toast[TOAST_MAX];
static int  g_n_toast;
static D2D1_RECT_F g_toast_box[TOAST_MAX];   /* hit-boxes, captured during paint */
static uint32_t g_err_seq;
static char g_err_seen[160];                 /* last `last_error` we turned into a toast */
static D2D1_RECT_F g_retry_btn;              /* banner Retry-now hit-box */
static int  g_banner_on;                     /* banner drawn this frame (arms the hit-box) */

/* Drop toast `i`, sliding the rest down. */
static void toast_drop(int i) {
    for (int k = i; k < g_n_toast - 1; k++) g_toast[k] = g_toast[k + 1];
    if (g_n_toast > 0) g_n_toast--;
}

/* Show a toast. A repeat of the text currently showing refreshes its timer
 * rather than stacking, so a burst of identical failures reads as one event. */
static void toast_push(const char *text, int danger) {
    if (!text || !text[0]) return;
    for (int i = 0; i < g_n_toast; i++) {
        if (strcmp(g_toast[i].text, text) == 0) { g_toast[i].born = GetTickCount64(); return; }
    }
    if (g_n_toast == TOAST_MAX) toast_drop(0);      /* oldest falls off the top */
    snprintf(g_toast[g_n_toast].text, sizeof g_toast[g_n_toast].text, "%s", text);
    g_toast[g_n_toast].born   = GetTickCount64();
    g_toast[g_n_toast].danger = danger;
    g_n_toast++;
}

/* Expire elapsed toasts, and turn a *new* model error into one. Called each tick
 * before painting (the tick repaints unconditionally, so nothing is returned). */
static void toast_tick(const oc_model *m) {
    ULONGLONG now = GetTickCount64();
    for (int i = g_n_toast - 1; i >= 0; i--)
        if (now - g_toast[i].born >= TOAST_MS) toast_drop(i);
    /* Keyed on the sequence, not the text: repeating a failing action must
     * notify again, or the second attempt reads as having worked. */
    if (m && m->error_seq != g_err_seq) {
        g_err_seq = m->error_seq;
        snprintf(g_err_seen, sizeof g_err_seen, "%s", m->last_error);
        /* Divide the two surfaces by what the failure *is*, so the same sentence
         * never appears twice: a connection problem is a persistent state and
         * belongs to the banner (which is on screen exactly when !authed), while
         * anything that goes wrong mid-session — a refused send, a rate limit, a
         * storage refusal — has no persistent home and needs the toast. */
        if (g_err_seen[0] && m->authed) toast_push(g_err_seen, 1);
    }
}

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

/* Width of `s` in `fmt`, for placing something immediately after it. */
static float text_width(const char *s, IDWriteTextFormat *fmt) {
    WCHAR w[256];
    int n = to_w(s, w, 256);
    if (n <= 0 || !g_dwrite) return 0;
    IDWriteTextLayout *tl = NULL;
    if (FAILED(IDWriteFactory_CreateTextLayout(g_dwrite, w, (UINT32)n, fmt,
                                               4000.0f, 100.0f, &tl)) || !tl)
        return 0;
    DWRITE_TEXT_METRICS tm;
    float out = 0;
    if (SUCCEEDED(IDWriteTextLayout_GetMetrics(tl, &tm))) out = tm.widthIncludingTrailingWhitespace;
    IDWriteTextLayout_Release(tl);
    return out;
}

/* Colour emoji need both the emoji font and ENABLE_COLOR_FONT; without the flag
 * D2D renders the COLR layers as a single flat glyph. */
static void draw_emoji_fmt(ID2D1RenderTarget *rt, const char *s, D2D1_RECT_F r,
                           IDWriteTextFormat *fmt) {
    WCHAR w[16];
    int n = to_w(s, w, 16);
    if (n <= 0 || !fmt) return;
    ID2D1RenderTarget_DrawText(rt, w, (UINT32)n, fmt, &r, paint_with(OC_COL_TEXT),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP | D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT,
                               DWRITE_MEASURING_MODE_NATURAL);
}
static void draw_emoji_glyph(ID2D1RenderTarget *rt, const char *s, D2D1_RECT_F r) {
    draw_emoji_fmt(rt, s, r, g_emoji);
}

static void draw_text(ID2D1RenderTarget *rt, const char *s, IDWriteTextFormat *fmt,
                      D2D1_RECT_F r, uint32_t rgb) {
    WCHAR w[1024];
    int n = to_w(s, w, 1024);
    if (n <= 0) return;
    ID2D1RenderTarget_DrawText(rt, w, (UINT32)n, fmt, &r, paint_with(rgb),
                               D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

/* Like draw_text, but tints every occurrence of a whitespace-separated term from
 * `terms` (WIN-3). Matching is case-insensitive and substring-based, which is
 * what the daemon's LIKE-based search does — highlighting on a stricter rule
 * than the search itself would leave hits visibly unmarked. */
static void draw_text_hl(ID2D1RenderTarget *rt, const char *s, IDWriteTextFormat *fmt,
                         D2D1_RECT_F r, uint32_t rgb, const char *terms) {
    WCHAR w[1024];
    int n = to_w(s, w, 1024);
    if (n <= 0) return;
    IDWriteTextLayout *tl = NULL;
    if (!g_dwrite || !terms || !terms[0] ||
        FAILED(IDWriteFactory_CreateTextLayout(g_dwrite, w, (UINT32)n, fmt,
                                               r.right - r.left, r.bottom - r.top, &tl)) || !tl) {
        draw_text(rt, s, fmt, r, rgb);
        return;
    }
    /* Fold once, then scan for each term over the folded copy so the offsets line
     * up with the layout's own (unfolded) character indices. */
    WCHAR low[1024];
    for (int i = 0; i < n; i++) low[i] = (WCHAR)towlower(w[i]);
    low[n] = 0;

    const char *p = terms;
    while (*p) {
        while (*p == ' ') p++;
        char term[64]; size_t tn = 0;
        while (*p && *p != ' ' && tn + 1 < sizeof term) term[tn++] = *p++;
        term[tn] = '\0';
        if (tn == 0) continue;
        WCHAR tw[64];
        int twn = to_w(term, tw, 64);
        if (twn <= 0) continue;
        for (int i = 0; i < twn; i++) tw[i] = (WCHAR)towlower(tw[i]);
        for (int i = 0; i + twn <= n; i++) {
            if (wcsncmp(low + i, tw, (size_t)twn) != 0) continue;
            DWRITE_HIT_TEST_METRICS hm[16]; UINT32 got = 0;
            if (SUCCEEDED(IDWriteTextLayout_HitTestTextRange(tl, (UINT32)i, (UINT32)twn,
                                                             r.left, r.top, hm, 16, &got)))
                for (UINT32 k = 0; k < got; k++) {
                    D2D1_RECT_F hr = rf(hm[k].left - 1, hm[k].top,
                                        hm[k].left + hm[k].width + 1, hm[k].top + hm[k].height);
                    fill_round_a(rt, hr, 2.0f, OC_COL_NOTICE, 0.34f);
                }
            i += twn - 1;
        }
    }
    D2D1_POINT_2F org = { r.left, r.top };
    ID2D1RenderTarget_DrawTextLayout(rt, org, tl, paint_with(rgb),
                                     D2D1_DRAW_TEXT_OPTIONS_CLIP |
                                     D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    IDWriteTextLayout_Release(tl);
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
    g_small_w = mk_fmt(UI, 12.5f, DWRITE_FONT_WEIGHT_NORMAL,  DWRITE_TEXT_ALIGNMENT_LEADING,  DWRITE_PARAGRAPH_ALIGNMENT_NEAR, 1);
    g_ava   = mk_fmt(UI, 15.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER,   DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0);
    g_rail  = mk_fmt(UI, 9.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0);
    /* Named explicitly, because falling back from "Segoe UI" reaches the
     * monochrome Segoe UI Symbol glyphs first — the picker rendered as outlines. */
    g_emoji = mk_fmt(L"Segoe UI Emoji", 22.0f, DWRITE_FONT_WEIGHT_NORMAL,
                     DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0);
    g_emoji_s = mk_fmt(L"Segoe UI Emoji", 13.0f, DWRITE_FONT_WEIGHT_NORMAL,
                       DWRITE_TEXT_ALIGNMENT_CENTER, DWRITE_PARAGRAPH_ALIGNMENT_CENTER, 0);
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
    /* The whole scene is authored in DIPs; this is what makes it scale. */
    rtp.dpiX = rtp.dpiY = (float)g_dpi;
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
        D2D1_COLOR_F acc = col(OC_COL_ACCENT);
        ID2D1RenderTarget_CreateSolidColorBrush((ID2D1RenderTarget *)g_rt, &acc, NULL, &g_brush3);
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
    g_prefs_open = 0;
    g_profile_uid = 0;
    g_notify_open = 0;
    g_keys_open = 0;
    g_wsmgr_open = 0;
    if (!mm) return;
    if (mm->thread_open)    oc_client_close_thread(g_client);
    if (mm->search_open)    oc_client_close_search(g_client);
    if (mm->reactlist_open) oc_client_close_reactions(g_client);
    if (mm->pinlist_open)   oc_client_close_pins(g_client);
    if (mm->weblist_open)   oc_client_close_webhooks(g_client);
    if (mm->storage_open)   oc_client_toggle_storage(g_client, 0);
    if (mm->audit_open)     oc_client_toggle_audit(g_client, 0);
}

/* Something is covering the MIDDLE column. Modals are not here: they cover the
 * whole window and are handled ahead of everything (see draw_modal). Nor is the
 * context pane (profile, reactors) — it sits BESIDE the transcript rather than
 * over it, so the transcript stays live and hoverable while it is open. */
static int any_overlay(const oc_model *m) {
    return
           (m && (m->thread_open || m->search_open ||
                  m->pinlist_open || m->weblist_open || m->storage_open ||
                  m->audit_open));
}

static void ac_close(void);   /* fwd */

/* Stash whatever is in the composer under `cid`; an empty composer clears it. */
static void draft_save(uint64_t cid) {
    if (!g_re || !cid || g_edit_msg) return;      /* an edit-in-progress is not a draft */
    WCHAR w[1024];
    int n = GetWindowTextW(g_re, w, 1024);
    int slot = -1;
    for (int i = 0; i < g_n_drafts; i++) if (g_drafts[i].cid == cid) { slot = i; break; }
    if (n <= 0) {                                  /* drop it, keeping the array dense */
        if (slot >= 0) g_drafts[slot] = g_drafts[--g_n_drafts];
        return;
    }
    if (slot < 0) {
        if (g_n_drafts == DRAFT_MAX) g_drafts[0] = g_drafts[--g_n_drafts];   /* oldest out */
        slot = g_n_drafts++;
        g_drafts[slot].cid = cid;
    }
    lstrcpynW(g_drafts[slot].text, w, 1024);
}

static void draft_restore(uint64_t cid) {
    if (!g_re) return;
    for (int i = 0; i < g_n_drafts; i++)
        if (g_drafts[i].cid == cid) {
            SetWindowTextW(g_re, g_drafts[i].text);
            SendMessageW(g_re, EM_SETSEL, (WPARAM)-2, -1);   /* caret to end */
            return;
        }
    SetWindowTextW(g_re, L"");
}

static void select_channel(uint64_t cid) {
    if (!g_client || !cid) return;
    if (g_sel && g_sel != cid) draft_save(g_sel);
    close_overlays();
    g_has_sel = 0;                       /* drop any transcript text selection */

    /* Snapshot where the read marker stood BEFORE the mark-read below, so the
     * "New" divider survives entering the channel (WIN-14). */
    const oc_model *sm = model();
    const oc_channel *sc = sm ? oc_model_channel((oc_model *)sm, cid) : NULL;
    g_unread_from  = (sc && sc->high_water > sc->read_marker) ? sc->read_marker : 0;
    g_unread_chan  = cid;
    g_unread_count = sc ? sc->unread : 0;

    g_sel = cid;
    g_scroll = 0;
    if (!already_backfilled(cid)) {
        oc_client_backfill(g_client, cid);
        if (g_n_backfilled < (int)(sizeof g_backfilled / sizeof g_backfilled[0]))
            g_backfilled[g_n_backfilled++] = cid;
    }
    oc_client_mark_read(g_client, cid);
    /* This channel's own roster (REQ-031). Asked on every switch rather than
     * cached: membership changes from other clients, a client stores nothing
     * (ARCH-88), and the list is small. */
    oc_client_list_members(g_client, cid);
    g_tab = TAB_MESSAGES;                /* a new channel opens on its transcript */
    draft_restore(cid);
    ac_close();
}

/* A channel's display name into `out` ("# general" / "@ bob"). */
static void channel_label(const oc_model *m, const oc_channel *c, char *out, size_t cap) {
    if (c->kind == OC_CHANNEL_KIND_DM) {
        const char *pn = oc_model_user_name(m, c->peer_id);
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
    { VIEW_ACTIVITY, OC_ICON_BELL,     "Activity", 0 },
    { VIEW_FILES,    OC_ICON_FILE,     "Files",    0 },
    { VIEW_LATER,    OC_ICON_BOOKMARK, "Later",    0 },
    { VIEW_ADMIN,    OC_ICON_SETTINGS, "Admin",    1 },
};
#define RAIL_N_ITEMS ((int)(sizeof RAIL_ITEMS / sizeof RAIL_ITEMS[0]))

static int ws_unread_elsewhere(void);   /* fwd */

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
    /* "N elsewhere" (WIN-29): unread sitting in the workspaces you are not
     * looking at. Without it, holding several clients would be invisible. */
    int elsewhere = ws_unread_elsewhere();
    if (elsewhere > 0) {
        char badge[8];
        snprintf(badge, sizeof badge, "%d", elsewhere > 99 ? 99 : elsewhere);
        float bw = text_width(badge, g_small) + 12;
        if (bw < 18) bw = 18;
        D2D1_RECT_F b = rf(av.right - bw + 6, av.top - 6, av.right + 6, av.top + 12);
        fill_round(rt, b, 9.0f, OC_COL_DANGER);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, badge, g_small, b, 0xFFFFFF);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    rail_hit(av.top - 6, av.bottom + 6, NAV_SWITCHER);
    fill(rt, rf(14, 58, RAIL_W - 14, 59), OC_COL_BORDER);   /* divider */

    /* Gate admin-only items, then overflow the tail into "More" when there isn't
     * enough vertical space (fold Admin first, up toward Home; Home never folds). */
    int vis[RAIL_N_ITEMS], nv = 0;
    for (int i = 0; i < RAIL_N_ITEMS; i++)
        if (!RAIL_ITEMS[i].admin || (m && self_role(m) >= OC_ROLE_ADMIN)) vis[nv++] = i;

    float y = 64;                                   /* below the workspace + divider */
    float by = h - 2 * RAIL_IH - 6;                 /* bottom cluster top */
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

    /* Bottom cluster (elastic spacer above): New, Profile.
     *
     * "Alerts" used to sit here with a bell, beside an "Activity" item carrying
     * a heartbeat line — two entries for one idea, both dead ends. Notification
     * *settings* already have a real home (the Notifications pane, WIN-12), so
     * Alerts was redundant as well as empty. It is gone, and Activity inherits
     * the bell: a pulse-line glyph reads as a system monitor, not as "things
     * that happened to you". */
    rail_item(rt, by,               OC_ICON_PLUS, "New",    NAV_NEW);
    /* Profile: a colored initial avatar for the signed-in user. */
    {
        float py = by + RAIL_IH;
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
        /* Reset the transform first: this block draws a glyph inside a filled
         * square, and a scale/translate left behind by any earlier icon in the
         * frame distorts it into the malformed-avatar artifact. */
        D2D1_MATRIX_3X2_F mid = {{{ 1, 0, 0, 1, 0, 0 }}};
        ID2D1RenderTarget_SetTransform(rt, &mid);

        D2D1_RECT_F av = rf(x + 14, cy + 12, x + 50, cy + 48);   /* 36px, as the rail */
        fill_round(rt, av, 10.0f, OC_COL_ACCENT);
        char nm[80]; ws_display_name(m, nm, sizeof nm);
        if (!nm[0]) snprintf(nm, sizeof nm, "OpenChime");        /* never a blank row */
        /* Skip anything that is not a letter or digit, so a workspace typed as
         * ":8443" or "-acme" still yields a sensible initial. */
        char in0 = 'O';
        for (const char *p = nm; *p; p++) {
            if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9')) {
                in0 = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
                break;
            }
        }
        char init[2] = { in0, 0 };
        draw_text(rt, init, g_ava, av, 0xFFFFFF);
        /* Three rows, 18px apart and non-overlapping (they used to collide). */
        draw_text(rt, nm, g_name, rf(x + 60, cy + 8, panel.right - 12, cy + 26), OC_COL_TEXT);
        char hostline[288]; snprintf(hostline, sizeof hostline, "%s:%d", g_host, g_port);
        draw_text(rt, hostline, g_small, rf(x + 60, cy + 26, panel.right - 12, cy + 43), OC_COL_MUTED);
        char mode[64]; ws_mode_line(m, mode, sizeof mode);
        draw_text(rt, mode, g_small, rf(x + 60, cy + 42, panel.right - 12, cy + 59), OC_COL_FAINT);
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
    /* Hit-boxes stay 24px for a comfortable click target, but the GLYPH is drawn
     * at 20px to match the rail — mixed icon sizes in one chrome read as a bug. */
    g_hdr_compose = rf(x1 - 24, 16, x1, 40);
    g_hdr_gear    = rf(x1 - 54, 16, x1 - 30, 40);
    D2D1_RECT_F ci = rf(g_hdr_compose.left + 2, g_hdr_compose.top + 2,
                        g_hdr_compose.right - 2, g_hdr_compose.bottom - 2);
    D2D1_RECT_F gi = rf(g_hdr_gear.left + 2, g_hdr_gear.top + 2,
                        g_hdr_gear.right - 2, g_hdr_gear.bottom - 2);
    draw_lucide(rt, OC_ICON_SQUARE_PEN, ci, OC_COL_MUTED);
    draw_lucide(rt, OC_ICON_SETTINGS,   gi, OC_COL_MUTED);
    g_ws_hdr_btn = rf(RAIL_W, 0, g_hdr_gear.left - 4, HEADER_H);
    char wsname[80]; ws_display_name(m, wsname, sizeof wsname);
    draw_text(rt, wsname, g_hdr, rf(RAIL_W + 16, 0, g_hdr_gear.left - 22, HEADER_H), OC_COL_TEXT);
    /* Chevron and connection dot both hug the NAME, in that order — the chevron
     * belongs to the name (it opens the workspace menu), and the dot is a status
     * on the workspace, so both travel with the text rather than sitting at the
     * far edge where they read as unrelated chrome. */
    {
        float nx  = RAIL_W + 16 + text_width(wsname, g_hdr);
        float lim = g_hdr_gear.left - 30;              /* never crowd the gear */
        if (nx > lim) nx = lim;
        draw_text(rt, "\xE2\x96\xBE", g_small, rf(nx + 6, 2, nx + 22, HEADER_H), OC_COL_MUTED);

        float dx = nx + 22;
        if (dx > g_hdr_gear.left - 20) dx = g_hdr_gear.left - 20;
        int live = m->authed ? 1 : 0;
        draw_conn_dot(rt, dx + 5, HEADER_H / 2, 4.5f, live);
        /* Only a hit-box when it would DO something: a control that silently
         * does nothing is worse than no control. */
        g_ws_dot = live ? rf(0, 0, 0, 0)
                        : rf(dx - 4, HEADER_H / 2 - 12, dx + 14, HEADER_H / 2 + 12);
    }

    /* "Find a conversation" box — a rounded container with a search glyph; the
     * native EDIT child (g_find) sits inside and live-filters the list. */
    D2D1_RECT_F fb = rf(RAIL_W + 10, HEADER_H + 6, RAIL_W + SIDEBAR_W - 10, HEADER_H + 36);
    fill_round(rt, fb, 8.0f, OC_COL_INPUT);
    stroke_round(rt, fb, 8.0f, OC_COL_BORDER, 1.0f);
    draw_lucide(rt, OC_ICON_SEARCH, rf(fb.left + 8, fb.top + 7, fb.left + 24, fb.top + 23), OC_COL_MUTED);

    /* Rows come from the core (oc_model_sidebar): grouping, filtering, sorting
     * and collapse are decided there so both frontends agree. The DMs rail view
     * shows only that section; Home shows both. */
    oc_sidebar_opts o = g_sb;
    snprintf(o.find, sizeof o.find, "%s", g_find_filter);
    if (g_view == VIEW_DMS) o.collapsed[OC_SB_CHANNELS] = 1;
    /* Sized to what the model actually holds — a fixed 512 was a silent cap on
     * how many conversations could appear, which a busy workspace would hit
     * with no indication that rows were missing. Grown once and kept. */
    static oc_sidebar_row *rows;
    static size_t rows_cap;
    size_t need = m->n_channels + OC_SB_SECTIONS + 2;
    if (need > rows_cap) {
        oc_sidebar_row *g = realloc(rows, need * sizeof *g);
        if (g) { rows = g; rows_cap = need; }
    }
    if (!rows) return;
    size_t nrows = oc_model_sidebar(m, &o, rows, rows_cap);

    float top = HEADER_H + 46, bot = h;
    g_sb_view = bot - top;
    g_sb_content = (float)nrows * ROW_H;
    /* Clamp before drawing, so a resize or a collapse cannot strand the list. */
    float maxscroll = g_sb_content > g_sb_view ? g_sb_content - g_sb_view : 0;
    if (g_sb_scroll > maxscroll) g_sb_scroll = maxscroll;
    if (g_sb_scroll < 0) g_sb_scroll = 0;

    float sx0 = RAIL_W + 8, sx1 = RAIL_W + SIDEBAR_W - 8;
    float y = top - g_sb_scroll;
    g_n_rows = 0;
    g_sb_kebab = rf(0, 0, 0, 0);

    for (size_t ri = 0; ri < nrows; ri++) {
        const oc_sidebar_row *r = &rows[ri];
        float ry = y; y += ROW_H;
        if (ry + ROW_H < top || ry > bot) continue;          /* virtualized */

        if (r->is_header) {
            const char *chev = g_sb.collapsed[r->section] ? "\xE2\x96\xB8" : "\xE2\x96\xBE";
            draw_text(rt, chev, g_small, rf(sx0 + 6, ry, sx0 + 22, ry + ROW_H), OC_COL_MUTED);
            draw_text(rt, r->label, g_small, rf(sx0 + 22, ry, sx1 - 30, ry + ROW_H), OC_COL_FAINT);
            if (g_sb_hover_sec == r->section || g_sb_menu_sec == r->section) {
                g_sb_kebab = rf(sx1 - 26, ry + 4, sx1 - 6, ry + ROW_H - 4);
                IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
                draw_text(rt, "\xE2\x8B\xAF", g_small, g_sb_kebab, OC_COL_MUTED);
                IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        } else {
            int selected = (r->channel_id == g_sel);
            int unread = r->unread > 0;
            if (selected) fill_round(rt, rf(sx0, ry + 2, sx1, ry + ROW_H - 2), 6.0f, OC_COL_SELECT);
            if (r->section == OC_SB_DMS) {
                /* A person gets an avatar and a presence dot, not an "@" glyph —
                 * the marker in Slack's DM list is the human, not the sigil. */
                D2D1_RECT_F av = rf(sx0 + 12, ry + (ROW_H - 18) / 2, sx0 + 30, ry + (ROW_H + 18) / 2);
                uint32_t tint = AVPAL[r->peer_id % (sizeof AVPAL / sizeof AVPAL[0])];
                fill_round(rt, av, 5.0f, tint);
                char ini[2] = { (char)(r->label[0] >= 'a' && r->label[0] <= 'z'
                                       ? r->label[0] - 32 : r->label[0]), 0 };
                IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
                draw_text(rt, ini, g_small, av, 0xFFFFFF);
                IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
                draw_presence_dot(rt, av.right - 1, av.bottom - 1, 3.5f,
                                  oc_model_presence_of(m, r->peer_id),
                                  selected ? OC_COL_SELECT : OC_COL_SIDEBAR);
            } else {
                const char *mark = r->is_private ? "\xF0\x9F\x94\x92" : "#";
                draw_text(rt, mark, g_ui, rf(sx0 + 12, ry, sx0 + 34, ry + ROW_H),
                          selected ? OC_COL_TEXT : OC_COL_FAINT);
            }
            uint32_t fg = (selected || unread) ? OC_COL_TEXT : OC_COL_MUTED;
            draw_text(rt, r->label, unread ? g_ui_b : g_ui,
                      rf(sx0 + 34, ry, sx1 - 44, ry + ROW_H), fg);
            if (r->is_self) {
                /* "you" sits right after the name, dimmed — so the self-DM is
                 * still addressed by account name (Slack's treatment). */
                float w = text_width(r->label, unread ? g_ui_b : g_ui);
                draw_text(rt, "you", g_small,
                          rf(sx0 + 34 + w + 8, ry, sx1 - 44, ry + ROW_H), OC_COL_FAINT);
            }
            if (unread) {
                char badge[16]; snprintf(badge, sizeof badge, "%d", r->unread);
                D2D1_RECT_F br = rf(sx1 - 40, ry + 6, sx1 - 10, ry + ROW_H - 6);
                fill_round(rt, br, 9.0f, OC_COL_ACCENT);
                IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
                draw_text(rt, badge, g_small, br, 0xFFFFFF);
                IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
            }
        }
        if (g_n_rows < (int)(sizeof g_rows / sizeof g_rows[0])) {
            g_rows[g_n_rows].top = ry; g_rows[g_n_rows].bot = ry + ROW_H;
            g_rows[g_n_rows].cid = r->channel_id;
            g_rows[g_n_rows].header = r->is_header;
            g_rows[g_n_rows].sec = r->section;
            g_n_rows++;
        }
    }

    /* Scrollbar, only when there is overflow — the reason channels past the fold
     * used to be unreachable (WIN-6). */
    if (maxscroll > 0) {
        float trackh = bot - top;
        float th = trackh * (g_sb_view / g_sb_content); if (th < 24) th = 24;
        float ty = top + (trackh - th) * (g_sb_scroll / maxscroll);
        fill_round(rt, rf(sx1 + 2, ty, sx1 + 5, ty + th), 1.5f, OC_COL_BORDER);
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
    /* Mark the @mentions (REQ-221). The scanner is the one the daemon resolves
     * with, so what is highlighted is exactly what the server acted on — a
     * second implementation would drift and nobody could tell which was right.
     * Spans arrive as BYTE offsets into UTF-8 and the layout indexes UTF-16, so
     * each is re-measured rather than assumed equal. */
    if (layout && !msg->deleted && g_brush3) {
        const char *utf8 = body_text(msg);
        size_t blen = utf8 ? strlen(utf8) : 0;
        oc_mention mm[OC_MENTION_MAX];
        size_t nm = oc_mention_scan(utf8, blen, mm, OC_MENTION_MAX);
        if (nm > OC_MENTION_MAX) nm = OC_MENTION_MAX;
        for (size_t i = 0; i < nm; i++) {
            int u16_start = MultiByteToWideChar(CP_UTF8, 0, utf8, (int)mm[i].start, NULL, 0);
            int u16_len   = MultiByteToWideChar(CP_UTF8, 0, utf8 + mm[i].start,
                                                (int)mm[i].len, NULL, 0);
            if (u16_start < 0 || u16_len <= 0) continue;
            DWRITE_TEXT_RANGE tr = { (UINT32)u16_start, (UINT32)u16_len };
            IDWriteTextLayout_SetDrawingEffect(layout, (IUnknown *)g_brush3, tr);
            IDWriteTextLayout_SetFontWeight(layout, DWRITE_FONT_WEIGHT_SEMI_BOLD, tr);
        }
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
/* A pinned message gets a marker line above its header (REQ-230), the way
 * Slack does — a pin you can only see by opening a list is a pin you forget.
 * It sits ABOVE the block, so it costs height on grouped continuations too. */
#define MSG_PIN_H 17.0f
#define MSG_PIN(msg) ((msg)->pinned ? MSG_PIN_H : 0.0f)

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
    float thumbs = 0;
    if (msg->n_reactions) extra++;
    for (int i = 0; i < msg->n_attach; i++) {
        /* An image gets a thumbnail box instead of a text line — and the space
         * must be reserved whether or not the bitmap has arrived, or the
         * transcript jumps under the reader the moment one decodes. */
        if (!msg->attach[i].reclaimed && mime_is_image(msg->attach[i].mime))
            thumbs += THUMB_H + 6.0f + LINE_H;   /* + the filename line above it */
        else
            extra++;
    }
    if (msg->reply_count) extra++;
    return MSG_PIN(msg) + MSG_BODY_DY(grouped) + body_h +
           (float)extra * LINE_H + thumbs + MSG_BOT(grouped);
}

static int reaction_is_mine(const oc_msg *msg, const char *emoji);   /* fwd */

static void draw_message(ID2D1RenderTarget *rt, const oc_model *m, const oc_msg *msg,
                         IDWriteTextLayout *body, float x0, float y, float content_w,
                         int grouped) {
    float ax = x0, tx = x0 + AVA + 12;

    /* The pin marker, in the body column so it lines up with the text rather
     * than floating over the avatar gutter. */
    if (msg->pinned) {
        float py = y + 3;
        draw_lucide(rt, OC_ICON_PIN, rf(tx, py, tx + 12, py + 12), OC_COL_MUTED);
        char lbl[96];
        const char *who = oc_model_user_name((oc_model *)m, msg->pinned_by);
        if (who && who[0]) snprintf(lbl, sizeof lbl, "Pinned by %s", who);
        else               snprintf(lbl, sizeof lbl, "Pinned");
        draw_text(rt, lbl, g_small, rf(tx + 17, py - 2, x0 + content_w + AVA + 12, py + 14),
                  OC_COL_MUTED);
        y += MSG_PIN_H;          /* everything below shifts down by the marker */
    }

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
            struct tm tv; char when[24] = "";
            if (oc_localtime_r(&t, &tv))
                strftime(when, sizeof when, g_pref_time24 ? "%H:%M" : "%I:%M %p", &tv);
            draw_text(rt, when, g_time, hl, OC_COL_FAINT);
        }
    }

    /* Body. */
    float by = y + MSG_BODY_DY(grouped);
    if (body) {
        D2D1_POINT_2F org = { tx, by };
        uint32_t bcol = msg->deleted ? OC_COL_FAINT : OC_COL_TEXT;
        /* Colour emoji in message text. Without ENABLE_COLOR_FONT DirectWrite
         * falls back to the monochrome outline glyphs, so a posted emoji looked
         * washed out next to the very same character in the picker or on a
         * reaction chip — which do set the flag. */
        ID2D1RenderTarget_DrawTextLayout(rt, org, body, paint_with(bcol),
                                         D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
        DWRITE_TEXT_METRICS tm;
        if (SUCCEEDED(IDWriteTextLayout_GetMetrics(body, &tm))) by += tm.height;
        else by += 18;
    }
    /* "(edited)" is drawn inline by body_layout (faint, after the last word). */

    /* Meta lines: attachments, thread, then reactions LAST — see below. */
    for (int i = 0; i < msg->n_attach; i++) {
        const oc_attachment *at = &msg->attach[i];
        if (!at->reclaimed && mime_is_image(at->mime)) {
            /* Filename above the image, as Slack does: the thumbnail alone does
             * not tell you what the file is called or whether it is the one you
             * were sent. */
            draw_text(rt, at->filename, g_small,
                      rf(tx, by, x0 + content_w + AVA + 12, by + LINE_H), OC_COL_MUTED);
            by += LINE_H;
            UINT iw = 0, ih = 0;
            ID2D1Bitmap *bmp = thumb_get(rt, at->id, &iw, &ih);
            float bw = THUMB_W, bh = THUMB_H;
            if (bmp) {
                /* Fit inside the box and never upscale: the box is a maximum,
                 * not a target, and a 40px icon blown up to 260 looks broken. */
                if (iw > 0 && ih > 0) {
                    float sc = THUMB_W / (float)iw;
                    if (THUMB_H / (float)ih < sc) sc = THUMB_H / (float)ih;
                    if (sc > 1.0f) sc = 1.0f;
                    bw = (float)iw * sc; bh = (float)ih * sc;
                }
                D2D1_RECT_F dst = rf(tx, by + 3, tx + bw, by + 3 + bh);
                ID2D1RenderTarget_DrawBitmap(rt, bmp, &dst, 1.0f,
                                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
                stroke_round(rt, dst, 6.0f, OC_COL_BORDER, 1.0f);
            } else {
                D2D1_RECT_F ph = rf(tx, by + 3, tx + bw, by + 3 + bh);
                fill_round(rt, ph, 6.0f, OC_COL_INPUT);
                stroke_round(rt, ph, 6.0f, OC_COL_BORDER, 1.0f);
                /* The filename is on the line above; this only says why there is
                 * no picture yet. */
                IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
                draw_text(rt, thumb_failed(at->id) ? "preview unavailable" : "loading\u2026",
                          g_small, ph, OC_COL_FAINT);
                IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
                /* Ask for it — one at a time, and only for what is on screen.
                 * Never while thumbnails are suppressed for the screenshot
                 * harness: that render is not the user's screen, and treating
                 * its forced miss as "not fetched yet" re-requested every image
                 * on every shot. */
                if (!g_thumbs_off && !thumb_failed(at->id) && !g_thumb_pending && g_client) {
                    g_thumb_pending = at->id;
                    g_thumb_deadline = GetTickCount64() + 8000;
                    oc_client_fetch_attachment(g_client, at->id);
                }
            }
            /* One hit-box for the whole image area whether or not it decoded,
             * so hover (and therefore the toolbar) works while it is loading. */
            if (g_n_thumb_hits < 32) {
                g_thumb_hits[g_n_thumb_hits].r = rf(tx, by + 3, tx + bw, by + 3 + bh);
                g_thumb_hits[g_n_thumb_hits].id = at->id;
                g_n_thumb_hits++;
            }
            /* Hover toolbar, top-right of the image: save, and a kebab for the
             * rest. Outside the decoded-bitmap branch on purpose — it acts on
             * the ATTACHMENT, so it should work while the thumbnail is still
             * loading or failed to decode, not only once a picture exists. */
            if (g_thumb_hover == at->id && g_n_thumb_dl + 1 < 32) {
                float bs = 26, gap = 4;
                D2D1_RECT_F area = rf(tx, by + 3, tx + bw, by + 3 + bh);
                D2D1_RECT_F kb = rf(area.right - 6 - bs, area.top + 6,
                                    area.right - 6, area.top + 6 + bs);
                D2D1_RECT_F db = rf(kb.left - gap - bs, kb.top, kb.left - gap, kb.bottom);
                fill_round_a(rt, db, 6.0f, 0x000000, 0.62f);
                fill_round_a(rt, kb, 6.0f, 0x000000, 0.62f);
                draw_lucide(rt, OC_ICON_DOWNLOAD, rf(db.left + 5, db.top + 5,
                                                     db.right - 5, db.bottom - 5), 0xFFFFFF);
                draw_lucide(rt, OC_ICON_ELLIPSIS, rf(kb.left + 4, kb.top + 4,
                                                     kb.right - 4, kb.bottom - 4), 0xFFFFFF);
                g_thumb_dl[g_n_thumb_dl].r = db;
                g_thumb_dl[g_n_thumb_dl].attach_ix = i;
                g_thumb_dl[g_n_thumb_dl].mid = msg->message_id;
                g_n_thumb_dl++;
                g_thumb_dl[g_n_thumb_dl].r = kb;
                g_thumb_dl[g_n_thumb_dl].attach_ix = -(i + 1);   /* negative = kebab */
                g_thumb_dl[g_n_thumb_dl].mid = msg->message_id;
                g_n_thumb_dl++;
            }
            by += THUMB_H + 6.0f;
            continue;
        }
        char line[200];
        if (at->reclaimed)
            snprintf(line, sizeof line, "\xF0\x9F\x93\x8E %s (no longer available)", at->filename);
        else
            snprintf(line, sizeof line, "\xF0\x9F\x93\x8E %s", at->filename);
        draw_text(rt, line, g_small, rf(tx, by, x0 + content_w + AVA + 12, by + LINE_H), OC_COL_ACCENT);
        by += LINE_H;
    }
    if (msg->reply_count) {
        char line[64];
        snprintf(line, sizeof line, "\xE2\x86\xB3 %u %s", msg->reply_count,
                 msg->reply_count == 1 ? "reply" : "replies");
        draw_text(rt, line, g_small, rf(tx, by, x0 + content_w + AVA + 12, by + LINE_H), OC_COL_ACCENT);
        by += LINE_H;
    }

    /* Reactions LAST, under everything the message carries. Drawn between the
     * body and the attachments they were stranded mid-block on any message with
     * an image; they are the footer of a message, not part of its text.
     *
     * Chips rather than one grey run: the emoji needs the colour-font path
     * anyway, and a bordered count reads as the clickable thing it now is. */
    if (msg->n_reactions) {
        float cx = tx, ch = 22, top = by + 1;
        for (int i = 0; i < msg->n_reactions; i++) {
            char cnt[16];
            snprintf(cnt, sizeof cnt, "%u", msg->reactions[i].count);
            float cw = 34 + text_width(cnt, g_small);
            if (cx + cw > x0 + content_w + AVA + 12) break;
            D2D1_RECT_F chip = rf(cx, top, cx + cw, top + ch);
            int mine = reaction_is_mine(msg, msg->reactions[i].emoji);
            fill_round(rt, chip, 11.0f, mine ? OC_COL_SELECT : OC_COL_INPUT);
            stroke_round(rt, chip, 11.0f, mine ? OC_COL_ACCENT : OC_COL_BORDER, 1.0f);
            draw_emoji_fmt(rt, msg->reactions[i].emoji,
                           rf(cx + 4, top, cx + 22, top + ch), g_emoji_s);
            draw_text(rt, cnt, g_small, rf(cx + 24, top, chip.right - 4, top + ch),
                      mine ? OC_COL_TEXT : OC_COL_MUTED);
            /* Hit-box so a chip can be clicked: +1, or undo if it is already
               yours. The direction is `mine`, exactly as the message menu
               computes it — one rule, two entry points. */
            if (g_n_chips < (int)(sizeof g_chips / sizeof g_chips[0])) {
                g_chips[g_n_chips].r = chip;
                g_chips[g_n_chips].mid = msg->message_id;
                g_chips[g_n_chips].mine = (uint8_t)mine;
                snprintf(g_chips[g_n_chips].emoji, sizeof g_chips[g_n_chips].emoji,
                         "%s", msg->reactions[i].emoji);
                g_n_chips++;
            }
            cx += cw + 5;
        }
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
/* mode: MSGLIST_MAIN for the transcript, MSGLIST_THREAD for the thread pane.
 * The thread pane needs the same hit-boxes and scrollbar the main list has —
 * without them its replies were read-only and it could not scroll (WIN-15) —
 * but not the date dividers, which only make sense on a day-spanning scroll. */
enum { MSGLIST_MAIN = 1, MSGLIST_THREAD = 2 };

static void draw_msglist(ID2D1RenderTarget *rt, const oc_model *m,
                         const oc_msg *msgs, size_t nmsgs, D2D1_RECT_F reg, int mode) {
    int capture = (mode == MSGLIST_MAIN);
    int hits    = (mode != 0);
    float *scroll     = capture ? &g_scroll     : &g_thr_scroll;
    float *scroll_max = capture ? &g_scroll_max : &g_thr_scroll_max;
    float pad = 20.0f;
    float x0 = reg.left + pad;
    float content_w = (reg.right - pad) - (x0 + AVA + 12);
    if (content_w < 80) content_w = 80;

    size_t n = nmsgs, first = 0;
    /* The render window, not a history limit: with paging (WIN-16) a channel can
     * hold far more than this, and only the newest CAP are laid out. Raised from
     * 600 so several pages stay reachable by scrolling without re-requesting. */
    enum { CAP = 2000 };
    static IDWriteTextLayout *layouts[CAP];
    static float heights[CAP];
    static uint32_t wlens[CAP];
    static uint8_t grouped[CAP];
    static uint8_t sep[CAP];       /* a date divider precedes this message */
    if (n > CAP) { first = n - CAP; n = CAP; }

    float total = 0;
    long jump_i = -1; float jump_off = 0;
    for (size_t i = 0; i < n; i++) {
        /* Date dividers only in the main transcript, not the thread/search panes. */
        sep[i] = (uint8_t)(capture && g_pref_daysep && (i == 0 ||
                 !same_day(msgs[first + i - 1].server_time, msgs[first + i].server_time)));
        grouped[i] = (uint8_t)(!sep[i] && i > 0 &&
                     groups_with(&msgs[first + i - 1], &msgs[first + i]));
        heights[i] = msg_height(&msgs[first + i], content_w, grouped[i], &layouts[i], &wlens[i]);
        total += (sep[i] ? SEP_H : 0);
        if (capture && g_unread_from && g_unread_chan == g_sel &&
            msgs[first + i].message_id > g_unread_from &&
            (i == 0 || msgs[first + i - 1].message_id <= g_unread_from))
            total += SEP_H;
        if (capture && g_jump_mid && msgs[first + i].message_id == g_jump_mid) {
            jump_i = (long)i; jump_off = total;      /* distance from content top */
        }
        total += heights[i];
    }

    float visible = reg.bottom - reg.top;
    *scroll_max = total > visible ? total - visible : 0;

    /* Resolve an armed jump (WIN-3). A message's top sits at
     *   screen_y = (reg.bottom - total) + g_scroll + jump_off,
     * so placing it a third of the way down the pane solves for g_scroll. The
     * clamp is what keeps a hit in the newest or oldest screenful sensible
     * rather than scrolling past the end. */
    if (capture && jump_i >= 0) {
        *scroll = total - jump_off - visible * 0.66f;
        g_flash_mid = g_jump_mid;
        g_flash_until = GetTickCount64() + 1600;
        g_jump_mid = 0;
    }

    if (*scroll > *scroll_max) *scroll = *scroll_max;
    if (*scroll < 0) *scroll = 0;

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

    float y = (reg.bottom - total) + *scroll;     /* g_scroll 0 => newest pinned to bottom */

    ID2D1RenderTarget_PushAxisAlignedClip(rt, &reg, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    /* Chip hit-boxes reset unconditionally: the thread pane draws chips too, and
     * only one of the two lists is drawn per frame — resetting only on `capture`
     * would leave the thread's chips pointing at stale rectangles. */
    g_n_chips = 0;
    if (capture) { g_n_msgrows = 0; g_n_thumb_hits = 0; g_n_thumb_dl = 0; }
    else if (hits) g_n_thrrows = 0;
    for (size_t i = 0; i < n; i++) {
        if (sep[i]) {
            if (y + SEP_H >= reg.top && y <= reg.bottom)
                draw_day_sep(rt, msgs[first + i].server_time, reg, y);
            y += SEP_H;
        }
        /* The "New" divider sits above the first message past the marker. */
        if (capture && g_unread_from && g_unread_chan == g_sel &&
            msgs[first + i].message_id > g_unread_from &&
            (i == 0 || msgs[first + i - 1].message_id <= g_unread_from)) {
            if (y + SEP_H >= reg.top && y <= reg.bottom) {
                float my = y + SEP_H / 2;
                fill(rt, rf(reg.left + 20, my, reg.right - 70, my + 1), OC_COL_DANGER);
                IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_TRAILING);
                draw_text(rt, "New", g_small,
                          rf(reg.left + 20, y, reg.right - 20, y + SEP_H), OC_COL_DANGER);
                IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
            }
            y += SEP_H;
        }
        if (y + heights[i] >= reg.top && y <= reg.bottom) {
            float bx = x0 + AVA + 12;
            float by = y + MSG_PIN(&msgs[first + i]) + MSG_BODY_DY(grouped[i]);
            /* Hover highlight behind the whole row (main transcript only). */
            if (capture && !g_selecting && g_hover_mid == msgs[first + i].message_id)
                fill(rt, rf(reg.left, y, reg.right, y + heights[i]), OC_COL_HOVER);
            /* A message that names YOU gets the row tinted, which is the
             * "highlighted for the mentioned party" half of REQ-221 — a
             * coloured word alone is easy to scroll past. Same scanner as the
             * notification, so the two always agree about what counted. */
            if (capture && !msgs[first + i].deleted && msgs[first + i].body) {
                const char *me = oc_model_user_name(m, m->user_id);
                if (msgs[first + i].author_id != m->user_id &&
                    oc_mention_targets(msgs[first + i].body,
                                       strlen(msgs[first + i].body), me)) {
                    D2D1_RECT_F mr = rf(reg.left, y, reg.right, y + heights[i]);
                    ID2D1RenderTarget_FillRectangle(rt, &mr, paint_alpha(OC_COL_ACCENT, 0.10f));
                    fill(rt, rf(reg.left, y, reg.left + 3, y + heights[i]), OC_COL_ACCENT);
                }
            }
            /* Jump flash — fades out so it reads as "here it is", not as state. */
            if (capture && g_flash_mid == msgs[first + i].message_id) {
                ULONGLONG now = GetTickCount64();
                if (now < g_flash_until) {
                    float a = (float)(g_flash_until - now) / 1600.0f;
                    D2D1_RECT_F fr = rf(reg.left, y, reg.right, y + heights[i]);
                    ID2D1RenderTarget_FillRectangle(rt, &fr, paint_alpha(OC_COL_NOTICE, a * 0.30f));
                } else {
                    g_flash_mid = 0;
                }
            }
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
            if (hits) {
                int *n = capture ? &g_n_msgrows : &g_n_thrrows;
                int cap = capture ? (int)(sizeof g_msgrows / sizeof g_msgrows[0])
                                  : (int)(sizeof g_thrrows / sizeof g_thrrows[0]);
                if (*n < cap) {
                    oc_msgrow *row = capture ? &g_msgrows[*n] : &g_thrrows[*n];
                    row->top = y; row->bot = y + heights[i];
                    row->bx = bx; row->by = by; row->cw = content_w;
                    row->mid = msgs[first + i].message_id;
                    (*n)++;
                }
            }
        }
        y += heights[i];
        if (layouts[i]) IDWriteTextLayout_Release(layouts[i]);
        layouts[i] = NULL;
    }

    /* At the top of what we hold, pull the previous page (WIN-16). Guarded by a
     * single in-flight request and by a per-channel "no more" mark, so this
     * cannot turn a scroll into a request storm. */
    if (capture && *scroll_max > 0.5f && *scroll >= *scroll_max - 1.0f &&
        nmsgs > 0 && g_sel && !g_hist_pending_chan) {
        int done = 0;
        for (int k = 0; k < g_n_hist_exhausted; k++) if (g_hist_exhausted[k] == g_sel) done = 1;
        if (!done) {
            g_hist_pending_chan = g_sel;
            g_hist_deadline = GetTickCount64() + 5000;
            g_hist_before = msgs[0].message_id;
            oc_client_history(g_client, g_sel, msgs[0].message_id);
        }
    }
    if (capture && g_hist_pending_chan == g_sel)
        draw_text(rt, "Loading older messages\u2026", g_small,
                  rf(reg.left + 20, reg.top + 4, reg.right - 20, reg.top + 24), OC_COL_FAINT);

    /* Scrollbar. scroll 0 => pinned bottom => thumb at the bottom of the track;
     * scroll_max => scrolled to top => thumb at top. */
    if (hits) {
        if (*scroll_max > 0.5f && total > 0) {
            float track_top = reg.top + 4, track_h = visible - 8;
            float thumb_h = visible / total * track_h;
            if (thumb_h < 30) thumb_h = 30;
            if (thumb_h > track_h) thumb_h = track_h;
            float travel = track_h - thumb_h;
            float thumb_top = track_top + (1.0f - *scroll / *scroll_max) * travel;
            float sx = reg.right - 10;
            D2D1_RECT_F th = rf(sx, thumb_top, sx + 6, thumb_top + thumb_h);
            /* Only the main transcript's thumb is draggable; the thread pane
             * scrolls by wheel, so it must not claim the drag hit-box. */
            if (capture) { g_sbar_track_top = track_top; g_sbar_travel = travel; g_sbar_thumb = th; }
            fill_round(rt, th, 3.0f, OC_COL_FAINT);
        } else if (capture) {
            g_sbar_thumb = rf(0, 0, 0, 0);
        }
    }
    ID2D1RenderTarget_PopAxisAlignedClip(rt);
}

/* ---- shared overlay scrolling ---------------------------------------------
 * Every list overlay used to stop drawing at the pane bottom, so anything past
 * the fold was unreachable. One scroll offset serves them all: only one overlay
 * is ever open, and switching overlays resets it. */
static float g_ovl_scroll, g_ovl_max;
static int   g_ovl_kind;          /* which overlay the offset belongs to */

static void ovl_use(int kind) {
    if (g_ovl_kind != kind) { g_ovl_kind = kind; g_ovl_scroll = 0; }
}

/* Clip to `body`, clamp the offset for `content_h`, and return the first row's
 * y. Pair with ovl_end(). */
static float ovl_begin(ID2D1RenderTarget *rt, D2D1_RECT_F body, float content_h) {
    float visible = body.bottom - body.top;
    g_ovl_max = content_h > visible ? content_h - visible : 0;
    if (g_ovl_scroll > g_ovl_max) g_ovl_scroll = g_ovl_max;
    if (g_ovl_scroll < 0) g_ovl_scroll = 0;
    ID2D1RenderTarget_PushAxisAlignedClip(rt, &body, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    return body.top + 6 - g_ovl_scroll;
}

static void ovl_end(ID2D1RenderTarget *rt, D2D1_RECT_F body) {
    if (g_ovl_max > 0.5f) {
        float visible = body.bottom - body.top;
        float track = visible - 8, thumb = visible / (visible + g_ovl_max) * track;
        if (thumb < 30) thumb = 30;
        float top = body.top + 4 + (g_ovl_scroll / g_ovl_max) * (track - thumb);
        fill_round(rt, rf(body.right - 10, top, body.right - 4, top + thumb), 3.0f, OC_COL_FAINT);
    }
    ID2D1RenderTarget_PopAxisAlignedClip(rt);
}

enum { OVL_AUDIT = 1, OVL_WEB, OVL_REACT, OVL_NOTIFY, OVL_KEYS };

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
    else draw_msglist(rt, m, m->thread_msgs, m->n_thread_msgs, replies, MSGLIST_THREAD);
}

static void draw_search(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    D2D1_RECT_F body = overlay_header(rt, reg, "Search");
    g_n_searchrows = 0;

    /* The query box lives IN the overlay (WIN-4), so refining a search never
     * closes and reopens it. The native EDIT is placed over this chrome by
     * layout_search(). */
    g_srch_box = rf(body.left + 20, body.top + 10, body.right - 20, body.top + 42);
    fill_round(rt, g_srch_box, 6.0f, OC_COL_INPUT);
    stroke_round(rt, g_srch_box, 6.0f, OC_COL_BORDER, 1.0f);
    draw_lucide(rt, OC_ICON_SEARCH, rf(g_srch_box.left + 8, g_srch_box.top + 8,
                                       g_srch_box.left + 24, g_srch_box.top + 24), OC_COL_MUTED);
    body.top += 52;

    /* A count line, so "5 results" and "no matches" are told apart at a glance. */
    char count[96];
    if (!m->search_query[0])   snprintf(count, sizeof count, "Type a query and press Enter.");
    else if (m->n_search == 0) snprintf(count, sizeof count, "No matches for \u201c%s\u201d.", m->search_query);
    else snprintf(count, sizeof count, "%zu %s for \u201c%s\u201d%s",
                  m->n_search, m->n_search == 1 ? "result" : "results", m->search_query,
                  /* No cursor exists on the wire to page with (WIN-38), so say
                   * that more exist rather than implying these are all of them. */
                  m->search_truncated ? " \u2014 more exist; narrow the query to see them." : "");
    draw_text(rt, count, g_small, rf(body.left + 20, body.top, body.right - 16, body.top + 18),
              m->search_truncated ? OC_COL_NOTICE : OC_COL_MUTED);
    body.top += 22;

    if (m->n_search == 0) { g_srch_max = g_srch_scroll = 0; return; }

    float rowh = 54;
    float visible = body.bottom - body.top;
    float total = (float)m->n_search * rowh + 12;
    g_srch_max = total > visible ? total - visible : 0;
    if (g_srch_scroll > g_srch_max) g_srch_scroll = g_srch_max;
    if (g_srch_scroll < 0) g_srch_scroll = 0;

    ID2D1RenderTarget_PushAxisAlignedClip(rt, &body, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
    float y = body.top + 6 - g_srch_scroll;
    for (size_t i = 0; i < m->n_search; i++) {
        if (y + rowh < body.top) { y += rowh; continue; }    /* above the fold */
        if (y > body.bottom) break;
        const oc_search_result *r = &m->search_results[i];
        const char *au = oc_model_user_name(m, r->author_id);
        const oc_channel *ch = oc_model_channel((oc_model *)m, r->channel_id);
        char head[160];
        /* 17 bytes minimum for "YYYY-MM-DD HH:MM" plus the NUL — at 16 strftime
         * silently writes nothing and every result showed a blank time. */
        char when[32] = "";
        if (r->server_time) { time_t t = (time_t)(r->server_time / 1000); struct tm tv;
            if (oc_localtime_r(&t, &tv)) strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tv); }
        snprintf(head, sizeof head, "%s  ·  %s  ·  %s",
                 (au && au[0]) ? au : "user", (ch && ch->name) ? ch->name : "channel", when);
        draw_text(rt, head, g_small, rf(body.left + 20, y, body.right - 16, y + 20), OC_COL_MUTED);
        draw_text_hl(rt, r->snippet ? r->snippet : "", g_ui,
                     rf(body.left + 20, y + 20, body.right - 16, y + 46), OC_COL_TEXT,
                     m->search_query);
        fill(rt, rf(body.left + 20, y + rowh - 1, body.right - 16, y + rowh), OC_COL_BORDER);
        if (g_n_searchrows < (int)(sizeof g_searchrows / sizeof g_searchrows[0])) {
            g_searchrows[g_n_searchrows].top = y; g_searchrows[g_n_searchrows].bot = y + rowh;
            g_searchrows[g_n_searchrows].cid = r->channel_id;
            g_searchrows[g_n_searchrows].mid = r->message_id; g_n_searchrows++;
        }
        y += rowh;
    }
    if (g_srch_max > 0.5f) {
        float track_h = visible - 8, thumb_h = visible / total * track_h;
        if (thumb_h < 30) thumb_h = 30;
        float thumb_top = body.top + 4 + (g_srch_scroll / g_srch_max) * (track_h - thumb_h);
        fill_round(rt, rf(body.right - 10, thumb_top, body.right - 4, thumb_top + thumb_h),
                   3.0f, OC_COL_FAINT);
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

/* WIN-24: TUI parity plus a refresh. The TUI's version groups the numbers into
 * Disk / Policy / Reclaimed and flags pressure and evictions in red; this was a
 * flat key/value dump with no way to ask again. */
static D2D1_RECT_F g_storage_refresh;

/* `embedded` = drawn inside another surface that already has a header (the Admin
 * view). Two stacked titles, the inner one offering "Esc to close" for something
 * Esc does not close, is worse than no title. */
static void draw_storage(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg, int embedded) {
    D2D1_RECT_F body = embedded ? reg : overlay_header(rt, reg, "Storage usage");

    g_storage_refresh = rf(body.right - 116, body.top + 8, body.right - 20, body.top + 36);
    fill_round(rt, g_storage_refresh, 6.0f, OC_COL_INPUT);
    stroke_round(rt, g_storage_refresh, 6.0f, OC_COL_BORDER, 1.0f);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(rt, "Refresh", g_small, g_storage_refresh, OC_COL_MUTED);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);

    if (!m->storage_have) { overlay_empty(rt, body, "Loading\u2026"); return; }
    const oc_storage_view *s = &m->storage;
    char v[96]; float y = body.top + 12;

    draw_text(rt, "DISK", g_small, rf(body.left + 24, y, body.right - 20, y + 18), OC_COL_FAINT);
    y += 22;
    human_bytes(s->avail_bytes, v, sizeof v);
    char both[96]; char tot[64];
    human_bytes(s->total_bytes, tot, sizeof tot);
    snprintf(both, sizeof both, "%s free of %s", v, tot);
    draw_kv(rt, body, &y, "Free", both, s->under_pressure ? OC_COL_DANGER : OC_COL_TEXT);
    human_bytes(s->attach_bytes, v, sizeof v);
    snprintf(both, sizeof both, "%llu file(s), %s", (unsigned long long)s->attach_count, v);
    draw_kv(rt, body, &y, "Attachments", both, OC_COL_TEXT);
    if (s->under_pressure)
        draw_kv(rt, body, &y, "State", "under pressure \u2014 uploads may be refused", OC_COL_DANGER);

    y += 10;
    draw_text(rt, "POLICY", g_small, rf(body.left + 24, y, body.right - 20, y + 18), OC_COL_FAINT);
    y += 22;
    if (s->max_age_days) snprintf(v, sizeof v, "expire after %llu day(s)", (unsigned long long)s->max_age_days);
    else                 snprintf(v, sizeof v, "kept indefinitely");
    draw_kv(rt, body, &y, "Attachments", v, OC_COL_TEXT);
    draw_kv(rt, body, &y, "Eviction under pressure",
            s->evict_enabled ? "on (oldest first)" : "off", OC_COL_TEXT);
    human_bytes(s->reserve_bytes, v, sizeof v);
    draw_kv(rt, body, &y, "Database reserve", v, OC_COL_TEXT);

    y += 10;
    draw_text(rt, "RECLAIMED SO FAR", g_small, rf(body.left + 24, y, body.right - 20, y + 18), OC_COL_FAINT);
    y += 22;
    snprintf(v, sizeof v, "%llu abandoned \u00b7 %llu expired \u00b7 %llu evicted",
             (unsigned long long)s->rec_orphan, (unsigned long long)s->rec_expired,
             (unsigned long long)s->rec_evicted);
    /* Evictions are the destructive reclaims — flagged once any have happened. */
    draw_kv(rt, body, &y, "Attachments", v, s->rec_evicted ? OC_COL_DANGER : OC_COL_TEXT);
}

static const char *audit_family(uint8_t f) {
    return f == 1 ? "admin" : f == 2 ? "account" : f == 3 ? "security" : f == 4 ? "moderation" : "";
}

static void draw_audit(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg, int embedded) {
    D2D1_RECT_F body = embedded ? reg : overlay_header(rt, reg, "Audit log");
    ovl_use(OVL_AUDIT);
    if (m->n_audit == 0) { overlay_empty(rt, body, "No audit entries."); return; }

    /* Family filter (WIN-19). Client-side over what has been paged in, which is
     * honest: it narrows what you are looking at, it does not re-query. */
    static const char *FAMS[5] = { "All", "Admin", "Account", "Security", "Moderation" };
    float fx = body.left + 20;
    g_n_audit_filters = 0;
    for (int f = 0; f < 5; f++) {
        float fw = text_width(FAMS[f], g_small) + 22;
        D2D1_RECT_F b = rf(fx, body.top + 6, fx + fw, body.top + 30);
        int on = (g_audit_family == f);
        fill_round(rt, b, 6.0f, on ? OC_COL_ACCENT : OC_COL_INPUT);
        if (!on) stroke_round(rt, b, 6.0f, OC_COL_BORDER, 1.0f);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, FAMS[f], g_small, b, on ? 0xFFFFFF : OC_COL_MUTED);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
        if (g_n_audit_filters < 5) g_audit_filters[g_n_audit_filters++] = b;
        fx += fw + 6;
    }
    body.top += 36;

    size_t shown = 0;
    for (size_t i = 0; i < m->n_audit; i++)
        if (!g_audit_family || m->audit[i].family == g_audit_family) shown++;
    if (shown == 0) { overlay_empty(rt, body, "Nothing in that category yet."); return; }

    float rowh = 46;
    float y = ovl_begin(rt, body, (float)shown * rowh + 34);
    for (size_t i = 0; i < m->n_audit; i++) {
        const oc_audit_view *a = &m->audit[i];
        if (g_audit_family && a->family != g_audit_family) continue;
        if (y + rowh < body.top) { y += rowh; continue; }
        if (y > body.bottom) break;
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
    /* Scrolling to the bottom pages older entries: the frame is timestamp-cursor
     * paged, but oc_client_audit_query(c, 0) was called once and never again. */
    if (g_ovl_max > 0.5f && g_ovl_scroll >= g_ovl_max - 1.0f)
        draw_text(rt, "Loading older entries\u2026", g_small,
                  rf(body.left + 20, y + 4, body.right - 20, y + 26), OC_COL_FAINT);
    ovl_end(rt, body);
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
    draw_text(rt, "Click a webhook to delete it \u2014 you will be asked to confirm.", g_small,
              rf(body.left + 20, body.top + 4, body.right - 16, body.top + 24), OC_COL_FAINT);
    body.top += 28;
    ovl_use(OVL_WEB);
    float rowh = 40;
    float y = ovl_begin(rt, body, (float)m->n_webhooks * rowh);
    for (size_t i = 0; i < m->n_webhooks; i++) {
        const oc_webhook_view *wv = &m->webhooks[i];
        if (y + rowh < body.top) { y += rowh; continue; }
        if (y > body.bottom) break;
        draw_text(rt, wv->label, g_ui, rf(body.left + 20, y, body.right - 120, y + 24),
                  wv->disabled ? OC_COL_MUTED : OC_COL_TEXT);
        /* State as a chip rather than a "(disabled)" suffix, so an enabled hook
         * is positively marked instead of merely lacking a word. */
        const char *st = wv->disabled ? "disabled" : "active";
        float cw = text_width(st, g_small) + 18;
        D2D1_RECT_F chip = rf(body.right - 24 - cw, y + 2, body.right - 24, y + 24);
        fill_round(rt, chip, 10.0f, OC_COL_INPUT);
        stroke_round(rt, chip, 10.0f, wv->disabled ? OC_COL_BORDER : OC_COL_ONLINE, 1.0f);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, st, g_small, chip, wv->disabled ? OC_COL_FAINT : OC_COL_ONLINE);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
        /* No created-date column: WEBHOOK_LIST carries id/channel/label/disabled
         * and nothing else, so there is no date to show without a wire change. */
        fill(rt, rf(body.left + 20, y + rowh - 1, body.right - 20, y + rowh), OC_COL_BORDER);
        if (g_n_webrows < (int)(sizeof g_webrows / sizeof g_webrows[0])) {
            g_webrows[g_n_webrows].top = y; g_webrows[g_n_webrows].bot = y + rowh;
            g_webrows[g_n_webrows].wid = wv->webhook_id; g_n_webrows++;
        }
        y += rowh;
    }
    ovl_end(rt, body);
}

/* WIN-12: every channel's notification level, editable in place. The prefs are
 * server-synced (REQ-130/131) and were reachable only one channel at a time from
 * a context menu, so there was no way to review them. */
static void draw_notify_prefs(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    D2D1_RECT_F body = overlay_header(rt, reg, "Notifications");
    ovl_use(OVL_NOTIFY);
    g_n_notify_hits = 0;

    char dnd[96];
    if (m->dnd_enabled)
        snprintf(dnd, sizeof dnd, "Do not disturb %02u:%02u \u2013 %02u:%02u",
                 m->dnd_start_min / 60, m->dnd_start_min % 60,
                 m->dnd_end_min / 60, m->dnd_end_min % 60);
    else
        snprintf(dnd, sizeof dnd, "Do not disturb is off");
    draw_text(rt, dnd, g_small, rf(body.left + 20, body.top + 4, body.right - 20, body.top + 26),
              m->dnd_enabled ? OC_COL_NOTICE : OC_COL_FAINT);
    body.top += 30;

    static const char *LEVELS[3] = { "All", "Mentions", "None" };
    float rowh = 38;
    float y = ovl_begin(rt, body, (float)m->n_channels * rowh);
    for (size_t i = 0; i < m->n_channels; i++) {
        const oc_channel *c = &m->channels[i];
        if (y + rowh < body.top) { y += rowh; continue; }
        if (y > body.bottom) break;
        char label[128];
        channel_label(m, c, label, sizeof label);
        draw_text(rt, label, g_ui, rf(body.left + 20, y, body.right - 260, y + rowh), OC_COL_TEXT);
        float bx = body.right - 24;
        for (int L = 2; L >= 0; L--) {
            float bw = text_width(LEVELS[L], g_small) + 20;
            D2D1_RECT_F b = rf(bx - bw, y + 5, bx, y + 29);
            int on = (c->notify_level == L);
            fill_round(rt, b, 6.0f, on ? OC_COL_ACCENT : OC_COL_INPUT);
            if (!on) stroke_round(rt, b, 6.0f, OC_COL_BORDER, 1.0f);
            IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
            draw_text(rt, LEVELS[L], g_small, b, on ? 0xFFFFFF : OC_COL_MUTED);
            IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
            if (g_n_notify_hits < 128) {
                g_notify_hits[g_n_notify_hits].r = b;
                g_notify_hits[g_n_notify_hits].cid = c->channel_id;
                g_notify_hits[g_n_notify_hits].level = (uint8_t)L;
                g_n_notify_hits++;
            }
            bx = b.left - 6;
        }
        fill(rt, rf(body.left + 20, y + rowh - 1, body.right - 20, y + rowh), OC_COL_BORDER);
        y += rowh;
    }
    ovl_end(rt, body);
}

static void layout_composer(HWND hwnd);   /* fwd */

/* Move `delta` conversations through the sidebar as it is currently shown, so
 * the order matches what the eye sees — the section order, sort and filter all
 * apply. `unread_only` skips everything already read, which is the "next thing
 * that wants me" move rather than plain next.
 *
 * Reuses the same core helper the sidebar draws from; walking m->channels
 * directly would drift from the visible order the moment a sort changed. */
static void nav_conversation(HWND hwnd, int delta, int unread_only) {
    const oc_model *m = model();
    if (!m || !m->n_channels) return;
    oc_sidebar_opts o = g_sb;
    if (g_view == VIEW_DMS) o.collapsed[OC_SB_CHANNELS] = 1;
    snprintf(o.find, sizeof o.find, "%s", g_find_filter);
    size_t cap = m->n_channels + OC_SB_SECTIONS + 2;
    oc_sidebar_row *rows = malloc(cap * sizeof *rows);
    if (!rows) return;
    size_t n = oc_model_sidebar(m, &o, rows, cap);

    /* Collapse to selectable rows only: headers are not destinations. */
    uint64_t ids[256]; int nids = 0, cur = -1;
    for (size_t i = 0; i < n && nids < 256; i++) {
        if (rows[i].is_header || !rows[i].channel_id) continue;
        if (unread_only && rows[i].unread <= 0 && rows[i].channel_id != g_sel) continue;
        if (rows[i].channel_id == g_sel) cur = nids;
        ids[nids++] = rows[i].channel_id;
    }
    free(rows);
    if (nids == 0) return;

    int next;
    if (cur < 0) next = (delta > 0) ? 0 : nids - 1;
    else         next = ((cur + delta) % nids + nids) % nids;   /* wrap */
    if (ids[next] == g_sel && nids == 1) return;
    select_channel(ids[next]);
    layout_composer(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* WIN-25: the shortcut sheet, generated from one table so it cannot drift from
 * what the key handlers actually do. */
static const struct { const char *keys, *what; } KEYMAP[] = {
    { "Enter",              "Send the message" },
    { "Shift+Enter",        "New line" },
    { "Esc",                "Close the open pane, popover or picker" },
    { "Tab",                "Insert the highlighted completion" },
    { "Up / Down",          "Move through completions" },
    { "Alt+Up / Alt+Down",  "Previous / next conversation" },
    { "Alt+Shift+Up / Down","Previous / next UNREAD conversation" },
    { "Ctrl+K",             "Command palette" },
    { "Ctrl+F",             "Search messages" },
    { "Ctrl+/",             "This list" },
    { "F6",                 "Move focus between the composer and the filter box" },
    { "Mouse wheel",        "Scroll the transcript, sidebar or open pane" },
    { "Right-click",        "Actions for a message, member or channel" },
};

static void draw_keys(ID2D1RenderTarget *rt, D2D1_RECT_F reg) {
    D2D1_RECT_F body = overlay_header(rt, reg, "Keyboard shortcuts");
    ovl_use(OVL_KEYS);
    int n = (int)(sizeof KEYMAP / sizeof KEYMAP[0]);
    float rowh = 32;
    float y = ovl_begin(rt, body, (float)n * rowh + 12);
    for (int i = 0; i < n; i++) {
        if (y > body.bottom) break;
        draw_text(rt, KEYMAP[i].keys, g_ui_b, rf(body.left + 24, y, body.left + 200, y + rowh), OC_COL_TEXT);
        draw_text(rt, KEYMAP[i].what, g_ui, rf(body.left + 210, y, body.right - 24, y + rowh), OC_COL_MUTED);
        y += rowh;
    }
    ovl_end(rt, body);
}


/* A channel's pinned messages (REQ-230). Each row is the message itself, so the
 * list is readable without jumping — and clicking still jumps, because a pin is
 * usually a pointer into a conversation rather than the whole of it. */
/* The About tab (REQ-034/035/036): what the channel *is*, and the three ways to
 * change it. Rename and archive are owner/admin (ARCH-93); the buttons are
 * hidden for a member rather than shown-and-refused, because an affordance you
 * are not allowed to use is worse than no affordance — but the daemon enforces
 * it regardless, so hiding is courtesy, not security. */
static void draw_about(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    g_about_topic = g_about_rename = g_about_archive = g_about_hooks = rf(0, 0, 0, 0);
    const oc_channel *c = oc_model_channel((oc_model *)m, g_sel);
    if (!c) { overlay_empty(rt, reg, "No channel."); return; }

    float x = reg.left + 24, w = reg.right - reg.left - 48, y = reg.top + 20;
    char line[320];

    snprintf(line, sizeof line, "%s%s", c->kind == OC_CHANNEL_KIND_DM ? "@ " :
             (c->is_public ? "# " : "\U0001F512 "), c->name ? c->name : "");
    draw_text(rt, line, g_hdr, rf(x, y, x + w, y + 30), OC_COL_TEXT);
    y += 34;

    if (c->archived) {
        D2D1_RECT_F badge = rf(x, y, x + text_width("Archived \u00B7 read-only", g_small) + 22, y + 24);
        fill_round(rt, badge, 6.0f, OC_COL_INPUT);
        stroke_round(rt, badge, 6.0f, OC_COL_BORDER, 1.0f);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, "Archived \u00B7 read-only", g_small, badge, OC_COL_AWAY);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
        y += 32;
    }

    draw_text(rt, "TOPIC", g_small, rf(x, y, x + w, y + 20), OC_COL_FAINT);
    y += 22;
    draw_text(rt, (c->topic && c->topic[0]) ? c->topic : "No topic set.", g_ui,
              rf(x, y, x + w - 120, y + 44),
              (c->topic && c->topic[0]) ? OC_COL_TEXT : OC_COL_FAINT);
    g_about_topic = rf(reg.right - 24 - 110, y - 4, reg.right - 24, y + 24);
    stroke_round(rt, g_about_topic, 6.0f, OC_COL_BORDER, 1.0f);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(rt, "Set topic\u2026", g_small, g_about_topic, OC_COL_MUTED);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
    y += 56;

    /* Facts worth having in one place, none of which needed a new query. */
    const char *nm = oc_model_user_name((oc_model *)m, 0);
    (void)nm;
    if (m->chanmem_channel == g_sel && !m->chanmem_loading) {
        snprintf(line, sizeof line, "%zu member%s", m->n_chanmem, m->n_chanmem == 1 ? "" : "s");
        draw_text(rt, line, g_small, rf(x, y, x + w, y + 20), OC_COL_MUTED);
        y += 22;
    }
    if (c->created_at) {
        time_t t = (time_t)(c->created_at / 1000);
        struct tm tv; char when[40] = "";
        if (oc_localtime_r(&t, &tv)) strftime(when, sizeof when, "%d %b %Y", &tv);
        snprintf(line, sizeof line, "Created %s", when);
        draw_text(rt, line, g_small, rf(x, y, x + w, y + 20), OC_COL_MUTED);
        y += 22;
    }
    y += 16;

    if (c->kind != OC_CHANNEL_KIND_DM && self_role(m) >= OC_ROLE_ADMIN) {
        fill(rt, rf(x, y, x + w, y + 1), OC_COL_BORDER);
        y += 18;
        draw_text(rt, "ADMIN", g_small, rf(x, y, x + w, y + 20), OC_COL_FAINT);
        y += 24;
        /* Webhooks are channel-scoped admin, so the channel's own settings page
         * is where they belong — they were reachable only from a right-click
         * menu, which is not somewhere anyone looks for configuration. */
        g_about_hooks = rf(x + 320, y, x + 440, y + 28);
        stroke_round(rt, g_about_hooks, 6.0f, OC_COL_BORDER, 1.0f);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, "Webhooks\u2026", g_small, g_about_hooks, OC_COL_MUTED);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);

        g_about_rename = rf(x, y, x + 150, y + 28);
        stroke_round(rt, g_about_rename, 6.0f, OC_COL_BORDER, 1.0f);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, "Rename channel\u2026", g_small, g_about_rename, OC_COL_MUTED);
        g_about_archive = rf(x + 160, y, x + 310, y + 28);
        stroke_round(rt, g_about_archive, 6.0f, OC_COL_BORDER, 1.0f);
        draw_text(rt, c->archived ? "Unarchive" : "Archive channel", g_small,
                  g_about_archive, c->archived ? OC_COL_NOTICE : OC_COL_DANGER);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
        y += 36;
        draw_text(rt, c->archived
                      ? "Unarchiving makes the channel writable again."
                      : "Archiving makes it read-only and hides it from people who are not in it. "
                        "History stays searchable, and it can be undone.",
                  g_small_w, rf(x, y, x + w, y + 56), OC_COL_FAINT);
    }
}

static void draw_pinlist(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    /* No overlay header: the tab strip above already says where you are, and
     * two titles stacked read as a bug. */
    D2D1_RECT_F body = reg;
    g_n_pinrows = 0;
    if (m->pinlist_loading) { overlay_empty(rt, body, "Loading\u2026"); return; }
    if (m->n_pins == 0) {
        overlay_empty(rt, body,
                      "Nothing pinned yet. Pin a message from its \u22EF menu.");
        return;
    }
    float y = body.top + 8;
    for (size_t i = 0; i < m->n_pins && y < body.bottom; i++) {
        const oc_pinned_row *pr = &m->pins[i];
        float rh = 58;
        D2D1_RECT_F row = rf(body.left + 12, y, body.right - 12, y + rh - 6);
        if (g_hover_pinrow == pr->message_id) fill_round(rt, row, 6.0f, OC_COL_HOVER);

        const char *who = oc_model_user_name((oc_model *)m, pr->author_id);
        char head[192];
        char when[24] = "";
        if (pr->server_time) {
            time_t t = (time_t)(pr->server_time / 1000);
            struct tm tv;
            if (oc_localtime_r(&t, &tv))
                strftime(when, sizeof when, g_pref_time24 ? "%H:%M" : "%I:%M %p", &tv);
        }
        snprintf(head, sizeof head, "%s  %s", (who && who[0]) ? who : "user", when);
        draw_text(rt, head, g_name, rf(row.left + 10, y + 4, row.right - 90, y + 24), OC_COL_TEXT);
        /* An attachment-only message has no body; naming the file is the only
         * thing that makes such a row mean anything. */
        char prev[256];
        if (pr->body && pr->body[0]) snprintf(prev, sizeof prev, "%s", pr->body);
        else if (pr->attach_name[0]) snprintf(prev, sizeof prev, "\U0001F4CE %s", pr->attach_name);
        else                         snprintf(prev, sizeof prev, "%s", "");
        draw_text(rt, prev, g_ui,
                  rf(row.left + 10, y + 24, row.right - 90, y + 46), OC_COL_MUTED);

        /* Who pinned it, on the same line as the unpin — the attribution and
         * the action that undoes it belong together. */
        const char *pby = oc_model_user_name((oc_model *)m, pr->pinned_by);
        if (pby && pby[0]) {
            char lbl[96];
            snprintf(lbl, sizeof lbl, "pinned by %s", pby);
            IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_TRAILING);
            draw_text(rt, lbl, g_small, rf(row.left, y + 4, row.right - 12, y + 22), OC_COL_FAINT);
            IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        D2D1_RECT_F un = rf(row.right - 74, y + 24, row.right - 12, y + 46);
        stroke_round(rt, un, 6.0f, OC_COL_BORDER, 1.0f);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, "Unpin", g_small, un, OC_COL_MUTED);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);

        if (g_n_pinrows < (int)(sizeof g_pinrows / sizeof g_pinrows[0])) {
            g_pinrows[g_n_pinrows].row   = row;
            g_pinrows[g_n_pinrows].unpin = un;
            g_pinrows[g_n_pinrows].mid   = pr->message_id;
            g_n_pinrows++;
        }
        y += rh;
    }
}

/* Files shared in this channel (REQ-143, ARCH-91). Deliberately a flat,
 * newest-first list rather than a grid: most of what gets shared here is not an
 * image, and a grid of generic file glyphs is worse than a line of names. */
/* File-type filter (REQ-143). Client-side over the `mime` the wire already
 * carries — the server returns the newest 200 and the user narrows what they are
 * looking at, which is honest: it filters the page, it does not re-query. */
enum { FF_ALL = 0, FF_IMAGES, FF_DOCS, FF_OTHER, FF_KINDS };
static int g_file_filter;
static D2D1_RECT_F g_file_filters[FF_KINDS];
/* Scope is a separate axis from type — Slack splits them the same way, because
 * "spreadsheets" and "things I shared" are different questions. */
enum { FS_ALL = 0, FS_MINE, FS_THEIRS, FS_SCOPES };
static int g_file_scope;
static D2D1_RECT_F g_file_scopes[FS_SCOPES];

static int file_kind(const char *mime) {
    if (mime_is_image(mime)) return FF_IMAGES;
    if (!mime) return FF_OTHER;
    if (!strncmp(mime, "text/", 5) || strstr(mime, "pdf") || strstr(mime, "word") ||
        strstr(mime, "sheet") || strstr(mime, "presentation") || strstr(mime, "document"))
        return FF_DOCS;
    return FF_OTHER;
}

/* A coloured badge per family, the way every file browser worth using does it:
 * the extension is the fastest thing to scan for, so give it colour and a shape
 * rather than making every row the same grey page glyph. */
static void file_badge(ID2D1RenderTarget *rt, const oc_file_view *f, D2D1_RECT_F r) {
    const char *ext = strrchr(f->filename, '.');
    char tag[6] = "FILE";
    uint32_t col = 0x5B6270;                       /* generic */
    if (mime_is_image(f->mime))                     { col = 0x8B5CF6; snprintf(tag, sizeof tag, "IMG"); }
    else if (ext && !_stricmp(ext, ".pdf"))       { col = 0xD64545; snprintf(tag, sizeof tag, "PDF"); }
    else if (ext && (!_stricmp(ext, ".doc") || !_stricmp(ext, ".docx")))
                                                    { col = 0x2B5CE6; snprintf(tag, sizeof tag, "DOC"); }
    else if (ext && (!_stricmp(ext, ".xls") || !_stricmp(ext, ".xlsx") || !_stricmp(ext, ".csv")))
                                                    { col = 0x1E8E4E; snprintf(tag, sizeof tag, "XLS"); }
    else if (ext && (!_stricmp(ext, ".zip") || !_stricmp(ext, ".gz") || !_stricmp(ext, ".7z")))
                                                    { col = 0xB2802E; snprintf(tag, sizeof tag, "ZIP"); }
    else if (ext && !_stricmp(ext, ".txt"))       { col = 0x4B7A9B; snprintf(tag, sizeof tag, "TXT"); }
    if (f->reclaimed) col = OC_COL_FAINT;
    fill_round(rt, r, 6.0f, col);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(rt, tag, g_small, rf(r.left, r.top + 2, r.right, r.bottom), 0xFFFFFF);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
}

static int file_passes(const oc_model *m, const oc_file_view *f) {
    if (g_file_filter != FF_ALL && file_kind(f->mime) != g_file_filter) return 0;
    if (g_file_scope == FS_MINE   && f->uploader_id != m->user_id) return 0;
    if (g_file_scope == FS_THEIRS && f->uploader_id == m->user_id) return 0;
    return 1;
}

/* The filter chips. Returns the y below them. */
static float draw_file_filters(ID2D1RenderTarget *rt, D2D1_RECT_F body) {
    static const char *L[FF_KINDS] = { "All", "Images", "Documents", "Other" };
    float fx = body.left + 20, y = body.top + 8;
    for (int i = 0; i < FF_KINDS; i++) {
        float fw = text_width(L[i], g_small) + 22;
        D2D1_RECT_F b = rf(fx, y, fx + fw, y + 24);
        int on = (g_file_filter == i);
        fill_round(rt, b, 6.0f, on ? OC_COL_ACCENT : OC_COL_INPUT);
        if (!on) stroke_round(rt, b, 6.0f, OC_COL_BORDER, 1.0f);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, L[i], g_small, b, on ? 0xFFFFFF : OC_COL_MUTED);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
        g_file_filters[i] = b;
        fx = b.right + 6;
    }
    /* Scope on the same row, right-aligned: a different question from type. */
    static const char *SC[FS_SCOPES] = { "Everyone", "Shared by you", "Shared with you" };
    float rx = body.right - 20;
    for (int i = FS_SCOPES - 1; i >= 0; i--) {
        float fw = text_width(SC[i], g_small) + 22;
        D2D1_RECT_F b = rf(rx - fw, y, rx, y + 24);
        int on = (g_file_scope == i);
        fill_round(rt, b, 6.0f, on ? OC_COL_SELECT : OC_COL_INPUT);
        if (!on) stroke_round(rt, b, 6.0f, OC_COL_BORDER, 1.0f);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, SC[i], g_small, b, on ? OC_COL_TEXT : OC_COL_MUTED);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
        g_file_scopes[i] = b;
        rx = b.left - 6;
    }
    return y + 32;
}

/* One list of files, used by both the channel's Files tab and the workspace-wide
 * Files view. `show_channel` adds which channel each file came from — essential
 * across channels, noise inside one. */
static void draw_file_rows(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F body,
                           float y, int show_channel) {
    g_n_filerows = 0;
    int shown = 0;
    for (size_t i = 0; i < m->n_files && y < body.bottom; i++) {
        const oc_file_view *f = &m->files[i];
        if (!file_passes(m, f)) continue;
        shown++;
        D2D1_RECT_F row = rf(body.left + 12, y, body.right - 12, y + 46);
        if (g_hover_filerow == f->id) fill_round(rt, row, 6.0f, OC_COL_HOVER);

        file_badge(rt, f, rf(row.left + 8, y + 8, row.left + 42, y + 38));
        draw_text(rt, f->filename, g_ui, rf(row.left + 52, y + 4, row.right - 100, y + 24),
                  f->reclaimed ? OC_COL_FAINT : OC_COL_TEXT);

        /* Uploader, size, date — and the channel when this list spans them. A
         * reclaimed row says so rather than offering a download that cannot
         * work (REQ-215/217). */
        const char *who = oc_model_user_name((oc_model *)m, f->uploader_id);
        char sub[256], when[24] = "", sz[32], chan[80] = "";
        if (f->created_at) {
            time_t t = (time_t)(f->created_at / 1000);
            struct tm tv;
            if (oc_localtime_r(&t, &tv)) strftime(when, sizeof when, "%d %b", &tv);
        }
        if (f->size >= 1024 * 1024) snprintf(sz, sizeof sz, "%.1f MB", (double)f->size / (1024 * 1024));
        else if (f->size >= 1024)   snprintf(sz, sizeof sz, "%.0f KB", (double)f->size / 1024);
        else                        snprintf(sz, sizeof sz, "%llu B", (unsigned long long)f->size);
        if (show_channel) {
            const oc_channel *fc = oc_model_channel((oc_model *)m, f->channel_id);
            if (fc && fc->name && fc->name[0]) snprintf(chan, sizeof chan, "#%s \u00B7 ", fc->name);
        }
        snprintf(sub, sizeof sub, "%s%s \u00B7 %s \u00B7 %s%s", chan,
                 (who && who[0]) ? who : "someone", sz, when,
                 f->reclaimed ? "  \u00B7 no longer stored" : "");
        draw_text(rt, sub, g_small, rf(row.left + 52, y + 24, row.right - 100, y + 42),
                  OC_COL_FAINT);

        if (!f->reclaimed) {
            D2D1_RECT_F dl = rf(row.right - 86, y + 12, row.right - 12, y + 34);
            stroke_round(rt, dl, 6.0f, OC_COL_BORDER, 1.0f);
            IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
            draw_text(rt, "Download", g_small, dl, OC_COL_MUTED);
            IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
            if (g_n_filerows < (int)(sizeof g_filerows / sizeof g_filerows[0])) {
                g_filerows[g_n_filerows].row = row;
                g_filerows[g_n_filerows].dl  = dl;
                g_filerows[g_n_filerows].ix  = (int)i;
                g_n_filerows++;
            }
        }
        y += 50;
    }
    if (!shown && m->n_files)
        draw_text(rt, "Nothing of that type here.", g_small,
                  rf(body.left + 20, y + 6, body.right - 20, y + 26), OC_COL_FAINT);
    /* The server caps the response; saying so beats a list that silently stops. */
    if (m->n_files >= OC_MAX_FILE_LIST && y < body.bottom)
        draw_text(rt, "Showing the most recent 200. Older files are in search.", g_small,
                  rf(body.left + 20, y + 8, body.right - 20, y + 28), OC_COL_FAINT);
}

static void draw_filelist(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    D2D1_RECT_F body = reg;
    g_n_filerows = 0;
    if (m->filelist_loading) { overlay_empty(rt, body, "Loading\u2026"); return; }
    if (m->n_files == 0) {
        overlay_empty(rt, body, "No files shared in this channel yet.");
        return;
    }
    float y = draw_file_filters(rt, body);
    draw_file_rows(rt, m, body, y, 0);
}

/* The workspace-wide Files view (rail). The same list with `channel_id 0`, which
 * the daemon already answers as "every channel I can read" — so this view cost a
 * fetch and a header, not a protocol change. */
static void draw_files_view(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    fill(rt, rf(reg.left, reg.top, reg.right, reg.top + HEADER_H), OC_COL_HEADER);
    draw_text(rt, "Files", g_hdr, rf(reg.left + 20, reg.top, reg.right - 20, reg.top + HEADER_H),
              OC_COL_TEXT);
    fill(rt, rf(reg.left, reg.top + HEADER_H - 1, reg.right, reg.top + HEADER_H), OC_COL_BORDER);
    D2D1_RECT_F body = rf(reg.left, reg.top + HEADER_H, reg.right, reg.bottom);

    g_n_filerows = 0;
    if (m->filelist_loading) { overlay_empty(rt, body, "Loading\u2026"); return; }
    if (m->n_files == 0) {
        overlay_empty(rt, body, "No files shared anywhere you can see yet.");
        return;
    }
    float y = draw_file_filters(rt, body);
    draw_file_rows(rt, m, body, y, 1);
}

/* One preference row: a label, a sub-label, and a segmented set of choices on
 * the right. Returns the y for the next row. */
static float pref_row(ID2D1RenderTarget *rt, D2D1_RECT_F body, float y, int row,
                      const char *label, const char *hint,
                      const char *const *opts, int n_opts, int cur) {
    draw_text(rt, label, g_ui_b, rf(body.left + 24, y, body.left + 320, y + 22), OC_COL_TEXT);
    if (hint && hint[0])
        /* Run to where the choice buttons start, not a magic 340: the hint was
         * clipped mid-word ("outside Do Not Dis…") at any pane width. */
        draw_text(rt, hint, g_small, rf(body.left + 24, y + 20, body.right - 210, y + 40), OC_COL_FAINT);

    float bx = body.right - 24;
    for (int i = n_opts - 1; i >= 0; i--) {
        float w = text_width(opts[i], g_small) + 26;
        D2D1_RECT_F b = rf(bx - w, y + 2, bx, y + 28);
        int on = (i == cur);
        fill_round(rt, b, 6.0f, on ? OC_COL_ACCENT : OC_COL_INPUT);
        if (!on) stroke_round(rt, b, 6.0f, OC_COL_BORDER, 1.0f);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, opts[i], g_small, b, on ? 0xFFFFFF : OC_COL_MUTED);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
        if (g_n_pref_hits < 16) {
            g_pref_hits[g_n_pref_hits].r = b;
            g_pref_hits[g_n_pref_hits].row = row;
            g_pref_hits[g_n_pref_hits].val = i;
            g_n_pref_hits++;
        }
        bx = b.left - 6;
    }
    fill(rt, rf(body.left + 24, y + 46, body.right - 24, y + 47), OC_COL_BORDER);
    return y + 62;
}

enum { PREF_ROW_THEME = 0, PREF_ROW_TIME, PREF_ROW_MEMBERS, PREF_ROW_DAYSEP,
       PREF_ROW_NOTIFY, PREF_ROW_QUICK };

static void draw_prefs(ID2D1RenderTarget *rt, D2D1_RECT_F reg) {
    D2D1_RECT_F body = overlay_header(rt, reg, "Preferences");
    g_n_pref_hits = 0;
    float y = body.top + 16;

    static const char *THEMES[3] = { "Dark", "Light", "System" };
    static const char *TIMES[2]  = { "12-hour", "24-hour" };
    static const char *ONOFF[2]  = { "Off", "On" };

    y = pref_row(rt, body, y, PREF_ROW_THEME, "Appearance",
                 "System follows the Windows app theme.", THEMES, 3, oc_theme_mode());
    y = pref_row(rt, body, y, PREF_ROW_TIME, "Time format",
                 "How message timestamps are shown.", TIMES, 2, g_pref_time24);
    y = pref_row(rt, body, y, PREF_ROW_MEMBERS, "Members pane",
                 "Shown by default when you open a channel.", ONOFF, 2, g_pref_members);
    y = pref_row(rt, body, y, PREF_ROW_DAYSEP, "Date dividers",
                 "A separator between days in the transcript.", ONOFF, 2, g_pref_daysep);
    static const char *NOTIF[3] = { "Off", "Count", "Preview" };
    y = pref_row(rt, body, y, PREF_ROW_NOTIFY, "Desktop notifications",
                 "When OpenChime is not in front, and outside Do Not Disturb.",
                 NOTIF, 3, g_pref_notify);

    /* WIN-28: the quick reactions were six literals in the source. */
    {
        static const char *EDIT1[1] = { "Change\u2026" };
        char cur[128] = "";
        for (int i = 0; i < g_n_quick; i++)
            snprintf(cur + strlen(cur), sizeof cur - strlen(cur), "%s ", REACT_EMO[i]);
        draw_text(rt, "Quick reactions", g_ui_b, rf(body.left + 24, y, body.left + 320, y + 22), OC_COL_TEXT);
        draw_emoji_fmt(rt, "", rf(0, 0, 0, 0), g_emoji_s);      /* keep the format warm */
        draw_text(rt, cur, g_emoji_s, rf(body.left + 24, y + 20, body.left + 340, y + 42), OC_COL_TEXT);
        y = pref_row(rt, body, y, PREF_ROW_QUICK, "", "", EDIT1, 1, -1);
    }

    draw_text(rt, "Saved to your account, so they follow you to another machine.",
              g_small, rf(body.left + 24, y + 4, body.right - 24, y + 24), OC_COL_FAINT);
}

/* A person's card, in the context pane (right). Laid out VERTICALLY: the old
 * version was a wide avatar-beside-text block built for the full middle column,
 * which does not fit 300px and would fit less as REQ-240/241 add fields. */
static void draw_profile_card(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    const char *nm = oc_model_user_name(m, g_profile_uid);
    if (!nm || !nm[0]) nm = "user";
    float cx = (reg.left + reg.right) / 2, y = reg.top + 18;

    D2D1_ELLIPSE av = { { cx, y + 36 }, 36, 36 };
    ID2D1RenderTarget_FillEllipse(rt, &av, paint_with(AVPAL[g_profile_uid % 6]));
    char ini[2] = { (char)(nm[0] >= 'a' && nm[0] <= 'z' ? nm[0] - 32 : nm[0]), 0 };
    IDWriteTextFormat_SetTextAlignment(g_hdr, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(rt, ini, g_hdr, rf(cx - 36, y, cx + 36, y + 72), 0xFFFFFF);
    y += 84;

    draw_text(rt, nm, g_hdr, rf(reg.left + 12, y, reg.right - 12, y + 28), OC_COL_TEXT);
    IDWriteTextFormat_SetTextAlignment(g_hdr, DWRITE_TEXT_ALIGNMENT_LEADING);
    y += 30;

    uint8_t pres = oc_model_presence_of(m, g_profile_uid);
    const char *pl = pres == OC_PRESENCE_ONLINE ? "Active"
                   : pres == OC_PRESENCE_AWAY   ? "Away" : "Offline";
    uint8_t role = OC_ROLE_MEMBER;
    int known = 0;
    for (size_t i = 0; i < m->n_users; i++)
        if (m->users[i].user_id == g_profile_uid) { role = m->users[i].role; known = 1; break; }
    char sub[96];
    const char *rl = role_label(role);
    if (known && rl[0]) snprintf(sub, sizeof sub, "%s \u00B7 %s", pl, rl);
    else                snprintf(sub, sizeof sub, "%s", pl);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(rt, sub, g_small, rf(reg.left + 12, y, reg.right - 12, y + 20), OC_COL_MUTED);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
    y += 34;

    if (g_profile_uid != m->user_id) {
        g_prof_dm_btn = rf(reg.left + 24, y, reg.right - 24, y + 32);
        fill_round(rt, g_prof_dm_btn, 7.0f, OC_COL_ACCENT);
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, "Message", g_ui, g_prof_dm_btn, 0xFFFFFF);
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_LEADING);
        y += 44;
    } else {
        g_prof_dm_btn = rf(0, 0, 0, 0);
        draw_text(rt, "This is you.", g_small,
                  rf(reg.left + 24, y, reg.right - 12, y + 20), OC_COL_FAINT);
        y += 28;
    }
    /* The fields a real profile wants (REQ-240/241) do not exist yet; saying so
     * is better than a card that looks finished and is not. */
    draw_text(rt, "No title, timezone or email yet \u2014 those fields are not built.",
              g_small_w, rf(reg.left + 16, y + 6, reg.right - 16, y + 56), OC_COL_FAINT);
}

/* Who reacted (REQ-071) — a list of PEOPLE, so it belongs in the context pane
 * beside the conversation rather than replacing it. */
static void draw_reactors_list(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    if (m->n_reactors == 0) {
        draw_text(rt, "No reactions.", g_small,
                  rf(reg.left + 16, reg.top + 8, reg.right - 12, reg.top + 30), OC_COL_FAINT);
        return;
    }
    float y = reg.top + 6;
    for (size_t i = 0; i < m->n_reactors && y < reg.bottom; i++) {
        const oc_reactor_row *rr = &m->reactors[i];
        draw_emoji_fmt(rt, rr->emoji, rf(reg.left + 14, y, reg.left + 38, y + ROW_H), g_emoji_s);
        const char *nm = oc_model_user_name(m, rr->user_id);
        draw_text(rt, (nm && nm[0]) ? nm : "user", g_ui,
                  rf(reg.left + 44, y, reg.right - 12, y + ROW_H), OC_COL_TEXT);
        y += ROW_H;
    }
}

static void sw_book_load(void);       /* fwd */

static void draw_wsmgr(ID2D1RenderTarget *rt, D2D1_RECT_F reg) {
    D2D1_RECT_F body = overlay_header(rt, reg, "Workspaces");
    g_n_wsmgr_hits = 0;
    draw_text(rt, "Sign out keeps a workspace here; Remove deletes it from this device.",
              g_small, rf(body.left + 24, body.top + 6, body.right - 24, body.top + 26), OC_COL_FAINT);
    float y = body.top + 34, rowh = 54;

    for (int i = 0; i < g_n_sw; i++) {
        int slot = ws_find(g_sw[i].ws);
        int live = (slot >= 0 && g_wss[slot].client);
        draw_text(rt, g_sw[i].label, g_ui_b, rf(body.left + 24, y, body.left + 300, y + 22), OC_COL_TEXT);
        /* State on the sub-line rather than its own column: as a column it
         * collided with the buttons whenever the pane was narrow. */
        const char *state = g_sw[i].current ? "current" : live ? "connected" : "signed out";
        uint32_t sc = g_sw[i].current ? OC_COL_ACCENT : live ? OC_COL_ONLINE : OC_COL_FAINT;
        draw_text(rt, state, g_small, rf(body.left + 24 + text_width(g_sw[i].label, g_ui_b) + 12,
                                         y, body.left + 340, y + 22), sc);
        char sub[320];
        snprintf(sub, sizeof sub, "%s%s%s", g_sw[i].ws,
                 g_sw[i].user[0] ? "  \u00b7  " : "", g_sw[i].user);
        draw_text(rt, sub, g_small, rf(body.left + 24, y + 20, body.left + 340, y + 40), OC_COL_FAINT);

        /* Buttons right-aligned, built right-to-left so widths can vary. */
        float bx = body.right - 24;
        struct { const char *lbl; int act; uint32_t col; } B[2];
        int nb = 0;
        B[nb].lbl = "Remove"; B[nb].act = WSM_FORGET; B[nb].col = OC_COL_DANGER; nb++;
        if (live && !g_sw[i].current) { B[nb].lbl = "Switch to"; B[nb].act = WSM_GO; B[nb].col = OC_COL_MUTED; nb++; }
        else if (live)                { B[nb].lbl = "Sign out";  B[nb].act = WSM_SIGNOUT; B[nb].col = OC_COL_MUTED; nb++; }
        else                          { B[nb].lbl = "Sign in";   B[nb].act = WSM_GO; B[nb].col = OC_COL_MUTED; nb++; }
        for (int k = 0; k < nb; k++) {
            float bw = text_width(B[k].lbl, g_small) + 24;
            D2D1_RECT_F b = rf(bx - bw, y + 8, bx, y + 34);
            fill_round(rt, b, 6.0f, OC_COL_INPUT);
            stroke_round(rt, b, 6.0f, OC_COL_BORDER, 1.0f);
            IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
            draw_text(rt, B[k].lbl, g_small, b, B[k].col);
            IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
            if (g_n_wsmgr_hits < 48) {
                g_wsmgr_hits[g_n_wsmgr_hits].r = b;
                g_wsmgr_hits[g_n_wsmgr_hits].row = i;
                g_wsmgr_hits[g_n_wsmgr_hits].act = B[k].act;
                g_n_wsmgr_hits++;
            }
            bx = b.left - 8;
        }
        fill(rt, rf(body.left + 24, y + rowh - 1, body.right - 24, y + rowh), OC_COL_BORDER);
        y += rowh;
    }
    if (g_n_sw == 0) overlay_empty(rt, body, "No workspaces remembered on this device.");
}

static void draw_transcript(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    if (!m->authed) {
        /* The reason lives in the banner above (draw_banner) — repeating it here
         * put the same sentence on screen twice. This is just the empty state. */
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, "No conversation to show while you are offline.", g_ui, reg, OC_COL_FAINT);
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_LEADING);
        return;
    }
    if (m->thread_open)    { draw_thread(rt, m, reg);    return; }
    if (m->search_open)    { draw_search(rt, m, reg);    return; }
    if (m->pinlist_open)   { draw_pinlist(rt, m, reg);   return; }
    if (m->filelist_open)  { draw_filelist(rt, m, reg);  return; }
    if (g_tab == TAB_ABOUT) { draw_about(rt, m, reg);    return; }
    if (m->weblist_open)   { draw_weblist(rt, m, reg);   return; }
    if (m->storage_open)   { draw_storage(rt, m, reg, 0);   return; }
    if (m->audit_open)     { draw_audit(rt, m, reg, 0);     return; }

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
        draw_msglist(rt, m, c->msgs, c->n_msgs, lr, MSGLIST_MAIN);
        draw_text(rt, line, g_small, rf(reg.left + 72, reg.bottom - 20, reg.right - 20, reg.bottom),
                  OC_COL_FAINT);
        return;
    }
    draw_msglist(rt, m, c->msgs, c->n_msgs, reg, MSGLIST_MAIN);
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
    /* The topic sits on the header's second line, where Slack puts it — and it
     * yields to a typing indicator, which is transient and more urgent. */
    const char *topic = (c && c->topic && c->topic[0]) ? c->topic : NULL;
    if (c && c->archived) {
        char t2[200];
        snprintf(t2, sizeof t2, "%s  \u00B7  archived", title);
        snprintf(title, sizeof title, "%s", t2);
    }
    if (typing[0]) {
        draw_text(rt, title, g_hdr, rf(x0 + 20, 6, x0 + w - 240, 34), OC_COL_TEXT);
        draw_text(rt, typing, g_small, rf(x0 + 20, 32, x0 + w - 240, HEADER_H - 6), OC_COL_ACCENT);
    } else if (topic) {
        draw_text(rt, title, g_hdr, rf(x0 + 20, 6, x0 + w - 240, 34), OC_COL_TEXT);
        draw_text(rt, topic, g_small, rf(x0 + 20, 32, x0 + w - 240, HEADER_H - 6), OC_COL_MUTED);
    } else {
        draw_text(rt, title, g_hdr, rf(x0 + 20, 0, x0 + w - 240, HEADER_H), OC_COL_TEXT);
    }

    /* Member count chip (right), Slack's shape: an icon and a number rather than
     * the word "Members". The count is the CHANNEL's roster (REQ-031), not the
     * tenant's — which is what the pane beside it used to show. */
    char mc[16] = "";
    if (c && m->chanmem_channel == g_sel && !m->chanmem_loading)
        snprintf(mc, sizeof mc, "%u", (unsigned)m->n_chanmem);
    float cw = 30 + (mc[0] ? text_width(mc, g_small) + 4 : 0);
    g_memchip = rf(x0 + w - 16 - cw, 13, x0 + w - 16, HEADER_H - 13);
    if (g_show_members) fill_round(rt, g_memchip, 6.0f, OC_COL_SELECT);
    else                stroke_round(rt, g_memchip, 6.0f, OC_COL_BORDER, 1.0f);
    uint32_t mcol = g_show_members ? OC_COL_TEXT : OC_COL_MUTED;
    draw_lucide(rt, OC_ICON_USER, rf(g_memchip.left + 5, g_memchip.top + 4,
                                     g_memchip.left + 23, g_memchip.bottom - 4), mcol);
    if (mc[0])
        draw_text(rt, mc, g_small, rf(g_memchip.left + 25, g_memchip.top,
                                      g_memchip.right, g_memchip.bottom), mcol);
    g_members_btn = rf(0, 0, 0, 0);   /* superseded by the chip */

    /* Jump-to-unread (WIN-14): only while this channel actually has a divider to
     * jump to, so it is never a dead control. */
    float statr = g_memchip.left - 12;
    if (g_unread_from && g_unread_chan == g_sel && g_unread_count > 0) {
        char lbl[40];
        snprintf(lbl, sizeof lbl, "%d new \u2191", g_unread_count);
        float bw = text_width(lbl, g_small) + 22;
        g_unread_jump = rf(statr - bw, 14, statr, HEADER_H - 14);
        fill_round(rt, g_unread_jump, 12.0f, OC_COL_DANGER);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, lbl, g_small, g_unread_jump, 0xFFFFFF);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
        statr = g_unread_jump.left - 12;
    } else {
        g_unread_jump = rf(0, 0, 0, 0);
    }

    /* No connection text here any more (WIN-64): it is workspace state, shown on
     * the workspace name, and the banner below carries the detail when it
     * matters. `statr` survives because the unread-jump pill positions off it. */
    (void)statr;
}

/* Switch channel tab (WIN-37). Each tab owns the sub-view it names, so entering
 * one closes the others rather than stacking overlays — the state the tab strip
 * displays and the state the transcript renders are the same variable. */
static void select_tab(int t) {
    if (t < 0 || t >= TAB_COUNT) return;
    const oc_model *mm = model();
    if (mm && mm->pinlist_open)  oc_client_close_pins(g_client);
    if (mm && mm->filelist_open) oc_client_close_files(g_client);
    g_tab = t;
    if (!g_sel) { g_tab = TAB_MESSAGES; return; }
    if (t == TAB_PINS)  oc_client_list_pins(g_client, g_sel);
    if (t == TAB_FILES) oc_client_list_files(g_client, g_sel);
    if (t == TAB_ABOUT) oc_client_list_members(g_client, g_sel);   /* refresh the count */
}

/* The channel tab strip. Returns its height so the caller can push content
 * down; zero when there is no channel to have tabs for. */
static float draw_tabbar(ID2D1RenderTarget *rt, const oc_model *m, float x0, float w) {
    for (int i = 0; i < TAB_COUNT; i++) g_tab_r[i] = rf(0, 0, 0, 0);
    if (!g_sel || !oc_model_channel((oc_model *)m, g_sel)) return 0;

    fill(rt, rf(x0, HEADER_H, x0 + w, HEADER_H + TABBAR_H), OC_COL_HEADER);
    fill(rt, rf(x0, HEADER_H + TABBAR_H - 1, x0 + w, HEADER_H + TABBAR_H), OC_COL_BORDER);

    static const struct { const char *label; int icon; } TABS[TAB_COUNT] = {
        { "Messages",      OC_ICON_DMS  },
        { "Files & links", OC_ICON_FILE },
        { "Pins",          OC_ICON_PIN  },
        { "About",         OC_ICON_SETTINGS },
    };
    float tx = x0 + 16;
    for (int i = 0; i < TAB_COUNT; i++) {
        float tw = 26 + text_width(TABS[i].label, g_ui) + 16;
        D2D1_RECT_F r = rf(tx, HEADER_H + 2, tx + tw, HEADER_H + TABBAR_H - 1);
        int on = (g_tab == i);
        uint32_t col = on ? OC_COL_TEXT : OC_COL_MUTED;
        if (g_tab_hover == i && !on) fill_round(rt, r, 5.0f, OC_COL_HOVER);
        draw_lucide(rt, TABS[i].icon, rf(r.left + 6, r.top + 8, r.left + 22, r.bottom - 8), col);
        draw_text(rt, TABS[i].label, g_ui, rf(r.left + 26, r.top, r.right, r.bottom), col);
        /* The selected tab is marked by an underline on the strip's own border,
         * which is how a tab reads as a tab rather than as a button. */
        if (on) fill(rt, rf(r.left + 4, r.bottom - 2, r.right - 4, r.bottom), OC_COL_ACCENT);
        g_tab_r[i] = r;
        tx = r.right + 4;
    }
    return TABBAR_H;
}

/* The connection banner (REQ-263): a strip under the header whenever we are not
 * authenticated, naming the state and offering an immediate retry. `last_error`
 * carries the specific reason when the net thread has one ("could not reach the
 * server", the reconnect countdown, a changed certificate); without one we fall
 * back to the phase. Returns its height so the caller can push content down. */
static float draw_banner(ID2D1RenderTarget *rt, const oc_model *m, float x0, float w, float top_off) {
    g_banner_on = 0;
    if (!m || m->authed) return 0;

    /* A LIVE countdown (WIN-55). The core's error string states the delay once
     * per backoff, so the number in it never moved — it read as a hung client.
     * The deadline ticks because the model now carries it, and the same clock
     * source is used on both sides so the two cannot disagree. */
    char live[192];
    const char *why = m->last_error[0] ? m->last_error
                    : !m->connected    ? "Connecting…"
                                       : "Signing in…";
    uint64_t left = oc_model_reconnect_in(m, oc_model_now_ms());
    if (left > 0) {
        snprintf(live, sizeof live, "Connection lost — reconnecting in %llus…",
                 (unsigned long long)((left + 999) / 1000));
        why = live;
    } else if (m->reconnect_at_ms) {
        why = "Reconnecting…";
    }
    /* Amber while a connection is plausibly coming back, red once the core has
     * told us something concrete went wrong — the distinction the user acts on. */
    uint32_t accent = m->last_error[0] ? OC_COL_DANGER : OC_COL_AWAY;

    D2D1_RECT_F r = rf(x0, HEADER_H + top_off, x0 + w, HEADER_H + top_off + BANNER_H);
    fill(rt, r, OC_COL_SIDEBAR);
    fill(rt, rf(x0, r.top, x0 + 3, r.bottom), accent);          /* status edge */
    fill(rt, rf(x0, r.bottom - 1, x0 + w, r.bottom), OC_COL_BORDER);

    g_retry_btn = rf(x0 + w - 104, r.top + 5, x0 + w - 14, r.bottom - 5);
    fill_round(rt, g_retry_btn, 6.0f, OC_COL_INPUT);
    stroke_round(rt, g_retry_btn, 6.0f, OC_COL_BORDER, 1.0f);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(rt, "Retry now", g_small, g_retry_btn, OC_COL_TEXT);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);

    draw_text(rt, why, g_small, rf(x0 + 16, r.top, g_retry_btn.left - 12, r.bottom), accent);
    g_banner_on = 1;
    return BANNER_H;
}

/* Toasts, bottom-right of the window, newest nearest the composer. Drawn last so
 * nothing can hide them, and clipped to nothing else — they are the surface of
 * last resort for telling the user something failed. */
/* One action catalogue, shared by the palette. The `cmd` values are the same
 * menu_dispatch codes the menus use, so the palette can never offer an action
 * the menus do not have or dispatch it differently. */
static const struct { const char *label; int cmd; } PALETTE[] = {
    { "Create a channel",        1  },
    { "New direct message",      6  },
    { "Search messages",         4  },
    { "Upload a file",           7  },
    { "Preferences",             70 },
    { "Notifications",           71 },
    { "Keyboard shortcuts",      72 },
    { "Mark all as read",        73 },
    { "Edit display name",       30 },
    { "Change password",         31 },
    { "Do not disturb",          50 },
    { "Set yourself active",     10 },
    { "Set yourself away",       11 },
    { "Invite people as member", 40 },
    { "Invite people as admin",  41 },
    { "Storage usage",           60 },
    { "Audit log",               61 },
    { "Reconnect now",           2  },
    { "Add a workspace",         80 },
    { "Sign out",                3  },
};

/* Subsequence match, so "cp" finds "Change password" — the usual palette
 * behaviour, and cheap enough to run over the whole catalogue every frame. */
static int pal_match(const char *hay, const char *needle) {
    if (!needle || !needle[0]) return 1;
    const char *n = needle;
    for (const char *h = hay; *h; h++) {
        if (tolower((unsigned char)*h) == tolower((unsigned char)*n)) { n++; if (!*n) return 1; }
    }
    return 0;
}

static void draw_palette(ID2D1RenderTarget *rt, const oc_model *m, float W, float H) {
    g_n_pal_rows = 0;
    if (!g_pal_open) { g_pal_panel = rf(0, 0, 0, 0); return; }

    /* Dim the app behind it: the palette takes the keyboard, and saying so is
     * cheaper than having the user discover it by typing into nothing. */
    D2D1_RECT_F all = rf(0, 0, W, H);
    ID2D1RenderTarget_FillRectangle(rt, &all, paint_alpha(0x000000, 0.35f));

    float pw = 520; if (pw > W - 80) pw = W - 80;
    float px = (W - pw) / 2, py = 96;
    float rowh = 32, maxrows = 10;

    char q[64] = "";
    if (g_pal_edit) {
        WCHAR wq[64]; GetWindowTextW(g_pal_edit, wq, 64);
        WideCharToMultiByte(CP_UTF8, 0, wq, -1, q, sizeof q, NULL, NULL);
    }

    /* Collect matches first so the panel can be sized to them. */
    struct { const char *label, *kind; int cmd; uint64_t cid; } hit[12];
    int nh = 0;
    for (size_t i = 0; i < sizeof PALETTE / sizeof PALETTE[0] && nh < 12; i++)
        if (pal_match(PALETTE[i].label, q)) {
            hit[nh].label = PALETTE[i].label; hit[nh].kind = "Action";
            hit[nh].cmd = PALETTE[i].cmd; hit[nh].cid = 0; nh++;
        }
    static char names[12][96];
    if (m) for (size_t i = 0; i < m->n_channels && nh < 12; i++) {
        channel_label(m, &m->channels[i], names[nh], sizeof names[nh]);
        if (!pal_match(names[nh], q)) continue;
        hit[nh].label = names[nh]; hit[nh].kind = "Go to";
        hit[nh].cmd = 0; hit[nh].cid = m->channels[i].channel_id; nh++;
    }
    if (nh > (int)maxrows) nh = (int)maxrows;
    if (g_pal_sel >= nh) g_pal_sel = nh ? nh - 1 : 0;
    if (g_pal_sel < 0) g_pal_sel = 0;

    float ph = 58 + (nh ? nh * rowh : rowh) + 10;
    g_pal_panel = rf(px, py, px + pw, py + ph);
    fill_round(rt, rf(px + 3, py + 5, px + pw + 3, py + ph + 5), 12.0f, 0x000000);
    fill_round(rt, g_pal_panel, 12.0f, OC_COL_INPUT);
    stroke_round(rt, g_pal_panel, 12.0f, OC_COL_BORDER, 1.0f);

    g_pal_box = rf(px + 12, py + 12, px + pw - 12, py + 46);
    fill_round(rt, g_pal_box, 7.0f, OC_COL_BASE);
    draw_lucide(rt, OC_ICON_SEARCH, rf(g_pal_box.left + 9, g_pal_box.top + 9,
                                       g_pal_box.left + 25, g_pal_box.top + 25), OC_COL_MUTED);

    float y = py + 54;
    if (nh == 0) {
        draw_text(rt, "No matching action or conversation.", g_ui,
                  rf(px + 20, y, px + pw - 20, y + rowh), OC_COL_FAINT);
        return;
    }
    for (int i = 0; i < nh; i++) {
        D2D1_RECT_F r = rf(px + 6, y, px + pw - 6, y + rowh);
        if (i == g_pal_sel) fill_round(rt, r, 6.0f, OC_COL_ACCENT);
        draw_text(rt, hit[i].label, g_ui, rf(px + 18, y, px + pw - 90, y + rowh),
                  i == g_pal_sel ? 0xFFFFFF : OC_COL_TEXT);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_TRAILING);
        draw_text(rt, hit[i].kind, g_small, rf(px + 18, y, px + pw - 18, y + rowh),
                  i == g_pal_sel ? 0xFFFFFF : OC_COL_FAINT);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
        g_pal_rows[g_n_pal_rows].r = r;
        g_pal_rows[g_n_pal_rows].cmd = hit[i].cmd;
        g_pal_rows[g_n_pal_rows].cid = hit[i].cid;
        g_n_pal_rows++;
        y += rowh;
    }
}

static void draw_toasts(ID2D1RenderTarget *rt, float W, float H) {
    /* Clear every hit-box first: in a short window the loop below stops early,
     * and a box left over from a taller frame would let a click dismiss a toast
     * that isn't on screen. */
    for (int i = 0; i < TOAST_MAX; i++) g_toast_box[i] = rf(0, 0, 0, 0);
    float y = H - g_composer_h - TOAST_GAP;
    for (int i = g_n_toast - 1; i >= 0; i--) {
        D2D1_RECT_F r = rf(W - TOAST_W - 20, y - TOAST_H, W - 20, y);
        g_toast_box[i] = r;
        /* Squared on every corner — a toast is a rectangle, not a pill — with a
         * full-height accent stripe flush to the left edge. */
        uint32_t accent = g_toast[i].danger ? OC_COL_DANGER : OC_COL_NOTICE;
        uint32_t edge   = g_toast[i].danger ? OC_COL_DANGER : OC_COL_BORDER;
        float bar = 4.0f;
        fill(rt, rf(r.left + 2, r.top + 3, r.right + 2, r.bottom + 3), OC_COL_RAIL);  /* shadow */
        fill(rt, r, OC_COL_INPUT);
        fill(rt, rf(r.left, r.top, r.right, r.top + 1), edge);            /* border */
        fill(rt, rf(r.left, r.bottom - 1, r.right, r.bottom), edge);
        fill(rt, rf(r.right - 1, r.top, r.right, r.bottom), edge);
        fill(rt, rf(r.left, r.top, r.left + bar, r.bottom), accent);      /* stripe */
        draw_text(rt, g_toast[i].text, g_small,
                  rf(r.left + 14, r.top + 6, r.right - 12, r.bottom - 6), OC_COL_TEXT);
        y -= TOAST_H + TOAST_GAP;
        if (y < HEADER_H + TOAST_H) break;     /* never climb into the header */
    }
}

/* What the context pane is currently showing. MEMBERS is the resting state; the
 * others are pushed on top of it and pop back with the header's back arrow. */
enum { RP_MEMBERS = 0, RP_PROFILE, RP_REACTORS };
static int      g_rp_mode;
static D2D1_RECT_F g_rp_back, g_rp_close;

static void rp_push(int mode) { g_rp_mode = mode; g_show_members = 1; }
static void rp_pop(void) { g_rp_mode = RP_MEMBERS; g_profile_uid = 0; }

static void draw_profile_card(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg);
static void draw_reactors_list(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg);

static void draw_members(ID2D1RenderTarget *rt, const oc_model *m, float W, float H) {
    float x0 = W - MEMBERS_W;
    fill(rt, rf(x0, 0, W, H), OC_COL_SIDEBAR);
    fill(rt, rf(x0, 0, x0 + 1, H), OC_COL_BORDER);

    /* One header for every mode: a title, a back arrow when there is somewhere
     * to go back to, and a close. Without the back arrow, opening a person's
     * card stranded you — the list you came from was gone. */
    const char *title = g_rp_mode == RP_PROFILE  ? "PROFILE"
                      : g_rp_mode == RP_REACTORS ? "REACTIONS" : "MEMBERS";
    g_rp_back = g_rp_mode == RP_MEMBERS ? rf(0, 0, 0, 0) : rf(x0 + 8, 8, x0 + 30, 30);
    if (g_rp_mode != RP_MEMBERS) {
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, "\xE2\x80\xB9", g_ui, g_rp_back, OC_COL_MUTED);
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_LEADING);
    }
    draw_text(rt, title, g_small,
              rf(x0 + (g_rp_mode == RP_MEMBERS ? 16 : 34), 10, W - 34, 34), OC_COL_FAINT);
    g_rp_close = rf(W - 30, 8, W - 8, 30);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(rt, "\xC3\x97", g_small, g_rp_close, OC_COL_MUTED);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);

    if (g_rp_mode == RP_PROFILE)  { g_n_memrows = 0; draw_profile_card(rt, m, rf(x0, 40, W, H)); return; }
    if (g_rp_mode == RP_REACTORS) { g_n_memrows = 0; draw_reactors_list(rt, m, rf(x0, 40, W, H)); return; }

    float y = 40;
    g_n_memrows = 0;
    /* This channel's members (REQ-031) — NOT the tenant roster, which is what
     * this pane used to list. With two users the two are the same set, which is
     * exactly why the bug survived: in any real workspace it showed people who
     * cannot read the channel they were listed beside. */
    if (m->chanmem_channel != g_sel || m->chanmem_loading) {
        draw_text(rt, m->chanmem_loading ? "Loading\u2026" : "", g_small,
                  rf(x0 + 16, 44, W - 12, 68), OC_COL_FAINT);
        return;
    }
    for (size_t i = 0; i < m->n_chanmem; i++) {
        const oc_chan_member *cm = &m->chanmem[i];
        if (y > H) break;
        const char *nm = oc_model_user_name((oc_model *)m, cm->user_id);
        draw_presence_dot(rt, x0 + 22, y + ROW_H / 2, 4.5f,
                          oc_model_presence_of(m, cm->user_id), OC_COL_SIDEBAR);
        const char *disp = (nm && nm[0]) ? nm : "user";
        draw_text(rt, disp, g_ui, rf(x0 + 34, y, W - 14, y + ROW_H), OC_COL_TEXT);
        /* Role glyph INLINE, immediately after the name — not in a column of its
         * own. Almost nobody is an owner or an admin, so a reserved column spends
         * horizontal space on every row to serve the rare one, in a pane only
         * 220px wide. Owner gets a crown, admin a shield, member nothing: a
         * marker on every row is noise, and "member" is the default that needs no
         * saying. The glyph is an at-a-glance hint; the profile pane (WIN-10)
         * remains the answer, in words. */
        if (cm->role >= OC_ROLE_ADMIN) {
            float gx = x0 + 34 + text_width(disp, g_ui) + 6;
            if (gx + 14 < W - 14)          /* skip it rather than collide with a long name */
                draw_lucide(rt, cm->role == OC_ROLE_OWNER ? OC_ICON_CROWN : OC_ICON_SHIELD,
                            rf(gx, y + ROW_H / 2 - 7, gx + 14, y + ROW_H / 2 + 7), OC_COL_FAINT);
        }
        if (g_n_memrows < (int)(sizeof g_memrows / sizeof g_memrows[0])) {
            g_memrows[g_n_memrows].r = rf(x0, y, W, y + ROW_H);
            g_memrows[g_n_memrows].uid = cm->user_id; g_n_memrows++;
        }
        y += ROW_H;
    }
}

/* The composer region behind the RichEdit child (which paints its own box),
 * plus an attach (+) button to the left of the input. */
/* Card geometry, derived once so draw_signin() and layout_signin() cannot drift
 * (they must agree on the field rects to the pixel — the EDITs sit on top of the
 * chrome the painter draws). Height grows with the error row and the step, so an
 * error can never overlap the button below it. */
static si_geom si_layout(float W, float H) {
    si_geom g;
    g.w  = SI_W;
    g.x0 = W / 2 - SI_W / 2;
    float pad = 28;
    g.fx = g.x0 + pad;
    g.fw = SI_W - 2 * pad;

    float head = 26 + 52 + 30 + 30;              /* mark + heading + subheading */
    int   nfields = (g_si_step == 1) ? 1 : 2;
    float body = g_si_connecting ? 76.0f
               : (float)nfields * 62.0f
                 + (g_si_err[0] ? 34.0f : 0.0f)
                 + (g_si_step == 1 ? 26.0f : 0.0f)   /* advanced-options link */
                 + (g_si_step == 2 ? 30.0f : 0.0f)   /* remember-me row */
                 + 40.0f + 24.0f                      /* button + bottom pad */
                 + (g_si_step == 2 ? 26.0f + 22.0f : 0.0f)   /* back + signup links */
                 + (g_si_overlay ? 24.0f : 0.0f);            /* cancel row */
    g.h  = head + body;
    g.y0 = (H - g.h) / 2; if (g.y0 < 24) g.y0 = 24;
    g.fields_y = g.y0 + head;
    return g;
}

/* One centred sign-in card, both steps. Field *chrome* is drawn here; the text
 * itself lives in native EDIT children positioned over it by layout_signin(),
 * so we get IME, selection, clipboard and password masking for free rather than
 * hand-rolling a text editor (the same trade the composer and find box make). */
static void draw_signin(ID2D1RenderTarget *rt, float W, float H) {
    si_geom g = si_layout(W, H);
    float cx = W / 2, x0 = g.x0, y0 = g.y0;

    D2D1_RECT_F card = rf(x0, y0, x0 + SI_W, y0 + g.h);
    fill_round(rt, rf(card.left + 2, card.top + 4, card.right + 2, card.bottom + 4), 14.0f, OC_COL_RAIL);
    fill_round(rt, card, 14.0f, OC_COL_SIDEBAR);
    stroke_round(rt, card, 14.0f, OC_COL_BORDER, 1.0f);

    float fx = g.fx, fw = g.fw, y = y0 + 26;

    /* Mark: the accent rounded square the rail uses for the workspace avatar. */
    D2D1_RECT_F mark = rf(cx - 18, y, cx + 18, y + 36);
    fill_round(rt, mark, 11.0f, OC_COL_ACCENT);
    draw_text(rt, "O", g_ava, mark, 0xFFFFFF);
    y += 52;

    IDWriteTextFormat_SetTextAlignment(g_hdr,   DWRITE_TEXT_ALIGNMENT_CENTER);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
    char head[288];
    if (g_si_step == 1) snprintf(head, sizeof head, "Sign in to OpenChime");
    else                snprintf(head, sizeof head, "Sign in to %s", g_si_ws);
    draw_text(rt, head, g_hdr, rf(x0 + 12, y, x0 + SI_W - 12, y + 30), OC_COL_TEXT);
    y += 30;
    if (g_si_step == 2) {
        char sub[300]; snprintf(sub, sizeof sub, "%s:%d", g_si_host, g_si_port);
        draw_text(rt, sub, g_small, rf(x0 + 12, y, x0 + SI_W - 12, y + 20), OC_COL_MUTED);
    } else {
        draw_text(rt, "Enter your workspace address", g_small,
                  rf(x0 + 12, y, x0 + SI_W - 12, y + 20), OC_COL_MUTED);
    }
    IDWriteTextFormat_SetTextAlignment(g_hdr,   DWRITE_TEXT_ALIGNMENT_LEADING);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
    y += 30;

    /* While connecting the fields are hidden and the card says so — the wait is
     * the whole content, so there is nothing to mis-click. */
    if (g_si_connecting) {
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, "Signing in\xE2\x80\xA6", g_ui, rf(x0, y + 24, x0 + SI_W, y + 52), OC_COL_MUTED);
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_LEADING);
        g_si_btn = g_si_remember_box = g_si_back = g_si_adv_link = rf(0, 0, 0, 0);
        return;
    }

    /* Field chrome. The EDITs are placed on these same rects by layout_signin. */
    const char *labels[2]; int nfields;
    if (g_si_step == 1) { labels[0] = "Workspace"; nfields = 1; }
    else                { labels[0] = "Username"; labels[1] = "Password"; nfields = 2; }
    for (int i = 0; i < nfields; i++) {
        draw_text(rt, labels[i], g_small, rf(fx, y, fx + fw, y + 18), OC_COL_MUTED);
        D2D1_RECT_F box = rf(fx, y + 20, fx + fw, y + 52);
        fill_round(rt, box, 8.0f, OC_COL_INPUT);
        stroke_round(rt, box, 8.0f, g_si_err[0] ? OC_COL_DANGER : OC_COL_BORDER, 1.0f);
        /* Hosted mode: the service suffix is chrome, not something to type. */
        if (g_si_step == 1 && !g_si_advanced) {
            char suf[80]; snprintf(suf, sizeof suf, ".%s", oc_default_suffix());
            IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_TRAILING);
            draw_text(rt, suf, g_small, rf(fx, y + 20, fx + fw - 12, y + 52), OC_COL_MUTED);
            IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
        }
        y += 62;
    }

    if (g_si_err[0]) {
        char line[208]; snprintf(line, sizeof line, "\xE2\x9A\xA0 %s", g_si_err);
        draw_text(rt, line, g_small, rf(fx, y, fx + fw, y + 34), OC_COL_DANGER);
        y += 34;
    }

    if (g_si_step == 1) {
        g_si_adv_link = rf(fx, y, fx + fw, y + 20);
        draw_text(rt, g_si_advanced ? "\xE2\x86\x90 Use a workspace name"
                                    : "Advanced options\xE2\x80\xA6",
                  g_small, g_si_adv_link, OC_COL_ACCENT);
        y += 26;
    } else {
        g_si_adv_link = rf(0, 0, 0, 0);
    }

    if (g_si_step == 2) {
        /* Remember me — a drawn checkbox, matching the rest of the shell rather
         * than dropping a native BUTTON into an otherwise D2D card. */
        g_si_remember_box = rf(fx, y + 2, fx + 16, y + 18);
        fill_round(rt, g_si_remember_box, 4.0f, g_si_remember ? OC_COL_ACCENT : OC_COL_INPUT);
        stroke_round(rt, g_si_remember_box, 4.0f,
                     g_si_remember ? OC_COL_ACCENT : OC_COL_BORDER, 1.0f);
        if (g_si_remember)
            draw_text(rt, "\xE2\x9C\x93", g_small, rf(fx + 2, y, fx + 18, y + 20), 0xFFFFFF);
        draw_text(rt, "Remember me", g_small, rf(fx + 24, y, fx + fw, y + 20), OC_COL_MUTED);
        y += 30;
    }

    g_si_btn = rf(fx, y, fx + fw, y + 40);
    fill_round(rt, g_si_btn, 8.0f, OC_COL_ACCENT);
    IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(rt, g_si_step == 1 ? "Continue" : "Sign in", g_ui, g_si_btn, 0xFFFFFF);
    IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_LEADING);
    y += 50;

    if (g_si_step == 2) {
        g_si_back = rf(fx, y, fx + fw, y + 20);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, "\xE2\x86\x90 Use a different workspace", g_small, g_si_back, OC_COL_ACCENT);
        /* WIN-32: the only way to turn an invite into an account used to be the
         * command line. Signup is its own small form rather than three more
         * fields on this card, which would push the layout around for a path
         * most people take once. */
        g_si_invite_link = rf(fx, y + 22, fx + fw, y + 42);
        draw_text(rt, "Have an invite? Create an account", g_small,
                  g_si_invite_link, OC_COL_MUTED);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
    } else {
        g_si_back = rf(0, 0, 0, 0);
        g_si_invite_link = rf(0, 0, 0, 0);
    }

    /* A way out, when there is somewhere to go back to. Esc does it too, but a
     * modal card with no visible exit is a trap. Inside the card: below it the
     * text landed on whatever the transcript happened to be showing. */
    if (g_si_overlay) {
        g_si_cancel = rf(fx, card.bottom - 28, fx + fw, card.bottom - 6);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, "Cancel  (Esc)", g_small, g_si_cancel, OC_COL_MUTED);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
    } else {
        g_si_cancel = rf(0, 0, 0, 0);
    }
}

/* The candidate popover, anchored to the composer's top edge and growing
 * upwards. Drawn last so it sits over the transcript. */
static void draw_autocomplete(ID2D1RenderTarget *rt, float x0, float w, float h) {
    if (g_n_ac <= 0) { g_ac_panel = rf(0, 0, 0, 0); return; }
    /* Height = header band + rows + hint band. Each band is accounted for once,
     * or the hint lands on top of the last row. */
    float rowh = 26, hdr_h = 22, hint_h = 22;
    float pw = 320; if (pw > w - 40) pw = w - 40;
    float ph = hdr_h + g_n_ac * rowh + hint_h;
    float px = x0 + 20, py = h - g_composer_h - ph - 6;
    if (py < HEADER_H + 6) py = HEADER_H + 6;
    g_ac_panel = rf(px, py, px + pw, py + ph);

    /* Same surface + drop shadow as the dropdown menus, so the popover reads as
     * one of the app's floating panels rather than a new kind of thing. */
    fill_round(rt, rf(g_ac_panel.left + 2, g_ac_panel.top + 4,
                      g_ac_panel.right + 2, g_ac_panel.bottom + 4), 8.0f, OC_COL_RAIL);
    fill_round(rt, g_ac_panel, 8.0f, OC_COL_INPUT);
    stroke_round(rt, g_ac_panel, 8.0f, OC_COL_BORDER, 1.0f);

    const char *hdr = g_ac_kind == OC_AC_MENTION ? "PEOPLE"
                    : g_ac_kind == OC_AC_CHANNEL ? "CHANNELS" : "EMOJI";
    draw_text(rt, hdr, g_small, rf(px + 12, py + 5, px + pw - 12, py + hdr_h), OC_COL_FAINT);

    float y = py + hdr_h;
    for (int i = 0; i < g_n_ac; i++) {
        D2D1_RECT_F r = rf(px + 4, y, px + pw - 4, y + rowh);
        g_ac_rows[i] = r;
        if (i == g_ac_sel) fill_round(rt, r, 5.0f, OC_COL_ACCENT);
        draw_text(rt, g_ac[i].disp, g_ui, rf(px + 12, y + 3, px + pw - 12, y + rowh),
                  i == g_ac_sel ? 0xFFFFFF : OC_COL_TEXT);
        y += rowh;
    }
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_TRAILING);
    draw_text(rt, "Tab or Enter to insert", g_small,
              rf(px + 12, py + ph - hint_h + 3, px + pw - 12, py + ph - 2), OC_COL_FAINT);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
}

/* The emoji picker: a search box over a category-sectioned grid. Anchored above
 * the composer like the autocomplete popover, so both "insert an emoji" paths
 * appear in the same place. */
static void draw_emoji_picker(ID2D1RenderTarget *rt, float x0, float w, float h) {
    g_n_pick_cells = 0;
    if (!g_pick_open) { g_pick_panel = rf(0, 0, 0, 0); return; }

    float pw = 360; if (pw > w - 40) pw = w - 40;
    float ph = 300; if (ph > h - HEADER_H - g_composer_h - 20) ph = h - HEADER_H - g_composer_h - 20;
    float px = x0 + 20, py = h - g_composer_h - ph - 6;
    if (py < HEADER_H + 6) py = HEADER_H + 6;
    g_pick_panel = rf(px, py, px + pw, py + ph);

    fill_round(rt, rf(px + 2, py + 4, px + pw + 2, py + ph + 4), 10.0f, OC_COL_RAIL);
    fill_round(rt, g_pick_panel, 10.0f, OC_COL_INPUT);
    stroke_round(rt, g_pick_panel, 10.0f, OC_COL_BORDER, 1.0f);

    draw_text(rt, g_pick_mid ? "Add reaction" : "Emoji", g_name,
              rf(px + 14, py + 8, px + pw - 14, py + 30), OC_COL_TEXT);

    g_pick_box = rf(px + 12, py + 32, px + pw - 12, py + 62);
    fill_round(rt, g_pick_box, 6.0f, OC_COL_BASE);
    stroke_round(rt, g_pick_box, 6.0f, OC_COL_BORDER, 1.0f);
    draw_lucide(rt, OC_ICON_SEARCH, rf(g_pick_box.left + 8, g_pick_box.top + 7,
                                       g_pick_box.left + 24, g_pick_box.top + 23), OC_COL_MUTED);

    char q[64] = "";
    if (g_pick_edit) {
        WCHAR wq[64]; GetWindowTextW(g_pick_edit, wq, 64);
        WideCharToMultiByte(CP_UTF8, 0, wq, -1, q, sizeof q, NULL, NULL);
    }

    D2D1_RECT_F body = rf(px + 8, py + 68, px + pw - 8, py + ph - 8);
    ID2D1RenderTarget_PushAxisAlignedClip(rt, &body, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);

    const oc_emoji *hits[256];
    size_t nh = oc_emoji_search(q[0] ? q : NULL, hits, 256);

    float cell = 34, cols = (float)(int)((body.right - body.left) / cell);
    if (cols < 1) cols = 1;
    float y = body.top - g_pick_scroll, x = body.left;
    int col = 0, last_cat = -1;
    for (size_t i = 0; i < nh; i++) {
        /* Category headings only when browsing; a filtered list is already the
         * answer to a question and section breaks just fragment it. */
        if (!q[0] && hits[i]->category != last_cat) {
            if (col != 0) { y += cell; col = 0; x = body.left; }
            if (y + 18 >= body.top && y <= body.bottom)
                draw_text(rt, oc_emoji_category_name(hits[i]->category), g_small,
                          rf(body.left + 4, y, body.right, y + 18), OC_COL_FAINT);
            y += 20;
            last_cat = hits[i]->category;
        }
        if (y + cell >= body.top && y <= body.bottom) {
            D2D1_RECT_F r = rf(x, y, x + cell, y + cell);
            if (g_n_pick_cells < 256) {
                g_pick_cells[g_n_pick_cells].r = r;
                g_pick_cells[g_n_pick_cells].emoji = hits[i]->emoji;
                g_n_pick_cells++;
            }
            draw_emoji_glyph(rt, hits[i]->emoji, r);
        }
        x += cell;
        if (++col >= (int)cols) { col = 0; x = body.left; y += cell; }
    }
    if (col != 0) y += cell;

    float content = (y + g_pick_scroll) - body.top, visible = body.bottom - body.top;
    float maxs = content > visible ? content - visible : 0;
    if (g_pick_scroll > maxs) g_pick_scroll = maxs;
    if (g_pick_scroll < 0) g_pick_scroll = 0;
    if (nh == 0) overlay_empty(rt, body, "No emoji match that.");
    ID2D1RenderTarget_PopAxisAlignedClip(rt);
}

static void ac_close(void);                                   /* fwd */
static int  reaction_is_mine(const oc_msg *msg, const char *emoji);   /* fwd */

/* Open the picker for the composer (mid == 0) or for reacting to a message. */
static void picker_open(HWND hwnd, uint64_t mid) {
    g_pick_open = 1; g_pick_mid = mid; g_pick_scroll = 0;
    ac_close();
    if (g_pick_edit) {
        SetWindowTextW(g_pick_edit, L"");
        ShowWindow(g_pick_edit, SW_SHOW);
        SetFocus(g_pick_edit);
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

static void picker_close(HWND hwnd) {
    g_pick_open = 0; g_pick_mid = 0;
    if (g_pick_edit) ShowWindow(g_pick_edit, SW_HIDE);
    if (g_re) SetFocus(g_re);
    if (hwnd) InvalidateRect(hwnd, NULL, FALSE);
}

/* Apply the chosen emoji to whichever caller opened the picker. */
static void picker_choose(HWND hwnd, const char *emoji) {
    if (!emoji || !g_client) { picker_close(hwnd); return; }
    if (g_pick_mid) {
        const oc_model *m = model();
        const oc_channel *c = m ? oc_model_channel((oc_model *)m, g_sel) : NULL;
        const oc_msg *msg = find_msg(c, g_pick_mid);
        uint64_t chan = g_sel;
        if (!msg && m && m->thread_open)          /* the target may be a reply */
            for (size_t i = 0; i < m->n_thread_msgs; i++)
                if (m->thread_msgs[i].message_id == g_pick_mid) {
                    msg = &m->thread_msgs[i]; chan = m->thread_channel; break;
                }
        oc_client_react(g_client, chan, g_pick_mid, emoji,
                        (msg && reaction_is_mine(msg, emoji)) ? 0 : 1);
    } else if (g_re) {
        WCHAR w[16];
        if (MultiByteToWideChar(CP_UTF8, 0, emoji, -1, w, 16) > 0) {
            SendMessageW(g_re, EM_SETSEL, (WPARAM)-1, -1);   /* caret to end */
            SendMessageW(g_re, EM_REPLACESEL, TRUE, (LPARAM)w);
        }
    }
    picker_close(hwnd);
}

static void composer_placeholder(const oc_model *m);   /* fwd */

static void draw_composer(ID2D1RenderTarget *rt, float x0, float w, float h) {
    composer_placeholder(model());   /* the cue tracks the open conversation */
    float top = h - g_composer_h;
    fill(rt, rf(x0, top, x0 + w, h), OC_COL_BASE);

    /* A bordered, rounded input container the composer lives inside (Slack-style),
     * so the field reads as a real control rather than a naked line of text. */
    float bx0 = x0 + 20, bx1 = x0 + w - 20;
    float by0 = top + COMPOSER_MT, by1 = h - COMPOSER_MB;
    fill_round(rt, rf(bx0, by0, bx1, by1), 10.0f, OC_COL_INPUT);
    stroke_round(rt, rf(bx0, by0, bx1, by1), 10.0f, OC_COL_BORDER, 1.0f);

    /* Buttons sit on the box's bottom line. At rest that is also its middle,
     * so a one-line composer reads as a single centred row. */
    float sq = COMPOSER_BTN;
    float cy = by1 - COMPOSER_PAD - sq;

    g_attach_btn = rf(bx0 + 6, cy, bx0 + 6 + sq, cy + sq);
    draw_lucide(rt, OC_ICON_PLUS, rf(g_attach_btn.left + 8, g_attach_btn.top + 8,
                                     g_attach_btn.right - 8, g_attach_btn.bottom - 8),
                OC_COL_MUTED);

    /* A monochrome line icon, not a colour glyph: this is chrome, and it sat
     * next to a grey "+" and a grey paper plane as the only coloured control in
     * the whole shell. Colour is for content — messages, reactions, and the
     * glyphs you pick in the picker. */
    g_emoji_btn = rf(bx0 + 6 + sq, cy, bx0 + 6 + sq * 2, cy + sq);
    draw_lucide(rt, OC_ICON_SMILE, rf(g_emoji_btn.left + 8, g_emoji_btn.top + 8,
                                      g_emoji_btn.right - 8, g_emoji_btn.bottom - 8),
                OC_COL_MUTED);

    /* Mention. The '@' trigger already worked when typed; ARCH-82 says the GUI
     * is affordance-driven, so it needs to be visible too. */
    g_at_btn = rf(bx0 + 6 + sq * 2, cy, bx0 + 6 + sq * 3, cy + sq);
    draw_lucide(rt, OC_ICON_AT, rf(g_at_btn.left + 8, g_at_btn.top + 8,
                                   g_at_btn.right - 8, g_at_btn.bottom - 8), OC_COL_MUTED);

    /* Send on the right — accent when there is something to send. A paper
     * plane rather than an up-arrow, which read as "scroll" more than "send". */
    /* An archived channel is read-only (REQ-035). The daemon refuses the send
     * regardless; this is so you can see why before you type it. */
    const oc_model *cm = model();
    const oc_channel *cc = cm && g_sel ? oc_model_channel((oc_model *)cm, g_sel) : NULL;
    int ro = (cc && cc->archived);
    int has_text = !ro && g_re && GetWindowTextLengthW(g_re) > 0;
    g_send_btn = rf(bx1 - 6 - sq, cy, bx1 - 6, cy + sq);
    fill_round(rt, g_send_btn, 8.0f, has_text ? OC_COL_ACCENT : OC_COL_INPUT);
    if (!has_text) stroke_round(rt, g_send_btn, 8.0f, OC_COL_BORDER, 1.0f);
    draw_lucide(rt, OC_ICON_SEND, rf(g_send_btn.left + 8, g_send_btn.top + 8,
                                     g_send_btn.right - 8, g_send_btn.bottom - 8),
                has_text ? 0xFFFFFF : OC_COL_FAINT);
}

/* Your-account surfaces are MODALS, not panes (the three-column rule): the left
 * column is navigation, the middle is the conversation, the right is people. Your
 * own preferences, shortcuts, workspaces and notification settings are none of
 * those — they interrupt, they are not context for what you are reading — so they
 * dim the shell and sit in a centred card you dismiss.
 *
 * They used to replace the transcript, which made "change a setting" cost you
 * your place in the conversation. */
static int modal_open(void) {
    return g_prefs_open || g_keys_open || g_wsmgr_open || g_notify_open;
}

static D2D1_RECT_F g_modal_card;

static void draw_modal(ID2D1RenderTarget *rt, const oc_model *m, float W, float H) {
    if (!modal_open()) { g_modal_card = rf(0, 0, 0, 0); return; }
    D2D1_RECT_F all = rf(0, 0, W, H);
    ID2D1RenderTarget_FillRectangle(rt, &all, paint_alpha(0x000000, 0.50f));

    float cw = W - 160, ch = H - 120;
    if (cw > 720) cw = 720;
    if (ch > 620) ch = 620;
    if (cw < 320) cw = W;
    if (ch < 240) ch = H;
    D2D1_RECT_F card = rf((W - cw) / 2, (H - ch) / 2, (W + cw) / 2, (H + ch) / 2);
    g_modal_card = card;
    fill_round(rt, card, 10.0f, OC_COL_BASE);
    stroke_round(rt, card, 10.0f, OC_COL_BORDER, 1.0f);

    if (g_prefs_open)       draw_prefs(rt, card);
    else if (g_keys_open)   draw_keys(rt, card);
    else if (g_wsmgr_open)  draw_wsmgr(rt, card);
    else if (g_notify_open) draw_notify_prefs(rt, m, card);
}

/* ---- paint --------------------------------------------------------------- */

/* Draw the whole UI into `rt` (window RT for painting, or a DC RT for test
 * shots). Caller wraps this in BeginDraw/EndDraw; brushes must belong to `rt`. */
/* Views that show the channel sidebar + transcript + composer.
 *
 * Deliberately FALSE during sign-in even when the shell is drawn behind the
 * card (g_si_overlay): this gates the native children — composer, find box —
 * and a native child composites above Direct2D, so leaving them shown would
 * punch them straight through the sign-in card. `shell_visible` is the drawing
 * question; this is the input/child-window question. */
static int view_has_sidebar(void) { return g_view == VIEW_HOME || g_view == VIEW_DMS; }

/* Whether to PAINT the shell chrome. During sign-in that is true only when a
 * workspace is still live behind the card. */
static int shell_visible(void) {
    if (g_view == VIEW_SIGNIN) return g_si_overlay && g_client != NULL;
    return g_view == VIEW_HOME || g_view == VIEW_DMS;
}

/* The Admin view (rail). Workspace-scoped, so it lives in the MIDDLE column by
 * ARCH-94 — but it used to be a stub whose entire content was an instruction to
 * open the workspace menu instead, which is a dead end wearing a signpost. Both
 * reports were already built; this just puts them where the rail already
 * promised they were. */
enum { ADM_STORAGE = 0, ADM_AUDIT, ADM_COUNT };
static int g_adm_tab;
static D2D1_RECT_F g_adm_tabs[ADM_COUNT];

static void admin_select(int t) {
    g_adm_tab = t;
    if (!g_client) return;
    /* Ask on entry: these are point-in-time reports, and a stale one is worse
     * than a moment's wait. */
    if (t == ADM_STORAGE) { oc_client_toggle_storage(g_client, 1); oc_client_storage_status(g_client); }
    else                  { oc_client_toggle_audit(g_client, 1);   oc_client_audit_query(g_client, 0); }
}

static void draw_admin(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    fill(rt, rf(reg.left, reg.top, reg.right, reg.top + HEADER_H), OC_COL_HEADER);
    draw_text(rt, "Admin", g_hdr, rf(reg.left + 20, reg.top, reg.right - 20, reg.top + HEADER_H),
              OC_COL_TEXT);
    fill(rt, rf(reg.left, reg.top + HEADER_H, reg.right, reg.top + HEADER_H + TABBAR_H), OC_COL_HEADER);
    fill(rt, rf(reg.left, reg.top + HEADER_H + TABBAR_H - 1, reg.right,
                reg.top + HEADER_H + TABBAR_H), OC_COL_BORDER);

    static const struct { const char *label; int icon; } T[ADM_COUNT] = {
        { "Storage",   OC_ICON_FILE },
        { "Audit log", OC_ICON_SETTINGS },
    };
    float tx = reg.left + 16, ty = reg.top + HEADER_H;
    for (int i = 0; i < ADM_COUNT; i++) {
        float tw = 26 + text_width(T[i].label, g_ui) + 16;
        D2D1_RECT_F r = rf(tx, ty + 2, tx + tw, ty + TABBAR_H - 1);
        int on = (g_adm_tab == i);
        uint32_t col = on ? OC_COL_TEXT : OC_COL_MUTED;
        draw_lucide(rt, T[i].icon, rf(r.left + 6, r.top + 8, r.left + 22, r.bottom - 8), col);
        draw_text(rt, T[i].label, g_ui, rf(r.left + 26, r.top, r.right, r.bottom), col);
        if (on) fill(rt, rf(r.left + 4, r.bottom - 2, r.right - 4, r.bottom), OC_COL_ACCENT);
        g_adm_tabs[i] = r;
        tx = r.right + 4;
    }

    D2D1_RECT_F body = rf(reg.left, ty + TABBAR_H, reg.right, reg.bottom);
    if (self_role(m) < OC_ROLE_ADMIN) {
        overlay_empty(rt, body, "Admin only.");
        return;
    }
    if (g_adm_tab == ADM_STORAGE) draw_storage(rt, m, body, 1);
    else                          draw_audit(rt, m, body, 1);
}

/* The DMs list (second column, in the DMs view).
 *
 * Keyed on the DM **channel**, not on a person. That distinction is invisible
 * today — every DM has exactly one peer — and decisive the moment group DMs land
 * (REQ-056): a three-person conversation has no single person to hang a row off,
 * which is why Slack's rows carry several names. Our schema already agrees:
 * migration 0019 keys a DM on `dm_key`, the sorted participant SET, so a
 * person-keyed list would have been a dead end against our own storage.
 *
 * Starting a new conversation is the compose button, not a row per human — the
 * reference lists conversations only. */
static struct { D2D1_RECT_F r; uint64_t cid; } g_dmrows[256];
static int g_n_dmrows;
static uint64_t g_dm_hover;
/* A DM we asked the server to open; selected as soon as it appears. Without
 * this, picking someone created the conversation and left you looking at the
 * list, with the new row somewhere in the sidebar. */
static uint64_t g_dm_pending;
static int g_dm_compose;              /* the "start a conversation" picker is up */
static D2D1_RECT_F g_dm_compose_btn;

/* The DM channel with `uid`, or NULL. */
static const oc_channel *dm_with(const oc_model *m, uint64_t uid) {
    for (size_t i = 0; i < m->n_channels; i++)
        if (m->channels[i].kind == OC_CHANNEL_KIND_DM && m->channels[i].peer_id == uid)
            return &m->channels[i];
    return NULL;
}

/* "15 mins" / "Yesterday" / "12 Jul" — a DM list is scanned, and an absolute
 * timestamp on every row makes it a table to read rather than a list to skim. */
static void rel_time(uint64_t ms, char *out, size_t cap) {
    out[0] = '\0';
    if (!ms) return;
    time_t t = (time_t)(ms / 1000), now = time(NULL);
    double d = difftime(now, t);
    if (d < 60)         snprintf(out, cap, "now");
    else if (d < 3600)  snprintf(out, cap, "%d min", (int)(d / 60));
    else if (d < 86400) snprintf(out, cap, "%dh", (int)(d / 3600));
    else if (d < 172800) snprintf(out, cap, "Yesterday");
    else {
        struct tm tv;
        if (oc_localtime_r(&t, &tv)) strftime(out, cap, "%d %b", &tv);
    }
}

static void draw_dm_list(ID2D1RenderTarget *rt, const oc_model *m, float h) {
    float x0 = RAIL_W, x1 = RAIL_W + SIDEBAR_W;
    fill(rt, rf(x0, 0, x1, h), OC_COL_SIDEBAR);

    draw_text(rt, "Direct messages", g_hdr, rf(x0 + 16, 0, x1 - 40, HEADER_H), OC_COL_TEXT);
    g_dm_compose_btn = rf(x1 - 36, 16, x1 - 12, 40);
    draw_lucide(rt, OC_ICON_SQUARE_PEN, rf(g_dm_compose_btn.left + 2, g_dm_compose_btn.top + 2,
                                           g_dm_compose_btn.right - 2, g_dm_compose_btn.bottom - 2),
                OC_COL_MUTED);

    g_n_dmrows = 0;
    float y = HEADER_H + 6;
    int any = 0;
    /* Newest first: a DM list is ordered by when you last spoke, not by name. */
    for (;;) {
        const oc_channel *best = NULL;
        for (size_t i = 0; i < m->n_channels; i++) {
            const oc_channel *c = &m->channels[i];
            if (c->kind != OC_CHANNEL_KIND_DM) continue;
            int taken = 0;
            for (int k = 0; k < g_n_dmrows; k++) if (g_dmrows[k].cid == c->channel_id) taken = 1;
            if (taken) continue;
            if (!best || c->last_message_at > best->last_message_at) best = c;
        }
        if (!best || y > h - 40) break;
        any = 1;

        D2D1_RECT_F row = rf(x0 + 6, y, x1 - 6, y + 52);
        if (g_dm_hover == best->channel_id || g_sel == best->channel_id)
            fill_round(rt, row, 6.0f, g_sel == best->channel_id ? OC_COL_SELECT : OC_COL_HOVER);

        const char *nm = oc_model_user_name((oc_model *)m, best->peer_id);
        if (!nm || !nm[0]) nm = "user";
        D2D1_ELLIPSE av = { { row.left + 24, y + 26 }, 15, 15 };
        ID2D1RenderTarget_FillEllipse(rt, &av, paint_with(AVPAL[best->peer_id % 6]));
        char ini[2] = { (char)(nm[0] >= 'a' && nm[0] <= 'z' ? nm[0] - 32 : nm[0]), 0 };
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, ini, g_ui, rf(row.left + 9, y + 11, row.left + 39, y + 41), 0xFFFFFF);
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_LEADING);
        draw_presence_dot(rt, row.left + 35, y + 37, 4.5f,
                          oc_model_presence_of(m, best->peer_id), OC_COL_SIDEBAR);

        int unread = best->unread > 0;
        char label[80];
        snprintf(label, sizeof label, "%s%s", nm,
                 best->peer_id == m->user_id ? " (you)" : "");
        draw_text(rt, label, unread ? g_ui_b : g_ui,
                  rf(row.left + 48, y + 5, row.right - 60, y + 25), OC_COL_TEXT);

        char when[24];
        rel_time(best->last_message_at, when, sizeof when);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_TRAILING);
        draw_text(rt, when, g_small, rf(row.left + 48, y + 6, row.right - 10, y + 24),
                  unread ? OC_COL_ACCENT : OC_COL_FAINT);
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);

        /* The preview, prefixed the way the reference does it so you can tell
         * whose turn it is at a glance. */
        char prev[160] = "";
        if (best->preview[0])
            snprintf(prev, sizeof prev, "%s%s",
                     best->preview_author == m->user_id ? "You: " : "", best->preview);
        draw_text(rt, prev[0] ? prev : "No messages yet", g_small,
                  rf(row.left + 48, y + 25, row.right - 10, y + 45),
                  unread ? OC_COL_TEXT : OC_COL_FAINT);

        if (g_n_dmrows < (int)(sizeof g_dmrows / sizeof g_dmrows[0])) {
            g_dmrows[g_n_dmrows].r = row;
            g_dmrows[g_n_dmrows].cid = best->channel_id;
            g_n_dmrows++;
        }
        y += 56;
    }
    if (!any)
        draw_text(rt, "No conversations yet \u2014 use the compose button.", g_small_w,
                  rf(x0 + 16, HEADER_H + 12, x1 - 16, HEADER_H + 60), OC_COL_FAINT);
}

/* The "start a conversation" picker: every person, which is what the compose
 * button is FOR. Not the resting state of the DMs view — the reference lists
 * conversations, and a roster masquerading as an inbox was my mistake. */
static struct { D2D1_RECT_F r; uint64_t uid; } g_pickrows[256];
static int g_n_pickrows;

static void draw_dm_compose(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    D2D1_RECT_F body = overlay_header(rt, reg, "New direct message");
    g_n_pickrows = 0;
    float y = body.top + 8;
    for (size_t i = 0; i < m->n_users && y < body.bottom; i++) {
        const oc_member *u = &m->users[i];
        if (u->disabled) continue;
        D2D1_RECT_F row = rf(body.left + 12, y, body.right - 12, y + 44);
        if (g_dm_hover == u->user_id) fill_round(rt, row, 6.0f, OC_COL_HOVER);
        D2D1_ELLIPSE av = { { row.left + 28, y + 22 }, 16, 16 };
        ID2D1RenderTarget_FillEllipse(rt, &av, paint_with(AVPAL[u->user_id % 6]));
        char ini[2] = { (char)(u->name[0] >= 'a' && u->name[0] <= 'z' ? u->name[0] - 32 : u->name[0]), 0 };
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, ini, g_ui, rf(row.left + 12, y + 6, row.left + 44, y + 38), 0xFFFFFF);
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_LEADING);
        char nm[96];
        snprintf(nm, sizeof nm, "%s%s", u->name[0] ? u->name : "user",
                 u->user_id == m->user_id ? " (you)" : "");
        draw_text(rt, nm, g_ui, rf(row.left + 56, y + 3, row.right - 20, y + 25), OC_COL_TEXT);
        draw_text(rt, u->user_id == m->user_id
                      ? "Message yourself \u2014 notes, links, reminders"
                      : (dm_with(m, u->user_id) ? "Open the conversation" : "Start a conversation"),
                  g_small, rf(row.left + 56, y + 23, row.right - 20, y + 41), OC_COL_FAINT);
        if (g_n_pickrows < (int)(sizeof g_pickrows / sizeof g_pickrows[0])) {
            g_pickrows[g_n_pickrows].r = row;
            g_pickrows[g_n_pickrows].uid = u->user_id;
            g_n_pickrows++;
        }
        y += 48;
    }
}

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

static void draw_lightbox(ID2D1RenderTarget *rt, float W, float H);   /* fwd */

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
    /* Sign-in owns the whole window only when there is nothing behind it. With a
     * workspace still connected the shell stays on screen, dimmed, so adding or
     * re-entering a workspace never blanks an app you are already using. */
    int si_over = (g_view == VIEW_SIGNIN) && shell_visible();
    if (g_view == VIEW_SIGNIN && !si_over) { draw_signin(rt, W, H); return; }
    if (!m) return;
    ensure_selection(m);
    draw_rail(rt, m, H);

    if (shell_visible()) {
        float main_x = RAIL_W + SIDEBAR_W;
        float members = (g_show_members && m->authed) ? MEMBERS_W : 0;
        float main_r = W - members, main_w = main_r - main_x;
        if (g_view == VIEW_DMS) draw_dm_list(rt, m, H); else draw_sidebar(rt, m, H);
        const oc_channel *selc0 = g_sel ? oc_model_channel((oc_model *)m, g_sel) : NULL;
        int dm_index0 = (g_view == VIEW_DMS &&
                         (g_dm_compose || !(selc0 && selc0->kind == OC_CHANNEL_KIND_DM)));
        if (!dm_index0) draw_header(rt, m, main_x, main_w);
        float th = dm_index0 ? 0 : draw_tabbar(rt, m, main_x, main_w);
        float bh = draw_banner(rt, m, main_x, main_w, th);  /* pushes the transcript down */
        {
            /* In the DMs view, the middle column is the PERSON list until a
             * conversation is picked — that is what makes it a destination
             * rather than Home with a section folded. */
            const oc_channel *selc = g_sel ? oc_model_channel((oc_model *)m, g_sel) : NULL;
            int dm_index = (g_view == VIEW_DMS &&
                            (g_dm_compose || !(selc && selc->kind == OC_CHANNEL_KIND_DM)));
            if (dm_index) {
                if (g_dm_compose) draw_dm_compose(rt, m, rf(main_x, 0, main_r, H - g_composer_h));
                else overlay_empty(rt, rf(main_x, 0, main_r, H - g_composer_h),
                                   "Pick a conversation, or start a new one.");
            } else {
                draw_transcript(rt, m, rf(main_x, HEADER_H + th + bh, main_r, H - g_composer_h));
            }
        }
        draw_composer(rt, main_x, main_w, H);
        draw_autocomplete(rt, main_x, main_w, H);
        draw_emoji_picker(rt, main_x, main_w, H);
        if (members > 0) draw_members(rt, m, W, H);
        else g_n_memrows = 0;
    } else {
        g_n_ac = 0;
        g_pick_open = 0;
        g_banner_on = 0;
        D2D1_RECT_F reg = rf(RAIL_W, 0, W, H);
        switch (g_view) {
            case VIEW_ACTIVITY:      draw_stub_view(rt, reg, "Activity", "Activity feed \xE2\x80\x94 coming soon"); break;
            case VIEW_FILES:         draw_files_view(rt, m, reg); break;
            case VIEW_LATER:         draw_stub_view(rt, reg, "Later", "Saved items \xE2\x80\x94 coming soon"); break;
            case VIEW_ADMIN:         draw_admin(rt, m, reg); break;
            default:                 draw_stub_view(rt, reg, "OpenChime", ""); break;
        }
        g_n_memrows = 0;
    }
    draw_modal(rt, m, W, H);  /* your-account surfaces, over a dimmed shell */
    draw_more_flyout(rt);   /* floats over the pane when open */
    draw_palette(rt, m, W, H);   /* the palette dims and covers the app */
    draw_menu(rt);          /* dropdown menus float on top of everything */
    if (si_over) {          /* the sign-in card, over a dimmed live shell */
        D2D1_RECT_F all = rf(0, 0, W, H);
        ID2D1RenderTarget_FillRectangle(rt, &all, paint_alpha(0x000000, 0.55f));
        draw_signin(rt, W, H);
    }
    draw_lightbox(rt, W, H);   /* the expanded image covers everything */
    draw_toasts(rt, W, H);  /* …and failure notices float above even those */
}

static void layout_search(HWND hwnd);
static void layout_find(HWND hwnd);    /* fwd */

static void paint(HWND hwnd) {
    d2d_ensure_rt(hwnd);
    if (!g_rt || !g_brush) return;
    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)g_rt;
    RECT rc; GetClientRect(hwnd, &rc);
    /* DIPs, not pixels — the drawing coordinate space now that the target
     * carries the window's DPI. */
    float W = DIPF(rc.right - rc.left), H = DIPF(rc.bottom - rc.top);
    const oc_model *m = model();

    /* Both are created with the target but coloured from the theme, which can
     * change without the target being recreated. Cheaper to re-set them each
     * frame than to remember every place a theme switch can happen. */
    if (g_brush2) { D2D1_COLOR_F c2 = col(OC_COL_FAINT);  ID2D1SolidColorBrush_SetColor(g_brush2, &c2); }
    if (g_brush3) { D2D1_COLOR_F c3 = col(OC_COL_ACCENT); ID2D1SolidColorBrush_SetColor(g_brush3, &c3); }

    ID2D1RenderTarget_BeginDraw(rt);
    render_scene(rt, m, W, H);
    /* Boxes placed against chrome the scene just measured, so they can only be
     * positioned after the scene is laid out. `layout_find` is here for a
     * different reason: it has to react to a menu opening, which is not a
     * relayout and so never reached it. */
    layout_find(hwnd);
    layout_search(hwnd);
    if (g_pal_edit) {
        if (g_pal_open) {
            ShowWindow(g_pal_edit, SW_SHOW);
            MoveWindow(g_pal_edit, PX(g_pal_box.left + 32), PX(g_pal_box.top + 8),
                       PX(g_pal_box.right - g_pal_box.left - 44), PX(20), TRUE);
        } else {
            ShowWindow(g_pal_edit, SW_HIDE);
        }
    }
    if (g_pick_edit) {
        if (g_pick_open) {
            ShowWindow(g_pick_edit, SW_SHOW);
            MoveWindow(g_pick_edit, PX(g_pick_box.left + 30), PX(g_pick_box.top + 6),
                       PX(g_pick_box.right - g_pick_box.left - 40), PX(18), TRUE);
        } else {
            ShowWindow(g_pick_edit, SW_HIDE);
        }
    }

    HRESULT hr = ID2D1RenderTarget_EndDraw(rt, NULL, NULL);
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {
        thumbs_drop();   /* the bitmaps belong to the target that just died */
        if (g_brush) { ID2D1SolidColorBrush_Release(g_brush); g_brush = NULL; }
        if (g_brush2) { ID2D1SolidColorBrush_Release(g_brush2); g_brush2 = NULL; }
        if (g_brush3) { ID2D1SolidColorBrush_Release(g_brush3); g_brush3 = NULL; }
        ID2D1HwndRenderTarget_Release(g_rt);
        g_rt = NULL;
    }
}

/* ---- composer (native RichEdit) ------------------------------------------ */

/* Send the composer's text to the selected channel and clear it. */
/* ---- composer autocomplete (WIN-7) ---------------------------------------
 * Everything works in UTF-16 for the RichEdit and UTF-8 for the core: the text
 * up to the caret is converted once for oc_complete(), while the replacement
 * range is found by scanning the UTF-16 buffer back to the token's whitespace
 * boundary. Keeping the two independent avoids converting offsets between the
 * encodings, which is where this kind of code usually goes wrong. */

/* The caret, and the start of the token it sits in, as UTF-16 indices. */
static int ac_token_range(WCHAR *buf, int cap, int *tok_start) {
    if (!g_re) return -1;
    DWORD sels = 0, sele = 0;
    SendMessageW(g_re, EM_GETSEL, (WPARAM)&sels, (LPARAM)&sele);
    if (sels != sele) return -1;                 /* a live selection: not completing */
    int len = GetWindowTextLengthW(g_re);
    if (len <= 0 || len >= cap) return -1;
    GetWindowTextW(g_re, buf, cap);
    int caret = (int)sele;
    if (caret > len) caret = len;
    int ts = 0;
    for (int i = caret - 1; i >= 0; i--)
        if (buf[i] == L' ' || buf[i] == L'\t' || buf[i] == L'\n' || buf[i] == L'\r') { ts = i + 1; break; }
    *tok_start = ts;
    return caret;
}

static void ac_close(void) { g_n_ac = 0; g_ac_sel = 0; }

static void ac_rebuild(void) {
    const oc_model *m = model();
    WCHAR buf[4096]; int ts = 0;
    int caret = m ? ac_token_range(buf, 4096, &ts) : -1;
    if (caret < 0 || caret == ts) { ac_close(); return; }

    /* Only the text up to the caret is a completion context — what follows is
     * already-typed content the user moved back past. */
    buf[caret] = 0;
    char u8[4096];
    if (WideCharToMultiByte(CP_UTF8, 0, buf, -1, u8, sizeof u8, NULL, NULL) <= 0) { ac_close(); return; }

    g_n_ac = (int)oc_complete(m, u8, g_ac, AC_MAX, NULL, &g_ac_kind);
    if (g_ac_sel >= g_n_ac) g_ac_sel = 0;
}

/* Replace the trailing token with the selected candidate, plus a trailing space
 * so the next word starts cleanly. */
static void ac_accept(void) {
    if (g_n_ac <= 0 || !g_re) return;
    WCHAR buf[4096]; int ts = 0;
    int caret = ac_token_range(buf, 4096, &ts);
    if (caret < 0) { ac_close(); return; }

    WCHAR repl[128];
    int n = MultiByteToWideChar(CP_UTF8, 0, g_ac[g_ac_sel].repl, -1, repl, 126);
    if (n <= 0) { ac_close(); return; }
    repl[n - 1] = L' '; repl[n] = 0;             /* overwrite the NUL with a space */

    SendMessageW(g_re, EM_SETSEL, (WPARAM)ts, (LPARAM)caret);
    SendMessageW(g_re, EM_REPLACESEL, TRUE, (LPARAM)repl);
    ac_close();
}

static void composer_send(void) {
    if (!g_re || !g_client || !g_sel) return;
    /* Refuse locally in an archived channel so the text is not lost to a server
     * rejection you have to read in a toast (REQ-035). The daemon refuses it
     * too — this is the courteous half, not the enforcing one. */
    {
        const oc_model *m = model();
        const oc_channel *c = m ? oc_model_channel((oc_model *)m, g_sel) : NULL;
        if (c && c->archived) {
            toast_push("This channel is archived \u2014 it is read-only.", 1);
            return;
        }
    }
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
    ac_close();
    /* Your own message ends the unread run; leaving the divider above it would
     * claim there is still something new to read. */
    g_unread_from = 0; g_unread_count = 0;
    for (int i = 0; i < g_n_drafts; i++)          /* it is sent; not a draft any more */
        if (g_drafts[i].cid == g_sel) { g_drafts[i] = g_drafts[--g_n_drafts]; break; }
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

/* The composer's placeholder. It has to be painted INSIDE the control: the
 * RichEdit is opaque and composites above the Direct2D chrome, so anything the
 * scene draws behind it is hidden. RichEdit also ignores EM_SETCUEBANNER, which
 * is what the other boxes use.
 *
 * Worth the trouble because the composer said nothing about where the message
 * was going — and with several workspaces open (WIN-29) "which conversation is
 * this?" is a real question. */
static char g_ph[160];
static HFONT g_ph_font;

static void composer_placeholder(const oc_model *m) {
    const oc_channel *c = (m && g_sel) ? oc_model_channel((oc_model *)m, g_sel) : NULL;
    if (g_edit_msg)            snprintf(g_ph, sizeof g_ph, "Edit this message \u2014 Esc to cancel");
    /* Say why, not just that it is disabled (REQ-035). */
    else if (c && c->archived) snprintf(g_ph, sizeof g_ph,
                                        "#%s is archived \u2014 read-only",
                                        c->name ? c->name : "this channel");
    else if (m && m->thread_open) snprintf(g_ph, sizeof g_ph, "Reply\u2026");
    else if (!c)               snprintf(g_ph, sizeof g_ph, "Message");
    else if (c->kind == OC_CHANNEL_KIND_DM) {
        const char *pn = oc_model_user_name(m, c->peer_id);
        snprintf(g_ph, sizeof g_ph, "Message %s", (pn && pn[0]) ? pn : "them");
    } else
        snprintf(g_ph, sizeof g_ph, "Message #%s", c->name ? c->name : "channel");
}

static void composer_draw_placeholder(HWND hwnd) {
    if (!g_ph[0] || GetWindowTextLengthW(hwnd) > 0) return;
    HDC dc = GetDC(hwnd);
    if (!dc) return;
    if (!g_ph_font)
        g_ph_font = CreateFontW(-PX(BODY_DIP), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                                VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
    HFONT old = (HFONT)SelectObject(dc, g_ph_font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, OCRGB(OC_COL_FAINT));
    WCHAR w[160]; int n = to_w(g_ph, w, 160);
    RECT rc; GetClientRect(hwnd, &rc);
    /* Matches EM_SETMARGINS(12) and the control's own first-line origin. */
    TextOutW(dc, 0, PX(1), w, n);
    SelectObject(dc, old);
    ReleaseDC(hwnd, dc);
}

/* Subclass proc: Enter sends, Shift+Enter inserts a newline. While the
 * autocomplete popover is open it takes Up/Down/Tab/Enter/Esc first — Enter
 * accepting a candidate rather than sending is what makes the popover feel like
 * part of the composer instead of a thing floating over it. */
static void nav_conversation(HWND hwnd, int delta, int unread_only);   /* fwd */

static LRESULT CALLBACK re_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_PAINT) {
        /* Let the control paint, then overlay the cue on the empty field. */
        LRESULT r = CallWindowProcW(g_re_oldproc, hwnd, msg, wp, lp);
        composer_draw_placeholder(hwnd);
        return r;
    }
    if (g_n_ac > 0 && (msg == WM_KEYDOWN || msg == WM_CHAR)) {
        int handled = 1;
        switch (wp) {
        case VK_UP:     if (msg == WM_KEYDOWN) g_ac_sel = (g_ac_sel + g_n_ac - 1) % g_n_ac; break;
        case VK_DOWN:   if (msg == WM_KEYDOWN) g_ac_sel = (g_ac_sel + 1) % g_n_ac; break;
        case VK_TAB:
        case VK_RETURN: if (msg == WM_KEYDOWN) ac_accept(); break;
        case VK_ESCAPE: if (msg == WM_KEYDOWN) ac_close(); break;
        default: handled = 0;
        }
        if (handled) { InvalidateRect(GetParent(hwnd), NULL, FALSE); return 0; }
    }
    /* Conversation movement while typing: the composer holds focus almost
     * always, so these have to work from here or they do not work at all. */
    if (msg == WM_KEYDOWN && (GetKeyState(VK_MENU) & 0x8000) &&
        (wp == VK_UP || wp == VK_DOWN)) {
        nav_conversation(GetParent(hwnd), wp == VK_DOWN ? 1 : -1,
                         (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_F6) {
        SetFocus(g_find ? g_find : hwnd);
        return 0;
    }
    if ((msg == WM_KEYDOWN || msg == WM_CHAR) && wp == VK_RETURN
        && !(GetKeyState(VK_SHIFT) & 0x8000)) {
        if (msg == WM_KEYDOWN) composer_send();
        return 0;                                   /* eat both so no newline/bell */
    }
    if ((msg == WM_KEYDOWN || msg == WM_CHAR) && wp == VK_ESCAPE) {
        if (g_pick_open) { if (msg == WM_KEYDOWN) picker_close(GetParent(hwnd)); return 0; }
        /* modal_open() explicitly: a modal is not an `any_overlay` (it covers the
         * whole window, not the middle column), but Esc must still dismiss it. */
        if (modal_open() || any_overlay(model())) {
            if (msg == WM_KEYDOWN) close_overlays();
            return 0;
        }
        if (g_edit_msg) { if (msg == WM_KEYDOWN) composer_cancel_edit(); return 0; }
    }
    return CallWindowProcW(g_re_oldproc, hwnd, msg, wp, lp);
}

/* Recompute the composer's height from what is actually in it. EM_GETLINECOUNT
 * counts WRAPPED lines on a multiline control, so a long single paragraph grows
 * the box too — which is the case that made the old fixed height worst. Returns
 * 1 when the height changed, so the caller can re-lay-out rather than doing it
 * on every keystroke. */
static int composer_remeasure(void) {
    float want = COMPOSER_H;
    if (g_re) {
        int lines = (int)SendMessageW(g_re, EM_GETLINECOUNT, 0, 0);
        if (lines < 1) lines = 1;
        if (lines > COMPOSER_MAX_LINES) lines = COMPOSER_MAX_LINES;
        float th = (float)lines * COMPOSER_LINE;
        want = COMPOSER_CHROME + (th > COMPOSER_BTN ? th : COMPOSER_BTN);
    }
    if (want == g_composer_h) return 0;
    g_composer_h = want;
    return 1;
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
    rc.right = (LONG)DIPF(rc.right); rc.bottom = (LONG)DIPF(rc.bottom);
    const oc_model *m = model();
    float members = (g_show_members && m && m->authed) ? MEMBERS_W : 0;
    float main_x = RAIL_W + SIDEBAR_W;
    /* Inside the composer box, between the attach (+) and send buttons. */
    float bx0 = main_x + 20, bx1 = (rc.right - members) - 20;
    float by0 = rc.bottom - g_composer_h + COMPOSER_MT;
    float by1 = rc.bottom - COMPOSER_MB; (void)by1;
    /* The field sits between the left buttons and Send. Its height is the text,
     * top-aligned once the box is taller than one line so growth reads
     * downward; at rest it is centred against the buttons. */
    float sq = COMPOSER_BTN;
    float tx = bx0 + 6 + sq * 3 + 8, tr = bx1 - 6 - sq - 8;
    float inner = composer_inner_h();
    float texth = inner > COMPOSER_BTN ? inner : COMPOSER_LINE;
    float ty = by0 + COMPOSER_PAD + (inner > COMPOSER_BTN ? 0.0f : (COMPOSER_BTN - COMPOSER_LINE) / 2);
    MoveWindow(g_re, PX(tx), PX(ty), PX(tr - tx), PX(texth), TRUE);
}

/* Position the "Find a conversation" EDIT inside its sidebar box (transcript
 * views only). */
static void layout_find(HWND hwnd) {
    (void)hwnd;   /* the sidebar has a fixed width; geometry is constant */
    if (!g_find) return;

    /* A native child window composites ABOVE the parent's Direct2D output, so
     * this box punches a hole through anything the shell floats over the
     * sidebar — it was cutting the workspace menu's header block in half, which
     * read as a corrupt avatar and a missing workspace name. The D2D panels
     * cannot draw over it, so it has to get out of the way.
     *
     * The search and emoji boxes never showed this because each is already
     * gated on its own pane's open flag; the find box had no such guard. */
    /* Also hidden in the DMs view: that column is the DM list, not the channel
     * sidebar, so the box has nothing to filter and — being a native child that
     * composites ABOVE the D2D output — it floated over the first row as a bare
     * white rectangle. Same class of bug as the workspace menu it already
     * guards against, and the same fix: get out of the way. */
    int want = view_has_sidebar() && g_view != VIEW_DMS &&
               !g_menu && !g_more_open && !g_pal_open;

    /* Only act on a change: this runs every frame, and a redundant MoveWindow
     * still churns WM_WINDOWPOSCHANGED and can flicker the control. The DPI is
     * part of "changed" — the box is placed in device pixels, so a scale change
     * has to re-place it even though its visibility did not move. */
    static int shown = -1;
    static UINT laid_at_dpi = 0;
    if (want == shown && laid_at_dpi == g_dpi) return;
    shown = want;
    laid_at_dpi = g_dpi;
    if (!want) { ShowWindow(g_find, SW_HIDE); return; }
    int x = (int)(RAIL_W + 10 + 28), r = (int)(RAIL_W + SIDEBAR_W - 10 - 8);
    int top = (int)(HEADER_H + 6 + 6), hgt = 18;
    MoveWindow(g_find, PX(x), PX(top), PX(r - x), PX(hgt), TRUE);
    ShowWindow(g_find, SW_SHOW);
}

static WNDPROC g_find_oldproc;
static void nav_conversation(HWND hwnd, int delta, int unread_only);   /* fwd */

/* F6 has to work in both directions, or moving focus to the filter box strands
 * the keyboard there. Esc returns as well, since that is the reflex. */
static LRESULT CALLBACK find_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && (wp == VK_F6 || wp == VK_ESCAPE)) {
        SetFocus(g_re ? g_re : GetParent(hwnd));
        return 0;
    }
    if (msg == WM_CHAR && wp == VK_ESCAPE) return 0;    /* no MessageBeep */
    if (msg == WM_KEYDOWN && (GetKeyState(VK_MENU) & 0x8000) &&
        (wp == VK_UP || wp == VK_DOWN)) {
        nav_conversation(GetParent(hwnd), wp == VK_DOWN ? 1 : -1,
                         (GetKeyState(VK_SHIFT) & 0x8000) != 0);
        return 0;
    }
    return CallWindowProcW(g_find_oldproc, hwnd, msg, wp, lp);
}

static void find_create(HWND parent) {
    g_find = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL, 0, 0, 10, 10, parent,
        (HMENU)(INT_PTR)0xF1, GetModuleHandleW(NULL), NULL);
    if (!g_find) return;
    SendMessageW(g_find, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    SendMessageW(g_find, EM_SETCUEBANNER, TRUE, (LPARAM)L"Find a conversation…");
    g_find_oldproc = (WNDPROC)SetWindowLongPtrW(g_find, GWLP_WNDPROC, (LONG_PTR)find_proc);
    layout_find(parent);
}

/* Place the search-overlay query EDIT over the chrome draw_search() painted.
 * g_srch_box is only valid after a paint, so this runs from the paint path as
 * well as from WM_SIZE. */
static void layout_search(HWND hwnd) {
    const oc_model *m = model();
    if (!g_srch) return;
    if (!m || !m->search_open) { ShowWindow(g_srch, SW_HIDE); return; }
    (void)hwnd;
    ShowWindow(g_srch, SW_SHOW);
    MoveWindow(g_srch, PX(g_srch_box.left + 30), PX(g_srch_box.top + 8),
               PX(g_srch_box.right - g_srch_box.left - 40), PX(20), TRUE);
}

/* Enter submits the query; Escape closes the overlay. An EDIT swallows both, so
 * they are intercepted by a subclass rather than in the main key handler. */
static LRESULT CALLBACK srch_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static WNDPROC g_srch_prev;

static void search_create(HWND parent) {
    g_srch = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | ES_AUTOHSCROLL, 0, 0, 10, 10, parent,
        (HMENU)(INT_PTR)0xF2, GetModuleHandleW(NULL), NULL);
    if (!g_srch) return;
    SendMessageW(g_srch, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    SendMessageW(g_srch, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search messages…");
    g_srch_prev = (WNDPROC)SetWindowLongPtrW(g_srch, GWLP_WNDPROC, (LONG_PTR)srch_proc);
}

static void menu_dispatch(HWND hwnd, int cmd);   /* fwd */

static void palette_close(HWND hwnd) {
    g_pal_open = 0; g_pal_sel = 0;
    if (g_pal_edit) ShowWindow(g_pal_edit, SW_HIDE);
    if (g_re) SetFocus(g_re);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void palette_open(HWND hwnd) {
    g_pal_open = 1; g_pal_sel = 0;
    if (g_pal_edit) {
        SetWindowTextW(g_pal_edit, L"");
        ShowWindow(g_pal_edit, SW_SHOW);
        SetFocus(g_pal_edit);
    }
    InvalidateRect(hwnd, NULL, FALSE);
}

/* Run the highlighted row. Closing FIRST matters: several commands open a modal
 * form, and the palette must not still be on screen behind it. */
static void palette_accept(HWND hwnd) {
    if (g_pal_sel < 0 || g_pal_sel >= g_n_pal_rows) { palette_close(hwnd); return; }
    int cmd = g_pal_rows[g_pal_sel].cmd;
    uint64_t cid = g_pal_rows[g_pal_sel].cid;
    palette_close(hwnd);
    if (cid) { g_view = VIEW_HOME; close_overlays(); select_channel(cid); }
    else if (cmd) menu_dispatch(hwnd, cmd);
}

static LRESULT CALLBACK pal_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static WNDPROC g_pal_prev;

/* The expanded image: the full bitmap fitted to the window over a dimmed
 * backdrop. Drawn last so nothing overlaps it. */
static ID2D1Bitmap *thumb_get(ID2D1RenderTarget *rt, uint64_t id, UINT *w, UINT *h);  /* fwd */

static void draw_lightbox(ID2D1RenderTarget *rt, float W, float H) {
    if (!g_lightbox) return;
    UINT iw = 0, ih = 0;
    ID2D1Bitmap *bmp = thumb_get(rt, g_lightbox, &iw, &ih);
    if (!bmp || !iw || !ih) { return; }

    D2D1_RECT_F all = rf(0, 0, W, H);
    ID2D1RenderTarget_FillRectangle(rt, &all, paint_alpha(0x000000, 0.82f));

    /* Fit inside a margin, and never enlarge past 1:1 — blowing a small image
     * up to fill the window makes it look broken rather than bigger. */
    float mw = W - 96, mh = H - 96;
    float sc = mw / (float)iw;
    if (mh / (float)ih < sc) sc = mh / (float)ih;
    if (sc > 1.0f) sc = 1.0f;
    float dw = (float)iw * sc, dh = (float)ih * sc;
    D2D1_RECT_F dst = rf((W - dw) / 2, (H - dh) / 2, (W + dw) / 2, (H + dh) / 2);
    ID2D1RenderTarget_DrawBitmap(rt, bmp, &dst, 1.0f,
                                 D2D1_BITMAP_INTERPOLATION_MODE_LINEAR, NULL);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(rt, "Click anywhere or press Esc to close", g_small,
              rf(0, dst.bottom + 10, W, dst.bottom + 32), OC_COL_MUTED);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
}

/* Decode `len` bytes into an RT bitmap and cache it under `id`. WIC works from
 * an IStream, so the buffer is wrapped rather than copied to disk. */
static void thumb_decode(uint64_t id, const uint8_t *data, size_t len) {
    if (!g_rt || !data || !len) return;
    if (!g_wic &&
        FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&g_wic)))
        return;

    UINT iw = 0, ih = 0;
    IWICStream *stream = NULL;
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    ID2D1Bitmap *bmp = NULL;

    if (SUCCEEDED(IWICImagingFactory_CreateStream(g_wic, &stream)) &&
        SUCCEEDED(IWICStream_InitializeFromMemory(stream, (BYTE *)data, (DWORD)len)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(
            g_wic, (IStream *)stream, NULL, WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(g_wic, &conv)) &&
        /* D2D wants premultiplied BGRA whatever the source format was. */
        SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame,
                                                 &GUID_WICPixelFormat32bppPBGRA,
                                                 WICBitmapDitherTypeNone, NULL, 0.0,
                                                 WICBitmapPaletteTypeMedianCut))) {
        IWICBitmapSource_GetSize((IWICBitmapSource *)conv, &iw, &ih);
        ID2D1RenderTarget_CreateBitmapFromWicBitmap((ID2D1RenderTarget *)g_rt,
                                                    (IWICBitmapSource *)conv, NULL, &bmp);
    }
    if (conv)   IWICFormatConverter_Release(conv);
    if (frame)  IWICBitmapFrameDecode_Release(frame);
    if (dec)    IWICBitmapDecoder_Release(dec);
    if (stream) IWICStream_Release(stream);

    if (!bmp) {   /* not decodable: remember, so we do not re-fetch every frame */
        if (g_n_thumb_missing < THUMB_CACHE) g_thumb_missing[g_n_thumb_missing++] = id;
        return;
    }
    if (g_n_thumbs == THUMB_CACHE) {          /* oldest out */
        if (g_thumbs[0].bmp) ID2D1Bitmap_Release(g_thumbs[0].bmp);
        memmove(&g_thumbs[0], &g_thumbs[1], (THUMB_CACHE - 1) * sizeof g_thumbs[0]);
        g_n_thumbs--;
    }
    g_thumbs[g_n_thumbs].id = id;
    g_thumbs[g_n_thumbs].bmp = bmp;
    g_thumbs[g_n_thumbs].w = iw;
    g_thumbs[g_n_thumbs].h = ih;
    g_n_thumbs++;
}

/* Run whatever is currently in the query box. */
static void search_submit(void) {
    if (!g_srch || !g_client) return;
    WCHAR w[256]; GetWindowTextW(g_srch, w, 256);
    char q[256];
    if (WideCharToMultiByte(CP_UTF8, 0, w, -1, q, sizeof q, NULL, NULL) <= 0) return;
    if (!q[0]) return;
    g_srch_scroll = 0;
    oc_client_search(g_client, q);
}

/* Open the overlay with the box focused and empty (WIN-4) — no modal prompt. */
static void search_open(HWND hwnd) {
    if (!g_client) return;
    g_view = VIEW_HOME;
    g_srch_scroll = 0;
    oc_client_open_search(g_client);
    SetWindowTextW(g_srch, L"");
    layout_search(hwnd);
    SetFocus(g_srch);
    InvalidateRect(hwnd, NULL, FALSE);
}

static LRESULT CALLBACK pal_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    HWND parent = GetParent(hwnd);
    if (msg == WM_KEYDOWN) {
        switch (wp) {
        case VK_ESCAPE: palette_close(parent); return 0;
        case VK_RETURN: palette_accept(parent); return 0;
        case VK_UP:     g_pal_sel--; InvalidateRect(parent, NULL, FALSE); return 0;
        case VK_DOWN:   g_pal_sel++; InvalidateRect(parent, NULL, FALSE); return 0;
        default: break;
        }
    }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;   /* no bell */
    return CallWindowProcW(g_pal_prev, hwnd, msg, wp, lp);
}

static LRESULT CALLBACK srch_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_KEYDOWN && wp == VK_RETURN) { search_submit(); return 0; }
    if (msg == WM_CHAR && (wp == VK_RETURN || wp == VK_ESCAPE)) return 0;  /* no MessageBeep */
    if (msg == WM_KEYDOWN && wp == VK_ESCAPE) {
        HWND parent = GetParent(hwnd);
        if (g_client) oc_client_close_search(g_client);
        ShowWindow(hwnd, SW_HIDE);
        SetFocus(g_re ? g_re : parent);
        InvalidateRect(parent, NULL, FALSE);
        return 0;
    }
    return CallWindowProcW(g_srch_prev, hwnd, msg, wp, lp);
}

/* Place the sign-in EDITs over the field chrome draw_signin() paints. The two
 * must agree on geometry, so both derive it from the same card maths — keep the
 * pad/step offsets here in step with that function. */
static void layout_signin(HWND hwnd) {
    if (!g_si_e_ws) return;
    int on = (g_view == VIEW_SIGNIN && !g_si_connecting);
    ShowWindow(g_si_e_ws,   (on && g_si_step == 1) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_si_e_user, (on && g_si_step == 2) ? SW_SHOW : SW_HIDE);
    ShowWindow(g_si_e_pass, (on && g_si_step == 2) ? SW_SHOW : SW_HIDE);
    if (!on) return;

    RECT rc; GetClientRect(hwnd, &rc);
    si_geom g = si_layout(DIPF(rc.right), DIPF(rc.bottom));
    float y = g.fields_y;

    /* Inset inside the drawn 32px-tall rounded box. */
    int ex = (int)(g.fx + 12), ew = (int)(g.fw - 24), eh = 20;
    if (g_si_step == 1) {
        /* Leave room for the ".openchime.io" chip the painter draws at the right
         * edge of the box, so typed text can never run under it. */
        int sw = g_si_advanced ? 0 : (int)(8 + 7.0 * (double)(strlen(oc_default_suffix()) + 1));
        MoveWindow(g_si_e_ws, PX(ex), PX(y + 20 + 6), PX(ew - sw), PX(eh), TRUE);
    } else {
        MoveWindow(g_si_e_user, PX(ex), PX(y + 20 + 6), PX(ew), PX(eh), TRUE);
        MoveWindow(g_si_e_pass, PX(ex), PX(y + 62 + 20 + 6), PX(ew), PX(eh), TRUE);
    }
}

/* Enter submits from any sign-in field. Subclassed rather than left to
 * IsDialogMessage's default-button handling, which we have no default button
 * for (the buttons are D2D-drawn) — and eating WM_CHAR too silences the EDIT's
 * beep on Enter, exactly as re_proc does for the composer. */
static WNDPROC g_si_oldproc;
static LRESULT CALLBACK si_edit_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if ((msg == WM_KEYDOWN || msg == WM_CHAR) && wp == VK_RETURN) {
        if (msg == WM_KEYDOWN) signin_submit(GetParent(hwnd));
        return 0;
    }
    if ((msg == WM_KEYDOWN || msg == WM_CHAR) && wp == VK_ESCAPE) {
        if (msg == WM_KEYDOWN) signin_cancel(GetParent(hwnd));
        return 0;
    }
    return CallWindowProcW(g_si_oldproc, hwnd, msg, wp, lp);
}

static void signin_create(HWND parent) {
    HINSTANCE inst = GetModuleHandleW(NULL);
    DWORD base = WS_CHILD | WS_TABSTOP | ES_AUTOHSCROLL;
    g_si_e_ws   = CreateWindowExW(0, L"EDIT", L"", base, 0, 0, 10, 10, parent,
                                  (HMENU)(INT_PTR)0xF2, inst, NULL);
    g_si_e_user = CreateWindowExW(0, L"EDIT", L"", base, 0, 0, 10, 10, parent,
                                  (HMENU)(INT_PTR)0xF3, inst, NULL);
    g_si_e_pass = CreateWindowExW(0, L"EDIT", L"", base | ES_PASSWORD, 0, 0, 10, 10, parent,
                                  (HMENU)(INT_PTR)0xF4, inst, NULL);
    HWND all[3] = { g_si_e_ws, g_si_e_user, g_si_e_pass };
    for (int i = 0; i < 3; i++)
        if (all[i]) SendMessageW(all[i], WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
    if (g_si_e_ws)
        SendMessageW(g_si_e_ws, EM_SETCUEBANNER, TRUE, (LPARAM)L"your-workspace");
    for (int i = 0; i < 3; i++) {
        if (!all[i]) continue;
        g_si_oldproc = (WNDPROC)SetWindowLongPtrW(all[i], GWLP_WNDPROC, (LONG_PTR)si_edit_proc);
    }
    layout_signin(parent);
}

/* Re-skin the native children after a theme change. The D2D chrome repaints
 * itself from oc_theme[] every frame, but a RichEdit keeps its own background
 * and the EDIT brush is cached — leave them and the controls stay dark on a
 * light shell. */
static void theme_restyle_children(void) {
    if (g_find_brush) { DeleteObject(g_find_brush); g_find_brush = NULL; }
    if (!g_re) return;
    SendMessageW(g_re, EM_SETBKGNDCOLOR, 0, (LPARAM)OCRGB(OC_COL_INPUT));
    CHARFORMAT2W cf; ZeroMemory(&cf, sizeof cf);
    cf.cbSize = sizeof cf;
    cf.dwMask = CFM_COLOR;
    cf.crTextColor = OCRGB(OC_COL_TEXT);
    SendMessageW(g_re, EM_SETCHARFORMAT, SCF_ALL, (LPARAM)&cf);
}

/* Every theme switch goes through here so no caller can forget the children. */
static void theme_set(int mode) {
    oc_theme_apply(mode);
    theme_restyle_children();
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
    /* No inner margin: the control is placed at the box's text inset already,
     * and a second margin pushed the caret visibly off the left edge. */
    SendMessageW(g_re, EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, MAKELONG(0, 0));
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
    /* A thread reply is not in the channel's message list, so look there too —
     * otherwise the menu simply never appeared for replies (WIN-15). */
    int is_reply = 0;
    uint64_t chan = g_sel;
    if (!msg && m->thread_open) {
        for (size_t i = 0; i < m->n_thread_msgs; i++)
            if (m->thread_msgs[i].message_id == mid) {
                msg = &m->thread_msgs[i]; is_reply = 1; chan = m->thread_channel; break;
            }
    }
    if (!msg) return;
    WCHAR wb[128];
    HMENU menu = CreatePopupMenu();
    /* The six quick reactions stay inline as one-click affordances, with the
     * full catalogue behind "More…" (WIN-8) rather than being the only choice. */
    HMENU react = CreatePopupMenu();
    for (int i = 0; i < g_n_quick; i++)
        AppendMenuW(react, MF_STRING, (UINT_PTR)(1 + i), wmenu(REACT_EMO[i], wb, 128));
    AppendMenuW(react, MF_SEPARATOR, 0, NULL);
    AppendMenuW(react, MF_STRING, 7, L"More\u2026");
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)react, L"React");
    for (int i = 0; i < msg->n_attach; i++) {
        char lbl[200];
        snprintf(lbl, sizeof lbl, "Download %s", msg->attach[i].filename);
        AppendMenuW(menu, MF_STRING | (msg->attach[i].reclaimed ? MF_GRAYED : 0),
                    (UINT_PTR)(30 + i), wmenu(lbl, wb, 128));
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    /* No nested threads (REQ-060), so a reply offers no thread item. */
    if (!msg->deleted && !is_reply)
        AppendMenuW(menu, MF_STRING, 100, msg->reply_count ? L"Open thread" : L"Reply in thread");
    if (msg->n_reactions) AppendMenuW(menu, MF_STRING, 102, L"Who reacted");
    /* A pin belongs to the channel, so this reads the same for everyone —
     * anyone may unpin, including someone else's pin (ARCH-90). A tombstone has
     * nothing to pin to. */
    if (!msg->deleted)
        AppendMenuW(menu, MF_STRING, 103,
                    msg->pinned ? L"Unpin from channel" : L"Pin to channel");
    AppendMenuW(menu, MF_STRING, 20, L"Copy text");
    int own = (msg->author_id == m->user_id);
    int canmod = own || self_role(m) >= OC_ROLE_ADMIN;
    if (own && !msg->deleted)    AppendMenuW(menu, MF_STRING, 21, L"Edit");
    if (canmod && !msg->deleted) AppendMenuW(menu, MF_STRING, 22, L"Delete");

    int cmd = (int)TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, sx, sy, 0, hwnd, NULL);
    DestroyMenu(menu);
    if (cmd >= 1 && cmd <= g_n_quick) {
        const char *e = REACT_EMO[cmd - 1];
        oc_client_react(g_client, chan, mid, e, reaction_is_mine(msg, e) ? 0 : 1);
    } else if (cmd == 7) {
        picker_open(hwnd, mid);
    } else if (cmd == 20) {
        copy_to_clipboard(hwnd, msg->body ? msg->body : "");
    } else if (cmd == 21) {
        composer_begin_edit(msg);
    } else if (cmd == 22) {
        oc_client_delete(g_client, chan, mid);
    } else if (cmd == 100) {
        g_scroll = 0; oc_client_open_thread(g_client, g_sel, mid);
    } else if (cmd == 102) {
        oc_client_list_reactions(g_client, chan, mid); rp_push(RP_REACTORS);
    } else if (cmd == 103) {
        oc_client_pin(g_client, chan, mid, msg->pinned ? OC_PIN_REMOVE : OC_PIN_ADD);
    } else if (cmd >= 30 && cmd - 30 < msg->n_attach) {
        download_attachment(hwnd, &msg->attach[cmd - 30]);
    }
}

static void show_member_menu(HWND hwnd, const oc_model *m, uint64_t uid, int sx, int sy) {
    int self = (uid == m->user_id);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, 2, L"View profile");
    if (!self) AppendMenuW(menu, MF_STRING, 1, L"Message");
    uint8_t me = self_role(m);
    if (me >= OC_ROLE_ADMIN && !self) {
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
    case 2:  g_profile_uid = uid; rp_push(RP_PROFILE); break;
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
static void theme_set(int mode);       /* fwd */
static void prefs_save(void);          /* fwd */
static void prefs_load(const oc_model *m);   /* fwd */

/* Clicks on the file list — shared by the channel's Files tab and the
 * workspace-wide Files view, which draw the same rows from the same model. */
static int files_click(HWND hwnd, int x, int y) {
    for (int i = 0; i < FF_KINDS; i++)
        if (in_rect(g_file_filters[i], (float)x, (float)y)) { g_file_filter = i; return 1; }
    for (int i = 0; i < FS_SCOPES; i++)
        if (in_rect(g_file_scopes[i], (float)x, (float)y)) { g_file_scope = i; return 1; }
    const oc_model *fm = model();
    for (int i = 0; i < g_n_filerows && fm; i++) {
        if ((size_t)g_filerows[i].ix >= fm->n_files) continue;
        const oc_file_view *f = &fm->files[g_filerows[i].ix];
        if (in_rect(g_filerows[i].dl, (float)x, (float)y)) {
            oc_attachment at = { f->id, {0}, {0}, f->size, f->reclaimed };
            snprintf(at.filename, sizeof at.filename, "%s", f->filename);
            snprintf(at.mime, sizeof at.mime, "%s", f->mime);
            download_attachment(hwnd, &at);
            return 1;
        }
        if (in_rect(g_filerows[i].row, (float)x, (float)y)) {
            /* Jump to the message it was shared with — a file is a thing
             * somebody said something about. From the workspace view that means
             * changing channel first, which is the point of showing which one. */
            if (f->message_id) {
                /* Always leave the Files VIEW, even when the file is in the
                 * channel already selected: otherwise select_tab closes the list
                 * we are standing in and leaves an empty page behind. */
                int from_view = (g_view == VIEW_FILES);
                if (f->channel_id && f->channel_id != g_sel) select_channel(f->channel_id);
                if (from_view) { g_view = VIEW_HOME; layout_composer(hwnd); }
                g_jump_mid = f->message_id;
                select_tab(TAB_MESSAGES);
            }
            return 1;
        }
    }
    return 0;
}

static int on_click(HWND hwnd, int x, int y) {
    /* A modal owns the window while it is up: a click outside the card dismisses
     * it, and nothing behind it is reachable. Tested first for that reason. */
    if (modal_open() && !in_rect(g_modal_card, (float)x, (float)y)) {
        close_overlays();
        return 1;
    }
    if (g_lightbox) { g_lightbox = 0; return 1; }   /* any click dismisses it */
    if (g_pal_open) {
        for (int i = 0; i < g_n_pal_rows; i++)
            if (in_rect(g_pal_rows[i].r, x, y)) { g_pal_sel = i; palette_accept(hwnd); return 1; }
        if (!in_rect(g_pal_panel, x, y)) palette_close(hwnd);
        return 1;
    }
    if (g_pick_open) {
        if (in_rect(g_pick_panel, x, y)) {
            for (int i = 0; i < g_n_pick_cells; i++)
                if (in_rect(g_pick_cells[i].r, x, y)) { picker_choose(hwnd, g_pick_cells[i].emoji); return 1; }
            return 1;   /* inside the panel but not on a cell: stay open */
        }
        picker_close(hwnd);
        return 1;
    }
    {
        const oc_model *om = model();
        if (om && om->storage_open && in_rect(g_storage_refresh, x, y)) {
            oc_client_storage_status(g_client);
            return 1;
        }
        if (om && om->audit_open) {
            for (int i = 0; i < g_n_audit_filters; i++)
                if (in_rect(g_audit_filters[i], x, y)) {
                    g_audit_family = i; g_ovl_scroll = 0; return 1;
                }
        }
        if (g_notify_open) {
            for (int i = 0; i < g_n_notify_hits; i++)
                if (in_rect(g_notify_hits[i].r, x, y)) {
                    oc_client_set_notify_pref(g_client, g_notify_hits[i].cid,
                                              g_notify_hits[i].level);
                    return 1;
                }
        }
    }
    if (g_profile_uid && in_rect(g_prof_dm_btn, x, y)) {
        uint64_t uid = g_profile_uid;
        close_overlays();
        oc_client_open_dm(g_client, uid);
        return 1;
    }
    if (g_wsmgr_open) {
        for (int i = 0; i < g_n_wsmgr_hits; i++) {
            if (!in_rect(g_wsmgr_hits[i].r, x, y)) continue;
            int row = g_wsmgr_hits[i].row;
            if (row < 0 || row >= g_n_sw) return 1;
            char ws[256], label[80], user[80];
            snprintf(ws, sizeof ws, "%s", g_sw[row].ws);
            snprintf(label, sizeof label, "%s", g_sw[row].label);
            snprintf(user, sizeof user, "%s", g_sw[row].user);
            int slot = ws_find(ws);
            int live = (slot >= 0 && g_wss[slot].client);
            switch (g_wsmgr_hits[i].act) {
            case WSM_GO:
                if (live) { close_overlays(); switch_workspace(hwnd, ws, ""); }
                else        signin_begin_known(hwnd, ws, user);
                break;
            case WSM_SIGNOUT:
                /* Route through the normal sign-out so the server revokes the
                 * session — a local drop would leave it valid elsewhere. */
                close_overlays();
                oc_client_logout(g_client, OC_LOGOUT_THIS);
                g_logging_out = 1;
                break;
            case WSM_FORGET: {
                WCHAR w[400]; char line[400];
                snprintf(line, sizeof line,
                         "Remove %s from this device?\n\n"
                         "Its saved sign-in is deleted. You can add it again by entering "
                         "its address.%s",
                         label[0] ? label : ws,
                         live ? "\n\nYou are currently signed in; this signs you out first." : "");
                to_w(line, w, 400);
                if (MessageBoxW(hwnd, w, L"Remove workspace",
                                MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK) return 1;
                if (live && slot == g_ws_active) {
                    close_overlays();
                    oc_client_logout(g_client, OC_LOGOUT_THIS);
                    g_logging_out = 1;
                    g_forget_after_logout = 1;      /* delete the entry once it lands */
                } else {
                    if (live) {                     /* a background one: stop it here */
                        oc_client_stop(g_wss[slot].client);
                        for (int k = slot; k + 1 < g_n_wss; k++) g_wss[k] = g_wss[k + 1];
                        g_n_wss--;
                        if (g_ws_active > slot) g_ws_active--;
                        g_n_notify_hw = 0;
                    }
                    ws_forget(ws);
                    sw_book_load();
                }
                break;
            }
            }
            return 1;
        }
        return 1;
    }
    if (g_prefs_open) {
        for (int i = 0; i < g_n_pref_hits; i++) {
            if (!in_rect(g_pref_hits[i].r, x, y)) continue;
            int v = g_pref_hits[i].val;
            switch (g_pref_hits[i].row) {
            case PREF_ROW_THEME:   theme_set(v); break;
            case PREF_ROW_TIME:    g_pref_time24 = v; break;
            /* Applying it live as well as saving it: a preference you have to
             * restart to see is indistinguishable from one that did nothing. */
            case PREF_ROW_MEMBERS: g_pref_members = v; g_show_members = v; layout_composer(hwnd); break;
            case PREF_ROW_DAYSEP:  g_pref_daysep = v; break;
            case PREF_ROW_NOTIFY:  g_pref_notify = v; break;
            case PREF_ROW_QUICK: {
                oc_field f[1] = { { FF_TEXT, "Quick reactions",
                                    "Up to six emoji shortcodes, comma separated (e.g. +1, fire, tada).", "" } };
                snprintf(f[0].value, sizeof f[0].value, "%s", g_quick_names);
                if (!form_dialog(hwnd, "Quick reactions", f, 1)) return 1;
                snprintf(g_quick_names, sizeof g_quick_names, "%s", f[0].value);
                quick_rebuild();
                break;
            }
            }
            prefs_save();
            return 1;
        }
    }
    /* The autocomplete popover floats over the transcript, so it claims clicks
     * inside it before anything underneath sees them. */
    if (g_n_ac > 0) {
        if (in_rect(g_ac_panel, x, y)) {
            for (int i = 0; i < g_n_ac; i++)
                if (in_rect(g_ac_rows[i], x, y)) { g_ac_sel = i; ac_accept(); SetFocus(g_re); break; }
            return 1;
        }
        ac_close();     /* a click anywhere else dismisses it, then falls through */
    }
    /* The sign-in view owns the window; nothing else is on screen. */
    if (g_view == VIEW_SIGNIN) {
        if (g_si_connecting) return 1;
        if (pt_in(g_si_btn, x, y))           { signin_submit(hwnd); return 1; }
        if (pt_in(g_si_cancel, x, y))        { signin_cancel(hwnd); return 1; }
        if (pt_in(g_si_back, x, y))          { signin_back(hwnd);   return 1; }
        if (pt_in(g_si_adv_link, x, y))      { signin_set_advanced(hwnd, !g_si_advanced); return 1; }
        if (pt_in(g_si_invite_link, x, y)) {
            oc_field f[3] = {
                { FF_TEXT,     "Invite token", "The one-time token you were sent.", "" },
                { FF_TEXT,     "Choose a username", "", "" },
                { FF_PASSWORD, "Choose a password", "", "" },
            };
            if (!form_dialog(hwnd, "Create your account", f, 3)) return 1;
            if (!f[0].value[0] || !f[1].value[0] || !f[2].value[0]) {
                snprintf(g_si_err, sizeof g_si_err, "invite token, username and password are all required");
                InvalidateRect(hwnd, NULL, FALSE);
                return 1;
            }
            snprintf(g_si_invite, sizeof g_si_invite, "%s", f[0].value);
            WCHAR wu[192], wp[192];
            to_w(f[1].value, wu, 192); to_w(f[2].value, wp, 192);
            if (g_si_e_user) SetWindowTextW(g_si_e_user, wu);
            if (g_si_e_pass) SetWindowTextW(g_si_e_pass, wp);
            signin_submit(hwnd);      /* redeems, because g_si_invite is set */
            return 1;
        }
        if (pt_in(g_si_remember_box, x, y) ||
            (g_si_step == 2 && y >= (int)g_si_remember_box.top &&
             y <= (int)g_si_remember_box.bottom &&
             x >= (int)g_si_remember_box.left && x < (int)g_si_remember_box.left + 140)) {
            g_si_remember = !g_si_remember; InvalidateRect(hwnd, NULL, FALSE); return 1;
        }
        return 1;
    }
    /* Toasts are painted above everything, so they must be hit-tested above
     * everything too — otherwise a toast over the composer eats a click it
     * appears to own. Clicking one dismisses it. */
    for (int i = g_n_toast - 1; i >= 0; i--)
        if (pt_in(g_toast_box[i], x, y)) { toast_drop(i); return 1; }
    /* Banner "Retry now": cut short the net thread's backoff sleep. */
    if (g_banner_on && pt_in(g_retry_btn, x, y)) {
        if (g_client) oc_client_reconnect(g_client);
        toast_push("Reconnecting\xE2\x80\xA6", 0);
        return 1;
    }
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
                if (act >= 0)                  {
                    g_view = act; layout_composer(hwnd);
                    /* Entering a report view fetches it: both are point-in-time
                     * and a stale one is worse than a moment's wait. */
                    if (act == VIEW_ADMIN) admin_select(g_adm_tab);
                    if (act == VIEW_FILES) oc_client_list_files(g_client, 0);   /* 0 = everywhere */
                }
                else if (act == NAV_MORE)      { g_more_open = !g_more_open; }
                else if (act == NAV_SWITCHER)  { open_switcher(hwnd); }
                else if (act == NAV_NEW)       { open_new_menu(hwnd); }
                else if (act == NAV_PROFILE)   { open_profile_menu(hwnd); }
                return 1;
            }
        return 1;   /* clicks in the rail gutter do nothing, but are swallowed */
    }
    if (g_view == VIEW_ADMIN)
        for (int i = 0; i < ADM_COUNT; i++)
            if (in_rect(g_adm_tabs[i], x, y)) { admin_select(i); return 1; }
    /* The Files view has no sidebar, so its clicks must be served before the
     * transcript-only guard below. */
    if (g_view == VIEW_FILES && files_click(hwnd, x, y)) return 1;
    if (g_view == VIEW_DMS) {
        if (in_rect(g_dm_compose_btn, x, y)) { g_dm_compose = !g_dm_compose; return 1; }
        for (int i = 0; i < g_n_dmrows; i++)
            if (in_rect(g_dmrows[i].r, x, y)) {
                g_dm_compose = 0;
                select_channel(g_dmrows[i].cid);
                return 1;
            }
        /* A person in the compose picker: open the conversation, creating it if
         * it does not exist yet. */
        for (int i = 0; i < g_n_pickrows; i++)
            if (in_rect(g_pickrows[i].r, x, y)) {
                const oc_model *dm_m = model();
                const oc_channel *ex = dm_m ? dm_with(dm_m, g_pickrows[i].uid) : NULL;
                g_dm_compose = 0;
                if (ex) select_channel(ex->channel_id);
                else { g_dm_pending = g_pickrows[i].uid; oc_client_open_dm(g_client, g_pickrows[i].uid); }
                return 1;
            }
    }
    /* Everything below is only meaningful in the transcript views. */
    if (!view_has_sidebar()) return 1;

    /* Header buttons + workspace header. */
    if (in_rect(g_hdr_gear, x, y))    { open_ws_menu(hwnd); return 1; }
    if (in_rect(g_hdr_compose, x, y)) { open_new_menu(hwnd); return 1; }
    /* The dot sits inside the workspace-header button, so it must be tested
     * first or the menu swallows it (WIN-64). It only exists while retrying is
     * meaningful — see draw_sidebar. */
    if (in_rect(g_ws_dot, x, y)) {
        oc_client_reconnect(g_client);
        toast_push("Reconnecting\u2026", 0);
        return 1;
    }
    if (in_rect(g_ws_hdr_btn, x, y))  { open_ws_menu(hwnd); return 1; }
    /* Section kebab -> that section's Filter/Sort menu. Slack's placement, and
     * the right one: these are PER-SECTION settings, so a single header gear
     * would have to ask which section you meant. */
    if (g_sb_hover_sec >= 0 && in_rect(g_sb_kebab, x, y)) {
        open_section_menu(hwnd, g_sb_hover_sec);
        return 1;
    }
    /* The channel tabs (WIN-37). Selecting a tab re-asks the server rather than
     * showing a cached list: pins and files change from other clients, and a
     * stale list is worse than a moment's load. */
    for (int t = 0; t < TAB_COUNT; t++)
        if (in_rect(g_tab_r[t], x, y)) { select_tab(t); return 1; }
    /* The member chip toggles the roster pane. */
    if (in_rect(g_memchip, x, y)) {
        g_show_members = !g_show_members;
        layout_composer(hwnd);
        return 1;
    }
    /* About-tab actions (REQ-034/035/036). */
    if (g_tab == TAB_ABOUT && g_sel) {
        const oc_model *am = model();
        const oc_channel *ac = am ? oc_model_channel((oc_model *)am, g_sel) : NULL;
        if (ac && in_rect(g_about_topic, x, y)) {
            oc_field f[1] = { { FF_TEXT, "Topic", "Shown in the channel header. Leave empty to clear it.", "" } };
            if (ac->topic) snprintf(f[0].value, sizeof f[0].value, "%s", ac->topic);
            if (form_dialog(hwnd, "Set channel topic", f, 1))
                oc_client_update_channel(g_client, g_sel, OC_CHUP_TOPIC, f[0].value);
            return 1;
        }
        if (ac && in_rect(g_about_rename, x, y)) {
            oc_field f[1] = { { FF_TEXT, "Channel name", "Lowercase, no spaces. The id does not change, so history and membership follow.", "" } };
            snprintf(f[0].value, sizeof f[0].value, "%s", ac->name ? ac->name : "");
            if (form_dialog(hwnd, "Rename channel", f, 1))
                oc_client_update_channel(g_client, g_sel, OC_CHUP_RENAME, f[0].value);
            return 1;
        }
        if (ac && in_rect(g_about_hooks, x, y)) {
            close_overlays();
            oc_client_webhooks(g_client, g_sel);
            return 1;
        }
        if (ac && in_rect(g_about_archive, x, y)) {
            if (ac->archived) {
                oc_client_update_channel(g_client, g_sel, OC_CHUP_UNARCHIVE, "");
            } else {
                /* Reversible, but it changes the channel for everyone — worth a
                 * confirm, unlike the topic. */
                WCHAR q[320]; char t[240];
                snprintf(t, sizeof t,
                         "Archive #%s?\n\nIt becomes read-only and disappears from the sidebar of "
                         "anyone who is not a member. History stays searchable, and you can "
                         "unarchive it later.", ac->name ? ac->name : "");
                to_w(t, q, 320);
                if (MessageBoxW(hwnd, q, L"Archive channel", MB_OKCANCEL | MB_ICONWARNING) == IDOK)
                    oc_client_update_channel(g_client, g_sel, OC_CHUP_ARCHIVE, "");
            }
            return 1;
        }
    }
    /* A reaction chip: +1 it, or undo it if it is already yours (REQ-070).
     * Tested before the message rows, which cover the same pixels. */
    for (int i = 0; i < g_n_chips; i++)
        if (in_rect(g_chips[i].r, x, y)) {
            /* A reply's chips belong to the thread's channel, not the selected
             * one — the same lookup the message menu does (WIN-15's lesson). */
            const oc_model *rm = model();
            uint64_t rch = g_sel;
            if (rm && rm->thread_open && !find_msg(oc_model_channel((oc_model *)rm, g_sel),
                                                   g_chips[i].mid))
                rch = rm->thread_channel;
            oc_client_react(g_client, rch, g_chips[i].mid, g_chips[i].emoji,
                            g_chips[i].mine ? OC_REACT_REMOVE : OC_REACT_ADD);
            return 1;
        }
    if (files_click(hwnd, x, y)) return 1;
    /* Rows of the open pins overlay. */
    for (int i = 0; i < g_n_pinrows; i++) {
        if (in_rect(g_pinrows[i].unpin, x, y)) {
            oc_client_pin(g_client, g_sel, g_pinrows[i].mid, OC_PIN_REMOVE);
            return 1;
        }
        if (in_rect(g_pinrows[i].row, x, y)) {
            /* Jump to it in the transcript — a pin is a pointer into the
             * conversation, so landing on it in context is the point. */
            g_jump_mid = g_pinrows[i].mid;
            oc_client_close_pins(g_client);
            return 1;
        }
    }
    /* Members toggle. */
    if (in_rect(g_members_btn, x, y)) {
        g_show_members = !g_show_members;
        layout_composer(hwnd);
        return 1;
    }
    /* Composer attach (+) and send buttons. */
    /* The hover toolbar is inside the image, so it must be tested first or the
     * image's own click (expand) swallows it. */
    for (int i = 0; i < g_n_thumb_dl; i++) {
        if (!in_rect(g_thumb_dl[i].r, x, y)) continue;
        const oc_model *tm = model();
        const oc_channel *tc = tm ? oc_model_channel((oc_model *)tm, g_sel) : NULL;
        const oc_msg *tmsg = find_msg(tc, g_thumb_dl[i].mid);
        int ix = g_thumb_dl[i].attach_ix;
        int kebab = ix < 0;
        if (kebab) ix = -ix - 1;
        if (!tmsg || ix < 0 || ix >= tmsg->n_attach) return 1;
        const oc_attachment *at = &tmsg->attach[ix];
        if (!kebab) { download_attachment(hwnd, at); return 1; }

        /* Slack's menu here also offers copy-link, save-for-later and share.
         * Those map to REQ-232, REQ-231 and REQ-057, none of which exist yet —
         * offering them greyed out would be four dead entries, so the menu is
         * only what actually works. */
        POINT pt = { PX(x), PX(y) };
        ClientToScreen(hwnd, &pt);
        HMENU mnu = CreatePopupMenu();
        AppendMenuW(mnu, MF_STRING, 1, L"View full size");
        AppendMenuW(mnu, MF_STRING, 2, L"Save image as\u2026");
        AppendMenuW(mnu, MF_SEPARATOR, 0, NULL);
        AppendMenuW(mnu, MF_STRING, 3, L"Copy filename");
        int cmd = (int)TrackPopupMenu(mnu, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, hwnd, NULL);
        DestroyMenu(mnu);
        if (cmd == 1)      g_lightbox = at->id;
        else if (cmd == 2) download_attachment(hwnd, at);
        else if (cmd == 3) copy_to_clipboard(hwnd, at->filename);
        return 1;
    }
    for (int i = 0; i < g_n_thumb_hits; i++)
        if (in_rect(g_thumb_hits[i].r, x, y)) { g_lightbox = g_thumb_hits[i].id; return 1; }
    if (in_rect(g_unread_jump, x, y)) {
        /* Jump to the first message past the marker, reusing the search-hit
         * machinery: same scroll-into-view and flash. */
        const oc_model *jm = model();
        const oc_channel *jc = jm ? oc_model_channel((oc_model *)jm, g_sel) : NULL;
        if (jc) for (size_t i = 0; i < jc->n_msgs; i++)
            if (jc->msgs[i].message_id > g_unread_from) {
                g_jump_mid = jc->msgs[i].message_id;
                g_jump_deadline = GetTickCount64() + 2000;
                break;
            }
        return 1;
    }
    if (in_rect(g_attach_btn, x, y)) { upload_file(hwnd); return 1; }
    if (in_rect(g_emoji_btn, x, y))  { picker_open(hwnd, 0); return 1; }
    if (in_rect(g_at_btn, x, y)) {
        /* Insert the trigger at the caret and let the normal completion path
         * take over, so the button and typing "@" behave identically. */
        if (g_re) {
            SetFocus(g_re);
            SendMessageW(g_re, EM_REPLACESEL, TRUE, (LPARAM)L"@");
            ac_rebuild();
            InvalidateRect(hwnd, NULL, FALSE);
        }
        return 1;
    }
    if (in_rect(g_send_btn, x, y))   { composer_send(); return 1; }
    /* Overlay row clicks (main area only). */
    {
        const oc_model *mm = model();
        if (mm && x > RAIL_W + SIDEBAR_W) {
            if (mm->search_open)
                for (int i = 0; i < g_n_searchrows; i++)
                    if ((float)y >= g_searchrows[i].top && (float)y < g_searchrows[i].bot) {
                        /* Arm the jump BEFORE selecting: select_channel closes the
                         * overlay and may fire the backfill this jump waits on. */
                        g_jump_mid = g_searchrows[i].mid;
                        g_jump_deadline = GetTickCount64() + 4000;
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
                if (g_rows[i].header) {
                    g_sb.collapsed[g_rows[i].sec] = (uint8_t)!g_sb.collapsed[g_rows[i].sec];
                    sidebar_opts_save();
                } else if (g_rows[i].cid != g_sel) {
                    select_channel(g_rows[i].cid);
                }
                return 1;
            }
        return 1;
    }
    /* Context-pane header: back pops to the member list, close hides the pane. */
    if (g_show_members && in_rect(g_rp_back, x, y))  { rp_pop(); return 1; }
    if (g_show_members && in_rect(g_rp_close, x, y)) {
        rp_pop(); g_show_members = 0; layout_composer(hwnd); return 1;
    }
    /* Members-pane rows: click opens the person's profile (WIN-10), which is
     * where "Message" now lives. Jumping straight into a DM made viewing someone
     * impossible, and it is the more destructive of the two actions. */
    for (int i = 0; i < g_n_memrows; i++)
        if (in_rect(g_memrows[i].r, x, y)) {
            g_profile_uid = g_memrows[i].uid;
            rp_push(RP_PROFILE);
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
    POINT pt = { PX(x), PX(y) };   /* back to device pixels for the popup */
    ClientToScreen(hwnd, &pt);
    /* Sidebar channel rows -> channel menu. */
    if (x >= RAIL_W && x <= RAIL_W + SIDEBAR_W) {
        for (int i = 0; i < g_n_rows; i++)
            if ((float)y >= g_rows[i].top && (float)y < g_rows[i].bot) {
                /* Right-clicking a header opens the same menu as its kebab. */
                if (g_rows[i].header) open_section_menu(hwnd, g_rows[i].sec);
                else show_channel_menu(hwnd, m, g_rows[i].cid, pt.x, pt.y);
                return;
            }
        return;
    }
    /* Members pane first (it overlaps the right edge). */
    for (int i = 0; i < g_n_memrows; i++)
        if (in_rect(g_memrows[i].r, (float)x, (float)y)) {
            show_member_menu(hwnd, m, g_memrows[i].uid, pt.x, pt.y);
            return;
        }
    /* Inside an open thread the replies own the region, so their rows are
     * checked first — and the same message menu applies, since a reply is an
     * ordinary message with an id (WIN-15). */
    if (m->thread_open) {
        for (int i = 0; i < g_n_thrrows; i++)
            if ((float)y >= g_thrrows[i].top && (float)y < g_thrrows[i].bot) {
                show_msg_menu(hwnd, m, g_thrrows[i].mid, pt.x, pt.y);
                return;
            }
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
    /* The client writes NOTHING to disk (ARCH-88), so this directory is never
     * created — the path exists only so a caller can distinguish "persistence
     * on" from NULL. We do still clear anything a pre-ARCH-88 build left, since
     * a stale state.db holds a cache and a plaintext token nobody reads now. */
    char old[1024];
    snprintf(old, sizeof old, "%s\\state.db", dir);     remove(old);
    snprintf(old, sizeof old, "%s\\state.db-wal", dir); remove(old);
    snprintf(old, sizeof old, "%s\\state.db-shm", dir); remove(old);
    _rmdir(dir);                                  /* only succeeds if now empty */
    snprintf(path, sizeof path, "%s\\state", dir);
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
    oc_store_set_secret(s, g_secret);   /* the book + token live in the credential store */
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
    if (oc_resolve(ws, oc_default_suffix(), &ep) != OC_RESOLVE_OK) return 0;
    const char *sp = store_path();
    oc_store *s = sp ? oc_store_open(sp) : NULL;
    if (!s) return 0;
    oc_store_set_secret(s, g_secret);   /* the book + token live in the credential store */
    /* The token lives in the OS credential store, so this probe has to look
     * there too — without the secret attached, oc_store_load_session() correctly
     * reports "no persisted session" and we would show the sign-in screen to a
     * user who is still signed in. */
    oc_store_set_secret(s, g_secret);
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
    oc_store_set_secret(s, g_secret);   /* the book + token live in the credential store */
    oc_store_workspace_remember(s, ws, ws, user, (uint64_t)time(NULL) * 1000);
    oc_store_close(s);
}

/* ---- N concurrent workspaces (WIN-29, REQ-012–015) -------------------------
 * The rail switcher used to stop the single client and start another, so a
 * background workspace received nothing and accrued no unread — you only found
 * out you had messages by switching to look.
 *
 * Rather than thread a workspace handle through the ~100 sites that use
 * g_client / g_sel / g_scroll, those stay as the ACTIVE workspace's state and a
 * slot array holds the rest. Switching saves the active globals into their slot
 * and loads the target's. Every client is ticked each frame; only the active one
 * is rendered. The whole diff is then confined to switching and ticking, which
 * is where the behaviour actually lives. */
/* (oc_ws_slot / g_wss are declared with the other globals near the top.) */

/* Unread across every workspace that is NOT the one on screen — the number the
 * rail badge exists to show. */
static int ws_unread_elsewhere(void) {
    int total = 0;
    for (int i = 0; i < g_n_wss; i++) {
        if (i == g_ws_active || !g_wss[i].client) continue;
        const oc_model *m = oc_client_model(g_wss[i].client);
        if (!m) continue;
        for (size_t c = 0; c < m->n_channels; c++) total += m->channels[c].unread;
    }
    return total;
}

static int ws_find(const char *ws) {
    for (int i = 0; i < g_n_wss; i++)
        if (strcmp(g_wss[i].ws, ws) == 0) return i;
    return -1;
}

/* Give the workspace now in g_client/g_cur_ws a slot and make it active. Both
 * entry points must call this — boot goes through connect_start, but the
 * interactive sign-in builds its client itself and used to skip registration
 * entirely, leaving a live client in no slot: invisible to the switcher, to the
 * tick loop's "other workspaces" pass, and to the unread badge. */
static void ws_register(void) {
    int slot = ws_find(g_cur_ws);
    if (slot < 0 && g_n_wss < WS_MAX) {
        slot = g_n_wss++;
        memset(&g_wss[slot], 0, sizeof g_wss[slot]);
        snprintf(g_wss[slot].ws, sizeof g_wss[slot].ws, "%s", g_cur_ws);
    }
    if (slot < 0) return;
    snprintf(g_wss[slot].cred, sizeof g_wss[slot].cred, "%s", g_cred);
    snprintf(g_wss[slot].host, sizeof g_wss[slot].host, "%s", g_host);
    g_wss[slot].port = g_port;
    g_wss[slot].client = g_client;
    g_wss[slot].sel = 0; g_wss[slot].scroll = 0;
    g_wss[slot].post_auth = 0; g_wss[slot].n_backfilled = 0;
    g_ws_active = slot;
}

/* The first workspace with a live client other than `except` (-1 for any) —
 * where to land when the one you were in goes away. */
static int ws_first_live(int except) {
    for (int i = 0; i < g_n_wss; i++)
        if (i != except && g_wss[i].client) return i;
    return -1;
}

/* Drop a workspace's stored session token, keeping its book entry so it stays
 * in the switcher. After a sign-out the server has revoked that token, so
 * leaving it behind means the next launch tries a corpse and falls back to the
 * sign-in view — which is what made returning feel worse than it should. */
static void ws_clear_session(const char *ws) {
    const char *sp = store_path();
    oc_store *st = (sp && ws && ws[0]) ? oc_store_open(sp) : NULL;
    if (!st) return;
    oc_store_set_secret(st, g_secret);
    oc_store_clear_session(st, ws);
    oc_store_close(st);
}

/* Remove a workspace from this device entirely (REQ-012): credential, TOFU pin
 * and book entry in one delete, so "forget" leaves nothing behind. */
static void ws_forget(const char *ws) {
    const char *sp = store_path();
    oc_store *st = (sp && ws && ws[0]) ? oc_store_open(sp) : NULL;
    if (!st) return;
    oc_store_set_secret(st, g_secret);
    oc_store_workspace_forget(st, ws);
    oc_store_close(st);
}

/* Park the active globals in their slot. */
static void ws_save_active(void) {
    if (g_ws_active < 0 || g_ws_active >= g_n_wss) return;
    oc_ws_slot *w = &g_wss[g_ws_active];
    w->client = g_client;
    w->sel = g_sel; w->scroll = g_scroll; w->post_auth = g_post_auth;
    w->n_backfilled = g_n_backfilled;
    for (int i = 0; i < g_n_backfilled && i < 64; i++) w->backfilled[i] = g_backfilled[i];
    snprintf(w->host, sizeof w->host, "%s", g_host);
    w->port = g_port;
}

/* Make slot `i` the one on screen. */
static void ws_load(int i) {
    if (i < 0 || i >= g_n_wss) return;
    oc_ws_slot *w = &g_wss[i];
    g_ws_active = i;
    g_client = w->client;
    g_sel = w->sel; g_scroll = w->scroll; g_post_auth = w->post_auth;
    g_n_backfilled = w->n_backfilled;
    for (int k = 0; k < w->n_backfilled && k < 64; k++) g_backfilled[k] = w->backfilled[k];
    snprintf(g_cur_ws, sizeof g_cur_ws, "%s", w->ws);
    snprintf(g_cred, sizeof g_cred, "%s", w->cred);
    snprintf(g_host, sizeof g_host, "%s", w->host);
    g_port = w->port;
    /* Cross-workspace leftovers: a selection, an edit or a toast from the other
     * workspace means nothing here. */
    g_has_sel = 0; g_edit_msg = 0; g_n_toast = 0; g_err_seen[0] = '\0';
    g_menu = MENU_NONE; g_more_open = 0;
}

/* Collect the remembered workspaces, so boot can connect them all. */
typedef struct { char ws[WS_MAX][256]; int n; } ws_book;

static void boot_book_cb(void *ctx, const char *workspace, const char *label,
                         const char *username, uint64_t last) {
    (void)label; (void)username; (void)last;
    ws_book *b = ctx;
    if (!workspace || !workspace[0] || b->n >= WS_MAX) return;
    snprintf(b->ws[b->n++], 256, "%s", workspace);
}

static void connect_start(const char *ws, const char *cred) {
    oc_endpoint ep;
    if (oc_resolve(ws, oc_default_suffix(), &ep) != OC_RESOLVE_OK) {
        snprintf(g_host, sizeof g_host, "%s", "?");
        return;
    }
    snprintf(g_host, sizeof g_host, "%s", ep.host);
    g_port = ep.port;
    snprintf(g_cur_ws, sizeof g_cur_ws, "%s", ws);
    snprintf(g_cred, sizeof g_cred, "%s", cred);
    g_client = oc_client_start_secure(g_host, g_port, g_cred, store_path(), g_secret);

    ws_register();

    /* Remember this workspace (+ the username, parsed off "user:pass") so the next
     * launch reconnects silently via the stored session token. */
    char user[128] = ""; const char *colon = strchr(cred, ':');
    if (colon && colon > cred) {
        size_t n = (size_t)(colon - cred); if (n >= sizeof user) n = sizeof user - 1;
        memcpy(user, cred, n); user[n] = 0;
    }
    remember_workspace(ws, user[0] ? user : NULL);
}

/* Connect every remembered workspace except `skip`, which is already up. Only
 * those with a stored session token: one without a credential would sit at a
 * failed connection with nothing to offer, and boot is not the place to ask. */
static void boot_other_workspaces(const char *skip) {
    const char *sp = store_path();
    oc_store *s = sp ? oc_store_open(sp) : NULL;
    if (!s) return;
    oc_store_set_secret(s, g_secret);
    ws_book b; b.n = 0;
    oc_store_workspace_each(s, boot_book_cb, &b);
    oc_store_close(s);

    int keep = g_ws_active;      /* -1 when nothing is up yet; ws_load handles it */
    for (int i = 0; i < b.n && g_n_wss < WS_MAX; i++) {
        if (skip && strcmp(b.ws[i], skip) == 0) continue;
        if (ws_find(b.ws[i]) >= 0) continue;
        if (!have_stored_token(b.ws[i])) continue;
        ws_save_active();
        g_client = NULL; g_sel = 0; g_scroll = 0; g_post_auth = 0; g_n_backfilled = 0;
        connect_start(b.ws[i], "");
    }
    /* Back to the one the user last used: the extras are background. */
    ws_save_active();
    if (keep >= 0) ws_load(keep);
}

/* ---- sign-in flow (WIN-2) -------------------------------------------------- */

/* Read a native EDIT's text as UTF-8. */
static void si_get(HWND e, char *out, size_t cap) {
    WCHAR w[320]; out[0] = '\0';
    if (!e) return;
    GetWindowTextW(e, w, (int)(sizeof w / sizeof w[0]));
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
}

/* Tear a session down to the state a fresh sign-in expects. Shared by the
 * workspace switcher and sign-out so the two can never drift. */
static void reset_session(void) {
    if (g_client) {
        /* Retire this workspace's slot as well: leaving it behind would keep a
         * freed client in the array for the switcher and the tick loop. */
        for (int i = 0; i < g_n_wss; i++)
            if (g_wss[i].client == g_client) {
                for (int k = i; k + 1 < g_n_wss; k++) g_wss[k] = g_wss[k + 1];
                g_n_wss--;
                break;
            }
        g_ws_active = -1;
        g_n_notify_hw = 0;      /* slot indices just shifted */
        oc_client_stop(g_client);
        g_client = NULL;
    }
    g_sel = 0; g_scroll = 0; g_post_auth = 0; g_has_sel = 0;
    g_n_backfilled = 0; g_more_open = 0; g_menu = MENU_NONE;
    g_edit_msg = 0; g_n_toast = 0; g_err_seen[0] = '\0'; g_err_seq = 0;
}

/* Sign back in to a workspace we already know: the book has its address and the
 * account that used it, so the address step is answered already. Land straight
 * on the password — which is the whole point of keeping the entry after a
 * sign-out rather than forgetting it. */
static void signin_begin_known(HWND hwnd, const char *ws, const char *user) {
    oc_endpoint ep;
    if (!ws || !ws[0] || oc_resolve(ws, oc_default_suffix(), &ep) != OC_RESOLVE_OK) {
        signin_begin(hwnd, ws, user);       /* unresolvable: let step 1 say so */
        return;
    }
    close_overlays();
    signin_begin(hwnd, ws, user);
    snprintf(g_si_host, sizeof g_si_host, "%s", ep.host);
    g_si_port = ep.port;
    g_si_step = 2;
    layout_signin(hwnd);
    /* Set the account AFTER the step-2 layout has shown the field. signin_begin
     * fills it while step 1 still has it hidden, which did not stick. */
    if (g_si_e_user && user && user[0]) {
        WCHAR wu[320]; to_w(user, wu, 320);
        SetWindowTextW(g_si_e_user, wu);
    }
    if (g_si_e_pass) SetFocus(g_si_e_pass);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* Abandon an overlay sign-in and go back to the workspace behind it. Only
 * possible when there IS one — at cold start there is nowhere to return to. */
static void signin_cancel(HWND hwnd) {
    if (!g_si_overlay || !g_client) return;
    if (g_si_client) { oc_client_stop(g_si_client); g_si_client = NULL; }
    g_si_connecting = 0; g_si_err[0] = '\0'; g_si_invite[0] = '\0';
    g_si_overlay = 0;
    g_view = VIEW_HOME;
    layout_signin(hwnd);
    layout_composer(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* Sign in to an ADDITIONAL workspace, leaving the current one connected. The
 * card renders over the live shell (see g_si_overlay) so the app never blanks
 * while you have somewhere to be. */
static void signin_begin_add(HWND hwnd) {
    close_overlays();
    /* The current workspace is left completely alone — still connected, still
     * rendered behind the card. It is parked only if the new sign-in succeeds. */
    signin_begin(hwnd, NULL, NULL);
}

/* Enter the sign-in view at step 1, pre-filled with `ws`/`user` when known. */
static void signin_begin(HWND hwnd, const char *ws, const char *user) {
    /* Overlay exactly when there is something worth keeping on screen. Covers
     * every caller: cold start and a last-workspace sign-out have no client and
     * get the full-window card; adding or re-entering a workspace has one and
     * gets the card over the live shell. */
    g_si_overlay = (g_client != NULL);
    g_view = VIEW_SIGNIN;
    g_si_step = 1; g_si_connecting = 0; g_si_err[0] = '\0';
    snprintf(g_si_ws, sizeof g_si_ws, "%s", ws ? ws : "");
    /* A remembered "chat.acme.com" or "127.0.0.1:8443" is a self-hosted address,
     * so open on the field that can actually hold it. */
    g_si_advanced = (g_si_ws[0] && (strchr(g_si_ws, '.') || strchr(g_si_ws, ':'))) ? 1 : 0;
    if (g_si_e_ws)
        SendMessageW(g_si_e_ws, EM_SETCUEBANNER, TRUE,
                     (LPARAM)(g_si_advanced ? L"chat.example.com or host:port" : L"your-workspace"));
    WCHAR w[320];
    if (g_si_e_ws)   { to_w(g_si_ws, w, 320);              SetWindowTextW(g_si_e_ws, w); }
    if (g_si_e_user) { to_w(user ? user : "", w, 320);     SetWindowTextW(g_si_e_user, w); }
    if (g_si_e_pass) SetWindowTextW(g_si_e_pass, L"");
    layout_signin(hwnd);
    layout_composer(hwnd);        /* hides the composer + find box for this view */
    if (g_si_e_ws) SetFocus(g_si_e_ws);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* Back to step 1 from step 2 (or from a failure), keeping what was typed. */
static void signin_back(HWND hwnd) {
    g_si_step = 1; g_si_connecting = 0; g_si_err[0] = '\0';
    layout_signin(hwnd);
    if (g_si_e_ws) SetFocus(g_si_e_ws);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* Switch step 1 between the hosted short-name field and the full-address field.
 * The cue banner and the field width change with it; what was typed is kept, so
 * flipping modes to correct a mistake never costs the user their input. */
static void signin_set_advanced(HWND hwnd, int on) {
    g_si_advanced = on;
    g_si_err[0] = '\0';
    if (g_si_e_ws)
        SendMessageW(g_si_e_ws, EM_SETCUEBANNER, TRUE,
                     (LPARAM)(on ? L"chat.example.com or host:port" : L"your-workspace"));
    layout_signin(hwnd);
    if (g_si_e_ws) SetFocus(g_si_e_ws);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* Fail the in-flight attempt: drop the client, return to the credential step
 * with `why`, and clear the password — the field that needs re-entering. */
static void signin_fail(HWND hwnd, const char *why) {
    /* Copy the reason BEFORE stopping the client: callers pass m->last_error,
     * which lives in the model that oc_client_stop() frees (oc_model_free +
     * free(c)). Reading it afterwards is a use-after-free — it silently produced
     * an empty error, which is exactly the silence this item exists to remove. */
    char tmp[192];
    snprintf(tmp, sizeof tmp, "%s", (why && why[0]) ? why : "sign-in failed");
    /* The core reports a rejected credential as the terse "auth failed"; say what
     * the TUI says, since that is the wording a user can act on. */
    if (strstr(tmp, "auth failed"))
        snprintf(tmp, sizeof tmp, "sign-in failed — check your username and password");

    if (g_si_client) { oc_client_stop(g_si_client); g_si_client = NULL; }
    g_si_connecting = 0; g_si_step = 2;
    snprintf(g_si_err, sizeof g_si_err, "%s", tmp);
    if (g_si_e_pass) SetWindowTextW(g_si_e_pass, L"");
    layout_signin(hwnd);
    if (g_si_e_pass) SetFocus(g_si_e_pass);
    InvalidateRect(hwnd, NULL, FALSE);
}

/* The primary button / Enter. Step 1 resolves the workspace (REQ-010/011) and
 * advances; step 2 starts the client and hands off to the tick, which watches
 * the model for the outcome (signin_poll). */
static void signin_submit(HWND hwnd) {
    g_si_err[0] = '\0';
    if (g_si_step == 1) {
        si_get(g_si_e_ws, g_si_ws, sizeof g_si_ws);
        if (!g_si_ws[0]) { snprintf(g_si_err, sizeof g_si_err,
                                    "enter a workspace (domain or name)"); goto redraw; }
        oc_endpoint ep;
        /* Same statuses and wording as the TUI's login box, so the two clients
         * describe an unresolvable workspace identically. */
        oc_resolve_status st = oc_resolve(g_si_ws, oc_default_suffix(), &ep);
        if (st == OC_RESOLVE_BAD_WORKSPACE) {
            snprintf(g_si_err, sizeof g_si_err, "invalid workspace '%s'", g_si_ws); goto redraw;
        }
        if (st != OC_RESOLVE_OK) {
            snprintf(g_si_err, sizeof g_si_err,
                     "'%s' not found — does not resolve in DNS", g_si_ws); goto redraw;
        }
        snprintf(g_si_host, sizeof g_si_host, "%s", ep.host);
        g_si_port = ep.port;
        g_si_step = 2;
        layout_signin(hwnd);
        if (g_si_e_user) SetFocus(g_si_e_user);
        goto redraw;
    }

    char user[128], pass[192];
    si_get(g_si_e_user, user, sizeof user);
    si_get(g_si_e_pass, pass, sizeof pass);
    if (!user[0]) {
        snprintf(g_si_err, sizeof g_si_err, "enter a username");
        if (g_si_e_user) SetFocus(g_si_e_user);
        goto redraw;
    }

    snprintf(g_host, sizeof g_host, "%s", g_si_host);
    g_port = g_si_port;
    snprintf(g_cur_ws, sizeof g_cur_ws, "%s", g_si_ws);
    snprintf(g_cred, sizeof g_cred, "%s:%s", user, pass);
    /* "Remember me" off means leave no trace: passing a NULL store path keeps
     * the session token out of the store entirely (the TUI's mechanism). */
    g_si_client = oc_client_start_secure(g_host, g_port, g_cred,
                                         g_si_remember ? store_path() : NULL,
                                         g_si_remember ? g_secret : NULL);
    if (!g_si_client) { snprintf(g_si_err, sizeof g_si_err, "could not start the client"); goto redraw; }
    /* Signup (WIN-32): with an invite in hand this connection redeems it instead
     * of authenticating — one step that creates the account and signs in — so
     * bringing up a tenant no longer needs the command line. */
    if (g_si_invite[0]) {
        oc_client_redeem_invite(g_si_client, g_si_invite);
        g_si_invite[0] = '\0';
    }
    g_si_connecting = 1;
    g_si_started = GetTickCount64();
    layout_signin(hwnd);
redraw:
    InvalidateRect(hwnd, NULL, FALSE);
}

/* Called each tick while an attempt is in flight. Mirrors the TUI's await_auth:
 * authed wins; a sticky last_error with no connection is the failure; and a
 * deadline stops us waiting forever on a black-hole endpoint. */
static void signin_poll(HWND hwnd) {
    const oc_model *m = g_si_client ? oc_client_model(g_si_client) : NULL;
    if (!m) return;
    if (m->authed) {
        /* Authenticated: NOW park whatever workspace was on screen and make this
         * the active one. Doing it here rather than at submit is what let the
         * previous workspace stay live and visible throughout. */
        ws_save_active();
        g_client = g_si_client; g_si_client = NULL;
        g_sel = 0; g_scroll = 0; g_post_auth = 0; g_has_sel = 0;
        g_n_backfilled = 0; g_edit_msg = 0; g_n_toast = 0; g_err_seen[0] = '\0';
        g_si_overlay = 0;
        g_si_connecting = 0;
        g_si_err[0] = '\0';
        g_view = VIEW_HOME;
        ws_register();               /* the client exists; give it a slot (WIN-29) */
        if (g_si_remember) {
            char user[128]; si_get(g_si_e_user, user, sizeof user);
            remember_workspace(g_si_ws, user[0] ? user : NULL);
        }
        if (g_si_e_pass) SetWindowTextW(g_si_e_pass, L"");   /* don't keep it in a control */
        layout_signin(hwnd);
        layout_composer(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    if (m->last_error[0] && !m->connected) { signin_fail(hwnd, m->last_error); return; }
    if (GetTickCount64() - g_si_started > SI_TIMEOUT) {
        char why[224]; snprintf(why, sizeof why, "timed out reaching %s", g_si_host);
        signin_fail(hwnd, why);
    }
}

/* ---- generic single-field modal prompt ----------------------------------- */

static void lg_set_font(HWND w) {
    SendMessageW(w, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
}

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

/* ---- generic multi-field modal form (WIN-21) ------------------------------
 * Six flows used to collapse into one single-line prompt, which is why the
 * password change had no confirm field and the DND window was an HH:MM-HH:MM
 * string parsed with sscanf. One form definition with typed fields replaces the
 * lot: each caller describes its fields and gets them back validated.
 *
 * Deliberately still native Win32 controls rather than D2D chrome — these are
 * short-lived modals, and the platform's own focus, tab order and IME handling
 * are worth more here than matching the shell's palette. */
/* oc_field / FF_* are declared near the top of the file so earlier code can
 * build forms; form_dialog() itself lives here. */

#define FORM_MAX_FIELDS 6
static HWND g_ff_ctl[FORM_MAX_FIELDS][4];   /* per field: up to 4 controls (choice radios) */

/* Returns 1 on OK with every field's `value` updated, 0 on Cancel. */
static int form_dialog(HWND owner, const char *title, oc_field *f, int n) {
    if (n > FORM_MAX_FIELDS) n = FORM_MAX_FIELDS;
    HINSTANCE inst = GetModuleHandleW(NULL);
    static int registered;
    if (!registered) {
        WNDCLASSW wc; memset(&wc, 0, sizeof wc);
        wc.lpfnWndProc = prompt_proc; wc.hInstance = inst;
        wc.hCursor = LoadCursorW(NULL, IDC_ARROW);
        wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
        wc.lpszClassName = L"OcForm";
        RegisterClassW(&wc); registered = 1;
    }

    /* Height is computed from the fields, so a two-field form is not padded out
     * to the size of a six-field one. */
    int y = 16, W = 420;
    int rowh[FORM_MAX_FIELDS];
    for (int i = 0; i < n; i++) {
        rowh[i] = (f[i].kind == FF_CHECK) ? 30 : (f[i].hint && f[i].hint[0] && f[i].kind != FF_CHOICE ? 68 : 52);
        y += rowh[i];
    }
    int H = y + 54 + 40;

    WCHAR wt[128]; to_w(title, wt, 128);
    RECT orc; GetWindowRect(owner, &orc);
    int sx = orc.left + ((orc.right - orc.left) - W) / 2;
    int sy = orc.top + ((orc.bottom - orc.top) - H) / 2;
    HWND dlg = CreateWindowExW(WS_EX_DLGMODALFRAME, L"OcForm", wt,
        WS_POPUP | WS_CAPTION | WS_SYSMENU, sx, sy, W, H, owner, NULL, inst, NULL);
    if (!dlg) return 0;

    memset(g_ff_ctl, 0, sizeof g_ff_ctl);
    y = 12;
    for (int i = 0; i < n; i++) {
        WCHAR wl[192]; to_w(f[i].label, wl, 192);
        if (f[i].kind == FF_CHECK) {
            HWND c = CreateWindowExW(0, L"BUTTON", wl,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                18, y + 4, W - 50, 22, dlg, NULL, inst, NULL);
            lg_set_font(c);
            SendMessageW(c, BM_SETCHECK, atoi(f[i].value) ? BST_CHECKED : BST_UNCHECKED, 0);
            g_ff_ctl[i][0] = c;
        } else if (f[i].kind == FF_CHOICE) {
            HWND s2 = CreateWindowExW(0, L"STATIC", wl, WS_CHILD | WS_VISIBLE,
                                      18, y, W - 50, 18, dlg, NULL, inst, NULL);
            lg_set_font(s2);
            int cur = atoi(f[i].value), k = 0, x = 18;
            const char *p2 = f[i].hint ? f[i].hint : "";
            while (*p2 && k < 4) {
                char opt[48]; int oi = 0;
                while (*p2 && *p2 != '|' && oi + 1 < (int)sizeof opt) opt[oi++] = *p2++;
                opt[oi] = 0;
                if (*p2 == '|') p2++;
                WCHAR wo[48]; to_w(opt, wo, 48);
                HWND rb = CreateWindowExW(0, L"BUTTON", wo,
                    WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTORADIOBUTTON | (k == 0 ? WS_GROUP : 0),
                    x, y + 20, 130, 22, dlg, NULL, inst, NULL);
                lg_set_font(rb);
                if (k == cur) SendMessageW(rb, BM_SETCHECK, BST_CHECKED, 0);
                g_ff_ctl[i][k] = rb;
                x += 135; k++;
            }
        } else {
            HWND s2 = CreateWindowExW(0, L"STATIC", wl, WS_CHILD | WS_VISIBLE,
                                      18, y, W - 50, 18, dlg, NULL, inst, NULL);
            lg_set_font(s2);
            WCHAR wv[192]; to_w(f[i].value, wv, 192);
            HWND e = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", wv,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL |
                (f[i].kind == FF_PASSWORD ? ES_PASSWORD : 0),
                18, y + 20, W - 54, 24, dlg, NULL, inst, NULL);
            lg_set_font(e);
            g_ff_ctl[i][0] = e;
            if (f[i].hint && f[i].hint[0]) {
                WCHAR wh[192]; to_w(f[i].hint, wh, 192);
                HWND h2 = CreateWindowExW(0, L"STATIC", wh, WS_CHILD | WS_VISIBLE,
                                          18, y + 46, W - 50, 18, dlg, NULL, inst, NULL);
                lg_set_font(h2);
            }
        }
        y += rowh[i];
    }

    HWND ok = CreateWindowExW(0, L"BUTTON", L"OK",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON, W - 200, y + 8, 84, 28,
        dlg, (HMENU)IDOK, inst, NULL);
    HWND cancel = CreateWindowExW(0, L"BUTTON", L"Cancel",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP, W - 108, y + 8, 84, 28, dlg, (HMENU)IDCANCEL, inst, NULL);
    lg_set_font(ok); lg_set_font(cancel);

    EnableWindow(owner, FALSE);
    ShowWindow(dlg, SW_SHOW);
    if (g_ff_ctl[0][0]) { SetFocus(g_ff_ctl[0][0]); SendMessageW(g_ff_ctl[0][0], EM_SETSEL, 0, -1); }
    g_pr_result = -1; g_pr_done = 0;

    MSG m;
    while (!g_pr_done && GetMessageW(&m, NULL, 0, 0) > 0)
        if (!IsDialogMessageW(dlg, &m)) { TranslateMessage(&m); DispatchMessageW(&m); }

    if (g_pr_result == 1) {
        for (int i = 0; i < n; i++) {
            if (f[i].kind == FF_CHECK) {
                snprintf(f[i].value, sizeof f[i].value, "%d",
                         SendMessageW(g_ff_ctl[i][0], BM_GETCHECK, 0, 0) == BST_CHECKED);
            } else if (f[i].kind == FF_CHOICE) {
                int pick = 0;
                for (int k = 0; k < 4; k++)
                    if (g_ff_ctl[i][k] && SendMessageW(g_ff_ctl[i][k], BM_GETCHECK, 0, 0) == BST_CHECKED)
                        pick = k;
                snprintf(f[i].value, sizeof f[i].value, "%d", pick);
            } else {
                WCHAR w[192]; GetWindowTextW(g_ff_ctl[i][0], w, 192);
                WideCharToMultiByte(CP_UTF8, 0, w, -1, f[i].value, (int)sizeof f[i].value, NULL, NULL);
            }
        }
    }
    EnableWindow(owner, TRUE);
    SetForegroundWindow(owner);
    if (IsWindow(dlg)) DestroyWindow(dlg);
    return g_pr_result == 1;
}

static void copy_to_clipboard(HWND hwnd, const char *utf8);   /* fwd */

/* A one-time secret (invite / webhook token). A MessageBox cannot be selected
 * from, so the token was easy to lose the moment it was dismissed — the one
 * thing that must not happen to a value shown exactly once (WIN-22). This gives
 * it a read-only-ish field, puts it on the clipboard immediately, and says
 * plainly that it will not be shown again. */
static void show_secret(HWND owner, const char *title, const char *what,
                        const char *token, const char *note) {
    static char label[192];
    oc_field f[1] = { { FF_TEXT, "", "", "" } };
    snprintf(f[0].value, sizeof f[0].value, "%s", token ? token : "");
    copy_to_clipboard(owner, token ? token : "");
    snprintf(label, sizeof label, "%s \u2014 copied to your clipboard", what);
    f[0].label = label;
    f[0].hint  = note;
    form_dialog(owner, title, f, 1);
}

/* text_prompt() is gone (WIN-21): every flow that used it now has a form
 * describing its actual shape. */

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
/* Switch to `ws`. Already connected -> instant, and the one we leave keeps
 * running (WIN-29). Otherwise connect it as an additional client; only when
 * there is no credential to connect with do we fall back to sign-in. */
static void switch_workspace(HWND hwnd, const char *ws, const char *cred) {
    if (!ws || !ws[0]) return;
    int existing = ws_find(ws);
    if (existing >= 0 && g_wss[existing].client) {
        ws_save_active();
        close_overlays();
        ws_load(existing);
        g_view = VIEW_HOME;
        layout_signin(hwnd);
        layout_composer(hwnd);
        InvalidateRect(hwnd, NULL, FALSE);
        return;
    }
    ws_save_active();          /* keep the current one running */
    close_overlays();
    g_client = NULL;           /* not a stop: the old client lives on in its slot */
    g_sel = 0; g_scroll = 0; g_post_auth = 0; g_has_sel = 0;
    g_n_backfilled = 0; g_edit_msg = 0; g_n_toast = 0; g_err_seen[0] = '\0';
    g_view = VIEW_HOME;
    connect_start(ws, cred ? cred : "");
    layout_signin(hwnd);
    layout_composer(hwnd);
    InvalidateRect(hwnd, NULL, FALSE);
}

static void sw_book_cb(void *ctx, const char *workspace, const char *label,
                       const char *username, uint64_t last) {
    (void)ctx; (void)last;
    if (g_n_sw >= (int)(sizeof g_sw / sizeof g_sw[0])) return;
    snprintf(g_sw[g_n_sw].ws, sizeof g_sw[g_n_sw].ws, "%s", workspace ? workspace : "");
    snprintf(g_sw[g_n_sw].label, sizeof g_sw[g_n_sw].label, "%s",
             (label && label[0]) ? label : (workspace ? workspace : "?"));
    /* Kept so a signed-out workspace can go straight to its password. */
    snprintf(g_sw[g_n_sw].user, sizeof g_sw[g_n_sw].user, "%s", username ? username : "");
    g_sw[g_n_sw].current = (g_client != NULL && strcmp(g_sw[g_n_sw].ws, g_cur_ws) == 0);
    g_n_sw++;
}

/* Refresh g_sw[] from the credential-store book. */
static void sw_book_load(void) {
    g_n_sw = 0;
    const char *sp = store_path();
    oc_store *st = sp ? oc_store_open(sp) : NULL;
    if (!st) return;
    oc_store_set_secret(st, g_secret);
    oc_store_workspace_each(st, sw_book_cb, NULL);
    oc_store_close(st);
}

static void open_ws_menu(HWND hwnd) {
    (void)hwnd;
    const oc_model *m = model();
    int admin = m && self_role(m) >= OC_ROLE_ADMIN;
    g_n_mi = 0;
    if (admin) { mi_item(40, "Invite people as member"); mi_item(41, "Invite people as admin"); mi_sep(); }
    mi_item(70, "Preferences");
    mi_item(71, "Notifications\u2026");
    mi_item(73, "Mark all as read");
    mi_item(72, "Keyboard shortcuts");
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

/* Sidebar prefs persist through the daemon's per-(user, client_type) settings
 * bucket — the client itself stores nothing (ARCH-88). First caller of
 * oc_client_set_setting; the `gui` bucket is separate from the TUI's by design,
 * so a terminal and a window can keep different sidebar shapes. */
static void prefs_save(void) {
    if (!g_client) return;
    char enc[352];
    snprintf(enc, sizeof enc, "t:%d;h:%d;m:%d;d:%d;n:%d;w:%d,%d,%d,%d,%d;q:%s",
             oc_theme_mode(), g_pref_time24, g_pref_members, g_pref_daysep,
             g_pref_notify, g_win_x, g_win_y, g_win_w, g_win_h, g_win_max,
             g_quick_names);
    oc_client_set_setting(g_client, PREFS_SETTING_KEY, enc);
}

static void prefs_load(const oc_model *m) {
    const char *v = m ? oc_model_setting(m, PREFS_SETTING_KEY) : NULL;
    if (!v || !v[0]) return;
    int t = oc_theme_mode(), h = g_pref_time24, mm = g_pref_members, d = g_pref_daysep;
    /* Tolerant of missing keys and of any order, so a value written by a newer
     * build with extra fields still loads what this one understands. */
    for (const char *p = v; *p; ) {
        char k = *p;
        if (p[1] == ':') {
            int val = atoi(p + 2);
            if (k == 't') t = val; else if (k == 'h') h = val;
            else if (k == 'm') mm = val; else if (k == 'd') d = val;
            else if (k == 'n') g_pref_notify = (val < 0 || val > 2) ? NOTIFY_FULL : val;
            else if (k == 'w') {
                int a, b2, c2, d2, e2;
                if (sscanf(p + 2, "%d,%d,%d,%d,%d", &a, &b2, &c2, &d2, &e2) == 5 && c2 > 200 && d2 > 150) {
                    g_win_x = a; g_win_y = b2; g_win_w = c2; g_win_h = d2; g_win_max = e2;
                }
            }
            else if (k == 'q') {
                size_t n2 = 0;
                for (const char *q = p + 2; *q && *q != ';' && n2 + 1 < sizeof g_quick_names; q++)
                    g_quick_names[n2++] = *q;
                g_quick_names[n2] = '\0';
                quick_rebuild();
            }
        }
        while (*p && *p != ';') p++;
        while (*p == ';') p++;
    }
    g_pref_time24  = h ? 1 : 0;
    g_pref_members = mm ? 1 : 0;
    g_pref_daysep  = d ? 1 : 0;
    g_show_members = g_pref_members;
    theme_set(t);
}

static void sidebar_opts_save(void) {
    if (!g_client) return;
    char enc[64];
    oc_sidebar_opts_encode(&g_sb, enc, sizeof enc);
    oc_client_set_setting(g_client, SB_SETTING_KEY, enc);
}
static void sidebar_opts_load(const oc_model *m) {
    const char *v = m ? oc_model_setting(m, SB_SETTING_KEY) : NULL;
    if (v && v[0]) oc_sidebar_opts_parse(&g_sb, v);
}

/* A section's Filter/Sort menu (Slack's "Section settings"). Flat rather than
 * nested submenus: three sorts and three filters fit one panel, and a checkmark
 * shows the active choice — the very thing a submenu would hide a level down.
 * Commands encode as 200 + section*16 + slot so one dispatcher serves both. */
#define SEC_CMD(sec, slot) (200 + (sec) * 16 + (slot))
static void open_section_menu(HWND hwnd, int sec) {
    (void)hwnd;
    if (sec < 0 || sec >= OC_SB_SECTIONS) return;
    g_sb_menu_sec = sec;
    g_n_mi = 0;
    mi_section(sec == OC_SB_CHANNELS ? "CHANNELS" : "DIRECT MESSAGES");
    mi_item(SEC_CMD(sec, 0), g_sb.collapsed[sec] ? "Expand" : "Collapse");
    mi_sep();
    mi_section("SORT");
    static const char *SORTS[3] = { "A\xE2\x80\x93Z", "Recent activity", "Unread first" };
    for (int i = 0; i < 3; i++) {
        char lbl[72];
        snprintf(lbl, sizeof lbl, "%s%s", g_sb.sort[sec] == i ? "\xE2\x9C\x93 " : "    ", SORTS[i]);
        mi_item(SEC_CMD(sec, 1 + i), lbl);
    }
    mi_sep();
    mi_section("FILTER");
    /* "Active only" means different things per section, so name what it does. */
    const char *FILTERS[3] = { "All", "Unread only",
                               sec == OC_SB_DMS ? "Online only" : "Joined only" };
    for (int i = 0; i < 3; i++) {
        char lbl[72];
        snprintf(lbl, sizeof lbl, "%s%s", g_sb.filter[sec] == i ? "\xE2\x9C\x93 " : "    ", FILTERS[i]);
        mi_item(SEC_CMD(sec, 4 + i), lbl);
    }
    g_menu = MENU_SECTION; g_menu_headerblock = 0; g_menu_hover = -1;
    g_menu_x = RAIL_W + 24; g_menu_y = HEADER_H + 60; g_menu_w = 236;
}

static void open_switcher(HWND hwnd) {
    (void)hwnd;
    sw_book_load();
    g_n_mi = 0;
    mi_section("WORKSPACES");
    for (int i = 0; i < g_n_sw; i++) {
        /* Say which are actually connected and what is waiting in them — the
         * point of holding N clients is that you can see it without switching. */
        int slot = ws_find(g_sw[i].ws);
        int unread = 0;
        if (slot >= 0 && g_wss[slot].client) {
            const oc_model *wm = oc_client_model(g_wss[slot].client);
            if (wm) for (size_t c = 0; c < wm->n_channels; c++) unread += wm->channels[c].unread;
        }
        char lbl[140];
        if (g_sw[i].current)
            snprintf(lbl, sizeof lbl, "%s  \xE2\x9C\x93", g_sw[i].label);
        else if (unread > 0)
            snprintf(lbl, sizeof lbl, "%s  \xE2\x80\xA2 %d", g_sw[i].label, unread);
        else if (slot >= 0 && g_wss[slot].client)
            snprintf(lbl, sizeof lbl, "%s  (connected)", g_sw[i].label);
        else
            /* Remembered but not signed in — the state a sign-out now leaves
             * behind, and a click away from being live again. */
            snprintf(lbl, sizeof lbl, "%s  \u2014 signed out", g_sw[i].label);
        mi_item(100 + i, lbl);
    }
    if (g_n_sw == 0) mi_item(-1, "(no remembered workspaces)");
    mi_sep();
    mi_item(80, "Add a workspace\xE2\x80\xA6");
    mi_item(81, "Manage workspaces\xE2\x80\xA6");
    g_menu = MENU_SWITCHER; g_menu_headerblock = 0; g_menu_hover = -1;
    g_menu_x = RAIL_W + 8; g_menu_y = 12; g_menu_w = 244;
}

static void menu_dispatch(HWND hwnd, int cmd) {
    const oc_model *m = model();
    switch (cmd) {
    case 1: {   /* WIN-30: the wire has always carried is_public; now so does the UI. */
        oc_field f[2] = {
            { FF_TEXT,   "Channel name", "Lower-case, no spaces. Names are unique.", "" },
            { FF_CHOICE, "Visibility",   "Public|Private",                            "0" },
        };
        if (form_dialog(hwnd, "Create a channel", f, 2) && f[0].value[0])
            oc_client_create_channel_ex(g_client, f[0].value, atoi(f[1].value) == 0);
        break; }
    case 2:  oc_client_reconnect(g_client); break;
    case 3:  oc_client_logout(g_client, OC_LOGOUT_THIS); g_logging_out = 1; break;
    case 5:  oc_client_logout(g_client, OC_LOGOUT_ALL);  g_logging_out = 1; break;
    case 4:  search_open(hwnd); break;
    case 6: {
        oc_field f[1] = { { FF_TEXT, "Username",
                            "Who to open a direct message with.", "" } };
        if (!m || !form_dialog(hwnd, "New direct message", f, 1) || !f[0].value[0]) break;
        uint64_t id = oc_model_user_id(m, f[0].value);
        /* An unknown name used to do nothing at all, which looked like a bug. */
        if (!id) { toast_push("No such user in this workspace.", 1); break; }
        g_view = VIEW_HOME;
        oc_client_open_dm(g_client, id);
        break; }
    case 7:  g_view = VIEW_HOME; upload_file(hwnd); break;
    case 10: oc_client_set_presence(g_client, OC_PRESENCE_ONLINE); break;
    case 11: oc_client_set_presence(g_client, OC_PRESENCE_AWAY); break;
    case 30: {
        const char *cur = m ? oc_model_user_name(m, m->user_id) : "";
        oc_field f[1] = { { FF_TEXT, "Display name",
                            "How you appear to everyone in this workspace.", "" } };
        snprintf(f[0].value, sizeof f[0].value, "%s", cur ? cur : "");
        if (form_dialog(hwnd, "Edit profile", f, 1) && f[0].value[0])
            oc_client_set_display_name(g_client, f[0].value);
        break; }
    case 31: {   /* WIN-20: a confirm field, which the chained prompts had none of. */
        oc_field f[3] = {
            { FF_PASSWORD, "Current password", "", "" },
            { FF_PASSWORD, "New password",     "", "" },
            { FF_PASSWORD, "Confirm new password", "", "" },
        };
        if (!form_dialog(hwnd, "Change password", f, 3)) break;
        if (!f[1].value[0])                        toast_push("Enter a new password.", 1);
        else if (strcmp(f[1].value, f[2].value))   toast_push("The new passwords do not match.", 1);
        else                                       oc_client_change_password(g_client, f[0].value, f[1].value);
        break; }
    case 40: oc_client_invite_user(g_client, OC_ROLE_MEMBER); g_await_invite = 1; break;
    case 41: oc_client_invite_user(g_client, OC_ROLE_ADMIN);  g_await_invite = 1; break;
    case 60: g_view = VIEW_HOME; close_overlays(); oc_client_toggle_storage(g_client, 1); oc_client_storage_status(g_client); break;
    case 61: g_view = VIEW_HOME; close_overlays(); oc_client_toggle_audit(g_client, 1); oc_client_audit_query(g_client, 0); break;
    case 50: {   /* WIN-13: separate fields and an explicit on/off, not one parsed string. */
        oc_field f[3] = {
            { FF_CHECK, "Do not disturb during these hours", "", "0" },
            { FF_TEXT,  "From (HH:MM)", "", "22:00" },
            { FF_TEXT,  "To (HH:MM)",   "A window may cross midnight.", "08:00" },
        };
        if (m && m->dnd_enabled) {
            snprintf(f[0].value, sizeof f[0].value, "1");
            snprintf(f[1].value, sizeof f[1].value, "%02u:%02u", m->dnd_start_min / 60, m->dnd_start_min % 60);
            snprintf(f[2].value, sizeof f[2].value, "%02u:%02u", m->dnd_end_min / 60, m->dnd_end_min % 60);
        }
        if (!form_dialog(hwnd, "Do not disturb", f, 3)) break;
        if (!atoi(f[0].value)) { oc_client_set_dnd(g_client, 0, 0, 0); toast_push("Do not disturb is off.", 0); break; }
        int sh = 0, sm = 0, eh = 0, em = 0;
        if (sscanf(f[1].value, "%d:%d", &sh, &sm) != 2 || sscanf(f[2].value, "%d:%d", &eh, &em) != 2 ||
            sh < 0 || sh > 23 || eh < 0 || eh > 23 || sm < 0 || sm > 59 || em < 0 || em > 59) {
            toast_push("Use HH:MM, 00:00 to 23:59.", 1);
            break;
        }
        oc_client_set_dnd(g_client, 1, (uint16_t)(sh * 60 + sm), (uint16_t)(eh * 60 + em));
        break; }
    case 70: close_overlays(); g_prefs_open = 1; g_view = VIEW_HOME; break;
    case 73: {   /* WIN-33: catch-up, as a loop over the existing CLIENT_ACK.
                  * REQ-238 may later add a true bulk op; this needs no wire
                  * change and the acks are cumulative per channel anyway. */
        if (!m) break;
        int n = 0;
        for (size_t i = 0; i < m->n_channels; i++) {
            const oc_channel *c = &m->channels[i];
            if (c->high_water > c->read_marker) { oc_client_mark_read(g_client, c->channel_id); n++; }
        }
        char msg[64];
        snprintf(msg, sizeof msg, n ? "Marked %d conversation%s read." : "Nothing unread.",
                 n, n == 1 ? "" : "s");
        toast_push(msg, 0);
        break; }
    case 71: close_overlays(); g_notify_open = 1; g_view = VIEW_HOME;
             oc_client_list_notify_prefs(g_client); break;   /* WIN-12 */
    case 72: close_overlays(); g_keys_open = 1; g_view = VIEW_HOME; break;   /* WIN-25 */
    /* "Add a workspace…" drops the current session and goes to the same sign-in
     * view the app starts on — one sign-in implementation, not two. */
    /* Adding a workspace must not sign you out of the one you are in — which is
     * exactly what reset_session() here used to do. Park the current one and
     * sign in alongside it. */
    case 80: signin_begin_add(hwnd); break;
    case 81: close_overlays(); sw_book_load(); g_wsmgr_open = 1; g_view = VIEW_HOME; break;
    default:
        /* Section Filter/Sort (see SEC_CMD). */
        if (cmd >= 200 && cmd < 200 + OC_SB_SECTIONS * 16) {
            int sec = (cmd - 200) / 16, slot = (cmd - 200) % 16;
            if (slot == 0)                   g_sb.collapsed[sec] = (uint8_t)!g_sb.collapsed[sec];
            else if (slot >= 1 && slot <= 3) g_sb.sort[sec]      = (uint8_t)(slot - 1);
            else if (slot >= 4 && slot <= 6) g_sb.filter[sec]    = (uint8_t)(slot - 4);
            g_sb_menu_sec = -1;
            sidebar_opts_save();
            break;
        }
        if (cmd >= 100 && cmd - 100 < g_n_sw && !g_sw[cmd - 100].current) {
            int i = cmd - 100;
            int slot = ws_find(g_sw[i].ws);
            if (slot >= 0 && g_wss[slot].client) switch_workspace(hwnd, g_sw[i].ws, "");
            else signin_begin_known(hwnd, g_sw[i].ws, g_sw[i].user);
        }
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
            /* WIN-31: both frames have always existed on the wire and reached no
             * client, so a private channel could be created but never populated. */
            AppendMenuW(menu, MF_STRING, 6, L"Add someone…");
            AppendMenuW(menu, MF_STRING, 7, L"Remove someone…");
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
        oc_field f[1] = { { FF_TEXT, "Webhook label",
                            "Shown as the sender for messages posted through it.", "" } };
        if (form_dialog(hwnd, "Create webhook", f, 1) && f[0].value[0]) {
            oc_client_create_webhook(g_client, cid, f[0].value);
            g_await_webhook = 1;
        }
        break;
    }
    case 6:
    case 7: {
        int add = (cmd == 6);
        oc_field f[1] = { { FF_TEXT, "Username",
                            add ? "Who to add to this channel."
                                : "Who to remove from this channel.", "" } };
        if (!form_dialog(hwnd, add ? "Add to channel" : "Remove from channel", f, 1) ||
            !f[0].value[0]) break;
        uint64_t uid = oc_model_user_id(m, f[0].value);
        if (!uid) { toast_push("No such user in this workspace.", 1); break; }
        if (add) oc_client_channel_invite(g_client, cid, uid);
        else     oc_client_channel_kick(g_client, cid, uid);
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
/* Renders the scene into a DC target for the harness. Thumbnails are suppressed
 * for the duration: their bitmaps belong to the window's target and drawing one
 * here would fail the whole frame. */
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
        /* Match the window, or the harness renders an unscaled UI into a
         * device-sized bitmap and silently reports the wrong thing at any DPI
         * other than 100%. */
        props.dpiX = props.dpiY = (float)g_dpi;
        props.usage = D2D1_RENDER_TARGET_USAGE_GDI_COMPATIBLE;
        ID2D1DCRenderTarget *dcrt = NULL;
        if (SUCCEEDED(ID2D1Factory_CreateDCRenderTarget(g_factory, &props, &dcrt)) && dcrt) {
            RECT bind = { 0, 0, w, h };
            ID2D1DCRenderTarget_BindDC(dcrt, mem, &bind);
            ID2D1RenderTarget *rt = (ID2D1RenderTarget *)dcrt;
            /* Brushes are RT-specific; swap the globals to ones on this RT. */
            ID2D1SolidColorBrush *sb = g_brush, *sb2 = g_brush2, *sb3 = g_brush3;
            D2D1_COLOR_F white = col(0xFFFFFF), faint = col(OC_COL_FAINT);
            D2D1_COLOR_F acc = col(OC_COL_ACCENT);
            g_brush = NULL; g_brush2 = NULL; g_brush3 = NULL;
            ID2D1RenderTarget_CreateSolidColorBrush(rt, &white, NULL, &g_brush);
            ID2D1RenderTarget_CreateSolidColorBrush(rt, &faint, NULL, &g_brush2);
            /* g_brush3 too, or the mention spans carry a brush from the window's
             * target into this one and the whole frame fails to draw. */
            ID2D1RenderTarget_CreateSolidColorBrush(rt, &acc, NULL, &g_brush3);
            ID2D1RenderTarget_BeginDraw(rt);
            g_thumbs_off = 1;
            render_scene(rt, model(), DIPF(w), DIPF(h));
            g_thumbs_off = 0;
            if (SUCCEEDED(ID2D1RenderTarget_EndDraw(rt, NULL, NULL))) ok = 1;
            if (g_brush) ID2D1SolidColorBrush_Release(g_brush);
            if (g_brush2) ID2D1SolidColorBrush_Release(g_brush2);
            if (g_brush3) ID2D1SolidColorBrush_Release(g_brush3);
            g_brush = sb; g_brush2 = sb2; g_brush3 = sb3;
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
    {
        const oc_channel *dc = g_sel ? oc_model_channel((oc_model *)m, g_sel) : NULL;
        if (dc) for (size_t i = 0; i < dc->n_msgs; i++)
            for (int k = 0; k < dc->msgs[i].n_attach; k++)
                fprintf(f, "  attach msg=%llu id=%llu name=\"%s\" mime=\"%s\" recl=%d\n",
                        (unsigned long long)dc->msgs[i].message_id,
                        (unsigned long long)dc->msgs[i].attach[k].id,
                        dc->msgs[i].attach[k].filename, dc->msgs[i].attach[k].mime,
                        dc->msgs[i].attach[k].reclaimed);
    }
    fprintf(f, "view=%d si_overlay=%d wsmgr=%d\n", g_view, g_si_overlay, g_wsmgr_open);
    fprintf(f, "workspaces=%d active=%d elsewhere=%d\n", g_n_wss, g_ws_active, ws_unread_elsewhere());
    for (int i = 0; i < g_n_wss; i++) {
        int u = 0;
        const oc_model *wm = g_wss[i].client ? oc_client_model(g_wss[i].client) : NULL;
        if (wm) for (size_t c = 0; c < wm->n_channels; c++) u += wm->channels[c].unread;
        fprintf(f, "  ws[%d] %s client=%p authed=%d unread=%d\n", i, g_wss[i].ws,
                (void *)g_wss[i].client, wm ? wm->authed : 0, u);
    }
    fprintf(f, "tray_live=%d notify_pref=%d toasts_raised=%d dnd=%d\n",
            g_tray_live, g_pref_notify, g_toasts_raised, dnd_active(m));
    fprintf(f, "lightbox=%llu thumb_hits=%d\n", (unsigned long long)g_lightbox, g_n_thumb_hits);
    fprintf(f, "thumb_hover=%llu thumb_tools=%d\n", (unsigned long long)g_thumb_hover, g_n_thumb_dl);
    for (int i = 0; i < g_n_thumb_dl; i++)
        fprintf(f, "  tool[%d] %s %.0f,%.0f %.0fx%.0f\n", i,
                g_thumb_dl[i].attach_ix < 0 ? "kebab" : "download",
                g_thumb_dl[i].r.left, g_thumb_dl[i].r.top,
                g_thumb_dl[i].r.right - g_thumb_dl[i].r.left,
                g_thumb_dl[i].r.bottom - g_thumb_dl[i].r.top);
    for (int i = 0; i < g_n_thumb_hits; i++)
        fprintf(f, "  thumbhit[%d] id=%llu %.0f,%.0f %.0fx%.0f\n", i,
                (unsigned long long)g_thumb_hits[i].id,
                g_thumb_hits[i].r.left, g_thumb_hits[i].r.top,
                g_thumb_hits[i].r.right - g_thumb_hits[i].r.left,
                g_thumb_hits[i].r.bottom - g_thumb_hits[i].r.top);
    fprintf(f, "thumb_pending=%llu n_thumbs=%d n_missing=%d off=%d\n",
            (unsigned long long)g_thumb_pending, g_n_thumbs, g_n_thumb_missing, g_thumbs_off);
    for (int i = 0; i < g_n_thumbs; i++)
        fprintf(f, "  thumb[%d] id=%llu bmp=%p\n", i,
                (unsigned long long)g_thumbs[i].id, (void *)g_thumbs[i].bmp);
    fprintf(f, "unread_from=%llu unread_chan=%llu unread_count=%d\n",
            (unsigned long long)g_unread_from, (unsigned long long)g_unread_chan, g_unread_count);
    for (size_t i = 0; i < m->n_channels; i++)
        fprintf(f, "  marks ch %llu hw=%llu rm=%llu unread=%d\n",
                (unsigned long long)m->channels[i].channel_id,
                (unsigned long long)m->channels[i].high_water,
                (unsigned long long)m->channels[i].read_marker, m->channels[i].unread);
    fprintf(f, "channels=%zu\n", m->n_channels);
    for (size_t i = 0; i < m->n_channels; i++) {
        const oc_channel *c = &m->channels[i];
        /* DMs have no name, so the old `continue` hid every one of them from the
         * dump — which is why a DM-list bug could not be diagnosed from it. */
        fprintf(f, "  ch %llu %s\"%s\" unread=%d msgs=%zu prev=\"%s\" prevby=%llu%s\n",
                (unsigned long long)c->channel_id,
                c->kind == OC_CHANNEL_KIND_DM ? "DM " : "",
                c->name ? c->name : "", c->unread,
                c->n_msgs, c->preview, (unsigned long long)c->preview_author,
                c->channel_id == g_sel ? " *" : "");
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
    } else if (!strcmp(verb, "type")) {
        /* Put text in the composer WITHOUT sending, caret at the end, so the
         * autocomplete popover sees the same state as live typing. */
        if (g_re) {
            WCHAR w[1024]; to_w(arg, w, 1024);
            SetWindowTextW(g_re, w);
            SendMessageW(g_re, EM_SETSEL, (WPARAM)-2, -1);
            ac_rebuild();
            test_ack("ok");
        } else test_ack("err");
    } else if (!strcmp(verb, "upload")) {
        /* Bypass the file dialog so an attachment can be posted from the harness. */
        if (g_client && g_sel && arg[0]) { oc_client_upload(g_client, g_sel, arg); test_ack("ok"); }
        else test_ack("err");
    } else if (!strcmp(verb, "palette")) {
        palette_open(hwnd);
        if (arg[0]) { WCHAR w[64]; to_w(arg, w, 64); SetWindowTextW(g_pal_edit, w); }
        test_ack("ok");
    } else if (!strcmp(verb, "wsadd")) {
        /* "<workspace> <user:pass>" — connect an ADDITIONAL workspace. */
        char ws[256] = "", cred[256] = "";
        sscanf(arg, "%255s %255s", ws, cred);
        if (ws[0]) { switch_workspace(hwnd, ws, cred); test_ack("ok"); } else test_ack("err");
    } else if (!strcmp(verb, "move")) {
        /* A real WM_MOUSEMOVE, so hover goes through the same path the mouse
         * does rather than a test-only shortcut that could drift from it. */
        int x = 0, y = 0; sscanf(arg, "%d %d", &x, &y);
        SendMessageW(hwnd, WM_MOUSEMOVE, 0, MAKELPARAM(PX(x), PX(y)));
        test_ack("ok");
    } else if (!strcmp(verb, "nav")) {
        int d = 0, u = 0; sscanf(arg, "%d %d", &d, &u);
        nav_conversation(hwnd, d, u); test_ack("ok");
    } else if (!strcmp(verb, "dpi")) {
        /* Force a scale factor so the layout can be checked without a scaled
         * display attached. Same path WM_DPICHANGED takes. */
        int d = atoi(arg);
        if (d >= 48 && d <= 480) {
            g_dpi = (UINT)d;
            if (g_brush)  { ID2D1SolidColorBrush_Release(g_brush);  g_brush = NULL; }
            if (g_brush2) { ID2D1SolidColorBrush_Release(g_brush2); g_brush2 = NULL; }
        if (g_brush3) { ID2D1SolidColorBrush_Release(g_brush3); g_brush3 = NULL; }
            thumbs_drop();
            if (g_rt) { ID2D1HwndRenderTarget_Release(g_rt); g_rt = NULL; }
            layout_composer(hwnd);
            layout_signin(hwnd);
            InvalidateRect(hwnd, NULL, TRUE);
            test_ack("ok");
        } else test_ack("err");
    } else if (!strcmp(verb, "wsforget")) {
        /* The removal itself, minus the confirmation dialog a harness cannot
         * dismiss reliably. Same code path the button takes after OK. */
        int slot = ws_find(arg);
        if (slot >= 0 && g_wss[slot].client && slot != g_ws_active) {
            oc_client_stop(g_wss[slot].client);
            for (int k = slot; k + 1 < g_n_wss; k++) g_wss[k] = g_wss[k + 1];
            g_n_wss--;
            if (g_ws_active > slot) g_ws_active--;
            g_n_notify_hw = 0;
        }
        ws_forget(arg);
        sw_book_load();
        test_ack("ok");
    } else if (!strcmp(verb, "wsgo")) {
        int i = atoi(arg);
        if (i >= 0 && i < g_n_wss) { ws_save_active(); close_overlays(); ws_load(i);
                                     layout_composer(hwnd); test_ack("ok"); }
        else test_ack("err");
    } else if (!strcmp(verb, "toast")) {
        notify_toast("OpenChime", arg[0] ? arg : "test notification"); test_ack("ok");
    } else if (!strcmp(verb, "notify")) {
        close_overlays(); g_notify_open = 1; g_view = VIEW_HOME;
        oc_client_list_notify_prefs(g_client); test_ack("ok");
    } else if (!strcmp(verb, "keys")) {
        close_overlays(); g_keys_open = 1; g_view = VIEW_HOME; test_ack("ok");
    } else if (!strcmp(verb, "menu")) {
        /* Drive a workspace/new-menu command directly. Modal forms block this
         * poll loop until dismissed, so the ack lands after the dialog closes. */
        menu_dispatch(hwnd, atoi(arg)); test_ack("ok");
    } else if (!strcmp(verb, "profile")) {
        close_overlays(); g_profile_uid = strtoull(arg, NULL, 10); g_view = VIEW_HOME; test_ack("ok");
    } else if (!strcmp(verb, "prefs")) {
        close_overlays(); g_prefs_open = 1; g_view = VIEW_HOME; test_ack("ok");
    } else if (!strcmp(verb, "theme")) {
        theme_set(atoi(arg)); prefs_save(); test_ack("ok");
    } else if (!strcmp(verb, "emoji")) {
        picker_open(hwnd, (uint64_t)strtoull(arg, NULL, 10));
        test_ack("ok");
    } else if (!strcmp(verb, "actab")) {
        ac_accept(); test_ack("ok");
    } else if (!strcmp(verb, "search")) {
        search_open(hwnd);
        if (arg[0]) { WCHAR w[256]; to_w(arg, w, 256); SetWindowTextW(g_srch, w); search_submit(); }
        test_ack("ok");
    } else if (!strcmp(verb, "pin")) {
        /* The kebab's Pin item goes through a modal TrackPopupMenu the harness
         * cannot navigate, so this drives the same client call directly. */
        uint64_t mid = strtoull(arg, NULL, 10);
        const oc_model *pm = model();
        const oc_channel *pc = pm && g_sel ? oc_model_channel((oc_model *)pm, g_sel) : NULL;
        if (!mid && pc && pc->n_msgs) mid = pc->msgs[pc->n_msgs - 1].message_id;
        const oc_msg *pmsg = find_msg(pc, mid);
        oc_client_pin(g_client, g_sel, mid, (pmsg && pmsg->pinned) ? OC_PIN_REMOVE : OC_PIN_ADD);
        test_ack("ok");
    } else if (!strcmp(verb, "mkchan")) {
        /* Bypass the modal New-channel dialog, as "upload" bypasses the file
         * dialog: the harness cannot drive a modal. */
        char nm[64] = {0}; int pub = 1;
        sscanf(arg, "%63s %d", nm, &pub);
        if (g_client && nm[0]) { oc_client_create_channel_ex(g_client, nm, pub == 0); test_ack("ok"); }
        else test_ack("err");
    } else if (!strcmp(verb, "del")) {
        /* Delete a message without the modal menu; mid 0 = the newest. */
        unsigned long long mid = strtoull(arg, NULL, 10);
        const oc_model *dm = model();
        const oc_channel *dc = dm && g_sel ? oc_model_channel((oc_model *)dm, g_sel) : NULL;
        if (!mid && dc && dc->n_msgs) mid = dc->msgs[dc->n_msgs - 1].message_id;
        if (g_client && mid) { oc_client_delete(g_client, g_sel, (uint64_t)mid); test_ack("ok"); }
        else test_ack("err");
    } else if (!strcmp(verb, "react")) {
        /* "<mid> <emoji>"; mid 0 = the newest message. Bypasses the modal menu. */
        unsigned long long mid = 0; char emo[40] = {0};
        sscanf(arg, "%llu %39s", &mid, emo);
        const oc_model *rm = model();
        const oc_channel *rc = rm && g_sel ? oc_model_channel((oc_model *)rm, g_sel) : NULL;
        if (!mid && rc && rc->n_msgs) mid = rc->msgs[rc->n_msgs - 1].message_id;
        const oc_msg *rmsg = find_msg(rc, (uint64_t)mid);
        if (g_client && mid && emo[0]) {
            oc_client_react(g_client, g_sel, (uint64_t)mid, emo,
                            (rmsg && reaction_is_mine(rmsg, emo)) ? OC_REACT_REMOVE : OC_REACT_ADD);
            test_ack("ok");
        } else test_ack("err");
    } else if (!strcmp(verb, "chup")) {
        /* Bypass the modal form, as "mkchan" and "upload" do. */
        int op = 0; char val[256] = {0};
        sscanf(arg, "%d %255[^\n]", &op, val);
        if (g_client && g_sel) { oc_client_update_channel(g_client, g_sel, (uint8_t)op, val); test_ack("ok"); }
        else test_ack("err");
    } else if (!strcmp(verb, "tab")) {
        select_tab(atoi(arg));
        test_ack("ok");
    } else if (!strcmp(verb, "pins")) {
        const oc_model *pm = model();
        if (pm && pm->pinlist_open) oc_client_close_pins(g_client);
        else if (g_sel)             oc_client_list_pins(g_client, g_sel);
        test_ack("ok");
    } else if (!strcmp(verb, "dump")) {
        test_dump(arg); test_ack("ok");
    /* Sign-in drivers. Setting the EDIT text directly is deterministic, where
     * synthesised keystrokes depend on the window being foreground — which it
     * is not when the harness drives it from WSL. */
    } else if (!strcmp(verb, "siws") || !strcmp(verb, "siuser") || !strcmp(verb, "sipass")) {
        HWND e = !strcmp(verb, "siws")  ? g_si_e_ws
               : !strcmp(verb, "siuser") ? g_si_e_user : g_si_e_pass;
        if (e) { WCHAR w[320]; to_w(arg, w, 320); SetWindowTextW(e, w); test_ack("ok"); }
        else test_ack("err");
    } else if (!strcmp(verb, "siadv")) {
        signin_set_advanced(hwnd, !g_si_advanced); test_ack("ok");
    } else if (!strcmp(verb, "sisubmit")) {
        signin_submit(hwnd); test_ack("ok");
    } else if (!strcmp(verb, "siremember")) {
        g_si_remember = atoi(arg); test_ack("ok");
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
        search_create(hwnd);
        g_pal_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)0xF4, GetModuleHandleW(NULL), NULL);
        if (g_pal_edit) {
            SendMessageW(g_pal_edit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            SendMessageW(g_pal_edit, EM_SETCUEBANNER, TRUE,
                         (LPARAM)L"Run an action or jump to a conversation\u2026");
            g_pal_prev = (WNDPROC)SetWindowLongPtrW(g_pal_edit, GWLP_WNDPROC, (LONG_PTR)pal_proc);
        }
        g_pick_edit = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | ES_AUTOHSCROLL,
            0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)0xF3, GetModuleHandleW(NULL), NULL);
        if (g_pick_edit) {
            SendMessageW(g_pick_edit, WM_SETFONT, (WPARAM)GetStockObject(DEFAULT_GUI_FONT), TRUE);
            SendMessageW(g_pick_edit, EM_SETCUEBANNER, TRUE, (LPARAM)L"Search emoji\u2026");
        }
        signin_create(hwnd);
        DragAcceptFiles(hwnd, TRUE);              /* drop files anywhere to upload */
        g_dpi = dpi_for_window(hwnd);
        SetTimer(hwnd, TIMER_TICK, 30, NULL);
        tray_init(hwnd);   /* WIN-18: the notification surface */
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
        if (wp == TIMER_TICK) {
            /* Every workspace is ticked, not just the one on screen — that is
             * the whole point of WIN-29. A background workspace drains its
             * events, accrues unread, and can raise a notification. */
            for (int wi = 0; wi < g_n_wss; wi++)
                if (g_wss[wi].client && g_wss[wi].client != g_client)
                    oc_client_tick(g_wss[wi].client);
            /* And the sign-in attempt, which is not in a slot yet. */
            if (g_si_client) oc_client_tick(g_si_client);
        }
        if (wp == TIMER_TICK && g_client) {
            oc_client_tick(g_client);
            const oc_model *m = oc_client_model(g_client);
            /* A sign-in failure belongs in the card, not a toast — suppress the
             * toast channel while the sign-in view owns the window. */
            if (g_view == VIEW_SIGNIN) { if (g_si_connecting) signin_poll(hwnd); }
            else toast_tick(m);
            /* A DM we asked for has arrived — select it. Picking someone should
             * land you IN the conversation, not back at the list with a new row
             * somewhere in the sidebar. */
            if (g_dm_pending) {
                const oc_channel *nd = dm_with(m, g_dm_pending);
                if (nd) { g_dm_pending = 0; g_dm_compose = 0; select_channel(nd->channel_id);
                          InvalidateRect(hwnd, NULL, FALSE); }
            }
            /* Persist a settled move. WM_EXITSIZEMOVE covers a drag, but not a
             * programmatic move, and nothing covers being killed — a debounce
             * means the placement survives without waiting for a clean exit. */
            if (g_geom_dirty_at && g_geom_applied &&
                GetTickCount64() - g_geom_dirty_at > 1200) {
                g_geom_dirty_at = 0;
                prefs_save();
            }
            if (!g_geom_applied && g_geom_deadline && GetTickCount64() > g_geom_deadline) {
                g_geom_applied = 1;                 /* settings never came */
                if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOW);
            }
            /* OS notifications (WIN-18). Raised for messages that arrive while
             * the window is not in front, subject to the channel's level and the
             * DND window. The first pass after connecting only records the
             * marks: the backfill is not "new mail". */
            if (g_pref_notify != NOTIFY_OFF) {
                int fg = (GetForegroundWindow() == hwnd);
                int quiet = dnd_active(m);
                /* Every workspace, not just the visible one — a background
                 * workspace's mail is exactly what you cannot otherwise see. */
                for (int wi = 0; wi < (g_n_wss > 0 ? g_n_wss : 1); wi++) {
                  const oc_model *wm = (g_n_wss > 0)
                      ? (g_wss[wi].client ? oc_client_model(g_wss[wi].client) : NULL) : m;
                  if (!wm || !wm->authed) continue;
                  int is_active = (g_n_wss == 0) || (wi == g_ws_active);
                  for (size_t ci = 0; ci < wm->n_channels; ci++) {
                    const oc_channel *c = &wm->channels[ci];
                    int slot = -1;
                    for (int k = 0; k < g_n_notify_hw; k++)
                        if (g_notify_hw[k].slot == wi && g_notify_hw[k].cid == c->channel_id) { slot = k; break; }
                    if (slot < 0) {
                        if (g_n_notify_hw >= 128) continue;
                        slot = g_n_notify_hw++;
                        g_notify_hw[slot].slot = wi;
                        g_notify_hw[slot].cid = c->channel_id;
                        g_notify_hw[slot].seen = c->high_water;
                        continue;                      /* first sighting: record only */
                    }
                    uint64_t prev = g_notify_hw[slot].seen;
                    g_notify_hw[slot].seen = c->high_water;
                    if (!g_notify_primed || c->high_water <= prev) continue;
                    if (fg && is_active && c->channel_id == g_sel) continue;   /* you are reading it */
                    if (quiet) continue;
                    /* MENTIONS now has an answer (REQ-221): the same scanner
                     * the daemon resolves with, so the toast a client raises and
                     * the push the server sends agree by construction. */
                    if (c->notify_level == OC_NOTIFY_NONE) continue;
                    if (c->notify_level == OC_NOTIFY_MENTIONS) {
                        const oc_msg *lm = c->n_msgs ? &c->msgs[c->n_msgs - 1] : NULL;
                        const char *me = oc_model_user_name(wm, wm->user_id);
                        if (!lm || !lm->body ||
                            !oc_mention_targets(lm->body, strlen(lm->body), me))
                            continue;
                    }

                    const oc_msg *last = c->n_msgs ? &c->msgs[c->n_msgs - 1] : NULL;
                    if (last && last->author_id == wm->user_id) continue;   /* your own */
                    char label[96], title[160], body[256];
                    channel_label(wm, c, label, sizeof label);
                    /* Name the workspace when it is not the one on screen, or
                     * "#general" alone is ambiguous across several of them. */
                    if (is_active) snprintf(title, sizeof title, "%s", label);
                    else snprintf(title, sizeof title, "%s \u2014 %s",
                                  oc_model_workspace_name(wm), label);
                    if (g_pref_notify == NOTIFY_FULL && last && last->body) {
                        const char *who = last->author_name[0] ? last->author_name
                                        : oc_model_user_name(wm, last->author_id);
                        snprintf(body, sizeof body, "%s: %s", (who && who[0]) ? who : "someone",
                                 last->body);
                    } else {
                        snprintf(body, sizeof body, "%d new message%s",
                                 c->unread, c->unread == 1 ? "" : "s");
                    }
                    notify_toast(title, body);
                  }
                }
                g_notify_primed = 1;
            }
            /* An inline image arrived: decode and cache it (WIN-17). */
            {
                uint64_t fid = 0; size_t flen = 0;
                uint8_t *fd = oc_model_take_attachment((oc_model *)m, &fid, &flen);
                if (fd) {
                    thumb_decode(fid, fd, flen);
                    free(fd);
                    if (g_thumb_pending == fid) g_thumb_pending = 0;
                    InvalidateRect(hwnd, NULL, FALSE);
                }
            }
            /* A fetch that never came back — a reclaimed or oversized image.
             * Mark it so the transcript stops asking on every frame. */
            if (g_thumb_pending && GetTickCount64() > g_thumb_deadline) {
                if (g_n_thumb_missing < THUMB_CACHE) g_thumb_missing[g_n_thumb_missing++] = g_thumb_pending;
                g_thumb_pending = 0;
            }
            /* Resolve an in-flight history page. Older messages land ABOVE the
             * view, and g_scroll is measured from the bottom, so the reading
             * position stays put on its own — nothing to compensate for.
             *
             * A page that never arrives means we are at the top of the channel:
             * mark it so, or every scroll to the top asks again forever. */
            if (g_hist_pending_chan) {
                const oc_channel *hc = oc_model_channel((oc_model *)m, g_hist_pending_chan);
                if (hc && hc->n_msgs && hc->msgs[0].message_id < g_hist_before) {
                    g_hist_pending_chan = 0;
                } else if (GetTickCount64() > g_hist_deadline) {
                    if (g_n_hist_exhausted < 32) g_hist_exhausted[g_n_hist_exhausted++] = g_hist_pending_chan;
                    g_hist_pending_chan = 0;
                }
            }
            /* An armed jump the transcript never resolved (WIN-3): the message is
             * older than the backfill window, so say so rather than leaving the
             * click looking like it did nothing. */
            if (g_jump_mid && GetTickCount64() > g_jump_deadline) {
                g_jump_mid = 0;
                toast_push("That message is older than the loaded history.", 0);
            }
            /* The settings bucket arrives a beat after auth; fold it in once. */
            if (g_sb_settings_pending && oc_model_setting(m, SB_SETTING_KEY)) {
                sidebar_opts_load(m);
                g_sb_settings_pending = 0;
                InvalidateRect(hwnd, NULL, FALSE);
            }
            /* The oldest entry paged in is the cursor for loading older ones. */
            if (m->n_audit) {
                uint64_t oldest = m->audit[0].at_ms;
                for (size_t i = 1; i < m->n_audit; i++)
                    if (m->audit[i].at_ms && m->audit[i].at_ms < oldest) oldest = m->audit[i].at_ms;
                g_audit_oldest = oldest;
            }
            if (g_prefs_pending && oc_model_setting(m, PREFS_SETTING_KEY)) {
                prefs_load(m);
                g_prefs_pending = 0;
                /* Place the window where it was left. The settings arrive after
                 * auth, so the window is created hidden and shown here — moving
                 * a visible window would flash it at the default size first. */
                if (!g_geom_applied) {
                    g_geom_applied = 1;
                    if (g_win_x != -1) {
                        WINDOWPLACEMENT wp2; wp2.length = sizeof wp2;
                        wp2.flags = 0;
                        wp2.showCmd = g_win_max ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
                        wp2.ptMinPosition.x = wp2.ptMinPosition.y = 0;
                        wp2.ptMaxPosition.x = wp2.ptMaxPosition.y = 0;
                        wp2.rcNormalPosition.left = g_win_x;
                        wp2.rcNormalPosition.top = g_win_y;
                        wp2.rcNormalPosition.right = g_win_x + g_win_w;
                        wp2.rcNormalPosition.bottom = g_win_y + g_win_h;
                        SetWindowPlacement(hwnd, &wp2);
                    }
                    if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOW);
                }
                layout_composer(hwnd);
                InvalidateRect(hwnd, NULL, FALSE);
            }
            if (m->authed && !g_post_auth) {          /* one-shot: identify the bucket + pull state */
                oc_client_set_client_type(g_client, "gui");   /* our own settings bucket, not tui's */
                oc_client_list_settings(g_client);
                oc_client_list_users(g_client);
                oc_client_list_channels(g_client);
                g_post_auth = 1;
                g_sb_settings_pending = 1;   /* fold the synced sidebar prefs when they land */
                g_prefs_pending = 1;
                layout_composer(hwnd);   /* members pane now shows — re-fit the composer */
            }
            /* Sign-out is scoped to ONE workspace, not the app. It used to drop
             * the whole window into the sign-in view — while the other
             * workspaces stayed connected and receiving, merely unreachable,
             * because the rail is not drawn there. Now we leave the workspace
             * and land in a surviving one; the sign-in view appears only when
             * nothing is left to show. */
            if (g_logging_out && !m->connected) {     /* logout frame sent + server dropped us */
                g_logging_out = 0;
                char ws[256]; snprintf(ws, sizeof ws, "%s", g_cur_ws);
                char label[96]; ws_display_name(m, label, sizeof label);
                /* The server has revoked this token; drop it but KEEP the book
                 * entry, so the workspace stays in the switcher a password away
                 * rather than vanishing from the device. */
                /* Remove was chosen: delete the whole entry rather than just the
                 * token, once the server has actually revoked the session. */
                if (g_forget_after_logout) { g_forget_after_logout = 0; ws_forget(ws); }
                else                        ws_clear_session(ws);
                reset_session();                      /* stops the client, retires the slot */
                int nxt = ws_first_live(-1);
                if (nxt >= 0) {
                    ws_load(nxt);
                    g_view = VIEW_HOME;
                    layout_composer(hwnd);
                    char msg[160];
                    snprintf(msg, sizeof msg, "Signed out of %s.", label[0] ? label : ws);
                    toast_push(msg, 0);
                } else {
                    signin_begin(hwnd, ws, NULL);
                }
                InvalidateRect(hwnd, NULL, FALSE);
                return 0;
            }
            if (g_await_invite && m->invite_token[0]) {   /* show the minted token once */
                g_await_invite = 0;
                show_secret(hwnd, "Invite created", "Invite token", m->invite_token,
                            "Share it once. It is not shown again \u2014 mint a new invite if you lose it.");
            }
            if (g_await_webhook && m->webhook_token[0]) {  /* show the minted webhook token once */
                g_await_webhook = 0;
                show_secret(hwnd, "Webhook created", "Webhook token", m->webhook_token,
                            "POST to /webhook/<token>. Shown once; delete and recreate if you lose it.");
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
    case WM_DPICHANGED: {
        /* Dragged to a differently-scaled monitor, or the display setting
         * changed. Windows hands us the rect the window should occupy there;
         * honouring it is what stops the window jumping size. The render target
         * is rebuilt because its DPI is fixed at creation. */
        g_dpi = HIWORD(wp);
        RECT *sug = (RECT *)lp;
        if (sug)
            SetWindowPos(hwnd, NULL, sug->left, sug->top,
                         sug->right - sug->left, sug->bottom - sug->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
        if (g_brush)  { ID2D1SolidColorBrush_Release(g_brush);  g_brush = NULL; }
        if (g_brush2) { ID2D1SolidColorBrush_Release(g_brush2); g_brush2 = NULL; }
        if (g_brush3) { ID2D1SolidColorBrush_Release(g_brush3); g_brush3 = NULL; }
        thumbs_drop();
        if (g_rt) { ID2D1HwndRenderTarget_Release(g_rt); g_rt = NULL; }
        layout_composer(hwnd);
        layout_signin(hwnd);
        InvalidateRect(hwnd, NULL, TRUE);
        return 0;
    }
    case WM_MOVE:
        if (geom_capture(hwnd)) g_geom_dirty_at = GetTickCount64();
        return 0;
    case WM_EXITSIZEMOVE:
        /* Persist when a drag ENDS, not on every WM_SIZE: the bucket is a
         * network round trip and a resize emits dozens of those. */
        if (geom_capture(hwnd) && g_geom_applied) prefs_save();
        return 0;
    case WM_SIZE:
        if (geom_capture(hwnd)) g_geom_dirty_at = GetTickCount64();
        d2d_resize(hwnd);
        layout_composer(hwnd);
        layout_signin(hwnd);      /* the card is centred, so it moves with the window */
        return 0;
    case WM_COMMAND:
        if (g_re && (HWND)lp == g_re && HIWORD(wp) == EN_CHANGE) {
            DWORD now = GetTickCount();
            if (g_client && g_sel && now - g_last_typing > 2000) {
                oc_client_typing(g_client, g_sel);
                g_last_typing = now;
            }
            ac_rebuild();                       /* WIN-7: candidates track the caret */
            if (composer_remeasure()) layout_composer(hwnd);
            InvalidateRect(hwnd, NULL, FALSE);
        }
        if (g_pal_edit && (HWND)lp == g_pal_edit && HIWORD(wp) == EN_CHANGE) {
            g_pal_sel = 0;
            InvalidateRect(hwnd, NULL, FALSE);
        }
        if (g_pick_edit && (HWND)lp == g_pick_edit && HIWORD(wp) == EN_CHANGE) {
            g_pick_scroll = 0;
            InvalidateRect(hwnd, NULL, FALSE);
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
        /* The find box and the sign-in fields both sit on the OC_COL_INPUT
         * surface the D2D chrome paints under them, so they share a brush. */
        if ((HWND)lp == g_find || (HWND)lp == g_srch || (HWND)lp == g_pick_edit ||
            (HWND)lp == g_pal_edit ||
            (HWND)lp == g_si_e_ws ||
            (HWND)lp == g_si_e_user || (HWND)lp == g_si_e_pass) {
            SetBkColor((HDC)wp, OCRGB(OC_COL_INPUT));
            SetTextColor((HDC)wp, OCRGB(OC_COL_TEXT));
            if (!g_find_brush) g_find_brush = CreateSolidBrush(OCRGB(OC_COL_INPUT));
            return (LRESULT)g_find_brush;
        }
        return DefWindowProcW(hwnd, msg, wp, lp);
    case WM_MOUSEWHEEL: {
        /* WM_MOUSEWHEEL carries SCREEN coordinates; map them to decide whether
         * the sidebar or the transcript scrolls. */
        POINT wpt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
        ScreenToClient(hwnd, &wpt);
        wpt.x = (LONG)DIPF(wpt.x); wpt.y = (LONG)DIPF(wpt.y);
        float dy = (float)GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA * 48.0f;
        const oc_model *wm = model();
        if (view_has_sidebar() && wpt.x >= (int)RAIL_W && wpt.x < (int)(RAIL_W + SIDEBAR_W)) {
            float maxs = g_sb_content > g_sb_view ? g_sb_content - g_sb_view : 0;
            g_sb_scroll -= dy;
            if (g_sb_scroll < 0) g_sb_scroll = 0;
            if (g_sb_scroll > maxs) g_sb_scroll = maxs;
        } else if (wm && wm->thread_open) {
            g_thr_scroll += dy;
            if (g_thr_scroll < 0) g_thr_scroll = 0;
            if (g_thr_scroll > g_thr_scroll_max) g_thr_scroll = g_thr_scroll_max;
        } else if (wm && (wm->audit_open || wm->weblist_open || wm->reactlist_open)
                   && !wm->search_open) {
            g_ovl_scroll -= dy;
            if (g_ovl_scroll < 0) g_ovl_scroll = 0;
            if (g_ovl_scroll > g_ovl_max) g_ovl_scroll = g_ovl_max;
            /* At the bottom of the audit log, page older entries in (WIN-19). */
            if (wm->audit_open && g_ovl_scroll >= g_ovl_max - 0.5f && g_audit_oldest > 1)
                oc_client_audit_query(g_client, g_audit_oldest - 1);
        } else if (g_notify_open || g_keys_open) {
            g_ovl_scroll -= dy;
            if (g_ovl_scroll < 0) g_ovl_scroll = 0;
            if (g_ovl_scroll > g_ovl_max) g_ovl_scroll = g_ovl_max;
        } else if (wm && wm->search_open) {
            /* The results list scrolls top-down, unlike the bottom-pinned
             * transcript, so the sign is the other way round. */
            g_srch_scroll -= dy;
            if (g_srch_scroll < 0) g_srch_scroll = 0;
            if (g_srch_scroll > g_srch_max) g_srch_scroll = g_srch_max;
        } else {
            g_scroll += dy;
            if (g_scroll < 0) g_scroll = 0;
            if (g_scroll > g_scroll_max) g_scroll = g_scroll_max;
        }
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    }
    case WM_LBUTTONDOWN: {
        int mx = (int)DIPF(GET_X_LPARAM(lp)), my = (int)DIPF(GET_Y_LPARAM(lp));
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
        int mx = (int)DIPF(GET_X_LPARAM(lp)), my = (int)DIPF(GET_Y_LPARAM(lp));
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
        } else if (view_has_sidebar() && (float)mx >= RAIL_W &&
                   (float)mx < RAIL_W + SIDEBAR_W) {
            /* Reveal a header's kebab while the cursor is on its row, as Slack
             * does: there when you look for it, out of the way when you don't. */
            int sec = -1;
            for (int i = 0; i < g_n_rows; i++)
                if (g_rows[i].header && (float)my >= g_rows[i].top && (float)my < g_rows[i].bot) {
                    sec = g_rows[i].sec; break;
                }
            if (sec != g_sb_hover_sec) { g_sb_hover_sec = sec; InvalidateRect(hwnd, NULL, FALSE); }
        } else {
            uint64_t th = 0;
            for (int i = 0; i < g_n_thumb_hits; i++)
                if (in_rect(g_thumb_hits[i].r, mx, my)) { th = g_thumb_hits[i].id; break; }
            /* Keep the toolbar up while the cursor is on it, not just on the
             * image — it sits inside the image bounds, so this is only about
             * not flickering at the edges. */
            if (!th) for (int i = 0; i < g_n_thumb_dl; i++)
                if (in_rect(g_thumb_dl[i].r, mx, my)) { th = g_thumb_hover; break; }
            if (th != g_thumb_hover) { g_thumb_hover = th; InvalidateRect(hwnd, NULL, FALSE); }
        }
        if ((float)mx < RAIL_W) {
            /* Rail hover. */
            int a = -100;
            for (int i = 0; i < g_n_navrows; i++)
                if ((float)my >= g_navrows[i].top && (float)my < g_navrows[i].bot) { a = g_navrows[i].act; break; }
            if (a != g_nav_hover) { g_nav_hover = a; InvalidateRect(hwnd, NULL, FALSE); }
        } else {
            if (g_nav_hover != -100) { g_nav_hover = -100; InvalidateRect(hwnd, NULL, FALSE); }
            if (g_sb_hover_sec != -1)  { g_sb_hover_sec = -1;  InvalidateRect(hwnd, NULL, FALSE); }
            int r = (any_overlay(model()) || !view_has_sidebar()) ? -1 : msgrow_at(my);
            uint64_t h = r >= 0 ? g_msgrows[r].mid : 0;
            if (h != g_hover_mid) { g_hover_mid = h; InvalidateRect(hwnd, NULL, FALSE); }
            /* DMs-index row hover. */
            uint64_t dh = 0;
            for (int i = 0; i < g_n_dmrows; i++)
                if (in_rect(g_dmrows[i].r, (float)mx, (float)my)) { dh = g_dmrows[i].cid; break; }
            for (int i = 0; i < g_n_pickrows && !dh; i++)
                if (in_rect(g_pickrows[i].r, (float)mx, (float)my)) { dh = g_pickrows[i].uid; break; }
            if (dh != g_dm_hover) { g_dm_hover = dh; InvalidateRect(hwnd, NULL, FALSE); }
            /* Tab strip hover. */
            int th2 = -1;
            for (int i = 0; i < TAB_COUNT; i++)
                if (in_rect(g_tab_r[i], (float)mx, (float)my)) { th2 = i; break; }
            if (th2 != g_tab_hover) { g_tab_hover = th2; InvalidateRect(hwnd, NULL, FALSE); }
            /* Files-list row hover. */
            uint64_t fh = 0;
            {
                const oc_model *fm = model();
                for (int i = 0; i < g_n_filerows && fm; i++)
                    if (in_rect(g_filerows[i].row, (float)mx, (float)my) &&
                        (size_t)g_filerows[i].ix < fm->n_files) {
                        fh = fm->files[g_filerows[i].ix].id; break;
                    }
            }
            if (fh != g_hover_filerow) { g_hover_filerow = fh; InvalidateRect(hwnd, NULL, FALSE); }
            /* Pins-overlay row hover, so a row reads as the clickable thing it is. */
            uint64_t ph = 0;
            for (int i = 0; i < g_n_pinrows; i++)
                if (in_rect(g_pinrows[i].row, (float)mx, (float)my)) { ph = g_pinrows[i].mid; break; }
            if (ph != g_hover_pinrow) { g_hover_pinrow = ph; InvalidateRect(hwnd, NULL, FALSE); }
        }
        return 0;
    }
    case WM_LBUTTONUP:
        if (g_sbar_drag) { g_sbar_drag = 0; ReleaseCapture(); InvalidateRect(hwnd, NULL, FALSE); }
        else if (g_selecting) { selection_end(); InvalidateRect(hwnd, NULL, FALSE); }
        return 0;
    case WM_KEYDOWN:
        if (wp == 'C' && (GetKeyState(VK_CONTROL) & 0x8000)) { copy_selection(hwnd); return 0; }
        if (wp == VK_ESCAPE && g_lightbox) { g_lightbox = 0; InvalidateRect(hwnd, NULL, FALSE); return 0; }
        /* Esc pops the context pane back to the member list before it reaches
         * the middle column's overlays — the pane is what you just opened. */
        if (wp == VK_ESCAPE && g_rp_mode != RP_MEMBERS) {
            rp_pop(); InvalidateRect(hwnd, NULL, FALSE); return 0;
        }
        if (wp == VK_ESCAPE && g_menu) { g_menu = MENU_NONE; g_menu_hover = -1; InvalidateRect(hwnd, NULL, FALSE); return 0; }
        if (wp == VK_ESCAPE && modal_open()) { close_overlays(); InvalidateRect(hwnd, NULL, FALSE); return 0; }
        if (wp == VK_ESCAPE && g_more_open) { g_more_open = 0; InvalidateRect(hwnd, NULL, FALSE); return 0; }
        if (wp == VK_ESCAPE && g_has_sel) { g_has_sel = 0; InvalidateRect(hwnd, NULL, FALSE); return 0; }
        if (wp == VK_OEM_2 && (GetKeyState(VK_CONTROL) & 0x8000)) {   /* Ctrl+/ */
            int on = !g_keys_open;
            close_overlays();
            g_keys_open = on; g_view = VIEW_HOME;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (wp == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) { search_open(hwnd); return 0; }
        if (wp == 'K' && (GetKeyState(VK_CONTROL) & 0x8000)) { palette_open(hwnd); return 0; }
        if ((GetKeyState(VK_MENU) & 0x8000) && (wp == VK_UP || wp == VK_DOWN)) {
            nav_conversation(hwnd, wp == VK_DOWN ? 1 : -1,
                             (GetKeyState(VK_SHIFT) & 0x8000) != 0);
            return 0;
        }
        if (wp == VK_F6) { SetFocus(g_re ? g_re : hwnd); return 0; }
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
        int minClientH = PX(64 + 2 * RAIL_IH + 3 * RAIL_IH + 12);  /* start+Home+More+cluster */
        mmi->ptMinTrackSize.y = minClientH + frameH;
        mmi->ptMinTrackSize.x = PX(640) + frameW;
        return 0;
    }
    case WM_RBUTTONDOWN:
        on_rclick(hwnd, (int)DIPF(GET_X_LPARAM(lp)), (int)DIPF(GET_Y_LPARAM(lp)));
        InvalidateRect(hwnd, NULL, FALSE);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_CLOSE:
        /* WIN-59: the outbox is in memory now (ARCH-88), so quitting with a send
         * still queued loses it. Make that a choice rather than a surprise. */
        if (geom_capture(hwnd) && g_geom_applied) prefs_save();
        {
            /* Across EVERY workspace (WIN-29), not just the visible one: a
             * message stranded in a background workspace is the easiest of all
             * to lose, because you cannot see it. */
            int pending = g_client ? oc_client_outbox_pending(g_client) : 0;
            for (int i = 0; i < g_n_wss; i++)
                if (g_wss[i].client && g_wss[i].client != g_client)
                    pending += oc_client_outbox_pending(g_wss[i].client);
            if (pending > 0) {
                WCHAR w[320]; char line[320];
                snprintf(line, sizeof line,
                         "%d message%s composed while offline %s not been sent yet.\n\n"
                         "Quitting now will discard %s.",
                         pending, pending == 1 ? "" : "s", pending == 1 ? "has" : "have",
                         pending == 1 ? "it" : "them");
                to_w(line, w, 320);
                if (MessageBoxW(hwnd, w, L"Unsent messages",
                                MB_OKCANCEL | MB_ICONWARNING | MB_DEFBUTTON2) != IDOK)
                    return 0;
            }
        }
        return DefWindowProcW(hwnd, msg, wp, lp);   /* proceed with the close */
    case WM_DESTROY:
        KillTimer(hwnd, TIMER_TICK);
        tray_done();       /* or the icon lingers in the notification area */
        for (int i = 0; i < g_n_wss; i++)
            if (g_wss[i].client && g_wss[i].client != g_client) oc_client_stop(g_wss[i].client);
        g_n_wss = 0;
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

    /* Open the OS credential store first: the "do we have a session?" probe
     * below reads through it. */
    g_secret = oc_secret_open_os("openchime");
    oc_sidebar_opts_defaults(&g_sb);

    /* Credential resolution, uniform with the TUI:
     *   1. Dev quick-launch `openchime.exe <workspace> <user:pass>` — connect directly.
     *   2. A remembered workspace with a still-valid session token — reconnect
     *      silently; the net thread reuses the stored token.
     *   3. Otherwise the in-window sign-in view, pre-filled from the book.
     * Only case 3 needs the window up first, but the window is now created
     * before any of them so the sign-in view has somewhere to live (WIN-2). */
    static char aws[256], acred[264];
    static char pre_ws[256], pre_user[128];
    int direct = 0;
    int argc = 0; LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argv && argc >= 3) {
        WideCharToMultiByte(CP_UTF8, 0, argv[1], -1, aws, sizeof aws, NULL, NULL);
        WideCharToMultiByte(CP_UTF8, 0, argv[2], -1, acred, sizeof acred, NULL, NULL);
        direct = 1;
    } else {
        int have_last = pick_last_workspace(pre_ws, sizeof pre_ws, pre_user, sizeof pre_user);
        if (have_last && have_stored_token(pre_ws)) {
            snprintf(aws, sizeof aws, "%s", pre_ws);   /* silent reconnect: cred stays "" */
            direct = 1;
        }
    }
    if (argv) LocalFree(argv);

    /* Before the window exists: awareness is a process-wide, one-shot decision. */
    dpi_declare_awareness();
    LoadLibraryW(L"Msftedit.dll");        /* registers MSFTEDIT_CLASS (RICHEDIT50W) */
    /* Before anything paints: the palette is runtime state now, and every
     * OC_COL_* reads through it. */
    oc_theme_apply(OC_THEME_DARK);
    d2d_init();                           /* factory only; the RT is made per-hwnd in paint */
    if (direct) {
        connect_start(aws, acred);
        /* WIN-57: bring up EVERY other remembered workspace that has a stored
         * token, not just the one you used last. This was blocked purely on
         * WIN-29 — with one client there was nowhere to put them, so unread
         * elsewhere was invisible until you went looking. The most-recently-used
         * one stays active; the rest connect behind it. */
        if (!acred[0]) boot_other_workspaces(aws);
    } else {
        /* The most-recently-used workspace has no usable token — but another one
         * may still have. Signing out of the last workspace you used must not
         * strand the ones you are still signed in to. */
        boot_other_workspaces(NULL);
        int first = ws_first_live(-1);
        if (first >= 0) { ws_load(first); g_view = VIEW_HOME; }
        else            { g_view = VIEW_SIGNIN; }
    }

    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof wc);
    wc.cbSize        = sizeof wc;
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursorW(NULL, IDC_ARROW);
    /* Taskbar, Alt-Tab and the window corner all read these; without them the
     * app showed the generic Windows default everywhere. */
    wc.hIcon   = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.hIconSm = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APPICON));
    wc.lpszClassName = L"OpenChimeWin";
    if (!RegisterClassExW(&wc)) return 1;

    HWND hwnd = CreateWindowExW(0, L"OpenChimeWin", L"OpenChime",
                    WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
                    1120, 820, NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    apply_dark_titlebar(hwnd);
    /* When we are auto-connecting, hold the window back until the settings
     * bucket arrives so it can open where it was left rather than snapping
     * there a second later. Shown regardless after a short grace period, and
     * immediately when there is nothing to wait for (the sign-in view). */
    if (direct) g_geom_deadline = GetTickCount64() + 1500;
    else { g_geom_applied = 1; ShowWindow(hwnd, show); }
    UpdateWindow(hwnd);
    /* After ShowWindow: SetFocus on a child of a not-yet-shown window does not
     * stick, which left the workspace field unfocused and swallowed typing. */
    if (!direct) signin_begin(hwnd, pre_ws[0] ? pre_ws : NULL, pre_user[0] ? pre_user : NULL);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        /* Give the sign-in view a dialog manager so Tab moves between its fields
         * and Enter submits. Gated to that view so the RichEdit composer keeps
         * its own Tab/Enter handling (re_proc) everywhere else. */
        if (g_view == VIEW_SIGNIN && IsDialogMessageW(hwnd, &m)) continue;
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
    if (g_small_w) IDWriteTextFormat_Release(g_small_w);
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
    oc_secret_free(g_secret);
    return 0;
}
