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
#include <wchar.h>
#include <wctype.h>

#include "client.h"
#include "secret_os.h"
#include "model.h"
#include "complete.h"   /* shared @user / #channel / :emoji: completion */
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
       VIEW_ADMIN, VIEW_NOTIFICATIONS, VIEW_COUNT,
       /* Not a rail destination: the sign-in screen, which owns the whole window
        * (no rail, no sidebar, no composer) until there is a session. */
       VIEW_SIGNIN = 100 };
enum { NAV_SWITCHER = -2, NAV_NEW = -3, NAV_PROFILE = -4, NAV_MORE = -5 };
#define BODY_DIP    15.0f     /* message + composer text size (shared, so they match) */

/* Per-user avatar tints, shared by the transcript and the sidebar's DM rows so
 * one person is the same colour everywhere. */
static const uint32_t AVPAL[6] =
    { 0x2563EB, 0x3BA55D, 0xD9A441, 0xB05CCB, 0xE0725A, 0x2FA5A5 };

/* Typed modal form fields (WIN-21); form_dialog() is defined further down. */
enum { FF_TEXT = 0, FF_PASSWORD, FF_CHECK, FF_CHOICE };

typedef struct {
    int         kind;
    const char *label;
    const char *hint;              /* FF_CHOICE: "a|b|c"; else an optional sub-label */
    char        value[192];        /* in: initial; out: the result (FF_CHECK/CHOICE: "0".."n") */
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

static IDWriteTextFormat *g_hdr;    /* channel title */
static IDWriteTextFormat *g_name;   /* message author (semibold) */
static IDWriteTextFormat *g_time;   /* timestamp (trailing-aligned) */
static IDWriteTextFormat *g_body;   /* message body (wrapping) */
static IDWriteTextFormat *g_ui;     /* sidebar rows / composer */
static IDWriteTextFormat *g_ui_b;   /* unread sidebar rows (semibold) */
static IDWriteTextFormat *g_small;  /* subtitles / meta lines */
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

/* Members-pane row hit-boxes. */
static struct { float top, bot; uint64_t uid; } g_memrows[256];
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

/* Notification-prefs review (WIN-12) + the shortcut sheet (WIN-25). */
static int g_notify_open, g_keys_open;
static struct { D2D1_RECT_F r; uint64_t cid; uint8_t level; } g_notify_hits[128];
static int g_n_notify_hits;

static int      g_show_members = 1;     /* members pane visible */
static D2D1_RECT_F g_members_btn;       /* header toggle hit-box */
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
static struct { char ws[256], label[80]; int current; } g_sw[16];
static int   g_n_sw;

static D2D1_RECT_F g_attach_btn;        /* composer attach (+) hit-box */
static D2D1_RECT_F g_send_btn;          /* composer send-button hit-box */
static D2D1_RECT_F g_emoji_btn;         /* composer emoji-picker hit-box (WIN-8) */
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
    ID2D1RenderTarget_DrawTextLayout(rt, org, tl, paint_with(rgb), D2D1_DRAW_TEXT_OPTIONS_CLIP);
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
    g_prefs_open = 0;
    g_profile_uid = 0;
    g_notify_open = 0;
    g_keys_open = 0;
    if (!mm) return;
    if (mm->thread_open)    oc_client_close_thread(g_client);
    if (mm->search_open)    oc_client_close_search(g_client);
    if (mm->reactlist_open) oc_client_close_reactions(g_client);
    if (mm->weblist_open)   oc_client_close_webhooks(g_client);
    if (mm->storage_open)   oc_client_toggle_storage(g_client, 0);
    if (mm->audit_open)     oc_client_toggle_audit(g_client, 0);
}

static int any_overlay(const oc_model *m) {
    return g_prefs_open || g_profile_uid || g_notify_open || g_keys_open ||
           (m && (m->thread_open || m->search_open || m->reactlist_open ||
                  m->weblist_open || m->storage_open || m->audit_open));
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
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_TRAILING);
    draw_text(rt, "\xE2\x96\xBE", g_small, rf(RAIL_W + 16, 2, g_hdr_gear.left - 8, HEADER_H), OC_COL_MUTED);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);

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
    oc_sidebar_row rows[512];
    size_t nrows = oc_model_sidebar(m, &o, rows, 512);

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
                uint8_t pr = oc_model_presence_of(m, r->peer_id);
                uint32_t dot = pr == OC_PRESENCE_ONLINE ? OC_COL_ONLINE
                             : pr == OC_PRESENCE_AWAY   ? OC_COL_AWAY : OC_COL_FAINT;
                D2D1_ELLIPSE pe = { { av.right - 1, av.bottom - 1 }, 3.5f, 3.5f };
                ID2D1RenderTarget_FillEllipse(rt, &pe, paint_with(dot));
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

static int reaction_is_mine(const oc_msg *msg, const char *emoji);   /* fwd */

static void draw_message(ID2D1RenderTarget *rt, const oc_model *m, const oc_msg *msg,
                         IDWriteTextLayout *body, float x0, float y, float content_w,
                         int grouped) {
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
        ID2D1RenderTarget_DrawTextLayout(rt, org, body, paint_with(bcol),
                                         D2D1_DRAW_TEXT_OPTIONS_NONE);
        DWRITE_TEXT_METRICS tm;
        if (SUCCEEDED(IDWriteTextLayout_GetMetrics(body, &tm))) by += tm.height;
        else by += 18;
    }
    /* "(edited)" is drawn inline by body_layout (faint, after the last word). */

    /* Meta lines: reactions, attachments, thread. */
    if (msg->n_reactions) {
        /* Chips rather than one grey text run: the emoji needs the colour-font
         * path anyway, and a bordered count reads as the clickable thing it is. */
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
            cx += cw + 5;
        }
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
    enum { CAP = 600 };
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
    if (capture) g_n_msgrows = 0; else if (hits) g_n_thrrows = 0;
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
            float bx = x0 + AVA + 12, by = y + MSG_BODY_DY(grouped[i]);
            /* Hover highlight behind the whole row (main transcript only). */
            if (capture && !g_selecting && g_hover_mid == msgs[first + i].message_id)
                fill(rt, rf(reg.left, y, reg.right, y + heights[i]), OC_COL_HOVER);
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

static void draw_storage(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    D2D1_RECT_F body = overlay_header(rt, reg, "Storage usage");

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

static void draw_audit(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    D2D1_RECT_F body = overlay_header(rt, reg, "Audit log");
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

/* WIN-25: the shortcut sheet, generated from one table so it cannot drift from
 * what the key handlers actually do. */
static const struct { const char *keys, *what; } KEYMAP[] = {
    { "Enter",            "Send the message" },
    { "Shift+Enter",      "New line" },
    { "Esc",              "Close the open pane, popover or picker" },
    { "Tab",              "Insert the highlighted completion" },
    { "Up / Down",        "Move through completions" },
    { "Ctrl+K",           "Command palette" },
    { "Ctrl+F",           "Search messages" },
    { "Ctrl+/",           "This list" },
    { "Mouse wheel",      "Scroll the transcript, sidebar or open pane" },
    { "Right-click",      "Actions for a message, member or channel" },
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

/* One preference row: a label, a sub-label, and a segmented set of choices on
 * the right. Returns the y for the next row. */
static float pref_row(ID2D1RenderTarget *rt, D2D1_RECT_F body, float y, int row,
                      const char *label, const char *hint,
                      const char *const *opts, int n_opts, int cur) {
    draw_text(rt, label, g_ui_b, rf(body.left + 24, y, body.left + 320, y + 22), OC_COL_TEXT);
    if (hint && hint[0])
        draw_text(rt, hint, g_small, rf(body.left + 24, y + 20, body.left + 340, y + 40), OC_COL_FAINT);

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

enum { PREF_ROW_THEME = 0, PREF_ROW_TIME, PREF_ROW_MEMBERS, PREF_ROW_DAYSEP, PREF_ROW_QUICK };

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

static void draw_profile(ID2D1RenderTarget *rt, const oc_model *m, D2D1_RECT_F reg) {
    D2D1_RECT_F body = overlay_header(rt, reg, "Profile");
    const char *nm = oc_model_user_name(m, g_profile_uid);
    if (!nm || !nm[0]) nm = "user";

    float cx = body.left + 40, cy = body.top + 32;
    D2D1_ELLIPSE av = { { cx + 36, cy + 36 }, 36, 36 };
    ID2D1RenderTarget_FillEllipse(rt, &av, paint_with(AVPAL[g_profile_uid % 6]));
    char ini[2] = { (char)(nm[0] >= 'a' && nm[0] <= 'z' ? nm[0] - 32 : nm[0]), 0 };
    IDWriteTextFormat_SetTextAlignment(g_hdr, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(rt, ini, g_hdr, rf(cx, cy, cx + 72, cy + 72), 0xFFFFFF);
    IDWriteTextFormat_SetTextAlignment(g_hdr, DWRITE_TEXT_ALIGNMENT_LEADING);

    float tx = cx + 92;
    draw_text(rt, nm, g_hdr, rf(tx, cy + 4, body.right - 24, cy + 30), OC_COL_TEXT);

    uint8_t pres = oc_model_presence_of(m, g_profile_uid);
    const char *pl = pres == OC_PRESENCE_ONLINE ? "Active"
                   : pres == OC_PRESENCE_AWAY   ? "Away" : "Offline";
    uint32_t pc = pres == OC_PRESENCE_ONLINE ? OC_COL_ONLINE
                : pres == OC_PRESENCE_AWAY   ? OC_COL_AWAY : OC_COL_FAINT;
    D2D1_ELLIPSE dot = { { tx + 5, cy + 42 }, 5, 5 };
    ID2D1RenderTarget_FillEllipse(rt, &dot, paint_with(pc));
    draw_text(rt, pl, g_small, rf(tx + 16, cy + 32, body.right - 24, cy + 52), OC_COL_MUTED);

    uint8_t role = OC_ROLE_MEMBER;
    int known = 0;
    for (size_t i = 0; i < m->n_users; i++)
        if (m->users[i].user_id == g_profile_uid) { role = m->users[i].role; known = 1; break; }
    const char *rl = role_label(role);
    if (known && rl[0])
        draw_text(rt, rl, g_small, rf(tx, cy + 52, body.right - 24, cy + 72), OC_COL_FAINT);

    float by = cy + 96;
    if (g_profile_uid != m->user_id) {
        g_prof_dm_btn = rf(tx, by, tx + 140, by + 32);
        fill_round(rt, g_prof_dm_btn, 7.0f, OC_COL_ACCENT);
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_CENTER);
        draw_text(rt, "Message", g_ui, g_prof_dm_btn, 0xFFFFFF);
        IDWriteTextFormat_SetTextAlignment(g_ui, DWRITE_TEXT_ALIGNMENT_LEADING);
    } else {
        g_prof_dm_btn = rf(0, 0, 0, 0);
        draw_text(rt, "This is you \u2014 edit your display name from the profile menu.",
                  g_small, rf(tx, by + 6, body.right - 24, by + 26), OC_COL_FAINT);
    }
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
    if (g_prefs_open)      { draw_prefs(rt, reg);        return; }
    if (g_profile_uid)     { draw_profile(rt, m, reg);   return; }
    if (g_notify_open)     { draw_notify_prefs(rt, m, reg); return; }
    if (g_keys_open)       { draw_keys(rt, reg);         return; }
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

    /* Jump-to-unread (WIN-14): only while this channel actually has a divider to
     * jump to, so it is never a dead control. */
    float statr = g_members_btn.left - 12;
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

    const char *status = !m->connected ? "offline" : !m->authed ? "signing in" : "connected";
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_TRAILING);
    draw_text(rt, status, g_small, rf(x0 + 20, 0, statr, HEADER_H),
              m->authed ? OC_COL_ONLINE : OC_COL_MUTED);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
}

/* The connection banner (REQ-263): a strip under the header whenever we are not
 * authenticated, naming the state and offering an immediate retry. `last_error`
 * carries the specific reason when the net thread has one ("could not reach the
 * server", the reconnect countdown, a changed certificate); without one we fall
 * back to the phase. Returns its height so the caller can push content down. */
static float draw_banner(ID2D1RenderTarget *rt, const oc_model *m, float x0, float w) {
    g_banner_on = 0;
    if (!m || m->authed) return 0;

    const char *why = m->last_error[0] ? m->last_error
                    : !m->connected    ? "Connecting…"
                                       : "Signing in…";
    /* Amber while a connection is plausibly coming back, red once the core has
     * told us something concrete went wrong — the distinction the user acts on. */
    uint32_t accent = m->last_error[0] ? OC_COL_DANGER : OC_COL_AWAY;

    D2D1_RECT_F r = rf(x0, HEADER_H, x0 + w, HEADER_H + BANNER_H);
    fill(rt, r, OC_COL_SIDEBAR);
    fill(rt, rf(x0, HEADER_H, x0 + 3, r.bottom), accent);          /* status edge */
    fill(rt, rf(x0, r.bottom - 1, x0 + w, r.bottom), OC_COL_BORDER);

    g_retry_btn = rf(x0 + w - 104, HEADER_H + 5, x0 + w - 14, r.bottom - 5);
    fill_round(rt, g_retry_btn, 6.0f, OC_COL_INPUT);
    stroke_round(rt, g_retry_btn, 6.0f, OC_COL_BORDER, 1.0f);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_CENTER);
    draw_text(rt, "Retry now", g_small, g_retry_btn, OC_COL_TEXT);
    IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);

    draw_text(rt, why, g_small, rf(x0 + 16, HEADER_H, g_retry_btn.left - 12, r.bottom), accent);
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
    float y = H - COMPOSER_H - TOAST_GAP;
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
                 + (g_si_step == 2 ? 26.0f : 0.0f);   /* back link */
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
        IDWriteTextFormat_SetTextAlignment(g_small, DWRITE_TEXT_ALIGNMENT_LEADING);
    } else {
        g_si_back = rf(0, 0, 0, 0);
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
    float px = x0 + 20, py = h - COMPOSER_H - ph - 6;
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
    float ph = 300; if (ph > h - HEADER_H - COMPOSER_H - 20) ph = h - HEADER_H - COMPOSER_H - 20;
    float px = x0 + 20, py = h - COMPOSER_H - ph - 6;
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

    /* Emoji picker, immediately right of attach. */
    g_emoji_btn = rf(bx0 + 6 + sq, cy, bx0 + 6 + sq * 2, cy + sq);
    draw_emoji_glyph(rt, "\xF0\x9F\x99\x82", g_emoji_btn);
    IDWriteTextFormat_SetTextAlignment(g_hdr, DWRITE_TEXT_ALIGNMENT_CENTER);

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
    /* Sign-in owns the whole window: no rail, no sidebar, no composer, and it
     * runs with no client at all (m == NULL) until an attempt is in flight. */
    if (g_view == VIEW_SIGNIN) { draw_signin(rt, W, H); return; }
    if (!m) return;
    ensure_selection(m);
    draw_rail(rt, m, H);

    if (view_has_sidebar()) {
        float main_x = RAIL_W + SIDEBAR_W;
        float members = (g_show_members && m->authed) ? MEMBERS_W : 0;
        float main_r = W - members, main_w = main_r - main_x;
        draw_sidebar(rt, m, H);
        draw_header(rt, m, main_x, main_w);
        float bh = draw_banner(rt, m, main_x, main_w);   /* pushes the transcript down */
        draw_transcript(rt, m, rf(main_x, HEADER_H + bh, main_r, H - COMPOSER_H));
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
            case VIEW_FILES:         draw_stub_view(rt, reg, "Files", "File browser \xE2\x80\x94 coming soon"); break;
            case VIEW_LATER:         draw_stub_view(rt, reg, "Later", "Saved items \xE2\x80\x94 coming soon"); break;
            case VIEW_NOTIFICATIONS: draw_stub_view(rt, reg, "Notifications", "Notifications \xE2\x80\x94 coming soon"); break;
            case VIEW_ADMIN:         draw_stub_view(rt, reg, "Admin", "Storage & audit \xE2\x80\x94 open from the workspace menu"); break;
            default:                 draw_stub_view(rt, reg, "OpenChime", ""); break;
        }
        g_n_memrows = 0;
    }
    draw_more_flyout(rt);   /* floats over the pane when open */
    draw_palette(rt, m, W, H);   /* the palette dims and covers the app */
    draw_menu(rt);          /* dropdown menus float on top of everything */
    draw_toasts(rt, W, H);  /* …and failure notices float above even those */
}

static void layout_search(HWND hwnd);

static void paint(HWND hwnd) {
    d2d_ensure_rt(hwnd);
    if (!g_rt || !g_brush) return;
    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)g_rt;
    RECT rc; GetClientRect(hwnd, &rc);
    float W = (float)(rc.right - rc.left), H = (float)(rc.bottom - rc.top);
    const oc_model *m = model();

    ID2D1RenderTarget_BeginDraw(rt);
    render_scene(rt, m, W, H);
    /* Boxes placed against chrome the scene just measured, so they can only be
     * positioned after the scene is laid out. */
    layout_search(hwnd);
    if (g_pal_edit) {
        if (g_pal_open) {
            ShowWindow(g_pal_edit, SW_SHOW);
            MoveWindow(g_pal_edit, (int)(g_pal_box.left + 32), (int)(g_pal_box.top + 8),
                       (int)(g_pal_box.right - g_pal_box.left - 44), 20, TRUE);
        } else {
            ShowWindow(g_pal_edit, SW_HIDE);
        }
    }
    if (g_pick_edit) {
        if (g_pick_open) {
            ShowWindow(g_pick_edit, SW_SHOW);
            MoveWindow(g_pick_edit, (int)(g_pick_box.left + 30), (int)(g_pick_box.top + 6),
                       (int)(g_pick_box.right - g_pick_box.left - 40), 18, TRUE);
        } else {
            ShowWindow(g_pick_edit, SW_HIDE);
        }
    }

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

/* Subclass proc: Enter sends, Shift+Enter inserts a newline. While the
 * autocomplete popover is open it takes Up/Down/Tab/Enter/Esc first — Enter
 * accepting a candidate rather than sending is what makes the popover feel like
 * part of the composer instead of a thing floating over it. */
static LRESULT CALLBACK re_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
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
    if ((msg == WM_KEYDOWN || msg == WM_CHAR) && wp == VK_RETURN
        && !(GetKeyState(VK_SHIFT) & 0x8000)) {
        if (msg == WM_KEYDOWN) composer_send();
        return 0;                                   /* eat both so no newline/bell */
    }
    if ((msg == WM_KEYDOWN || msg == WM_CHAR) && wp == VK_ESCAPE) {
        if (g_pick_open) { if (msg == WM_KEYDOWN) picker_close(GetParent(hwnd)); return 0; }
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
    /* Clear BOTH left buttons (attach + emoji), or the native child window
     * covers the one nearest the text and it renders as a clipped sliver. */
    int x = (int)(bx0 + 84), r = (int)(bx1 - 48);
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

/* Place the search-overlay query EDIT over the chrome draw_search() painted.
 * g_srch_box is only valid after a paint, so this runs from the paint path as
 * well as from WM_SIZE. */
static void layout_search(HWND hwnd) {
    const oc_model *m = model();
    if (!g_srch) return;
    if (!m || !m->search_open) { ShowWindow(g_srch, SW_HIDE); return; }
    (void)hwnd;
    ShowWindow(g_srch, SW_SHOW);
    MoveWindow(g_srch, (int)(g_srch_box.left + 30), (int)(g_srch_box.top + 8),
               (int)(g_srch_box.right - g_srch_box.left - 40), 20, TRUE);
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
    si_geom g = si_layout((float)rc.right, (float)rc.bottom);
    float y = g.fields_y;

    /* Inset inside the drawn 32px-tall rounded box. */
    int ex = (int)(g.fx + 12), ew = (int)(g.fw - 24), eh = 20;
    if (g_si_step == 1) {
        /* Leave room for the ".openchime.io" chip the painter draws at the right
         * edge of the box, so typed text can never run under it. */
        int sw = g_si_advanced ? 0 : (int)(8 + 7.0 * (double)(strlen(oc_default_suffix()) + 1));
        MoveWindow(g_si_e_ws, ex, (int)(y + 20 + 6), ew - sw, eh, TRUE);
    } else {
        MoveWindow(g_si_e_user, ex, (int)(y + 20 + 6), ew, eh, TRUE);
        MoveWindow(g_si_e_pass, ex, (int)(y + 62 + 20 + 6), ew, eh, TRUE);
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
        g_scroll = 0; oc_client_list_reactions(g_client, chan, mid);
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
    case 2:  close_overlays(); g_profile_uid = uid; g_view = VIEW_HOME; break;
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

static int on_click(HWND hwnd, int x, int y) {
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
        if (pt_in(g_si_back, x, y))          { signin_back(hwnd);   return 1; }
        if (pt_in(g_si_adv_link, x, y))      { signin_set_advanced(hwnd, !g_si_advanced); return 1; }
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
    /* Section kebab -> that section's Filter/Sort menu. Slack's placement, and
     * the right one: these are PER-SECTION settings, so a single header gear
     * would have to ask which section you meant. */
    if (g_sb_hover_sec >= 0 && in_rect(g_sb_kebab, x, y)) {
        open_section_menu(hwnd, g_sb_hover_sec);
        return 1;
    }
    /* Members toggle. */
    if (in_rect(g_members_btn, x, y)) {
        g_show_members = !g_show_members;
        layout_composer(hwnd);
        return 1;
    }
    /* Composer attach (+) and send buttons. */
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
    /* Members-pane rows: click opens the person's profile (WIN-10), which is
     * where "Message" now lives. Jumping straight into a DM made viewing someone
     * impossible, and it is the more destructive of the two actions. */
    for (int i = 0; i < g_n_memrows; i++)
        if ((float)y >= g_memrows[i].top && (float)y < g_memrows[i].bot) {
            close_overlays();
            g_profile_uid = g_memrows[i].uid;
            g_view = VIEW_HOME;
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
                /* Right-clicking a header opens the same menu as its kebab. */
                if (g_rows[i].header) open_section_menu(hwnd, g_rows[i].sec);
                else show_channel_menu(hwnd, m, g_rows[i].cid, pt.x, pt.y);
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

    /* Remember this workspace (+ the username, parsed off "user:pass") so the next
     * launch reconnects silently via the stored session token. */
    char user[128] = ""; const char *colon = strchr(cred, ':');
    if (colon && colon > cred) {
        size_t n = (size_t)(colon - cred); if (n >= sizeof user) n = sizeof user - 1;
        memcpy(user, cred, n); user[n] = 0;
    }
    remember_workspace(ws, user[0] ? user : NULL);
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
    if (g_client) { oc_client_stop(g_client); g_client = NULL; }
    g_sel = 0; g_scroll = 0; g_post_auth = 0; g_has_sel = 0;
    g_n_backfilled = 0; g_more_open = 0; g_menu = MENU_NONE;
    g_edit_msg = 0; g_n_toast = 0; g_err_seen[0] = '\0'; g_err_seq = 0;
}

/* Enter the sign-in view at step 1, pre-filled with `ws`/`user` when known. */
static void signin_begin(HWND hwnd, const char *ws, const char *user) {
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

    if (g_client) { oc_client_stop(g_client); g_client = NULL; }
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
    g_client = oc_client_start_secure(g_host, g_port, g_cred,
                                      g_si_remember ? store_path() : NULL,
                                      g_si_remember ? g_secret : NULL);
    if (!g_client) { snprintf(g_si_err, sizeof g_si_err, "could not start the client"); goto redraw; }
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
    const oc_model *m = model();
    if (!m) return;
    if (m->authed) {
        g_si_connecting = 0;
        g_si_err[0] = '\0';
        g_view = VIEW_HOME;
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
static void switch_workspace(HWND hwnd, const char *ws, const char *cred) {
    reset_session();
    g_view = VIEW_HOME;
    connect_start(ws, cred ? cred : "");
    layout_signin(hwnd);
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
    char enc[288];
    snprintf(enc, sizeof enc, "t:%d;h:%d;m:%d;d:%d;q:%s",
             oc_theme_mode(), g_pref_time24, g_pref_members, g_pref_daysep, g_quick_names);
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
    g_n_sw = 0;
    const char *sp = store_path();
    oc_store *s = sp ? oc_store_open(sp) : NULL;
    if (s) {
        oc_store_set_secret(s, g_secret);   /* the book lives in the credential store */
        oc_store_workspace_each(s, sw_book_cb, NULL);
        oc_store_close(s);
    }
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
    case 80: reset_session(); signin_begin(hwnd, NULL, NULL); break;
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
        oc_field f[1] = { { FF_TEXT, "Webhook label",
                            "Shown as the sender for messages posted through it.", "" } };
        if (form_dialog(hwnd, "Create webhook", f, 1) && f[0].value[0]) {
            oc_client_create_webhook(g_client, cid, f[0].value);
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
    } else if (!strcmp(verb, "palette")) {
        palette_open(hwnd);
        if (arg[0]) { WCHAR w[64]; to_w(arg, w, 64); SetWindowTextW(g_pal_edit, w); }
        test_ack("ok");
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
            /* A sign-in failure belongs in the card, not a toast — suppress the
             * toast channel while the sign-in view owns the window. */
            if (g_view == VIEW_SIGNIN) { if (g_si_connecting) signin_poll(hwnd); }
            else toast_tick(m);
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
            /* Sign-out returns to the sign-in view rather than quitting the
             * app (Slack's behaviour), so signing back in — as the same user or
             * a different one — needs no restart. */
            if (g_logging_out && !m->connected) {     /* logout frame sent + server dropped us */
                g_logging_out = 0;
                char ws[256]; snprintf(ws, sizeof ws, "%s", g_cur_ws);
                reset_session();
                signin_begin(hwnd, ws, NULL);
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
    case WM_SIZE:
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
        } else if ((float)mx < RAIL_W) {
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
        if (wp == VK_OEM_2 && (GetKeyState(VK_CONTROL) & 0x8000)) {   /* Ctrl+/ */
            int on = !g_keys_open;
            close_overlays();
            g_keys_open = on; g_view = VIEW_HOME;
            InvalidateRect(hwnd, NULL, FALSE);
            return 0;
        }
        if (wp == 'F' && (GetKeyState(VK_CONTROL) & 0x8000)) { search_open(hwnd); return 0; }
        if (wp == 'K' && (GetKeyState(VK_CONTROL) & 0x8000)) { palette_open(hwnd); return 0; }
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
    case WM_CLOSE:
        /* WIN-59: the outbox is in memory now (ARCH-88), so quitting with a send
         * still queued loses it. Make that a choice rather than a surprise. */
        if (g_client) {
            int pending = oc_client_outbox_pending(g_client);
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

    LoadLibraryW(L"Msftedit.dll");        /* registers MSFTEDIT_CLASS (RICHEDIT50W) */
    /* Before anything paints: the palette is runtime state now, and every
     * OC_COL_* reads through it. */
    oc_theme_apply(OC_THEME_DARK);
    d2d_init();                           /* factory only; the RT is made per-hwnd in paint */
    if (direct) connect_start(aws, acred);
    else        g_view = VIEW_SIGNIN;

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
