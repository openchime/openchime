/*
 * OpenChime — OS notifications on Linux (osnotify.h): the desktop's
 * notification server, org.freedesktop.Notifications, over GDBus.
 *
 * Each notification is one Notify call. What it can carry is what the server
 * lists in GetCapabilities, asked once at init:
 *
 *   actions        the buttons, and a click (the "default" action), which come
 *                  back as ActionInvoked
 *   inline-reply   the reply box, by KDE's protocol: an "inline-reply" action,
 *                  the x-kde-reply-placeholder-text hint, and the typed text
 *                  coming back as NotificationReplied
 *   sound          the freedesktop sound theme's name in the sound-name hint,
 *                  so the person's own sound theme, and its silencing, is
 *                  followed; suppress-sound for a silent one
 *
 * Replacing and withdrawing are in every server: the id Notify answers is kept
 * against the notification's key within its group, a second for the same key
 * names it as replaces_id, and CloseNotification takes it back. The server's
 * NotificationClosed forgets it.
 *
 * THREADS. The connection is this file's own, and its signals are read by a
 * filter, which GDBus runs on its worker thread as each message arrives, so no
 * GLib main loop has to run anywhere (as in the AT-SPI provider,
 * a11y_atspi.c). The action callback is called there, on that thread, as the
 * Windows toast's activator calls it on COM's: osnotify.h's contract is that
 * the callback hands the work to the UI thread rather than acting. The table
 * of raised notifications is shared between the two threads under a lock.
 */

#include "osnotify.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>

#define N_NAME  "org.freedesktop.Notifications"
#define N_PATH  "/org/freedesktop/Notifications"
#define N_IFACE "org.freedesktop.Notifications"

/* How long a call to the notification server may take. Notify is answered
 * synchronously, because its id is what replacing and withdrawing need; a
 * server that takes longer than this is treated as absent for that one. */
#define OSN_TIMEOUT_MS 1000

/* The notifications kept for replacing, withdrawing and their actions: as
 * many as the application tracks conversations for. When full, the oldest is
 * forgotten; it stays on screen, and can no longer be replaced or acted on. */
#define OSN_KEEP 64
#define OSN_BTN_MAX 8

/* The freedesktop sound theme's names, by OC_OSN_SOUND_*. NULL is silence. */
static const char *const FDSOUND[OC_OSN_SOUND_COUNT] = {
    "message", "message-new-instant", "message-new-email", "alarm-clock-elapsed", NULL,
};

typedef struct {
    guint32 id;                 /* the server's; 0 for a free slot */
    guint64 seq;                /* when raised, for choosing the oldest */
    char   *group, *key;        /* key NULL: nothing replaces it */
    char   *open_url;           /* the "default" action's argument */
    char   *reply_arg;          /* NotificationReplied's argument */
    char   *btn_arg[OSN_BTN_MAX];   /* action "b<i>" hands back btn_arg[i] */
} osn_entry;

static GMutex           g_lock;
static osn_entry        g_ent[OSN_KEEP];
static guint64          g_seq;
static oc_osn_action_cb g_cb;

/* Owned by the application's thread. */
static GDBusConnection *g_bus;
static guint            g_filter;
static char             g_name[96];     /* the application, as the server shows it */
static unsigned         g_caps;
static int              g_markup, g_forced_off;

static void entry_clear(osn_entry *e) {
    g_free(e->group); g_free(e->key); g_free(e->open_url); g_free(e->reply_arg);
    for (int i = 0; i < OSN_BTN_MAX; i++) g_free(e->btn_arg[i]);
    memset(e, 0, sizeof *e);
}

/* Called with the lock held. */
static osn_entry *entry_by_id(guint32 id) {
    for (int i = 0; i < OSN_KEEP; i++)
        if (g_ent[i].id && g_ent[i].id == id) return &g_ent[i];
    return NULL;
}

static osn_entry *entry_by_key(const char *key, const char *group) {
    for (int i = 0; i < OSN_KEEP; i++)
        if (g_ent[i].id && g_ent[i].key && !strcmp(g_ent[i].key, key) &&
            !strcmp(g_ent[i].group, group ? group : ""))
            return &g_ent[i];
    return NULL;
}

static osn_entry *entry_free_slot(void) {
    osn_entry *old = &g_ent[0];
    for (int i = 0; i < OSN_KEEP; i++) {
        if (!g_ent[i].id) return &g_ent[i];
        if (g_ent[i].seq < old->seq) old = &g_ent[i];
    }
    entry_clear(old);
    return old;
}

/* ---- the server's signals ------------------------------------------------------ */

enum { SIG_ACTION, SIG_REPLY, SIG_CLOSED };

/* A signal can be read before the Notify that raised its notification has
 * come back with the id, since the reply and the signal are read on GDBus's
 * thread and the id is kept on the caller's. So one for an id not known is
 * kept, the last few, and the notification Notify answers with that id takes
 * those meant for it. The server broadcasts every application's, so most kept
 * here are never ours, and are simply overwritten. */
#define OSN_EARLY 16
typedef struct { guint32 id; int kind; char *s; } osn_sig;
static osn_sig g_early[OSN_EARLY];
static int     g_early_next;

/* What a signal of `kind` does to `e`, with the lock held: the argument to
 * hand back, copied, or NULL. `s` is the action's name, or the typed text.
 * "inline-reply" is not an action with an argument: the server answers it by
 * opening the box, and what was typed arrives as NotificationReplied. */
static char *apply(osn_entry *e, int kind, const char *s) {
    if (kind == SIG_CLOSED) { entry_clear(e); return NULL; }
    if (kind == SIG_REPLY) return g_strdup(e->reply_arg);
    if (!strcmp(s, "default")) return g_strdup(e->open_url);
    if (s[0] == 'b' && s[1] >= '0' && s[1] <= '9' && !s[2] && s[1] - '0' < OSN_BTN_MAX)
        return g_strdup(e->btn_arg[s[1] - '0']);
    return NULL;
}

static void deliver(char *arg, const char *reply) {
    g_mutex_lock(&g_lock);
    oc_osn_action_cb cb = g_cb;
    g_mutex_unlock(&g_lock);
    if (cb && arg) cb(arg, reply ? reply : "");
    g_free(arg);
}

/* Every signal of the server's, on GDBus's worker thread. */
static GDBusMessage *on_message(GDBusConnection *c, GDBusMessage *m, gboolean incoming, gpointer ud) {
    (void)c; (void)ud;
    if (!incoming || g_dbus_message_get_message_type(m) != G_DBUS_MESSAGE_TYPE_SIGNAL) return m;
    const char *path = g_dbus_message_get_path(m), *iface = g_dbus_message_get_interface(m);
    const char *member = g_dbus_message_get_member(m);
    GVariant *body = g_dbus_message_get_body(m);
    if (!path || !iface || !member || !body || strcmp(path, N_PATH) || strcmp(iface, N_IFACE)) return m;

    guint32 id;
    const char *str = "";
    int kind;
    if (!strcmp(member, "ActionInvoked") && g_variant_is_of_type(body, G_VARIANT_TYPE("(us)"))) {
        kind = SIG_ACTION;
        g_variant_get(body, "(u&s)", &id, &str);
    } else if (!strcmp(member, "NotificationReplied") && g_variant_is_of_type(body, G_VARIANT_TYPE("(us)"))) {
        kind = SIG_REPLY;
        g_variant_get(body, "(u&s)", &id, &str);
    } else if (!strcmp(member, "NotificationClosed") && g_variant_is_of_type(body, G_VARIANT_TYPE("(uu)"))) {
        kind = SIG_CLOSED;
        g_variant_get(body, "(uu)", &id, NULL);
    } else {
        return m;
    }
    char *arg = NULL;
    g_mutex_lock(&g_lock);
    osn_entry *e = entry_by_id(id);
    if (e) {
        arg = apply(e, kind, str);
    } else {
        osn_sig *k = &g_early[g_early_next];
        g_early_next = (g_early_next + 1) % OSN_EARLY;
        g_free(k->s);
        k->id = id; k->kind = kind; k->s = g_strdup(str);
    }
    g_mutex_unlock(&g_lock);
    deliver(arg, kind == SIG_REPLY ? str : "");
    return m;
}

/* The signals read before Notify answered with `e`'s id, oldest first, taken
 * by `e`, with the lock held. What they hand back is gathered into `arg` and
 * `text`, to be delivered once the lock is let go; the count is returned. */
static int take_early(osn_entry *e, char **arg, char **text) {
    int n = 0;
    guint32 id = e->id;
    for (int k = 0; k < OSN_EARLY; k++) {
        osn_sig *g = &g_early[(g_early_next + k) % OSN_EARLY];
        if (!g->id || g->id != id) continue;
        if (e->id == id) {
            char *a = apply(e, g->kind, g->s);
            if (a) {
                arg[n] = a;
                text[n] = NULL;
                if (g->kind == SIG_REPLY) { text[n] = g->s; g->s = NULL; }
                n++;
            }
        }
        g_free(g->s);
        memset(g, 0, sizeof *g);
    }
    return n;
}

/* ---- the connection ----------------------------------------------------------- */

static void disconnect(void) {
    if (!g_bus) return;
    g_dbus_connection_remove_filter(g_bus, g_filter);
    g_dbus_connection_close_sync(g_bus, NULL, NULL);
    g_object_unref(g_bus);
    g_bus = NULL;
    g_caps = 0;
    g_markup = 0;
}

/* A private connection to the session bus, so its filter sees only this
 * file's traffic, and the server's capabilities. 0, and no connection, when
 * there is no bus or no server on it. */
static int connect_server(void) {
    char *addr = g_dbus_address_get_for_bus_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!addr) return 0;
    GDBusConnection *c = g_dbus_connection_new_for_address_sync(
        addr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, NULL);
    g_free(addr);
    if (!c) return 0;
    /* The bus going away ends notifications, not the application. */
    g_dbus_connection_set_exit_on_close(c, FALSE);
    GVariant *r = g_dbus_connection_call_sync(c, N_NAME, N_PATH, N_IFACE, "GetCapabilities", NULL,
                                              G_VARIANT_TYPE("(as)"), G_DBUS_CALL_FLAGS_NONE, OSN_TIMEOUT_MS,
                                              NULL, NULL);
    if (!r) { g_dbus_connection_close_sync(c, NULL, NULL); g_object_unref(c); return 0; }
    unsigned caps = OC_OSN_CAP_REPLACE | OC_OSN_CAP_WITHDRAW;
    int actions = 0, reply = 0, markup = 0;
    GVariantIter *it;
    const char *cap;
    g_variant_get(r, "(as)", &it);
    while (g_variant_iter_loop(it, "&s", &cap)) {
        if (!strcmp(cap, "actions")) actions = 1;
        else if (!strcmp(cap, "inline-reply")) reply = 1;
        else if (!strcmp(cap, "sound")) caps |= OC_OSN_CAP_SOUND;
        else if (!strcmp(cap, "body-markup")) markup = 1;
    }
    g_variant_iter_free(it);
    g_variant_unref(r);
    if (actions) caps |= OC_OSN_CAP_ACTIONS;
    if (actions && reply) caps |= OC_OSN_CAP_REPLY;

    /* The server's signals are broadcast; the bus delivers them to a
     * connection that asks for them. */
    GVariant *m = g_dbus_connection_call_sync(
        c, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus", "AddMatch",
        g_variant_new("(s)", "type='signal',interface='" N_IFACE "',path='" N_PATH "'"), NULL,
        G_DBUS_CALL_FLAGS_NONE, OSN_TIMEOUT_MS, NULL, NULL);
    if (m) g_variant_unref(m);

    g_bus = c;
    g_caps = caps;
    g_markup = markup;
    g_filter = g_dbus_connection_add_filter(c, on_message, NULL, NULL);
    return 1;
}

/* ---- public surface ------------------------------------------------------------ */

/* `app_id`, `window` and `tray_id` name the application to Windows and the
 * tray icon its balloon hangs from; the server here needs only the name it
 * shows. */
int oc_osn_init(const char *app_id, const char *display_name, void *window, unsigned tray_id,
                oc_osn_action_cb cb) {
    (void)window; (void)tray_id;
    g_mutex_lock(&g_lock);
    g_cb = cb;
    g_mutex_unlock(&g_lock);
    snprintf(g_name, sizeof g_name, "%s", display_name ? display_name : app_id ? app_id : "");
    if (!g_bus) connect_server();
    return g_bus != NULL;
}

static int usable(void) { return g_bus && !g_forced_off; }

int      oc_osn_available(void) { return usable(); }
unsigned oc_osn_caps(void) { return usable() ? g_caps : 0; }

/* The body as the server reads it: escaped where it reads markup. */
static void body_append(GString *s, const char *t) {
    if (!g_markup) { g_string_append(s, t); return; }
    char *e = g_markup_escape_text(t, -1);
    g_string_append(s, e);
    g_free(e);
}

int oc_osn_show(const oc_osn *n) {
    if (!usable() || !n) return OC_OSN_NOT_SHOWN;
    int snd = n->sound >= 0 && n->sound < OC_OSN_SOUND_COUNT ? n->sound : OC_OSN_SOUND_DEFAULT;
    int actions = (g_caps & OC_OSN_CAP_ACTIONS) != 0;
    /* With a reply box the first button is its send, as on Windows; carried
     * by the inline reply where the server has one, and left out where it has
     * not, since a send with nothing to send does nothing. */
    int boxed = n->reply_placeholder && n->reply_placeholder[0] && n->n_btn > 0;
    int reply = boxed && (g_caps & OC_OSN_CAP_REPLY);
    int nb = n->n_btn < OSN_BTN_MAX ? n->n_btn : OSN_BTN_MAX;

    GVariantBuilder act;
    g_variant_builder_init(&act, G_VARIANT_TYPE("as"));
    if (actions) {
        if (n->open_url && n->open_url[0]) {
            g_variant_builder_add(&act, "s", "default");
            g_variant_builder_add(&act, "s", "");
        }
        if (reply) {
            g_variant_builder_add(&act, "s", "inline-reply");
            g_variant_builder_add(&act, "s", n->reply_placeholder);
        }
        for (int i = boxed ? 1 : 0; i < nb; i++) {
            if (!n->btn_label[i] || !n->btn_arg[i]) continue;
            char k[4] = { 'b', (char)('0' + i), 0, 0 };
            g_variant_builder_add(&act, "s", k);
            g_variant_builder_add(&act, "s", n->btn_label[i]);
        }
    }

    GVariantBuilder hints;
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "category", g_variant_new_string("im.received"));
    if (FDSOUND[snd]) g_variant_builder_add(&hints, "{sv}", "sound-name", g_variant_new_string(FDSOUND[snd]));
    else g_variant_builder_add(&hints, "{sv}", "suppress-sound", g_variant_new_boolean(TRUE));
    if (reply) {
        g_variant_builder_add(&hints, "{sv}", "x-kde-reply-placeholder-text",
                              g_variant_new_string(n->reply_placeholder));
        if (n->btn_label[0])
            g_variant_builder_add(&hints, "{sv}", "x-kde-reply-submit-button-text",
                                  g_variant_new_string(n->btn_label[0]));
    }

    /* Where it came from, under the text, as Windows' attribution line is. */
    GString *body = g_string_new(NULL);
    body_append(body, n->body ? n->body : "");
    if (n->source && n->source[0]) {
        g_string_append_c(body, '\n');
        body_append(body, n->source);
    }

    guint32 replaces = 0;
    if (n->key) {
        g_mutex_lock(&g_lock);
        osn_entry *e = entry_by_key(n->key, n->group);
        if (e) replaces = e->id;
        g_mutex_unlock(&g_lock);
    }

    GVariant *r = g_dbus_connection_call_sync(
        g_bus, N_NAME, N_PATH, N_IFACE, "Notify",
        g_variant_new("(susssasa{sv}i)", g_name, replaces, "", n->title ? n->title : "", body->str, &act,
                      &hints, -1),
        G_VARIANT_TYPE("(u)"), G_DBUS_CALL_FLAGS_NONE, OSN_TIMEOUT_MS, NULL, NULL);
    g_string_free(body, TRUE);
    if (!r) return OC_OSN_NOT_SHOWN;
    guint32 id;
    g_variant_get(r, "(u)", &id);
    g_variant_unref(r);

    g_mutex_lock(&g_lock);
    /* The one it replaced, under its old id where the server gave a new one,
     * and whatever already holds the new id (a NotificationClosed not yet
     * read), are forgotten before it is kept. */
    osn_entry *e = replaces ? entry_by_id(replaces) : NULL;
    if (e) entry_clear(e);
    e = entry_by_id(id);
    if (e) entry_clear(e);
    e = entry_free_slot();
    e->id = id;
    e->seq = ++g_seq;
    e->group = g_strdup(n->group ? n->group : "");
    e->key = g_strdup(n->key);
    e->open_url = g_strdup(n->open_url);
    e->reply_arg = reply ? g_strdup(n->btn_arg[0]) : NULL;
    for (int i = boxed ? 1 : 0; i < nb; i++) e->btn_arg[i] = g_strdup(n->btn_arg[i]);
    /* An id the server answered afresh may already have been acted on or
     * closed; one it reused for the replacement had those signals applied to
     * the notification it replaced. */
    char *early_arg[OSN_EARLY], *early_text[OSN_EARLY];
    int n_early = id != replaces ? take_early(e, early_arg, early_text) : 0;
    g_mutex_unlock(&g_lock);
    for (int i = 0; i < n_early; i++) {
        deliver(early_arg[i], early_text[i]);
        g_free(early_text[i]);
    }

    return FDSOUND[snd] && !(g_caps & OC_OSN_CAP_SOUND) ? OC_OSN_SHOWN_SILENT : OC_OSN_SHOWN;
}

/* Asked without waiting: the server answers an id it no longer shows with an
 * error, which changes nothing here. The table forgets it at once, so a
 * notification for the key that follows is a new one. */
static void close_id(guint32 id) {
    g_dbus_connection_call(g_bus, N_NAME, N_PATH, N_IFACE, "CloseNotification", g_variant_new("(u)", id), NULL,
                           G_DBUS_CALL_FLAGS_NONE, OSN_TIMEOUT_MS, NULL, NULL, NULL);
}

int oc_osn_withdraw(const char *key, const char *group) {
    if (!usable() || !key || !group) return 0;
    g_mutex_lock(&g_lock);
    osn_entry *e = entry_by_key(key, group);
    guint32 id = e ? e->id : 0;
    if (e) entry_clear(e);
    g_mutex_unlock(&g_lock);
    if (id) close_id(id);
    return 1;
}

int oc_osn_withdraw_group(const char *group) {
    if (!usable() || !group) return 0;
    guint32 ids[OSN_KEEP];
    int n = 0;
    g_mutex_lock(&g_lock);
    for (int i = 0; i < OSN_KEEP; i++)
        if (g_ent[i].id && !strcmp(g_ent[i].group, group)) {
            ids[n++] = g_ent[i].id;
            entry_clear(&g_ent[i]);
        }
    g_mutex_unlock(&g_lock);
    for (int i = 0; i < n; i++) close_id(ids[i]);
    return 1;
}

void oc_osn_test_unavailable(int on) { g_forced_off = on; }

/* What is on screen stays there, as Windows leaves its toasts in the
 * Notification Center; nothing is left to act on them. */
void oc_osn_done(void) {
    disconnect();
    g_mutex_lock(&g_lock);
    for (int i = 0; i < OSN_KEEP; i++) entry_clear(&g_ent[i]);
    for (int i = 0; i < OSN_EARLY; i++) { g_free(g_early[i].s); memset(&g_early[i], 0, sizeof g_early[i]); }
    g_cb = NULL;
    g_mutex_unlock(&g_lock);
}
