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
#include <wctype.h>   /* towlower — FindText's ignore-case */

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
    ITextProvider                   t;   /* transcript + composer (ARCH-99) */
    IValueProvider                  v;   /* the composer's text, read-only */
    LONG ref;
    int  idx;        /* EL_* or an index into g_items */
} acc_el;

static acc_el *el_new(int idx);
static ITextProviderVtbl  g_tpvt;    /* fwd — defined with the text surface below */
static IValueProviderVtbl g_vpvt;
static int  composer_idx(void);

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
    acc_el *e = CONTAINING_RECORD(this_, acc_el, s);
    *out = NULL;
    EnterCriticalSection(&g_lock);
    int is_composer = item_kind(e->idx, OC_ACC_COMPOSER);
    int is_script   = (e->idx == EL_MSGS);
    LeaveCriticalSection(&g_lock);
    /* TextPattern on the composer AND the transcript: one so what you typed can
     * be proof-read, the other so the conversation can be READ rather than
     * stepped through element by element. ValuePattern on the composer as well,
     * because a client that only wants "what is in that box" should not have to
     * open a text range to find out. */
    if (pid == UIA_TextPatternId && (is_composer || is_script)) {
        *out = (IUnknown *)&e->t; s_AddRef(&e->s); return S_OK;
    }
    if (pid == UIA_ValuePatternId && is_composer) {
        *out = (IUnknown *)&e->v; s_AddRef(&e->s); return S_OK;
    }
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
        /* The message list is a DOCUMENT, not a List: screen readers look for
         * TextPattern on a Document, and "read the conversation" is the whole
         * point. It keeps its ListItem children, so stepping message by message
         * still works. */
        v->lVal = (e->idx == EL_MSGS)                       ? UIA_DocumentControlTypeId
                : (e->idx == EL_CONVS)                      ? UIA_ListControlTypeId
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
    else if (IsEqualIID(iid, &IID_ITextProvider))
        *out = &e->t;
    else if (IsEqualIID(iid, &IID_IValueProvider))
        *out = &e->v;
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
    e->t.lpVtbl = &g_tpvt;
    e->v.lpVtbl = &g_vpvt;
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

/* ============================================================================
 * Text (REQ-269, ARCH-99) — ITextProvider / ITextRangeProvider / IValueProvider
 *
 * Two documents, addressed the same way:
 *
 *   DOC_COMPOSER   what you are typing, so it can be REVIEWED and not only
 *                  written — a field a screen reader cannot read back is a
 *                  field you cannot proof-read.
 *   DOC_TRANSCRIPT the loaded messages, flattened in drawn order, so "read all"
 *                  works and reading does not require stepping element by
 *                  element.
 *
 * The transcript document is the published item NAMES joined with newlines —
 * the same "who, when: what" a screen reader speaks when navigating the list.
 * Building it from anything else would give the two surfaces different words for
 * the same message.
 *
 * TWO BOUNDS, STATED RATHER THAN IMPLIED:
 *  - `Line` is a LOGICAL line (newline-delimited), not a visual one. The
 *    composer wraps, so a wrapped line reads as one line here. Honest and
 *    survivable; per-visual-line would mean holding a DWrite layout for every
 *    message.
 *  - `GetBoundingRectangles` resolves to MESSAGE granularity in the transcript
 *    (the enclosing row), not per character, for the same reason.
 * ========================================================================== */

enum { DOC_COMPOSER = 0, DOC_TRANSCRIPT = 1 };

static WCHAR g_doc[65536];
static int   g_doc_len;
static int   g_doc_which = -1;
/* Where each message landed in the flattened transcript, so a range can be
 * mapped back to the row it came from. */
static struct { int start, end, item; } g_doc_msg[OC_ACC_MAX];
static int   g_n_doc_msg;

/* Caller holds g_lock. */
static void doc_build(int doc) {
    g_doc_which = doc;
    g_n_doc_msg = 0;
    if (doc == DOC_COMPOSER) {
        int n = g_comp_len;
        if (n > (int)(sizeof g_doc / sizeof g_doc[0]) - 1) n = (int)(sizeof g_doc / sizeof g_doc[0]) - 1;
        memcpy(g_doc, g_comp, (size_t)n * sizeof(WCHAR));
        g_doc[n] = 0; g_doc_len = n;
        return;
    }
    int at = 0, cap = (int)(sizeof g_doc / sizeof g_doc[0]) - 2;
    for (int i = 0; i < g_n_items && at < cap; i++) {
        if (g_items[i].kind != OC_ACC_MESSAGE) continue;
        int w = MultiByteToWideChar(CP_UTF8, 0, g_items[i].name, -1, g_doc + at, cap - at);
        if (w <= 0) continue;
        w -= 1;                                     /* drop the NUL */
        if (g_n_doc_msg < OC_ACC_MAX) {
            g_doc_msg[g_n_doc_msg].start = at;
            g_doc_msg[g_n_doc_msg].end   = at + w;
            g_doc_msg[g_n_doc_msg].item  = i;
            g_n_doc_msg++;
        }
        at += w;
        if (at < cap) g_doc[at++] = L'\n';
    }
    g_doc[at] = 0; g_doc_len = at;
}

static void doc_need(int doc) { if (g_doc_which != doc) doc_build(doc); else doc_build(doc); }

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int is_wordch(WCHAR c) { return !(c == L' ' || c == L'\t' || c == L'\n' || c == L'\r'); }

/* ---- ITextRangeProvider ---------------------------------------------------- */

typedef struct {
    ITextRangeProvider itr;
    LONG ref;
    int  doc;        /* DOC_* */
    int  s, e;       /* [s,e) offsets into that document */
} acc_range;

static acc_range *range_new(int doc, int s, int e);

static HRESULT STDMETHODCALLTYPE tr_QI(ITextRangeProvider *t, REFIID iid, void **out) {
    acc_range *r = CONTAINING_RECORD(t, acc_range, itr);
    if (!out) return E_POINTER;
    if (IsEqualIID(iid, &IID_IUnknown) || IsEqualIID(iid, &IID_ITextRangeProvider)) {
        *out = &r->itr; InterlockedIncrement(&r->ref); return S_OK;
    }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE tr_AddRef(ITextRangeProvider *t) {
    return (ULONG)InterlockedIncrement(&CONTAINING_RECORD(t, acc_range, itr)->ref);
}
static ULONG STDMETHODCALLTYPE tr_Release(ITextRangeProvider *t) {
    acc_range *r = CONTAINING_RECORD(t, acc_range, itr);
    LONG n = InterlockedDecrement(&r->ref);
    if (!n) free(r);
    return (ULONG)n;
}
static HRESULT STDMETHODCALLTYPE tr_Clone(ITextRangeProvider *t, ITextRangeProvider **out) {
    acc_range *r = CONTAINING_RECORD(t, acc_range, itr);
    acc_range *c = range_new(r->doc, r->s, r->e);
    if (!c) { *out = NULL; return E_OUTOFMEMORY; }
    *out = &c->itr; return S_OK;
}
static HRESULT STDMETHODCALLTYPE tr_Compare(ITextRangeProvider *t, ITextRangeProvider *o, BOOL *eq) {
    acc_range *a = CONTAINING_RECORD(t, acc_range, itr);
    acc_range *b = CONTAINING_RECORD(o, acc_range, itr);
    *eq = (a->doc == b->doc && a->s == b->s && a->e == b->e) ? TRUE : FALSE;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE tr_CompareEndpoints(ITextRangeProvider *t,
        enum TextPatternRangeEndpoint ep1, ITextRangeProvider *o,
        enum TextPatternRangeEndpoint ep2, int *ret) {
    acc_range *a = CONTAINING_RECORD(t, acc_range, itr);
    acc_range *b = CONTAINING_RECORD(o, acc_range, itr);
    int va = (ep1 == TextPatternRangeEndpoint_Start) ? a->s : a->e;
    int vb = (ep2 == TextPatternRangeEndpoint_Start) ? b->s : b->e;
    *ret = va - vb;
    return S_OK;
}
/* Grow [s,e) out to the enclosing unit. */
static void unit_expand(int doc, enum TextUnit unit, int *s, int *e) {
    doc_need(doc);
    int L = g_doc_len;
    *s = clampi(*s, 0, L); *e = clampi(*e, *s, L);
    switch (unit) {
    case TextUnit_Character:
        if (*e <= *s) *e = clampi(*s + 1, 0, L);
        break;
    case TextUnit_Word:
        while (*s > 0 && is_wordch(g_doc[*s - 1])) (*s)--;
        while (*e < L && is_wordch(g_doc[*e])) (*e)++;
        if (*e == *s && *e < L) (*e)++;
        break;
    case TextUnit_Line:
    case TextUnit_Paragraph:
        while (*s > 0 && g_doc[*s - 1] != L'\n') (*s)--;
        while (*e < L && g_doc[*e] != L'\n') (*e)++;
        break;
    default:                       /* Page, Document, Format */
        *s = 0; *e = L;
        break;
    }
}
static HRESULT STDMETHODCALLTYPE tr_Expand(ITextRangeProvider *t, enum TextUnit unit) {
    acc_range *r = CONTAINING_RECORD(t, acc_range, itr);
    EnterCriticalSection(&g_lock);
    unit_expand(r->doc, unit, &r->s, &r->e);
    LeaveCriticalSection(&g_lock);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE tr_FindAttribute(ITextRangeProvider *t, TEXTATTRIBUTEID a,
                                                  VARIANT v, BOOL back, ITextRangeProvider **out) {
    (void)t; (void)a; (void)v; (void)back; *out = NULL; return S_OK;
}
static HRESULT STDMETHODCALLTYPE tr_FindText(ITextRangeProvider *t, BSTR text, BOOL back,
                                             BOOL ignorecase, ITextRangeProvider **out) {
    acc_range *r = CONTAINING_RECORD(t, acc_range, itr);
    *out = NULL;
    if (!text) return S_OK;
    EnterCriticalSection(&g_lock);
    doc_need(r->doc);
    int tl = (int)SysStringLen(text), found = -1;
    if (tl > 0 && tl <= r->e - r->s) {
        for (int i = back ? r->e - tl : r->s;
             back ? i >= r->s : i + tl <= r->e;
             i += back ? -1 : 1) {
            int k = 0;
            for (; k < tl; k++) {
                WCHAR c1 = g_doc[i + k], c2 = text[k];
                if (ignorecase) { c1 = (WCHAR)towlower(c1); c2 = (WCHAR)towlower(c2); }
                if (c1 != c2) break;
            }
            if (k == tl) { found = i; break; }
        }
    }
    LeaveCriticalSection(&g_lock);
    if (found < 0) return S_OK;
    acc_range *n = range_new(r->doc, found, found + tl);
    if (!n) return E_OUTOFMEMORY;
    *out = &n->itr;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE tr_GetAttributeValue(ITextRangeProvider *t, TEXTATTRIBUTEID a,
                                                      VARIANT *v) {
    (void)t; (void)a;
    VariantInit(v);
    v->vt = VT_UNKNOWN; v->punkVal = NULL;   /* "not supported", per the contract */
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE tr_GetBoundingRectangles(ITextRangeProvider *t, SAFEARRAY **out) {
    acc_range *r = CONTAINING_RECORD(t, acc_range, itr);
    *out = NULL;
    EnterCriticalSection(&g_lock);
    doc_need(r->doc);
    /* Message granularity — see the header comment. One rect per row the range
     * touches, which is what a magnifier needs to follow reading. */
    double rects[4 * 64];
    int n = 0;
    if (r->doc == DOC_TRANSCRIPT) {
        for (int i = 0; i < g_n_doc_msg && n < 64; i++) {
            if (g_doc_msg[i].end <= r->s || g_doc_msg[i].start >= r->e) continue;
            const oc_acc_item *it = &g_items[g_doc_msg[i].item];
            POINT tl = { it->l, it->t }, br = { it->r, it->b };
            ClientToScreen(g_hwnd, &tl); ClientToScreen(g_hwnd, &br);
            rects[n * 4 + 0] = tl.x; rects[n * 4 + 1] = tl.y;
            rects[n * 4 + 2] = br.x - tl.x; rects[n * 4 + 3] = br.y - tl.y;
            n++;
        }
    } else {
        int ci = composer_idx();
        if (ci >= 0) {
            const oc_acc_item *it = &g_items[ci];
            POINT tl = { it->l, it->t }, br = { it->r, it->b };
            ClientToScreen(g_hwnd, &tl); ClientToScreen(g_hwnd, &br);
            rects[0] = tl.x; rects[1] = tl.y; rects[2] = br.x - tl.x; rects[3] = br.y - tl.y;
            n = 1;
        }
    }
    LeaveCriticalSection(&g_lock);
    SAFEARRAY *sa = SafeArrayCreateVector(VT_R8, 0, (ULONG)(n * 4));
    if (!sa) return E_OUTOFMEMORY;
    for (LONG i = 0; i < n * 4; i++) SafeArrayPutElement(sa, &i, &rects[i]);
    *out = sa;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE tr_GetEnclosingElement(ITextRangeProvider *t,
                                                        IRawElementProviderSimple **out) {
    acc_range *r = CONTAINING_RECORD(t, acc_range, itr);
    *out = NULL;
    EnterCriticalSection(&g_lock);
    int idx = EL_MSGS;
    if (r->doc == DOC_COMPOSER) idx = composer_idx();
    else for (int i = 0; i < g_n_doc_msg; i++)
        if (g_doc_msg[i].end > r->s && g_doc_msg[i].start < r->e) { idx = g_doc_msg[i].item; break; }
    LeaveCriticalSection(&g_lock);
    acc_el *e = el_new(idx);
    if (!e) return E_OUTOFMEMORY;
    *out = &e->s;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE tr_GetText(ITextRangeProvider *t, int maxlen, BSTR *out) {
    acc_range *r = CONTAINING_RECORD(t, acc_range, itr);
    EnterCriticalSection(&g_lock);
    doc_need(r->doc);
    int s = clampi(r->s, 0, g_doc_len), e = clampi(r->e, s, g_doc_len);
    if (maxlen >= 0 && e - s > maxlen) e = s + maxlen;
    *out = SysAllocStringLen(g_doc + s, (UINT)(e - s));
    LeaveCriticalSection(&g_lock);
    return *out ? S_OK : E_OUTOFMEMORY;
}
/* Step whole units, collapsing the range as UIA specifies. */
static int unit_step(int doc, enum TextUnit unit, int pos, int count) {
    doc_need(doc);
    int L = g_doc_len, dir = count < 0 ? -1 : 1, todo = count < 0 ? -count : count;
    while (todo-- > 0) {
        if (unit == TextUnit_Character) { pos = clampi(pos + dir, 0, L); continue; }
        if (unit == TextUnit_Word) {
            pos = clampi(pos + dir, 0, L);
            while (pos > 0 && pos < L && is_wordch(g_doc[pos - 1]) == is_wordch(g_doc[pos])) pos += dir;
            continue;
        }
        if (unit == TextUnit_Line || unit == TextUnit_Paragraph) {
            pos = clampi(pos + dir, 0, L);
            while (pos > 0 && pos < L && g_doc[pos - 1] != L'\n') pos += dir;
            continue;
        }
        pos = dir < 0 ? 0 : L;
    }
    return clampi(pos, 0, L);
}
static HRESULT STDMETHODCALLTYPE tr_Move(ITextRangeProvider *t, enum TextUnit unit,
                                         int count, int *moved) {
    acc_range *r = CONTAINING_RECORD(t, acc_range, itr);
    EnterCriticalSection(&g_lock);
    int before = r->s;
    r->s = unit_step(r->doc, unit, r->s, count);
    r->e = r->s;
    unit_expand(r->doc, unit, &r->s, &r->e);
    *moved = (r->s == before) ? 0 : count;
    LeaveCriticalSection(&g_lock);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE tr_MoveEndpointByUnit(ITextRangeProvider *t,
        enum TextPatternRangeEndpoint ep, enum TextUnit unit, int count, int *moved) {
    acc_range *r = CONTAINING_RECORD(t, acc_range, itr);
    EnterCriticalSection(&g_lock);
    int *v = (ep == TextPatternRangeEndpoint_Start) ? &r->s : &r->e;
    int before = *v;
    *v = unit_step(r->doc, unit, *v, count);
    if (r->e < r->s) { if (ep == TextPatternRangeEndpoint_Start) r->e = r->s; else r->s = r->e; }
    *moved = (*v == before) ? 0 : count;
    LeaveCriticalSection(&g_lock);
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE tr_MoveEndpointByRange(ITextRangeProvider *t,
        enum TextPatternRangeEndpoint ep, ITextRangeProvider *o,
        enum TextPatternRangeEndpoint oep) {
    acc_range *r = CONTAINING_RECORD(t, acc_range, itr);
    acc_range *b = CONTAINING_RECORD(o, acc_range, itr);
    int v = (oep == TextPatternRangeEndpoint_Start) ? b->s : b->e;
    if (ep == TextPatternRangeEndpoint_Start) { r->s = v; if (r->e < r->s) r->e = r->s; }
    else                                      { r->e = v; if (r->e < r->s) r->s = r->e; }
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE tr_Select(ITextRangeProvider *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE tr_AddToSelection(ITextRangeProvider *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE tr_RemoveFromSelection(ITextRangeProvider *t) { (void)t; return S_OK; }
static HRESULT STDMETHODCALLTYPE tr_ScrollIntoView(ITextRangeProvider *t, BOOL top) {
    (void)t; (void)top; return S_OK;
}
static HRESULT STDMETHODCALLTYPE tr_GetChildren(ITextRangeProvider *t, SAFEARRAY **out) {
    (void)t;
    *out = SafeArrayCreateVector(VT_UNKNOWN, 0, 0);
    return *out ? S_OK : E_OUTOFMEMORY;
}

static ITextRangeProviderVtbl g_trvt = {
    tr_QI, tr_AddRef, tr_Release, tr_Clone, tr_Compare, tr_CompareEndpoints,
    tr_Expand, tr_FindAttribute, tr_FindText, tr_GetAttributeValue,
    tr_GetBoundingRectangles, tr_GetEnclosingElement, tr_GetText, tr_Move,
    tr_MoveEndpointByUnit, tr_MoveEndpointByRange, tr_Select, tr_AddToSelection,
    tr_RemoveFromSelection, tr_ScrollIntoView, tr_GetChildren
};

static acc_range *range_new(int doc, int s, int e) {
    acc_range *r = (acc_range *)calloc(1, sizeof *r);
    if (!r) return NULL;
    r->itr.lpVtbl = &g_trvt;
    r->ref = 1; r->doc = doc; r->s = s; r->e = e;
    return r;
}

/* ---- ITextProvider --------------------------------------------------------- */

static int el_doc(const acc_el *e) {
    return (e->idx == EL_MSGS) ? DOC_TRANSCRIPT : DOC_COMPOSER;
}

static HRESULT STDMETHODCALLTYPE tp_QI(ITextProvider *t, REFIID iid, void **o) {
    return s_QI(&CONTAINING_RECORD(t, acc_el, t)->s, iid, o);
}
static ULONG STDMETHODCALLTYPE tp_AddRef(ITextProvider *t) {
    return s_AddRef(&CONTAINING_RECORD(t, acc_el, t)->s);
}
static ULONG STDMETHODCALLTYPE tp_Release(ITextProvider *t) {
    return s_Release(&CONTAINING_RECORD(t, acc_el, t)->s);
}
static HRESULT STDMETHODCALLTYPE tp_GetSelection(ITextProvider *t, SAFEARRAY **out) {
    acc_el *e = CONTAINING_RECORD(t, acc_el, t);
    *out = NULL;
    int doc = el_doc(e), s = 0, en = 0;
    EnterCriticalSection(&g_lock);
    if (doc == DOC_COMPOSER) {
        s  = g_comp_caret < g_comp_anchor ? g_comp_caret : g_comp_anchor;
        en = g_comp_caret > g_comp_anchor ? g_comp_caret : g_comp_anchor;
    }
    LeaveCriticalSection(&g_lock);
    acc_range *r = range_new(doc, s, en);
    if (!r) return E_OUTOFMEMORY;
    SAFEARRAY *sa = SafeArrayCreateVector(VT_UNKNOWN, 0, 1);
    if (!sa) { tr_Release(&r->itr); return E_OUTOFMEMORY; }
    LONG i = 0;
    SafeArrayPutElement(sa, &i, (IUnknown *)&r->itr);
    tr_Release(&r->itr);            /* SafeArrayPutElement took its own ref */
    *out = sa;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE tp_GetVisibleRanges(ITextProvider *t, SAFEARRAY **out) {
    acc_el *e = CONTAINING_RECORD(t, acc_el, t);
    int doc = el_doc(e), len;
    EnterCriticalSection(&g_lock);
    doc_need(doc); len = g_doc_len;
    LeaveCriticalSection(&g_lock);
    /* Everything published IS what is on screen — the paint pass only describes
     * what it drew — so the visible range is the document. */
    acc_range *r = range_new(doc, 0, len);
    if (!r) { *out = NULL; return E_OUTOFMEMORY; }
    SAFEARRAY *sa = SafeArrayCreateVector(VT_UNKNOWN, 0, 1);
    if (!sa) { tr_Release(&r->itr); *out = NULL; return E_OUTOFMEMORY; }
    LONG i = 0;
    SafeArrayPutElement(sa, &i, (IUnknown *)&r->itr);
    tr_Release(&r->itr);
    *out = sa;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE tp_RangeFromChild(ITextProvider *t,
        IRawElementProviderSimple *child, ITextRangeProvider **out) {
    acc_el *e = CONTAINING_RECORD(t, acc_el, t);
    acc_el *c = CONTAINING_RECORD(child, acc_el, s);
    *out = NULL;
    int s = 0, en = 0;
    EnterCriticalSection(&g_lock);
    doc_need(el_doc(e));
    for (int i = 0; i < g_n_doc_msg; i++)
        if (g_doc_msg[i].item == c->idx) { s = g_doc_msg[i].start; en = g_doc_msg[i].end; break; }
    LeaveCriticalSection(&g_lock);
    acc_range *r = range_new(el_doc(e), s, en);
    if (!r) return E_OUTOFMEMORY;
    *out = &r->itr;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE tp_RangeFromPoint(ITextProvider *t, struct UiaPoint pt,
                                                   ITextRangeProvider **out) {
    acc_el *e = CONTAINING_RECORD(t, acc_el, t);
    POINT p = { (LONG)pt.x, (LONG)pt.y };
    ScreenToClient(g_hwnd, &p);
    int s = 0, en = 0;
    EnterCriticalSection(&g_lock);
    doc_need(el_doc(e));
    for (int i = 0; i < g_n_doc_msg; i++) {
        const oc_acc_item *it = &g_items[g_doc_msg[i].item];
        if (p.y >= it->t && p.y < it->b) { s = g_doc_msg[i].start; en = s; break; }
    }
    LeaveCriticalSection(&g_lock);
    acc_range *r = range_new(el_doc(e), s, en);
    if (!r) { *out = NULL; return E_OUTOFMEMORY; }
    *out = &r->itr;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE tp_DocumentRange(ITextProvider *t, ITextRangeProvider **out) {
    acc_el *e = CONTAINING_RECORD(t, acc_el, t);
    int doc = el_doc(e), len;
    EnterCriticalSection(&g_lock);
    doc_need(doc); len = g_doc_len;
    LeaveCriticalSection(&g_lock);
    acc_range *r = range_new(doc, 0, len);
    if (!r) { *out = NULL; return E_OUTOFMEMORY; }
    *out = &r->itr;
    return S_OK;
}
static HRESULT STDMETHODCALLTYPE tp_SupportedTextSelection(ITextProvider *t,
        enum SupportedTextSelection *out) {
    acc_el *e = CONTAINING_RECORD(t, acc_el, t);
    /* The composer has a caret and a selection; the transcript is read-only and
     * says so rather than pretending to a selection it cannot move. */
    *out = (el_doc(e) == DOC_COMPOSER) ? SupportedTextSelection_Single
                                       : SupportedTextSelection_None;
    return S_OK;
}

static ITextProviderVtbl g_tpvt = {
    tp_QI, tp_AddRef, tp_Release, tp_GetSelection, tp_GetVisibleRanges,
    tp_RangeFromChild, tp_RangeFromPoint, tp_DocumentRange, tp_SupportedTextSelection
};

/* ---- IValueProvider -------------------------------------------------------- */

static HRESULT STDMETHODCALLTYPE vp_QI(IValueProvider *t, REFIID iid, void **o) {
    return s_QI(&CONTAINING_RECORD(t, acc_el, v)->s, iid, o);
}
static ULONG STDMETHODCALLTYPE vp_AddRef(IValueProvider *t) {
    return s_AddRef(&CONTAINING_RECORD(t, acc_el, v)->s);
}
static ULONG STDMETHODCALLTYPE vp_Release(IValueProvider *t) {
    return s_Release(&CONTAINING_RECORD(t, acc_el, v)->s);
}
static HRESULT STDMETHODCALLTYPE vp_SetValue(IValueProvider *t, LPCWSTR val) {
    (void)t; (void)val;
    /* Deliberately refused. Letting automation stuff text into the composer
     * would bypass the completion, mention scanning and draft handling that make
     * it that field rather than a buffer — and a half-updated composer is worse
     * than a read-only one. */
    return UIA_E_NOTSUPPORTED;
}
static HRESULT STDMETHODCALLTYPE vp_get_Value(IValueProvider *t, BSTR *out) {
    (void)t;
    EnterCriticalSection(&g_lock);
    *out = SysAllocStringLen(g_comp, (UINT)g_comp_len);
    LeaveCriticalSection(&g_lock);
    return *out ? S_OK : E_OUTOFMEMORY;
}
static HRESULT STDMETHODCALLTYPE vp_get_IsReadOnly(IValueProvider *t, BOOL *out) {
    (void)t; *out = TRUE; return S_OK;
}

static IValueProviderVtbl g_vpvt = {
    vp_QI, vp_AddRef, vp_Release, vp_SetValue, vp_get_Value, vp_get_IsReadOnly
};
