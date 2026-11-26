/* The Linux capture backends: the camera through the Camera portal and a
 * screen or window through the ScreenCast portal, both of
 * org.freedesktop.portal.Desktop, over GDBus, with the frames read from the
 * PipeWire stream the portal opens (docs/details/VIDEO-MESSAGES.md §3.2,
 * ARCH-110). Built only into the Linux GUI, which defines
 * OC_HAVE_PORTAL_CAPTURE; everywhere else this file is empty, as cap_mf.c and
 * cap_wgc.c are off Windows.
 *
 * The portal is how a desktop application reaches a camera or a screen at all
 * under Wayland, and it is the same under X11, so there is one path: no V4L2
 * device is opened and no X11 screen grabbed. Each request is a method call
 * that answers at once with a Request object, and the answer -- granted (0),
 * refused (1) or dismissed (2) -- comes later as that object's Response signal,
 * after the system has asked the person. A refusal is OC_CAP_DENIED, as the
 * operating system's block is on Windows; no portal is OC_CAP_FAILED.
 *
 * The portal's own dialog picks the screen or the window, so the screen
 * backend lists one source, whatever is chosen there, as the browser's does.
 * The camera backend lists the cameras the portal has already let this
 * application see, and one default camera until it has.
 *
 * THREADS. Each capture has its own connection to the session bus, whose
 * signals are read by a filter that GDBus runs on its worker thread as each
 * message arrives, so no GLib main loop has to run anywhere (as in
 * osnotify_linux.c and the AT-SPI provider); the caller waits for a Response on
 * a condition variable. The stream runs on a PipeWire thread loop of the
 * capture's own: each buffer is converted to I420 there and handed to `next`
 * under the capture's lock, as Media Foundation's callback thread does in
 * cap_mf.c. A screen reports a frame only when something changed -- the
 * compositor sends one only then -- and the capture front end repeats the last
 * at the frame rate (capture.c), as with cap_wgc.c. */
#ifdef OC_HAVE_PORTAL_CAPTURE
#include "cap_portal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/buffer/meta.h>
#include <spa/pod/builder.h>
#include <spa/utils/result.h>

#define P_NAME    "org.freedesktop.portal.Desktop"
#define P_PATH    "/org/freedesktop/portal/desktop"
#define P_CAMERA  "org.freedesktop.portal.Camera"
#define P_SCREEN  "org.freedesktop.portal.ScreenCast"
#define P_REQUEST "org.freedesktop.portal.Request"
#define P_SESSION "org.freedesktop.portal.Session"

/* A method call to the portal answers at once; only its Response waits for a
 * person. */
#define CALL_TIMEOUT_MS 5000
/* How long a request may wait for the person to answer the system's dialog
 * before it is withdrawn, as long as the browser's backend waits for its own
 * prompt (cap_web.c). */
#define ANSWER_TIMEOUT_S 120
/* How long a stream may take to agree a format once the portal has opened it. */
#define NEGOTIATE_TIMEOUT_S 5

#define SCREEN_NAME "The screen or window chosen in the system dialog"

/* ---- the frame conversion ---------------------------------------------------------- */

/* The order is the preference: a camera's YUV formats first, which carry no
 * more than the frame needs, then the packed RGB a screen comes in. */
static const uint32_t FORMATS[] = {
    SPA_VIDEO_FORMAT_NV12, SPA_VIDEO_FORMAT_YUY2, SPA_VIDEO_FORMAT_I420,
    SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA,
};
#define N_FORMATS ((int)(sizeof FORMATS / sizeof FORMATS[0]))
#define FIRST_RGB 3

int oc_cap_portal_formats(uint32_t *out, int cap) {
    for (int i = 0; i < N_FORMATS && i < cap; i++) out[i] = FORMATS[i];
    return N_FORMATS;
}

int oc_cap_portal_convert(oc_frame *dst, uint32_t format, const uint8_t *const plane[3], const int stride[3]) {
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRx: case SPA_VIDEO_FORMAT_BGRA:
        oc_i420_from_bgra(dst, plane[0], stride[0]);
        return 0;
    case SPA_VIDEO_FORMAT_RGBx: case SPA_VIDEO_FORMAT_RGBA:
        oc_i420_from_rgba(dst, plane[0], stride[0]);
        return 0;
    case SPA_VIDEO_FORMAT_YUY2:
        oc_i420_from_yuy2(dst, plane[0], stride[0]);
        return 0;
    case SPA_VIDEO_FORMAT_NV12:
        oc_i420_from_nv12(dst, plane[0], stride[0], plane[1], stride[1]);
        return 0;
    case SPA_VIDEO_FORMAT_I420:
        for (int p = 0; p < 3; p++) {
            int w = p ? dst->width / 2 : dst->width, h = p ? dst->height / 2 : dst->height;
            for (int r = 0; r < h; r++)
                memcpy(dst->plane[p] + (size_t)r * dst->stride[p], plane[p] + (size_t)r * stride[p], (size_t)w);
        }
        return 0;
    default:
        return -1;
    }
}

/* Bytes a pixel in the first plane, 0 for a format not accepted. */
static int format_bpp(uint32_t format) {
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRx: case SPA_VIDEO_FORMAT_BGRA:
    case SPA_VIDEO_FORMAT_RGBx: case SPA_VIDEO_FORMAT_RGBA: return 4;
    case SPA_VIDEO_FORMAT_YUY2: return 2;
    case SPA_VIDEO_FORMAT_NV12: case SPA_VIDEO_FORMAT_I420: return 1;
    default: return 0;
    }
}

/* ---- the portal -------------------------------------------------------------------- */

struct oc_cap_portal {
    gint             refs;           /* the owner's, and the filter's until GDBus lets it go */
    GDBusConnection *bus;
    guint            filter;
    char             sender[128];    /* the unique name as request paths spell it */
    unsigned         token;
    GMutex           mu;
    GCond            cv;
    char             want[256];      /* the Request whose Response is awaited */
    int              got;
    guint32          code;
    GVariant        *results;
    /* A Response for another path, kept in case the portal answers with a
     * Request path other than the one predicted (portals before 0.9 did). */
    char             stray_path[256];
    guint32          stray_code;
    GVariant        *stray_results;
    char             session[256];   /* the screen cast's, "" before CreateSession answers */
    int              gone;           /* the portal left the bus */
    void           (*on_closed)(void *);
    void            *ud;
};

static void portal_unref(gpointer data) {
    oc_cap_portal *p = data;
    if (!g_atomic_int_dec_and_test(&p->refs)) return;
    if (p->results) g_variant_unref(p->results);
    if (p->stray_results) g_variant_unref(p->stray_results);
    g_mutex_clear(&p->mu);
    g_cond_clear(&p->cv);
    g_free(p);
}

/* Every signal the connection is sent, on GDBus's worker thread. */
static GDBusMessage *on_message(GDBusConnection *c, GDBusMessage *m, gboolean incoming, gpointer ud) {
    (void)c;
    oc_cap_portal *p = ud;
    if (!incoming || g_dbus_message_get_message_type(m) != G_DBUS_MESSAGE_TYPE_SIGNAL) return m;
    const char *path = g_dbus_message_get_path(m), *iface = g_dbus_message_get_interface(m);
    const char *member = g_dbus_message_get_member(m);
    GVariant *body = g_dbus_message_get_body(m);
    if (!path || !iface || !member || !body) return m;

    if (!strcmp(iface, P_REQUEST) && !strcmp(member, "Response") &&
        g_variant_is_of_type(body, G_VARIANT_TYPE("(ua{sv})"))) {
        guint32 code;
        GVariant *results;
        g_variant_get(body, "(u@a{sv})", &code, &results);
        g_mutex_lock(&p->mu);
        if (!strcmp(path, p->want)) {
            if (p->results) g_variant_unref(p->results);
            p->results = results;
            p->code = code;
            p->got = 1;
            g_cond_broadcast(&p->cv);
        } else {
            if (p->stray_results) g_variant_unref(p->stray_results);
            snprintf(p->stray_path, sizeof p->stray_path, "%s", path);
            p->stray_code = code;
            p->stray_results = results;
        }
        g_mutex_unlock(&p->mu);
        return m;
    }

    int closed = 0;
    if (!strcmp(iface, P_SESSION) && !strcmp(member, "Closed")) {
        g_mutex_lock(&p->mu);
        closed = p->session[0] && !strcmp(path, p->session);
        g_mutex_unlock(&p->mu);
    } else if (!strcmp(iface, "org.freedesktop.DBus") && !strcmp(member, "NameOwnerChanged") &&
               g_variant_is_of_type(body, G_VARIANT_TYPE("(sss)"))) {
        const char *name, *old_owner, *new_owner;
        g_variant_get(body, "(&s&s&s)", &name, &old_owner, &new_owner);
        if (!strcmp(name, P_NAME) && !new_owner[0]) {
            g_mutex_lock(&p->mu);
            p->gone = 1;
            g_cond_broadcast(&p->cv);
            g_mutex_unlock(&p->mu);
            closed = 1;
        }
    }
    if (closed) {
        /* Under the lock, so that oc_cap_portal_close, which clears the
         * callback under it, returns only once no call is under way. */
        g_mutex_lock(&p->mu);
        if (p->on_closed) p->on_closed(p->ud);
        g_mutex_unlock(&p->mu);
    }
    return m;
}

static void add_match(GDBusConnection *c, const char *rule) {
    GVariant *r = g_dbus_connection_call_sync(c, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                                              "org.freedesktop.DBus", "AddMatch", g_variant_new("(s)", rule),
                                              NULL, G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, NULL, NULL);
    if (r) g_variant_unref(r);
}

oc_cap_portal *oc_cap_portal_connect(void (*on_closed)(void *ud), void *ud) {
    char *addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!addr) { oc_capture_set_detail("no session bus"); return NULL; }
    GDBusConnection *c = g_dbus_connection_new_for_address_sync(
        addr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, NULL);
    g_free(addr);
    if (!c) { oc_capture_set_detail("no session bus"); return NULL; }
    /* The bus going away ends the capture, not the application. */
    g_dbus_connection_set_exit_on_close(c, FALSE);
    oc_cap_portal *p = g_new0(oc_cap_portal, 1);
    p->refs = 2;
    p->bus = c;
    p->on_closed = on_closed;
    p->ud = ud;
    g_mutex_init(&p->mu);
    g_cond_init(&p->cv);
    /* A request's path is made from the caller's unique name, ":1.42" as
     * "1_42", and a token the caller chooses. */
    const char *u = g_dbus_connection_get_unique_name(c);
    int n = 0;
    for (const char *s = u ? u : ""; *s && n < (int)sizeof p->sender - 1; s++)
        if (*s != ':') p->sender[n++] = *s == '.' ? '_' : *s;
    p->sender[n] = 0;
    p->filter = g_dbus_connection_add_filter(c, on_message, p, portal_unref);
    /* The portal's signals are sent to whoever asked, but the bus delivers
     * them only to a connection that has asked for them. */
    add_match(c, "type='signal',interface='" P_REQUEST "',member='Response'");
    add_match(c, "type='signal',interface='" P_SESSION "',member='Closed'");
    add_match(c, "type='signal',sender='org.freedesktop.DBus',interface='org.freedesktop.DBus',"
                 "member='NameOwnerChanged',arg0='" P_NAME "'");
    return p;
}

void oc_cap_portal_close(oc_cap_portal *p) {
    if (!p) return;
    g_mutex_lock(&p->mu);
    p->on_closed = NULL;
    char session[256];
    snprintf(session, sizeof session, "%s", p->session);
    g_mutex_unlock(&p->mu);
    if (session[0]) {
        GVariant *r = g_dbus_connection_call_sync(p->bus, P_NAME, session, P_SESSION, "Close", NULL, NULL,
                                                  G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, NULL, NULL);
        if (r) g_variant_unref(r);
    }
    /* The filter may still be running when this returns; it holds its own
     * reference, which GDBus lets go once it has finished. */
    g_dbus_connection_remove_filter(p->bus, p->filter);
    g_dbus_connection_close_sync(p->bus, NULL, NULL);
    g_object_unref(p->bus);
    portal_unref(p);
}

/* The detail of a failed call, and what it means: the portal's NotAllowed is a
 * refusal it made without asking (a camera the permission store says no to). */
static int call_error(const char *what, GError *e) {
    char *remote = e ? g_dbus_error_get_remote_error(e) : NULL;
    int denied = remote && !strcmp(remote, "org.freedesktop.portal.Error.NotAllowed");
    g_free(remote);
    oc_capture_set_detail("%s: %s", what, e ? e->message : "no answer");
    if (e) g_error_free(e);
    return denied ? OC_CAP_DENIED : OC_CAP_FAILED;
}

/* A fresh handle token, and the Request path it makes, which is awaited from
 * now on: a Response read before the call's own reply is still taken. */
static void next_request(oc_cap_portal *p, char *token, size_t tcap) {
    snprintf(token, tcap, "openchime%u", ++p->token);
    g_mutex_lock(&p->mu);
    snprintf(p->want, sizeof p->want, P_PATH "/request/%s/%s", p->sender, token);
    p->got = 0;
    if (p->results) { g_variant_unref(p->results); p->results = NULL; }
    g_mutex_unlock(&p->mu);
}

/* Call `method` (whose options carry the token next_request chose) and wait
 * for its Response. OC_CAP_OK with `*results` (the caller's to unref). */
static int request(oc_cap_portal *p, const char *iface, const char *method, GVariant *args, GVariant **results) {
    GError *e = NULL;
    GVariant *r = g_dbus_connection_call_sync(p->bus, P_NAME, P_PATH, iface, method, args, G_VARIANT_TYPE("(o)"),
                                              G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, NULL, &e);
    if (!r) return call_error(method, e);
    const char *handle;
    g_variant_get(r, "(&o)", &handle);
    gint64 end = g_get_monotonic_time() + (gint64)ANSWER_TIMEOUT_S * G_TIME_SPAN_SECOND;
    g_mutex_lock(&p->mu);
    if (strcmp(handle, p->want)) {
        snprintf(p->want, sizeof p->want, "%s", handle);
        if (p->stray_results && !strcmp(p->stray_path, handle)) {
            p->results = p->stray_results;
            p->code = p->stray_code;
            p->stray_results = NULL;
            p->got = 1;
        }
    }
    while (!p->got && !p->gone)
        if (!g_cond_wait_until(&p->cv, &p->mu, end)) break;
    int got = p->got, gone = p->gone;
    guint32 code = p->code;
    GVariant *res = p->results;
    p->results = NULL;
    p->want[0] = 0;
    g_mutex_unlock(&p->mu);
    if (!got) {
        if (!gone) {
            /* Nobody answered: the dialog is taken down with the request. */
            GVariant *cr = g_dbus_connection_call_sync(p->bus, P_NAME, handle, P_REQUEST, "Close", NULL, NULL,
                                                       G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, NULL, NULL);
            if (cr) g_variant_unref(cr);
        }
        oc_capture_set_detail("%s: %s", method, gone ? "the portal went away" : "the system dialog was not answered");
        g_variant_unref(r);
        return OC_CAP_FAILED;
    }
    g_variant_unref(r);
    if (code != 0) {
        if (res) g_variant_unref(res);
        oc_capture_set_detail("%s: %s", method, code == 1 ? "refused" : code == 2 ? "dismissed" : "failed");
        return code == 1 || code == 2 ? OC_CAP_DENIED : OC_CAP_FAILED;
    }
    *results = res ? res : g_variant_ref_sink(g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0));
    return OC_CAP_OK;
}

/* A property of the portal's, or NULL where it has none (or there is no
 * portal). */
static GVariant *portal_prop(oc_cap_portal *p, const char *iface, const char *name) {
    GVariant *r = g_dbus_connection_call_sync(p->bus, P_NAME, P_PATH, "org.freedesktop.DBus.Properties", "Get",
                                              g_variant_new("(ss)", iface, name), G_VARIANT_TYPE("(v)"),
                                              G_DBUS_CALL_FLAGS_NONE, CALL_TIMEOUT_MS, NULL, NULL);
    if (!r) return NULL;
    GVariant *v;
    g_variant_get(r, "(v)", &v);
    g_variant_unref(r);
    return v;
}

static guint32 prop_u32(oc_cap_portal *p, const char *iface, const char *name, guint32 absent) {
    GVariant *v = portal_prop(p, iface, name);
    guint32 u = v && g_variant_is_of_type(v, G_VARIANT_TYPE_UINT32) ? g_variant_get_uint32(v) : absent;
    if (v) g_variant_unref(v);
    return u;
}

/* OpenPipeWireRemote's answer: the file descriptor it carries. */
static int remote_fd(oc_cap_portal *p, GVariant *args, int *fd) {
    GError *e = NULL;
    GUnixFDList *fds = NULL;
    const char *iface = g_variant_is_of_type(args, G_VARIANT_TYPE("(a{sv})")) ? P_CAMERA : P_SCREEN;
    GVariant *r = g_dbus_connection_call_with_unix_fd_list_sync(p->bus, P_NAME, P_PATH, iface, "OpenPipeWireRemote",
                                                                args, G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE,
                                                                CALL_TIMEOUT_MS, NULL, &fds, NULL, &e);
    if (!r) return call_error("OpenPipeWireRemote", e);
    gint32 idx;
    g_variant_get(r, "(h)", &idx);
    g_variant_unref(r);
    *fd = fds ? g_unix_fd_list_get(fds, idx, &e) : -1;
    if (fds) g_object_unref(fds);
    if (*fd < 0) return call_error("OpenPipeWireRemote fd", e);
    return OC_CAP_OK;
}

/* An a{sv} of the handle token alone, and any more given as name/value pairs. */
static GVariant *options(const char *token, const char *k1, GVariant *v1, const char *k2, GVariant *v2,
                         const char *k3, GVariant *v3) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&b, "{sv}", "handle_token", g_variant_new_string(token));
    if (k1) g_variant_builder_add(&b, "{sv}", k1, v1);
    if (k2) g_variant_builder_add(&b, "{sv}", k2, v2);
    if (k3) g_variant_builder_add(&b, "{sv}", k3, v3);
    return g_variant_builder_end(&b);
}

int oc_cap_portal_camera(oc_cap_portal *p, int *fd) {
    char token[32];
    GVariant *res = NULL;
    next_request(p, token, sizeof token);
    int rc = request(p, P_CAMERA, "AccessCamera", g_variant_new("(@a{sv})", options(token, NULL, NULL, NULL, NULL, NULL, NULL)), &res);
    if (rc != OC_CAP_OK) return rc;
    g_variant_unref(res);
    GVariantBuilder none;
    g_variant_builder_init(&none, G_VARIANT_TYPE("a{sv}"));
    return remote_fd(p, g_variant_new("(a{sv})", &none), fd);
}

/* Source types and cursor modes, as the ScreenCast interface numbers them. */
#define SRC_MONITOR 1u
#define SRC_WINDOW  2u
#define CURSOR_EMBEDDED 2u

int oc_cap_portal_screen(oc_cap_portal *p, int *fd, uint32_t *node) {
    char token[32], stoken[32];
    GVariant *res = NULL;

    next_request(p, token, sizeof token);
    snprintf(stoken, sizeof stoken, "openchime_session%u", p->token);
    int rc = request(p, P_SCREEN, "CreateSession",
                     g_variant_new("(@a{sv})", options(token, "session_handle_token", g_variant_new_string(stoken),
                                                       NULL, NULL, NULL, NULL)), &res);
    if (rc != OC_CAP_OK) return rc;
    const char *session = NULL;
    if (!g_variant_lookup(res, "session_handle", "&s", &session) || !g_variant_is_object_path(session)) {
        g_variant_unref(res);
        oc_capture_set_detail("CreateSession: no session");
        return OC_CAP_FAILED;
    }
    g_mutex_lock(&p->mu);
    snprintf(p->session, sizeof p->session, "%s", session);
    g_mutex_unlock(&p->mu);
    g_variant_unref(res);

    /* A screen or a window, whichever the person picks in the dialog; the
     * cursor drawn into the picture where the portal can, since a recording
     * of a screen shows where the person is pointing. */
    guint32 types = prop_u32(p, P_SCREEN, "AvailableSourceTypes", SRC_MONITOR) & (SRC_MONITOR | SRC_WINDOW);
    guint32 cursors = prop_u32(p, P_SCREEN, "AvailableCursorModes", 0);
    next_request(p, token, sizeof token);
    rc = request(p, P_SCREEN, "SelectSources",
                 g_variant_new("(o@a{sv})", p->session,
                               options(token, "types", g_variant_new_uint32(types ? types : SRC_MONITOR),
                                       "multiple", g_variant_new_boolean(FALSE),
                                       (cursors & CURSOR_EMBEDDED) ? "cursor_mode" : NULL,
                                       (cursors & CURSOR_EMBEDDED) ? g_variant_new_uint32(CURSOR_EMBEDDED) : NULL)),
                 &res);
    if (rc != OC_CAP_OK) return rc;
    g_variant_unref(res);

    next_request(p, token, sizeof token);
    rc = request(p, P_SCREEN, "Start",
                 g_variant_new("(os@a{sv})", p->session, "", options(token, NULL, NULL, NULL, NULL, NULL, NULL)), &res);
    if (rc != OC_CAP_OK) return rc;
    GVariant *streams = g_variant_lookup_value(res, "streams", G_VARIANT_TYPE("a(ua{sv})"));
    g_variant_unref(res);
    if (!streams || g_variant_n_children(streams) < 1) {
        if (streams) g_variant_unref(streams);
        oc_capture_set_detail("Start: no stream");
        return OC_CAP_FAILED;
    }
    g_variant_get_child(streams, 0, "(u@a{sv})", node, NULL);
    g_variant_unref(streams);

    GVariantBuilder none;
    g_variant_builder_init(&none, G_VARIANT_TYPE("a{sv}"));
    return remote_fd(p, g_variant_new("(oa{sv})", p->session, &none), fd);
}

/* Whether the portal says a camera is there: 1, 0, or -1 where it does not say
 * (no portal, or one without the Camera interface). */
static int camera_present(oc_cap_portal *p) {
    GVariant *v = portal_prop(p, P_CAMERA, "IsCameraPresent");
    int r = v && g_variant_is_of_type(v, G_VARIANT_TYPE_BOOLEAN) ? g_variant_get_boolean(v) != 0 : -1;
    if (v) g_variant_unref(v);
    return r;
}

/* ---- PipeWire ---------------------------------------------------------------------- */

static void pw_up(void) {
    static gsize once;
    if (g_once_init_enter(&once)) {
        pw_init(NULL, NULL);
        g_once_init_leave(&once, 1);
    }
}

/* A thread loop and a core on the portal's remote, whose file descriptor is
 * the core's from here on. Opened, the loop is running and left locked;
 * failing, nothing is left open and `k` is zeroed. */
typedef struct {
    struct pw_thread_loop *loop;
    struct pw_context     *ctx;
    struct pw_core        *core;
} pw_conn;

/* Called with the loop locked, if it was started. */
static void pw_conn_close(pw_conn *k) {
    if (k->core) pw_core_disconnect(k->core);
    if (k->loop && k->ctx) {
        pw_thread_loop_unlock(k->loop);
        pw_thread_loop_stop(k->loop);
    }
    if (k->ctx) pw_context_destroy(k->ctx);
    if (k->loop) pw_thread_loop_destroy(k->loop);
    memset(k, 0, sizeof *k);
}

static int pw_conn_open(pw_conn *k, int fd) {
    pw_up();
    memset(k, 0, sizeof *k);
    k->loop = pw_thread_loop_new("openchime-capture", NULL);
    /* A context needs PipeWire's client configuration and modules, which
     * come with PipeWire itself, not with the library. */
    struct pw_context *ctx = k->loop ? pw_context_new(pw_thread_loop_get_loop(k->loop), NULL, 0) : NULL;
    if (!ctx || pw_thread_loop_start(k->loop) < 0) {
        oc_capture_set_detail("PipeWire: %s", !k->loop ? "no thread loop" : !ctx ? "no context (is PipeWire installed?)"
                                                                       : "the thread loop did not start");
        if (ctx) pw_context_destroy(ctx);
        pw_conn_close(k);
        close(fd);
        return -1;
    }
    k->ctx = ctx;
    pw_thread_loop_lock(k->loop);
    /* The remote is closed with the core, or at once if it cannot be used. */
    k->core = pw_context_connect_fd(k->ctx, fd, NULL, 0);
    if (!k->core) {
        pw_conn_close(k);
        oc_capture_set_detail("PipeWire: cannot connect to the portal's remote");
        return -1;
    }
    return 0;
}

/* ---- the cameras the portal lets this application see ----------------------------------- */

typedef struct {
    pw_conn            *k;
    oc_capture_device  *out;
    int                 cap, n, seq, done;
} cam_enum;

static void enum_global(void *data, uint32_t id, uint32_t perms, const char *type, uint32_t version,
                        const struct spa_dict *props) {
    (void)id; (void)perms; (void)version;
    cam_enum *e = data;
    if (e->n >= e->cap || !props || strcmp(type, PW_TYPE_INTERFACE_Node)) return;
    const char *cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
    const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
    if (!cls || strcmp(cls, "Video/Source") || !name || !name[0]) return;
    const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
    if (!desc || !desc[0]) desc = spa_dict_lookup(props, PW_KEY_NODE_NICK);
    oc_capture_device *d = &e->out[e->n++];
    memset(d, 0, sizeof *d);
    snprintf(d->id, sizeof d->id, "%s", name);
    snprintf(d->name, sizeof d->name, "%s", desc && desc[0] ? desc : "Camera");
    d->kind = OC_SOURCE_CAMERA;
}

static void enum_done(void *data, uint32_t id, int seq) {
    cam_enum *e = data;
    if (id == PW_ID_CORE && seq == e->seq) { e->done = 1; pw_thread_loop_signal(e->k->loop, false); }
}

static void enum_error(void *data, uint32_t id, int seq, int res, const char *message) {
    (void)seq; (void)res; (void)message;
    cam_enum *e = data;
    if (id == PW_ID_CORE) { e->done = -1; pw_thread_loop_signal(e->k->loop, false); }
}

static const struct pw_registry_events ENUM_REGISTRY = { PW_VERSION_REGISTRY_EVENTS, .global = enum_global };
static const struct pw_core_events ENUM_CORE = { PW_VERSION_CORE_EVENTS, .done = enum_done, .error = enum_error };

/* The cameras on an already-granted remote, or -1 when it cannot be read. */
static int enum_cameras(int fd, oc_capture_device *out, int cap) {
    pw_conn k;
    if (pw_conn_open(&k, fd) != 0) return -1;
    cam_enum e = { &k, out, cap, 0, 0, 0 };
    struct spa_hook core_hook, reg_hook;
    spa_zero(core_hook); spa_zero(reg_hook);
    struct pw_registry *reg = pw_core_get_registry(k.core, PW_VERSION_REGISTRY, 0);
    if (!reg) { pw_conn_close(&k); return -1; }
    pw_core_add_listener(k.core, &core_hook, &ENUM_CORE, &e);
    pw_registry_add_listener(reg, &reg_hook, &ENUM_REGISTRY, &e);
    /* Every global is announced before the answer to a sync sent after the
     * registry was asked for. */
    e.seq = pw_core_sync(k.core, PW_ID_CORE, 0);
    while (!e.done)
        if (pw_thread_loop_timed_wait(k.loop, NEGOTIATE_TIMEOUT_S) != 0) break;
    spa_hook_remove(&reg_hook);
    spa_hook_remove(&core_hook);
    pw_proxy_destroy((struct pw_proxy *)reg);
    int n = e.done == 1 ? e.n : -1;
    pw_conn_close(&k);
    return n;
}

/* ---- a capture ------------------------------------------------------------------------ */

typedef struct {
    int                screen;
    oc_cap_portal     *portal;
    pw_conn            k;
    struct pw_stream  *stream;
    struct spa_hook    stream_hook;
    int                max_w, max_h;   /* a screen's bounds */
    GMutex             mu;
    GCond              cv;
    int                negotiated;      /* a format is agreed */
    uint32_t           format;          /* and what it is, SPA_VIDEO_FORMAT_* */
    int                fw, fh;          /* its size */
    int                running, failed, gone;
    oc_frame           tmp;             /* a screen at its content's size; the loop thread's own */
    oc_frame           ready, out;      /* the newest frame; the one handed out */
    uint64_t           ready_seq, out_seq;
} pcap;

/* The portal closed the session or left the bus: what was shared has gone. */
static void on_session_closed(void *ud) {
    pcap *c = ud;
    g_mutex_lock(&c->mu);
    c->gone = 1;
    g_cond_broadcast(&c->cv);
    g_mutex_unlock(&c->mu);
}

static void on_state(void *data, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    (void)old;
    pcap *c = data;
    if (state != PW_STREAM_STATE_ERROR && state != PW_STREAM_STATE_UNCONNECTED) return;
    if (error) oc_capture_set_detail("PipeWire stream: %s", error);
    g_mutex_lock(&c->mu);
    /* A screen's node is taken away when the window closes or the person
     * stops sharing from the desktop's own control: that is the source going
     * away. Before a format was agreed, it is a failure to open. */
    if (c->screen && c->negotiated) c->gone = 1;
    else c->failed = 1;
    g_cond_broadcast(&c->cv);
    g_mutex_unlock(&c->mu);
}

/* The format the stream agreed: the frames are made ready for it, and the
 * buffers asked for in memory this process can read. */
static void on_param(void *data, uint32_t id, const struct spa_pod *param) {
    pcap *c = data;
    if (id != SPA_PARAM_Format || !param) return;
    uint32_t mtype, msub;
    struct spa_video_info_raw info;
    if (spa_format_parse(param, &mtype, &msub) < 0 || mtype != SPA_MEDIA_TYPE_video ||
        msub != SPA_MEDIA_SUBTYPE_raw || spa_format_video_raw_parse(param, &info) < 0)
        return;
    int w = (int)info.size.width & ~1, h = (int)info.size.height & ~1;
    if (!format_bpp(info.format) || w < 2 || h < 2) {
        oc_capture_set_detail("PipeWire: format %u %ux%u", info.format, info.size.width, info.size.height);
        g_mutex_lock(&c->mu); c->failed = 1; g_cond_broadcast(&c->cv); g_mutex_unlock(&c->mu);
        return;
    }
    g_mutex_lock(&c->mu);
    int ok = 1;
    if (c->screen) {
        /* The one size this capture has, fixed by the first format: the
         * source's shape fitted inside the maximum, even, never enlarged, as
         * cap_wgc.c fixes it. A later change of size is fitted into it. */
        if (!c->ready.plane[0]) {
            int ow = c->max_w, oh = (int)((int64_t)c->max_w * h / w);
            if (oh > c->max_h) { oh = c->max_h; ow = (int)((int64_t)c->max_h * w / h); }
            if (ow > w || oh > h) { ow = w; oh = h; }
            ow &= ~1; oh &= ~1;
            if (ow < 2) ow = 2;
            if (oh < 2) oh = 2;
            ok = oc_frame_alloc(&c->ready, ow, oh) == 0 && oc_frame_alloc(&c->out, ow, oh) == 0;
            if (ok) oc_i420_fill(&c->ready, 16, 128, 128);
        }
    } else if (c->ready.width != w || c->ready.height != h) {
        /* A camera's frames come at its own size, as cap_mf.c's do. */
        oc_frame_free(&c->ready);
        ok = oc_frame_alloc(&c->ready, w, h) == 0;
    }
    if (ok) {
        c->format = info.format;
        c->fw = (int)info.size.width;
        c->fh = (int)info.size.height;
        c->negotiated = 1;
    } else {
        c->failed = 1;
    }
    g_cond_broadcast(&c->cv);
    g_mutex_unlock(&c->mu);
    if (!ok) return;

    uint8_t buf[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    const struct spa_pod *params[2];
    int n = 0;
    params[n++] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 16),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)));
    /* A window is sent in a buffer the size of the largest it has been, with
     * the part in use marked by a crop region. */
    if (c->screen)
        params[n++] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
            SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region)));
    pw_stream_update_params(c->stream, params, (uint32_t)n);
}

/* Where a plane of the frame starts, and its stride, checked against the
 * buffer that holds it: NULL when `rows` rows of `row_bytes` from (`x`,`y`)
 * do not fit. */
static const uint8_t *plane_at(const struct spa_data *d, size_t base, int stride, int x_bytes, int y,
                               int row_bytes, int rows) {
    if (!d->data || stride <= 0 || rows <= 0) return NULL;
    size_t start = base + (size_t)y * (size_t)stride + (size_t)x_bytes;
    size_t end = start + (size_t)(rows - 1) * (size_t)stride + (size_t)row_bytes;
    if (end > d->maxsize) return NULL;
    return (const uint8_t *)d->data + start;
}

/* One buffer's picture, (x, y, w×h) of it, to I420 in `dst` (dst's size is
 * w×h rounded down to even). 0, or -1 when the buffer does not hold it. */
static int convert_buffer(pcap *c, struct spa_buffer *sb, int x, int y, oc_frame *dst) {
    const struct spa_data *d0 = &sb->datas[0];
    if (!d0->chunk || !d0->maxsize) return -1;
    int bpp = format_bpp(c->format);
    int w = dst->width, h = dst->height;
    size_t off = d0->chunk->offset % d0->maxsize;
    int stride = d0->chunk->stride > 0 ? d0->chunk->stride : c->fw * bpp;
    const uint8_t *plane[3] = { NULL, NULL, NULL };
    int strides[3] = { stride, 0, 0 };
    if (c->format == SPA_VIDEO_FORMAT_YUY2) x &= ~1;
    plane[0] = plane_at(d0, off, stride, x * bpp, y, w * bpp, h);
    if (!plane[0]) return -1;
    if (c->format == SPA_VIDEO_FORMAT_NV12 || c->format == SPA_VIDEO_FORMAT_I420) {
        /* The chroma planes, each in a data of its own where the producer
         * sent them so, otherwise one after another below the luma. */
        int nplanes = c->format == SPA_VIDEO_FORMAT_NV12 ? 2 : 3;
        size_t next = off + (size_t)stride * (size_t)c->fh;
        int cstride = c->format == SPA_VIDEO_FORMAT_NV12 ? stride : stride / 2;
        int cx = x / 2, cy = y / 2, crow = c->format == SPA_VIDEO_FORMAT_NV12 ? w : w / 2;
        for (int p = 1; p < nplanes; p++) {
            const struct spa_data *dp = d0;
            size_t base = next;
            int s = cstride;
            if (sb->n_datas >= (uint32_t)nplanes && sb->datas[p].chunk && sb->datas[p].maxsize) {
                dp = &sb->datas[p];
                base = dp->chunk->offset % dp->maxsize;
                if (dp->chunk->stride > 0) s = dp->chunk->stride;
            }
            plane[p] = plane_at(dp, base, s, c->format == SPA_VIDEO_FORMAT_NV12 ? cx * 2 : cx, cy, crow, h / 2);
            if (!plane[p]) return -1;
            strides[p] = s;
            next = base + (size_t)s * (size_t)(c->fh / 2);
        }
    }
    return oc_cap_portal_convert(dst, c->format, plane, strides);
}

static void on_process(void *data) {
    pcap *c = data;
    /* The newest buffer: any older one waiting is given back unread, since
     * only the last picture matters to a reader that took none since. */
    struct pw_buffer *b = NULL, *n;
    while ((n = pw_stream_dequeue_buffer(c->stream))) {
        if (b) pw_stream_queue_buffer(c->stream, b);
        b = n;
    }
    if (!b) return;
    g_mutex_lock(&c->mu);
    int take = c->running && c->negotiated;
    g_mutex_unlock(&c->mu);
    struct spa_buffer *sb = b->buffer;
    const struct spa_data *d0 = sb->n_datas ? &sb->datas[0] : NULL;
    /* A buffer with nothing in it is a compositor saying only the cursor
     * moved, which is not a change of picture; a corrupt one is skipped. */
    if (!take || !d0 || !d0->chunk || !d0->chunk->size || (d0->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
        pw_stream_queue_buffer(c->stream, b);
        return;
    }
    int64_t now = oc_media_clock_us();
    if (c->screen) {
        int x = 0, y = 0, w = c->fw, h = c->fh;
        struct spa_meta_region *crop = spa_buffer_find_meta_data(sb, SPA_META_VideoCrop, sizeof *crop);
        if (crop && spa_meta_region_is_valid(crop) && crop->region.position.x >= 0 && crop->region.position.y >= 0 &&
            crop->region.position.x + (int64_t)crop->region.size.width <= c->fw &&
            crop->region.position.y + (int64_t)crop->region.size.height <= c->fh) {
            x = crop->region.position.x; y = crop->region.position.y;
            w = (int)crop->region.size.width; h = (int)crop->region.size.height;
        }
        w &= ~1; h &= ~1;
        if (w >= 2 && h >= 2 && (c->tmp.width != w || c->tmp.height != h)) {
            oc_frame_free(&c->tmp);
            if (oc_frame_alloc(&c->tmp, w, h) != 0) w = 0;
        }
        if (w >= 2 && h >= 2 && convert_buffer(c, sb, x, y, &c->tmp) == 0) {
            g_mutex_lock(&c->mu);
            oc_i420_fit(&c->tmp, &c->ready);
            c->ready.pts_us = now;
            c->ready_seq++;
            g_cond_broadcast(&c->cv);
            g_mutex_unlock(&c->mu);
        }
    } else {
        g_mutex_lock(&c->mu);
        if (convert_buffer(c, sb, 0, 0, &c->ready) == 0) {
            c->ready.pts_us = now;
            c->ready_seq++;
            g_cond_broadcast(&c->cv);
        }
        g_mutex_unlock(&c->mu);
    }
    pw_stream_queue_buffer(c->stream, b);
}

static const struct pw_stream_events STREAM = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state,
    .param_changed = on_param,
    .process = on_process,
};

static void pcap_close(void *impl);

/* The stream on the portal's remote: `node` for a screen, and for a camera the
 * one named by `target` (a node name from list), or the default. Waits until a
 * format is agreed. */
static int pcap_stream(pcap *c, int fd, uint32_t node, const char *target, int want_w, int want_h, int fps) {
    if (pw_conn_open(&c->k, fd) != 0) return OC_CAP_FAILED;
    struct pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, c->screen ? "Screen" : "Camera", NULL);
    if (props && target && target[0])
#ifdef PW_KEY_TARGET_OBJECT
        pw_properties_set(props, PW_KEY_TARGET_OBJECT, target);
#else
        pw_properties_set(props, PW_KEY_NODE_TARGET, target);
#endif
    c->stream = props ? pw_stream_new(c->k.core, "OpenChime", props) : NULL;
    if (!c->stream) { pw_thread_loop_unlock(c->k.loop); oc_capture_set_detail("PipeWire: no stream"); return OC_CAP_FAILED; }
    pw_stream_add_listener(c->stream, &c->stream_hook, &STREAM, c);

    /* The formats this converts, at the size and rate asked for or the
     * nearest the source has. */
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof buf);
    struct spa_pod_frame f[2];
    spa_pod_builder_push_object(&b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(&b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_format, 0);
    spa_pod_builder_push_choice(&b, &f[1], SPA_CHOICE_Enum, 0);
    int first = c->screen ? FIRST_RGB : 0;
    spa_pod_builder_id(&b, FORMATS[first]);                  /* the default */
    for (int i = first; i < N_FORMATS; i++) spa_pod_builder_id(&b, FORMATS[i]);
    spa_pod_builder_pop(&b, &f[1]);
    struct spa_rectangle size = SPA_RECTANGLE((uint32_t)(want_w > 0 ? want_w : 1280), (uint32_t)(want_h > 0 ? want_h : 720));
    struct spa_rectangle smin = SPA_RECTANGLE(2, 2), smax = SPA_RECTANGLE(8192, 8192);
    struct spa_fraction rate = SPA_FRACTION((uint32_t)(fps > 0 && fps <= 120 ? fps : 30), 1);
    struct spa_fraction rmin = SPA_FRACTION(0, 1), rmax = SPA_FRACTION(120, 1);
    spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&size, &smin, &smax),
                        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&rate, &rmin, &rmax), 0);
    const struct spa_pod *params[1] = { spa_pod_builder_pop(&b, &f[0]) };

    /* Never moved to another source when this one goes: the session manager
     * would otherwise hold the stream, paused, for a source like it to come
     * back, or link it to another camera or screen than the one the person
     * chose, and the capture would wait on frames that never come. Told not
     * to, it ends the stream, and that is the source going away (on_state). */
    int rc = pw_stream_connect(c->stream, PW_DIRECTION_INPUT, c->screen ? node : PW_ID_ANY,
                               PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                               PW_STREAM_FLAG_DONT_RECONNECT, params, 1);
    pw_thread_loop_unlock(c->k.loop);
    if (rc < 0) { oc_capture_set_detail("pw_stream_connect: %s", spa_strerror(rc)); return OC_CAP_FAILED; }

    gint64 end = g_get_monotonic_time() + (gint64)NEGOTIATE_TIMEOUT_S * G_TIME_SPAN_SECOND;
    g_mutex_lock(&c->mu);
    while (!c->negotiated && !c->failed && !c->gone)
        if (!g_cond_wait_until(&c->cv, &c->mu, end)) break;
    int ok = c->negotiated && !c->failed && !c->gone;
    g_mutex_unlock(&c->mu);
    if (!ok && !oc_capture_detail()[0]) oc_capture_set_detail("PipeWire: no format agreed");
    return ok ? OC_CAP_OK : OC_CAP_FAILED;
}

static pcap *pcap_new(int screen) {
    pcap *c = calloc(1, sizeof *c);
    if (!c) return NULL;
    c->screen = screen;
    g_mutex_init(&c->mu);
    g_cond_init(&c->cv);
    return c;
}

static void *open_any(int screen, const char *device_id, int w, int h, int fps, int *err) {
    oc_capture_set_detail("%s", "");
    pcap *c = pcap_new(screen);
    if (!c) { *err = OC_CAP_FAILED; return NULL; }
    c->max_w = w > 0 ? w : 1920;
    c->max_h = h > 0 ? h : 1080;
    c->portal = oc_cap_portal_connect(screen ? on_session_closed : NULL, c);
    if (!c->portal) { *err = OC_CAP_FAILED; pcap_close(c); return NULL; }
    int fd = -1, rc;
    uint32_t node = 0;
    if (screen) {
        rc = oc_cap_portal_screen(c->portal, &fd, &node);
    } else if (camera_present(c->portal) == 0) {
        oc_capture_set_detail("the portal finds no camera");
        rc = OC_CAP_NODEVICE;
    } else {
        rc = oc_cap_portal_camera(c->portal, &fd);
    }
    if (rc == OC_CAP_OK) rc = pcap_stream(c, fd, node, screen ? NULL : device_id, w, h, fps);
    if (rc != OC_CAP_OK) { *err = rc; pcap_close(c); return NULL; }
    *err = OC_CAP_OK;
    return c;
}

static void *cam_open(const char *id, int w, int h, int fps, int *err) { return open_any(0, id, w, h, fps, err); }
static void *scr_open(const char *id, int w, int h, int fps, int *err) { return open_any(1, id, w, h, fps, err); }

static void set_active(pcap *c, int on) {
    if (!c->stream) return;
    pw_thread_loop_lock(c->k.loop);
    pw_stream_set_active(c->stream, on ? true : false);
    pw_thread_loop_unlock(c->k.loop);
}

static int pcap_start(void *impl) {
    pcap *c = impl;
    g_mutex_lock(&c->mu);
    int was = c->running;
    c->running = 1;
    g_mutex_unlock(&c->mu);
    if (!was) set_active(c, 1);
    return OC_CAP_OK;
}

/* A new frame when one has come since the last call: 1; 0 on timeout. A
 * screen that went away is OC_CAP_GONE; a stream that failed, OC_CAP_FAILED. */
static int pcap_next(void *impl, oc_frame *f, int timeout_ms) {
    pcap *c = impl;
    gint64 end = g_get_monotonic_time() + (gint64)(timeout_ms > 0 ? timeout_ms : 0) * G_TIME_SPAN_MILLISECOND;
    g_mutex_lock(&c->mu);
    while (c->ready_seq == c->out_seq && !c->gone && !c->failed && c->running)
        if (!g_cond_wait_until(&c->cv, &c->mu, end)) break;
    int rc;
    if (c->gone) rc = OC_CAP_GONE;
    else if (c->failed || (c->screen && !c->running)) rc = OC_CAP_FAILED;
    else if (c->ready_seq == c->out_seq) rc = 0;
    else {
        /* A camera that changed size since the last frame: the handed-out
         * frame follows it. */
        if (c->out.width != c->ready.width || c->out.height != c->ready.height) {
            oc_frame_free(&c->out);
            if (oc_frame_alloc(&c->out, c->ready.width, c->ready.height) != 0) {
                c->failed = 1;
                g_mutex_unlock(&c->mu);
                return OC_CAP_FAILED;
            }
        }
        oc_frame_copy(&c->out, &c->ready);
        c->out_seq = c->ready_seq;
        *f = c->out;
        rc = 1;
    }
    g_mutex_unlock(&c->mu);
    if (rc == OC_CAP_GONE) oc_capture_set_detail("the shared screen or window went away");
    return rc;
}

static void pcap_stop(void *impl) {
    pcap *c = impl;
    g_mutex_lock(&c->mu);
    int was = c->running;
    c->running = 0;
    g_cond_broadcast(&c->cv);
    g_mutex_unlock(&c->mu);
    if (was) set_active(c, 0);
}

/* Releases the camera or the screen at once (REQ-166): the stream, the
 * remote, then the portal's session. */
static void pcap_close(void *impl) {
    pcap *c = impl;
    if (!c) return;
    if (c->k.loop) {
        pw_thread_loop_lock(c->k.loop);
        if (c->stream) {
            spa_hook_remove(&c->stream_hook);
            pw_stream_destroy(c->stream);
            c->stream = NULL;
        }
        pw_conn_close(&c->k);
    }
    oc_cap_portal_close(c->portal);
    oc_frame_free(&c->tmp);
    oc_frame_free(&c->ready);
    oc_frame_free(&c->out);
    g_mutex_clear(&c->mu);
    g_cond_clear(&c->cv);
    free(c);
}

/* ---- the backends ---------------------------------------------------------------------- */

/* The cameras: those the portal has already let this application see, read
 * from its remote, which it opens without asking the person once they have
 * said yes. Until then the one default camera, which opening asks for; none
 * where the portal says there is no camera, or there is no portal. */
static int cam_list(oc_capture_device *out, int cap) {
    if (cap <= 0) return 0;
    oc_cap_portal *p = oc_cap_portal_connect(NULL, NULL);
    if (!p) return 0;
    int present = camera_present(p), n = 0;
    if (present == 1) {
        GVariantBuilder none;
        g_variant_builder_init(&none, G_VARIANT_TYPE("a{sv}"));
        int fd = -1;
        if (remote_fd(p, g_variant_new("(a{sv})", &none), &fd) == OC_CAP_OK) n = enum_cameras(fd, out, cap);
        if (n <= 0) {
            memset(out, 0, sizeof *out);
            snprintf(out->name, sizeof out->name, "Camera");
            out->kind = OC_SOURCE_CAMERA;
            n = 1;
        }
        oc_capture_set_detail("%s", "");
    }
    oc_cap_portal_close(p);
    return n;
}

/* One source, whatever the person picks in the portal's dialog; none where
 * there is no ScreenCast portal. */
static int scr_list(oc_capture_device *out, int cap) {
    if (cap <= 0) return 0;
    oc_cap_portal *p = oc_cap_portal_connect(NULL, NULL);
    if (!p) return 0;
    GVariant *v = portal_prop(p, P_SCREEN, "version");
    oc_cap_portal_close(p);
    if (!v) return 0;
    g_variant_unref(v);
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, "portal");
    snprintf(out->name, sizeof out->name, SCREEN_NAME);
    out->kind = OC_SOURCE_SCREEN;
    return 1;
}

static const oc_capture_backend cam_backend = { cam_list, cam_open, pcap_start, pcap_next, pcap_stop, pcap_close };
static const oc_capture_backend scr_backend = { scr_list, scr_open, pcap_start, pcap_next, pcap_stop, pcap_close };

const oc_capture_backend *oc_capture_platform(void) { return &cam_backend; }
const oc_capture_backend *oc_capture_screen_platform(void) { return &scr_backend; }

#endif /* OC_HAVE_PORTAL_CAPTURE */
