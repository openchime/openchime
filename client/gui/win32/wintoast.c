/* Real Windows notifications from C. See wintoast.h for why this file exists
 * and why everything in it fails soft. */

#include "wintoast.h"

#include <windows.h>
/* The two WinRT headers mingw DOES ship: HSTRING and the activation entry
 * points. The interfaces below are hand-declared precisely because
 * windows.ui.notifications.h and windows.data.xml.dom.h are not here. */
#include <winstring.h>
#include <roapi.h>
/* The Start-menu shortcut needs IShellLink and the property store; propkey.h
 * carries the two AppUserModel keys Windows resolves a toast's identity and its
 * activator through. */
#include <shlobj.h>
#include <shlguid.h>
#include <propvarutil.h>
#include <propkey.h>
#include <propsys.h>
#include <stdio.h>
#include <string.h>

/* ---- the pieces mingw does ship ------------------------------------------
 * roapi.h and winstring.h are present, so RoGetActivationFactory,
 * WindowsCreateString and friends are declared for us. They are resolved
 * DYNAMICALLY anyway: linking combase means a machine without it cannot start
 * the client at all, and "no notifications" must never become "no app". */
typedef HRESULT (WINAPI *pfn_RoInitialize)(int);
typedef HRESULT (WINAPI *pfn_RoGetActivationFactory)(HSTRING, REFIID, void **);
typedef HRESULT (WINAPI *pfn_WindowsCreateString)(const WCHAR *, UINT32, HSTRING *);
typedef HRESULT (WINAPI *pfn_WindowsDeleteString)(HSTRING);

static pfn_RoInitialize             p_RoInitialize;
static pfn_RoGetActivationFactory   p_RoGetActivationFactory;
static pfn_WindowsCreateString      p_WindowsCreateString;
static pfn_WindowsDeleteString      p_WindowsDeleteString;

/* ---- the interfaces mingw does NOT ship ----------------------------------
 * Hand-declared. Only the methods actually called are named; the rest of each
 * vtable is padding, because a COM vtable is positional and a missing slot
 * silently shifts every method after it. The padding is spelled out rather than
 * elided for exactly that reason — it is load-bearing.
 *
 * IIDs from the Windows SDK's IDL. They are stable ABI, not implementation
 * detail, which is what makes writing them out here legitimate. */

typedef struct IInspectable IInspectable;

/* Windows.Data.Xml.Dom.IXmlDocument — created via activation, then loaded from
 * a string through IXmlDocumentIO. Building the DOM node by node is the other
 * way and is many times the C for no gain: the payload is a fixed template. */
typedef struct IXmlDocument IXmlDocument;
typedef struct IXmlDocumentVtbl {
    HRESULT (WINAPI *QueryInterface)(IXmlDocument *, REFIID, void **);
    ULONG   (WINAPI *AddRef)(IXmlDocument *);
    ULONG   (WINAPI *Release)(IXmlDocument *);
    /* IInspectable */
    void   *GetIids, *GetRuntimeClassName, *GetTrustLevel;
} IXmlDocumentVtbl;
struct IXmlDocument { const IXmlDocumentVtbl *lpVtbl; };

typedef struct IXmlDocumentIO IXmlDocumentIO;
typedef struct IXmlDocumentIOVtbl {
    HRESULT (WINAPI *QueryInterface)(IXmlDocumentIO *, REFIID, void **);
    ULONG   (WINAPI *AddRef)(IXmlDocumentIO *);
    ULONG   (WINAPI *Release)(IXmlDocumentIO *);
    void   *GetIids, *GetRuntimeClassName, *GetTrustLevel;
    HRESULT (WINAPI *LoadXml)(IXmlDocumentIO *, HSTRING);
    void   *LoadXmlWithSettings;
    void   *SaveToFileAsync;
} IXmlDocumentIOVtbl;
struct IXmlDocumentIO { const IXmlDocumentIOVtbl *lpVtbl; };

/* Windows.UI.Notifications.IToastNotification — the notification object. */
typedef struct IToastNotification IToastNotification;
typedef struct IToastNotificationVtbl {
    HRESULT (WINAPI *QueryInterface)(IToastNotification *, REFIID, void **);
    ULONG   (WINAPI *AddRef)(IToastNotification *);
    ULONG   (WINAPI *Release)(IToastNotification *);
    void   *GetIids, *GetRuntimeClassName, *GetTrustLevel;
    void   *get_Content;
    void   *put_ExpirationTime, *get_ExpirationTime;
    void   *add_Dismissed, *remove_Dismissed;
    void   *add_Activated, *remove_Activated;
    void   *add_Failed,    *remove_Failed;
} IToastNotificationVtbl;
struct IToastNotification { const IToastNotificationVtbl *lpVtbl; };

/* IToastNotification2 carries Tag and Group — replace-by-tag, so a second
 * message in a conversation supersedes the first instead of stacking. */
typedef struct IToastNotification2 IToastNotification2;
typedef struct IToastNotification2Vtbl {
    HRESULT (WINAPI *QueryInterface)(IToastNotification2 *, REFIID, void **);
    ULONG   (WINAPI *AddRef)(IToastNotification2 *);
    ULONG   (WINAPI *Release)(IToastNotification2 *);
    void   *GetIids, *GetRuntimeClassName, *GetTrustLevel;
    HRESULT (WINAPI *put_Tag)(IToastNotification2 *, HSTRING);
    void   *get_Tag;
    HRESULT (WINAPI *put_Group)(IToastNotification2 *, HSTRING);
    void   *get_Group;
    void   *put_SuppressPopup, *get_SuppressPopup;
} IToastNotification2Vtbl;
struct IToastNotification2 { const IToastNotification2Vtbl *lpVtbl; };

/* The factory that turns an XmlDocument into a notification. */
typedef struct IToastNotificationFactory IToastNotificationFactory;
typedef struct IToastNotificationFactoryVtbl {
    HRESULT (WINAPI *QueryInterface)(IToastNotificationFactory *, REFIID, void **);
    ULONG   (WINAPI *AddRef)(IToastNotificationFactory *);
    ULONG   (WINAPI *Release)(IToastNotificationFactory *);
    void   *GetIids, *GetRuntimeClassName, *GetTrustLevel;
    HRESULT (WINAPI *CreateToastNotification)(IToastNotificationFactory *,
                                              IXmlDocument *, IToastNotification **);
} IToastNotificationFactoryVtbl;
struct IToastNotificationFactory { const IToastNotificationFactoryVtbl *lpVtbl; };

typedef struct IToastNotifier IToastNotifier;
typedef struct IToastNotifierVtbl {
    HRESULT (WINAPI *QueryInterface)(IToastNotifier *, REFIID, void **);
    ULONG   (WINAPI *AddRef)(IToastNotifier *);
    ULONG   (WINAPI *Release)(IToastNotifier *);
    void   *GetIids, *GetRuntimeClassName, *GetTrustLevel;
    HRESULT (WINAPI *Show)(IToastNotifier *, IToastNotification *);
    void   *Hide;
    void   *get_Setting;
    void   *AddToSchedule, *RemoveFromSchedule, *GetScheduledToastNotifications;
} IToastNotifierVtbl;
struct IToastNotifier { const IToastNotifierVtbl *lpVtbl; };

/* CreateToastNotifier(aumid) is the call that binds a toast to an identity. */
typedef struct IToastNotificationManagerStatics IToastNotificationManagerStatics;
typedef struct IToastNotificationManagerStaticsVtbl {
    HRESULT (WINAPI *QueryInterface)(IToastNotificationManagerStatics *, REFIID, void **);
    ULONG   (WINAPI *AddRef)(IToastNotificationManagerStatics *);
    ULONG   (WINAPI *Release)(IToastNotificationManagerStatics *);
    void   *GetIids, *GetRuntimeClassName, *GetTrustLevel;
    void   *CreateToastNotifier;                      /* () — package identity only */
    HRESULT (WINAPI *CreateToastNotifierWithId)(IToastNotificationManagerStatics *,
                                                HSTRING, IToastNotifier **);
    void   *GetTemplateContent;
} IToastNotificationManagerStaticsVtbl;
struct IToastNotificationManagerStatics { const IToastNotificationManagerStaticsVtbl *lpVtbl; };

static const IID IID_IXmlDocumentIO =
    { 0x6cd0e74e, 0xee65, 0x4489, { 0x9e, 0xbf, 0xca, 0x43, 0xe8, 0x7b, 0xa6, 0x37 } };
static const IID IID_IToastNotificationManagerStatics =
    { 0x50ac103f, 0xd235, 0x4598, { 0xbb, 0xef, 0x98, 0xfe, 0x4d, 0x1a, 0x3a, 0xd4 } };
static const IID IID_IToastNotificationFactory =
    { 0x04124b20, 0x82c6, 0x4229, { 0xb1, 0x09, 0xfd, 0x9e, 0xd4, 0x66, 0x2b, 0x53 } };
static const IID IID_IToastNotification2 =
    { 0x9dfb9fd1, 0x143a, 0x490e, { 0x90, 0xbf, 0xb9, 0xfb, 0xa7, 0x13, 0x2d, 0xe7 } };

/* Runtime class names, as HSTRINGs at init. */
static const WCHAR RCS_XMLDOC[]  = L"Windows.Data.Xml.Dom.XmlDocument";
static const WCHAR RCS_TOAST[]   = L"Windows.UI.Notifications.ToastNotification";
static const WCHAR RCS_MANAGER[] = L"Windows.UI.Notifications.ToastNotificationManager";

/* ---- state ---------------------------------------------------------------- */

static int   g_ready;
static int   g_tried;
static WCHAR g_aumid[256];
static IToastNotificationManagerStatics *g_mgr;
static IToastNotificationFactory        *g_toast_factory;

static HSTRING hs(const WCHAR *w) {
    HSTRING h = NULL;
    if (p_WindowsCreateString) p_WindowsCreateString(w, (UINT32)wcslen(w), &h);
    return h;
}
static void hs_free(HSTRING h) { if (h && p_WindowsDeleteString) p_WindowsDeleteString(h); }

/* A factory by runtime class name, or NULL. */
static void *factory_for(const WCHAR *cls, const IID *iid) {
    void *out = NULL;
    HSTRING h = hs(cls);
    if (!h) return NULL;
    if (FAILED(p_RoGetActivationFactory(h, iid, &out))) out = NULL;
    hs_free(h);
    return out;
}

int oc_wintoast_init(const char *aumid) {
    if (g_tried) return g_ready;
    g_tried = 1;
    if (!aumid || !aumid[0]) return 0;

    HMODULE cb = LoadLibraryW(L"combase.dll");
    if (!cb) return 0;
    p_RoInitialize           = (pfn_RoInitialize)(void *)GetProcAddress(cb, "RoInitialize");
    p_RoGetActivationFactory = (pfn_RoGetActivationFactory)(void *)GetProcAddress(cb, "RoGetActivationFactory");
    p_WindowsCreateString    = (pfn_WindowsCreateString)(void *)GetProcAddress(cb, "WindowsCreateString");
    p_WindowsDeleteString    = (pfn_WindowsDeleteString)(void *)GetProcAddress(cb, "WindowsDeleteString");
    if (!p_RoInitialize || !p_RoGetActivationFactory ||
        !p_WindowsCreateString || !p_WindowsDeleteString) return 0;

    /* RPC_E_CHANGED_MODE means someone already initialised this thread as STA,
     * which is fine and not a failure — the client's main thread is an STA
     * because of the shell and the file dialogs. */
    HRESULT hr = p_RoInitialize(1 /* RO_INIT_SINGLETHREADED */);
    if (FAILED(hr) && hr != (HRESULT)0x80010106L /* RPC_E_CHANGED_MODE */) return 0;

    MultiByteToWideChar(CP_UTF8, 0, aumid, -1, g_aumid,
                        (int)(sizeof g_aumid / sizeof g_aumid[0]));

    g_mgr = factory_for(RCS_MANAGER, &IID_IToastNotificationManagerStatics);
    g_toast_factory = factory_for(RCS_TOAST, &IID_IToastNotificationFactory);
    if (!g_mgr || !g_toast_factory) { oc_wintoast_done(); return 0; }

    /* PROVE IT, rather than assuming: ask for the notifier this AUMID resolves
     * to. Without a Start-menu shortcut carrying the same id this fails, and it
     * is the failure that otherwise looks like a toast that simply never
     * appears -- the caller needs the answer now, at startup, so it can fall
     * back for the whole session instead of dropping each notification. */
    {
        IToastNotifier *n = NULL;
        HSTRING a = hs(g_aumid);
        HRESULT r = a ? g_mgr->lpVtbl->CreateToastNotifierWithId(g_mgr, a, &n) : E_FAIL;
        hs_free(a);
        if (FAILED(r) || !n) { oc_wintoast_done(); return 0; }
        n->lpVtbl->Release(n);
    }
    g_ready = 1;
    return 1;
}

int oc_wintoast_available(void) { return g_ready; }

/* XML-escape into `out`. The body is a chat message: it will contain & and <
 * eventually, and an unescaped one makes LoadXml fail, which would read as
 * "notifications stopped working" for one particular message. */
static void xml_escape(const char *s, WCHAR *out, size_t cap) {
    WCHAR wide[1024];
    MultiByteToWideChar(CP_UTF8, 0, s ? s : "", -1, wide,
                        (int)(sizeof wide / sizeof wide[0]));
    size_t o = 0;
    for (size_t i = 0; wide[i] && o + 8 < cap; i++) {
        const WCHAR *rep = NULL;
        switch (wide[i]) {
            case L'&':  rep = L"&amp;";  break;
            case L'<':  rep = L"&lt;";   break;
            case L'>':  rep = L"&gt;";   break;
            case L'"':  rep = L"&quot;"; break;
            case L'\'': rep = L"&apos;"; break;
            default: break;
        }
        if (rep) { for (size_t k = 0; rep[k]; k++) out[o++] = rep[k]; }
        else out[o++] = wide[i];
    }
    out[o] = 0;
}

static int oc_wintoast_show_xml(const WCHAR *xml, const char *tag, const char *group);

int oc_wintoast_show(const char *title, const char *body,
                     const char *tag, const char *group,
                     const char *arg, const char *sound) {
    if (!g_ready) return 0;

    WCHAR wtitle[512], wbody[1024], warg[512], wsnd[128];
    xml_escape(title, wtitle, sizeof wtitle / sizeof wtitle[0]);
    xml_escape(body,  wbody,  sizeof wbody  / sizeof wbody[0]);
    xml_escape(arg,   warg,   sizeof warg   / sizeof warg[0]);
    /* The audio element, or nothing at all. "" is an explicit silence; NULL
     * leaves the element out and Windows plays its default. */
    wsnd[0] = 0;
    if (sound && sound[0]) {
        WCHAR n[96]; xml_escape(sound, n, 96);
        _snwprintf(wsnd, 128, L"<audio src=\"%s\"/>", n);
    } else if (sound) {
        _snwprintf(wsnd, 128, L"<audio silent=\"true\"/>");
    }

    /* ToastGeneric, not one of the legacy ToastText templates: the legacy ones
     * cannot carry a launch argument, and the launch argument is the entire
     * mechanism by which a click knows which conversation to open.
     *
     * activationType="protocol" means the launch argument is a URL and Windows
     * opens it. That is what lets this work with no COM activator, no CLSID
     * under HKCU and no LocalServer32 -- the app already registers openchime://
     * and already knows how to follow one. */
    WCHAR xml[3072];
    _snwprintf(xml, sizeof xml / sizeof xml[0],
        L"<toast launch=\"%s\" activationType=\"protocol\">"
        L"<visual><binding template=\"ToastGeneric\">"
        L"<text>%s</text><text>%s</text>"
        L"</binding></visual>%s</toast>",
        warg, wtitle, wbody, wsnd);
    return oc_wintoast_show_xml(xml, tag, group);
}

/* Everything from "here is the XML" onward, shared by the plain toast and the
 * one with actions: the difference between them is the document, and nothing
 * after it. */
static int oc_wintoast_show_xml(const WCHAR *xml, const char *tag, const char *group) {
    int ok = 0;
    IXmlDocument *doc = NULL;
    IXmlDocumentIO *io = NULL;
    IToastNotification *toast = NULL;
    IToastNotifier *notifier = NULL;
    HSTRING hxml = NULL, haumid = NULL;

    /* The XmlDocument is activated through its own factory's IActivationFactory
     * ActivateInstance slot, which is the 7th entry of every activation
     * factory's vtable -- IInspectable's three plus IUnknown's three. */
    {
        typedef struct IActFac IActFac;
        typedef struct IActFacVtbl {
            HRESULT (WINAPI *QueryInterface)(IActFac *, REFIID, void **);
            ULONG   (WINAPI *AddRef)(IActFac *);
            ULONG   (WINAPI *Release)(IActFac *);
            void   *GetIids, *GetRuntimeClassName, *GetTrustLevel;
            HRESULT (WINAPI *ActivateInstance)(IActFac *, IInspectable **);
        } IActFacVtbl;
        struct IActFac { const IActFacVtbl *lpVtbl; };
        static const IID IID_IActivationFactory =
            { 0x00000035, 0x0000, 0x0000, { 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };
        IActFac *af = factory_for(RCS_XMLDOC, &IID_IActivationFactory);
        if (!af) return 0;
        IInspectable *insp = NULL;
        HRESULT r = af->lpVtbl->ActivateInstance(af, &insp);
        af->lpVtbl->Release(af);
        if (FAILED(r) || !insp) return 0;
        doc = (IXmlDocument *)insp;
    }

    if (FAILED(doc->lpVtbl->QueryInterface(doc, &IID_IXmlDocumentIO, (void **)&io)) || !io)
        goto done;
    hxml = hs(xml);
    if (!hxml || FAILED(io->lpVtbl->LoadXml(io, hxml))) goto done;

    if (FAILED(g_toast_factory->lpVtbl->CreateToastNotification(g_toast_factory, doc, &toast))
        || !toast) goto done;

    /* Tag and group are best-effort: an older Windows without
     * IToastNotification2 still shows the toast, it just stacks instead of
     * replacing. Losing the nicety is not worth losing the notification. */
    if (tag && tag[0]) {
        IToastNotification2 *t2 = NULL;
        if (SUCCEEDED(toast->lpVtbl->QueryInterface(toast, &IID_IToastNotification2,
                                                    (void **)&t2)) && t2) {
            WCHAR w[128];
            MultiByteToWideChar(CP_UTF8, 0, tag, -1, w, 128);
            HSTRING h = hs(w); if (h) { t2->lpVtbl->put_Tag(t2, h); hs_free(h); }
            if (group && group[0]) {
                MultiByteToWideChar(CP_UTF8, 0, group, -1, w, 128);
                h = hs(w); if (h) { t2->lpVtbl->put_Group(t2, h); hs_free(h); }
            }
            t2->lpVtbl->Release(t2);
        }
    }

    haumid = hs(g_aumid);
    if (!haumid) goto done;
    if (FAILED(g_mgr->lpVtbl->CreateToastNotifierWithId(g_mgr, haumid, &notifier)) || !notifier)
        goto done;
    ok = SUCCEEDED(notifier->lpVtbl->Show(notifier, toast));

done:
    hs_free(hxml);
    hs_free(haumid);
    if (notifier) notifier->lpVtbl->Release(notifier);
    if (toast)    toast->lpVtbl->Release(toast);
    if (io)       io->lpVtbl->Release(io);
    if (doc)      doc->lpVtbl->Release(doc);
    return ok;
}

/* ---- acting on a toast ---------------------------------------------------
 *
 * INotificationActivationCallback, implemented as a C vtable. The interface has
 * exactly one method beyond IUnknown, and Windows calls it with the action's
 * argument and whatever the user typed. This is the only route by which an
 * <input>'s text comes back -- protocol activation cannot carry it -- which is
 * why the whole apparatus exists.
 *
 * A SINGLE STATIC OBJECT, not a heap-allocated one: there is one activator per
 * process, it lives as long as the process, and reference counting an object
 * that can never be destroyed only invents a way to get it wrong. */

/* {6A1B2C3D-4E5F-4A6B-9C8D-0E1F2A3B4C5D} — this application's activator. It is
 * written into the registry and onto the Start-menu shortcut, so it is a
 * permanent identifier: changing it orphans every toast already on screen. */
static const CLSID CLSID_OC_ACTIVATOR =
    { 0x6a1b2c3d, 0x4e5f, 0x4a6b, { 0x9c, 0x8d, 0x0e, 0x1f, 0x2a, 0x3b, 0x4c, 0x5d } };
static const char CLSID_OC_ACTIVATOR_STR[] = "{6A1B2C3D-4E5F-4A6B-9C8D-0E1F2A3B4C5D}";
/* mingw's propkey.h stops short of this one. The AppUserModel property set is
 * {9F4C2855-...} and the activator CLSID is PID 26 -- stable published values,
 * like the IIDs above. */
static const PROPERTYKEY OC_PKEY_ToastActivatorCLSID = {
    { 0x9F4C2855, 0x9F79, 0x4B39, { 0xA8, 0xD0, 0xE1, 0xD4, 0x2D, 0xE1, 0xD5, 0xF3 } }, 26 };

static const IID IID_INotificationActivationCallback =
    { 0x53e31837, 0x6600, 0x4a81, { 0x93, 0x95, 0x75, 0xcf, 0xfe, 0x74, 0x6f, 0x94 } };

const char *oc_wintoast_activator_clsid(void) { return CLSID_OC_ACTIVATOR_STR; }

/* One entry per <input> the toast declared. */
typedef struct { LPCWSTR Key, Value; } OC_USER_INPUT;

typedef struct INotifAct INotifAct;
typedef struct INotifActVtbl {
    HRESULT (WINAPI *QueryInterface)(INotifAct *, REFIID, void **);
    ULONG   (WINAPI *AddRef)(INotifAct *);
    ULONG   (WINAPI *Release)(INotifAct *);
    HRESULT (WINAPI *Activate)(INotifAct *, LPCWSTR appUserModelId, LPCWSTR invokedArgs,
                               const OC_USER_INPUT *data, ULONG count);
} INotifActVtbl;
struct INotifAct { const INotifActVtbl *lpVtbl; };

static oc_wintoast_action_cb g_action_cb;
static DWORD g_class_cookie;

static void w2u(LPCWSTR w, char *out, int cap) {
    if (!w) { out[0] = 0; return; }
    WideCharToMultiByte(CP_UTF8, 0, w, -1, out, cap, NULL, NULL);
}

static HRESULT WINAPI act_QI(INotifAct *self, REFIID riid, void **out) {
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) ||
        IsEqualIID(riid, &IID_INotificationActivationCallback)) {
        *out = self; return S_OK;
    }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI act_AddRef(INotifAct *self)  { (void)self; return 2; }
static ULONG WINAPI act_Release(INotifAct *self) { (void)self; return 1; }

static HRESULT WINAPI act_Activate(INotifAct *self, LPCWSTR aumid, LPCWSTR args,
                                   const OC_USER_INPUT *data, ULONG count) {
    (void)self; (void)aumid;
    char a[512] = "", reply[1024] = "";
    w2u(args, a, sizeof a);
    /* The reply box, when the toast had one. Matched by KEY rather than by
     * position: a toast with a button and a text box delivers them in no
     * promised order. */
    for (ULONG i = 0; i < count && data; i++) {
        char k[64]; w2u(data[i].Key, k, sizeof k);
        if (strcmp(k, "reply") == 0) { w2u(data[i].Value, reply, sizeof reply); break; }
    }
    if (g_action_cb) g_action_cb(a, reply);
    return S_OK;
}

static const INotifActVtbl g_act_vtbl = { act_QI, act_AddRef, act_Release, act_Activate };
static INotifAct g_activator = { &g_act_vtbl };

/* The class factory. Same reasoning: one static object, no reference counting. */
typedef struct IClsFac IClsFac;
typedef struct IClsFacVtbl {
    HRESULT (WINAPI *QueryInterface)(IClsFac *, REFIID, void **);
    ULONG   (WINAPI *AddRef)(IClsFac *);
    ULONG   (WINAPI *Release)(IClsFac *);
    HRESULT (WINAPI *CreateInstance)(IClsFac *, IUnknown *, REFIID, void **);
    HRESULT (WINAPI *LockServer)(IClsFac *, BOOL);
} IClsFacVtbl;
struct IClsFac { const IClsFacVtbl *lpVtbl; };

static HRESULT WINAPI cf_QI(IClsFac *self, REFIID riid, void **out) {
    if (!out) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown) || IsEqualIID(riid, &IID_IClassFactory)) {
        *out = self; return S_OK;
    }
    *out = NULL; return E_NOINTERFACE;
}
static ULONG WINAPI cf_AddRef(IClsFac *s)  { (void)s; return 2; }
static ULONG WINAPI cf_Release(IClsFac *s) { (void)s; return 1; }
static HRESULT WINAPI cf_Create(IClsFac *s, IUnknown *outer, REFIID riid, void **out) {
    (void)s;
    if (outer) return CLASS_E_NOAGGREGATION;
    return g_activator.lpVtbl->QueryInterface(&g_activator, riid, out);
}
static HRESULT WINAPI cf_Lock(IClsFac *s, BOOL b) { (void)s; (void)b; return S_OK; }
static const IClsFacVtbl g_cf_vtbl = { cf_QI, cf_AddRef, cf_Release, cf_Create, cf_Lock };
static IClsFac g_class_factory = { &g_cf_vtbl };

/* Point the CLSID at this exe, per user. Written on every start rather than at
 * install time: a portable copy, a developer build and an upgraded path all
 * need it to name THIS binary. */
static void activator_register_clsid(void) {
    WCHAR exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return;
    WCHAR key[160], cmd[MAX_PATH + 8];
    _snwprintf(key, 160, L"Software\\Classes\\CLSID\\%hs\\LocalServer32",
               CLSID_OC_ACTIVATOR_STR);
    _snwprintf(cmd, MAX_PATH + 8, L"\"%s\" -ToastActivated", exe);
    HKEY k;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, key, 0, NULL, 0, KEY_WRITE, NULL, &k, NULL)
        == ERROR_SUCCESS) {
        RegSetValueExW(k, NULL, 0, REG_SZ, (const BYTE *)cmd,
                       (DWORD)((wcslen(cmd) + 1) * sizeof(WCHAR)));
        RegCloseKey(k);
    }
}

int oc_wintoast_activator_register(oc_wintoast_action_cb cb) {
    g_action_cb = cb;
    activator_register_clsid();
    /* MULTIPLEUSE so the running client services activations itself. Windows
     * starts a second copy only when nobody has the class registered -- which
     * is the cold-start case, and the reason -ToastActivated exists at all. */
    HRESULT hr = CoRegisterClassObject(&CLSID_OC_ACTIVATOR, (IUnknown *)&g_class_factory,
                                       CLSCTX_LOCAL_SERVER, REGCLS_MULTIPLEUSE,
                                       &g_class_cookie);
    return SUCCEEDED(hr);
}

void oc_wintoast_activator_unregister(void) {
    if (g_class_cookie) { CoRevokeClassObject(g_class_cookie); g_class_cookie = 0; }
}

/* The shortcut. Windows resolves BOTH the AppUserModelID and the activator
 * CLSID through it for an unpackaged app, and Inno cannot write the second --
 * so the app writes its own rather than shipping a toast that silently cannot
 * be clicked. Idempotent: rewritten only when absent. */
int oc_wintoast_ensure_shortcut(const char *aumid, const char *display_name) {
    WCHAR path[MAX_PATH];
    if (FAILED(SHGetFolderPathW(NULL, CSIDL_STARTMENU, NULL, 0, path))) return 0;
    WCHAR wname[128];
    MultiByteToWideChar(CP_UTF8, 0, display_name ? display_name : "OpenChime", -1, wname, 128);
    _snwprintf(path + wcslen(path), MAX_PATH - wcslen(path), L"\\Programs\\%s.lnk", wname);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return 1;   /* already there */

    WCHAR exe[MAX_PATH];
    if (!GetModuleFileNameW(NULL, exe, MAX_PATH)) return 0;

    IShellLinkW *link = NULL;
    IPersistFile *file = NULL;
    IPropertyStore *store = NULL;
    int ok = 0;
    if (FAILED(CoCreateInstance(&CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER,
                                &IID_IShellLinkW, (void **)&link)) || !link) return 0;
    link->lpVtbl->SetPath(link, exe);
    /* AND ITS ICON. Setting an explicit AppUserModelID hands Windows the
     * taskbar identity, and from then on the button's icon comes from THIS
     * shortcut rather than from the window class -- so a shortcut without one
     * replaces the application's icon with a generic square. Named explicitly
     * rather than left to the shell to extract from the target, which it does
     * not reliably do when the target sits on a network path. */
    link->lpVtbl->SetIconLocation(link, exe, 0);
    link->lpVtbl->SetDescription(link, L"OpenChime");
    if (SUCCEEDED(link->lpVtbl->QueryInterface(link, &IID_IPropertyStore, (void **)&store))
        && store) {
        PROPVARIANT pv;
        WCHAR w[160];
        MultiByteToWideChar(CP_UTF8, 0, aumid, -1, w, 160);
        pv.vt = VT_LPWSTR; pv.pwszVal = w;
        store->lpVtbl->SetValue(store, &PKEY_AppUserModel_ID, &pv);
        /* As a CLSID, not a string: the property is typed, and a shortcut whose
         * activator is the wrong type reads as no activator at all -- a toast
         * that appears and cannot be clicked. */
        /* Built by hand: propsys's InitPropVariantFromCLSID is not in mingw's
         * import library, and the variant is two fields. */
        PROPVARIANT cv;
        PropVariantInit(&cv);
        cv.vt = VT_CLSID;
        cv.puuid = (CLSID *)CoTaskMemAlloc(sizeof(CLSID));
        if (cv.puuid) {
            *cv.puuid = CLSID_OC_ACTIVATOR;
            store->lpVtbl->SetValue(store, &OC_PKEY_ToastActivatorCLSID, &cv);
            PropVariantClear(&cv);
        }
        store->lpVtbl->Commit(store);
        store->lpVtbl->Release(store);
    }
    if (SUCCEEDED(link->lpVtbl->QueryInterface(link, &IID_IPersistFile, (void **)&file))
        && file) {
        ok = SUCCEEDED(file->lpVtbl->Save(file, path, TRUE));
        file->lpVtbl->Release(file);
    }
    link->lpVtbl->Release(link);
    return ok;
}

int oc_wintoast_show_actions(const char *title, const char *body,
                             const char *source,
                             const char *tag, const char *group,
                             const char *arg, const char *sound,
                             const char *reply_placeholder,
                             const char *const *btn_label,
                             const char *const *btn_arg, int n_btn) {
    if (!g_ready) return 0;
    WCHAR acts[2048]; acts[0] = 0;
    size_t o = 0;
    o += (size_t)_snwprintf(acts + o, 2048 - o, L"<actions>");
    if (reply_placeholder && reply_placeholder[0]) {
        WCHAR ph[128]; xml_escape(reply_placeholder, ph, 128);
        o += (size_t)_snwprintf(acts + o, 2048 - o,
                 L"<input id=\"reply\" type=\"text\" placeHolderContent=\"%s\"/>", ph);
    }
    for (int i = 0; i < n_btn && o < 1700; i++) {
        WCHAR l[96], a[192];
        xml_escape(btn_label[i], l, 96);
        xml_escape(btn_arg[i],   a, 192);
        /* BACKGROUND activation: the click is handled without the window being
         * raised, which is the point of a quick reaction -- reacting should not
         * drag you into the app. The reply button is the same: you answered
         * already. hint-inputId ties the button to the text box, and is what
         * makes Windows render them as one row. */
        o += (size_t)_snwprintf(acts + o, 2048 - o,
                 L"<action content=\"%s\" arguments=\"%s\" activationType=\"background\"%s/>",
                 l, a,
                 (reply_placeholder && reply_placeholder[0] && i == 0)
                     ? L" hint-inputId=\"reply\"" : L"");
    }
    o += (size_t)_snwprintf(acts + o, 2048 - o, L"</actions>");

    WCHAR wtitle[512], wbody[1024], warg[512], wsnd[128], wsrc[192];
    xml_escape(title, wtitle, 512);
    xml_escape(body,  wbody,  1024);
    xml_escape(arg,   warg,   512);
    /* WHERE it was said, as attribution rather than a third ordinary line: the
     * schema renders attribution smaller and below, which is the right weight
     * for context you only need when the name and the message are ambiguous.
     * A direct message has no source -- the person IS the conversation. */
    wsrc[0] = 0;
    if (source && source[0]) {
        WCHAR sc[160]; xml_escape(source, sc, 160);
        _snwprintf(wsrc, 192, L"<text placement=\"attribution\">%s</text>", sc);
    }
    wsnd[0] = 0;
    if (sound && sound[0]) { WCHAR n[96]; xml_escape(sound, n, 96);
                             _snwprintf(wsnd, 128, L"<audio src=\"%s\"/>", n); }
    else if (sound)        { _snwprintf(wsnd, 128, L"<audio silent=\"true\"/>"); }

    WCHAR xml[4096];
    /* The toast BODY still activates by protocol -- clicking the notification
     * itself opens the conversation through the permalink, which needs nothing
     * from the activator. Only the buttons and the reply box route through COM,
     * because only they carry data back. */
    _snwprintf(xml, 4096,
        L"<toast launch=\"%s\" activationType=\"protocol\">"
        L"<visual><binding template=\"ToastGeneric\">"
        /* One line for the name, however long it is: wrapping a person's name
         * onto two lines pushes the message off the toast entirely. */
        L"<text hint-maxLines=\"1\">%s</text><text>%s</text>%s"
        L"</binding></visual>%s%s</toast>",
        warg, wtitle, wbody, wsrc, acts, wsnd);
    return oc_wintoast_show_xml(xml, tag, group);
}

void oc_wintoast_done(void) {
    if (g_toast_factory) { g_toast_factory->lpVtbl->Release(g_toast_factory); g_toast_factory = NULL; }
    if (g_mgr)           { g_mgr->lpVtbl->Release(g_mgr); g_mgr = NULL; }
    g_ready = 0;
}
