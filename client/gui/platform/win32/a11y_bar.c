/*
 * OpenChime Win32 — the bars' UI Automation provider (REQ-269, REQ-290).
 *
 * The recording bar and the sharing bar are small windows of their own, drawn
 * with the app's primitives, so the main window's provider (a11y.c) does not
 * reach them: its tree hangs off the main window's WM_GETOBJECT alone. Each bar
 * is served here instead, from what the app publishes for it (a11y.h,
 * oc_a11y_bar_publish), as a tree one level deep:
 *
 *   the window (its own automation id and name)
 *     ├── the bar's line          (Text)
 *     └── each of its buttons     (Button, invokable while enabled)
 *
 * The bar's window procedure is subclassed while it is published, as the main
 * window's is (plat_win32.c), and put back before the window goes. The entry
 * points are resolved at runtime, for the reasons a11y.c gives.
 */
#define COBJMACROS
/* Raised for this file only, as in a11y.c: everything later is resolved with
 * GetProcAddress, so nothing an older Windows lacks is imported. */
#undef  _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#undef  NTDDI_VERSION
#define NTDDI_VERSION 0x0A000000
#include <windows.h>
#include <uiautomation.h>
#include <oleauto.h>
#include <stdio.h>
#include <string.h>

#include "a11y.h"
#include "platform.h"

#ifndef __IRawElementProviderFragment_INTERFACE_DEFINED__
void oc_a11y_bar_publish(SDL_Window *w, const char *aid, const char *name, const char *line,
                         const oc_acc_item *buttons, int n) {
    (void)w; (void)aid; (void)name; (void)line; (void)buttons; (void)n;
}
void oc_a11y_bar_close(SDL_Window *w) { (void)w; }
#else

typedef LRESULT (WINAPI *fn_return_provider)(HWND, WPARAM, LPARAM, IRawElementProviderSimple *);
typedef HRESULT (WINAPI *fn_host_provider)(HWND, IRawElementProviderSimple **);
static fn_return_provider p_return_provider;
static fn_host_provider   p_host_provider;
static int                g_resolved;

/* ---- the published bars ----------------------------------------------------
 * Two at most: the recording bar and the sharing bar. Guarded because UIA calls
 * a provider from its own threads while the UI thread publishes. A slot's
 * generation moves each time it is opened, so an element a client still holds
 * from a bar since closed answers as gone rather than as the new bar's. */
enum { BAR_SLOTS = 2, BAR_BTNS = 2, BAR_TEXT = 200 };
typedef struct {
    HWND        hwnd;
    WNDPROC     prev;
    unsigned    gen;
    char        aid[OC_ACC_AID_MAX];
    char        name[96];
    char        line[BAR_TEXT];
    oc_acc_item btn[BAR_BTNS];
    int         n_btn;
} bar_slot;
static bar_slot         g_bar[BAR_SLOTS];
static CRITICAL_SECTION g_lock;
static int              g_lock_ready;

/* Elements: the window, the line, then the buttons. */
enum { BE_ROOT = -1, BE_LINE = 0, BE_BTN0 = 1 };

typedef struct {
    IRawElementProviderSimple       s;
    IRawElementProviderFragment     f;
    IRawElementProviderFragmentRoot r;
    IInvokeProvider                 iv;
    LONG     ref;
    int      slot;
    unsigned gen;
    int      idx;
} bar_el;

static bar_el *bel_new(int slot, unsigned gen, int idx);

/* Is the element's bar still the one it was made for? Called under the lock. */
static bar_slot *live(const bar_el *e) {
    bar_slot *b = &g_bar[e->slot];
    return (b->hwnd && b->gen == e->gen) ? b : NULL;
}
static int has_el(const bar_slot *b, int idx) {
    return idx == BE_ROOT || idx == BE_LINE || (idx >= BE_BTN0 && idx < BE_BTN0 + b->n_btn);
}

static BSTR bstr_utf8(const char *s) {
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return SysAllocString(L"");
    WCHAR *w = (WCHAR *)calloc((size_t)n, sizeof(WCHAR));
    if (!w) return SysAllocString(L"");
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    BSTR r = SysAllocString(w);
    free(w);
    return r;
}

/* ---- IUnknown, shared by the four interfaces -------------------------------- */

static HRESULT STDMETHODCALLTYPE s_QI(IRawElementProviderSimple *t, REFIID iid, void **out) {
    bar_el *e = CONTAINING_RECORD(t, bar_el, s);
    if (!out) return E_POINTER;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IRawElementProviderSimple)) *out = &e->s;
    else if (IsEqualIID(iid, &IID_IRawElementProviderFragment))                         *out = &e->f;
    else if (IsEqualIID(iid, &IID_IRawElementProviderFragmentRoot) && e->idx == BE_ROOT) *out = &e->r;
    else if (IsEqualIID(iid, &IID_IInvokeProvider) && e->idx >= BE_BTN0)                *out = &e->iv;
    else { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&e->ref);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE s_AddRef(IRawElementProviderSimple *t) {
    return (ULONG)InterlockedIncrement(&CONTAINING_RECORD(t, bar_el, s)->ref);
}
static ULONG STDMETHODCALLTYPE s_Release(IRawElementProviderSimple *t) {
    bar_el *e = CONTAINING_RECORD(t, bar_el, s);
    LONG n = InterlockedDecrement(&e->ref);
    if (n == 0) free(e);
    return (ULONG)n;
}

/* ---- IRawElementProviderSimple ---------------------------------------------- */

static HRESULT STDMETHODCALLTYPE s_GetOptions(IRawElementProviderSimple *t, enum ProviderOptions *o) {
    (void)t; *o = ProviderOptions_ServerSideProvider; return S_OK;
}
static HRESULT STDMETHODCALLTYPE s_GetPatternProvider(IRawElementProviderSimple *t, PATTERNID pid, IUnknown **out) {
    bar_el *e = CONTAINING_RECORD(t, bar_el, s);
    *out = NULL;
    if (pid == UIA_InvokePatternId && e->idx >= BE_BTN0) { *out = (IUnknown *)&e->iv; s_AddRef(&e->s); }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE s_GetPropertyValue(IRawElementProviderSimple *t, PROPERTYID pid, VARIANT *v) {
    bar_el *e = CONTAINING_RECORD(t, bar_el, s);
    VariantInit(v);
    EnterCriticalSection(&g_lock);
    bar_slot *b = live(e);
    if (!b || !has_el(b, e->idx)) { LeaveCriticalSection(&g_lock); return UIA_E_ELEMENTNOTAVAILABLE; }
    const oc_acc_item *btn = e->idx >= BE_BTN0 ? &b->btn[e->idx - BE_BTN0] : NULL;
    switch (pid) {
    case UIA_ControlTypePropertyId:
        /* The window's own type is the host's (Window); only the parts say theirs. */
        if (e->idx == BE_LINE) { v->vt = VT_I4; v->lVal = UIA_TextControlTypeId; }
        else if (btn)          { v->vt = VT_I4; v->lVal = UIA_ButtonControlTypeId; }
        break;
    case UIA_AutomationIdPropertyId: {
        char id[OC_ACC_AID_MAX + 8];
        if (e->idx == BE_ROOT)      snprintf(id, sizeof id, "%s", b->aid);
        else if (e->idx == BE_LINE) snprintf(id, sizeof id, "%s.text", b->aid);
        else                        snprintf(id, sizeof id, "%s", btn->aid);
        v->vt = VT_BSTR; v->bstrVal = bstr_utf8(id);
        break;
    }
    case UIA_NamePropertyId:
        v->vt = VT_BSTR;
        v->bstrVal = bstr_utf8(e->idx == BE_ROOT ? b->name : e->idx == BE_LINE ? b->line : btn->name);
        break;
    case UIA_IsEnabledPropertyId:
        v->vt = VT_BOOL; v->boolVal = (!btn || btn->invoke) ? VARIANT_TRUE : VARIANT_FALSE;
        break;
    case UIA_IsControlElementPropertyId:
    case UIA_IsContentElementPropertyId:
        v->vt = VT_BOOL; v->boolVal = VARIANT_TRUE;
        break;
    case UIA_IsKeyboardFocusablePropertyId:
        v->vt = VT_BOOL; v->boolVal = VARIANT_FALSE;   /* the bar takes clicks, not the keyboard */
        break;
    default: break;
    }
    LeaveCriticalSection(&g_lock);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE s_GetHostProvider(IRawElementProviderSimple *t, IRawElementProviderSimple **out) {
    bar_el *e = CONTAINING_RECORD(t, bar_el, s);
    *out = NULL;
    if (e->idx != BE_ROOT || !p_host_provider) return S_OK;
    EnterCriticalSection(&g_lock);
    bar_slot *b = live(e);
    HWND h = b ? b->hwnd : NULL;
    LeaveCriticalSection(&g_lock);
    return h ? p_host_provider(h, out) : S_OK;
}
static IRawElementProviderSimpleVtbl g_svt = {
    s_QI, s_AddRef, s_Release, s_GetOptions, s_GetPatternProvider, s_GetPropertyValue, s_GetHostProvider
};

/* ---- IRawElementProviderFragment -------------------------------------------- */

static HRESULT STDMETHODCALLTYPE f_QI(IRawElementProviderFragment *t, REFIID iid, void **o) {
    return s_QI(&CONTAINING_RECORD(t, bar_el, f)->s, iid, o);
}
static ULONG STDMETHODCALLTYPE f_AddRef(IRawElementProviderFragment *t)  { return s_AddRef(&CONTAINING_RECORD(t, bar_el, f)->s); }
static ULONG STDMETHODCALLTYPE f_Release(IRawElementProviderFragment *t) { return s_Release(&CONTAINING_RECORD(t, bar_el, f)->s); }

static HRESULT STDMETHODCALLTYPE f_Navigate(IRawElementProviderFragment *t, enum NavigateDirection dir,
                                            IRawElementProviderFragment **out) {
    bar_el *e = CONTAINING_RECORD(t, bar_el, f);
    *out = NULL;
    EnterCriticalSection(&g_lock);
    bar_slot *b = live(e);
    int last = b ? BE_BTN0 + b->n_btn - 1 : BE_LINE, to = -9;
    if (b) switch (dir) {
    case NavigateDirection_Parent:          if (e->idx != BE_ROOT) to = BE_ROOT; break;
    case NavigateDirection_FirstChild:      if (e->idx == BE_ROOT) to = BE_LINE; break;
    case NavigateDirection_LastChild:       if (e->idx == BE_ROOT) to = last; break;
    case NavigateDirection_NextSibling:     if (e->idx != BE_ROOT && e->idx < last) to = e->idx + 1; break;
    case NavigateDirection_PreviousSibling: if (e->idx > BE_LINE) to = e->idx - 1; break;
    default: break;
    }
    unsigned gen = e->gen;
    LeaveCriticalSection(&g_lock);
    if (to == -9) return S_OK;
    bar_el *n = bel_new(e->slot, gen, to);
    if (!n) return E_OUTOFMEMORY;
    *out = &n->f;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_GetRuntimeId(IRawElementProviderFragment *t, SAFEARRAY **out) {
    bar_el *e = CONTAINING_RECORD(t, bar_el, f);
    *out = NULL;
    if (e->idx == BE_ROOT) return S_OK;          /* the host supplies the window's */
    SAFEARRAY *sa = SafeArrayCreateVector(VT_I4, 0, 2);
    if (!sa) return E_OUTOFMEMORY;
    LONG i0 = 0, i1 = 1;
    int v0 = UiaAppendRuntimeId, v1 = e->idx;   /* the line and each button keep their place */
    SafeArrayPutElement(sa, &i0, &v0);
    SafeArrayPutElement(sa, &i1, &v1);
    *out = sa;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_GetBounding(IRawElementProviderFragment *t, struct UiaRect *out) {
    bar_el *e = CONTAINING_RECORD(t, bar_el, f);
    out->left = out->top = out->width = out->height = 0;
    EnterCriticalSection(&g_lock);
    bar_slot *b = live(e);
    if (b && e->idx >= BE_BTN0 && e->idx < BE_BTN0 + b->n_btn) {
        const oc_acc_item *it = &b->btn[e->idx - BE_BTN0];
        POINT tl = { it->l, it->t }, br = { it->r, it->b };
        ClientToScreen(b->hwnd, &tl);
        ClientToScreen(b->hwnd, &br);
        out->left = tl.x; out->top = tl.y;
        out->width = (double)(br.x - tl.x); out->height = (double)(br.y - tl.y);
    } else if (b && e->idx == BE_LINE) {
        /* The line is the bar left of its first button. */
        RECT rc; GetClientRect(b->hwnd, &rc);
        POINT tl = { 0, 0 }, br = { b->n_btn ? b->btn[0].l : rc.right, rc.bottom };
        ClientToScreen(b->hwnd, &tl);
        ClientToScreen(b->hwnd, &br);
        out->left = tl.x; out->top = tl.y;
        out->width = (double)(br.x - tl.x); out->height = (double)(br.y - tl.y);
    }
    LeaveCriticalSection(&g_lock);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_GetRoots(IRawElementProviderFragment *t, SAFEARRAY **out) {
    (void)t; *out = NULL; return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_SetFocus(IRawElementProviderFragment *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE f_GetFragmentRoot(IRawElementProviderFragment *t, IRawElementProviderFragmentRoot **out) {
    bar_el *e = CONTAINING_RECORD(t, bar_el, f);
    bar_el *root = bel_new(e->slot, e->gen, BE_ROOT);
    if (!root) { *out = NULL; return E_OUTOFMEMORY; }
    *out = &root->r;
    return S_OK;
}
static IRawElementProviderFragmentVtbl g_fvt = {
    f_QI, f_AddRef, f_Release, f_Navigate, f_GetRuntimeId, f_GetBounding, f_GetRoots, f_SetFocus, f_GetFragmentRoot
};

/* ---- IRawElementProviderFragmentRoot ---------------------------------------- */

static HRESULT STDMETHODCALLTYPE r_QI(IRawElementProviderFragmentRoot *t, REFIID iid, void **o) {
    return s_QI(&CONTAINING_RECORD(t, bar_el, r)->s, iid, o);
}
static ULONG STDMETHODCALLTYPE r_AddRef(IRawElementProviderFragmentRoot *t)  { return s_AddRef(&CONTAINING_RECORD(t, bar_el, r)->s); }
static ULONG STDMETHODCALLTYPE r_Release(IRawElementProviderFragmentRoot *t) { return s_Release(&CONTAINING_RECORD(t, bar_el, r)->s); }
static HRESULT STDMETHODCALLTYPE r_FromPoint(IRawElementProviderFragmentRoot *t, double x, double y,
                                             IRawElementProviderFragment **out) {
    bar_el *e = CONTAINING_RECORD(t, bar_el, r);
    *out = NULL;
    EnterCriticalSection(&g_lock);
    bar_slot *b = live(e);
    int hit = -9;
    if (b) {
        POINT p = { (LONG)x, (LONG)y };
        ScreenToClient(b->hwnd, &p);
        hit = BE_LINE;
        for (int i = 0; i < b->n_btn; i++)
            if (p.x >= b->btn[i].l && p.x < b->btn[i].r && p.y >= b->btn[i].t && p.y < b->btn[i].b) hit = BE_BTN0 + i;
    }
    LeaveCriticalSection(&g_lock);
    if (hit == -9) return S_OK;
    bar_el *n = bel_new(e->slot, e->gen, hit);
    if (!n) return E_OUTOFMEMORY;
    *out = &n->f;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE r_GetFocus(IRawElementProviderFragmentRoot *t, IRawElementProviderFragment **out) {
    (void)t; *out = NULL; return S_OK;
}
static IRawElementProviderFragmentRootVtbl g_rvt = {
    r_QI, r_AddRef, r_Release, r_FromPoint, r_GetFocus
};

/* ---- IInvokeProvider ---------------------------------------------------------
 * Posted to the UI thread as the main provider's presses are (a11y.c says why);
 * a button published without a token is switched off and refuses. */
static HRESULT STDMETHODCALLTYPE iv_QI(IInvokeProvider *t, REFIID iid, void **o) {
    return s_QI(&CONTAINING_RECORD(t, bar_el, iv)->s, iid, o);
}
static ULONG STDMETHODCALLTYPE iv_AddRef(IInvokeProvider *t)  { return s_AddRef(&CONTAINING_RECORD(t, bar_el, iv)->s); }
static ULONG STDMETHODCALLTYPE iv_Release(IInvokeProvider *t) { return s_Release(&CONTAINING_RECORD(t, bar_el, iv)->s); }
static HRESULT STDMETHODCALLTYPE iv_Invoke(IInvokeProvider *t) {
    bar_el *e = CONTAINING_RECORD(t, bar_el, iv);
    uint64_t tok = 0;
    EnterCriticalSection(&g_lock);
    bar_slot *b = live(e);
    if (b && e->idx >= BE_BTN0 && e->idx < BE_BTN0 + b->n_btn) tok = b->btn[e->idx - BE_BTN0].invoke;
    LeaveCriticalSection(&g_lock);
    if (!b) return UIA_E_ELEMENTNOTAVAILABLE;
    if (!tok) return UIA_E_ELEMENTNOTENABLED;
    SDL_Event ev; memset(&ev, 0, sizeof ev);
    ev.type = oc_plat_event_type(); ev.user.code = OC_PLAT_EV_A11Y_INVOKE;
    ev.user.data1 = (void *)(uintptr_t)(tok >> 32); ev.user.data2 = (void *)(uintptr_t)(tok & 0xFFFFFFFFu);
    SDL_PushEvent(&ev);
    return S_OK;
}
static IInvokeProviderVtbl g_ivvt = { iv_QI, iv_AddRef, iv_Release, iv_Invoke };

static bar_el *bel_new(int slot, unsigned gen, int idx) {
    bar_el *e = (bar_el *)calloc(1, sizeof *e);
    if (!e) return NULL;
    e->s.lpVtbl = &g_svt; e->f.lpVtbl = &g_fvt; e->r.lpVtbl = &g_rvt; e->iv.lpVtbl = &g_ivvt;
    e->ref = 1; e->slot = slot; e->gen = gen; e->idx = idx;
    return e;
}

/* ---- the window procedure ---------------------------------------------------- */

static int slot_of(HWND h) {
    for (int i = 0; i < BAR_SLOTS; i++) if (g_bar[i].hwnd == h) return i;
    return -1;
}

static LRESULT CALLBACK bar_proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    EnterCriticalSection(&g_lock);
    int i = slot_of(h);
    WNDPROC prev = i >= 0 ? g_bar[i].prev : NULL;
    unsigned gen = i >= 0 ? g_bar[i].gen : 0;
    LeaveCriticalSection(&g_lock);
    if (!prev) return DefWindowProcW(h, msg, wp, lp);
    if (msg == WM_GETOBJECT && (DWORD)lp == (DWORD)UiaRootObjectId && p_return_provider) {
        bar_el *root = bel_new(i, gen, BE_ROOT);
        if (root) {
            LRESULT r = p_return_provider(h, wp, lp, &root->s);
            s_Release(&root->s);
            return r;
        }
    }
    return CallWindowProcW(prev, h, msg, wp, lp);
}

static void resolve(void) {
    if (g_resolved) return;
    g_resolved = 1;
    if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = 1; }
    HMODULE m = LoadLibraryW(L"UIAutomationCore.dll");
    if (!m) return;
    p_return_provider = (fn_return_provider)(void *)GetProcAddress(m, "UiaReturnRawElementProvider");
    p_host_provider   = (fn_host_provider)(void *)GetProcAddress(m, "UiaHostProviderFromHwnd");
}

static HWND hwnd_of(SDL_Window *w) {
    return w ? (HWND)SDL_GetPointerProperty(SDL_GetWindowProperties(w), SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL) : NULL;
}

/* ---- public surface ------------------------------------------------------------ */

void oc_a11y_bar_publish(SDL_Window *w, const char *aid, const char *name, const char *line,
                         const oc_acc_item *buttons, int n) {
    HWND h = hwnd_of(w);
    if (!h) return;
    resolve();
    if (!p_return_provider || !p_host_provider) return;
    if (n > BAR_BTNS) n = BAR_BTNS;
    EnterCriticalSection(&g_lock);
    int i = slot_of(h);
    int fresh = i < 0;
    if (fresh) for (int k = 0; k < BAR_SLOTS && i < 0; k++) if (!g_bar[k].hwnd) i = k;
    if (i < 0) { LeaveCriticalSection(&g_lock); return; }
    bar_slot *b = &g_bar[i];
    /* The procedure it chains to is recorded before ours goes in, so no
     * message can reach ours without somewhere to go. */
    if (fresh) { b->hwnd = h; b->gen++; b->prev = (WNDPROC)GetWindowLongPtrW(h, GWLP_WNDPROC); }
    snprintf(b->aid, sizeof b->aid, "%s", aid ? aid : "");
    snprintf(b->name, sizeof b->name, "%s", name ? name : "");
    snprintf(b->line, sizeof b->line, "%s", line ? line : "");
    for (int k = 0; k < n; k++) b->btn[k] = buttons[k];
    b->n_btn = n;
    LeaveCriticalSection(&g_lock);
    /* Subclassed outside the lock: SetWindowLongPtr can send messages, and the
     * procedure takes the lock. */
    if (fresh) SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)bar_proc);
}

void oc_a11y_bar_close(SDL_Window *w) {
    HWND h = hwnd_of(w);
    if (!h || !g_lock_ready) return;
    EnterCriticalSection(&g_lock);
    int i = slot_of(h);
    WNDPROC prev = i >= 0 ? g_bar[i].prev : NULL;
    if (i >= 0) { g_bar[i].hwnd = NULL; g_bar[i].prev = NULL; g_bar[i].n_btn = 0; }
    LeaveCriticalSection(&g_lock);
    if (i < 0) return;
    /* The window's own procedure back before SDL destroys it, and UIA told the
     * provider is gone (the documented answer for a window going away). */
    if (prev) SetWindowLongPtrW(h, GWLP_WNDPROC, (LONG_PTR)prev);
    if (p_return_provider) p_return_provider(h, 0, 0, NULL);
}

#endif /* __IRawElementProviderFragment_INTERFACE_DEFINED__ */
