/* OpenChime GUI -- the Windows platform (client/gui/platform/platform.h).
 *
 * Everything Win32 the application needs and SDL does not give it: the crash
 * handler and minidump, the identity the shell knows the process by, the
 * taskbar overlay and progress, the tray icon, the single-instance handoff, the
 * file pickers, WIC for images, the clipboard's images and files, the registry
 * for autostart and the URL scheme, DWM for the caption and corners, and the
 * UIA provider's hook on the window.
 *
 * SDL owns the main HWND. The provider's WM_GETOBJECT and the shell's
 * TaskbarButtonCreated need an answer from the window procedure, which SDL's
 * message hook cannot give (it can only pass or drop), so the HWND is
 * subclassed and everything else chains to SDL's procedure. A second,
 * message-only window of our own class carries the tray icon and receives
 * WM_COPYDATA, because FindWindow by class is how another process finds us and
 * every SDL application shares SDL's class name. */
#define COBJMACROS
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <wincodec.h>
#include <wincrypt.h>
#include <cryptuiapi.h>
#include <dbghelp.h>
#include <mmsystem.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "a11y_win32.h"
#include "openchime_res.h"
#include "st_dwrite.h"

#define OC_WIN_CLASS   L"OpenChimeWin"
#define OC_AUMID       L"BronzeVenture.OpenChime"
#define TRAY_UID       1
#define WM_APP_TRAY    (WM_APP + 1)
#define OC_COPYDATA_URL    0x4F43
#define OC_COPYDATA_ACTION 0x4F44

static HWND     g_main;             /* SDL's window */
static WNDPROC  g_sdl_proc;         /* ... and its procedure, chained to */
static HWND     g_msgwnd;           /* ours: tray + handoff */
static uint32_t g_evtype;
static UINT     g_taskbar_created_msg;
static ITaskbarList3 *g_taskbar;
static int      g_taskbar_dead;
static HRESULT  g_taskbar_hr;
static IWICImagingFactory *g_wic;
static NOTIFYICONDATAW g_tray;
static int      g_tray_live;

static HWND hwnd_of(SDL_Window *w) {
    return w ? (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(w),
                                            SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL) : NULL;
}

static int to_w(const char *s, WCHAR *out, int cap) {
    if (!s) { out[0] = 0; return 0; }
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, out, cap);
    if (n <= 0) { out[cap - 1] = 0; return 0; }
    return n - 1;
}
static int to_u8(const WCHAR *w, char *out, size_t cap) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int)cap, NULL, NULL);
    if (n <= 0) { out[0] = 0; return 0; }
    return n - 1;
}

uint32_t oc_plat_event_type(void) {
    if (!g_evtype) g_evtype = SDL_RegisterEvents(1);
    return g_evtype;
}

static void push_event(int code, void *d1, void *d2) {
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = oc_plat_event_type();
    e.user.code = code;
    e.user.data1 = d1;
    e.user.data2 = d2;
    SDL_PushEvent(&e);
}

/* ---- crash reports --------------------------------------------------------- */

static char g_crash_dir[600];
static oc_plat_report_fn g_report_fn;
static void *g_report_user;

typedef BOOL (WINAPI *mdwd_fn)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE,
                               const MINIDUMP_EXCEPTION_INFORMATION *, void *, void *);

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep) {
    DWORD pid = GetCurrentProcessId();
    char txt[700]; snprintf(txt, sizeof txt, "%s\\crash-%lu.txt", g_crash_dir, (unsigned long)pid);
    FILE *f = fopen(txt, "wb");
    if (f) {
        EXCEPTION_RECORD *er = ep && ep->ExceptionRecord ? ep->ExceptionRecord : NULL;
        void *base = (void *)GetModuleHandleW(NULL);
        void *addr = er ? er->ExceptionAddress : NULL;
        fprintf(f, "openchime crash\n");
        fprintf(f, "code=0x%08lx addr=%p module=%p rva=0x%llx\n",
                er ? (unsigned long)er->ExceptionCode : 0, addr, base,
                (addr && base) ? (unsigned long long)((char *)addr - (char *)base) : 0ull);
        if (er && er->ExceptionCode == (DWORD)EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
            fprintf(f, "access=%s at 0x%llx\n",
                    er->ExceptionInformation[0] == 0 ? "read" :
                    er->ExceptionInformation[0] == 1 ? "write" : "execute",
                    (unsigned long long)er->ExceptionInformation[1]);
        if (g_report_fn) g_report_fn(f, g_report_user);
        /* The dump is attempted while the report is open, so the report can
         * say whether one exists: a zero-length .dmp beside a crash log looks
         * like evidence. Stacks, threads and modules only -- the data segments
         * hold session tokens, message text and keys. */
        HMODULE dbg = LoadLibraryW(L"dbghelp.dll");
        int dumped = 0; DWORD dump_err = 0;
        char dmp[700]; snprintf(dmp, sizeof dmp, "%s\\crash-%lu.dmp", g_crash_dir, (unsigned long)pid);
        if (dbg) {
            mdwd_fn mdwd = (mdwd_fn)(void *)GetProcAddress(dbg, "MiniDumpWriteDump");
            if (mdwd) {
                HANDLE h = CreateFileA(dmp, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
                if (h != INVALID_HANDLE_VALUE) {
                    MINIDUMP_EXCEPTION_INFORMATION mei;
                    mei.ThreadId = GetCurrentThreadId();
                    mei.ExceptionPointers = ep;
                    mei.ClientPointers = FALSE;
                    dumped = mdwd(GetCurrentProcess(), pid, h, MiniDumpNormal | MiniDumpWithHandleData,
                                  &mei, NULL, NULL) ? 1 : 0;
                    if (!dumped) dump_err = GetLastError();
                    CloseHandle(h);
                }
            } else dump_err = GetLastError();
        } else dump_err = GetLastError();
        if (!dumped) { DeleteFileA(dmp); fprintf(f, "minidump: FAILED (err=%lu)\n", (unsigned long)dump_err); }
        else         fprintf(f, "minidump: %s\n", dmp);
        fclose(f);
    }
    /* Let the process die: swallowing the fault would leave a client running
     * on corrupt state, which is worse than an exit the user can see. */
    return EXCEPTION_EXECUTE_HANDLER;
}

/* The CRT's two ways out that never reach an exception filter. Both raise a
 * real exception so the ordinary path writes the ordinary report. */
static void crt_raise(void) {
    RaiseException(0xE0C11A5Du, EXCEPTION_NONCONTINUABLE, 0, NULL);
}
static void crt_bad_param(const wchar_t *expr, const wchar_t *fn, const wchar_t *file,
                          unsigned line, uintptr_t reserved) {
    (void)expr; (void)fn; (void)file; (void)line; (void)reserved;
    crt_raise();
}
static void crt_on_abort(int sig) { (void)sig; crt_raise(); }

void oc_plat_boot(void) {
    SetUnhandledExceptionFilter(crash_filter);
    /* A stack overflow runs no handler unless a slice is reserved for it. */
    { ULONG guard = 64 * 1024; SetThreadStackGuarantee(&guard); }
    _set_invalid_parameter_handler(crt_bad_param);
    signal(SIGABRT, crt_on_abort);
    /* COM, single-threaded apartment: the taskbar and WIC live on it. */
    CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    oc_plat_data_dir(g_crash_dir, sizeof g_crash_dir);
}

void oc_plat_after_sdl_init(void) {
    /* SDL installs its own filter, and the last installer wins. */
    SetUnhandledExceptionFilter(crash_filter);
}

void oc_plat_crash_reports(const char *dir, oc_plat_report_fn app_part, void *user) {
    if (dir && dir[0]) snprintf(g_crash_dir, sizeof g_crash_dir, "%s", dir);
    g_report_fn = app_part;
    g_report_user = user;
}

int oc_plat_data_dir(char *out, size_t cap) {
    const char *base = getenv("LOCALAPPDATA");
    if (!base || !base[0]) base = getenv("TEMP");
    if (!base || !base[0]) { snprintf(out, cap, "."); return 0; }
    snprintf(out, cap, "%s\\OpenChime", base);
    CreateDirectoryA(out, NULL);
    return 1;
}

static struct { void *p; HANDLE file, map; } g_rings[4];

void *oc_plat_ring_map(const char *path, size_t size) {
    int slot = -1;
    for (int i = 0; i < 4; i++) if (!g_rings[i].p) { slot = i; break; }
    if (slot < 0) return NULL;
    HANDLE fh = CreateFileA(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (fh == INVALID_HANDLE_VALUE) return NULL;
    HANDLE mp = CreateFileMappingW(fh, NULL, PAGE_READWRITE, 0, (DWORD)size, NULL);
    void *p = mp ? MapViewOfFile(mp, FILE_MAP_ALL_ACCESS, 0, 0, size) : NULL;
    if (!p) { if (mp) CloseHandle(mp); CloseHandle(fh); return NULL; }
    memset(p, 0, size);
    g_rings[slot].p = p; g_rings[slot].file = fh; g_rings[slot].map = mp;
    return p;
}

void oc_plat_ring_unmap(void *p, size_t size) {
    for (int i = 0; i < 4; i++)
        if (g_rings[i].p == p) {
            FlushViewOfFile(p, size);
            UnmapViewOfFile(p);
            CloseHandle(g_rings[i].map);
            CloseHandle(g_rings[i].file);
            memset(&g_rings[i], 0, sizeof g_rings[i]);
            return;
        }
}

/* ---- the message window: tray, handoff ------------------------------------- */

static LRESULT CALLBACK msg_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_COPYDATA: {
        const COPYDATASTRUCT *cds = (const COPYDATASTRUCT *)lp;
        if (!cds || !cds->lpData) return 0;
        const char *kind = cds->dwData == OC_COPYDATA_ACTION ? "action"
                         : cds->dwData == OC_COPYDATA_URL    ? "url" : NULL;
        if (!kind) return 0;
        size_t kl = strlen(kind), pl = cds->cbData;
        char *s = malloc(kl + 1 + pl + 1);
        if (!s) return 0;
        memcpy(s, kind, kl); s[kl] = '\n';
        memcpy(s + kl + 1, cds->lpData, pl); s[kl + 1 + pl] = 0;
        push_event(OC_PLAT_EV_HANDOFF, s, NULL);
        return 1;
    }
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_LBUTTONDBLCLK)
            push_event(OC_PLAT_EV_TRAY, (void *)(intptr_t)OC_TRAY_CLICK, NULL);
        else if (LOWORD(lp) == NIN_BALLOONUSERCLICK)
            push_event(OC_PLAT_EV_TRAY, (void *)(intptr_t)OC_TRAY_BALLOON_CLICK, NULL);
        else if (LOWORD(lp) == WM_RBUTTONUP)
            push_event(OC_PLAT_EV_TRAY, (void *)(intptr_t)OC_TRAY_RCLICK, NULL);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

static HWND msgwnd(void) {
    if (g_msgwnd) return g_msgwnd;
    static int registered;
    HINSTANCE inst = GetModuleHandleW(NULL);
    if (!registered) {
        WNDCLASSEXW wc; memset(&wc, 0, sizeof wc);
        wc.cbSize = sizeof wc; wc.lpfnWndProc = msg_proc; wc.hInstance = inst;
        wc.lpszClassName = OC_WIN_CLASS;
        if (!RegisterClassExW(&wc)) return NULL;
        registered = 1;
    }
    g_msgwnd = CreateWindowExW(0, OC_WIN_CLASS, L"OpenChime", 0, 0, 0, 0, 0,
                               HWND_MESSAGE, NULL, inst, NULL);
    return g_msgwnd;
}

int oc_plat_open_signin(const char *url) { return SDL_OpenURL(url) ? 1 : 0; }
int oc_plat_signin_result(char *out, size_t cap) { (void)out; (void)cap; return 0; }

int oc_plat_handoff(const char *kind, const char *payload) {
    HWND other = FindWindowW(OC_WIN_CLASS, NULL);
    if (!other || other == g_msgwnd) return 0;
    COPYDATASTRUCT cds;
    cds.dwData = !strcmp(kind, "action") ? OC_COPYDATA_ACTION : OC_COPYDATA_URL;
    cds.cbData = (DWORD)strlen(payload);
    cds.lpData = (void *)payload;
    SendMessageW(other, WM_COPYDATA, 0, (LPARAM)&cds);
    return 1;
}

int oc_plat_tray_init(SDL_Window *w, const char *tooltip) {
    (void)w;
    HWND h = msgwnd();
    if (!h) return 0;
    memset(&g_tray, 0, sizeof g_tray);
    g_tray.cbSize = sizeof g_tray;
    g_tray.hWnd = h;
    g_tray.uID = TRAY_UID;
    /* NIF_MESSAGE is what makes it a control rather than a mailbox. */
    g_tray.uFlags = NIF_ICON | NIF_TIP | NIF_MESSAGE;
    g_tray.uCallbackMessage = WM_APP_TRAY;
    g_tray.hIcon = LoadIconW(GetModuleHandleW(NULL), MAKEINTRESOURCEW(IDI_APPICON));
    if (!g_tray.hIcon) g_tray.hIcon = LoadIconW(NULL, MAKEINTRESOURCEW(32512));
    to_w(tooltip ? tooltip : "OpenChime", g_tray.szTip, 128);
    g_tray_live = Shell_NotifyIconW(NIM_ADD, &g_tray) ? 1 : 0;
    /* Version 4, and it is not optional: without it the shell never sends
     * NIN_BALLOONUSERCLICK at all. */
    if (g_tray_live) { g_tray.uVersion = NOTIFYICON_VERSION_4; Shell_NotifyIconW(NIM_SETVERSION, &g_tray); }
    return g_tray_live;
}

void oc_plat_tray_done(void) {
    if (g_tray_live) { Shell_NotifyIconW(NIM_DELETE, &g_tray); g_tray_live = 0; }
}

void *oc_plat_tray_notify_handle(unsigned *tray_id) {
    if (tray_id) *tray_id = TRAY_UID;
    return g_tray_live ? (void *)g_msgwnd : NULL;
}

/* ---- the main window ------------------------------------------------------- */

static LRESULT CALLBACK main_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_GETOBJECT) {
        /* The whole accessibility surface hangs off this one message (ARCH-99). */
        int handled = 0;
        LRESULT r = oc_a11y_get_object(h, wp, lp, &handled);
        if (handled) return r;
    } else if (g_taskbar_created_msg && msg == g_taskbar_created_msg) {
        /* Explorer restarted: its new taskbar has no overlay and the old COM
         * proxy points at the dead shell. Drop both; the application re-applies. */
        if (g_taskbar) { ITaskbarList3_Release(g_taskbar); g_taskbar = NULL; }
        g_taskbar_dead = 0;
        push_event(OC_PLAT_EV_TASKBAR_RESET, NULL, NULL);
        return 0;
    }
    return CallWindowProcW(g_sdl_proc, h, msg, wp, lp);
}

void oc_plat_window_attach(SDL_Window *w) {
    HWND h = hwnd_of(w);
    if (!h || g_main) return;
    g_main = h;
    HINSTANCE inst = GetModuleHandleW(NULL);
    /* The identity the shell resolves notifications through: the same string
     * the installer puts on the Start-menu shortcut. */
    {
        HRESULT (WINAPI *setid)(PCWSTR) = NULL;
        HMODULE sh = LoadLibraryW(L"shell32.dll");
        if (sh) setid = (HRESULT (WINAPI *)(PCWSTR))(void *)GetProcAddress(sh, "SetCurrentProcessExplicitAppUserModelID");
        if (setid) setid(OC_AUMID);
    }
    /* The window's own icon at the shell's metrics, so the taskbar button and
     * the thumbnail agree about what this application looks like. */
    HICON big = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXICON), GetSystemMetrics(SM_CYICON), LR_DEFAULTCOLOR);
    HICON sml = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                  GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), LR_DEFAULTCOLOR);
    if (big) SendMessageW(h, WM_SETICON, ICON_BIG, (LPARAM)big);
    if (sml) SendMessageW(h, WM_SETICON, ICON_SMALL, (LPARAM)sml);
    g_taskbar_created_msg = RegisterWindowMessageW(L"TaskbarButtonCreated");
    g_sdl_proc = (WNDPROC)SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)main_proc);
    msgwnd();
}

/* ShowWindow alone is not enough: Windows will not activate a window shown by
 * a process that does not already own the foreground, and this client shows
 * late by design when it auto-connects. SetForegroundWindow from the window's
 * own process is what Windows allows, and it was measured to work where the
 * attach-thread-input dance does not. */
void oc_plat_window_front(SDL_Window *w) {
    HWND h = hwnd_of(w);
    if (!h) return;
    if (!IsWindowVisible(h)) ShowWindow(h, SW_SHOW);
    if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
    SetForegroundWindow(h);
    SetActiveWindow(h);
    SetFocus(h);
}

int oc_plat_window_is_front(SDL_Window *w) { HWND h = hwnd_of(w); return h && GetForegroundWindow() == h; }
int oc_plat_window_has_keyboard(SDL_Window *w) { HWND h = hwnd_of(w); return h && GetFocus() == h; }

void oc_plat_window_caption_dark(SDL_Window *w, int dark) {
    HWND h = hwnd_of(w);
    if (!h) return;
    BOOL d = dark ? TRUE : FALSE;
    if (FAILED(DwmSetWindowAttribute(h, 20, &d, sizeof d)))
        DwmSetWindowAttribute(h, 19, &d, sizeof d);
}

int oc_plat_window_round(SDL_Window *w) {
    HWND h = hwnd_of(w);
    if (!h) return -1;
    /* 33 = DWMWA_WINDOW_CORNER_PREFERENCE, 2 = DWMWCP_ROUND; written out
     * because the mingw headers predate them. Ignored on Windows 10. */
    DWORD round = 2;
    return (int)DwmSetWindowAttribute(h, 33, &round, sizeof round);
}

int oc_plat_window_capture_exclude(SDL_Window *w, int on) {
    HWND h = hwnd_of(w);
    if (!h) return 0;
    return SetWindowDisplayAffinity(h, on ? 0x11 /* WDA_EXCLUDEFROMCAPTURE */ : 0) ? 1 : 0;
}

void oc_plat_window_click_through(SDL_Window *w, int on) {
    HWND h = hwnd_of(w);
    if (!h) return;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (on) ex |= WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW;
    else    ex &= ~(LONG_PTR)WS_EX_TRANSPARENT;
    SetWindowLongPtrW(h, GWL_EXSTYLE, ex);
}

static const GUID OC_CLSID_TaskbarList =
    { 0x56FDF344, 0xFD6D, 0x11D0, { 0x95, 0x8A, 0x00, 0x60, 0x97, 0xC9, 0xA0, 0x90 } };
static const GUID OC_IID_ITaskbarList3 =
    { 0xEA1AFB91, 0x9E28, 0x4B86, { 0x90, 0xE9, 0x9E, 0x9F, 0x8A, 0x5E, 0xEF, 0xAF } };

static ITaskbarList3 *taskbar_obj(void) {
    if (g_taskbar_dead) return NULL;
    if (!g_taskbar) {
        g_taskbar_hr = CoCreateInstance(&OC_CLSID_TaskbarList, NULL, CLSCTX_INPROC_SERVER,
                                        &OC_IID_ITaskbarList3, (void **)&g_taskbar);
        if (SUCCEEDED(g_taskbar_hr) && g_taskbar) g_taskbar_hr = ITaskbarList3_HrInit(g_taskbar);
        if (FAILED(g_taskbar_hr) || !g_taskbar) {
            if (g_taskbar) { ITaskbarList3_Release(g_taskbar); g_taskbar = NULL; }
            g_taskbar_dead = 1;
            return NULL;
        }
    }
    return g_taskbar;
}

/* A 32-bit icon for the overlay: a notice-coloured disc with the count in
 * white, at SM_CXSMICON. GDI text zeroes the alpha of every pixel it touches,
 * so the glyphs are rendered first as a grayscale mask and disc, text and
 * alpha are composed per pixel, the disc's coverage from a 4x4 supersample. */
static HICON badge_icon(int count, uint32_t disc) {
    int s = GetSystemMetrics(SM_CXSMICON);
    if (s < 16) s = 16;
    BITMAPINFO bi; ZeroMemory(&bi, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = s; bi.bmiHeader.biHeight = -s;
    bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    HDC mem = CreateCompatibleDC(NULL);
    void *bits = NULL;
    HBITMAP dib = mem ? CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0) : NULL;
    if (!dib || !bits) { if (dib) DeleteObject(dib); if (mem) DeleteDC(mem); return NULL; }
    HGDIOBJ oldbm = SelectObject(mem, dib);
    char t[4];
    if (count < 0)      t[0] = '\0';
    else if (count > 9) snprintf(t, sizeof t, "9+");
    else                snprintf(t, sizeof t, "%d", count);
    WCHAR wt[4]; to_w(t, wt, 4);
    HFONT font = CreateFontW(-(s * (count > 9 ? 10 : 12) / 16), 0, 0, 0, FW_BOLD, 0, 0, 0,
                             DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                             ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    HGDIOBJ oldf = SelectObject(mem, font);
    SetTextColor(mem, RGB(255, 255, 255));
    SetBkMode(mem, TRANSPARENT);
    RECT tr = { 0, 0, s, s };
    DrawTextW(mem, wt, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    GdiFlush();
    unsigned dr = (disc >> 16) & 0xFF, dg = (disc >> 8) & 0xFF, db = disc & 0xFF;
    uint32_t *px = (uint32_t *)bits;
    for (int y = 0; y < s; y++)
        for (int x = 0; x < s; x++) {
            int hits = 0;
            for (int sy = 0; sy < 4; sy++)
                for (int sx = 0; sx < 4; sx++) {
                    int ddx = 8 * x + 2 * sx + 1 - 4 * s, ddy = 8 * y + 2 * sy + 1 - 4 * s;
                    if (ddx * ddx + ddy * ddy <= (4 * s - 2) * (4 * s - 2)) hits++;
                }
            unsigned a = (unsigned)(hits * 255 / 16);
            unsigned tc = (px[y * s + x] >> 8) & 0xFF;
            unsigned r8 = (dr * (255 - tc) + 255 * tc) / 255;
            unsigned g8 = (dg * (255 - tc) + 255 * tc) / 255;
            unsigned b8 = (db * (255 - tc) + 255 * tc) / 255;
            px[y * s + x] = (a << 24) | (r8 << 16) | (g8 << 8) | b8;
        }
    SelectObject(mem, oldf); DeleteObject(font);
    SelectObject(mem, oldbm);
    int mrow = ((s + 15) / 16) * 2;
    uint8_t *mb = calloc((size_t)mrow * s, 1);
    HBITMAP mask = mb ? CreateBitmap(s, s, 1, 1, mb) : NULL;
    free(mb);
    ICONINFO ii = { TRUE, 0, 0, mask, dib };
    HICON ic = mask ? CreateIconIndirect(&ii) : NULL;
    if (mask) DeleteObject(mask);
    DeleteObject(dib); DeleteDC(mem);
    return ic;
}

int oc_plat_badge(SDL_Window *w, int count, uint32_t rgb) {
    HWND h = hwnd_of(w);
    if (!h || !taskbar_obj()) return 0;
    HICON ic = NULL;
    WCHAR alt[24] = L"";
    if (count != 0) {
        ic = badge_icon(count, rgb);
        if (!ic) return 0;
        char a[32];
        if (count < 0)      snprintf(a, sizeof a, "Unread messages");
        else if (count > 9) snprintf(a, sizeof a, "9+ for you");
        else                snprintf(a, sizeof a, "%d for you", count);
        to_w(a, alt, 24);
    }
    int ok = SUCCEEDED(ITaskbarList3_SetOverlayIcon(g_taskbar, h, ic, count != 0 ? alt : NULL));
    if (ic) DestroyIcon(ic);
    return ok;
}

int oc_plat_badge_status(void) { return (int)g_taskbar_hr; }

void oc_plat_progress(SDL_Window *w, uint64_t done, uint64_t total) {
    static uint64_t shown_done = UINT64_MAX, shown_total = UINT64_MAX;
    HWND h = hwnd_of(w);
    uint64_t d = total ? done * 1000 / total : 0, t = total ? 1000 : 0;
    if (d == shown_done && t == shown_total) return;
    ITaskbarList3 *tb = h ? taskbar_obj() : NULL;
    if (!tb) return;
    if (!t) ITaskbarList3_SetProgressState(tb, h, TBPF_NOPROGRESS);
    else {
        if (shown_total != 1000) ITaskbarList3_SetProgressState(tb, h, TBPF_NORMAL);
        ITaskbarList3_SetProgressValue(tb, h, d, t);
    }
    shown_done = d; shown_total = t;
}

typedef struct { int want, at; RECT r; int found; } mon_find;
static BOOL CALLBACK share_mon_cb(HMONITOR hm, HDC dc, LPRECT rc, LPARAM lp) {
    (void)hm; (void)dc;
    mon_find *f = (mon_find *)lp;
    if (f->at++ == f->want) { f->r = *rc; f->found = 1; return FALSE; }
    return TRUE;
}

int oc_plat_share_rect(const char *id, int *x, int *y, int *w, int *h) {
    RECT r;
    if (!strncmp(id, "screen:", 7) && id[7] >= '0' && id[7] <= '9') {
        mon_find f = { atoi(id + 7), 0, { 0, 0, 0, 0 }, 0 };
        EnumDisplayMonitors(NULL, NULL, share_mon_cb, (LPARAM)&f);
        if (!f.found) return 0;
        r = f.r;
    } else if (!strncmp(id, "window:", 7)) {
        HWND t = (HWND)(uintptr_t)strtoull(id + 7, NULL, 16);
        if (!t || !IsWindow(t) || IsIconic(t)) return 0;
        if (DwmGetWindowAttribute(t, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r) != S_OK && !GetWindowRect(t, &r))
            return 0;
    } else return 0;
    *x = r.left; *y = r.top; *w = r.right - r.left; *h = r.bottom - r.top;
    return 1;
}

/* ---- files, the shell, settings ------------------------------------------- */

/* "Label|*.png;*.jpg" -> the double-NUL filter the dialog wants. */
static void filter_w(const char *filter, WCHAR *out, int cap) {
    out[0] = out[1] = 0;
    if (!filter) return;
    const char *bar = strchr(filter, '|');
    if (!bar) return;
    char label[128], pat[256];
    snprintf(label, sizeof label, "%.*s", (int)(bar - filter), filter);
    snprintf(pat, sizeof pat, "%s", bar + 1);
    int n = to_w(label, out, cap - 2);
    n += 1;
    n += to_w(pat, out + n, cap - n - 1);
    out[n + 1] = 0;
}

int oc_plat_pick_files(SDL_Window *owner, const char *filter, int multi, char *out, size_t cap) {
    static WCHAR file[32 * MAX_PATH];
    WCHAR flt[400];
    file[0] = 0;
    filter_w(filter, flt, 400);
    OPENFILENAMEW ofn; ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hwnd_of(owner);
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file / sizeof file[0];
    ofn.lpstrFilter = flt[0] ? flt : NULL;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR | OFN_EXPLORER | (multi ? OFN_ALLOWMULTISELECT : 0);
    if (!GetOpenFileNameW(&ofn)) return 0;
    size_t o = 0; int n = 0;
    const WCHAR *first = file, *next = file + wcslen(file) + 1;
    char path[1024];
    if (!multi || !*next) {
        to_u8(first, path, sizeof path);
        size_t l = strlen(path);
        if (l + 2 > cap) return 0;
        memcpy(out, path, l + 1); out[l + 1] = 0;
        return 1;
    }
    for (; *next; next += wcslen(next) + 1) {
        WCHAR full[MAX_PATH];
        if (_snwprintf(full, MAX_PATH, L"%ls\\%ls", first, next) < 0) continue;
        full[MAX_PATH - 1] = 0;
        to_u8(full, path, sizeof path);
        size_t l = strlen(path);
        if (o + l + 2 > cap) break;
        memcpy(out + o, path, l + 1); o += l + 1; n++;
    }
    out[o] = 0;
    return n;
}

int oc_plat_pick_save(SDL_Window *owner, const char *suggested, char *out, size_t cap) {
    WCHAR file[MAX_PATH]; file[0] = 0;
    to_w(suggested ? suggested : "", file, MAX_PATH);
    OPENFILENAMEW ofn; ZeroMemory(&ofn, sizeof ofn);
    ofn.lStructSize = sizeof ofn;
    ofn.hwndOwner = hwnd_of(owner);
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (!GetSaveFileNameW(&ofn)) return 0;
    to_u8(file, out, cap);
    return 1;
}

void oc_plat_file_saved(const char *path) { (void)path; }   /* the picker already chose where */

int oc_plat_view_certificate(SDL_Window *owner, const uint8_t *der, size_t len) {
    typedef BOOL (WINAPI *view_fn)(PCCRYPTUI_VIEWCERTIFICATE_STRUCTW, BOOL *);
    HMODULE ui = LoadLibraryW(L"cryptui.dll");
    view_fn view = ui ? (view_fn)(void *)GetProcAddress(ui, "CryptUIDlgViewCertificateW") : NULL;
    PCCERT_CONTEXT cc = view ? CertCreateCertificateContext(X509_ASN_ENCODING, der, (DWORD)len) : NULL;
    int shown = 0;
    if (cc) {
        CRYPTUI_VIEWCERTIFICATE_STRUCTW v;
        memset(&v, 0, sizeof v);
        v.dwSize = sizeof v;
        v.hwndParent = hwnd_of(owner);
        v.szTitle = L"Server certificate";
        v.pCertContext = cc;
        /* Without "Install Certificate": that would make a server's own
         * certificate a root this computer trusts for anything. */
        v.dwFlags = CRYPTUI_DISABLE_ADDTOSTORE;
        BOOL changed = FALSE;
        view(&v, &changed);
        CertFreeCertificateContext(cc);
        shown = 1;
    }
    if (ui) FreeLibrary(ui);
    return shown;
}

#define OC_RUN_KEY   L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"
#define OC_RUN_VALUE L"OpenChime"

int oc_plat_autostart_get(void) {
    HKEY k; int on = 0;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, OC_RUN_KEY, 0, KEY_READ, &k) == ERROR_SUCCESS) {
        on = RegQueryValueExW(k, OC_RUN_VALUE, NULL, NULL, NULL, NULL) == ERROR_SUCCESS;
        RegCloseKey(k);
    }
    return on;
}

void oc_plat_autostart_set(int on) {
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, OC_RUN_KEY, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS)
        return;
    if (on) {
        WCHAR exe[MAX_PATH], q[MAX_PATH + 4];
        if (GetModuleFileNameW(NULL, exe, MAX_PATH)) {
            _snwprintf(q, MAX_PATH + 4, L"\"%ls\"", exe);
            RegSetValueExW(k, OC_RUN_VALUE, 0, REG_SZ, (const BYTE *)q, (DWORD)((wcslen(q) + 1) * sizeof(WCHAR)));
        }
    } else RegDeleteValueW(k, OC_RUN_VALUE);
    RegCloseKey(k);
}

/* Under HKCU rather than HKCR: no elevation, and it follows the person. */
void oc_plat_url_scheme_register(const char *scheme) {
    WCHAR exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
    WCHAR cmd[MAX_PATH + 16], key[160], ws[64];
    to_w(scheme, ws, 64);
    _snwprintf(cmd, MAX_PATH + 16, L"\"%ls\" \"%%1\"", exe);
    _snwprintf(key, 160, L"Software\\Classes\\%ls", ws);
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) != ERROR_SUCCESS) return;
    RegSetValueExW(k, NULL, 0, REG_SZ, (const BYTE *)L"URL:OpenChime", 28);
    RegSetValueExW(k, L"URL Protocol", 0, REG_SZ, (const BYTE *)L"", 2);
    RegCloseKey(k);
    _snwprintf(key, 160, L"Software\\Classes\\%ls\\shell\\open\\command", ws);
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL) == ERROR_SUCCESS) {
        RegSetValueExW(k, NULL, 0, REG_SZ, (const BYTE *)cmd, (DWORD)((wcslen(cmd) + 1) * sizeof(WCHAR)));
        RegCloseKey(k);
    }
}

void oc_plat_sound(const char *name) {
    if (!name || !name[0]) return;
    WCHAR w[96]; to_w(name, w, 96);
    /* An ALIAS, a registered system event, so a user who has silenced
     * notification sounds is silenced here too. ASYNC so it never blocks the
     * tick; NODEFAULT so an unknown alias is silence rather than the ding. */
    PlaySoundW(w, NULL, SND_ASYNC | SND_ALIAS | SND_NODEFAULT);
}

uint32_t oc_plat_idle_ms(void) {
    LASTINPUTINFO li = { sizeof li, 0 };
    if (!GetLastInputInfo(&li)) return 0;
    return (uint32_t)(GetTickCount() - li.dwTime);
}

/* ---- the clipboard --------------------------------------------------------- */

char *oc_plat_clipboard_files(void) {
    if (!IsClipboardFormatAvailable(CF_HDROP) || !OpenClipboard(g_main)) return NULL;
    char *out = NULL;
    HDROP drop = (HDROP)GetClipboardData(CF_HDROP);
    UINT nf = drop ? DragQueryFileW(drop, 0xFFFFFFFF, NULL, 0) : 0;
    if (nf) {
        size_t cap = (size_t)nf * 1024 + 2, o = 0;
        out = malloc(cap);
        if (out) {
            for (UINT i = 0; i < nf; i++) {
                WCHAR wf[MAX_PATH];
                if (!DragQueryFileW(drop, i, wf, MAX_PATH)) continue;
                char p[1024];
                if (!to_u8(wf, p, sizeof p)) continue;
                size_t l = strlen(p);
                if (o + l + 2 > cap) break;
                memcpy(out + o, p, l + 1); o += l + 1;
            }
            out[o] = 0;
            if (!o) { free(out); out = NULL; }
        }
    }
    CloseClipboard();
    return out;
}

static IWICImagingFactory *wic(void) {
    if (!g_wic &&
        FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IWICImagingFactory, (void **)&g_wic)))
        g_wic = NULL;
    return g_wic;
}

/* A clipboard DIB as a PNG file in memory, through WIC: the DIB becomes a BMP
 * file (a header in front of it), which WIC decodes and re-encodes. */
static uint8_t *dib_to_png(const uint8_t *dib, size_t dlen, size_t *out_len) {
    *out_len = 0;
    if (!dib || dlen < sizeof(BITMAPINFOHEADER) || !wic()) return NULL;
    const BITMAPINFOHEADER *bi = (const BITMAPINFOHEADER *)dib;
    if (bi->biSize < sizeof(BITMAPINFOHEADER) || bi->biSize > dlen) return NULL;
    size_t colors = bi->biClrUsed ? bi->biClrUsed : (bi->biBitCount <= 8 ? (1u << bi->biBitCount) : 0);
    size_t masks = (bi->biSize == sizeof(BITMAPINFOHEADER) && bi->biCompression == BI_BITFIELDS) ? 12 : 0;
    size_t off = sizeof(BITMAPFILEHEADER) + bi->biSize + masks + colors * sizeof(RGBQUAD);
    size_t blen = sizeof(BITMAPFILEHEADER) + dlen;
    if (off > blen) return NULL;
    uint8_t *bmp = malloc(blen);
    if (!bmp) return NULL;
    BITMAPFILEHEADER fh = { 0x4D42, (DWORD)blen, 0, 0, (DWORD)off };
    memcpy(bmp, &fh, sizeof fh);
    memcpy(bmp + sizeof fh, dib, dlen);
    uint8_t *png = NULL;
    IWICStream *in = NULL; IWICBitmapDecoder *dec = NULL; IWICBitmapFrameDecode *frame = NULL;
    IStream *out = NULL; IWICBitmapEncoder *enc = NULL; IWICBitmapFrameEncode *fe = NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(g_wic, &in)) &&
        SUCCEEDED(IWICStream_InitializeFromMemory(in, bmp, (DWORD)blen)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(g_wic, (IStream *)in, NULL,
                                                             WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) &&
        SUCCEEDED(CreateStreamOnHGlobal(NULL, TRUE, &out)) &&
        SUCCEEDED(IWICImagingFactory_CreateEncoder(g_wic, &GUID_ContainerFormatPng, NULL, &enc)) &&
        SUCCEEDED(IWICBitmapEncoder_Initialize(enc, out, WICBitmapEncoderNoCache)) &&
        SUCCEEDED(IWICBitmapEncoder_CreateNewFrame(enc, &fe, NULL)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Initialize(fe, NULL)) &&
        SUCCEEDED(IWICBitmapFrameEncode_WriteSource(fe, (IWICBitmapSource *)frame, NULL)) &&
        SUCCEEDED(IWICBitmapFrameEncode_Commit(fe)) &&
        SUCCEEDED(IWICBitmapEncoder_Commit(enc))) {
        STATSTG st; HGLOBAL hg = NULL;
        if (SUCCEEDED(IStream_Stat(out, &st, STATFLAG_NONAME)) && st.cbSize.QuadPart &&
            st.cbSize.QuadPart <= 64u * 1024u * 1024u && SUCCEEDED(GetHGlobalFromStream(out, &hg))) {
            const void *src = GlobalLock(hg);
            if (src && (png = malloc((size_t)st.cbSize.QuadPart)) != NULL) {
                memcpy(png, src, (size_t)st.cbSize.QuadPart);
                *out_len = (size_t)st.cbSize.QuadPart;
            }
            if (src) GlobalUnlock(hg);
        }
    }
    if (fe)    IWICBitmapFrameEncode_Release(fe);
    if (enc)   IWICBitmapEncoder_Release(enc);
    if (out)   IStream_Release(out);
    if (frame) IWICBitmapFrameDecode_Release(frame);
    if (dec)   IWICBitmapDecoder_Release(dec);
    if (in)    IWICStream_Release(in);
    free(bmp);
    return png;
}

static uint8_t *clip_bytes(UINT fmt, size_t *len) {
    *len = 0;
    HANDLE h = fmt ? GetClipboardData(fmt) : NULL;
    if (!h) return NULL;
    SIZE_T n = GlobalSize(h);
    const void *src = n ? GlobalLock(h) : NULL;
    uint8_t *d = (src && n <= 64u * 1024u * 1024u) ? malloc(n) : NULL;
    if (d) { memcpy(d, src, n); *len = n; }
    if (src) GlobalUnlock(h);
    return d;
}

/* As the source put it there: a GIF's own bytes, else PNG, else the bitmap
 * turned into a PNG. Anything that also carries text is text, not an image. */
uint8_t *oc_plat_clipboard_image(size_t *len, char ext[8]) {
    *len = 0; ext[0] = 0;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(g_main)) return NULL;
    uint8_t *d = NULL; size_t n = 0;
    UINT gif = RegisterClipboardFormatW(L"GIF"), png = RegisterClipboardFormatW(L"PNG");
    if (IsClipboardFormatAvailable(gif) && (d = clip_bytes(gif, &n)) != NULL) {
        snprintf(ext, 8, "gif");
    } else if (IsClipboardFormatAvailable(png) && (d = clip_bytes(png, &n)) != NULL) {
        snprintf(ext, 8, "png");
    } else if (IsClipboardFormatAvailable(CF_DIB)) {
        size_t dl = 0;
        uint8_t *dib = clip_bytes(CF_DIB, &dl);
        d = dib_to_png(dib, dl, &n);
        free(dib);
        snprintf(ext, 8, "png");
    }
    CloseClipboard();
    if (!d || !n) { free(d); return NULL; }
    *len = n;
    return d;
}

/* ---- images ---------------------------------------------------------------- */

uint8_t *oc_plat_image_decode(const uint8_t *data, size_t len, int *w, int *h) {
    if (!data || !len || !wic()) return NULL;
    UINT iw = 0, ih = 0;
    uint8_t *px = NULL;
    IWICStream *stream = NULL; IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL; IWICFormatConverter *conv = NULL;
    if (SUCCEEDED(IWICImagingFactory_CreateStream(g_wic, &stream)) &&
        SUCCEEDED(IWICStream_InitializeFromMemory(stream, (BYTE *)data, (DWORD)len)) &&
        SUCCEEDED(IWICImagingFactory_CreateDecoderFromStream(g_wic, (IStream *)stream, NULL,
                                                             WICDecodeMetadataCacheOnLoad, &dec)) &&
        SUCCEEDED(IWICBitmapDecoder_GetFrame(dec, 0, &frame)) &&
        SUCCEEDED(IWICImagingFactory_CreateFormatConverter(g_wic, &conv)) &&
        SUCCEEDED(IWICFormatConverter_Initialize(conv, (IWICBitmapSource *)frame,
                                                 &GUID_WICPixelFormat32bppPBGRA,
                                                 WICBitmapDitherTypeNone, NULL, 0.0,
                                                 WICBitmapPaletteTypeMedianCut))) {
        IWICBitmapSource_GetSize((IWICBitmapSource *)conv, &iw, &ih);
        if (iw && ih && (uint64_t)iw * ih <= 4096ull * 4096ull) {
            UINT stride = iw * 4;
            px = malloc((size_t)stride * ih);
            if (px) {
                WICRect all = { 0, 0, (INT)iw, (INT)ih };
                if (FAILED(IWICBitmapSource_CopyPixels((IWICBitmapSource *)conv, &all, stride, stride * ih, px))) {
                    free(px); px = NULL;
                }
            }
        }
    }
    if (conv)   IWICFormatConverter_Release(conv);
    if (frame)  IWICBitmapFrameDecode_Release(frame);
    if (dec)    IWICBitmapDecoder_Release(dec);
    if (stream) IWICStream_Release(stream);
    if (px) { *w = (int)iw; *h = (int)ih; }
    return px;
}

/* ---- text ------------------------------------------------------------------ */

st_ctx *oc_plat_text_create(const st_sink *sink) { return st_dwrite_create(sink); }

void oc_plat_quit(void) {
    oc_plat_tray_done();
    if (g_taskbar) { ITaskbarList3_Release(g_taskbar); g_taskbar = NULL; }
    if (g_wic) { IWICImagingFactory_Release(g_wic); g_wic = NULL; }
}

const char *oc_plat_name(void) { return "win32"; }
