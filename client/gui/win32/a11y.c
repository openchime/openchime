/*
 * OpenChime Win32 — the UI Automation provider (REQ-269, ARCH-99).
 *
 * See a11y.h for why this is served from a published snapshot.
 *
 * THE ENTRY POINTS ARE RESOLVED AT RUNTIME. mingw-w64 ships the UIA headers
 * (with C vtables — winmain.c defines COBJMACROS) but NO import library for
 * UIAutomationCore: `liboleacc.a` is the only accessibility lib in the
 * distribution. Rather than generate a .def and a link library — a build-system
 * dependency the cross-compile would have to carry — the five provider functions
 * are looked up with GetProcAddress, exactly as the client already resolves
 * SetProcessDpiAwarenessContext, GetDpiForWindow and MiniDumpWriteDump. The
 * consequence is a feature that degrades to nothing rather than a build that
 * fails somewhere else.
 *
 * The tree is two levels, because a flat list mixing conversations with messages
 * is technically accessible and useless to navigate:
 *
 *   root (the window)
 *     ├── "Conversations"  (List)  → one ListItem per sidebar row
 *     ├── "Messages"       (List)  → one ListItem per drawn message
 *     └── "Message"        (Edit)  → the composer
 */
#define COBJMACROS
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00   /* UiaRaiseNotificationEvent is Win10 1709+ */
#endif
#include <windows.h>
#include <initguid.h>
#include <uiautomation.h>
#include <oleauto.h>
#include <stdio.h>
#include <string.h>

#include "a11y.h"

/* ---- runtime-resolved provider API ---------------------------------------- */

typedef LRESULT (WINAPI *fn_return_provider)(HWND, WPARAM, LPARAM, IRawElementProviderSimple *);
typedef HRESULT (WINAPI *fn_host_provider)(HWND, IRawElementProviderSimple **);
typedef HRESULT (WINAPI *fn_raise_event)(IRawElementProviderSimple *, EVENTID);
typedef HRESULT (WINAPI *fn_raise_notify)(IRawElementProviderSimple *, enum NotificationKind,
                                          enum NotificationProcessing, BSTR, BSTR);
typedef HRESULT (WINAPI *fn_disconnect)(IRawElementProviderSimple *);

static fn_return_provider p_return_provider;
static fn_host_provider   p_host_provider;
static fn_raise_event     p_raise_event;
static fn_raise_notify    p_raise_notify;
static fn_disconnect      p_disconnect;

static HWND     g_hwnd;
static int      g_ready;
static unsigned g_announced;

/* ---- the published snapshot ------------------------------------------------
 * Guarded because UIA may call a provider from a thread that is not ours; the
 * paint pass writes it and the provider reads it. */
static CRITICAL_SECTION g_lock;
static int              g_lock_ready;
static oc_acc_item      g_items[OC_ACC_MAX];
static int              g_n_items;
static WCHAR            g_comp[4096];
static int              g_comp_len, g_comp_caret, g_comp_anchor;

/* Pseudo-indices for the three synthetic group elements. Real items are >= 0. */
enum { EL_ROOT = -1, EL_CONVS = -2, EL_MSGS = -3, EL_COMPOSER = -4 };

/* ---- the element object ----------------------------------------------------
 * One object serving three interfaces. Each method recovers the object with
 * CONTAINING_RECORD from whichever interface it was called through. */
typedef struct {
    IRawElementProviderSimple       s;
    IRawElementProviderFragment     f;
    IRawElementProviderFragmentRoot r;
    LONG ref;
    int  idx;        /* EL_* or an index into g_items */
} acc_el;

static acc_el *el_new(int idx);

/* ---- helpers --------------------------------------------------------------- */

static BSTR bstr_utf8(const char *s) {
    if (!s) s = "";
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return SysAllocString(L"");
    WCHAR *w = (WCHAR *)calloc((size_t)n, sizeof(WCHAR));
    if (!w) return SysAllocString(L"");
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    BSTR b = SysAllocString(w);
    free(w);
    return b;
}

/* Is this a real item of the given kind? Group membership is derived from the
 * snapshot rather than stored, so the publisher cannot get the two out of step. */
static int item_kind(int idx, oc_acc_kind k) {
    return idx >= 0 && idx < g_n_items && g_items[idx].kind == k;
}
static int group_of(int idx) {
    if (idx < 0 || idx >= g_n_items) return EL_ROOT;
    switch (g_items[idx].kind) {
    case OC_ACC_CONVERSATION: return EL_CONVS;
    case OC_ACC_MESSAGE:      return EL_MSGS;
    default:                  return EL_ROOT;   /* the composer hangs off the root */
    }
}
/* The next/previous item of the same kind, or -1. */
static int sibling(int idx, int dir) {
    if (idx < 0 || idx >= g_n_items) return -1;
    oc_acc_kind k = g_items[idx].kind;
    for (int i = idx + dir; i >= 0 && i < g_n_items; i += dir)
        if (g_items[i].kind == k) return i;
    return -1;
}
static int first_of(oc_acc_kind k, int last) {
    int found = -1;
    for (int i = 0; i < g_n_items; i++)
        if (g_items[i].kind == k) { found = i; if (!last) break; }
    return found;
}
static int composer_idx(void) { return first_of(OC_ACC_COMPOSER, 0); }

/* ---- IRawElementProviderSimple --------------------------------------------- */

static HRESULT STDMETHODCALLTYPE s_QI(IRawElementProviderSimple *this_, REFIID iid, void **out);
static ULONG   STDMETHODCALLTYPE s_AddRef(IRawElementProviderSimple *this_);
static ULONG   STDMETHODCALLTYPE s_Release(IRawElementProviderSimple *this_);

static HRESULT STDMETHODCALLTYPE s_GetOptions(IRawElementProviderSimple *this_, enum ProviderOptions *o) {
    (void)this_;
    *o = ProviderOptions_ServerSideProvider;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE s_GetPatternProvider(IRawElementProviderSimple *this_,
                                                      PATTERNID pid, IUnknown **out) {
    (void)this_; (void)pid;
    *out = NULL;                    /* text patterns land in the next commit */
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE s_GetPropertyValue(IRawElementProviderSimple *this_,
                                                    PROPERTYID pid, VARIANT *v) {
    acc_el *e = CONTAINING_RECORD(this_, acc_el, s);
    VariantInit(v);
    EnterCriticalSection(&g_lock);
    switch (pid) {
    case UIA_ControlTypePropertyId:
        v->vt = VT_I4;
        v->lVal = (e->idx == EL_CONVS || e->idx == EL_MSGS) ? UIA_ListControlTypeId
                : (e->idx == EL_ROOT)                       ? UIA_GroupControlTypeId
                : item_kind(e->idx, OC_ACC_COMPOSER)        ? UIA_EditControlTypeId
                                                            : UIA_ListItemControlTypeId;
        break;
    case UIA_NamePropertyId:
        v->vt = VT_BSTR;
        v->bstrVal = (e->idx == EL_CONVS) ? bstr_utf8("Conversations")
                   : (e->idx == EL_MSGS)  ? bstr_utf8("Messages")
                   : (e->idx == EL_ROOT)  ? bstr_utf8("OpenChime")
                   : (e->idx >= 0 && e->idx < g_n_items) ? bstr_utf8(g_items[e->idx].name)
                                                         : bstr_utf8("");
        break;
    case UIA_IsControlElementPropertyId:
    case UIA_IsContentElementPropertyId:
        v->vt = VT_BOOL; v->boolVal = VARIANT_TRUE;
        break;
    case UIA_IsKeyboardFocusablePropertyId:
        v->vt = VT_BOOL;
        v->boolVal = item_kind(e->idx, OC_ACC_COMPOSER) ? VARIANT_TRUE : VARIANT_FALSE;
        break;
    case UIA_HasKeyboardFocusPropertyId:
        v->vt = VT_BOOL;
        v->boolVal = (item_kind(e->idx, OC_ACC_COMPOSER) && GetFocus() == g_hwnd)
                     ? VARIANT_TRUE : VARIANT_FALSE;
        break;
    default: break;                 /* VT_EMPTY: "ask the host / no opinion" */
    }
    LeaveCriticalSection(&g_lock);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE s_GetHostProvider(IRawElementProviderSimple *this_,
                                                   IRawElementProviderSimple **out) {
    acc_el *e = CONTAINING_RECORD(this_, acc_el, s);
    *out = NULL;
    /* Only the root is hosted by the HWND. Handing the host provider to every
     * element would make each one claim to be the window, and the tree collapses
     * into one node wearing many names. */
    if (e->idx == EL_ROOT && p_host_provider) return p_host_provider(g_hwnd, out);
    return S_OK;
}

static IRawElementProviderSimpleVtbl g_svt = {
    s_QI, s_AddRef, s_Release,
    s_GetOptions, s_GetPatternProvider, s_GetPropertyValue, s_GetHostProvider
};

/* ---- IRawElementProviderFragment ------------------------------------------- */

static HRESULT STDMETHODCALLTYPE f_QI(IRawElementProviderFragment *t, REFIID iid, void **o) {
    return s_QI(&CONTAINING_RECORD(t, acc_el, f)->s, iid, o);
}
static ULONG STDMETHODCALLTYPE f_AddRef(IRawElementProviderFragment *t) {
    return s_AddRef(&CONTAINING_RECORD(t, acc_el, f)->s);
}
static ULONG STDMETHODCALLTYPE f_Release(IRawElementProviderFragment *t) {
    return s_Release(&CONTAINING_RECORD(t, acc_el, f)->s);
}
static HRESULT STDMETHODCALLTYPE f_Navigate(IRawElementProviderFragment *t,
                                            enum NavigateDirection dir,
                                            IRawElementProviderFragment **out) {
    acc_el *e = CONTAINING_RECORD(t, acc_el, f);
    int target = -9999;
    *out = NULL;
    EnterCriticalSection(&g_lock);
    switch (dir) {
    case NavigateDirection_Parent:
        target = (e->idx == EL_ROOT) ? -9999
               : (e->idx == EL_CONVS || e->idx == EL_MSGS) ? EL_ROOT
               : group_of(e->idx);
        break;
    case NavigateDirection_FirstChild:
        target = (e->idx == EL_ROOT)  ? EL_CONVS
               : (e->idx == EL_CONVS) ? first_of(OC_ACC_CONVERSATION, 0)
               : (e->idx == EL_MSGS)  ? first_of(OC_ACC_MESSAGE, 0) : -9999;
        break;
    case NavigateDirection_LastChild:
        target = (e->idx == EL_ROOT)  ? composer_idx()
               : (e->idx == EL_CONVS) ? first_of(OC_ACC_CONVERSATION, 1)
               : (e->idx == EL_MSGS)  ? first_of(OC_ACC_MESSAGE, 1) : -9999;
        break;
    case NavigateDirection_NextSibling:
        target = (e->idx == EL_CONVS) ? EL_MSGS
               : (e->idx == EL_MSGS)  ? composer_idx()
               : (e->idx >= 0 && item_kind(e->idx, OC_ACC_COMPOSER)) ? -9999
               : sibling(e->idx, +1);
        break;
    case NavigateDirection_PreviousSibling:
        target = (e->idx == EL_MSGS) ? EL_CONVS
               : (e->idx >= 0 && item_kind(e->idx, OC_ACC_COMPOSER)) ? EL_MSGS
               : sibling(e->idx, -1);
        break;
    default: break;
    }
    LeaveCriticalSection(&g_lock);
    if (target == -9999 || target == -1) return S_OK;
    acc_el *n = el_new(target);
    if (!n) return E_OUTOFMEMORY;
    *out = &n->f;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_GetRuntimeId(IRawElementProviderFragment *t, SAFEARRAY **out) {
    acc_el *e = CONTAINING_RECORD(t, acc_el, f);
    *out = NULL;
    if (e->idx == EL_ROOT) return S_OK;      /* the host supplies the root's id */
    SAFEARRAY *sa = SafeArrayCreateVector(VT_I4, 0, 3);
    if (!sa) return E_OUTOFMEMORY;
    /* UiaAppendRuntimeId marks this as "below the HWND"; the two ints after it
     * must be STABLE for the same element across calls, or a screen reader
     * cannot tell "the same message" from "a new one" and re-reads the list. So
     * it is the kind plus the channel/message id, never the array index — the
     * index moves whenever anything above it is drawn. */
    LONG i0 = 0, i1 = 1, i2 = 2;
    int  kind = (e->idx == EL_CONVS) ? 100 : (e->idx == EL_MSGS) ? 101 : (int)g_items[e->idx].kind;
    int  low  = (e->idx >= 0 && e->idx < g_n_items) ? (int)(g_items[e->idx].id & 0x7fffffff) : 0;
    int  v0 = UiaAppendRuntimeId, v1 = kind, v2 = low;
    SafeArrayPutElement(sa, &i0, &v0);
    SafeArrayPutElement(sa, &i1, &v1);
    SafeArrayPutElement(sa, &i2, &v2);
    *out = sa;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_GetBounding(IRawElementProviderFragment *t, struct UiaRect *out) {
    acc_el *e = CONTAINING_RECORD(t, acc_el, f);
    out->left = out->top = out->width = out->height = 0;
    EnterCriticalSection(&g_lock);
    if (e->idx >= 0 && e->idx < g_n_items) {
        /* Published in client device pixels, so the only conversion here is to
         * screen space — the DPI arithmetic stays on the publisher's side. */
        POINT tl = { g_items[e->idx].l, g_items[e->idx].t };
        POINT br = { g_items[e->idx].r, g_items[e->idx].b };
        ClientToScreen(g_hwnd, &tl);
        ClientToScreen(g_hwnd, &br);
        out->left = tl.x; out->top = tl.y;
        out->width  = (double)(br.x - tl.x);
        out->height = (double)(br.y - tl.y);
    }
    LeaveCriticalSection(&g_lock);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_GetRoots(IRawElementProviderFragment *t, SAFEARRAY **out) {
    (void)t; *out = NULL; return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_SetFocus(IRawElementProviderFragment *t) {
    (void)t;
    /* Focus lives with the window: the composer is drawn, not a child, so there
     * is nothing else to give it to. */
    if (g_hwnd) SetFocus(g_hwnd);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE f_GetFragmentRoot(IRawElementProviderFragment *t,
                                                   IRawElementProviderFragmentRoot **out) {
    (void)t;
    acc_el *root = el_new(EL_ROOT);
    if (!root) { *out = NULL; return E_OUTOFMEMORY; }
    *out = &root->r;
    return S_OK;
}

static IRawElementProviderFragmentVtbl g_fvt = {
    f_QI, f_AddRef, f_Release,
    f_Navigate, f_GetRuntimeId, f_GetBounding, f_GetRoots, f_SetFocus, f_GetFragmentRoot
};

/* ---- IRawElementProviderFragmentRoot --------------------------------------- */

static HRESULT STDMETHODCALLTYPE r_QI(IRawElementProviderFragmentRoot *t, REFIID iid, void **o) {
    return s_QI(&CONTAINING_RECORD(t, acc_el, r)->s, iid, o);
}
static ULONG STDMETHODCALLTYPE r_AddRef(IRawElementProviderFragmentRoot *t) {
    return s_AddRef(&CONTAINING_RECORD(t, acc_el, r)->s);
}
static ULONG STDMETHODCALLTYPE r_Release(IRawElementProviderFragmentRoot *t) {
    return s_Release(&CONTAINING_RECORD(t, acc_el, r)->s);
}
static HRESULT STDMETHODCALLTYPE r_FromPoint(IRawElementProviderFragmentRoot *t,
                                             double x, double y,
                                             IRawElementProviderFragment **out) {
    (void)t;
    *out = NULL;
    POINT p = { (LONG)x, (LONG)y };
    ScreenToClient(g_hwnd, &p);
    int hit = -1;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_n_items; i++)
        if (p.x >= g_items[i].l && p.x < g_items[i].r &&
            p.y >= g_items[i].t && p.y < g_items[i].b) { hit = i; break; }
    LeaveCriticalSection(&g_lock);
    if (hit < 0) return S_OK;
    acc_el *e = el_new(hit);
    if (!e) return E_OUTOFMEMORY;
    *out = &e->f;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE r_GetFocus(IRawElementProviderFragmentRoot *t,
                                            IRawElementProviderFragment **out) {
    (void)t;
    *out = NULL;
    EnterCriticalSection(&g_lock);
    int ci = composer_idx();
    LeaveCriticalSection(&g_lock);
    if (ci < 0) return S_OK;
    acc_el *e = el_new(ci);
    if (!e) return E_OUTOFMEMORY;
    *out = &e->f;
    return S_OK;
}

static IRawElementProviderFragmentRootVtbl g_rvt = {
    r_QI, r_AddRef, r_Release, r_FromPoint, r_GetFocus
};

/* ---- IUnknown -------------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE s_QI(IRawElementProviderSimple *this_, REFIID iid, void **out) {
    acc_el *e = CONTAINING_RECORD(this_, acc_el, s);
    if (!out) return E_POINTER;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_IRawElementProviderSimple))
        *out = &e->s;
    else if (IsEqualIID(iid, &IID_IRawElementProviderFragment))
        *out = &e->f;
    else if (IsEqualIID(iid, &IID_IRawElementProviderFragmentRoot) && e->idx == EL_ROOT)
        *out = &e->r;
    else { *out = NULL; return E_NOINTERFACE; }
    InterlockedIncrement(&e->ref);
    return S_OK;
}
static ULONG STDMETHODCALLTYPE s_AddRef(IRawElementProviderSimple *this_) {
    return (ULONG)InterlockedIncrement(&CONTAINING_RECORD(this_, acc_el, s)->ref);
}
static ULONG STDMETHODCALLTYPE s_Release(IRawElementProviderSimple *this_) {
    acc_el *e = CONTAINING_RECORD(this_, acc_el, s);
    LONG n = InterlockedDecrement(&e->ref);
    if (n == 0) free(e);
    return (ULONG)n;
}

static acc_el *el_new(int idx) {
    acc_el *e = (acc_el *)calloc(1, sizeof *e);
    if (!e) return NULL;
    e->s.lpVtbl = &g_svt;
    e->f.lpVtbl = &g_fvt;
    e->r.lpVtbl = &g_rvt;
    e->ref = 1;
    e->idx = idx;
    return e;
}

/* ---- public surface -------------------------------------------------------- */

void oc_a11y_init(HWND hwnd) {
    g_hwnd = hwnd;
    if (!g_lock_ready) { InitializeCriticalSection(&g_lock); g_lock_ready = 1; }
    HMODULE m = LoadLibraryW(L"UIAutomationCore.dll");
    if (!m) return;
    p_return_provider = (fn_return_provider)(void *)GetProcAddress(m, "UiaReturnRawElementProvider");
    p_host_provider   = (fn_host_provider)(void *)GetProcAddress(m, "UiaHostProviderFromHwnd");
    p_raise_event     = (fn_raise_event)(void *)GetProcAddress(m, "UiaRaiseAutomationEvent");
    p_raise_notify    = (fn_raise_notify)(void *)GetProcAddress(m, "UiaRaiseNotificationEvent");
    p_disconnect      = (fn_disconnect)(void *)GetProcAddress(m, "UiaDisconnectProvider");
    /* The first two are the feature; the event functions are refinements and are
     * allowed to be missing on an older build. */
    g_ready = (p_return_provider && p_host_provider) ? 1 : 0;
}

void oc_a11y_shutdown(void) {
    if (g_ready && p_disconnect) {
        acc_el *root = el_new(EL_ROOT);
        if (root) { p_disconnect(&root->s); s_Release(&root->s); }
    }
    g_ready = 0;
}

int oc_a11y_available(void) { return g_ready; }

LRESULT oc_a11y_get_object(HWND hwnd, WPARAM wp, LPARAM lp, int *handled) {
    *handled = 0;
    if (!g_ready || (DWORD)lp != (DWORD)UiaRootObjectId) return 0;
    acc_el *root = el_new(EL_ROOT);
    if (!root) return 0;
    LRESULT r = p_return_provider(hwnd, wp, lp, &root->s);
    s_Release(&root->s);
    *handled = 1;
    return r;
}

void oc_a11y_publish(const oc_acc_item *items, int n,
                     const WCHAR *composer, int caret, int anchor) {
    if (!g_lock_ready) return;
    if (n < 0) n = 0;
    if (n > OC_ACC_MAX) n = OC_ACC_MAX;
    EnterCriticalSection(&g_lock);
    memcpy(g_items, items, (size_t)n * sizeof *items);
    g_n_items = n;
    g_comp_len = 0; g_comp[0] = 0;
    if (composer) {
        int L = (int)wcsnlen(composer, (sizeof g_comp / sizeof g_comp[0]) - 1);
        memcpy(g_comp, composer, (size_t)L * sizeof(WCHAR));
        g_comp[L] = 0; g_comp_len = L;
    }
    g_comp_caret = caret; g_comp_anchor = anchor;
    LeaveCriticalSection(&g_lock);
}

void oc_a11y_announce(const char *utf8) {
    g_announced++;
    if (!g_ready || !p_raise_notify || !utf8 || !utf8[0]) return;
    acc_el *root = el_new(EL_ROOT);
    if (!root) return;
    BSTR text = bstr_utf8(utf8);
    BSTR id   = SysAllocString(L"openchime");
    /* NotificationProcessing_MostRecent: a burst of arriving messages should
     * leave the reader saying the latest, not queueing a backlog it will still
     * be reading a minute later. */
    p_raise_notify(&root->s, NotificationKind_Other, NotificationProcessing_MostRecent, text, id);
    SysFreeString(text);
    SysFreeString(id);
    s_Release(&root->s);
}

unsigned oc_a11y_announced(void) { return g_announced; }

void oc_a11y_focus(oc_acc_kind kind, uint64_t id) {
    if (!g_ready || !p_raise_event) return;
    int found = -1;
    EnterCriticalSection(&g_lock);
    for (int i = 0; i < g_n_items; i++)
        if (g_items[i].kind == kind && g_items[i].id == id) { found = i; break; }
    LeaveCriticalSection(&g_lock);
    if (found < 0) return;
    acc_el *e = el_new(found);
    if (!e) return;
    p_raise_event(&e->s, UIA_AutomationFocusChangedEventId);
    s_Release(&e->s);
}
