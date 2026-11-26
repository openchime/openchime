/* OpenChime GUI -- the AT-SPI accessibility provider (REQ-269, REQ-290, ARCH-99).
 *
 * See a11y.h for why this is served from a published snapshot. The snapshot
 * is published as AT-SPI objects on the accessibility bus, by speaking the
 * AT-SPI D-Bus interfaces directly over GDBus: no ATK and no libatspi, which
 * are a toolkit's widget model and a client's library, and this is neither.
 *
 * The tree mirrors the one the Windows provider serves (win32/a11y.c), under
 * an application root, with each bar (oc_a11y_bar_publish) a window of its own
 * beside the main one, as win32/a11y_bar.c serves them:
 *
 *   the application
 *     |-- the main window       (Frame)
 *     |     |-- "Conversations" (List)  -> one list item per sidebar row
 *     |     |-- "Messages"      (List)  -> one list item per drawn message
 *     |     `-- the composer (Entry), and every button, tab and pane row
 *     `-- each bar              (Frame)
 *           |-- its line        (Label)
 *           `-- its buttons     (Push button, actionable while enabled)
 *
 * An item's object path is made from its kind, its channel or message id and
 * its automation id, never from its place in the list, so a client holding an
 * object still holds the same message after a row is drawn above it; a bar's
 * paths carry the generation of its slot, so one held from a bar since closed
 * answers as gone rather than as the next bar's.
 *
 * THREADS. The method calls are answered by a filter on the connection, which
 * GDBus runs on its own worker thread as each message arrives, so no GLib main
 * loop has to run anywhere: the platform asks the desktop its questions with
 * synchronous calls and runs none (plat_linux.c), and this adds none. The
 * filter reads the snapshot under a lock the publishing thread writes it
 * under, and never touches SDL's video state: what it needs of the windows
 * (where they are, how big, whether focused) is taken when the snapshot is
 * published, on the thread that owns them. A press is posted as an SDL event,
 * as on Windows, and the application runs it on its own thread. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gio/gio.h>

#include "a11y.h"
#include "platform.h"

/* How long a question to the session bus about the accessibility bus, or to
 * the registry, may take. Asked once, at start-up; a desktop that does not
 * answer in this time has no assistive technology to serve. */
#define OC_ATSPI_TIMEOUT_MS 1000

#define P_BASE  "/org/a11y/atspi/accessible/"
#define P_ROOT  P_BASE "root"
#define P_WIN   P_BASE "window"
#define P_CONVS P_BASE "conversations"
#define P_MSGS  P_BASE "messages"
#define P_NULL  "/org/a11y/atspi/null"

#define I_ACC   "org.a11y.atspi.Accessible"
#define I_APP   "org.a11y.atspi.Application"
#define I_COMP  "org.a11y.atspi.Component"
#define I_ACT   "org.a11y.atspi.Action"
#define I_TEXT  "org.a11y.atspi.Text"
#define I_PROPS "org.freedesktop.DBus.Properties"
#define I_INTRO "org.freedesktop.DBus.Introspectable"
#define I_EVOBJ "org.a11y.atspi.Event.Object"
#define I_EVWIN "org.a11y.atspi.Event.Window"

/* The numbers AT-SPI gives roles, states, coordinate types and layers
 * (at-spi2-core's AtspiRole, AtspiStateType, AtspiCoordType, AtspiLayer). */
enum {
    ROLE_FRAME = 23, ROLE_LABEL = 29, ROLE_LIST = 31, ROLE_LIST_ITEM = 32, ROLE_PAGE_TAB = 37,
    ROLE_PUSH_BUTTON = 43, ROLE_APPLICATION = 75, ROLE_ENTRY = 79
};
enum {
    ST_ACTIVE = 1, ST_EDITABLE = 7, ST_ENABLED = 8, ST_FOCUSABLE = 11, ST_FOCUSED = 12,
    ST_MULTI_LINE = 17, ST_RESIZABLE = 21, ST_SELECTABLE = 22, ST_SELECTED = 23, ST_SENSITIVE = 24,
    ST_SHOWING = 25, ST_VISIBLE = 30, ST_SELECTABLE_TEXT = 38
};
enum { COORD_SCREEN = 0, COORD_WINDOW = 1, COORD_PARENT = 2 };
enum { LAYER_WIDGET = 3, LAYER_POPUP = 5, LAYER_WINDOW = 7 };
/* The politeness an announcement carries (AtspiLive). */
enum { LIVE_POLITE = 1, LIVE_ASSERTIVE = 2 };

/* ---- the published snapshot ------------------------------------------------
 * Written by the application's thread, read by GDBus's worker. */
static GMutex       g_lock;
static oc_acc_item  g_items[OC_ACC_MAX];
static uint64_t     g_keys[OC_ACC_MAX];     /* each item's path, see item_keys */
static int          g_n_items;
static uint16_t     g_comp[4096];
static int          g_comp_len, g_comp_caret, g_comp_anchor;
/* The main window as it was at the last publish. */
static int          g_win_x, g_win_y, g_win_w, g_win_h, g_win_focus;
static char         g_win_title[160];
/* What the application last said has focus (oc_a11y_focus). */
static int          g_focus_set;
static oc_acc_kind  g_focus_kind;
static uint64_t     g_focus_id;
/* The registry's desktop, which is the application's parent. */
static char         g_desk_name[128];
static char         g_desk_path[128];
static int          g_app_id;

/* The bars: two at most, the recording bar and the sharing bar. */
enum { BAR_SLOTS = 2, BAR_BTNS = 2, BAR_TEXT = 200 };
typedef struct {
    SDL_Window *w;
    unsigned    gen;
    char        aid[OC_ACC_AID_MAX];
    char        name[96];
    char        line[BAR_TEXT];
    oc_acc_item btn[BAR_BTNS];
    int         n_btn;
    int         x, y, wpx, hpx;
} bar_slot;
static bar_slot g_bar[BAR_SLOTS];

/* Owned by the application's thread. */
static SDL_Window      *g_win;
static GDBusConnection *g_bus;
static guint            g_filter;
static int              g_ready;
static unsigned         g_announced;
static char             g_focus_path[96];   /* the object last told it has focus */

/* ---- objects ---------------------------------------------------------------- */

typedef enum { O_NONE, O_ROOT, O_WIN, O_CONVS, O_MSGS, O_ITEM, O_BAR, O_BAR_LINE, O_BAR_BTN } obj_type;
typedef struct {
    obj_type t;
    int      i;       /* O_ITEM: index into g_items; O_BAR_BTN: the button */
    int      slot;    /* the bars' */
} obj;

/* Which container an item hangs off: the conversations and the messages have
 * a list each, and everything else hangs directly off the window, a flat set
 * a client finds by automation id (win32/a11y.c says why). */
enum { G_WIN = 0, G_CONVS = 1, G_MSGS = 2, G_COUNT = 3 };
static int group_of_kind(oc_acc_kind k) {
    return k == OC_ACC_CONVERSATION ? G_CONVS : k == OC_ACC_MESSAGE ? G_MSGS : G_WIN;
}
static const char *group_path(int g) { return g == G_CONVS ? P_CONVS : g == G_MSGS ? P_MSGS : P_WIN; }
/* The window's first two children are the two lists. */
static int group_base(int g) { return g == G_WIN ? 2 : 0; }

static void item_path(char *buf, size_t cap, uint64_t key) {
    snprintf(buf, cap, P_BASE "item_%016llx", (unsigned long long)key);
}

/* FNV-1a, folded over the parts of an item that name it. */
static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

/* Each item's key: its kind, id and automation id, which is what stays the
 * same about it from one frame to the next. Two items alike in all three are
 * told apart by their order, through a small open-addressed set. */
static void item_keys(const oc_acc_item *items, int n, uint64_t *keys) {
    enum { SET = 2048 };                      /* a power of two above OC_ACC_MAX */
    static uint64_t set[SET];
    memset(set, 0, sizeof set);
    for (int i = 0; i < n; i++) {
        uint64_t h = 14695981039346656037ull;
        int kind = (int)items[i].kind;
        h = fnv(h, &kind, sizeof kind);
        h = fnv(h, &items[i].id, sizeof items[i].id);
        h = fnv(h, items[i].aid, strlen(items[i].aid));
        for (;;) {
            if (!h) h = 1;                    /* 0 marks an empty slot */
            unsigned s = (unsigned)(h & (SET - 1));
            while (set[s] && set[s] != h) s = (s + 1) & (SET - 1);
            if (!set[s]) { set[s] = h; break; }
            h = fnv(h, &i, sizeof i);         /* taken: this one is the next alike */
        }
        keys[i] = h;
    }
}

/* The lock is held from here to the end of the object functions. */

static int item_with_key(uint64_t key) {
    for (int i = 0; i < g_n_items; i++) if (g_keys[i] == key) return i;
    return -1;
}

static obj resolve(const char *path) {
    obj o = { O_NONE, 0, 0 };
    if (!strcmp(path, P_ROOT))  { o.t = O_ROOT;  return o; }
    if (!strcmp(path, P_WIN))   { o.t = O_WIN;   return o; }
    if (!strcmp(path, P_CONVS)) { o.t = O_CONVS; return o; }
    if (!strcmp(path, P_MSGS))  { o.t = O_MSGS;  return o; }
    const char *rest = path + strlen(P_BASE);
    unsigned long long key;
    int used = 0;
    if (sscanf(rest, "item_%16llx%n", &key, &used) == 1 && !rest[used]) {
        int i = item_with_key((uint64_t)key);
        if (i >= 0) { o.t = O_ITEM; o.i = i; }
        return o;
    }
    int slot;
    unsigned gen;
    if (sscanf(rest, "bar%d_%u%n", &slot, &gen, &used) == 2 && slot >= 0 && slot < BAR_SLOTS &&
        g_bar[slot].w && g_bar[slot].gen == gen) {
        const char *tail = rest + used;
        int b;
        o.slot = slot;
        if (!*tail) o.t = O_BAR;
        else if (!strcmp(tail, "_line")) o.t = O_BAR_LINE;
        else if (sscanf(tail, "_b%d%n", &b, &used) == 1 && !tail[used] && b >= 0 && b < g_bar[slot].n_btn) {
            o.t = O_BAR_BTN; o.i = b;
        }
    }
    return o;
}

static void obj_path(obj o, char *buf, size_t cap) {
    switch (o.t) {
    case O_ROOT:     snprintf(buf, cap, P_ROOT); break;
    case O_WIN:      snprintf(buf, cap, P_WIN); break;
    case O_CONVS:    snprintf(buf, cap, P_CONVS); break;
    case O_MSGS:     snprintf(buf, cap, P_MSGS); break;
    case O_ITEM:     item_path(buf, cap, g_keys[o.i]); break;
    case O_BAR:      snprintf(buf, cap, P_BASE "bar%d_%u", o.slot, g_bar[o.slot].gen); break;
    case O_BAR_LINE: snprintf(buf, cap, P_BASE "bar%d_%u_line", o.slot, g_bar[o.slot].gen); break;
    case O_BAR_BTN:  snprintf(buf, cap, P_BASE "bar%d_%u_b%d", o.slot, g_bar[o.slot].gen, o.i); break;
    default:         snprintf(buf, cap, P_NULL); break;
    }
}

static const char *unique_name(void) {
    const char *n = g_bus ? g_dbus_connection_get_unique_name(g_bus) : NULL;
    return n ? n : "";
}
static GVariant *ref_path(const char *path) { return g_variant_new("(so)", unique_name(), path); }
static GVariant *ref_obj(obj o) {
    char p[96];
    obj_path(o, p, sizeof p);
    return ref_path(p);
}
static GVariant *ref_null(void) { return g_variant_new("(so)", "", P_NULL); }

static int is_item_kind(obj o, oc_acc_kind k) { return o.t == O_ITEM && g_items[o.i].kind == k; }
static int composer_idx(void) {
    for (int i = 0; i < g_n_items; i++) if (g_items[i].kind == OC_ACC_COMPOSER) return i;
    return -1;
}
static int item_invokable(obj o) {
    return (o.t == O_ITEM && g_items[o.i].invoke) || (o.t == O_BAR_BTN && g_bar[o.slot].btn[o.i].invoke);
}

/* The n-th item of a group, and an item's place in its group. */
static int group_nth(int g, int n) {
    for (int i = 0; i < g_n_items; i++)
        if (group_of_kind(g_items[i].kind) == g && n-- == 0) return i;
    return -1;
}
static int group_count(int g) {
    int c = 0;
    for (int i = 0; i < g_n_items; i++) if (group_of_kind(g_items[i].kind) == g) c++;
    return c;
}
static int group_pos(int idx) {
    int g = group_of_kind(g_items[idx].kind), c = 0;
    for (int i = 0; i < idx; i++) if (group_of_kind(g_items[i].kind) == g) c++;
    return group_base(g) + c;
}
static int bars_before(int slot) {
    int c = 0;
    for (int i = 0; i < slot; i++) if (g_bar[i].w) c++;
    return c;
}

static int child_count(obj o) {
    switch (o.t) {
    case O_ROOT:  return 1 + bars_before(BAR_SLOTS);
    case O_WIN:   return 2 + group_count(G_WIN);
    case O_CONVS: return group_count(G_CONVS);
    case O_MSGS:  return group_count(G_MSGS);
    case O_BAR:   return 1 + g_bar[o.slot].n_btn;
    default:      return 0;
    }
}

static obj child_at(obj o, int n) {
    obj c = { O_NONE, 0, 0 };
    if (n < 0) return c;
    switch (o.t) {
    case O_ROOT:
        if (n == 0) { c.t = O_WIN; break; }
        for (int s = 0; s < BAR_SLOTS; s++)
            if (g_bar[s].w && --n == 0) { c.t = O_BAR; c.slot = s; break; }
        break;
    case O_WIN:
        if (n == 0) c.t = O_CONVS;
        else if (n == 1) c.t = O_MSGS;
        else if ((c.i = group_nth(G_WIN, n - 2)) >= 0) c.t = O_ITEM;
        break;
    case O_CONVS:
    case O_MSGS:
        if ((c.i = group_nth(o.t == O_CONVS ? G_CONVS : G_MSGS, n)) >= 0) c.t = O_ITEM;
        break;
    case O_BAR:
        c.slot = o.slot;
        if (n == 0) c.t = O_BAR_LINE;
        else if (n - 1 < g_bar[o.slot].n_btn) { c.t = O_BAR_BTN; c.i = n - 1; }
        break;
    default: break;
    }
    return c;
}

static obj parent_of(obj o) {
    obj p = { O_NONE, 0, o.slot };
    switch (o.t) {
    case O_WIN: case O_BAR:       p.t = O_ROOT; break;
    case O_CONVS: case O_MSGS:    p.t = O_WIN; break;
    case O_ITEM: {
        int g = group_of_kind(g_items[o.i].kind);
        p.t = g == G_CONVS ? O_CONVS : g == G_MSGS ? O_MSGS : O_WIN;
        break;
    }
    case O_BAR_LINE: case O_BAR_BTN: p.t = O_BAR; break;
    default: break;
    }
    return p;
}

static int index_in_parent(obj o) {
    switch (o.t) {
    case O_WIN:      return 0;
    case O_BAR:      return 1 + bars_before(o.slot);
    case O_CONVS:    return 0;
    case O_MSGS:     return 1;
    case O_ITEM:     return group_pos(o.i);
    case O_BAR_LINE: return 0;
    case O_BAR_BTN:  return 1 + o.i;
    default:         return -1;
    }
}

static const char *obj_name(obj o) {
    switch (o.t) {
    case O_ROOT:     return "OpenChime";
    case O_WIN:      return g_win_title[0] ? g_win_title : "OpenChime";
    case O_CONVS:    return "Conversations";
    case O_MSGS:     return "Messages";
    case O_ITEM:     return g_items[o.i].name;
    case O_BAR:      return g_bar[o.slot].name;
    case O_BAR_LINE: return g_bar[o.slot].line;
    case O_BAR_BTN:  return g_bar[o.slot].btn[o.i].name;
    default:         return "";
    }
}

/* The identifier assistive technology locates by (REQ-290): the item's own
 * automation id, and the fixed names the Windows provider gives the parts the
 * provider makes up. The application root has none: it is the process, not a
 * control. */
static void obj_aid(obj o, char *buf, size_t cap) {
    switch (o.t) {
    case O_WIN:      snprintf(buf, cap, "openchime"); break;
    case O_CONVS:    snprintf(buf, cap, "conversations"); break;
    case O_MSGS:     snprintf(buf, cap, "transcript"); break;
    case O_ITEM:     snprintf(buf, cap, "%s", g_items[o.i].aid); break;
    case O_BAR:      snprintf(buf, cap, "%s", g_bar[o.slot].aid); break;
    case O_BAR_LINE: snprintf(buf, cap, "%s.text", g_bar[o.slot].aid); break;
    case O_BAR_BTN:  snprintf(buf, cap, "%s", g_bar[o.slot].btn[o.i].aid); break;
    default:         buf[0] = 0; break;
    }
}

static unsigned obj_role(obj o) {
    switch (o.t) {
    case O_ROOT:     return ROLE_APPLICATION;
    case O_WIN:      return ROLE_FRAME;
    case O_BAR:      return ROLE_FRAME;
    case O_CONVS:    return ROLE_LIST;
    case O_MSGS:     return ROLE_LIST;
    case O_BAR_LINE: return ROLE_LABEL;
    case O_BAR_BTN:  return ROLE_PUSH_BUTTON;
    case O_ITEM:
        switch (g_items[o.i].kind) {
        case OC_ACC_COMPOSER: return ROLE_ENTRY;
        case OC_ACC_BUTTON:   return ROLE_PUSH_BUTTON;
        case OC_ACC_TAB:      return ROLE_PAGE_TAB;
        default:              return ROLE_LIST_ITEM;    /* a conversation, message or pane row */
        }
    default:         return 0;
    }
}
static const char *role_name(unsigned r) {
    switch (r) {
    case ROLE_APPLICATION: return "application";
    case ROLE_FRAME:       return "frame";
    case ROLE_LIST:        return "list";
    case ROLE_LIST_ITEM:   return "list item";
    case ROLE_ENTRY:       return "entry";
    case ROLE_PUSH_BUTTON: return "push button";
    case ROLE_PAGE_TAB:    return "page tab";
    case ROLE_LABEL:       return "label";
    default:               return "unknown";
    }
}

/* What has focus: nothing while the main window lacks it; else the item the
 * application last named (oc_a11y_focus) while it is still on screen; else
 * the composer, where typing goes, as on Windows; else the window itself. */
static obj focus_obj(void) {
    obj o = { O_NONE, 0, 0 };
    if (!g_win_focus) return o;
    if (g_focus_set)
        for (int i = 0; i < g_n_items; i++)
            if (g_items[i].kind == g_focus_kind && g_items[i].id == g_focus_id) { o.t = O_ITEM; o.i = i; return o; }
    int ci = composer_idx();
    if (ci >= 0) { o.t = O_ITEM; o.i = ci; return o; }
    o.t = O_WIN;
    return o;
}

/* A tab of a set says which is the current one in its name, ", selected", as
 * the application names them (a11y.h carries no other mark of it). */
static int name_says_selected(const char *name) {
    static const char suffix[] = ", selected";
    size_t n = strlen(name), s = sizeof suffix - 1;
    return n >= s && !strcmp(name + n - s, suffix);
}

static void obj_states(obj o, uint32_t st[2]) {
#define SET_ST(s) (st[(s) / 32] |= 1u << ((s) % 32))
    st[0] = st[1] = 0;
    if (o.t == O_ROOT || o.t == O_NONE) return;
    SET_ST(ST_SHOWING); SET_ST(ST_VISIBLE);
    if (o.t != O_BAR_BTN || g_bar[o.slot].btn[o.i].invoke) { SET_ST(ST_ENABLED); SET_ST(ST_SENSITIVE); }
    if (o.t == O_WIN) { SET_ST(ST_RESIZABLE); if (g_win_focus) SET_ST(ST_ACTIVE); }
    obj f = focus_obj();
    int focused = f.t != O_NONE && f.t == o.t && f.i == o.i;
    if (o.t == O_ITEM) {
        const oc_acc_item *it = &g_items[o.i];
        int named = g_focus_set && it->kind == g_focus_kind && it->id == g_focus_id;
        if (it->kind == OC_ACC_COMPOSER) {
            SET_ST(ST_EDITABLE); SET_ST(ST_MULTI_LINE); SET_ST(ST_SELECTABLE_TEXT); SET_ST(ST_FOCUSABLE);
        }
        if (named) SET_ST(ST_FOCUSABLE);
        if (it->kind == OC_ACC_CONVERSATION || it->kind == OC_ACC_TAB || it->kind == OC_ACC_LISTITEM) {
            SET_ST(ST_SELECTABLE);
            if (named || (it->kind == OC_ACC_TAB && name_says_selected(it->name))) SET_ST(ST_SELECTED);
        }
    }
    if (focused) { SET_ST(ST_FOCUSABLE); SET_ST(ST_FOCUSED); }
#undef SET_ST
}

/* ---- where things are ---------------------------------------------------------
 * Published in device pixels relative to the window, which are AT-SPI's window
 * coordinates as they are. The screen's are the window's position added,
 * where SDL knows it: under X11; a Wayland client is never told where its
 * window is, so there the two agree. */

typedef struct { int x, y, w, h; } box;

static box obj_box(obj o) {
    box b = { 0, 0, 0, 0 };
    switch (o.t) {
    case O_WIN: b.w = g_win_w; b.h = g_win_h; break;
    case O_CONVS:
    case O_MSGS: {
        /* A list is where its rows are. */
        int g = o.t == O_CONVS ? G_CONVS : G_MSGS, l = 0, t = 0, r = 0, bt = 0, any = 0;
        for (int i = 0; i < g_n_items; i++) {
            const oc_acc_item *it = &g_items[i];
            if (group_of_kind(it->kind) != g) continue;
            if (!any || it->l < l) l = it->l;
            if (!any || it->t < t) t = it->t;
            if (!any || it->r > r) r = it->r;
            if (!any || it->b > bt) bt = it->b;
            any = 1;
        }
        b.x = l; b.y = t; b.w = r - l; b.h = bt - t;
        break;
    }
    case O_ITEM: {
        const oc_acc_item *it = &g_items[o.i];
        b.x = it->l; b.y = it->t; b.w = it->r - it->l; b.h = it->b - it->t;
        break;
    }
    case O_BAR: b.w = g_bar[o.slot].wpx; b.h = g_bar[o.slot].hpx; break;
    case O_BAR_LINE:
        /* The line is the bar left of its first button. */
        b.w = g_bar[o.slot].n_btn ? g_bar[o.slot].btn[0].l : g_bar[o.slot].wpx;
        b.h = g_bar[o.slot].hpx;
        break;
    case O_BAR_BTN: {
        const oc_acc_item *it = &g_bar[o.slot].btn[o.i];
        b.x = it->l; b.y = it->t; b.w = it->r - it->l; b.h = it->b - it->t;
        break;
    }
    default: break;
    }
    return b;
}

/* The screen position of the window an object is drawn in. */
static void window_origin(obj o, int *x, int *y) {
    if (o.t == O_BAR || o.t == O_BAR_LINE || o.t == O_BAR_BTN) { *x = g_bar[o.slot].x; *y = g_bar[o.slot].y; }
    else { *x = g_win_x; *y = g_win_y; }
}

/* The offset that turns window coordinates into `coord`'s. */
static void coord_offset(obj o, unsigned coord, int *dx, int *dy) {
    *dx = *dy = 0;
    if (coord == COORD_SCREEN) window_origin(o, dx, dy);
    else if (coord == COORD_PARENT) {
        obj p = parent_of(o);
        if (p.t != O_ROOT && p.t != O_NONE) { box pb = obj_box(p); *dx = -pb.x; *dy = -pb.y; }
    }
}

static box obj_extents(obj o, unsigned coord) {
    box b = obj_box(o);
    int dx, dy;
    coord_offset(o, coord, &dx, &dy);
    b.x += dx; b.y += dy;
    return b;
}

static int box_has(box b, int x, int y) { return x >= b.x && x < b.x + b.w && y >= b.y && y < b.y + b.h; }

/* The child of `o` under a point given in `coord`, or none. Over the window,
 * an item on a raised layer (a menu's rows) wins over what it covers. */
static obj child_at_point(obj o, int x, int y, unsigned coord) {
    obj none = { O_NONE, 0, 0 };
    int dx, dy;
    coord_offset(o, coord, &dx, &dy);
    x -= dx; y -= dy;
    if (o.t == O_BAR) {
        for (int i = 0; i < g_bar[o.slot].n_btn; i++) {
            obj c = { O_BAR_BTN, i, o.slot };
            if (box_has(obj_box(c), x, y)) return c;
        }
        obj line = { O_BAR_LINE, 0, o.slot };
        return box_has(obj_box(line), x, y) ? line : none;
    }
    if (o.t != O_WIN && o.t != O_CONVS && o.t != O_MSGS) return none;
    int hit = -1;
    for (int i = 0; i < g_n_items; i++) {
        const oc_acc_item *it = &g_items[i];
        if (x < it->l || x >= it->r || y < it->t || y >= it->b) continue;
        if (hit < 0 || it->layer > g_items[hit].layer) hit = i;
    }
    if (hit < 0) return none;
    int g = group_of_kind(g_items[hit].kind);
    obj c = { O_ITEM, hit, 0 };
    if (o.t == O_WIN) {
        if (g == G_CONVS) c.t = O_CONVS;
        else if (g == G_MSGS) c.t = O_MSGS;
        return c;
    }
    return g == (o.t == O_CONVS ? G_CONVS : G_MSGS) ? c : none;
}

/* ---- the composer's text --------------------------------------------------------
 * AT-SPI counts text in characters (code points); the composer is published in
 * UTF-16, so it is decoded for each question, under the lock, into this. */
static uint32_t g_cps[4096];
static int      g_ncp;

static void comp_decode(void) {
    g_ncp = 0;
    for (int i = 0; i < g_comp_len; i++) {
        uint32_t c = g_comp[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < g_comp_len && g_comp[i + 1] >= 0xDC00 && g_comp[i + 1] < 0xE000) {
            c = 0x10000 + ((c - 0xD800) << 10) + (uint32_t)(g_comp[i + 1] - 0xDC00);
            i++;
        }
        g_cps[g_ncp++] = c;
    }
}
/* Characters before a UTF-16 offset. */
static int u16_to_cp(const uint16_t *s, int len, int off) {
    int n = 0;
    if (off > len) off = len;
    for (int i = 0; i < off; i++) {
        if (s[i] >= 0xD800 && s[i] < 0xDC00 && i + 1 < len && s[i + 1] >= 0xDC00 && s[i + 1] < 0xE000) i++;
        n++;
    }
    return n;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

static char *cps_utf8(const uint32_t *cp, int s, int e) {
    GString *str = g_string_new(NULL);
    for (int i = s; i < e; i++) g_string_append_unichar(str, (gunichar)cp[i]);
    return g_string_free(str, FALSE);
}
static GVariant *text_range(int s, int e) {
    char *t = cps_utf8(g_cps, s, e);
    GVariant *v = g_variant_new_string(t);
    g_free(t);
    return v;
}

static int is_space(uint32_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || g_unichar_isspace(c); }

/* AT-SPI's text boundaries (AtspiTextBoundaryType). A sentence is read as a
 * line, as the Windows provider reads a paragraph: the composer has no
 * sentence breaker, and a line is the honest unit nearest one. A line is a
 * logical line, newline to newline, not a wrapped one (win32/a11y.c says why). */
enum { TB_CHAR = 0, TB_WORD_START, TB_WORD_END, TB_SENT_START, TB_SENT_END, TB_LINE_START, TB_LINE_END };

static int is_boundary(int kind, int p) {           /* 0 < p < g_ncp */
    switch (kind) {
    case TB_WORD_START: return !is_space(g_cps[p]) && is_space(g_cps[p - 1]);
    case TB_WORD_END:   return is_space(g_cps[p]) && !is_space(g_cps[p - 1]);
    case TB_SENT_START:
    case TB_LINE_START: return g_cps[p - 1] == '\n';
    case TB_SENT_END:
    case TB_LINE_END:   return g_cps[p] == '\n';
    default:            return 1;
    }
}
/* The unit of `kind` holding `off`: from the boundary at or before it to the
 * next after it. */
static void unit_at(int kind, int off, int *s, int *e) {
    int L = g_ncp;
    off = clampi(off, 0, L);
    int a = off;
    while (a > 0 && a < L && !is_boundary(kind, a)) a--;
    if (a == L && L > 0) { a = L - 1; while (a > 0 && !is_boundary(kind, a)) a--; }
    int b = off + 1;
    while (b < L && !is_boundary(kind, b)) b++;
    *s = a; *e = clampi(b, a, L);
}
/* Before and after: the unit next to the one at `off`, or nothing at the ends. */
static void unit_near(int kind, int off, int dir, int *s, int *e) {
    int a, b;
    unit_at(kind, off, &a, &b);
    if (dir < 0) { if (a > 0) unit_at(kind, a - 1, s, e); else *s = *e = 0; }
    else         { if (b < g_ncp) unit_at(kind, b, s, e); else *s = *e = g_ncp; }
}
/* AT-SPI's granularities (AtspiTextGranularity): a character, the word or
 * the run of spaces holding it, and a line, which stands for the sentence and
 * the paragraph too, for the reason above. */
static void granule_at(int gran, int off, int *s, int *e) {
    int L = g_ncp;
    off = clampi(off, 0, L);
    if (off >= L) { *s = *e = L; return; }
    if (gran == 0) { *s = off; *e = off + 1; return; }
    int a = off, b = off;
    if (gran == 1) {
        int sp = is_space(g_cps[off]);
        while (a > 0 && is_space(g_cps[a - 1]) == sp) a--;
        while (b < L && is_space(g_cps[b]) == sp) b++;
    } else {
        while (a > 0 && g_cps[a - 1] != '\n') a--;
        while (b < L && g_cps[b] != '\n') b++;
    }
    *s = a; *e = b;
}

static void comp_selection(int *s, int *e) {
    int c = u16_to_cp(g_comp, g_comp_len, g_comp_caret), a = u16_to_cp(g_comp, g_comp_len, g_comp_anchor);
    *s = c < a ? c : a;
    *e = c < a ? a : c;
}

/* ---- answering a method call -------------------------------------------------- */

static GVariant *empty_ss(void) { return g_variant_new_array(G_VARIANT_TYPE("{ss}"), NULL, 0); }

static GVariant *obj_attributes(obj o) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{ss}"));
    char aid[OC_ACC_AID_MAX + 8];
    obj_aid(o, aid, sizeof aid);
    if (aid[0]) g_variant_builder_add(&b, "{ss}", "id", aid);
    return g_variant_builder_end(&b);
}

static int obj_has_text(obj o) { return is_item_kind(o, OC_ACC_COMPOSER); }

static GVariant *obj_interfaces(obj o) {
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("as"));
    g_variant_builder_add(&b, "s", I_ACC);
    if (o.t == O_ROOT) g_variant_builder_add(&b, "s", I_APP);
    else g_variant_builder_add(&b, "s", I_COMP);
    if (item_invokable(o)) g_variant_builder_add(&b, "s", I_ACT);
    if (obj_has_text(o)) g_variant_builder_add(&b, "s", I_TEXT);
    return g_variant_builder_end(&b);
}

/* One property, or NULL where `o` has no such property. */
static GVariant *obj_prop(obj o, const char *iface, const char *name) {
    if (!strcmp(iface, I_ACC)) {
        if (!strcmp(name, "Name")) return g_variant_new_string(obj_name(o));
        if (!strcmp(name, "Description") || !strcmp(name, "HelpText") || !strcmp(name, "Locale"))
            return g_variant_new_string("");
        if (!strcmp(name, "ChildCount")) return g_variant_new_int32(child_count(o));
        if (!strcmp(name, "AccessibleId")) {
            char aid[OC_ACC_AID_MAX + 8];
            obj_aid(o, aid, sizeof aid);
            return g_variant_new_string(aid);
        }
        if (!strcmp(name, "Parent")) {
            if (o.t != O_ROOT) return ref_obj(parent_of(o));
            return g_desk_name[0] ? g_variant_new("(so)", g_desk_name, g_desk_path) : ref_null();
        }
        return NULL;
    }
    if (!strcmp(iface, I_APP) && o.t == O_ROOT) {
        if (!strcmp(name, "ToolkitName")) return g_variant_new_string("OpenChime");
#ifdef OC_BUILD_ID
        if (!strcmp(name, "Version")) return g_variant_new_string(OC_BUILD_ID);
#else
        if (!strcmp(name, "Version")) return g_variant_new_string("");
#endif
        if (!strcmp(name, "AtspiVersion")) return g_variant_new_string("2.1");
        if (!strcmp(name, "Id")) return g_variant_new_int32(g_app_id);
        return NULL;
    }
    if (!strcmp(iface, I_ACT) && item_invokable(o)) {
        if (!strcmp(name, "NActions")) return g_variant_new_int32(1);
        return NULL;
    }
    if (!strcmp(iface, I_TEXT) && obj_has_text(o)) {
        comp_decode();
        if (!strcmp(name, "CharacterCount")) return g_variant_new_int32(g_ncp);
        if (!strcmp(name, "CaretOffset")) return g_variant_new_int32(u16_to_cp(g_comp, g_comp_len, g_comp_caret));
        return NULL;
    }
    return NULL;
}

static const char *const PROPS_ACC[]  = { "Name", "Description", "Parent", "ChildCount", "Locale", "AccessibleId", "HelpText", NULL };
static const char *const PROPS_APP[]  = { "ToolkitName", "Version", "AtspiVersion", "Id", NULL };
static const char *const PROPS_ACT[]  = { "NActions", NULL };
static const char *const PROPS_TEXT[] = { "CharacterCount", "CaretOffset", NULL };

static GVariant *obj_all_props(obj o, const char *iface) {
    const char *const *names = !strcmp(iface, I_ACC) ? PROPS_ACC : !strcmp(iface, I_APP) ? PROPS_APP
                             : !strcmp(iface, I_ACT) ? PROPS_ACT : !strcmp(iface, I_TEXT) ? PROPS_TEXT : NULL;
    GVariantBuilder b;
    g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
    for (int i = 0; names && names[i]; i++) {
        GVariant *v = obj_prop(o, iface, names[i]);
        if (v) g_variant_builder_add(&b, "{sv}", names[i], v);
    }
    return g_variant_new("(@a{sv})", g_variant_builder_end(&b));
}

/* What each interface carries, for Introspect: the tools that type their
 * arguments from it (gdbus call, d-feet) need it, and AT-SPI's own clients do
 * not ask. As at-spi2-core's descriptions of the interfaces. */
#define XM(n, a)  "  <method name=\"" n "\">" a "</method>\n"
#define XI(t)     "<arg direction=\"in\" type=\"" t "\"/>"
#define XO(t)     "<arg direction=\"out\" type=\"" t "\"/>"
#define XP(n, t)  "  <property name=\"" n "\" type=\"" t "\" access=\"read\"/>\n"
static const char XML_ACC[] =
    XP("Name", "s") XP("Description", "s") XP("Parent", "(so)") XP("ChildCount", "i") XP("Locale", "s")
    XP("AccessibleId", "s") XP("HelpText", "s")
    XM("GetChildAtIndex", XI("i") XO("(so)")) XM("GetChildren", XO("a(so)")) XM("GetIndexInParent", XO("i"))
    XM("GetRelationSet", XO("a(ua(so))")) XM("GetRole", XO("u")) XM("GetRoleName", XO("s"))
    XM("GetLocalizedRoleName", XO("s")) XM("GetState", XO("au")) XM("GetAttributes", XO("a{ss}"))
    XM("GetApplication", XO("(so)")) XM("GetInterfaces", XO("as"));
static const char XML_APP[] =
    XP("ToolkitName", "s") XP("Version", "s") XP("AtspiVersion", "s")
    "  <property name=\"Id\" type=\"i\" access=\"readwrite\"/>\n"
    XM("GetLocale", XI("u") XO("s"));
static const char XML_COMP[] =
    XM("Contains", XI("i") XI("i") XI("u") XO("b")) XM("GetAccessibleAtPoint", XI("i") XI("i") XI("u") XO("(so)"))
    XM("GetExtents", XI("u") XO("(iiii)")) XM("GetPosition", XI("u") XO("i") XO("i")) XM("GetSize", XO("i") XO("i"))
    XM("GetLayer", XO("u")) XM("GetMDIZOrder", XO("n")) XM("GrabFocus", XO("b")) XM("GetAlpha", XO("d"))
    XM("SetExtents", XI("i") XI("i") XI("i") XI("i") XI("u") XO("b")) XM("SetPosition", XI("i") XI("i") XI("u") XO("b"))
    XM("SetSize", XI("i") XI("i") XO("b")) XM("ScrollTo", XI("u") XO("b")) XM("ScrollToPoint", XI("u") XI("i") XI("i") XO("b"));
static const char XML_ACT[] =
    XP("NActions", "i")
    XM("GetDescription", XI("i") XO("s")) XM("GetName", XI("i") XO("s")) XM("GetLocalizedName", XI("i") XO("s"))
    XM("GetKeyBinding", XI("i") XO("s")) XM("GetActions", XO("a(sss)")) XM("DoAction", XI("i") XO("b"));
static const char XML_TEXT[] =
    XP("CharacterCount", "i") XP("CaretOffset", "i")
    XM("GetStringAtOffset", XI("i") XI("u") XO("s") XO("i") XO("i")) XM("GetText", XI("i") XI("i") XO("s"))
    XM("SetCaretOffset", XI("i") XO("b")) XM("GetTextBeforeOffset", XI("i") XI("u") XO("s") XO("i") XO("i"))
    XM("GetTextAtOffset", XI("i") XI("u") XO("s") XO("i") XO("i"))
    XM("GetTextAfterOffset", XI("i") XI("u") XO("s") XO("i") XO("i")) XM("GetCharacterAtOffset", XI("i") XO("i"))
    XM("GetAttributeValue", XI("i") XI("s") XO("s")) XM("GetAttributes", XI("i") XO("a{ss}") XO("i") XO("i"))
    XM("GetDefaultAttributes", XO("a{ss}")) XM("GetCharacterExtents", XI("i") XI("u") XO("i") XO("i") XO("i") XO("i"))
    XM("GetOffsetAtPoint", XI("i") XI("i") XI("u") XO("i")) XM("GetNSelections", XO("i"))
    XM("GetSelection", XI("i") XO("i") XO("i")) XM("AddSelection", XI("i") XI("i") XO("b"))
    XM("RemoveSelection", XI("i") XO("b")) XM("SetSelection", XI("i") XI("i") XI("i") XO("b"))
    XM("GetRangeExtents", XI("i") XI("i") XI("u") XO("i") XO("i") XO("i") XO("i"))
    XM("GetBoundedRanges", XI("i") XI("i") XI("i") XI("i") XI("u") XI("u") XI("u") XO("a(iisv)"))
    XM("GetAttributeRun", XI("i") XI("b") XO("a{ss}") XO("i") XO("i")) XM("GetDefaultAttributeSet", XO("a{ss}"))
    XM("ScrollSubstringTo", XI("i") XI("i") XI("u") XO("b"))
    XM("ScrollSubstringToPoint", XI("i") XI("i") XI("u") XI("i") XI("i") XO("b"));
#undef XM
#undef XI
#undef XO
#undef XP

static GVariant *obj_introspect(obj o) {
    GString *x = g_string_new("<node>\n");
    GVariant *ifs = g_variant_ref_sink(obj_interfaces(o));
    GVariantIter it;
    const char *name;
    g_variant_iter_init(&it, ifs);
    while (g_variant_iter_next(&it, "&s", &name)) {
        const char *body = !strcmp(name, I_ACC) ? XML_ACC : !strcmp(name, I_APP) ? XML_APP
                         : !strcmp(name, I_COMP) ? XML_COMP : !strcmp(name, I_ACT) ? XML_ACT : XML_TEXT;
        g_string_append_printf(x, " <interface name=\"%s\">\n%s </interface>\n", name, body);
    }
    g_variant_unref(ifs);
    g_string_append(x, "</node>\n");
    char *str = g_string_free(x, FALSE);
    GVariant *v = g_variant_new("(s)", str);
    g_free(str);
    return v;
}

/* The press itself is not performed here: this runs on GDBus's worker
 * thread, and everything the press touches belongs to the application's. The
 * token is posted, as the Windows provider posts it (win32/a11y.c), and the
 * application runs it where it is safe to. */
static int obj_press(obj o) {
    uint64_t tok = o.t == O_ITEM ? g_items[o.i].invoke : o.t == O_BAR_BTN ? g_bar[o.slot].btn[o.i].invoke : 0;
    if (!tok) return 0;
    SDL_Event e;
    memset(&e, 0, sizeof e);
    e.type = oc_plat_event_type();
    e.user.code = OC_PLAT_EV_A11Y_INVOKE;
    e.user.data1 = (void *)(uintptr_t)(tok >> 32);
    e.user.data2 = (void *)(uintptr_t)(tok & 0xFFFFFFFFu);
    return SDL_PushEvent(&e) ? 1 : 0;
}

#define ARGS(sig) (body && g_variant_is_of_type(body, G_VARIANT_TYPE(sig)))
#define MEMBER(m) (!strcmp(member, (m)))
#define IFACE(i)  (!iface || !strcmp(iface, (i)))

/* The answer to one call on `o`, a tuple; NULL with *err set when there is
 * none. Called with the lock held. */
static GVariant *answer(obj o, const char *iface, const char *member, GVariant *body, const char **err) {
    *err = "org.freedesktop.DBus.Error.UnknownMethod";

    if (iface && !strcmp(iface, I_PROPS)) {
        const char *pi, *pn;
        if (MEMBER("Get") && ARGS("(ss)")) {
            g_variant_get(body, "(&s&s)", &pi, &pn);
            GVariant *v = obj_prop(o, pi, pn);
            if (!v) { *err = "org.freedesktop.DBus.Error.UnknownProperty"; return NULL; }
            return g_variant_new("(v)", v);
        }
        if (MEMBER("GetAll") && ARGS("(s)")) {
            g_variant_get(body, "(&s)", &pi);
            return obj_all_props(o, pi);
        }
        if (MEMBER("Set") && ARGS("(ssv)")) {
            GVariant *v;
            g_variant_get(body, "(&s&sv)", &pi, &pn, &v);
            /* The registry numbers the application as it embeds it; nothing
             * else here is a client's to change. */
            int ok = o.t == O_ROOT && !strcmp(pi, I_APP) && !strcmp(pn, "Id") &&
                     g_variant_is_of_type(v, G_VARIANT_TYPE_INT32);
            if (ok) g_app_id = g_variant_get_int32(v);
            g_variant_unref(v);
            if (!ok) { *err = "org.freedesktop.DBus.Error.PropertyReadOnly"; return NULL; }
            return g_variant_new("()");
        }
        return NULL;
    }
    if (iface && !strcmp(iface, I_INTRO)) return MEMBER("Introspect") ? obj_introspect(o) : NULL;

    if (IFACE(I_ACC)) {
        if (MEMBER("GetChildAtIndex") && ARGS("(i)")) {
            int n;
            g_variant_get(body, "(i)", &n);
            obj c = child_at(o, n);
            return g_variant_new("(@(so))", c.t == O_NONE ? ref_null() : ref_obj(c));
        }
        if (MEMBER("GetChildren")) {
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("a(so)"));
            int n = child_count(o);
            for (int i = 0; i < n; i++) {
                obj c = child_at(o, i);
                if (c.t != O_NONE) g_variant_builder_add_value(&b, ref_obj(c));
            }
            return g_variant_new("(@a(so))", g_variant_builder_end(&b));
        }
        if (MEMBER("GetIndexInParent")) return g_variant_new("(i)", index_in_parent(o));
        if (MEMBER("GetRelationSet")) return g_variant_new("(@a(ua(so)))", g_variant_new_array(G_VARIANT_TYPE("(ua(so))"), NULL, 0));
        if (MEMBER("GetRole")) return g_variant_new("(u)", obj_role(o));
        if (MEMBER("GetRoleName") || MEMBER("GetLocalizedRoleName")) return g_variant_new("(s)", role_name(obj_role(o)));
        if (MEMBER("GetState")) {
            uint32_t st[2];
            obj_states(o, st);
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("au"));
            g_variant_builder_add(&b, "u", st[0]);
            g_variant_builder_add(&b, "u", st[1]);
            return g_variant_new("(@au)", g_variant_builder_end(&b));
        }
        if (MEMBER("GetAttributes") && (!body || ARGS("()")))
            return g_variant_new("(@a{ss})", obj_attributes(o));
        if (MEMBER("GetApplication")) return g_variant_new("(@(so))", ref_path(P_ROOT));
        if (MEMBER("GetInterfaces")) return g_variant_new("(@as)", obj_interfaces(o));
        if (iface) return NULL;
    }

    if (IFACE(I_APP) && o.t == O_ROOT) {
        if (MEMBER("GetLocale")) return g_variant_new("(s)", "");
        if (iface) return NULL;
    }

    if (IFACE(I_COMP) && o.t != O_ROOT) {
        if (MEMBER("GetExtents") && ARGS("(u)")) {
            unsigned c;
            g_variant_get(body, "(u)", &c);
            box b = obj_extents(o, c);
            return g_variant_new("((iiii))", b.x, b.y, b.w, b.h);
        }
        if (MEMBER("GetPosition") && ARGS("(u)")) {
            unsigned c;
            g_variant_get(body, "(u)", &c);
            box b = obj_extents(o, c);
            return g_variant_new("(ii)", b.x, b.y);
        }
        if (MEMBER("GetSize")) {
            box b = obj_box(o);
            return g_variant_new("(ii)", b.w, b.h);
        }
        if (MEMBER("Contains") && ARGS("(iiu)")) {
            int x, y;
            unsigned c;
            g_variant_get(body, "(iiu)", &x, &y, &c);
            return g_variant_new("(b)", box_has(obj_extents(o, c), x, y));
        }
        if (MEMBER("GetAccessibleAtPoint") && ARGS("(iiu)")) {
            int x, y;
            unsigned c;
            g_variant_get(body, "(iiu)", &x, &y, &c);
            obj h = child_at_point(o, x, y, c);
            return g_variant_new("(@(so))", h.t == O_NONE ? ref_null() : ref_obj(h));
        }
        if (MEMBER("GetLayer")) {
            unsigned l = (o.t == O_WIN || o.t == O_BAR) ? LAYER_WINDOW
                       : (o.t == O_ITEM && g_items[o.i].layer > 0) ? LAYER_POPUP : LAYER_WIDGET;
            return g_variant_new("(u)", l);
        }
        if (MEMBER("GetMDIZOrder")) return g_variant_new("(n)", (gint16)0);
        if (MEMBER("GetAlpha")) return g_variant_new("(d)", 1.0);
        /* Focus, scrolling and geometry are the application's to move, and
         * its thread's: a client is told no rather than half obeyed. */
        if (MEMBER("GrabFocus") || MEMBER("ScrollTo") || MEMBER("ScrollToPoint") ||
            MEMBER("SetExtents") || MEMBER("SetPosition") || MEMBER("SetSize"))
            return g_variant_new("(b)", FALSE);
        if (iface) return NULL;
    }

    if (IFACE(I_ACT) && item_invokable(o)) {
        int n = -1;
        if (ARGS("(i)")) g_variant_get(body, "(i)", &n);
        /* One action, what pressing the control does (REQ-290). */
        if (MEMBER("GetActions")) {
            GVariantBuilder b;
            g_variant_builder_init(&b, G_VARIANT_TYPE("a(sss)"));
            g_variant_builder_add(&b, "(sss)", "click", "Press", "");
            return g_variant_new("(@a(sss))", g_variant_builder_end(&b));
        }
        if (MEMBER("GetName"))          return g_variant_new("(s)", n == 0 ? "click" : "");
        if (MEMBER("GetLocalizedName")) return g_variant_new("(s)", n == 0 ? "click" : "");
        if (MEMBER("GetDescription"))   return g_variant_new("(s)", n == 0 ? "Press" : "");
        if (MEMBER("GetKeyBinding"))    return g_variant_new("(s)", "");
        if (MEMBER("DoAction"))         return g_variant_new("(b)", n == 0 && obj_press(o));
        if (iface) return NULL;
    }

    if (IFACE(I_TEXT) && obj_has_text(o)) {
        comp_decode();
        int L = g_ncp, s, e;
        if (MEMBER("GetText") && ARGS("(ii)")) {
            g_variant_get(body, "(ii)", &s, &e);
            if (e < 0 || e > L) e = L;
            s = clampi(s, 0, e);
            return g_variant_new("(@s)", text_range(s, e));
        }
        if ((MEMBER("GetTextAtOffset") || MEMBER("GetTextBeforeOffset") || MEMBER("GetTextAfterOffset")) && ARGS("(iu)")) {
            int off;
            unsigned kind;
            g_variant_get(body, "(iu)", &off, &kind);
            if (kind > TB_LINE_END) kind = TB_CHAR;
            if (MEMBER("GetTextAtOffset")) unit_at((int)kind, off, &s, &e);
            else unit_near((int)kind, off, MEMBER("GetTextBeforeOffset") ? -1 : 1, &s, &e);
            return g_variant_new("(@sii)", text_range(s, e), s, e);
        }
        if (MEMBER("GetStringAtOffset") && ARGS("(iu)")) {
            int off;
            unsigned gran;
            g_variant_get(body, "(iu)", &off, &gran);
            granule_at((int)gran, off, &s, &e);
            return g_variant_new("(@sii)", text_range(s, e), s, e);
        }
        if (MEMBER("GetCharacterAtOffset") && ARGS("(i)")) {
            int off;
            g_variant_get(body, "(i)", &off);
            return g_variant_new("(i)", off >= 0 && off < L ? (int)g_cps[off] : 0);
        }
        if (MEMBER("GetNSelections")) {
            comp_selection(&s, &e);
            return g_variant_new("(i)", s != e ? 1 : 0);
        }
        if (MEMBER("GetSelection") && ARGS("(i)")) {
            int n;
            g_variant_get(body, "(i)", &n);
            comp_selection(&s, &e);
            if (n != 0 || s == e) s = e = 0;
            return g_variant_new("(ii)", s, e);
        }
        /* There is no glyph-by-glyph layout to give here, as on Windows:
         * every character answers with the composer's own box. */
        if (MEMBER("GetCharacterExtents") && ARGS("(iu)")) {
            int off;
            unsigned c;
            g_variant_get(body, "(iu)", &off, &c);
            box b = obj_extents(o, c);
            return g_variant_new("(iiii)", b.x, b.y, b.w, b.h);
        }
        if (MEMBER("GetRangeExtents") && ARGS("(iiu)")) {
            unsigned c;
            g_variant_get(body, "(iiu)", &s, &e, &c);
            box b = obj_extents(o, c);
            return g_variant_new("(iiii)", b.x, b.y, b.w, b.h);
        }
        if (MEMBER("GetOffsetAtPoint")) return g_variant_new("(i)", -1);
        if (MEMBER("GetAttributes") && ARGS("(i)")) return g_variant_new("(@a{ss}ii)", empty_ss(), 0, L);
        if (MEMBER("GetAttributeRun") && ARGS("(ib)")) return g_variant_new("(@a{ss}ii)", empty_ss(), 0, L);
        if (MEMBER("GetDefaultAttributes") || MEMBER("GetDefaultAttributeSet")) return g_variant_new("(@a{ss})", empty_ss());
        if (MEMBER("GetAttributeValue")) return g_variant_new("(s)", "");
        if (MEMBER("GetBoundedRanges"))
            return g_variant_new("(@a(iisv))", g_variant_new_array(G_VARIANT_TYPE("(iisv)"), NULL, 0));
        /* Read-only to automation, as the Windows provider's ValuePattern is:
         * moving the caret or the selection from outside would bypass the
         * completion and mention handling that make the composer what it is. */
        if (MEMBER("SetCaretOffset") || MEMBER("AddSelection") || MEMBER("RemoveSelection") ||
            MEMBER("SetSelection") || MEMBER("ScrollSubstringTo") || MEMBER("ScrollSubstringToPoint"))
            return g_variant_new("(b)", FALSE);
        return NULL;
    }
    return NULL;
}

/* The filter that answers every call to an object of ours, on GDBus's worker
 * thread, as the header says. Everything else passes through untouched. */
static GDBusMessage *on_message(GDBusConnection *c, GDBusMessage *m, gboolean incoming, gpointer ud) {
    (void)ud;
    if (!incoming || g_dbus_message_get_message_type(m) != G_DBUS_MESSAGE_TYPE_METHOD_CALL) return m;
    const char *path = g_dbus_message_get_path(m);
    const char *iface = g_dbus_message_get_interface(m);
    const char *member = g_dbus_message_get_member(m);
    if (!path || !member || strncmp(path, P_BASE, strlen(P_BASE)) != 0) return m;
    if (iface && !strcmp(iface, "org.freedesktop.DBus.Peer")) return m;   /* GDBus answers those */

    GVariant *body = g_dbus_message_get_body(m);
    const char *err = NULL;
    g_mutex_lock(&g_lock);
    obj o = resolve(path);
    GVariant *out = o.t == O_NONE ? NULL : answer(o, iface, member, body, &err);
    g_mutex_unlock(&g_lock);
    if (o.t == O_NONE) err = "org.freedesktop.DBus.Error.UnknownObject";

    if (!(g_dbus_message_get_flags(m) & G_DBUS_MESSAGE_FLAGS_NO_REPLY_EXPECTED)) {
        GDBusMessage *r;
        if (out) {
            r = g_dbus_message_new_method_reply(m);
            g_dbus_message_set_body(r, out);
        } else {
            r = g_dbus_message_new_method_error(m, err, "%s.%s is not answered by %s",
                                                iface ? iface : "", member, path);
        }
        g_dbus_connection_send_message(c, r, G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL);
        g_object_unref(r);
    } else if (out) {
        g_variant_unref(g_variant_ref_sink(out));
    }
    g_object_unref(m);
    return NULL;
}

/* ---- events -------------------------------------------------------------------
 * Gathered under the lock as a publish finds them, and sent after it, so the
 * worker answering calls never waits on the bus for the lock. Every AT-SPI
 * event has the one shape: a detail, two numbers, a value, and properties
 * (none are sent). Bounded per publish: one notice of a change is enough for
 * a client to read the list again, and a scroll that replaces every row should
 * not become hundreds of signals a frame. */
enum { EV_MAX = 128, EV_TREE_MAX = 96 };   /* the rest kept for text and focus */
typedef struct {
    char        path[96];
    const char *iface, *member;
    char        detail[24];
    int         d1, d2;
    GVariant   *any;
} a11y_ev;
static a11y_ev g_ev[EV_MAX];
static int     g_n_ev;

static void ev_add(const char *path, const char *iface, const char *member, const char *detail,
                   int d1, int d2, GVariant *any) {
    if (g_n_ev >= EV_MAX) { g_variant_unref(g_variant_ref_sink(any)); return; }
    a11y_ev *e = &g_ev[g_n_ev++];
    snprintf(e->path, sizeof e->path, "%s", path);
    e->iface = iface; e->member = member;
    snprintf(e->detail, sizeof e->detail, "%s", detail);
    e->d1 = d1; e->d2 = d2;
    e->any = g_variant_ref_sink(any);
}

static void ev_flush(void) {
    for (int i = 0; i < g_n_ev; i++) {
        a11y_ev *e = &g_ev[i];
        if (g_bus)
            g_dbus_connection_emit_signal(g_bus, NULL, e->path, e->iface, e->member,
                                          g_variant_new("(siiv@a{sv})", e->detail, e->d1, e->d2, e->any,
                                                        g_variant_new_array(G_VARIANT_TYPE("{sv}"), NULL, 0)),
                                          NULL);
        g_variant_unref(e->any);
    }
    g_n_ev = 0;
}

/* Tell the old holder of focus it lost it and the new one it has it, when
 * they differ. Called with the lock held. */
static void focus_events(void) {
    char now[96] = "";
    obj f = focus_obj();
    if (f.t != O_NONE) obj_path(f, now, sizeof now);
    if (!strcmp(now, g_focus_path)) return;
    if (g_focus_path[0]) ev_add(g_focus_path, I_EVOBJ, "StateChanged", "focused", 0, 0, g_variant_new_int32(0));
    if (now[0])          ev_add(now, I_EVOBJ, "StateChanged", "focused", 1, 0, g_variant_new_int32(0));
    snprintf(g_focus_path, sizeof g_focus_path, "%s", now);
}

/* The children a publish added to and removed from one container, by key. */
typedef struct { uint64_t key; int pos, item; } kpos;
static int kpos_cmp(const void *a, const void *b) {
    uint64_t x = ((const kpos *)a)->key, y = ((const kpos *)b)->key;
    return x < y ? -1 : x > y;
}
static kpos g_old_k[OC_ACC_MAX], g_new_k[OC_ACC_MAX];

static int group_keys(const oc_acc_item *items, const uint64_t *keys, int n, int g, kpos *out) {
    int c = 0;
    for (int i = 0; i < n; i++)
        if (group_of_kind(items[i].kind) == g) { out[c].key = keys[i]; out[c].pos = group_base(g) + c; out[c].item = i; c++; }
    return c;
}

/* Compare the snapshot about to be replaced with the one replacing it, before
 * it is: children gone and come, per container, and names changed. */
static void tree_events(const oc_acc_item *items, const uint64_t *keys, int n) {
    for (int g = 0; g < G_COUNT; g++) {
        int no = group_keys(g_items, g_keys, g_n_items, g, g_old_k);
        int nn = group_keys(items, keys, n, g, g_new_k);
        int same = no == nn;
        for (int i = 0; same && i < nn; i++) same = g_old_k[i].key == g_new_k[i].key;
        if (!same) {
            qsort(g_old_k, (size_t)no, sizeof *g_old_k, kpos_cmp);
            qsort(g_new_k, (size_t)nn, sizeof *g_new_k, kpos_cmp);
        }
        char p[96];
        int a = 0, b = 0;
        while ((a < no || b < nn) && g_n_ev < EV_TREE_MAX) {
            if (b >= nn || (a < no && g_old_k[a].key < g_new_k[b].key)) {
                item_path(p, sizeof p, g_old_k[a].key);
                ev_add(group_path(g), I_EVOBJ, "ChildrenChanged", "remove", g_old_k[a].pos, 0, ref_path(p));
                a++;
            } else if (a >= no || g_new_k[b].key < g_old_k[a].key) {
                item_path(p, sizeof p, g_new_k[b].key);
                ev_add(group_path(g), I_EVOBJ, "ChildrenChanged", "add", g_new_k[b].pos, 0, ref_path(p));
                b++;
            } else {
                const char *was = g_items[g_old_k[a].item].name, *is = items[g_new_k[b].item].name;
                if (strcmp(was, is)) {
                    item_path(p, sizeof p, g_new_k[b].key);
                    ev_add(p, I_EVOBJ, "PropertyChange", "accessible-name", 0, 0, g_variant_new_string(is));
                }
                a++; b++;
            }
        }
    }
}

/* What typing did to the composer, as text inserted and deleted where the two
 * versions part, and where the caret and the selection went. */
static void text_events(const char *path, const uint16_t *nt, int nlen, int ncaret, int nanchor) {
    int p = 0, s = 0;
    while (p < g_comp_len && p < nlen && g_comp[p] == nt[p]) p++;
    while (s < g_comp_len - p && s < nlen - p && g_comp[g_comp_len - 1 - s] == nt[nlen - 1 - s]) s++;
    /* Never part a surrogate pair. */
    if (p > 0 && g_comp[p - 1] >= 0xD800 && g_comp[p - 1] < 0xDC00) p--;
    if (s > 0 && g_comp_len - s < g_comp_len && g_comp[g_comp_len - s] >= 0xDC00 && g_comp[g_comp_len - s] < 0xE000) s--;
    int at = u16_to_cp(g_comp, g_comp_len, p);
    if (p < g_comp_len - s) {
        int a = at, b = u16_to_cp(g_comp, g_comp_len, g_comp_len - s);
        comp_decode();
        char *t = cps_utf8(g_cps, a, b);
        ev_add(path, I_EVOBJ, "TextChanged", "delete", a, b - a, g_variant_new_string(t));
        g_free(t);
    }
    if (p < nlen - s) {
        glong w = 0;
        char *t = g_utf16_to_utf8((const gunichar2 *)(nt + p), nlen - s - p, NULL, &w, NULL);
        ev_add(path, I_EVOBJ, "TextChanged", "insert", at, (int)g_utf8_strlen(t ? t : "", -1),
               g_variant_new_string(t ? t : ""));
        g_free(t);
    }
    int oc = u16_to_cp(g_comp, g_comp_len, g_comp_caret), nc = u16_to_cp(nt, nlen, ncaret);
    int text_moved = p < g_comp_len - s || p < nlen - s;
    if (oc != nc || text_moved) ev_add(path, I_EVOBJ, "TextCaretMoved", "", nc, 0, g_variant_new_int32(0));
    int os, oe, ns = nc < u16_to_cp(nt, nlen, nanchor) ? nc : u16_to_cp(nt, nlen, nanchor);
    int ne = nc < u16_to_cp(nt, nlen, nanchor) ? u16_to_cp(nt, nlen, nanchor) : nc;
    comp_selection(&os, &oe);
    if ((os != oe || ns != ne) && (os != ns || oe != ne))
        ev_add(path, I_EVOBJ, "TextSelectionChanged", "", 0, 0, g_variant_new_int32(0));
}

/* ---- the registry and the bus ---------------------------------------------------- */

/* The accessibility bus: where AT_SPI_BUS_ADDRESS says, as every AT-SPI
 * client and toolkit reads it, else where the session bus's org.a11y.Bus says
 * (asking it starts the bus where it is not running). NULL with neither. */
static char *a11y_bus_address(void) {
    const char *env = getenv("AT_SPI_BUS_ADDRESS");
    if (env && env[0]) return g_strdup(env);
    GDBusConnection *s = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, NULL);
    if (!s) return NULL;
    GVariant *r = g_dbus_connection_call_sync(s, "org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress", NULL,
                                              G_VARIANT_TYPE("(s)"), G_DBUS_CALL_FLAGS_NONE, OC_ATSPI_TIMEOUT_MS,
                                              NULL, NULL);
    g_object_unref(s);
    if (!r) return NULL;
    char *addr = NULL;
    g_variant_get(r, "(s)", &addr);
    g_variant_unref(r);
    if (addr && !addr[0]) { g_free(addr); addr = NULL; }
    return addr;
}

/* Hand the application root to the registry, which makes it a child of the
 * desktop that clients start from, and answers with that desktop. A bus with
 * no registry still has our objects on it, for a client that knows our name;
 * the root's parent is then the null object. */
static void embed(void) {
    GVariant *r = g_dbus_connection_call_sync(g_bus, "org.a11y.atspi.Registry", P_ROOT, "org.a11y.atspi.Socket", "Embed",
                                              g_variant_new("((so))", unique_name(), P_ROOT), G_VARIANT_TYPE("((so))"),
                                              G_DBUS_CALL_FLAGS_NONE, OC_ATSPI_TIMEOUT_MS, NULL, NULL);
    if (!r) return;
    const char *name, *path;
    g_variant_get(r, "((&s&o))", &name, &path);
    g_mutex_lock(&g_lock);
    snprintf(g_desk_name, sizeof g_desk_name, "%s", name);
    snprintf(g_desk_path, sizeof g_desk_path, "%s", path);
    g_mutex_unlock(&g_lock);
    g_variant_unref(r);
}

/* The main window as the filter will describe it. Called on the
 * application's thread, with the lock held. */
static void window_state(void) {
    if (!g_win) return;
    int was = g_win_focus;
    SDL_GetWindowPosition(g_win, &g_win_x, &g_win_y);
    SDL_GetWindowSizeInPixels(g_win, &g_win_w, &g_win_h);
    g_win_focus = (SDL_GetWindowFlags(g_win) & SDL_WINDOW_INPUT_FOCUS) ? 1 : 0;
    const char *t = SDL_GetWindowTitle(g_win);
    snprintf(g_win_title, sizeof g_win_title, "%s", t ? t : "");
    if (was != g_win_focus) {
        ev_add(P_WIN, I_EVWIN, g_win_focus ? "Activate" : "Deactivate", "", 0, 0, g_variant_new_string(""));
        ev_add(P_WIN, I_EVOBJ, "StateChanged", "active", g_win_focus, 0, g_variant_new_int32(0));
    }
}

/* ---- public surface ------------------------------------------------------------ */

void oc_a11y_init(SDL_Window *w) {
    g_win = w;
    if (g_bus) return;
    oc_plat_event_type();                 /* registered here, not on GDBus's thread */
    char *addr = a11y_bus_address();
    if (!addr) return;
    GDBusConnection *c = g_dbus_connection_new_for_address_sync(
        addr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT | G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
        NULL, NULL, NULL);
    g_free(addr);
    if (!c) return;
    /* The bus going away ends accessibility, not the application. */
    g_dbus_connection_set_exit_on_close(c, FALSE);
    g_mutex_lock(&g_lock);
    g_bus = c;
    window_state();
    g_mutex_unlock(&g_lock);
    ev_flush();
    g_filter = g_dbus_connection_add_filter(c, on_message, NULL, NULL);
    embed();
    g_ready = 1;
}

void oc_a11y_shutdown(void) {
    if (!g_bus) return;
    g_ready = 0;
    g_dbus_connection_remove_filter(g_bus, g_filter);
    /* Closing the connection is what tells the registry the application has
     * gone; the filter may still be finishing a call, which reads only the
     * snapshot, and that stays. */
    g_dbus_connection_close_sync(g_bus, NULL, NULL);
    g_mutex_lock(&g_lock);
    GDBusConnection *c = g_bus;
    g_bus = NULL;
    g_mutex_unlock(&g_lock);
    g_object_unref(c);
}

int oc_a11y_available(void) { return g_ready; }

void oc_a11y_publish(const oc_acc_item *items, int n,
                     const uint16_t *composer, int caret, int anchor) {
    if (!g_ready) return;
    static uint64_t keys[OC_ACC_MAX];
    static uint16_t comp[4096];
    if (n < 0) n = 0;
    if (n > OC_ACC_MAX) n = OC_ACC_MAX;
    item_keys(items, n, keys);
    int len = 0;
    if (composer) while (composer[len] && len < (int)(sizeof comp / sizeof comp[0]) - 1) len++;
    if (len) memcpy(comp, composer, (size_t)len * sizeof *comp);
    caret = clampi(caret, 0, len);
    anchor = clampi(anchor, 0, len);

    g_mutex_lock(&g_lock);
    tree_events(items, keys, n);
    /* The composer's text events, where it was and is the same composer. */
    int oci = composer_idx(), nci = -1;
    for (int i = 0; i < n && nci < 0; i++) if (items[i].kind == OC_ACC_COMPOSER) nci = i;
    if (oci >= 0 && nci >= 0 && g_keys[oci] == keys[nci]) {
        char p[96];
        item_path(p, sizeof p, keys[nci]);
        text_events(p, comp, len, caret, anchor);
    }
    memcpy(g_items, items, (size_t)n * sizeof *items);
    memcpy(g_keys, keys, (size_t)n * sizeof *keys);
    g_n_items = n;
    memcpy(g_comp, comp, (size_t)len * sizeof *comp);
    g_comp_len = len; g_comp_caret = caret; g_comp_anchor = anchor;
    window_state();
    focus_events();
    g_mutex_unlock(&g_lock);
    ev_flush();
}

static void announce(const char *utf8, int politeness) {
    g_announced++;
    if (!g_ready || !utf8 || !utf8[0]) return;
    /* An announcement is said for the window, which is always there to say
     * it; the politeness is what makes an assertive one interrupt. */
    ev_add(P_WIN, I_EVOBJ, "Announcement", "", politeness, 0, g_variant_new_string(utf8));
    ev_flush();
}
void oc_a11y_announce(const char *utf8) { announce(utf8, LIVE_POLITE); }
void oc_a11y_announce_assertive(const char *utf8) { announce(utf8, LIVE_ASSERTIVE); }

unsigned oc_a11y_announced(void) { return g_announced; }

void oc_a11y_focus(oc_acc_kind kind, uint64_t id) {
    if (!g_ready) return;
    g_mutex_lock(&g_lock);
    g_focus_set = 1; g_focus_kind = kind; g_focus_id = id;
    focus_events();
    g_mutex_unlock(&g_lock);
    ev_flush();
}

static int bar_slot_of(SDL_Window *w) {
    for (int i = 0; i < BAR_SLOTS; i++) if (g_bar[i].w == w) return i;
    return -1;
}

void oc_a11y_bar_publish(SDL_Window *w, const char *aid, const char *name, const char *line,
                         const oc_acc_item *buttons, int n) {
    if (!g_ready || !w) return;
    if (n < 0) n = 0;
    if (n > BAR_BTNS) n = BAR_BTNS;
    int x = 0, y = 0, wpx = 0, hpx = 0;
    SDL_GetWindowPosition(w, &x, &y);
    SDL_GetWindowSizeInPixels(w, &wpx, &hpx);
    g_mutex_lock(&g_lock);
    int i = bar_slot_of(w), fresh = i < 0;
    if (fresh) for (int k = 0; k < BAR_SLOTS && i < 0; k++) if (!g_bar[k].w) i = k;
    if (i < 0) { g_mutex_unlock(&g_lock); return; }
    bar_slot *b = &g_bar[i];
    char was[BAR_TEXT];
    snprintf(was, sizeof was, "%s", fresh ? "" : b->line);
    if (fresh) { b->w = w; b->gen++; }
    snprintf(b->aid, sizeof b->aid, "%s", aid ? aid : "");
    snprintf(b->name, sizeof b->name, "%s", name ? name : "");
    snprintf(b->line, sizeof b->line, "%s", line ? line : "");
    for (int k = 0; k < n; k++) b->btn[k] = buttons[k];
    b->n_btn = n;
    b->x = x; b->y = y; b->wpx = wpx; b->hpx = hpx;
    obj o = { O_BAR, 0, i };
    if (fresh) {
        ev_add(P_ROOT, I_EVOBJ, "ChildrenChanged", "add", index_in_parent(o), 0, ref_obj(o));
    } else if (strcmp(was, b->line)) {
        obj l = { O_BAR_LINE, 0, i };
        char p[96];
        obj_path(l, p, sizeof p);
        ev_add(p, I_EVOBJ, "PropertyChange", "accessible-name", 0, 0, g_variant_new_string(b->line));
    }
    g_mutex_unlock(&g_lock);
    ev_flush();
}

void oc_a11y_bar_close(SDL_Window *w) {
    if (!g_ready || !w) return;
    g_mutex_lock(&g_lock);
    int i = bar_slot_of(w);
    if (i >= 0) {
        obj o = { O_BAR, 0, i };
        ev_add(P_ROOT, I_EVOBJ, "ChildrenChanged", "remove", index_in_parent(o), 0, ref_obj(o));
        g_bar[i].w = NULL;
        g_bar[i].n_btn = 0;
    }
    g_mutex_unlock(&g_lock);
    ev_flush();
}
