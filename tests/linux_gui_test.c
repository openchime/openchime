/*
 * The Linux GUI's test program: the platform (client/gui/platform/linux/
 * plat_linux.c), the AT-SPI provider (a11y_atspi.c), desktop notifications
 * (client/shared/osnotify_linux.c) and capture through the portals
 * (client/core/media/cap_portal.c) and PipeWire, each driven through its own
 * interface.
 * Its exit code is its failure count.
 *
 * It is a program of its own, built by path (`make build/linux_gui_test`,
 * mk/tests.mk) and run by the full suite, because it needs the development
 * packages the Linux GUI needs, which `make test` must not (TESTING.md). It is
 * linked from the GUI's own objects, compiled as the GUI compiles them, and
 * from none of the application layer: what it tests is the platform's side of
 * platform.h, a11y.h, osnotify.h and cap_portal.h.
 *
 * The desktop it talks to is made here, in C over GDBus: a notification
 * server, an accessibility bus and registry, and a portal with the Camera,
 * ScreenCast and Settings interfaces, each owning its well-known name on the
 * session bus only while a check wants it there. They need a session bus of
 * their own, which `dbus-run-session build/linux_gui_test` gives them, and the
 * program stops at once, saying so, without one. The services are answered on
 * a thread of their own, with its own main loop, so the code under test can
 * make the synchronous calls it makes in the application. The PipeWire the
 * portal hands out is real: the stream group starts `pipewire` and
 * `wireplumber` as its children, under directories of its own, and stops them.
 *
 * Groups:
 *   platform  the data directory, the crash ring's mapping, the handoff of a
 *             URL from a second process, and the autostart and URL-scheme
 *             .desktop files, all in temporary XDG directories
 *   image     each format decoded to known pixels, and a header over the
 *             pixel cap refused before any pixel buffer is allocated
 *   notify    Notify's arguments, replacing, withdrawing, a click, a button
 *             and a typed reply reaching the callback, and no server
 *   a11y      the tree read back over the bus, the composer's text and caret,
 *             and DoAction posting the item's token
 *   capture   the portal's requests granted, refused and dismissed, no portal,
 *             and the frame conversion from synthetic buffers in every format
 *   stream    a private PipeWire and WirePlumber, and a source published on
 *             them, BGRx with a crop region as a screen and NV12 as a camera:
 *             the format agreed, frames converted, a screen's frame only when
 *             it changed, the camera listed, and the source going away
 *   settings  colour scheme, contrast and animations read from the portal,
 *             and each absent one answering -1
 */
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <jpeglib.h>
#include <spng.h>
#include <webp/encode.h>
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include "check.h"
#include "testenv.h"

#include "platform.h"
#include "a11y.h"
#include "osnotify.h"
#include "oc_capture.h"
#include "oc_media.h"
#include "cap_portal.h"
#include "preview.h"

int oc_test_skips;
extern char **environ;

/* ---- what the code under test allocates ---------------------------------------
 * The program is linked with -Wl,--wrap=malloc (mk/tests.mk), so every malloc
 * in the objects it links, the platform's and the vendored decoders' alike,
 * comes through here. The largest request since the last reset is how the
 * image group tells a header refused before its pixels were allocated from
 * one refused after. */
void *__real_malloc(size_t n);
static _Atomic size_t g_largest;

void *__wrap_malloc(size_t n) {
    size_t m = atomic_load(&g_largest);
    while (n > m && !atomic_compare_exchange_weak(&g_largest, &m, n)) {}
    return __real_malloc(n);
}

/* ---- the stand-in desktop ------------------------------------------------------- */

#define N_NAME   "org.freedesktop.Notifications"
#define N_PATH   "/org/freedesktop/Notifications"
#define P_NAME   "org.freedesktop.portal.Desktop"
#define P_PATH   "/org/freedesktop/portal/desktop"
#define A_BUS    "org.a11y.Bus"
#define A_REG    "org.a11y.atspi.Registry"
#define A_ROOT   "/org/a11y/atspi/accessible/root"
#define I_ACC    "org.a11y.atspi.Accessible"

static const char XML[] =
    "<node>"
    "<interface name='org.freedesktop.Notifications'>"
    " <method name='GetCapabilities'><arg type='as' direction='out'/></method>"
    " <method name='Notify'><arg type='s'/><arg type='u'/><arg type='s'/><arg type='s'/><arg type='s'/>"
    "  <arg type='as'/><arg type='a{sv}'/><arg type='i'/><arg type='u' direction='out'/></method>"
    " <method name='CloseNotification'><arg type='u'/></method>"
    " <method name='GetServerInformation'><arg type='s' direction='out'/><arg type='s' direction='out'/>"
    "  <arg type='s' direction='out'/><arg type='s' direction='out'/></method>"
    "</interface>"
    "<interface name='org.a11y.Bus'>"
    " <method name='GetAddress'><arg type='s' direction='out'/></method>"
    "</interface>"
    "<interface name='org.a11y.atspi.Socket'>"
    " <method name='Embed'><arg type='(so)' direction='in'/><arg type='(so)' direction='out'/></method>"
    "</interface>"
    "<interface name='org.freedesktop.portal.Camera'>"
    " <method name='AccessCamera'><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
    " <method name='OpenPipeWireRemote'><arg type='a{sv}' direction='in'/><arg type='h' direction='out'/></method>"
    " <property name='IsCameraPresent' type='b' access='read'/>"
    "</interface>"
    "<interface name='org.freedesktop.portal.ScreenCast'>"
    " <method name='CreateSession'><arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
    " <method name='SelectSources'><arg type='o' direction='in'/><arg type='a{sv}' direction='in'/>"
    "  <arg type='o' direction='out'/></method>"
    " <method name='Start'><arg type='o' direction='in'/><arg type='s' direction='in'/>"
    "  <arg type='a{sv}' direction='in'/><arg type='o' direction='out'/></method>"
    " <method name='OpenPipeWireRemote'><arg type='o' direction='in'/><arg type='a{sv}' direction='in'/>"
    "  <arg type='h' direction='out'/></method>"
    " <property name='AvailableSourceTypes' type='u' access='read'/>"
    " <property name='AvailableCursorModes' type='u' access='read'/>"
    " <property name='version' type='u' access='read'/>"
    "</interface>"
    "<interface name='org.freedesktop.portal.Settings'>"
    " <method name='ReadOne'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "  <arg type='v' direction='out'/></method>"
    " <method name='Read'><arg type='s' direction='in'/><arg type='s' direction='in'/>"
    "  <arg type='v' direction='out'/></method>"
    "</interface>"
    "<interface name='org.freedesktop.portal.Session'>"
    " <method name='Close'/>"
    "</interface>"
    "</node>";

/* The stand-ins' connection, their main loop's context and its thread. */
static GDBusConnection *g_srv;
static GMainContext    *g_ctx;
static GMainLoop       *g_loop;
static GThread         *g_thread;
static GDBusNodeInfo   *g_node;

/* What the stand-ins were asked and are to answer, under g_mu. */
static GMutex g_mu;
static struct {
    /* the notification server */
    const char *const *caps;
    guint32   next_id;
    int       n_notify, n_close;
    GVariant *last_notify;
    guint32   last_closed;
    /* the accessibility registry */
    int       n_embed;
    char      plug_name[128], plug_path[128];
    /* the portal: the answer to every request (0 granted, 1 refused, 2
     * dismissed), what it was asked, and its settings */
    guint32   response;
    int       n_access, n_remote, n_create, n_select, n_start, n_session_close;
    /* The PipeWire the remote is a connection to, and the node Start names
     * (the stream group's); "" and 0 otherwise, for /dev/null and node 42. */
    char      pw_socket[108];
    guint32   node;
    GVariant *last_select;
    int       settings_v1;          /* answers as version 1 does: no ReadOne */
    struct { const char *ns, *key; GVariant *v; } set[8];
    int       n_set;
} S;

static guint g_session_reg;

static GDBusInterfaceInfo *iface_info(const char *name) {
    return g_dbus_node_info_lookup_interface(g_node, name);
}

/* A request's or a session's object path, as the portal makes it from the
 * caller's unique name (":1.42" as "1_42") and the caller's token. */
static void portal_path(char *out, size_t cap, const char *kind, const char *sender, const char *token) {
    char s[128];
    int n = 0;
    for (const char *p = sender; *p && n < (int)sizeof s - 1; p++)
        if (*p != ':') s[n++] = *p == '.' ? '_' : *p;
    s[n] = 0;
    snprintf(out, cap, P_PATH "/%s/%s/%s", kind, s, token);
}

static void session_call(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                         const char *method, GVariant *args, GDBusMethodInvocation *inv, gpointer ud) {
    (void)c; (void)sender; (void)path; (void)iface; (void)method; (void)args; (void)ud;
    g_mutex_lock(&g_mu);
    S.n_session_close++;
    g_mutex_unlock(&g_mu);
    g_dbus_method_invocation_return_value(inv, NULL);
    if (g_session_reg) { g_dbus_connection_unregister_object(g_srv, g_session_reg); g_session_reg = 0; }
}
static const GDBusInterfaceVTable SESSION_VT = { session_call, NULL, NULL, { 0 } };

/* A request answered: its path, and then its Response, as a portal sends them
 * once the person has answered the system's dialog. */
static void portal_request(GDBusConnection *c, const char *sender, GVariant *opts, GDBusMethodInvocation *inv,
                           const char *method) {
    const char *token = "t";
    g_variant_lookup(opts, "handle_token", "&s", &token);
    char rp[256];
    portal_path(rp, sizeof rp, "request", sender, token);
    g_mutex_lock(&g_mu);
    guint32 code = S.response;
    g_mutex_unlock(&g_mu);
    GVariantBuilder res;
    g_variant_builder_init(&res, G_VARIANT_TYPE("a{sv}"));
    if (!strcmp(method, "CreateSession") && code == 0) {
        const char *st = "s";
        g_variant_lookup(opts, "session_handle_token", "&s", &st);
        char sp[256];
        portal_path(sp, sizeof sp, "session", sender, st);
        g_variant_builder_add(&res, "{sv}", "session_handle", g_variant_new_string(sp));
        if (g_session_reg) g_dbus_connection_unregister_object(g_srv, g_session_reg);
        g_session_reg = g_dbus_connection_register_object(g_srv, sp, iface_info("org.freedesktop.portal.Session"),
                                                          &SESSION_VT, NULL, NULL, NULL);
    }
    if (!strcmp(method, "Start") && code == 0) {
        GVariantBuilder streams, props;
        g_variant_builder_init(&streams, G_VARIANT_TYPE("a(ua{sv})"));
        g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
        g_variant_builder_add(&props, "{sv}", "size", g_variant_new("(ii)", 1920, 1080));
        g_mutex_lock(&g_mu);
        guint32 node = S.node ? S.node : 42u;
        g_mutex_unlock(&g_mu);
        g_variant_builder_add(&streams, "(u@a{sv})", node, g_variant_builder_end(&props));
        g_variant_builder_add(&res, "{sv}", "streams", g_variant_builder_end(&streams));
    }
    g_dbus_method_invocation_return_value(inv, g_variant_new("(o)", rp));
    g_dbus_connection_emit_signal(c, sender, rp, "org.freedesktop.portal.Request", "Response",
                                  g_variant_new("(u@a{sv})", code, g_variant_builder_end(&res)), NULL);
}

/* A socket connected to the PipeWire listening at `path`, or -1. */
static int pw_socket_connect(const char *path) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (!path[0] || strlen(path) >= sizeof a.sun_path) return -1;
    memcpy(a.sun_path, path, strlen(path));
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    return fd;
}

/* OpenPipeWireRemote: a descriptor, as the portal hands the remote over. In
 * the stream group it is a real connection to the private PipeWire, as the
 * portal's is to the desktop's; elsewhere what it is matters to no check, and
 * it is /dev/null. */
static void portal_remote(GDBusMethodInvocation *inv) {
    char path[sizeof S.pw_socket];
    g_mutex_lock(&g_mu);
    snprintf(path, sizeof path, "%s", S.pw_socket);
    g_mutex_unlock(&g_mu);
    int fd = path[0] ? pw_socket_connect(path) : open("/dev/null", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.portal.Error.Failed", "no PipeWire");
        return;
    }
    GUnixFDList *fds = g_unix_fd_list_new();
    gint idx = g_unix_fd_list_append(fds, fd, NULL);
    close(fd);
    g_dbus_method_invocation_return_value_with_unix_fd_list(inv, g_variant_new("(h)", idx), fds);
    g_object_unref(fds);
}

static GVariant *setting(const char *ns, const char *key) {
    for (int i = 0; i < S.n_set; i++)
        if (!strcmp(S.set[i].ns, ns) && !strcmp(S.set[i].key, key)) return S.set[i].v;
    return NULL;
}

static void on_call(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                    const char *method, GVariant *args, GDBusMethodInvocation *inv, gpointer ud) {
    (void)path; (void)ud;
    if (!strcmp(iface, N_NAME)) {
        if (!strcmp(method, "GetCapabilities")) {
            g_mutex_lock(&g_mu);
            GVariant *caps = g_variant_new_strv(S.caps, -1);
            g_mutex_unlock(&g_mu);
            g_dbus_method_invocation_return_value(inv, g_variant_new("(@as)", caps));
        } else if (!strcmp(method, "Notify")) {
            guint32 replaces;
            g_variant_get_child(args, 1, "u", &replaces);
            g_mutex_lock(&g_mu);
            guint32 id = replaces ? replaces : ++S.next_id;
            if (S.last_notify) g_variant_unref(S.last_notify);
            S.last_notify = g_variant_ref(args);
            S.n_notify++;
            g_mutex_unlock(&g_mu);
            g_dbus_method_invocation_return_value(inv, g_variant_new("(u)", id));
        } else if (!strcmp(method, "CloseNotification")) {
            g_mutex_lock(&g_mu);
            g_variant_get(args, "(u)", &S.last_closed);
            S.n_close++;
            g_mutex_unlock(&g_mu);
            g_dbus_method_invocation_return_value(inv, NULL);
        } else {
            g_dbus_method_invocation_return_value(inv, g_variant_new("(ssss)", "stand-in", "test", "1", "1.2"));
        }
        return;
    }
    if (!strcmp(iface, "org.a11y.Bus")) {
        /* The accessibility bus is this session bus: what matters to the
         * provider is that it is the one the bus launcher names. */
        char *addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL, NULL);
        g_dbus_method_invocation_return_value(inv, g_variant_new("(s)", addr ? addr : ""));
        g_free(addr);
        return;
    }
    if (!strcmp(iface, "org.a11y.atspi.Socket")) {
        const char *name, *p;
        g_variant_get(args, "((&s&o))", &name, &p);
        g_mutex_lock(&g_mu);
        snprintf(S.plug_name, sizeof S.plug_name, "%s", name);
        snprintf(S.plug_path, sizeof S.plug_path, "%s", p);
        S.n_embed++;
        g_mutex_unlock(&g_mu);
        g_dbus_method_invocation_return_value(
            inv, g_variant_new("((so))", g_dbus_connection_get_unique_name(c), A_ROOT));
        return;
    }
    if (!strcmp(iface, "org.freedesktop.portal.Camera")) {
        if (!strcmp(method, "AccessCamera")) {
            g_mutex_lock(&g_mu); S.n_access++; g_mutex_unlock(&g_mu);
            GVariant *opts = g_variant_get_child_value(args, 0);
            portal_request(c, sender, opts, inv, method);
            g_variant_unref(opts);
        } else {
            g_mutex_lock(&g_mu); S.n_remote++; g_mutex_unlock(&g_mu);
            portal_remote(inv);
        }
        return;
    }
    if (!strcmp(iface, "org.freedesktop.portal.ScreenCast")) {
        if (!strcmp(method, "OpenPipeWireRemote")) {
            g_mutex_lock(&g_mu); S.n_remote++; g_mutex_unlock(&g_mu);
            portal_remote(inv);
            return;
        }
        GVariant *opts = g_variant_get_child_value(args, g_variant_n_children(args) - 1);
        g_mutex_lock(&g_mu);
        if (!strcmp(method, "CreateSession")) S.n_create++;
        else if (!strcmp(method, "Start")) S.n_start++;
        else {
            S.n_select++;
            if (S.last_select) g_variant_unref(S.last_select);
            S.last_select = g_variant_ref(opts);
        }
        g_mutex_unlock(&g_mu);
        portal_request(c, sender, opts, inv, method);
        g_variant_unref(opts);
        return;
    }
    if (!strcmp(iface, "org.freedesktop.portal.Settings")) {
        const char *ns, *key;
        g_variant_get(args, "(&s&s)", &ns, &key);
        g_mutex_lock(&g_mu);
        int v1 = S.settings_v1;
        GVariant *v = setting(ns, key);
        if (v) g_variant_ref(v);
        g_mutex_unlock(&g_mu);
        if (!strcmp(method, "ReadOne") && v1) {
            g_dbus_method_invocation_return_error(inv, G_DBUS_ERROR, G_DBUS_ERROR_UNKNOWN_METHOD, "version 1");
        } else if (!v) {
            g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.portal.Error.NotFound", "not set");
        } else if (!strcmp(method, "ReadOne")) {
            g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", v));
        } else {
            /* Read's answer is the value wrapped once more. */
            g_dbus_method_invocation_return_value(inv, g_variant_new("(v)", g_variant_new_variant(v)));
        }
        if (v) g_variant_unref(v);
        return;
    }
    g_dbus_method_invocation_return_dbus_error(inv, "org.freedesktop.DBus.Error.UnknownMethod", method);
}

static GVariant *on_get(GDBusConnection *c, const char *sender, const char *path, const char *iface,
                        const char *name, GError **err, gpointer ud) {
    (void)c; (void)sender; (void)path; (void)iface; (void)err; (void)ud;
    if (!strcmp(name, "IsCameraPresent")) return g_variant_new_boolean(TRUE);
    if (!strcmp(name, "AvailableSourceTypes")) return g_variant_new_uint32(3);   /* monitor | window */
    if (!strcmp(name, "AvailableCursorModes")) return g_variant_new_uint32(7);   /* hidden | embedded | metadata */
    return g_variant_new_uint32(4);                                              /* version */
}

static const GDBusInterfaceVTable VT = { on_call, on_get, NULL, { 0 } };

static gpointer serve(gpointer unused) {
    (void)unused;
    g_main_context_push_thread_default(g_ctx);
    g_main_loop_run(g_loop);
    g_main_context_pop_thread_default(g_ctx);
    return NULL;
}

/* Every stand-in object registered, its calls answered on the services'
 * thread; no name owned yet. 0 without a session bus. */
static int services_start(void) {
    char *addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!addr) return 0;
    g_srv = g_dbus_connection_new_for_address_sync(
        addr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, NULL);
    g_free(addr);
    if (!g_srv) return 0;
    g_dbus_connection_set_exit_on_close(g_srv, FALSE);
    g_node = g_dbus_node_info_new_for_xml(XML, NULL);
    g_ctx = g_main_context_new();
    g_loop = g_main_loop_new(g_ctx, FALSE);
    g_main_context_push_thread_default(g_ctx);
    static const struct { const char *path, *iface; } OBJ[] = {
        { N_PATH, N_NAME },
        { "/org/a11y/bus", "org.a11y.Bus" },
        { A_ROOT, "org.a11y.atspi.Socket" },
        { P_PATH, "org.freedesktop.portal.Camera" },
        { P_PATH, "org.freedesktop.portal.ScreenCast" },
        { P_PATH, "org.freedesktop.portal.Settings" },
    };
    for (size_t i = 0; i < sizeof OBJ / sizeof OBJ[0]; i++)
        if (!g_dbus_connection_register_object(g_srv, OBJ[i].path, iface_info(OBJ[i].iface), &VT, NULL, NULL, NULL))
            return 0;
    g_main_context_pop_thread_default(g_ctx);
    g_thread = g_thread_new("stand-ins", serve, NULL);
    return 1;
}

static void services_stop(void) {
    g_main_loop_quit(g_loop);
    g_thread_join(g_thread);
    g_dbus_connection_close_sync(g_srv, NULL, NULL);
    g_object_unref(g_srv);
    g_main_loop_unref(g_loop);
    g_main_context_unref(g_ctx);
    g_dbus_node_info_unref(g_node);
}

static GVariant *bus_call(const char *dest, const char *path, const char *iface, const char *method,
                          GVariant *args, const char *reply) {
    return g_dbus_connection_call_sync(g_srv, dest, path, iface, method, args,
                                       reply ? G_VARIANT_TYPE(reply) : NULL, G_DBUS_CALL_FLAGS_NONE,
                                       WAIT_MS(5000), NULL, NULL);
}

/* `name` on the bus as a stand-in's, or gone from it. Owning one that is
 * already there means this is not a private bus: the program says so. */
static int own(const char *name) {
    GVariant *r = bus_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "RequestName",
                           g_variant_new("(su)", name, 4u /* DO_NOT_QUEUE */), "(u)");
    guint32 got = 0;
    if (r) { g_variant_get(r, "(u)", &got); g_variant_unref(r); }
    if (got != 1)
        printf("  %s is already on this session bus: run under dbus-run-session, whose bus is private\n", name);
    return got == 1;
}

static void disown(const char *name) {
    GVariant *r = bus_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "ReleaseName",
                           g_variant_new("(s)", name), "(u)");
    if (r) g_variant_unref(r);
}

/* Whether the bus would start a real service for `name` when asked: a check
 * that wants it absent cannot be made then, and says so. */
static int activatable(const char *name) {
    GVariant *r = bus_call("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                           "ListActivatableNames", NULL, "(as)");
    int found = 0;
    if (r) {
        GVariantIter *it;
        const char *n;
        g_variant_get(r, "(as)", &it);
        while (g_variant_iter_loop(it, "&s", &n)) if (!strcmp(n, name)) found = 1;
        g_variant_iter_free(it);
        g_variant_unref(r);
    }
    return found;
}

/* ---- SDL's events, which the platform posts ---------------------------------- */

/* Takes every queued event and keeps the first of the platform's with `code`. */
static int take_plat_event(int code, SDL_Event *out) {
    SDL_Event e;
    int found = 0;
    while (SDL_PollEvent(&e))
        if (!found && e.type == oc_plat_event_type() && e.user.code == code) { *out = e; found = 1; }
    return found;
}

/* ---- platform -------------------------------------------------------------------- */

static char g_tmp[512];

static const char *tmp_path(char *out, size_t cap, const char *rel) { return oc_test_path(out, cap, g_tmp, rel); }

static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *buf = malloc(1 << 20);
    size_t n = buf ? fread(buf, 1, (1 << 20) - 1, f) : 0;
    fclose(f);
    if (buf) buf[n] = 0;
    if (len) *len = n;
    return buf;
}

/* The value of `key` in a .desktop file's text, into `out`; 0 when absent. */
static int desktop_value(const char *text, const char *key, char *out, size_t cap) {
    size_t kl = strlen(key);
    for (const char *p = text; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t vl = strcspn(v, "\n");
            snprintf(out, cap, "%.*s", (int)vl, v);
            return 1;
        }
    }
    out[0] = 0;
    return 0;
}

/* This program run again with `a`, `b`, `c` as its arguments: its exit code,
 * or -1. */
static int run_self(const char *a, const char *b, const char *c) {
    char exe[1024];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    if (n <= 0) return -1;
    exe[n] = 0;
    char *argv[] = { exe, (char *)a, (char *)b, (char *)c, NULL };
    pid_t pid;
    if (posix_spawn(&pid, exe, NULL, NULL, argv, environ) != 0) return -1;
    int st;
    while (waitpid(pid, &st, 0) < 0) if (errno != EINTR) return -1;
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

static void test_data_dir(void) {
    char p[700], want[700];
    struct stat st;
    setenv("XDG_DATA_HOME", tmp_path(want, sizeof want, "data"), 1);
    CHECK(oc_plat_data_dir(p, sizeof p) == 1);
    CHECK(!strcmp(p, tmp_path(want, sizeof want, "data/openchime")));
    CHECK(stat(p, &st) == 0 && S_ISDIR(st.st_mode) && (st.st_mode & 0777) == 0700);
    /* A relative XDG_DATA_HOME is ignored, as the specification says, for
     * $HOME/.local/share. */
    setenv("XDG_DATA_HOME", "relative/data", 1);
    CHECK(oc_plat_data_dir(p, sizeof p) == 1);
    CHECK(!strcmp(p, tmp_path(want, sizeof want, "home/.local/share/openchime")));
    CHECK(stat(p, &st) == 0 && S_ISDIR(st.st_mode));
    setenv("XDG_DATA_HOME", tmp_path(want, sizeof want, "data"), 1);
}

static void test_ring_map(void) {
    char path[700];
    tmp_path(path, sizeof path, "ring");
    /* Written by a process that ends without unmapping or syncing: the bytes
     * are in the file because the mapping is the file's. */
    CHECK(run_self("--ring", path, "ring-bytes") == 0);
    size_t n = 0;
    char *d = slurp(path, &n);
    CHECK(d && n == 8192);
    CHECK(d && n == 8192 && !memcmp(d + 100, "ring-bytes", 10));
    free(d);
    /* And here: zeroed when mapped, written, and on disk after unmapping. */
    uint8_t *r = oc_plat_ring_map(path, 4096);
    CHECK(r != NULL);
    if (!r) return;
    int zero = 1;
    for (int i = 0; i < 4096; i++) if (r[i]) zero = 0;
    CHECK(zero);
    memcpy(r + 4000, "tail", 4);
    oc_plat_ring_unmap(r, 4096);
    d = slurp(path, &n);
    CHECK(d && n == 4096 && !memcmp(d + 4000, "tail", 4));
    free(d);
}

static void test_handoff(SDL_Window *w) {
    char sock[700];
    tmp_path(sock, sizeof sock, "run/openchime.sock");
    oc_plat_window_attach(w);
    struct stat st;
    CHECK(stat(sock, &st) == 0 && S_ISSOCK(st.st_mode));
    /* A second launch hands its URL over and is told it was taken. */
    CHECK(run_self("--handoff", "url", "openchime://example.org/c/5") == 0);
    SDL_Event e;
    int got = WAIT_UNTIL(take_plat_event(OC_PLAT_EV_HANDOFF, &e), 5000);
    CHECK(got);
    if (got) {
        CHECK(e.user.data1 && !strcmp(e.user.data1, "url\nopenchime://example.org/c/5"));
        free(e.user.data1);
    }
    /* A kind the platform does not carry is not taken (the URL above shows a
     * handoff that is taken is seen). */
    CHECK(run_self("--handoff", "shell", "anything") != 0);
    /* Once the instance has stopped listening, a second launch finds nobody,
     * and the socket file is gone. */
    oc_plat_quit();
    CHECK(stat(sock, &st) != 0);
    CHECK(run_self("--handoff", "url", "openchime://example.org/c/6") != 0);
}

static void test_autostart(void) {
    char p[700], exe[1024], want[1100], v[1100];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = 0;
    snprintf(want, sizeof want, "\"%s\"", exe);
    tmp_path(p, sizeof p, "config/autostart/openchime.desktop");
    CHECK(oc_plat_autostart_get() == 0);
    oc_plat_autostart_set(1);
    char *d = slurp(p, NULL);
    CHECK(d != NULL);
    if (d) {
        /* contract: the Desktop Entry specification's group header */
        CHECK(!strncmp(d, "[Desktop Entry]\n", 16));
        CHECK(desktop_value(d, "Type", v, sizeof v) && !strcmp(v, "Application"));
        CHECK(desktop_value(d, "Exec", v, sizeof v) && !strcmp(v, want));
    }
    free(d);
    CHECK(oc_plat_autostart_get() == 1);
    /* A desktop's own settings switch an entry off inside the file. */
    FILE *f = fopen(p, "ab");
    if (f) { fputs("Hidden=true\n", f); fclose(f); }
    CHECK(oc_plat_autostart_get() == 0);
    oc_plat_autostart_set(1);
    CHECK(oc_plat_autostart_get() == 1);
    oc_plat_autostart_set(0);
    struct stat st;
    CHECK(stat(p, &st) != 0);
    CHECK(oc_plat_autostart_get() == 0);
}

/* The line count of `needle` in `hay`. */
static int count_lines(const char *hay, const char *needle) {
    int c = 0;
    size_t nl = strlen(needle);
    for (const char *p = hay; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL)
        if (!strncmp(p, needle, nl) && (p[nl] == '\n' || !p[nl])) c++;
    return c;
}

static void test_url_scheme(void) {
    char list[700], entry[700], exe[1024], want[1100], v[1100];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = 0;
    snprintf(want, sizeof want, "\"%s\" %%u", exe);
    tmp_path(list, sizeof list, "config/mimeapps.list");
    tmp_path(entry, sizeof entry, "data/applications/openchime-url.desktop");
    /* What is there already is kept, whichever group it is in. */
    char cfg[700];
    mkdir(tmp_path(cfg, sizeof cfg, "config"), 0700);
    FILE *f = fopen(list, "wb");
    if (f) {
        fputs("[Added Associations]\ntext/plain=editor.desktop;\n\n[Default Applications]\ntext/html=browser.desktop\n", f);
        fclose(f);
    }
    oc_plat_url_scheme_register("openchime");
    char *d = slurp(entry, NULL);
    CHECK(d != NULL);
    if (d) {
        CHECK(desktop_value(d, "Exec", v, sizeof v) && !strcmp(v, want));
        CHECK(desktop_value(d, "MimeType", v, sizeof v) && !strcmp(v, "x-scheme-handler/openchime;"));
        CHECK(desktop_value(d, "NoDisplay", v, sizeof v) && !strcmp(v, "true"));
    }
    free(d);
    d = slurp(list, NULL);
    CHECK(d != NULL);
    if (d) {
        CHECK(count_lines(d, "x-scheme-handler/openchime=openchime-url.desktop") == 1);
        CHECK(count_lines(d, "text/plain=editor.desktop;") == 1);
        CHECK(count_lines(d, "text/html=browser.desktop") == 1);
        /* In the default applications' group, not the added associations'. */
        /* contract: the MIME applications specification's group name */
        const char *def = strstr(d, "[Default Applications]");
        const char *key = strstr(d, "x-scheme-handler/openchime=");
        CHECK(def && key && key > def);
    }
    free(d);
    /* Registered again on every start: the list is the same after. */
    oc_plat_url_scheme_register("openchime");
    d = slurp(list, NULL);
    CHECK(d && count_lines(d, "x-scheme-handler/openchime=openchime-url.desktop") == 1);
    free(d);
}

static void group_platform(SDL_Window *w) {
    char p[700];
    setenv("HOME", tmp_path(p, sizeof p, "home"), 1);
    setenv("XDG_CONFIG_HOME", tmp_path(p, sizeof p, "config"), 1);
    mkdir(tmp_path(p, sizeof p, "run"), 0700);
    setenv("XDG_RUNTIME_DIR", p, 1);
    test_data_dir();
    test_ring_map();
    if (w) test_handoff(w);
    else CHECK(!"a window to attach the handoff to");
    test_autostart();
    test_url_scheme();
}

/* ---- image ------------------------------------------------------------------------- */

/* The four pixels every lossless format is given, straight RGBA, and what the
 * platform must answer for them: premultiplied BGRA. */
static const uint8_t RGBA4[16] = { 255, 0, 0, 255,   0, 255, 0, 128,   0, 0, 255, 0,   10, 20, 30, 255 };
static const uint8_t BGRA4[16] = { 0, 0, 255, 255,   0, 128, 0, 128,   0, 0, 0, 0,     30, 20, 10, 255 };

static int pixels_near(const uint8_t *a, const uint8_t *b, size_t n, int tol) {
    for (size_t i = 0; i < n; i++) if (abs((int)a[i] - (int)b[i]) > tol) return 0;
    return 1;
}

static void check_decode(const char *fmt, const uint8_t *b, size_t n, int w, int h, const uint8_t *want, int tol) {
    int ow = 0, oh = 0;
    atomic_store(&g_largest, 0);
    uint8_t *px = oc_plat_image_decode(b, n, &ow, &oh);
    if (!px) printf("  %s: not decoded\n", fmt);
    CHECK(px != NULL);
    if (!px) return;
    CHECK(ow == w && oh == h);
    /* The pixel buffer is among what was allocated: what the cap's check
     * below would see if a refused header were allocated for. */
    CHECK(atomic_load(&g_largest) >= (size_t)w * (size_t)h * 4);
    if (ow == w && oh == h && !pixels_near(px, want, (size_t)w * h * 4, tol)) {
        printf("  %s: pixels differ\n", fmt);
        CHECK(!"pixels as given");
    }
    free(px);
}

static uint8_t *png_of(const uint8_t *rgba, int w, int h, size_t *len) {
    spng_ctx *ctx = spng_ctx_new(SPNG_CTX_ENCODER);
    if (!ctx) return NULL;
    spng_set_option(ctx, SPNG_ENCODE_TO_BUFFER, 1);
    struct spng_ihdr ihdr = { (uint32_t)w, (uint32_t)h, 8, SPNG_COLOR_TYPE_TRUECOLOR_ALPHA, 0, 0, 0 };
    spng_set_ihdr(ctx, &ihdr);
    int err = spng_encode_image(ctx, rgba, (size_t)w * h * 4, SPNG_FMT_PNG, SPNG_ENCODE_FINALIZE);
    uint8_t *out = err ? NULL : spng_get_png_buffer(ctx, len, &err);
    spng_ctx_free(ctx);
    return err ? NULL : out;
}

static uint8_t *jpeg_of(const uint8_t *rgb, int w, int h, size_t *len) {
    struct jpeg_compress_struct c;
    struct jpeg_error_mgr je;
    unsigned char *out = NULL;
    unsigned long n = 0;
    c.err = jpeg_std_error(&je);
    jpeg_create_compress(&c);
    jpeg_mem_dest(&c, &out, &n);
    c.image_width = (JDIMENSION)w;
    c.image_height = (JDIMENSION)h;
    c.input_components = 3;
    c.in_color_space = JCS_RGB;
    jpeg_set_defaults(&c);
    jpeg_set_quality(&c, 100, TRUE);
    jpeg_start_compress(&c, TRUE);
    while (c.next_scanline < c.image_height) {
        JSAMPROW row = (JSAMPROW)(rgb + (size_t)c.next_scanline * w * 3);
        jpeg_write_scanlines(&c, &row, 1);
    }
    jpeg_finish_compress(&c);
    jpeg_destroy_compress(&c);
    *len = n;
    return out;
}

/* A 2x2 GIF: a four-colour palette whose last entry is transparent (a
 * graphic control extension), and a raster of a clear code before every pixel,
 * so every code is three bits and no table entry is ever made. */
static size_t gif_of(uint8_t *out) {
    static const uint8_t head[] = {
        'G', 'I', 'F', '8', '9', 'a', 2, 0, 2, 0, 0xF1, 0, 0,
        0xFF, 0, 0,   0, 0xFF, 0,   0, 0, 0xFF,   0, 0, 0,
        0x21, 0xF9, 4, 0x01, 0, 0, 3, 0,
        0x2C, 0, 0, 0, 0, 2, 0, 2, 0, 0,
        2,
    };
    size_t o = sizeof head;
    memcpy(out, head, o);
    static const int codes[] = { 4, 0, 4, 1, 4, 2, 4, 3, 5 };
    uint8_t data[8] = { 0 };
    int bit = 0;
    for (size_t i = 0; i < sizeof codes / sizeof codes[0]; i++)
        for (int b = 0; b < 3; b++, bit++)
            if (codes[i] >> b & 1) data[bit / 8] |= (uint8_t)(1 << (bit % 8));
    int nb = (bit + 7) / 8;
    out[o++] = (uint8_t)nb;
    memcpy(out + o, data, (size_t)nb);
    o += (size_t)nb;
    out[o++] = 0;
    out[o++] = 0x3B;
    return o;
}

static void le16(uint8_t *p, unsigned v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void le32(uint8_t *p, uint32_t v) { le16(p, v & 0xFFFF); le16(p + 2, v >> 16); }
static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }

/* A 24-bit BMP's 54-byte header, w x h, with `body` bytes of pixels after it. */
static void bmp_header(uint8_t *p, int w, int h, uint32_t body) {
    memset(p, 0, 54);
    p[0] = 'B'; p[1] = 'M';
    le32(p + 2, 54 + body);
    le32(p + 10, 54);
    le32(p + 14, 40);
    le32(p + 18, (uint32_t)w);
    le32(p + 22, (uint32_t)h);
    le16(p + 26, 1);
    le16(p + 28, 24);
    le32(p + 34, body);
}

/* The opaque pixels of RGBA4 as a bottom-up 24-bit BMP. */
static size_t bmp_of(uint8_t *out) {
    bmp_header(out, 2, 2, 16);
    uint8_t *row = out + 54;
    for (int r = 1; r >= 0; r--, row += 8) {          /* bottom row first; each padded to 8 */
        memset(row, 0, 8);
        for (int c = 0; c < 2; c++) {
            const uint8_t *s = RGBA4 + (r * 2 + c) * 4;
            row[c * 3] = s[2]; row[c * 3 + 1] = s[1]; row[c * 3 + 2] = s[0];
        }
    }
    return 54 + 16;
}

static uint32_t crc32_of(const uint8_t *p, size_t n) {
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= p[i];
        for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c ^ 0xFFFFFFFFu;
}

/* A PNG's signature and IHDR, w x h, and nothing after. */
static size_t png_header(uint8_t *p, uint32_t w, uint32_t h) {
    memcpy(p, "\x89PNG\r\n\x1a\n", 8);
    be32(p + 8, 13);
    memcpy(p + 12, "IHDR", 4);
    be32(p + 16, w);
    be32(p + 20, h);
    p[24] = 8; p[25] = 6; p[26] = 0; p[27] = 0; p[28] = 0;
    be32(p + 29, crc32_of(p + 12, 17));
    return 33;
}

static void group_image(void) {
    size_t n = 0;
    uint8_t *b = png_of(RGBA4, 2, 2, &n);
    CHECK(b != NULL);
    if (b) check_decode("png", b, n, 2, 2, BGRA4, 0);
    free(b);

    /* WebP lossless; libwebp premultiplies itself, rounding its own way. */
    uint8_t *wb = NULL;
    n = WebPEncodeLosslessRGBA(RGBA4, 2, 2, 8, &wb);
    CHECK(n > 0);
    if (n) check_decode("webp", wb, n, 2, 2, BGRA4, 1);
    WebPFree(wb);

    /* GIF has a palette and one transparent entry: red, green and blue
     * opaque, and the fourth transparent. */
    static const uint8_t GIF_BGRA[16] = { 0, 0, 255, 255,   0, 255, 0, 255,   255, 0, 0, 255,   0, 0, 0, 0 };
    uint8_t gif[128];
    n = gif_of(gif);
    check_decode("gif", gif, n, 2, 2, GIF_BGRA, 0);

    /* BMP has no alpha: the opaque pixels, and the others as opaque. */
    uint8_t bmp[128], want[16];
    n = bmp_of(bmp);
    for (int i = 0; i < 4; i++) {
        want[i * 4] = RGBA4[i * 4 + 2]; want[i * 4 + 1] = RGBA4[i * 4 + 1];
        want[i * 4 + 2] = RGBA4[i * 4]; want[i * 4 + 3] = 255;
    }
    check_decode("bmp", bmp, n, 2, 2, want, 0);

    /* JPEG is lossy: one colour over whole blocks, near what was given. */
    uint8_t rgb[16 * 16 * 3], jwant[16 * 16 * 4];
    for (int i = 0; i < 16 * 16; i++) {
        rgb[i * 3] = 200; rgb[i * 3 + 1] = 100; rgb[i * 3 + 2] = 50;
        jwant[i * 4] = 50; jwant[i * 4 + 1] = 100; jwant[i * 4 + 2] = 200; jwant[i * 4 + 3] = 255;
    }
    b = jpeg_of(rgb, 16, 16, &n);
    CHECK(b != NULL);
    if (b) check_decode("jpeg", b, n, 16, 16, jwant, 4);
    free(b);

    /* A header one row over the pixel cap is refused, and nothing the size
     * of its pixels is allocated: the cap is read before the decoder is
     * asked for the image. (check_decode showed the pixel buffer is seen.) */
    uint32_t cw = 10000, ch = OC_PREVIEW_MAX_PIXELS_DEFAULT / 10000 + 1;
    int w = 0, h = 0;
    uint8_t hdr[64];
    n = png_header(hdr, cw, ch);
    atomic_store(&g_largest, 0);
    CHECK(oc_plat_image_decode(hdr, n, &w, &h) == NULL);
    CHECK(atomic_load(&g_largest) < OC_PREVIEW_MAX_PIXELS_DEFAULT);
    bmp_header(hdr, (int)cw, (int)ch, cw * 3 * ch);
    atomic_store(&g_largest, 0);
    CHECK(oc_plat_image_decode(hdr, 54, &w, &h) == NULL);
    CHECK(atomic_load(&g_largest) < OC_PREVIEW_MAX_PIXELS_DEFAULT);
    /* Not an image at all. */
    CHECK(oc_plat_image_decode((const uint8_t *)"plain", 5, &w, &h) == NULL);
}

/* ---- notify ------------------------------------------------------------------------ */

static GMutex g_osn_mu;
static int    g_osn_n;
static char   g_osn_arg[256], g_osn_reply[256];

static void osn_cb(const char *arg, const char *reply) {
    g_mutex_lock(&g_osn_mu);
    snprintf(g_osn_arg, sizeof g_osn_arg, "%s", arg);
    snprintf(g_osn_reply, sizeof g_osn_reply, "%s", reply);
    g_osn_n++;
    g_mutex_unlock(&g_osn_mu);
}

static int osn_count(void) {
    g_mutex_lock(&g_osn_mu);
    int n = g_osn_n;
    g_mutex_unlock(&g_osn_mu);
    return n;
}

/* The arguments of the last Notify the server was sent. */
typedef struct {
    char     app[64], summary[64], body[128];
    guint32  replaces;
    char     actions[8][32];
    int      n_actions;
    char     sound[64], placeholder[64];
    int      suppress;
} notify_args;

static int last_notify(notify_args *a) {
    memset(a, 0, sizeof *a);
    g_mutex_lock(&g_mu);
    GVariant *v = S.last_notify ? g_variant_ref(S.last_notify) : NULL;
    g_mutex_unlock(&g_mu);
    if (!v) return 0;
    const char *app, *icon, *sum, *body;
    GVariantIter *acts;
    GVariant *hints;
    gint32 timeout;
    g_variant_get(v, "(&su&s&s&sas@a{sv}i)", &app, &a->replaces, &icon, &sum, &body, &acts, &hints, &timeout);
    snprintf(a->app, sizeof a->app, "%s", app);
    snprintf(a->summary, sizeof a->summary, "%s", sum);
    snprintf(a->body, sizeof a->body, "%s", body);
    const char *s;
    while (g_variant_iter_loop(acts, "&s", &s))
        if (a->n_actions < 8) snprintf(a->actions[a->n_actions++], 32, "%s", s);
    g_variant_iter_free(acts);
    if (g_variant_lookup(hints, "sound-name", "&s", &s)) snprintf(a->sound, sizeof a->sound, "%s", s);
    if (g_variant_lookup(hints, "x-kde-reply-placeholder-text", "&s", &s))
        snprintf(a->placeholder, sizeof a->placeholder, "%s", s);
    gboolean sup = FALSE;
    if (g_variant_lookup(hints, "suppress-sound", "b", &sup)) a->suppress = sup;
    g_variant_unref(hints);
    g_variant_unref(v);
    return 1;
}

static int close_count(void) {
    g_mutex_lock(&g_mu);
    int k = S.n_close;
    g_mutex_unlock(&g_mu);
    return k;
}

static void signal_server(const char *member, GVariant *args) {
    g_dbus_connection_emit_signal(g_srv, NULL, N_PATH, N_NAME, member, args, NULL);
}

static int acts_are(const notify_args *a, const char *const *want, int n) {
    if (a->n_actions != n) return 0;
    for (int i = 0; i < n; i++) if (strcmp(a->actions[i], want[i])) return 0;
    return 1;
}

static void group_notify(void) {
    static const char *const CAPS_ALL[] = { "actions", "body", "sound", "inline-reply", NULL };
    static const char *const CAPS_BASIC[] = { "actions", "body", NULL };
    static const char *const LABEL[] = { "Send", "Like" };
    static const char *const ARG[] = { "reply|w0|5", "react|w0|5|like" };
    oc_osn n = { "Alice", "hi", "#general", "c5", "w0", "openchime://example.org/c/5", OC_OSN_SOUND_IM,
                 "Reply", LABEL, ARG, 2 };

    /* No server: nothing to raise notifications with. */
    if (activatable(N_NAME)) {
        SKIP("the bus would start a real notification server, so none being there cannot be shown");
    } else {
        CHECK(oc_osn_init("openchime", "OpenChime", NULL, 0, osn_cb) == 0);
        CHECK(oc_osn_available() == 0);
        CHECK(oc_osn_show(&n) == OC_OSN_NOT_SHOWN);
    }

    g_mutex_lock(&g_mu);
    S.caps = CAPS_ALL;
    g_mutex_unlock(&g_mu);
    if (!own(N_NAME)) { CHECK(!"the notification server's name"); return; }
    CHECK(oc_osn_init("openchime", "OpenChime", NULL, 0, osn_cb) == 1);
    CHECK(oc_osn_available() == 1);
    CHECK(oc_osn_caps() == (OC_OSN_CAP_ACTIONS | OC_OSN_CAP_REPLY | OC_OSN_CAP_REPLACE | OC_OSN_CAP_WITHDRAW |
                            OC_OSN_CAP_SOUND));

    /* Notify's arguments: the application's name, the text with where it
     * came from under it, a click, the reply box carrying the first button,
     * the other button, and the freedesktop sound's name. */
    CHECK(oc_osn_show(&n) == OC_OSN_SHOWN);
    notify_args a;
    CHECK(last_notify(&a));
    CHECK(!strcmp(a.app, "OpenChime"));
    CHECK(a.replaces == 0);
    CHECK(!strcmp(a.summary, "Alice"));
    CHECK(!strcmp(a.body, "hi\n#general"));
    static const char *const ACTS[] = { "default", "", "inline-reply", "Reply", "b1", "Like" };
    CHECK(acts_are(&a, ACTS, 6));
    CHECK(!strcmp(a.sound, "message-new-instant"));
    CHECK(!strcmp(a.placeholder, "Reply"));
    guint32 id = 0;
    g_mutex_lock(&g_mu); id = S.next_id; g_mutex_unlock(&g_mu);

    /* A click, a button and a typed reply each reach the callback with what
     * the notification was raised with. */
    signal_server("ActionInvoked", g_variant_new("(us)", id, "default"));
    CHECK(WAIT_UNTIL(osn_count() == 1, 5000));
    g_mutex_lock(&g_osn_mu);
    CHECK(!strcmp(g_osn_arg, n.open_url) && !g_osn_reply[0]);
    g_mutex_unlock(&g_osn_mu);
    signal_server("ActionInvoked", g_variant_new("(us)", id, "b1"));
    CHECK(WAIT_UNTIL(osn_count() == 2, 5000));
    g_mutex_lock(&g_osn_mu);
    CHECK(!strcmp(g_osn_arg, ARG[1]) && !g_osn_reply[0]);
    g_mutex_unlock(&g_osn_mu);
    signal_server("NotificationReplied", g_variant_new("(us)", id, "typed"));
    CHECK(WAIT_UNTIL(osn_count() == 3, 5000));
    g_mutex_lock(&g_osn_mu);
    CHECK(!strcmp(g_osn_arg, ARG[0]) && !strcmp(g_osn_reply, "typed"));
    g_mutex_unlock(&g_osn_mu);

    /* A second for the same conversation replaces the first. */
    n.body = "again";
    CHECK(oc_osn_show(&n) == OC_OSN_SHOWN);
    CHECK(last_notify(&a) && a.replaces == id && !strcmp(a.body, "again\n#general"));

    /* Withdrawn: the server is asked to close it, and the next one for the
     * conversation is a new one. */
    CHECK(oc_osn_withdraw("c5", "w0") == 1);
    CHECK(WAIT_UNTIL(close_count() == 1, 5000));
    g_mutex_lock(&g_mu);
    CHECK(S.last_closed == id);
    g_mutex_unlock(&g_mu);
    CHECK(oc_osn_show(&n) == OC_OSN_SHOWN);
    CHECK(last_notify(&a) && a.replaces == 0);

    /* A silent one asks the server not to play anything. */
    oc_osn quiet = { "Bob", "psst", NULL, "c6", "w0", NULL, OC_OSN_SOUND_SILENT, NULL, NULL, NULL, 0 };
    CHECK(oc_osn_show(&quiet) == OC_OSN_SHOWN);
    CHECK(last_notify(&a) && a.suppress && !a.sound[0] && a.n_actions == 0 && !strcmp(a.body, "psst"));

    /* A server without inline replies: no reply box, the send button left
     * out with it, and no sound of the server's own. */
    oc_osn_done();
    g_mutex_lock(&g_mu);
    S.caps = CAPS_BASIC;
    g_mutex_unlock(&g_mu);
    CHECK(oc_osn_init("openchime", "OpenChime", NULL, 0, osn_cb) == 1);
    CHECK(oc_osn_caps() == (OC_OSN_CAP_ACTIONS | OC_OSN_CAP_REPLACE | OC_OSN_CAP_WITHDRAW));
    n.key = "c7";
    CHECK(oc_osn_show(&n) == OC_OSN_SHOWN_SILENT);
    static const char *const ACTS_BASIC[] = { "default", "", "b1", "Like" };
    CHECK(last_notify(&a) && acts_are(&a, ACTS_BASIC, 4) && !a.placeholder[0]);
    oc_osn_done();
    disown(N_NAME);
}

/* ---- a11y ------------------------------------------------------------------------- */

static char g_app[128];

static GVariant *app_call(const char *path, const char *iface, const char *method, GVariant *args, const char *reply) {
    return bus_call(g_app, path, iface, method, args, reply);
}

/* The children of `path`, their paths into `out`. */
static int children(const char *path, char out[][128], int cap) {
    GVariant *r = app_call(path, I_ACC, "GetChildren", NULL, "(a(so))");
    if (!r) return -1;
    GVariantIter *it;
    const char *name, *p;
    int n = 0;
    g_variant_get(r, "(a(so))", &it);
    while (g_variant_iter_loop(it, "(&s&o)", &name, &p))
        if (n < cap) snprintf(out[n++], 128, "%s", p);
    g_variant_iter_free(it);
    g_variant_unref(r);
    return n;
}

static int name_is(const char *path, const char *want) {
    GVariant *r = app_call(path, "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", I_ACC, "Name"), "(v)");
    if (!r) return 0;
    GVariant *v;
    g_variant_get(r, "(v)", &v);
    int ok = g_variant_is_of_type(v, G_VARIANT_TYPE_STRING) && !strcmp(g_variant_get_string(v, NULL), want);
    g_variant_unref(v);
    g_variant_unref(r);
    return ok;
}

static guint32 role_of(const char *path) {
    GVariant *r = app_call(path, I_ACC, "GetRole", NULL, "(u)");
    guint32 role = 0;
    if (r) { g_variant_get(r, "(u)", &role); g_variant_unref(r); }
    return role;
}

static int id_is(const char *path, const char *want) {
    GVariant *r = app_call(path, I_ACC, "GetAttributes", NULL, "(a{ss})");
    if (!r) return 0;
    GVariant *attrs = g_variant_get_child_value(r, 0);
    const char *id = NULL;
    int ok = g_variant_lookup(attrs, "id", "&s", &id) && !strcmp(id, want);
    g_variant_unref(attrs);
    g_variant_unref(r);
    return ok;
}

/* AT-SPI's numbers for the roles the provider gives (at-spi2-core's
 * AtspiRole). */
enum { ROLE_FRAME = 23, ROLE_LIST = 31, ROLE_LIST_ITEM = 32, ROLE_PUSH_BUTTON = 43, ROLE_APPLICATION = 75,
       ROLE_ENTRY = 79 };

static void group_a11y(SDL_Window *w) {
    if (!w) { CHECK(!"a window for the provider"); return; }
    if (!own(A_BUS) || !own(A_REG)) { CHECK(!"the accessibility bus's names"); return; }
    unsetenv("AT_SPI_BUS_ADDRESS");
    oc_a11y_init(w);
    CHECK(oc_a11y_available() == 1);
    /* The application handed its root to the registry. */
    g_mutex_lock(&g_mu);
    CHECK(S.n_embed == 1 && !strcmp(S.plug_path, A_ROOT));
    snprintf(g_app, sizeof g_app, "%s", S.plug_name);
    g_mutex_unlock(&g_mu);
    if (!g_app[0]) return;

    oc_acc_item it[4];
    memset(it, 0, sizeof it);
    it[0] = (oc_acc_item){ .kind = OC_ACC_CONVERSATION, .id = 5, .l = 0, .t = 0, .r = 200, .b = 30 };
    snprintf(it[0].name, sizeof it[0].name, "general");
    snprintf(it[0].aid, sizeof it[0].aid, "conv.5");
    it[1] = (oc_acc_item){ .kind = OC_ACC_MESSAGE, .id = 77, .l = 210, .t = 0, .r = 600, .b = 40 };
    snprintf(it[1].name, sizeof it[1].name, "Alice");
    snprintf(it[1].aid, sizeof it[1].aid, "msg.77");
    it[2] = (oc_acc_item){ .kind = OC_ACC_COMPOSER, .l = 210, .t = 400, .r = 560, .b = 440 };
    snprintf(it[2].name, sizeof it[2].name, "Message");
    snprintf(it[2].aid, sizeof it[2].aid, "composer");
    it[3] = (oc_acc_item){ .kind = OC_ACC_BUTTON, .l = 560, .t = 400, .r = 600, .b = 440,
                           .invoke = 0x0123456789ABCDEFull };
    snprintf(it[3].name, sizeof it[3].name, "Send");
    snprintf(it[3].aid, sizeof it[3].aid, "composer.send");
    /* "hé😀x" in UTF-16, the caret after the emoji's surrogate pair: unit
     * offset 4, which is code point 3. */
    static const uint16_t text[] = { 'h', 0xE9, 0xD83D, 0xDE00, 'x', 0 };
    oc_a11y_publish(it, 4, text, 4, 4);

    char kids[8][128], sub[8][128];
    CHECK(role_of(A_ROOT) == ROLE_APPLICATION);
    CHECK(children(A_ROOT, kids, 8) == 1);
    char win[128];
    snprintf(win, sizeof win, "%s", kids[0]);
    CHECK(role_of(win) == ROLE_FRAME);
    CHECK(id_is(win, "openchime"));
    /* The window: the conversations' list, the messages' list, then the
     * composer and the button. */
    CHECK(children(win, kids, 8) == 4);
    CHECK(name_is(kids[0], "Conversations") && role_of(kids[0]) == ROLE_LIST);
    CHECK(name_is(kids[1], "Messages") && role_of(kids[1]) == ROLE_LIST);
    CHECK(children(kids[0], sub, 8) == 1);
    CHECK(name_is(sub[0], "general") && role_of(sub[0]) == ROLE_LIST_ITEM && id_is(sub[0], "conv.5"));
    CHECK(children(kids[1], sub, 8) == 1);
    CHECK(name_is(sub[0], "Alice") && role_of(sub[0]) == ROLE_LIST_ITEM && id_is(sub[0], "msg.77"));
    const char *comp = kids[2], *send = kids[3];
    CHECK(name_is(comp, "Message") && role_of(comp) == ROLE_ENTRY && id_is(comp, "composer"));
    CHECK(name_is(send, "Send") && role_of(send) == ROLE_PUSH_BUTTON && id_is(send, "composer.send"));

    /* The composer's text, and its caret in code points. */
    GVariant *r = app_call(comp, "org.a11y.atspi.Text", "GetText", g_variant_new("(ii)", 0, -1), "(s)");
    const char *s = NULL;
    if (r) g_variant_get(r, "(&s)", &s);
    CHECK(s && !strcmp(s, "h\xC3\xA9\xF0\x9F\x98\x80x"));
    if (r) g_variant_unref(r);
    r = app_call(comp, "org.freedesktop.DBus.Properties", "Get", g_variant_new("(ss)", "org.a11y.atspi.Text", "CaretOffset"), "(v)");
    GVariant *v = NULL;
    if (r) g_variant_get(r, "(v)", &v);
    CHECK(v && g_variant_is_of_type(v, G_VARIANT_TYPE_INT32) && g_variant_get_int32(v) == 3);
    if (v) g_variant_unref(v);
    if (r) g_variant_unref(r);

    /* Pressing the button posts its token for the application to run. */
    r = app_call(send, "org.a11y.atspi.Action", "DoAction", g_variant_new("(i)", 0), "(b)");
    gboolean done = FALSE;
    if (r) { g_variant_get(r, "(b)", &done); g_variant_unref(r); }
    CHECK(done);
    SDL_Event e;
    int got = WAIT_UNTIL(take_plat_event(OC_PLAT_EV_A11Y_INVOKE, &e), 5000);
    CHECK(got);
    if (got) {
        uint64_t tok = ((uint64_t)(uintptr_t)e.user.data1 << 32) | (uint64_t)(uintptr_t)e.user.data2;
        CHECK(tok == 0x0123456789ABCDEFull);
    }
    /* An item that does nothing when pressed offers no action to press. */
    r = app_call(sub[0], "org.a11y.atspi.Action", "DoAction", g_variant_new("(i)", 0), "(b)");
    CHECK(r == NULL);
    if (r) g_variant_unref(r);

    oc_a11y_shutdown();
    disown(A_REG);
    disown(A_BUS);
}

/* ---- capture ----------------------------------------------------------------------- */

static void respond(guint32 code) {
    g_mutex_lock(&g_mu);
    S.response = code;
    g_mutex_unlock(&g_mu);
}

/* BT.709 limited range, as oc_media.h converts, for checking within a step or
 * two of rounding. */
static void yuv_of(int r, int g, int b, int *y, int *u, int *v) {
    double yl = 0.2126 * r + 0.7152 * g + 0.0722 * b;
    *y = (int)(16 + 219.0 * yl / 255.0 + 0.5);
    *u = (int)(128 + 224.0 * (b - yl) / (1.8556 * 255.0) + 0.5);
    *v = (int)(128 + 224.0 * (r - yl) / (1.5748 * 255.0) + 0.5);
}

/* A 16x8 frame, red on the left half and blue on the right, in `format`,
 * each row padded past its width; converted, and compared plane by plane. */
static int convert_one(uint32_t format) {
    enum { W = 16, H = 8, PAD = 24 };
    static const int RGB[2][3] = { { 255, 0, 0 }, { 0, 0, 255 } };
    int Y[2], U[2], V[2];
    for (int k = 0; k < 2; k++) yuv_of(RGB[k][0], RGB[k][1], RGB[k][2], &Y[k], &U[k], &V[k]);
    static uint8_t p0[(W * 4 + PAD) * H], p1[(W + PAD) * H], p2[(W + PAD) * H];
    const uint8_t *plane[3] = { p0, p1, p2 };
    int stride[3] = { 0, 0, 0 };
    memset(p0, 0xAA, sizeof p0); memset(p1, 0xAA, sizeof p1); memset(p2, 0xAA, sizeof p2);
    for (int r = 0; r < H; r++)
        for (int c = 0; c < W; c++) {
            int k = c >= W / 2;
            const int *px = RGB[k];
            switch (format) {
            case SPA_VIDEO_FORMAT_BGRx: case SPA_VIDEO_FORMAT_BGRA:
            case SPA_VIDEO_FORMAT_RGBx: case SPA_VIDEO_FORMAT_RGBA: {
                int bgr = format == SPA_VIDEO_FORMAT_BGRx || format == SPA_VIDEO_FORMAT_BGRA;
                stride[0] = W * 4 + PAD;
                uint8_t *d = p0 + r * stride[0] + c * 4;
                d[0] = (uint8_t)px[bgr ? 2 : 0]; d[1] = (uint8_t)px[1]; d[2] = (uint8_t)px[bgr ? 0 : 2];
                d[3] = (format == SPA_VIDEO_FORMAT_BGRA || format == SPA_VIDEO_FORMAT_RGBA) ? 255 : 0x5A;
                break;
            }
            case SPA_VIDEO_FORMAT_YUY2: {
                stride[0] = W * 2 + PAD;
                uint8_t *d = p0 + r * stride[0] + c * 2;
                d[0] = (uint8_t)Y[k];
                d[1] = (uint8_t)(c & 1 ? V[k] : U[k]);
                break;
            }
            case SPA_VIDEO_FORMAT_NV12:
                stride[0] = stride[1] = W + PAD;
                p0[r * stride[0] + c] = (uint8_t)Y[k];
                if (!(r & 1)) p1[(r / 2) * stride[1] + c] = (uint8_t)(c & 1 ? V[k] : U[k]);   /* Cb, Cr, ... */
                break;
            case SPA_VIDEO_FORMAT_I420:
                stride[0] = W + PAD; stride[1] = stride[2] = W / 2 + PAD;
                p0[r * stride[0] + c] = (uint8_t)Y[k];
                if (!(r & 1) && !(c & 1)) {
                    p1[(r / 2) * stride[1] + c / 2] = (uint8_t)U[k];
                    p2[(r / 2) * stride[2] + c / 2] = (uint8_t)V[k];
                }
                break;
            }
        }
    oc_frame f;
    if (oc_frame_alloc(&f, W, H) != 0) return 0;
    int ok = oc_cap_portal_convert(&f, format, plane, stride) == 0;
    for (int r = 0; ok && r < H; r++)
        for (int c = 0; c < W; c++) {
            int k = c >= W / 2;
            if (abs(f.plane[0][r * f.stride[0] + c] - Y[k]) > 2) ok = 0;
            if (r < H / 2 && c < W / 2 && (abs(f.plane[1][r * f.stride[1] + c] - U[c >= W / 4]) > 2 ||
                                           abs(f.plane[2][r * f.stride[2] + c] - V[c >= W / 4]) > 2))
                ok = 0;
        }
    oc_frame_free(&f);
    return ok;
}

static void test_convert(void) {
    uint32_t fmts[16];
    int n = oc_cap_portal_formats(fmts, 16);
    CHECK(n == 7);
    static const uint32_t ALL[7] = { SPA_VIDEO_FORMAT_NV12, SPA_VIDEO_FORMAT_YUY2, SPA_VIDEO_FORMAT_I420,
                                     SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx,
                                     SPA_VIDEO_FORMAT_RGBA };
    for (int i = 0; i < 7; i++) {
        int listed = 0;
        for (int j = 0; j < n && j < 16; j++) if (fmts[j] == ALL[i]) listed = 1;
        CHECK(listed);
        int ok = convert_one(ALL[i]);
        if (!ok) printf("  format %u: converted wrong\n", (unsigned)ALL[i]);
        CHECK(ok);
    }
    /* A format not negotiated is refused, not misread. */
    oc_frame f;
    const uint8_t *pl[3] = { NULL, NULL, NULL };
    int st[3] = { 0, 0, 0 };
    if (oc_frame_alloc(&f, 16, 8) == 0) {
        CHECK(oc_cap_portal_convert(&f, SPA_VIDEO_FORMAT_RGB, pl, st) == -1);
        oc_frame_free(&f);
    }
}

static void group_capture(void) {
    int fd = -1;
    uint32_t node = 0;
    oc_capture_device d[4];
    int err = 0;

    /* No portal: the requests fail, and no screen is offered. */
    if (activatable(P_NAME)) {
        SKIP("the bus would start a real portal, so none being there cannot be shown");
    } else {
        oc_cap_portal *p = oc_cap_portal_connect(NULL, NULL);
        CHECK(p != NULL);
        if (p) {
            CHECK(oc_cap_portal_camera(p, &fd) == OC_CAP_FAILED);
            CHECK(oc_cap_portal_screen(p, &fd, &node) == OC_CAP_FAILED);
            oc_cap_portal_close(p);
        }
        CHECK(oc_capture_list_screens(d, 4) == 0);
    }

    if (!own(P_NAME)) { CHECK(!"the portal's name"); return; }
    CHECK(oc_capture_platform() != NULL && oc_capture_screen_platform() != NULL);
    /* Through capture.c's choice of backend: the portal's one screen. */
    CHECK(oc_capture_list_screens(d, 4) == 1 && !strcmp(d[0].id, "portal") && d[0].kind == OC_SOURCE_SCREEN);

    /* Granted: the camera's remote, and the screen cast's session, sources,
     * start and remote, with the stream's node. */
    respond(0);
    oc_cap_portal *p = oc_cap_portal_connect(NULL, NULL);
    CHECK(p != NULL);
    if (p) {
        fd = -1;
        CHECK(oc_cap_portal_camera(p, &fd) == OC_CAP_OK);
        CHECK(fd >= 0);
        if (fd >= 0) close(fd);
        fd = -1;
        CHECK(oc_cap_portal_screen(p, &fd, &node) == OC_CAP_OK);
        CHECK(fd >= 0 && node == 42);
        if (fd >= 0) close(fd);
        g_mutex_lock(&g_mu);
        CHECK(S.n_access == 1 && S.n_create == 1 && S.n_select == 1 && S.n_start == 1 && S.n_remote == 2);
        /* A screen or a window, one, with the cursor drawn in, which the
         * portal offers. */
        guint32 types = 0, cursor = 0;
        gboolean multiple = TRUE;
        CHECK(S.last_select && g_variant_lookup(S.last_select, "types", "u", &types) && types == 3);
        CHECK(S.last_select && g_variant_lookup(S.last_select, "multiple", "b", &multiple) && !multiple);
        CHECK(S.last_select && g_variant_lookup(S.last_select, "cursor_mode", "u", &cursor) && cursor == 2);
        g_mutex_unlock(&g_mu);
        /* Closing ends the screen cast's session. */
        oc_cap_portal_close(p);
        g_mutex_lock(&g_mu);
        CHECK(S.n_session_close == 1);
        g_mutex_unlock(&g_mu);
    }

    /* Refused, and dismissed: denied, for the camera and the screen alike. */
    for (guint32 code = 1; code <= 2; code++) {
        respond(code);
        p = oc_cap_portal_connect(NULL, NULL);
        CHECK(p != NULL);
        if (!p) continue;
        fd = -1;
        CHECK(oc_cap_portal_camera(p, &fd) == OC_CAP_DENIED && fd < 0);
        CHECK(oc_cap_portal_screen(p, &fd, &node) == OC_CAP_DENIED && fd < 0);
        oc_cap_portal_close(p);
    }
    /* And through the backends, as the application opens them. */
    respond(1);
    err = 0;
    CHECK(oc_capture_open_screen("portal", 1280, 720, 30, &err) == NULL && err == OC_CAP_DENIED);
    err = 0;
    CHECK(oc_capture_open("", 640, 480, 30, &err) == NULL && err == OC_CAP_DENIED);
    respond(0);
    disown(P_NAME);

    test_convert();
}

/* ---- stream ------------------------------------------------------------------------ */

/* A PipeWire and a WirePlumber of the group's own, children of this program,
 * with their runtime, configuration and state directories under its temporary
 * directory and on the session bus dbus-run-session gave it, so the machine's
 * own (if it has one) is neither used nor changed. A producer in this program
 * publishes a video source on it, and the stand-in portal hands the backends
 * a connection to it and the source's node, as a desktop's portal does. */

static pid_t g_pw_pid = -1, g_wp_pid = -1;
static char  g_pw_dir[600];

/* `path` run as a child with `env`, its output into `log`. It is killed if this
 * program dies first, so a crash leaves nothing running. -1 when it cannot be
 * started. */
static pid_t spawn_daemon(const char *path, char **env, const char *log) {
    int lfd = open(log, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (lfd < 0) return -1;
    pid_t parent = getpid();
    pid_t pid = fork();
    if (pid == 0) {
        /* Only calls that are safe between fork and exec in a program with
         * threads. */
        prctl(PR_SET_PDEATHSIG, SIGKILL);
        if (getppid() != parent) _exit(127);
        int nul = open("/dev/null", O_RDONLY);
        if (nul >= 0) dup2(nul, 0);
        dup2(lfd, 1);
        dup2(lfd, 2);
        char *argv[] = { (char *)path, NULL };
        execve(path, argv, env);
        _exit(127);
    }
    close(lfd);
    return pid;
}

/* Whether the child has ended; reaped, it is forgotten. */
static int daemon_ended(pid_t *pid) {
    int st;
    if (*pid <= 0 || waitpid(*pid, &st, WNOHANG) != *pid) return 0;
    *pid = -1;
    return 1;
}

/* Asked to stop, and waited for; killed if it has not stopped by the
 * deadline. 1 when it stopped when asked. */
static int stop_daemon(pid_t *pid) {
    if (*pid <= 0) return 1;
    kill(*pid, SIGTERM);
    int st, stopped = WAIT_UNTIL(waitpid(*pid, &st, WNOHANG) == *pid, 5000);
    if (!stopped) {
        kill(*pid, SIGKILL);
        while (waitpid(*pid, &st, 0) < 0 && errno == EINTR) {}
    }
    *pid = -1;
    return stopped;
}

static int write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    int ok = fputs(text, f) >= 0;
    return fclose(f) == 0 && ok;
}

/* The daemon's log, on a check's failure: what it said is the why. */
static void print_log(const char *name) {
    char p[700];
    char *d = slurp(oc_test_path(p, sizeof p, g_pw_dir, name), NULL);
    if (d && d[0]) printf("  %s:\n%s\n", name, d);
    free(d);
}

/* This program's environment for the daemons: the group's own directories;
 * no display, so PipeWire rings no X11 bell; no system bus, so nothing there
 * is asked. */
static char **daemon_env(void) {
    char **env = g_get_environ(), p[700];
    static const char *const DIRS[][2] = {
        { "XDG_RUNTIME_DIR", "run" }, { "XDG_CONFIG_HOME", "config" }, { "XDG_STATE_HOME", "state" },
        { "XDG_DATA_HOME", "data" }, { "HOME", "home" },
    };
    for (size_t i = 0; i < sizeof DIRS / sizeof DIRS[0]; i++)
        env = g_environ_setenv(env, DIRS[i][0], oc_test_path(p, sizeof p, g_pw_dir, DIRS[i][1]), TRUE);
    char bus[720];
    snprintf(bus, sizeof bus, "unix:path=%s", oc_test_path(p, sizeof p, g_pw_dir, "no-system-bus"));
    env = g_environ_setenv(env, "DBUS_SYSTEM_BUS_ADDRESS", bus, TRUE);
    static const char *const UNSET[] = {
        "DISPLAY", "WAYLAND_DISPLAY", "PIPEWIRE_RUNTIME_DIR", "PIPEWIRE_REMOTE", "PIPEWIRE_CONFIG_DIR",
        "PIPEWIRE_CONFIG_NAME", "WIREPLUMBER_CONFIG_DIR", "WIREPLUMBER_DATA_DIR",
    };
    for (size_t i = 0; i < sizeof UNSET / sizeof UNSET[0]; i++) env = g_environ_unsetenv(env, UNSET[i]);
    return env;
}

/* The configuration the group adds to the installed one: no device monitors,
 * so the session manager sees no camera, sound card or Bluetooth of the
 * machine's, only the sources published here. */
static int daemon_config(void) {
    static const char *const DIRS[] = {
        "", "run", "config", "config/pipewire", "config/pipewire/pipewire.conf.d", "config/wireplumber",
        "config/wireplumber/main.lua.d", "config/wireplumber/bluetooth.lua.d", "state", "data", "home",
    };
    char p[700];
    for (size_t i = 0; i < sizeof DIRS / sizeof DIRS[0]; i++)
        if (mkdir(oc_test_path(p, sizeof p, g_pw_dir, DIRS[i]), 0700) != 0 && errno != EEXIST) return 0;
    return write_file(oc_test_path(p, sizeof p, g_pw_dir, "config/pipewire/pipewire.conf.d/90-test.conf"),
                      "context.properties = {\n  module.x11.bell = false\n  module.jackdbus-detect = false\n}\n") &&
           write_file(oc_test_path(p, sizeof p, g_pw_dir, "config/wireplumber/main.lua.d/51-test.lua"),
                      "alsa_monitor.enabled = false\nv4l2_monitor.enabled = false\n"
                      "libcamera_monitor.enabled = false\n") &&
           write_file(oc_test_path(p, sizeof p, g_pw_dir, "config/wireplumber/bluetooth.lua.d/51-test.lua"),
                      "bluez_monitor.enabled = false\nbluez_midi_monitor.enabled = false\n");
}

/* ---- this program's own connection to the private PipeWire ---- */

static struct {
    struct pw_thread_loop *loop;
    struct pw_context     *ctx;
    struct pw_core        *core;
    struct pw_registry    *reg;
    struct spa_hook        reg_hook;
    int                    session_manager;   /* WirePlumber's "default" metadata is there */
} G;

static void reg_global(void *data, uint32_t id, uint32_t perms, const char *type, uint32_t version,
                       const struct spa_dict *props) {
    (void)data; (void)id; (void)perms; (void)version;
    const char *name = props ? spa_dict_lookup(props, PW_KEY_METADATA_NAME) : NULL;
    /* contract: the metadata object WirePlumber makes once its configuration is loaded */
    if (!strcmp(type, PW_TYPE_INTERFACE_Metadata) && name && !strcmp(name, "default")) G.session_manager = 1;
}

static const struct pw_registry_events REG = { PW_VERSION_REGISTRY_EVENTS, .global = reg_global };

static int session_manager_up(void) {
    pw_thread_loop_lock(G.loop);
    int up = G.session_manager;
    pw_thread_loop_unlock(G.loop);
    return up;
}

static void pw_own_close(void) {
    if (!G.loop) return;
    pw_thread_loop_lock(G.loop);
    if (G.reg) { spa_hook_remove(&G.reg_hook); pw_proxy_destroy((struct pw_proxy *)G.reg); }
    if (G.core) pw_core_disconnect(G.core);
    pw_thread_loop_unlock(G.loop);
    pw_thread_loop_stop(G.loop);
    if (G.ctx) pw_context_destroy(G.ctx);
    pw_thread_loop_destroy(G.loop);
    memset(&G, 0, sizeof G);
}

static int pw_own_open(const char *sock) {
    memset(&G, 0, sizeof G);
    G.loop = pw_thread_loop_new("stream-test", NULL);
    G.ctx = G.loop ? pw_context_new(pw_thread_loop_get_loop(G.loop), NULL, 0) : NULL;
    if (!G.ctx || pw_thread_loop_start(G.loop) < 0) { pw_own_close(); return 0; }
    int fd = pw_socket_connect(sock);
    if (fd < 0) { pw_own_close(); return 0; }
    pw_thread_loop_lock(G.loop);
    G.core = pw_context_connect_fd(G.ctx, fd, NULL, 0);
    if (G.core) {
        G.reg = pw_core_get_registry(G.core, PW_VERSION_REGISTRY, 0);
        if (G.reg) pw_registry_add_listener(G.reg, &G.reg_hook, &REG, NULL);
    }
    pw_thread_loop_unlock(G.loop);
    if (!G.reg) { pw_own_close(); return 0; }
    return 1;
}

/* ---- the producer: a video source, as a compositor's screen cast or a camera
 * publishes one ----
 * It sends a picture only when asked, as a compositor sends one only when
 * something on the screen changed: a pattern of flat blocks, after the
 * synthetic sources' (capture.c), spelling a frame counter in its top band,
 * black for 0 and white for 1, over red on the left half and blue on the
 * right. A screen's buffer is larger than its picture, with a green margin
 * round it and the picture marked by a crop region, as a window is sent. */

enum { PIC_W = 64, PIC_H = 48, BITS = 8 };
enum { SEND_NONE, SEND_PICTURE, SEND_CURSOR };

typedef struct {
    struct pw_stream    *stream;
    struct spa_hook      hook;
    uint32_t             format;          /* SPA_VIDEO_FORMAT_BGRx or _NV12 */
    int                  w, h;            /* the buffer's size */
    int                  cx, cy;          /* where the picture is in it */
    int                  crop;            /* sends a crop region */
    enum pw_stream_state state;
    uint32_t             agreed;          /* the format the stream agreed, 0 before */
    int                  agreed_w, agreed_h;
    int                  send;            /* SEND_*: what the next turn sends */
    unsigned             counter;
    int                  sent;            /* buffers sent */
} producer;

static const int BLACK[3] = { 0, 0, 0 }, WHITE[3] = { 255, 255, 255 }, RED[3] = { 255, 0, 0 },
                 BLUE[3] = { 0, 0, 255 }, GREEN[3] = { 0, 255, 0 };

/* The pattern's colour at (x, y) of the picture, as RGB. */
static const int *pattern_at(unsigned counter, int x, int y) {
    if (y < PIC_H / 4) return (counter >> (BITS - 1 - x / (PIC_W / BITS))) & 1 ? WHITE : BLACK;
    return x < PIC_W / 2 ? RED : BLUE;
}

static void draw(producer *p, uint8_t *d, int stride) {
    if (p->format == SPA_VIDEO_FORMAT_BGRx) {
        for (int y = 0; y < p->h; y++)
            for (int x = 0; x < p->w; x++) {
                int px = x - p->cx, py = y - p->cy;
                const int *c = px >= 0 && py >= 0 && px < PIC_W && py < PIC_H ? pattern_at(p->counter, px, py) : GREEN;
                uint8_t *o = d + (size_t)y * stride + (size_t)x * 4;
                o[0] = (uint8_t)c[2]; o[1] = (uint8_t)c[1]; o[2] = (uint8_t)c[0]; o[3] = 0x5A;
            }
        return;
    }
    /* NV12: the luma, then the interleaved chroma below it in the same
     * block, a pair of samples for each 2x2. */
    uint8_t *uv = d + (size_t)stride * p->h;
    for (int y = 0; y < p->h; y++)
        for (int x = 0; x < p->w; x++) {
            const int *c = pattern_at(p->counter, x, y);
            int Y, U, V;
            yuv_of(c[0], c[1], c[2], &Y, &U, &V);
            d[(size_t)y * stride + x] = (uint8_t)Y;
            if (!(y & 1) && !(x & 1)) {
                uv[(size_t)(y / 2) * stride + x] = (uint8_t)U;
                uv[(size_t)(y / 2) * stride + x + 1] = (uint8_t)V;
            }
        }
}

static int frame_bytes(const producer *p, int *stride) {
    *stride = p->format == SPA_VIDEO_FORMAT_BGRx ? p->w * 4 : p->w;
    return p->format == SPA_VIDEO_FORMAT_BGRx ? *stride * p->h : *stride * p->h * 3 / 2;
}

static void prod_state(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    (void)old; (void)error;
    ((producer *)data)->state = state;
}

static void prod_param(void *data, uint32_t id, const struct spa_pod *param) {
    producer *p = data;
    struct spa_video_info_raw info;
    uint32_t mt, ms;
    if (id != SPA_PARAM_Format || !param || spa_format_parse(param, &mt, &ms) < 0 ||
        spa_format_video_raw_parse(param, &info) < 0)
        return;
    p->agreed = info.format;
    p->agreed_w = (int)info.size.width;
    p->agreed_h = (int)info.size.height;
    int stride, size = frame_bytes(p, &stride);
    uint8_t buf[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *params[3];
    uint32_t n = 0;
    params[n++] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size, SPA_POD_Int(size),
        SPA_PARAM_BUFFERS_stride, SPA_POD_Int(stride),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)));
    params[n++] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header),
        SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
    if (p->crop)
        params[n++] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
            SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region)));
    pw_stream_update_params(p->stream, params, n);
}

/* What was asked for, into a free buffer: the picture, or a buffer with
 * nothing in it, which is how a compositor says only the cursor moved. With
 * the loop locked; 0 when no buffer is free yet. */
static int prod_fill(producer *p) {
    struct pw_buffer *b = pw_stream_dequeue_buffer(p->stream);
    if (!b) return 0;
    struct spa_buffer *sb = b->buffer;
    struct spa_data *d = &sb->datas[0];
    int stride, size = frame_bytes(p, &stride);
    if (!d->data || d->maxsize < (uint32_t)size) { pw_stream_queue_buffer(p->stream, b); return 0; }
    if (p->send == SEND_PICTURE) draw(p, d->data, stride);
    d->chunk->offset = 0;
    d->chunk->stride = stride;
    d->chunk->size = p->send == SEND_PICTURE ? (uint32_t)size : 0;
    d->chunk->flags = 0;
    struct spa_meta_region *crop = spa_buffer_find_meta_data(sb, SPA_META_VideoCrop, sizeof *crop);
    if (crop) crop->region = SPA_REGION(p->cx, p->cy, PIC_W, PIC_H);
    pw_stream_queue_buffer(p->stream, b);
    return 1;
}

static const struct pw_stream_events PROD = {
    PW_VERSION_STREAM_EVENTS, .state_changed = prod_state, .param_changed = prod_param,
};

/* Published as `name`, a Video/Source; a camera says so in its role, which is
 * what the Camera portal's remote shows. */
static int prod_start(producer *p, const char *name, const char *desc, int camera) {
    pw_thread_loop_lock(G.loop);
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Source", PW_KEY_MEDIA_CLASS, "Video/Source",
        PW_KEY_NODE_NAME, name, PW_KEY_NODE_DESCRIPTION, desc, NULL);
    if (camera) pw_properties_set(props, PW_KEY_MEDIA_ROLE, "Camera");
    p->stream = pw_stream_new(G.core, name, props);
    int rc = -1;
    if (p->stream) {
        pw_stream_add_listener(p->stream, &p->hook, &PROD, p);
        uint8_t buf[512];
        struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
        struct spa_video_info_raw info = { 0 };
        info.format = p->format;
        info.size = SPA_RECTANGLE((uint32_t)p->w, (uint32_t)p->h);
        info.framerate = SPA_FRACTION(camera ? 30 : 0, 1);
        const struct spa_pod *params[1] = { spa_format_video_raw_build(&b, SPA_PARAM_EnumFormat, &info) };
        /* It drives its graph: a turn runs when it has something to send. */
        rc = pw_stream_connect(p->stream, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                               PW_STREAM_FLAG_DRIVER | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
    }
    pw_thread_loop_unlock(G.loop);
    return rc == 0;
}

static void prod_stop(producer *p) {
    pw_thread_loop_lock(G.loop);
    if (p->stream) {
        spa_hook_remove(&p->hook);
        pw_stream_destroy(p->stream);
        p->stream = NULL;
    }
    pw_thread_loop_unlock(G.loop);
}

/* The source's node, once it is on the graph; SPA_ID_INVALID before. */
static uint32_t prod_node(producer *p) {
    pw_thread_loop_lock(G.loop);
    uint32_t id = p->state >= PW_STREAM_STATE_PAUSED ? pw_stream_get_node_id(p->stream) : SPA_ID_INVALID;
    pw_thread_loop_unlock(G.loop);
    return id;
}

/* Whether the producer's side has agreed `format` at the size it offered:
 * each end of a link is told on its own. */
static int prod_agreed(producer *p, uint32_t format) {
    pw_thread_loop_lock(G.loop);
    int ok = p->agreed == format && p->agreed_w == p->w && p->agreed_h == p->h;
    pw_thread_loop_unlock(G.loop);
    return ok;
}

static int prod_streaming(producer *p) {
    pw_thread_loop_lock(G.loop);
    int on = p->state == PW_STREAM_STATE_STREAMING;
    pw_thread_loop_unlock(G.loop);
    return on;
}

/* What was asked for, sent if a buffer is free, as a compositor sends a
 * picture: filled and queued, and then a turn of the graph, which this
 * producer drives, to carry it. The number sent. */
static int prod_turn(producer *p) {
    pw_thread_loop_lock(G.loop);
    if (p->send != SEND_NONE && prod_fill(p)) {
        p->send = SEND_NONE;
        p->sent++;
        pw_stream_trigger_process(p->stream);
    }
    int sent = p->sent;
    pw_thread_loop_unlock(G.loop);
    return sent;
}

/* Sends a picture with `counter` (or, SEND_CURSOR, a buffer with none). */
static int prod_send(producer *p, int what, unsigned counter) {
    pw_thread_loop_lock(G.loop);
    int want = p->sent + 1;
    p->send = what;
    p->counter = counter;
    pw_thread_loop_unlock(G.loop);
    return WAIT_UNTIL(prod_turn(p) >= want, 5000);
}

/* ---- what the backends hand out ---- */

/* The counter the frame's top band spells, or -1 where a block is neither
 * black nor white. */
static int frame_counter(const oc_frame *f) {
    int n = 0;
    for (int bit = 0; bit < BITS; bit++) {
        int v = f->plane[0][(size_t)(PIC_H / 8) * f->stride[0] + bit * (PIC_W / BITS) + PIC_W / BITS / 2];
        if (v > 40 && v < 210) return -1;
        n = (n << 1) | (v >= 128);
    }
    return n;
}

/* Whether (x, y) of the frame is `rgb`, converted, within a step or two of
 * rounding. */
static int pixel_is(const oc_frame *f, int x, int y, const int *rgb) {
    int Y, U, V;
    yuv_of(rgb[0], rgb[1], rgb[2], &Y, &U, &V);
    return abs(f->plane[0][(size_t)y * f->stride[0] + x] - Y) <= 3 &&
           abs(f->plane[1][(size_t)(y / 2) * f->stride[1] + x / 2] - U) <= 3 &&
           abs(f->plane[2][(size_t)(y / 2) * f->stride[2] + x / 2] - V) <= 3;
}

/* The frame is the pattern with `counter`, the picture's size, converted to
 * I420: the counter in the band, red and blue below, and the corners the
 * picture's own, not the margin round it. */
static int frame_is(const oc_frame *f, unsigned counter) {
    if (f->width != PIC_W || f->height != PIC_H) {
        printf("  frame %dx%d, not %dx%d\n", f->width, f->height, PIC_W, PIC_H);
        return 0;
    }
    int n = frame_counter(f);
    if (n != (int)counter) printf("  frame counter %d, not %u\n", n, counter);
    return n == (int)counter && pixel_is(f, 0, 0, (counter >> (BITS - 1)) & 1 ? WHITE : BLACK) &&
           pixel_is(f, PIC_W / 4, PIC_H * 3 / 4, RED) && pixel_is(f, PIC_W * 3 / 4, PIC_H * 3 / 4, BLUE) &&
           pixel_is(f, 0, PIC_H - 1, RED) && pixel_is(f, PIC_W - 1, PIC_H - 1, BLUE);
}

/* How long a check that no frame comes gives the backend's own next(). A
 * short wait is a full one here: the producer sends only when asked, and the
 * last picture it sent has already been taken, so held still nothing is on its
 * way and a frame would be one the backend made up. A buffer with no picture
 * may still be on its way when the wait ends; then the picture sent after it,
 * which must come as itself and not as the one before, is the check that
 * reporting it would have been seen. */
#define STILL_MS 100

static void stream_screen(producer *p) {
    const oc_capture_backend *b = oc_capture_screen_platform();
    p->format = SPA_VIDEO_FORMAT_BGRx;
    p->w = PIC_W + 16; p->h = PIC_H + 12; p->cx = 8; p->cy = 6; p->crop = 1;
    if (!prod_start(p, "openchime-test-screen", "TestScreen", 0)) { CHECK(!"the screen's source"); return; }
    uint32_t node = SPA_ID_INVALID;
    CHECK(WAIT_UNTIL((node = prod_node(p)) != SPA_ID_INVALID, 5000));
    g_mutex_lock(&g_mu);
    S.node = node;
    g_mutex_unlock(&g_mu);

    /* Opened at the picture's size: the format is agreed by the time open
     * returns, the producer's own as it offered it. */
    int err = 0;
    void *c = b->open("portal", PIC_W, PIC_H, 30, &err);
    if (!c) printf("  screen: %d, %s\n", err, oc_capture_detail());
    CHECK(c != NULL && err == OC_CAP_OK);
    if (!c) { prod_stop(p); return; }
    CHECK(WAIT_UNTIL(prod_agreed(p, SPA_VIDEO_FORMAT_BGRx), 5000));
    CHECK(b->start(c) == OC_CAP_OK);
    CHECK(WAIT_UNTIL(prod_streaming(p), 5000));

    /* A picture arrives, converted, the crop region's part of the buffer. */
    oc_frame f;
    CHECK(prod_send(p, SEND_PICTURE, 0xA5));
    CHECK(b->next(c, &f, WAIT_MS(5000)) == 1 && frame_is(&f, 0xA5));
    /* Held still, nothing; a buffer with no picture in it, only the cursor
     * moving, is not a change either. Then a change is one frame, itself. */
    CHECK(b->next(c, &f, STILL_MS) == 0);
    CHECK(prod_send(p, SEND_CURSOR, 0));
    CHECK(b->next(c, &f, STILL_MS) == 0);
    CHECK(prod_send(p, SEND_PICTURE, 0x5A));
    CHECK(b->next(c, &f, WAIT_MS(5000)) == 1 && frame_is(&f, 0x5A));

    /* The source taken away, as when the window shared closes: gone. */
    prod_stop(p);
    int rc = 0;
    CHECK(WAIT_UNTIL((rc = b->next(c, &f, 50)) < 0, 5000));
    if (rc != OC_CAP_GONE) printf("  screen after its source went: %d, %s\n", rc, oc_capture_detail());
    CHECK(rc == OC_CAP_GONE);
    b->stop(c);
    b->close(c);
}

#define CAM_NODE "openchime-test-camera"
#define CAM_DESC "TestCamera"

static void stream_camera(producer *p) {
    const oc_capture_backend *b = oc_capture_platform();
    p->format = SPA_VIDEO_FORMAT_NV12;
    p->w = PIC_W; p->h = PIC_H; p->cx = p->cy = 0; p->crop = 0;
    if (!prod_start(p, CAM_NODE, CAM_DESC, 1)) { CHECK(!"the camera's source"); return; }
    CHECK(WAIT_UNTIL(prod_node(p) != SPA_ID_INVALID, 5000));

    /* The list is read from the portal's remote: the camera is in it by its
     * node's name and description. */
    oc_capture_device d[4];
    int n = b->list(d, 4), found = -1;
    for (int i = 0; i < n; i++) if (!strcmp(d[i].id, CAM_NODE)) found = i;
    CHECK(found >= 0 && !strcmp(d[found].name, CAM_DESC) && d[found].kind == OC_SOURCE_CAMERA);

    int err = 0;
    void *c = b->open(CAM_NODE, PIC_W, PIC_H, 30, &err);
    if (!c) printf("  camera: %d, %s\n", err, oc_capture_detail());
    CHECK(c != NULL && err == OC_CAP_OK);
    if (!c) { prod_stop(p); return; }
    CHECK(WAIT_UNTIL(prod_agreed(p, SPA_VIDEO_FORMAT_NV12), 5000));
    CHECK(b->start(c) == OC_CAP_OK);
    CHECK(WAIT_UNTIL(prod_streaming(p), 5000));

    oc_frame f;
    CHECK(prod_send(p, SEND_PICTURE, 0x3C));
    CHECK(b->next(c, &f, WAIT_MS(5000)) == 1 && frame_is(&f, 0x3C));
    CHECK(prod_send(p, SEND_PICTURE, 0xC3));
    CHECK(b->next(c, &f, WAIT_MS(5000)) == 1 && frame_is(&f, 0xC3));

    /* The camera taken away: the capture ends, as a camera unplugged does,
     * rather than waiting on frames that will not come. */
    prod_stop(p);
    int rc = 0;
    CHECK(WAIT_UNTIL((rc = b->next(c, &f, 50)) < 0, 5000));
    CHECK(rc == OC_CAP_FAILED);
    b->stop(c);
    b->close(c);
}

static void group_stream(void) {
    int at_start = failures;
    char *pw = g_find_program_in_path("pipewire"), *wp = g_find_program_in_path("wireplumber");
    if (!pw || !wp) {
        SKIP("pipewire and wireplumber are not installed");
        g_free(pw); g_free(wp);
        return;
    }
    char sock[700];
    oc_test_path(g_pw_dir, sizeof g_pw_dir, g_tmp, "pw");
    oc_test_path(sock, sizeof sock, g_pw_dir, "run/pipewire-0");
    if (strlen(sock) >= sizeof S.pw_socket) { CHECK(!"a socket path short enough"); g_free(pw); g_free(wp); return; }
    if (!daemon_config()) { CHECK(!"the daemons' directories"); g_free(pw); g_free(wp); return; }
    char **env = daemon_env(), log[700];
    pw_init(NULL, NULL);

    /* PipeWire is up when its socket takes a connection; WirePlumber, when
     * its metadata is on the graph. */
    g_pw_pid = spawn_daemon(pw, env, oc_test_path(log, sizeof log, g_pw_dir, "pipewire.log"));
    int fd = -1;
    int up = g_pw_pid > 0 && WAIT_UNTIL((fd = pw_socket_connect(sock)) >= 0 || daemon_ended(&g_pw_pid), 10000) &&
             fd >= 0;
    if (fd >= 0) close(fd);
    CHECK(up);
    if (up) {
        g_wp_pid = spawn_daemon(wp, env, oc_test_path(log, sizeof log, g_pw_dir, "wireplumber.log"));
        up = g_wp_pid > 0 && pw_own_open(sock) &&
             WAIT_UNTIL(session_manager_up() || daemon_ended(&g_wp_pid), 10000) && session_manager_up();
        CHECK(up);
    }
    g_strfreev(env);
    g_free(pw); g_free(wp);

    if (up && own(P_NAME)) {
        g_mutex_lock(&g_mu);
        snprintf(S.pw_socket, sizeof S.pw_socket, "%s", sock);
        g_mutex_unlock(&g_mu);
        respond(0);
        producer p;
        memset(&p, 0, sizeof p);
        stream_screen(&p);
        memset(&p, 0, sizeof p);
        stream_camera(&p);
        g_mutex_lock(&g_mu);
        S.pw_socket[0] = 0;
        S.node = 0;
        g_mutex_unlock(&g_mu);
        disown(P_NAME);
    } else if (up) {
        CHECK(!"the portal's name");
    }
    pw_own_close();
    /* Stopped, in the order they started backwards, and nothing left. */
    CHECK(stop_daemon(&g_wp_pid));
    CHECK(stop_daemon(&g_pw_pid));
    if (failures != at_start) { print_log("pipewire.log"); print_log("wireplumber.log"); }
}

/* ---- settings ---------------------------------------------------------------------- */

static void set_setting(const char *ns, const char *key, GVariant *v) {
    g_mutex_lock(&g_mu);
    int i = 0;
    while (i < S.n_set && (strcmp(S.set[i].ns, ns) || strcmp(S.set[i].key, key))) i++;
    if (i == S.n_set && S.n_set < 8) S.n_set++;
    if (i < S.n_set) {
        if (S.set[i].v) g_variant_unref(S.set[i].v);
        S.set[i].ns = ns; S.set[i].key = key; S.set[i].v = g_variant_ref_sink(v);
    }
    g_mutex_unlock(&g_mu);
}

static void clear_settings(void) {
    g_mutex_lock(&g_mu);
    for (int i = 0; i < S.n_set; i++) g_variant_unref(S.set[i].v);
    S.n_set = 0;
    g_mutex_unlock(&g_mu);
}

#define APPEARANCE "org.freedesktop.appearance"
#define INTERFACE  "org.gnome.desktop.interface"

static void group_settings(void) {
    /* No portal: none can be said. */
    if (activatable(P_NAME)) {
        SKIP("the bus would start a real portal, so none being there cannot be shown");
    } else {
        CHECK(oc_plat_prefers_light() == -1);
        CHECK(oc_plat_high_contrast() == -1);
        CHECK(oc_plat_reduce_motion() == -1);
    }
    if (!own(P_NAME)) { CHECK(!"the portal's name"); return; }

    set_setting(APPEARANCE, "color-scheme", g_variant_new_uint32(2));      /* prefer light */
    set_setting(APPEARANCE, "contrast", g_variant_new_uint32(1));          /* higher */
    set_setting(INTERFACE, "enable-animations", g_variant_new_boolean(FALSE));
    CHECK(oc_plat_prefers_light() == 1);
    CHECK(oc_plat_high_contrast() == 1);
    CHECK(oc_plat_reduce_motion() == 1);

    set_setting(APPEARANCE, "color-scheme", g_variant_new_uint32(1));      /* prefer dark */
    set_setting(APPEARANCE, "contrast", g_variant_new_uint32(0));          /* no preference */
    set_setting(INTERFACE, "enable-animations", g_variant_new_boolean(TRUE));
    CHECK(oc_plat_prefers_light() == 0);
    CHECK(oc_plat_high_contrast() == 0);
    CHECK(oc_plat_reduce_motion() == 0);

    /* A colour scheme of "no preference" says nothing either way. */
    set_setting(APPEARANCE, "color-scheme", g_variant_new_uint32(0));
    CHECK(oc_plat_prefers_light() == -1);

    /* A portal of version 1, without ReadOne: the same through Read. */
    set_setting(APPEARANCE, "color-scheme", g_variant_new_uint32(2));
    g_mutex_lock(&g_mu); S.settings_v1 = 1; g_mutex_unlock(&g_mu);
    CHECK(oc_plat_prefers_light() == 1);
    CHECK(oc_plat_reduce_motion() == 0);
    g_mutex_lock(&g_mu); S.settings_v1 = 0; g_mutex_unlock(&g_mu);

    /* A portal without the settings: each answers -1. */
    clear_settings();
    CHECK(oc_plat_prefers_light() == -1);
    CHECK(oc_plat_high_contrast() == -1);
    CHECK(oc_plat_reduce_motion() == -1);
    disown(P_NAME);
}

/* ---- the program ------------------------------------------------------------------- */

int main(int argc, char **argv) {
    /* The second process of a check: a second launch handing over, or a
     * process writing its crash ring and ending. */
    if (argc == 4 && !strcmp(argv[1], "--handoff")) return oc_plat_handoff(argv[2], argv[3]) ? 0 : 3;
    if (argc == 4 && !strcmp(argv[1], "--ring")) {
        uint8_t *r = oc_plat_ring_map(argv[2], 8192);
        if (!r) return 3;
        memcpy(r + 100, argv[3], strlen(argv[3]));
        _exit(0);
    }

    setvbuf(stdout, NULL, _IONBF, 0);
    if (!getenv("DBUS_SESSION_BUS_ADDRESS") || !services_start()) {
        fprintf(stderr, "linux_gui_test: no session bus; run it as dbus-run-session build/linux_gui_test\n");
        return 1;
    }
    if (oc_test_mkdtemp("linux_gui", g_tmp, sizeof g_tmp) != 0) {
        fprintf(stderr, "linux_gui_test: no temporary directory\n");
        return 1;
    }

    /* A window the platform and the provider can be given, on SDL's
     * offscreen driver: no display is needed. */
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "offscreen");
    SDL_Window *w = NULL;
    if (SDL_Init(SDL_INIT_VIDEO)) w = SDL_CreateWindow("OpenChime", 640, 480, SDL_WINDOW_HIDDEN);
    if (!w) printf("  no window: %s\n", SDL_GetError());

    static const char *const NAMES[] = { "platform", "image", "notify", "a11y", "capture", "stream", "settings" };
    for (int g = 0; g < 7; g++) {
        int before = failures;
        printf("linux_gui_test: %s\n", NAMES[g]);
        switch (g) {
        case 0: group_platform(w); break;
        case 1: group_image(); break;
        case 2: group_notify(); break;
        case 3: group_a11y(w); break;
        case 4: group_capture(); break;
        case 5: group_stream(); break;
        case 6: group_settings(); break;
        }
        printf("linux_gui_test: %s: %d failures\n", NAMES[g], failures - before);
    }

    if (w) SDL_DestroyWindow(w);
    SDL_Quit();
    services_stop();
    oc_test_rmtree(g_tmp);
    printf("linux_gui_test: %d failures, %d skipped\n", failures, oc_test_skips);
    return failures;
}
