/* The Windows screen and window backend: Windows.Graphics.Capture
 * (docs/VIDEO-MESSAGES.md §3.2, ARCH-110).
 *
 * WinRT from C. The handful of interfaces this needs are declared here rather
 * than taken from headers -- mingw's declare the capture session and nothing
 * else of the API -- and every entry point is found at run time, so the client
 * starts on a Windows without the API and simply lists no screens there.
 *
 * One worker thread owns every WinRT and Direct3D object, in the multithreaded
 * apartment: it opens the capture, polls the frame pool, copies each frame
 * through a staging texture and converts it to I420, fitted into the one size
 * fixed when the capture opened. The system reports a frame only when something
 * changed; the capture front end repeats the last one at the frame rate
 * (capture.c). A window that closes ends the capture with OC_CAP_GONE.
 *
 * The system draws its own border around what is captured, and it is left on:
 * it is how a recording shows what it is recording (REQ-166). A window that has
 * excluded itself from capture (SetWindowDisplayAffinity) is left out of a
 * screen, which is how the recording bar stays out of the picture. */
#ifdef _WIN32
#define COBJMACROS
#define CINTERFACE
#define WIN32_LEAN_AND_MEAN
#include "oc_capture.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WGC_FAIL(what, hr) oc_capture_set_detail("%s: 0x%08lX", what, (unsigned long)(hr))

/* ---- the WinRT surface this uses ---------------------------------------------------- */

typedef void *hstr;                                  /* HSTRING */
typedef struct { void *r1; char r2[32]; } hstr_header;   /* HSTRING_HEADER, with room to spare */
typedef struct { int32_t w, h; } size32;             /* Windows.Graphics.SizeInt32 */

static const GUID IID_ItemInterop = {0x3628e81b,0x3cac,0x4c60,{0xb7,0xf4,0x23,0xce,0x0e,0x0c,0x33,0x56}};
static const GUID IID_Item        = {0x79c3f95b,0x31f7,0x4ec2,{0xa4,0x64,0x63,0x2e,0xf5,0xd3,0x07,0x60}};
static const GUID IID_PoolStatics2 = {0x589b103f,0x6bbc,0x5df5,{0xa9,0x91,0x02,0xe2,0x8b,0x3b,0x66,0xd5}};
static const GUID IID_SurfAccess  = {0xa9b3d012,0x3df2,0x4ee3,{0xb8,0xd1,0x86,0x95,0xf4,0x57,0xd3,0xc1}};
static const GUID IID_D3DDevice   = {0xa37624ab,0x8d5f,0x4650,{0x9d,0x3e,0x9e,0xae,0x3d,0x9b,0xc6,0x70}};
static const GUID IID_SessStatics = {0x2224a540,0x5974,0x49aa,{0xb2,0x32,0x08,0x82,0x53,0x6f,0x4c,0xb5}};
static const GUID IID_Closable    = {0x30d5a829,0x7fa4,0x4026,{0x83,0xbb,0xd7,0x5b,0xae,0x4e,0xa9,0x9e}};
static const GUID IID_Tex2D       = {0x6f15aaf2,0xd208,0x4e89,{0x9a,0xb4,0x48,0x95,0x35,0xd3,0x4f,0x9c}};
static const GUID IID_DXGIDevice  = {0x54ec77fa,0x1377,0x44e6,{0x8c,0x32,0x88,0xfd,0x5f,0x44,0xc8,0x4c}};

#define DXGI_BGRA 87                                 /* DirectXPixelFormat.B8G8R8A8UIntNormalized */

/* IUnknown and IInspectable, then the interface's own methods. */
#define UNKNOWN(T) \
    HRESULT (STDMETHODCALLTYPE *QueryInterface)(T *, REFIID, void **); \
    ULONG   (STDMETHODCALLTYPE *AddRef)(T *); \
    ULONG   (STDMETHODCALLTYPE *Release)(T *);
#define INSPECTABLE(T) UNKNOWN(T) \
    HRESULT (STDMETHODCALLTYPE *GetIids)(T *, ULONG *, IID **); \
    HRESULT (STDMETHODCALLTYPE *GetRuntimeClassName)(T *, hstr *); \
    HRESULT (STDMETHODCALLTYPE *GetTrustLevel)(T *, int *);
#define IFACE(T, ...) typedef struct T T; typedef struct { __VA_ARGS__ } T##_vt; struct T { const T##_vt *v; };

IFACE(unk, UNKNOWN(unk))
IFACE(item_interop, UNKNOWN(item_interop)
      HRESULT (STDMETHODCALLTYPE *CreateForWindow)(item_interop *, HWND, REFIID, void **);
      HRESULT (STDMETHODCALLTYPE *CreateForMonitor)(item_interop *, HMONITOR, REFIID, void **);)
IFACE(cap_item, INSPECTABLE(cap_item)
      HRESULT (STDMETHODCALLTYPE *get_DisplayName)(cap_item *, hstr *);
      HRESULT (STDMETHODCALLTYPE *get_Size)(cap_item *, size32 *);)
IFACE(cap_frame, INSPECTABLE(cap_frame)
      HRESULT (STDMETHODCALLTYPE *get_Surface)(cap_frame *, unk **);
      HRESULT (STDMETHODCALLTYPE *get_SystemRelativeTime)(cap_frame *, int64_t *);
      HRESULT (STDMETHODCALLTYPE *get_ContentSize)(cap_frame *, size32 *);)
IFACE(cap_session, INSPECTABLE(cap_session)
      HRESULT (STDMETHODCALLTYPE *StartCapture)(cap_session *);)
IFACE(sess_statics, INSPECTABLE(sess_statics)
      HRESULT (STDMETHODCALLTYPE *IsSupported)(sess_statics *, unsigned char *);)
IFACE(frame_pool, INSPECTABLE(frame_pool)
      HRESULT (STDMETHODCALLTYPE *Recreate)(frame_pool *, unk *, int, int32_t, size32);
      HRESULT (STDMETHODCALLTYPE *TryGetNextFrame)(frame_pool *, cap_frame **);
      HRESULT (STDMETHODCALLTYPE *add_FrameArrived)(frame_pool *, void *, int64_t *);
      HRESULT (STDMETHODCALLTYPE *remove_FrameArrived)(frame_pool *, int64_t);
      HRESULT (STDMETHODCALLTYPE *CreateCaptureSession)(frame_pool *, cap_item *, cap_session **);)
IFACE(pool_statics2, INSPECTABLE(pool_statics2)
      HRESULT (STDMETHODCALLTYPE *CreateFreeThreaded)(pool_statics2 *, unk *, int, int32_t, size32, frame_pool **);)
IFACE(surf_access, UNKNOWN(surf_access)
      HRESULT (STDMETHODCALLTYPE *GetInterface)(surf_access *, REFIID, void **);)
IFACE(closable, INSPECTABLE(closable)
      HRESULT (STDMETHODCALLTYPE *Close)(closable *);)

#define REL(p) do { if (p) { ((unk *)(p))->v->Release((unk *)(p)); (p) = NULL; } } while (0)

/* Close a WinRT object that holds resources (a frame, a session, a pool)
 * before letting it go, as the API asks. */
static void close_and_release(void *p) {
    if (!p) return;
    closable *c = NULL;
    if (SUCCEEDED(((unk *)p)->v->QueryInterface((unk *)p, &IID_Closable, (void **)&c)) && c) {
        c->v->Close(c);
        c->v->Release(c);
    }
    ((unk *)p)->v->Release((unk *)p);
}

/* ---- the entry points, found at run time ---------------------------------------------- */

typedef HRESULT (WINAPI *pfn_RoInitialize)(int);
typedef void    (WINAPI *pfn_RoUninitialize)(void);
typedef HRESULT (WINAPI *pfn_RoGetActivationFactory)(hstr, REFIID, void **);
typedef HRESULT (WINAPI *pfn_WindowsCreateStringReference)(const WCHAR *, UINT32, hstr_header *, hstr *);
typedef HRESULT (WINAPI *pfn_D3D11CreateDevice)(IDXGIAdapter *, D3D_DRIVER_TYPE, HMODULE, UINT,
                                                const D3D_FEATURE_LEVEL *, UINT, UINT, ID3D11Device **,
                                                D3D_FEATURE_LEVEL *, ID3D11DeviceContext **);
typedef HRESULT (WINAPI *pfn_FromDXGI)(IDXGIDevice *, unk **);

static struct {
    int loaded, ok;
    pfn_RoInitialize ro_init;
    pfn_RoUninitialize ro_uninit;
    pfn_RoGetActivationFactory factory;
    pfn_WindowsCreateStringReference str;
    pfn_D3D11CreateDevice d3d_create;
    pfn_FromDXGI from_dxgi;
} api;

static int load_api(void) {
    if (api.loaded) return api.ok;
    api.loaded = 1;
    HMODULE cb = LoadLibraryW(L"combase.dll"), d3 = LoadLibraryW(L"d3d11.dll");
    if (!cb || !d3) return 0;
    api.ro_init    = (pfn_RoInitialize)(void *)GetProcAddress(cb, "RoInitialize");
    api.ro_uninit  = (pfn_RoUninitialize)(void *)GetProcAddress(cb, "RoUninitialize");
    api.factory    = (pfn_RoGetActivationFactory)(void *)GetProcAddress(cb, "RoGetActivationFactory");
    api.str        = (pfn_WindowsCreateStringReference)(void *)GetProcAddress(cb, "WindowsCreateStringReference");
    api.d3d_create = (pfn_D3D11CreateDevice)(void *)GetProcAddress(d3, "D3D11CreateDevice");
    api.from_dxgi  = (pfn_FromDXGI)(void *)GetProcAddress(d3, "CreateDirect3D11DeviceFromDXGIDevice");
    api.ok = api.ro_init && api.ro_uninit && api.factory && api.str && api.d3d_create && api.from_dxgi;
    return api.ok;
}

static HRESULT factory_for(const WCHAR *cls, REFIID iid, void **out) {
    hstr_header hh;
    hstr s = NULL;
    HRESULT hr = api.str(cls, (UINT32)wcslen(cls), &hh, &s);
    return FAILED(hr) ? hr : api.factory(s, iid, out);
}

/* Whether this Windows can capture at all. Asked once, on a thread of its own:
 * WinRT wants an apartment, and the caller's may be the UI's. */
static DWORD WINAPI supported_thread(LPVOID arg) {
    int *ok = arg;
    HRESULT init = api.ro_init(1 /* RO_INIT_MULTITHREADED */);
    sess_statics *ss = NULL;
    unsigned char yes = 0;
    if (SUCCEEDED(factory_for(L"Windows.Graphics.Capture.GraphicsCaptureSession", &IID_SessStatics, (void **)&ss)) &&
        SUCCEEDED(ss->v->IsSupported(ss, &yes)))
        *ok = yes != 0;
    REL(ss);
    if (SUCCEEDED(init)) api.ro_uninit();
    return 0;
}

static int supported(void) {
    static int asked, ok;
    if (asked) return ok;
    asked = 1;
    if (!load_api()) return 0;
    HANDLE t = CreateThread(NULL, 0, supported_thread, &ok, 0, NULL);
    if (!t) return 0;
    WaitForSingleObject(t, 5000);
    CloseHandle(t);
    return ok;
}

/* ---- what can be captured ------------------------------------------------------------ */

typedef struct { oc_capture_device *out; int cap, n; } list_ctx;

static BOOL CALLBACK add_monitor(HMONITOR m, HDC dc, LPRECT r, LPARAM lp) {
    (void)m; (void)dc;
    list_ctx *l = (list_ctx *)lp;
    if (l->n >= l->cap) return FALSE;
    oc_capture_device *d = &l->out[l->n];
    memset(d, 0, sizeof *d);
    snprintf(d->id, sizeof d->id, "screen:%d", l->n);
    snprintf(d->name, sizeof d->name, "Screen %d (%ld\xC3\x97%ld)", l->n + 1,
             (long)(r->right - r->left), (long)(r->bottom - r->top));
    d->kind = OC_SOURCE_SCREEN;
    l->n++;
    return TRUE;
}

/* A window a person would pick: shown, titled, top-level, not a tool window
 * (which is what keeps the recording bar off the list) and not hidden by the
 * shell (cloaked). OpenChime's own window is listed: showing someone the app is
 * a recording worth making. */
static int pickable(HWND h) {
    if (!IsWindowVisible(h) || GetWindow(h, GW_OWNER) || GetWindowTextLengthW(h) == 0) return 0;
    if (GetWindowLongW(h, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) return 0;
    typedef HRESULT (WINAPI *pfn_attr)(HWND, DWORD, PVOID, DWORD);
    static pfn_attr get_attr;
    static int looked;
    if (!looked) {
        looked = 1;
        HMODULE dwm = LoadLibraryW(L"dwmapi.dll");
        if (dwm) get_attr = (pfn_attr)(void *)GetProcAddress(dwm, "DwmGetWindowAttribute");
    }
    DWORD cloaked = 0;
    if (get_attr && SUCCEEDED(get_attr(h, 14 /* DWMWA_CLOAKED */, &cloaked, sizeof cloaked)) && cloaked) return 0;
    return 1;
}

static BOOL CALLBACK add_window(HWND h, LPARAM lp) {
    list_ctx *l = (list_ctx *)lp;
    if (l->n >= l->cap) return FALSE;
    if (!pickable(h)) return TRUE;
    oc_capture_device *d = &l->out[l->n];
    memset(d, 0, sizeof *d);
    snprintf(d->id, sizeof d->id, "window:%llx", (unsigned long long)(uintptr_t)h);
    WCHAR title[128];
    GetWindowTextW(h, title, 128);
    WideCharToMultiByte(CP_UTF8, 0, title, -1, d->name, (int)sizeof d->name, NULL, NULL);
    d->kind = OC_SOURCE_WINDOW;
    l->n++;
    return TRUE;
}

static int wgc_list(oc_capture_device *out, int cap) {
    if (cap <= 0 || !supported()) return 0;
    list_ctx l = { out, cap, 0 };
    EnumDisplayMonitors(NULL, NULL, add_monitor, (LPARAM)&l);
    EnumWindows(add_window, (LPARAM)&l);
    return l.n;
}

typedef struct { int want, n; HMONITOR found; } mon_ctx;
static BOOL CALLBACK nth_monitor(HMONITOR m, HDC dc, LPRECT r, LPARAM lp) {
    (void)dc; (void)r;
    mon_ctx *c = (mon_ctx *)lp;
    if (c->n++ == c->want) { c->found = m; return FALSE; }
    return TRUE;
}

/* ---- a capture ------------------------------------------------------------------------ */

typedef struct {
    HMONITOR          mon;
    HWND              win;
    int               max_w, max_h;
    HANDLE            thread;
    CRITICAL_SECTION  mu;
    CONDITION_VARIABLE cv;
    int               opened;            /* the worker finished opening: 1 ok, <0 an OC_CAP_* */
    int               running, quit, gone, failed;
    oc_frame          ready, out;         /* the newest frame; the one handed out */
    uint64_t          ready_seq, out_seq;
} wgc;

static void set_opened(wgc *c, int v) {
    EnterCriticalSection(&c->mu);
    c->opened = v;
    WakeAllConditionVariable(&c->cv);
    LeaveCriticalSection(&c->mu);
}

/* Convert one captured texture (BGRA, `cw`×`ch` of it in use) into the fixed
 * output size. `tmp` is resized to the content as it changes. */
static void deliver(wgc *c, const uint8_t *px, int pitch, int cw, int ch, oc_frame *tmp) {
    cw &= ~1; ch &= ~1;
    if (cw < 2 || ch < 2) return;
    if (tmp->width != cw || tmp->height != ch) {
        oc_frame_free(tmp);
        if (oc_frame_alloc(tmp, cw, ch) != 0) return;
    }
    oc_i420_from_bgra(tmp, px, pitch);
    EnterCriticalSection(&c->mu);
    oc_i420_fit(tmp, &c->ready);
    c->ready.pts_us = oc_media_clock_us();
    c->ready_seq++;
    WakeAllConditionVariable(&c->cv);
    LeaveCriticalSection(&c->mu);
}

static DWORD WINAPI wgc_worker(LPVOID arg) {
    wgc *c = arg;
    HRESULT init = api.ro_init(1);
    item_interop *interop = NULL;
    cap_item *item = NULL;
    ID3D11Device *dev = NULL;
    ID3D11DeviceContext *ctx = NULL;
    IDXGIDevice *dxgi = NULL;
    unk *rtdev = NULL;
    pool_statics2 *pools = NULL;
    frame_pool *pool = NULL;
    cap_session *session = NULL;
    ID3D11Texture2D *staging = NULL;
    D3D11_TEXTURE2D_DESC sd;
    oc_frame tmp = {0};
    size32 pool_size = {0, 0};
    HRESULT hr;

    if (FAILED(hr = factory_for(L"Windows.Graphics.Capture.GraphicsCaptureItem", &IID_ItemInterop, (void **)&interop))) {
        WGC_FAIL("capture item factory", hr); set_opened(c, OC_CAP_NODEVICE); goto out;
    }
    hr = c->win ? interop->v->CreateForWindow(interop, c->win, &IID_Item, (void **)&item)
                : interop->v->CreateForMonitor(interop, c->mon, &IID_Item, (void **)&item);
    if (FAILED(hr)) {
        WGC_FAIL(c->win ? "CreateForWindow" : "CreateForMonitor", hr);
        set_opened(c, hr == E_ACCESSDENIED ? OC_CAP_DENIED : c->win && !IsWindow(c->win) ? OC_CAP_GONE : OC_CAP_FAILED);
        goto out;
    }
    if (FAILED(hr = item->v->get_Size(item, &pool_size)) || pool_size.w < 2 || pool_size.h < 2) {
        WGC_FAIL("item size", hr); set_opened(c, OC_CAP_FAILED); goto out;
    }
    {   /* The one size this capture has: the source's shape fitted inside the
         * maximum, even. */
        int w = c->max_w, h = (int)((int64_t)c->max_w * pool_size.h / pool_size.w);
        if (h > c->max_h) { h = c->max_h; w = (int)((int64_t)c->max_h * pool_size.w / pool_size.h); }
        if (w > pool_size.w || h > pool_size.h) { w = pool_size.w; h = pool_size.h; }   /* never enlarged */
        if (oc_frame_alloc(&c->ready, w & ~1, h & ~1) != 0 || oc_frame_alloc(&c->out, w & ~1, h & ~1) != 0) {
            set_opened(c, OC_CAP_FAILED); goto out;
        }
        oc_i420_fill(&c->ready, 16, 128, 128);
    }
    hr = api.d3d_create(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                        D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr))   /* no usable GPU: the software rasteriser does the copies */
        hr = api.d3d_create(NULL, D3D_DRIVER_TYPE_WARP, NULL, D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                            D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr)) { WGC_FAIL("D3D11CreateDevice", hr); set_opened(c, OC_CAP_FAILED); goto out; }
    if (FAILED(hr = ID3D11Device_QueryInterface(dev, &IID_DXGIDevice, (void **)&dxgi)) ||
        FAILED(hr = api.from_dxgi(dxgi, &rtdev))) {
        WGC_FAIL("Direct3D device for capture", hr); set_opened(c, OC_CAP_FAILED); goto out;
    }
    {   /* rtdev is IInspectable; the pool wants IDirect3DDevice. */
        unk *d3d = NULL;
        if (FAILED(hr = rtdev->v->QueryInterface(rtdev, &IID_D3DDevice, (void **)&d3d))) {
            WGC_FAIL("IDirect3DDevice", hr); set_opened(c, OC_CAP_FAILED); goto out;
        }
        REL(rtdev);
        rtdev = d3d;
    }
    if (FAILED(hr = factory_for(L"Windows.Graphics.Capture.Direct3D11CaptureFramePool", &IID_PoolStatics2, (void **)&pools)) ||
        FAILED(hr = pools->v->CreateFreeThreaded(pools, rtdev, DXGI_BGRA, 2, pool_size, &pool)) ||
        FAILED(hr = pool->v->CreateCaptureSession(pool, item, &session)) ||
        FAILED(hr = session->v->StartCapture(session))) {
        WGC_FAIL("start capture", hr); set_opened(c, hr == E_ACCESSDENIED ? OC_CAP_DENIED : OC_CAP_FAILED); goto out;
    }
    set_opened(c, 1);

    for (;;) {
        EnterCriticalSection(&c->mu);
        int quit = c->quit;
        LeaveCriticalSection(&c->mu);
        if (quit) break;
        if (c->win && !IsWindow(c->win)) {
            oc_capture_set_detail("the window was closed");
            EnterCriticalSection(&c->mu); c->gone = 1; WakeAllConditionVariable(&c->cv); LeaveCriticalSection(&c->mu);
            break;
        }
        cap_frame *fr = NULL;
        if (FAILED(pool->v->TryGetNextFrame(pool, &fr)) || !fr) { Sleep(4); continue; }
        size32 cs = {0, 0};
        fr->v->get_ContentSize(fr, &cs);
        if (cs.w != pool_size.w || cs.h != pool_size.h) {
            /* The window changed size: the pool follows, and this frame -- laid
             * out for the old size -- is skipped. */
            pool_size = cs;
            if (cs.w > 0 && cs.h > 0) pool->v->Recreate(pool, rtdev, DXGI_BGRA, 2, cs);
            close_and_release(fr);
            continue;
        }
        unk *surf = NULL;
        surf_access *acc = NULL;
        ID3D11Texture2D *tex = NULL;
        if (SUCCEEDED(fr->v->get_Surface(fr, &surf)) &&
            SUCCEEDED(surf->v->QueryInterface(surf, &IID_SurfAccess, (void **)&acc)) &&
            SUCCEEDED(acc->v->GetInterface(acc, &IID_Tex2D, (void **)&tex))) {
            D3D11_TEXTURE2D_DESC td;
            ID3D11Texture2D_GetDesc(tex, &td);
            if (staging && (sd.Width != td.Width || sd.Height != td.Height)) { ID3D11Texture2D_Release(staging); staging = NULL; }
            if (!staging) {
                sd = td;
                sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0;
                sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
                sd.MipLevels = 1; sd.ArraySize = 1;
                if (FAILED(ID3D11Device_CreateTexture2D(dev, &sd, NULL, &staging))) staging = NULL;
            }
            if (staging) {
                ID3D11DeviceContext_CopyResource(ctx, (ID3D11Resource *)staging, (ID3D11Resource *)tex);
                D3D11_MAPPED_SUBRESOURCE mp;
                if (SUCCEEDED(ID3D11DeviceContext_Map(ctx, (ID3D11Resource *)staging, 0, D3D11_MAP_READ, 0, &mp))) {
                    int cw = cs.w < (int)td.Width ? cs.w : (int)td.Width;
                    int ch = cs.h < (int)td.Height ? cs.h : (int)td.Height;
                    deliver(c, mp.pData, (int)mp.RowPitch, cw, ch, &tmp);
                    ID3D11DeviceContext_Unmap(ctx, (ID3D11Resource *)staging, 0);
                }
            }
            ID3D11Texture2D_Release(tex);
        }
        REL(acc);
        REL(surf);
        close_and_release(fr);
    }

out:
    if (staging) ID3D11Texture2D_Release(staging);
    close_and_release(session);
    close_and_release(pool);
    REL(pools);
    REL(rtdev);
    if (dxgi) IDXGIDevice_Release(dxgi);
    if (ctx) ID3D11DeviceContext_Release(ctx);
    if (dev) ID3D11Device_Release(dev);
    close_and_release(item);
    REL(interop);
    oc_frame_free(&tmp);
    if (SUCCEEDED(init)) api.ro_uninit();
    EnterCriticalSection(&c->mu);
    if (!c->gone && !c->quit) c->failed = 1;
    if (c->opened == 0) c->opened = OC_CAP_FAILED;
    WakeAllConditionVariable(&c->cv);
    LeaveCriticalSection(&c->mu);
    return 0;
}

static void wgc_close(void *impl);

static void *wgc_open(const char *id, int max_w, int max_h, int fps, int *err) {
    (void)fps;
    if (!supported()) { oc_capture_set_detail("Windows.Graphics.Capture is not available"); *err = OC_CAP_NODEVICE; return NULL; }
    wgc *c = calloc(1, sizeof *c);
    if (!c) { *err = OC_CAP_FAILED; return NULL; }
    InitializeCriticalSection(&c->mu);
    InitializeConditionVariable(&c->cv);
    c->max_w = max_w > 0 ? max_w : 1920;
    c->max_h = max_h > 0 ? max_h : 1080;
    if (id && !strncmp(id, "window:", 7)) {
        c->win = (HWND)(uintptr_t)strtoull(id + 7, NULL, 16);
        if (!IsWindow(c->win)) { oc_capture_set_detail("the window is gone"); wgc_close(c); *err = OC_CAP_GONE; return NULL; }
    } else {
        mon_ctx m = { id && !strncmp(id, "screen:", 7) ? atoi(id + 7) : 0, 0, NULL };
        EnumDisplayMonitors(NULL, NULL, nth_monitor, (LPARAM)&m);
        c->mon = m.found ? m.found : MonitorFromPoint((POINT){0, 0}, MONITOR_DEFAULTTOPRIMARY);
    }
    c->thread = CreateThread(NULL, 0, wgc_worker, c, 0, NULL);
    if (!c->thread) { wgc_close(c); *err = OC_CAP_FAILED; return NULL; }
    EnterCriticalSection(&c->mu);
    while (c->opened == 0) SleepConditionVariableCS(&c->cv, &c->mu, INFINITE);
    int opened = c->opened;
    LeaveCriticalSection(&c->mu);
    if (opened != 1) { wgc_close(c); *err = opened; return NULL; }
    *err = OC_CAP_OK;
    return c;
}

static int wgc_start(void *impl) {
    wgc *c = impl;
    EnterCriticalSection(&c->mu); c->running = 1; LeaveCriticalSection(&c->mu);
    return OC_CAP_OK;
}

/* A new frame when one has come since the last call: 1; 0 on timeout. */
static int wgc_next(void *impl, oc_frame *f, int timeout_ms) {
    wgc *c = impl;
    EnterCriticalSection(&c->mu);
    ULONGLONG end = GetTickCount64() + (ULONGLONG)(timeout_ms > 0 ? timeout_ms : 0);
    while (c->ready_seq == c->out_seq && !c->gone && !c->failed && c->running) {
        ULONGLONG now = GetTickCount64();
        if (now >= end) break;
        SleepConditionVariableCS(&c->cv, &c->mu, (DWORD)(end - now));
    }
    int rc;
    if (c->gone) rc = OC_CAP_GONE;
    else if (c->failed || !c->running) rc = OC_CAP_FAILED;
    else if (c->ready_seq == c->out_seq) rc = 0;
    else {
        oc_frame_copy(&c->out, &c->ready);
        c->out_seq = c->ready_seq;
        *f = c->out;
        rc = 1;
    }
    LeaveCriticalSection(&c->mu);
    return rc;
}

static void wgc_stop(void *impl) {
    wgc *c = impl;
    EnterCriticalSection(&c->mu); c->running = 0; WakeAllConditionVariable(&c->cv); LeaveCriticalSection(&c->mu);
}

static void wgc_close(void *impl) {
    wgc *c = impl;
    if (!c) return;
    if (c->thread) {
        EnterCriticalSection(&c->mu); c->quit = 1; LeaveCriticalSection(&c->mu);
        WaitForSingleObject(c->thread, INFINITE);
        CloseHandle(c->thread);
    }
    oc_frame_free(&c->ready);
    oc_frame_free(&c->out);
    DeleteCriticalSection(&c->mu);
    free(c);
}

static const oc_capture_backend wgc_backend = { wgc_list, wgc_open, wgc_start, wgc_next, wgc_stop, wgc_close };

const oc_capture_backend *oc_capture_screen_platform(void) { return &wgc_backend; }

#endif /* _WIN32 */
