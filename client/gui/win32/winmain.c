/*
 * OpenChime Win32 GUI client (ARCH-80/82) — pure C, over the shared app-core
 * (client/core). PHASE 0: the toolchain + core-wiring spike. It opens a native
 * window, spins up a Direct2D + DirectWrite render target (the custom-surface
 * stack the transcript/sidebar will use), connects to a daemon via the core, and
 * DirectWrite-draws the live connection status. It proves the riskiest thing —
 * Direct2D/DirectWrite-from-C compiles + links under mingw and renders — and that
 * the core wires up on Windows. The three-pane shell + widgets come in later
 * phases; no product logic lives here (the core owns it, ARCH-74).
 *
 *   openchime.exe [<workspace> <user:pass>]   (defaults: 127.0.0.1:8443 alice:pw)
 *
 * The core owns its network thread; this frontend is pure view + input, ticked on
 * a WM_TIMER (the GUI analogue of the TUI's 30 ms poll loop).
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN
#endif
#define COBJMACROS            /* C-style COM: Interface_Method(obj, ...) */
#include <windows.h>
#include <shellapi.h>         /* CommandLineToArgvW */
#include <d2d1.h>
#include <dwrite.h>

/* mingw ships IID_ID2D1Factory in libuuid but not IID_IDWriteFactory; define it
 * locally so we don't depend on the toolchain's GUID table for DWrite. */
static const GUID OC_IID_IDWriteFactory =
    { 0xb859ee5a, 0xd838, 0x4b5b, { 0xa2, 0xe8, 0x1a, 0xdc, 0x7d, 0x93, 0xdb, 0x48 } };

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "client.h"
#include "model.h"
#include "resolve.h"
#include "oc_port.h"          /* oc_mkdir */
#include "theme.h"

#define TIMER_TICK 1

/* ---- app state ----------------------------------------------------------- */

static oc_client *g_client;
static char       g_cred[264];        /* "user:pass" for the connect */
static char       g_host[256];
static int        g_port;

static ID2D1Factory          *g_factory;
static IDWriteFactory        *g_dwrite;
static ID2D1HwndRenderTarget *g_rt;   /* recreated on device loss / resize */
static IDWriteTextFormat     *g_fmt;

/* ---- Direct2D helpers (COM-from-C) --------------------------------------- */

static D2D1_COLOR_F col(uint32_t rgb) {
    D2D1_COLOR_F c;
    c.r = ((rgb >> 16) & 0xff) / 255.0f;
    c.g = ((rgb >> 8) & 0xff) / 255.0f;
    c.b = (rgb & 0xff) / 255.0f;
    c.a = 1.0f;
    return c;
}

static void d2d_init(void) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &IID_ID2D1Factory, NULL,
                      (void **)&g_factory);
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, &OC_IID_IDWriteFactory,
                        (IUnknown **)&g_dwrite);
    if (g_dwrite)
        IDWriteFactory_CreateTextFormat(g_dwrite, L"Segoe UI", NULL,
            DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-us", &g_fmt);
}

/* Create (or recreate) the window-bound render target at the current client size. */
static void d2d_ensure_rt(HWND hwnd) {
    if (g_rt || !g_factory) return;
    RECT rc; GetClientRect(hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES rtp;
    ZeroMemory(&rtp, sizeof rtp);                    /* defaults: B8G8R8A8, ignore alpha */
    D2D1_HWND_RENDER_TARGET_PROPERTIES hp;
    hp.hwnd = hwnd;
    hp.pixelSize.width  = (UINT32)(rc.right - rc.left);
    hp.pixelSize.height = (UINT32)(rc.bottom - rc.top);
    hp.presentOptions = D2D1_PRESENT_OPTIONS_NONE;
    ID2D1Factory_CreateHwndRenderTarget(g_factory, &rtp, &hp, &g_rt);
}

static void d2d_resize(HWND hwnd) {
    if (!g_rt) return;
    RECT rc; GetClientRect(hwnd, &rc);
    D2D1_SIZE_U s = { (UINT32)(rc.right - rc.left), (UINT32)(rc.bottom - rc.top) };
    ID2D1HwndRenderTarget_Resize(g_rt, &s);
}

/* The current human status line, from the model. */
static void status_text(char *out, size_t cap) {
    if (!g_client) { snprintf(out, cap, "starting…"); return; }
    const oc_model *m = oc_client_model(g_client);
    if (m->authed) {
        const char *me = m->user_id ? oc_model_user_name(m, m->user_id) : "";
        snprintf(out, cap, "connected as %s  (Phase 0 — the shell comes next)",
                 me && me[0] ? me : "you");
    } else if (m->last_error[0] && !m->connected) {
        snprintf(out, cap, "not connected: %s", m->last_error);
    } else if (m->connected) {
        snprintf(out, cap, "connected — authenticating…");
    } else {
        snprintf(out, cap, "connecting to %s:%d…", g_host, g_port);
    }
}

static void paint(HWND hwnd) {
    d2d_ensure_rt(hwnd);
    if (!g_rt) return;
    ID2D1RenderTarget *rt = (ID2D1RenderTarget *)g_rt;

    ID2D1RenderTarget_BeginDraw(rt);
    D2D1_COLOR_F bg = col(OC_COL_BASE);
    ID2D1RenderTarget_Clear(rt, &bg);

    ID2D1SolidColorBrush *brush = NULL;
    D2D1_COLOR_F fg = col(OC_COL_TEXT);
    ID2D1RenderTarget_CreateSolidColorBrush(rt, &fg, NULL, &brush);

    if (brush && g_fmt) {
        char s[256]; status_text(s, sizeof s);
        WCHAR w[256];
        int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, w, 256);
        RECT rc; GetClientRect(hwnd, &rc);
        D2D1_RECT_F r = { 24.0f, 24.0f, (FLOAT)rc.right - 24.0f, (FLOAT)rc.bottom - 24.0f };
        ID2D1RenderTarget_DrawText(rt, w, (UINT32)(n > 0 ? n - 1 : 0), g_fmt, &r,
                                   (ID2D1Brush *)brush, D2D1_DRAW_TEXT_OPTIONS_NONE,
                                   DWRITE_MEASURING_MODE_NATURAL);
    }
    if (brush) ID2D1SolidColorBrush_Release(brush);

    HRESULT hr = ID2D1RenderTarget_EndDraw(rt, NULL, NULL);
    if (hr == (HRESULT)D2DERR_RECREATE_TARGET) {  /* device lost — rebuild next paint */
        ID2D1HwndRenderTarget_Release(g_rt);
        g_rt = NULL;
    }
}

/* ---- core wiring (carried from the comctl32 draft, ARCH-82) --------------- */

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
        SetTimer(hwnd, TIMER_TICK, 30, NULL);        /* ~30 Hz: tick + repaint */
        return 0;
    case WM_TIMER:
        if (wp == TIMER_TICK && g_client) {
            oc_client_tick(g_client);
            InvalidateRect(hwnd, NULL, FALSE);       /* model may have advanced */
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
    case WM_ERASEBKGND:
        return 1;                                    /* D2D owns the surface */
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

    /* Dev direct-connect args, mirroring the TUI: <workspace> <user:pass>. */
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
                    WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 1000, 660,
                    NULL, NULL, inst, NULL);
    if (!hwnd) return 1;
    ShowWindow(hwnd, show);
    UpdateWindow(hwnd);

    MSG m;
    while (GetMessageW(&m, NULL, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }

    if (g_fmt)    IDWriteTextFormat_Release(g_fmt);
    if (g_rt)     ID2D1HwndRenderTarget_Release(g_rt);
    if (g_dwrite) IDWriteFactory_Release(g_dwrite);
    if (g_factory) ID2D1Factory_Release(g_factory);
    return 0;
}
