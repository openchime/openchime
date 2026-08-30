/* Real Windows notifications from C. See wintoast.h for why this file exists
 * and why everything in it fails soft. */

#include "wintoast.h"

#include <windows.h>
/* The two WinRT headers mingw DOES ship: HSTRING and the activation entry
 * points. The interfaces below are hand-declared precisely because
 * windows.ui.notifications.h and windows.data.xml.dom.h are not here. */
#include <winstring.h>
#include <roapi.h>
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

int oc_wintoast_show(const char *title, const char *body,
                     const char *tag, const char *group,
                     const char *arg, int silent) {
    if (!g_ready) return 0;

    WCHAR wtitle[512], wbody[1024], warg[512];
    xml_escape(title, wtitle, sizeof wtitle / sizeof wtitle[0]);
    xml_escape(body,  wbody,  sizeof wbody  / sizeof wbody[0]);
    xml_escape(arg,   warg,   sizeof warg   / sizeof warg[0]);

    /* ToastGeneric, not one of the legacy ToastText templates: the legacy ones
     * cannot carry a launch argument, and the launch argument is the entire
     * mechanism by which a click knows which conversation to open. */
    WCHAR xml[3072];
    _snwprintf(xml, sizeof xml / sizeof xml[0],
        L"<toast launch=\"%s\" activationType=\"foreground\">"
        L"<visual><binding template=\"ToastGeneric\">"
        L"<text>%s</text><text>%s</text>"
        L"</binding></visual>%s</toast>",
        warg, wtitle, wbody,
        silent ? L"<audio silent=\"true\"/>" : L"");

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

void oc_wintoast_done(void) {
    if (g_toast_factory) { g_toast_factory->lpVtbl->Release(g_toast_factory); g_toast_factory = NULL; }
    if (g_mgr)           { g_mgr->lpVtbl->Release(g_mgr); g_mgr = NULL; }
    g_ready = 0;
}
