/*
 * OpenChime Win32 GUI client (ARCH-80) — pure C, native controls, over the
 * shared app-core (client/core). This first pass is the login surface: a native
 * window with Workspace / Username / Password fields and a Connect button that
 * drives oc_client to a real authenticated session against the daemon. The chat
 * view is a later pass; the point here is to prove the native-GUI path over the
 * ported core, the way the TUI proved the terminal path.
 *
 * The core owns its own network thread; this frontend is pure view + input. It
 * ticks the client on a WM_TIMER (the GUI analogue of the TUI's poll loop) and
 * renders the model into the status line. No logic lives here.
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "model.h"
#include "resolve.h"

/* Control ids */
enum { ID_WS = 100, ID_USER, ID_PASS, ID_CONNECT, ID_STATUS };
#define TIMER_TICK 1

static oc_client *g_client;
static HWND g_hws, g_huser, g_hpass, g_hconnect, g_hstatus;
static int  g_authed_shown;

/* Read a control's text into a fixed buffer (UTF-8 not needed here — the fields
 * are host/user/pass, ASCII in practice; GetWindowTextA suffices for now). */
static void get_text(HWND h, char *out, int cap) {
    GetWindowTextA(h, out, cap);
}

static void set_status(const char *s) {
    SetWindowTextA(g_hstatus, s);
}

/* Parse "host:port" typed in the Workspace field, or resolve a workspace name.
 * Mirrors the TUI: an explicit :port pins host:port; otherwise DNS resolution.
 * Returns 0 and fills host/port, or -1. */
static int resolve_target(const char *ws, char *host, size_t hostcap, int *port) {
    oc_endpoint ep;
    oc_resolve_status st = oc_resolve(ws, getenv("OPENCHIME_SUFFIX"), &ep);
    if (st != OC_RESOLVE_OK) return -1;
    snprintf(host, hostcap, "%s", ep.host);
    *port = ep.port;
    return 0;
}

static void do_connect(HWND hwnd) {
    if (g_client) { set_status("already connecting"); return; }

    char ws[256], user[128], pass[128];
    get_text(g_hws, ws, sizeof ws);
    get_text(g_huser, user, sizeof user);
    get_text(g_hpass, pass, sizeof pass);
    if (!ws[0])   { set_status("enter a workspace (e.g. 127.0.0.1:8443)"); return; }
    if (!user[0]) { set_status("enter a username"); return; }

    char host[256]; int port = 0;
    if (resolve_target(ws, host, sizeof host, &port) != 0) {
        set_status("could not resolve that workspace");
        return;
    }

    char cred[264];
    snprintf(cred, sizeof cred, "%s:%s", user, pass);

    /* Store/keyring are NULL for this first pass — no persistence yet, matching
     * the "basic, get to login" scope. */
    g_client = oc_client_start_secure(host, port, cred, NULL, NULL);
    if (!g_client) { set_status("failed to start client"); return; }

    g_authed_shown = 0;
    EnableWindow(g_hconnect, FALSE);
    set_status("connecting…");
    SetTimer(hwnd, TIMER_TICK, 30, NULL);   /* ~30 Hz: drain events, refresh UI */
}

/* Called on every timer tick: drain the core's events into the model, then
 * reflect connection/auth state in the status line. */
static void on_tick(HWND hwnd) {
    if (!g_client) return;
    oc_client_tick(g_client);
    const oc_model *m = oc_client_model(g_client);

    if (m->authed) {
        if (!g_authed_shown) {
            char s[160];
            const char *me = m->user_id ? oc_model_user_name(m, m->user_id) : "";
            snprintf(s, sizeof s, "connected — signed in as %s  (%zu channel%s)",
                     me[0] ? me : "you", m->n_channels, m->n_channels == 1 ? "" : "s");
            set_status(s);
            g_authed_shown = 1;
        }
        return;
    }
    if (m->last_error[0] && !m->connected) {
        char s[192];
        snprintf(s, sizeof s, "not connected: %s", m->last_error);
        set_status(s);
        KillTimer(hwnd, TIMER_TICK);
        oc_client_stop(g_client);
        g_client = NULL;
        EnableWindow(g_hconnect, TRUE);
        return;
    }
    if (m->connected) set_status("connected — authenticating…");
}

/* A labeled edit row at (y). `pw` masks the input. Returns the edit HWND. */
static HWND add_field(HWND parent, HINSTANCE inst, const char *label, int id,
                      int y, int pw) {
    CreateWindowExA(0, "STATIC", label, WS_CHILD | WS_VISIBLE,
                    20, y + 3, 90, 20, parent, NULL, inst, NULL);
    DWORD style = WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | (pw ? ES_PASSWORD : 0);
    return CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", "", style,
                           115, y, 260, 24, parent, (HMENU)(INT_PTR)id, inst, NULL);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        HINSTANCE inst = ((LPCREATESTRUCT)lp)->hInstance;
        CreateWindowExA(0, "STATIC", "Sign in to OpenChime",
                        WS_CHILD | WS_VISIBLE, 20, 16, 300, 22, hwnd, NULL, inst, NULL);
        g_hws   = add_field(hwnd, inst, "Workspace", ID_WS,   52, 0);
        g_huser = add_field(hwnd, inst, "Username",  ID_USER, 88, 0);
        g_hpass = add_field(hwnd, inst, "Password",  ID_PASS, 124, 1);
        SetWindowTextA(g_hws, "127.0.0.1:8443");   /* dev default; user edits it */

        g_hconnect = CreateWindowExA(0, "BUTTON", "Connect",
                        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
                        115, 162, 100, 30, hwnd, (HMENU)(INT_PTR)ID_CONNECT, inst, NULL);
        g_hstatus = CreateWindowExA(0, "STATIC", "",
                        WS_CHILD | WS_VISIBLE, 20, 208, 380, 44, hwnd, (HMENU)(INT_PTR)ID_STATUS, inst, NULL);
        return 0;
    }
    case WM_COMMAND:
        if (LOWORD(wp) == ID_CONNECT && HIWORD(wp) == BN_CLICKED) {
            do_connect(hwnd);
            return 0;
        }
        return 0;
    case WM_TIMER:
        if (wp == TIMER_TICK) on_tick(hwnd);
        return 0;
    case WM_DESTROY:
        if (g_client) { KillTimer(hwnd, TIMER_TICK); oc_client_stop(g_client); g_client = NULL; }
        PostQuitMessage(0);
        return 0;
    default:
        return DefWindowProcA(hwnd, msg, wp, lp);
    }
}

int WINAPI WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show) {
    (void)prev; (void)cmdline;
    INITCOMMONCONTROLSEX icc = { sizeof icc, ICC_STANDARD_CLASSES };
    InitCommonControlsEx(&icc);

    WNDCLASSA wc;
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = inst;
    wc.hCursor       = LoadCursor(NULL, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "OpenChimeWin";
    if (!RegisterClassA(&wc)) return 1;

    HWND hwnd = CreateWindowExA(0, "OpenChimeWin", "OpenChime",
                    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX,
                    CW_USEDEFAULT, CW_USEDEFAULT, 440, 320,
                    NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessage(&m, NULL, 0, 0) > 0) {
        if (IsDialogMessage(hwnd, &m)) continue;   /* Tab between fields, Enter = default btn */
        TranslateMessage(&m);
        DispatchMessage(&m);
    }
    return 0;
}
