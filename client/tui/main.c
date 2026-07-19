/*
 * OpenChime TUI — the first frontend over the app-core (ARCH-74/75). termbox2
 * gives the cell grid + input; utf8proc gives correct display width so emoji and
 * CJK don't corrupt the layout. A modern paneled layout (the lazygit/k9s idiom):
 * a header bar (workspace · you · presence · connection · unread), three bordered
 * panels — Channels │ Messages │ Members — a status line, an always-ready
 * composer, and a context keybinding hint bar; `?` opens a full help overlay.
 * All state/logic lives in oc_client; this file is pure view + input. Mouse
 * (click a channel/member, wheel to scroll) and machine-local prefs — panel
 * widths, Members visibility, 12/24h time, a default workspace — come from
 * ~/.config/openchime/config (config.c), created with defaults on first run.
 *
 * **Multiple workspaces (REQ-012/013/014/015).** The TUI holds one oc_client per
 * signed-in workspace (g_ws, up to MAX_WS), ticks them all every frame so a
 * background workspace keeps receiving and counting unread, and renders only the
 * active one. ^W opens the switcher: remembered workspaces with their unread +
 * connection state, and an always-present "Log in to new workspace" row. Each
 * session carries its own focused channel, scroll, and half-typed message, so
 * switching away and back doesn't lose work.
 *
 * Usage: openchime-tui [<workspace>] [user:pass]   (login box if omitted)
 *        openchime-tui <host> <port> [user:pass]   (dev/local direct connect)
 *        (credentials also read from $OPENCHIME_CRED = "user:pass")
 *
 * Keys: type + Enter to send · Tab autocompletes the trailing token (commands,
 *       #channels, @users, :emoji:) or, on an empty composer, enters navigation
 *       mode · ↑/↓ + PgUp/PgDn scroll · ? help · Ctrl-Q quit. **Navigation mode**
 *       (Esc from the composer): a panel is focused — Tab cycles Channels /
 *       Messages / Members, j/k select, Esc returns to the composer. On the
 *       Messages panel single keys act on the selected message: Enter/t thread ·
 *       r react (opens a filterable emoji picker) · e edit · x delete · w
 *       who-reacted. Members: Enter opens a DM. **Command palette** (`:` on an empty composer, or Ctrl-K anywhere):
 *       fuzzy-filter every command + jump to any channel/DM; Enter runs it (or
 *       prefills the composer for commands needing an argument). A live
 *       autocomplete strip shows candidates as you type. Commands (on the last
 *       message in the focused channel):
 *       /react <emoji> · /reactions (who reacted to it) · /edit <text> ·
 *       /delete · /thread (open its thread; then Enter posts a reply) ·
 *       /search <query> · /close (leave a thread/search/roster/reactions
 *       overlay) · /create <name> · /join <name> · /leave ·
 *       /who (member roster + presence) · /away · /online · /dm <name> (open a
 *       direct message) · /prefs (notification settings) · /notify
 *       all|mentions|none (this channel's level) · /dnd HH:MM HH:MM | off
 *       (do-not-disturb window) · /set <key> <value> (a synced pref: mouse,
 *       members, time, channels-width, members-width, reset) ·
 *       /profile (your identity modal) · /nick <name> (rename yourself) ·
 *       /passwd <old> <new> (change your password) ·
 *       /role <name> owner|admin|member ·
 *       /invite [admin|member] (mint a token) · /remove <name> (owner/admin) ·
 *       /webhook (list this channel's incoming webhooks) · /webhook create
 *       <label> (mint one; token shown once) · /webhook rm <id> (delete one) ·
 *       /upload <path> (post a file here) · /download <id> [path] (save an
 *       attachment) · /logout (revoke this session and quit).
 */

#define TB_IMPL
#include "termbox2.h"

#include "utf8proc.h"

#include "client.h"
#include "model.h"
#include "resolve.h"    /* workspace -> host:port (REQ-010/011) */
#include "store.h"      /* peek a stored session token (skip the login box) */
#include "secret_backend.h" /* OS keyring for the session token */
#include "config.h"     /* machine-local prefs (mouse, panels, time) */
#include "protocol.h"   /* OC_PRESENCE_*, OC_SESSION_TOKEN_LEN */

#include <ctype.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

#define SIDEBAR_W 22
#define COMPOSER_CAP 1024

/* Machine-local prefs, loaded once in main() (see config.h). */
static const oc_config *g_cfg = NULL;

/* Keys of the portable prefs mirrored into the daemon's synced settings bucket
 * (the daemon-side config layer). Machine-local-only fields (e.g. workspace) are
 * deliberately absent — which server you connect to should not sync. */
#define OC_SYNC_KEY_MOUSE     "mouse"
#define OC_SYNC_KEY_MEMBERS   "members_panel"
#define OC_SYNC_KEY_TIME24    "time_24h"
#define OC_SYNC_KEY_CHWIDTH   "channels_width"
#define OC_SYNC_KEY_MEMWIDTH  "members_width"

/* A synced setting's integer value, or `fallback` when the bucket lacks the key. */
static int synced_int(const oc_model *m, const char *key, int fallback) {
    const char *v = oc_model_setting(m, key);
    if (!v || !v[0]) return fallback;
    return atoi(v);
}

/* Layer the daemon's synced bucket over the machine-local file defaults (`base`):
 * a key present in the bucket wins, else the file/default value stands. Returns 1
 * if the effective mouse setting changed, so the caller re-arms termbox input. */
static int apply_synced_settings(oc_config *cfg, const oc_config *base, const oc_model *m) {
    int old_mouse = cfg->mouse;
    cfg->mouse          = synced_int(m, OC_SYNC_KEY_MOUSE,    base->mouse) ? 1 : 0;
    cfg->members_panel  = synced_int(m, OC_SYNC_KEY_MEMBERS,  base->members_panel);
    cfg->time_24h       = synced_int(m, OC_SYNC_KEY_TIME24,   base->time_24h) ? 1 : 0;
    cfg->channels_width = synced_int(m, OC_SYNC_KEY_CHWIDTH,  base->channels_width);
    cfg->members_width  = synced_int(m, OC_SYNC_KEY_MEMWIDTH, base->members_width);
    if (cfg->members_panel < 0 || cfg->members_panel > 2) cfg->members_panel = base->members_panel;
    if (cfg->channels_width < 10 || cfg->channels_width > 60) cfg->channels_width = base->channels_width;
    if (cfg->members_width  < 10 || cfg->members_width  > 60) cfg->members_width  = base->members_width;
    return cfg->mouse != old_mouse;
}

/* Distinct-ish colors for author nicks, picked by hashing the user id. */
static const uintattr_t NICK_COLORS[] = {
    TB_RED, TB_GREEN, TB_YELLOW, TB_BLUE, TB_MAGENTA, TB_CYAN, TB_WHITE
};
static uintattr_t nick_color(uint64_t uid) {
    return NICK_COLORS[uid % (sizeof NICK_COLORS / sizeof NICK_COLORS[0])];
}

static int cp_width(int32_t cp) {
    int w = utf8proc_charwidth(cp);
    if (w < 0) return 0;         /* control/combining: no advance */
    return w > 2 ? 2 : w;
}

/* Draw a UTF-8 string starting at (x,y), clipped to columns [x, xmax). Advances
 * by each glyph's display width (utf8proc). Returns the next free column. */
static int draw_clip(int x, int y, int xmax, const char *s, uintattr_t fg, uintattr_t bg) {
    utf8proc_ssize_t len = (utf8proc_ssize_t)strlen(s), off = 0;
    while (off < len && x < xmax) {
        int32_t cp;
        utf8proc_ssize_t n = utf8proc_iterate((const utf8proc_uint8_t *)s + off, len - off, &cp);
        if (n <= 0) break;
        off += n;
        int w = cp_width(cp);
        if (w == 0) continue;                 /* skip zero-width (combining/control) */
        if (x + w > xmax) break;              /* a wide glyph won't fit the last cell */
        tb_set_cell(x, y, (uint32_t)cp, fg, bg);
        x += w;
    }
    return x;
}

static void fill_row(int y, int x0, int x1, uintattr_t bg) {
    for (int x = x0; x < x1; x++) tb_set_cell(x, y, ' ', TB_DEFAULT, bg);
}

/* ---- a rebuilt-each-frame list of wrapped display rows for the message pane -- */

typedef struct { char *s; uintattr_t fg; int mi; } row_t;   /* mi = message index (-1 = none) */
typedef struct { row_t *v; size_t n, cap; } rows_t;

static void rows_push(rows_t *r, char *s, uintattr_t fg) {
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 64;
        row_t *nv = realloc(r->v, cap * sizeof *nv);
        if (!nv) { free(s); return; }
        r->v = nv; r->cap = cap;
    }
    r->v[r->n].s = s; r->v[r->n].fg = fg; r->v[r->n].mi = -1; r->n++;
}
static void rows_free(rows_t *r) {
    for (size_t i = 0; i < r->n; i++) free(r->v[i].s);
    free(r->v);
    r->v = NULL; r->n = r->cap = 0;
}

/* Wrap `text` to `width` display columns, pushing each row (with `fg`). A
 * continuation row is prefixed with `indent` spaces. */
static void wrap_push(rows_t *r, const char *text, uintattr_t fg, int width, int indent) {
    if (width < 4) width = 4;
    utf8proc_ssize_t len = (utf8proc_ssize_t)strlen(text), off = 0;
    int first = 1;
    do {
        int avail = width - (first ? 0 : indent);
        if (avail < 1) avail = 1;
        int used = 0;
        utf8proc_ssize_t start = off;
        while (off < len) {
            int32_t cp;
            utf8proc_ssize_t n = utf8proc_iterate((const utf8proc_uint8_t *)text + off, len - off, &cp);
            if (n <= 0) { off = len; break; }
            int w = cp_width(cp);
            if (used + w > avail) break;
            used += w; off += n;
        }
        size_t blen = (size_t)(off - start);
        char *row = malloc((size_t)indent + blen + 1);
        if (!row) return;
        size_t p = 0;
        if (!first) for (int i = 0; i < indent; i++) row[p++] = ' ';
        memcpy(row + p, text + start, blen); p += blen;
        row[p] = '\0';
        rows_push(r, row, fg);
        first = 0;
        if (off == start) break;              /* no progress (avail too small) — stop */
    } while (off < len);
}

/* Forward decl: resolve a display name for a message's author. */
static const char *name_for(const oc_channel *ch, uint64_t uid);

/* Fill `out` with the nick to show for message `m`: the name the daemon stamped,
 * "you" for self, a name resolved from the channel (thread replies carry no
 * name), else "user<id>". */
static void msg_nick(char *out, size_t cap, const oc_msg *m, uint64_t me, const oc_channel *ch) {
    if (m->author_name[0]) { snprintf(out, cap, "%s", m->author_name); return; }
    if (m->author_id == me) { snprintf(out, cap, "you"); return; }
    const char *nm = ch ? name_for(ch, m->author_id) : "someone";
    if (strcmp(nm, "someone") != 0) { snprintf(out, cap, "%s", nm); return; }
    snprintf(out, cap, "user%llu", (unsigned long long)m->author_id);
}

/* Append the wrapped rows for one message: header, body (or tombstone),
 * reactions, and — when `show_replies` — a thread reply-count marker. `ch` is
 * used only to resolve author names. */
static void append_msg_rows(rows_t *r, const oc_msg *m, uint64_t me, int width,
                            const oc_channel *ch, int show_replies, int mi) {
    size_t rstart = r->n;
    char line[256], nick[80];
    char stamp[12] = "--:--";
    if (m->server_time) {
        time_t t = (time_t)(m->server_time / 1000);
        struct tm tmv;
        if (localtime_r(&t, &tmv))
            strftime(stamp, sizeof stamp, (g_cfg && !g_cfg->time_24h) ? "%I:%M%p" : "%H:%M", &tmv);
    }
    msg_nick(nick, sizeof nick, m, me, ch);
    snprintf(line, sizeof line, "%s  %s", stamp, nick);
    if (m->edited && !m->deleted) {
        size_t l = strlen(line);
        snprintf(line + l, sizeof line - l, " (edited)");
    }
    char *hdr = malloc(strlen(line) + 1);
    if (hdr) { strcpy(hdr, line); rows_push(r, hdr, nick_color(m->author_id) | TB_BOLD); }
    if (m->deleted) {                     /* tombstone: no body, no reactions */
        char *d = malloc(sizeof "    [message deleted]");
        if (d) { strcpy(d, "    [message deleted]"); rows_push(r, d, TB_DEFAULT); }
        for (size_t k = rstart; k < r->n; k++) r->v[k].mi = mi;
        return;
    }
    wrap_push(r, m->body ? m->body : "", TB_DEFAULT, width, 4);
    for (uint8_t k = 0; k < m->n_attach; k++) {           /* attachments (REQ-140) */
        const oc_attachment *a = &m->attach[k];
        char al[256];
        /* 📎 filename (12.3 KB) #id — download with /download <id> */
        double kb = (double)a->size / 1024.0;
        if (a->size >= 1024ull * 1024)
            snprintf(al, sizeof al, "    \xf0\x9f\x93\x8e %s (%.1f MB) #%llu",
                     a->filename[0] ? a->filename : "file", kb / 1024.0, (unsigned long long)a->id);
        else
            snprintf(al, sizeof al, "    \xf0\x9f\x93\x8e %s (%.1f KB) #%llu",
                     a->filename[0] ? a->filename : "file", kb, (unsigned long long)a->id);
        char *ar = malloc(strlen(al) + 1);
        if (ar) { strcpy(ar, al); rows_push(r, ar, TB_CYAN | TB_BOLD); }
    }
    if (m->n_reactions) {
        char rl[256]; size_t p = 0;
        p += (size_t)snprintf(rl, sizeof rl, "    ");
        for (uint8_t k = 0; k < m->n_reactions && p < sizeof rl - 1; k++) {
            const oc_reaction *rx = &m->reactions[k];
            int w = rx->mine
                ? snprintf(rl + p, sizeof rl - p, "%s [%u]  ", rx->emoji, rx->count)
                : snprintf(rl + p, sizeof rl - p, "%s %u  ", rx->emoji, rx->count);
            if (w < 0 || (size_t)w >= sizeof rl - p) break;
            p += (size_t)w;
        }
        char *rr = malloc(p + 1);
        if (rr) { memcpy(rr, rl, p); rr[p] = '\0'; rows_push(r, rr, TB_MAGENTA); }
    }
    if (show_replies && m->reply_count > 0) {
        char rl[64];
        snprintf(rl, sizeof rl, "    \xe2\x86\xb3 %u %s", m->reply_count,
                 m->reply_count == 1 ? "reply" : "replies");
        char *rr = malloc(strlen(rl) + 1);
        if (rr) { strcpy(rr, rl); rows_push(r, rr, TB_CYAN); }
    }
    for (size_t k = rstart; k < r->n; k++) r->v[k].mi = mi;   /* tag rows with the message */
}

/* Build the wrapped rows for a channel's messages (oldest→newest), then a
 * "seen by …" footer (REQ-090) naming the other members whose read cursor has
 * reached the last message. */
static void build_rows(rows_t *r, const oc_model *m, const oc_channel *ch, uint64_t me, int width) {
    for (size_t i = 0; i < ch->n_msgs; i++)
        append_msg_rows(r, &ch->msgs[i], me, width, ch, 1, (int)i);
    if (ch->n_msgs == 0) return;
    uint64_t last = ch->msgs[ch->n_msgs - 1].message_id;
    char names[192]; size_t p = 0; int shown = 0;
    for (size_t i = 0; i < ch->n_readers; i++) {
        if (ch->readers[i].user_id == me || ch->readers[i].message_id < last) continue;
        const char *nm = oc_model_user_name(m, ch->readers[i].user_id);   /* roster first */
        if (!nm || !nm[0]) nm = name_for(ch, ch->readers[i].user_id);     /* else a message author */
        int w = snprintf(names + p, sizeof names - p, "%s%s", shown ? ", " : "", nm);
        if (w < 0 || (size_t)w >= sizeof names - p) break;
        p += (size_t)w; shown++;
    }
    if (shown) {
        char line[224];
        snprintf(line, sizeof line, "    \xe2\x9c\x93 seen by %s", names);   /* ✓ */
        wrap_push(r, line, TB_GREEN, width, 6);
    }
}

/* Build the rows for the open thread: the parent (found in `ch`) then replies. */
static void build_thread_rows(rows_t *r, const oc_model *m, const oc_channel *ch,
                              uint64_t me, int width) {
    if (ch) for (size_t i = 0; i < ch->n_msgs; i++)
        if (ch->msgs[i].message_id == m->thread_parent) {
            append_msg_rows(r, &ch->msgs[i], me, width, ch, 0, -1);
            break;
        }
    for (size_t i = 0; i < m->n_thread_msgs; i++)
        append_msg_rows(r, &m->thread_msgs[i], me, width, ch, 0, -1);
}

/* ---- rendering ------------------------------------------------------------- */

/* A display name for `uid`, taken from that user's most recent named message in
 * the channel; "someone" if we've never seen them post. */
static const char *name_for(const oc_channel *ch, uint64_t uid) {
    for (size_t i = ch->n_msgs; i > 0; i--)
        if (ch->msgs[i - 1].author_id == uid && ch->msgs[i - 1].author_name[0])
            return ch->msgs[i - 1].author_name;
    return "someone";
}

/* Build the rows for the open search view: a header + each hit (channel, time,
 * author, snippet). */
static void build_search_rows(rows_t *r, const oc_model *m, int width) {
    char line[200];
    snprintf(line, sizeof line, "%zu result%s for \"%s\"", m->n_search,
             m->n_search == 1 ? "" : "s", m->search_query);
    char *h = malloc(strlen(line) + 1);
    if (h) { strcpy(h, line); rows_push(r, h, TB_YELLOW | TB_BOLD); }
    for (size_t i = 0; i < m->n_search; i++) {
        const oc_search_result *s = &m->search_results[i];
        const char *cn = "?";
        for (size_t j = 0; j < m->n_channels; j++)
            if (m->channels[j].channel_id == s->channel_id) { cn = m->channels[j].name ? m->channels[j].name : "…"; break; }
        char stamp[8] = "--:--";
        if (s->server_time) { time_t t = (time_t)(s->server_time / 1000); struct tm tv;
            if (localtime_r(&t, &tv)) strftime(stamp, sizeof stamp, "%H:%M", &tv); }
        snprintf(line, sizeof line, "#%s  %s  user%llu:", cn, stamp, (unsigned long long)s->author_id);
        char *hd = malloc(strlen(line) + 1);
        if (hd) { strcpy(hd, line); rows_push(r, hd, TB_CYAN); }
        wrap_push(r, s->snippet ? s->snippet : "", TB_DEFAULT, width, 4);
    }
}

/* Build the rows for the roster overlay: each member with a presence dot,
 * colored by presence, plus a role tag. */
static void build_roster_rows(rows_t *r, const oc_model *m, int width) {
    (void)width;
    char line[200];
    snprintf(line, sizeof line, "%zu member%s", m->n_users, m->n_users == 1 ? "" : "s");
    char *h = malloc(strlen(line) + 1);
    if (h) { strcpy(h, line); rows_push(r, h, TB_YELLOW | TB_BOLD); }
    /* Surface the last-minted invite token (shown once) at the top of the roster. */
    if (m->invite_token[0]) {
        snprintf(line, sizeof line, "invite (%s): %s",
                 m->invite_role == OC_ROLE_ADMIN ? "admin" : "member", m->invite_token);
        char *iv = malloc(strlen(line) + 1);
        if (iv) { strcpy(iv, line); rows_push(r, iv, TB_MAGENTA | TB_BOLD); }
    }
    for (size_t i = 0; i < m->n_users; i++) {
        const oc_member *u = &m->users[i];
        uint8_t pr = oc_model_presence_of(m, u->user_id);
        const char *dot = pr == OC_PRESENCE_ONLINE ? "\xe2\x97\x8f"    /* ● */
                        : pr == OC_PRESENCE_AWAY   ? "\xe2\x97\x90"    /* ◐ */
                                                   : "\xe2\x97\x8b";   /* ○ */
        const char *role = u->role == OC_ROLE_OWNER ? "  (owner)"
                         : u->role == OC_ROLE_ADMIN ? "  (admin)" : "";
        snprintf(line, sizeof line, "%s %s%s%s", dot, u->name[0] ? u->name : "?",
                 role, u->disabled ? "  (disabled)" : "");
        uintattr_t col = pr == OC_PRESENCE_ONLINE ? TB_GREEN
                       : pr == OC_PRESENCE_AWAY   ? TB_YELLOW : TB_DEFAULT;
        char *rr = malloc(strlen(line) + 1);
        if (rr) { strcpy(rr, line); rows_push(r, rr, col); }
    }
}

/* Build the rows for the "who reacted" overlay: each reactor as "<emoji> name",
 * grouped in the emoji-then-user order the server returns. */
static void build_reactlist_rows(rows_t *r, const oc_model *m, int width) {
    (void)width;
    char line[160];
    snprintf(line, sizeof line, "%zu reaction%s", m->n_reactors,
             m->n_reactors == 1 ? "" : "s");
    char *h = malloc(strlen(line) + 1);
    if (h) { strcpy(h, line); rows_push(r, h, TB_YELLOW | TB_BOLD); }
    for (size_t i = 0; i < m->n_reactors; i++) {
        const oc_reactor_row *rr = &m->reactors[i];
        const char *nm = oc_model_user_name(m, rr->user_id);
        if (nm[0]) snprintf(line, sizeof line, "%s  %s", rr->emoji, nm);
        else       snprintf(line, sizeof line, "%s  user%llu", rr->emoji,
                            (unsigned long long)rr->user_id);
        char *rw = malloc(strlen(line) + 1);
        if (rw) { strcpy(rw, line); rows_push(r, rw, TB_DEFAULT); }
    }
}

/* Human label for a per-channel notification level. */
static const char *notify_label(uint8_t level) {
    return level == OC_NOTIFY_NONE     ? "none"
         : level == OC_NOTIFY_MENTIONS ? "mentions" : "all";
}

/* Build the rows for the notification-prefs overlay: the DND window, then each
 * channel with a non-default level (default = all, elided to keep it short). */
static void build_prefs_rows(rows_t *r, const oc_model *m, int width) {
    (void)width;
    char line[160];
    if (m->dnd_enabled)
        snprintf(line, sizeof line, "do-not-disturb: %02u:%02u\xe2\x80\x93%02u:%02u",
                 m->dnd_start_min / 60, m->dnd_start_min % 60,
                 m->dnd_end_min / 60, m->dnd_end_min % 60);
    else
        snprintf(line, sizeof line, "do-not-disturb: off");
    char *h = malloc(strlen(line) + 1);
    if (h) { strcpy(h, line); rows_push(r, h, TB_YELLOW | TB_BOLD); }

    int any = 0;
    for (size_t i = 0; i < m->n_channels; i++) {
        const oc_channel *c = &m->channels[i];
        if (c->notify_level == OC_NOTIFY_ALL) continue;   /* default — elide */
        any = 1;
        const char *nm = c->name ? c->name : "…";
        if (c->kind == OC_CHANNEL_KIND_DM) {
            const char *pn = oc_model_user_name(m, c->peer_id);
            snprintf(line, sizeof line, "@%s  %s", pn[0] ? pn : "dm", notify_label(c->notify_level));
        } else {
            snprintf(line, sizeof line, "#%s  %s", nm, notify_label(c->notify_level));
        }
        uintattr_t col = c->notify_level == OC_NOTIFY_NONE ? TB_BLACK | TB_BOLD : TB_DEFAULT;
        char *rw = malloc(strlen(line) + 1);
        if (rw) { strcpy(rw, line); rows_push(r, rw, col); }
    }
    if (!any) {
        const char *msg = "all channels: all";
        char *rw = malloc(strlen(msg) + 1);
        if (rw) { strcpy(rw, msg); rows_push(r, rw, TB_DEFAULT); }
    }
}

/* Build the rows for the incoming-webhook overlay (REQ-170): a header, the
 * last-minted token (shown once), then each webhook as "#id  label". */
static void build_webhooks_rows(rows_t *r, const oc_model *m, int width) {
    (void)width;
    char line[200];
    snprintf(line, sizeof line, "%zu webhook%s  (/webhook create <label>, /webhook rm <id>)",
             m->n_webhooks, m->n_webhooks == 1 ? "" : "s");
    char *h = malloc(strlen(line) + 1);
    if (h) { strcpy(h, line); rows_push(r, h, TB_YELLOW | TB_BOLD); }
    /* Surface the last-minted token (shown once) below the header. */
    if (m->webhook_token[0]) {
        snprintf(line, sizeof line, "new #%llu token: %s",
                 (unsigned long long)m->webhook_new_id, m->webhook_token);
        char *tk = malloc(strlen(line) + 1);
        if (tk) { strcpy(tk, line); rows_push(r, tk, TB_MAGENTA | TB_BOLD); }
    }
    for (size_t i = 0; i < m->n_webhooks; i++) {
        const oc_webhook_view *w = &m->webhooks[i];
        snprintf(line, sizeof line, "#%llu  %s%s", (unsigned long long)w->webhook_id,
                 w->label[0] ? w->label : "(webhook)", w->disabled ? "  (disabled)" : "");
        uintattr_t col = w->disabled ? TB_BLACK | TB_BOLD : TB_DEFAULT;
        char *rw = malloc(strlen(line) + 1);
        if (rw) { strcpy(rw, line); rows_push(r, rw, col); }
    }
}

/* ---- workspace sessions (REQ-013/014) ---------------------------------------
 * The TUI holds one oc_client per signed-in workspace, all ticking every frame,
 * so a workspace the user isn't looking at still receives messages and counts
 * unread (REQ-014). Only the active one is rendered. Each session also carries
 * its own view state — focused channel, scroll, half-typed message — so
 * switching away and back doesn't discard work in progress (REQ-015).
 *
 * The cap is a fixed array rather than a grown list: a human signs into a
 * handful of workspaces, and every session costs a thread + a TLS connection. */
#define MAX_WS 8

typedef struct {
    oc_client *cl;
    char       key[288];              /* "host:port" — the store/book key */
    char       label[256];            /* what the user typed, e.g. acme.example.com */
    char       user[128];             /* the account signed in as */
    /* Per-workspace view state, saved on switch-away and restored on return. */
    char       composer[COMPOSER_CAP];
    size_t     clen;
    size_t     focus;
    int        scroll;
    uint64_t   last_focus_cid;
    int        settings_req;          /* pulled the synced bucket this session */
} ws_session;

static ws_session g_ws[MAX_WS];
static int        g_nws = 0;          /* live sessions */
static int        g_active = 0;       /* index into g_ws */

/* The workspace label shown in the header (the active session's). */
static const char *active_label(void) {
    return (g_nws && g_active < g_nws && g_ws[g_active].label[0])
         ? g_ws[g_active].label : "—";
}

/* Total unread across a workspace's channels — what the switcher badges. */
static int ws_unread(const ws_session *w) {
    if (!w->cl) return 0;
    const oc_model *m = oc_client_model(w->cl);
    int n = 0;
    for (size_t i = 0; i < m->n_channels; i++) n += m->channels[i].unread;
    return n;
}

/* Index of the open session for `key`, or -1. */
static int ws_find(const char *key) {
    for (int i = 0; i < g_nws; i++)
        if (strcmp(g_ws[i].key, key) == 0) return i;
    return -1;
}

/* Compute panel columns from the terminal width + config (shared by render and
 * the mouse handler so clicks map to what's drawn). */
static void layout(int W, int *ch_w, int *mem_w, int *msg_x, int *msg_w) {
    int cw = g_cfg ? g_cfg->channels_width : 22;
    if (cw < 8) cw = 8;
    if (cw > W / 3) cw = W / 3;
    int want = g_cfg ? g_cfg->members_width : 22;
    int mode = g_cfg ? g_cfg->members_panel : 2;
    int mw = (mode == 0) ? 0 : (mode == 1) ? want : (W >= 74 ? want : 0);
    if (mw > W / 3) mw = W / 3;
    if (W - cw - mw < 24) mw = 0;             /* keep the message pane usable */
    *ch_w = cw; *mem_w = mw; *msg_x = cw; *msg_w = W - cw - mw;
}

/* A bordered, titled panel; content is drawn inside by the caller. An active
 * panel gets a bright (cyan) border, matching the lazygit/k9s idiom. */
static void draw_panel(int x, int y, int w, int h, const char *title, int active) {
    if (w < 2 || h < 2) return;
    uintattr_t bc = active ? (TB_CYAN | TB_BOLD) : TB_WHITE;
    for (int i = 1; i < w - 1; i++) {
        tb_set_cell(x + i, y, 0x2500, bc, TB_DEFAULT);
        tb_set_cell(x + i, y + h - 1, 0x2500, bc, TB_DEFAULT);
    }
    for (int j = 1; j < h - 1; j++) {
        tb_set_cell(x, y + j, 0x2502, bc, TB_DEFAULT);
        tb_set_cell(x + w - 1, y + j, 0x2502, bc, TB_DEFAULT);
    }
    tb_set_cell(x, y, 0x250c, bc, TB_DEFAULT);
    tb_set_cell(x + w - 1, y, 0x2510, bc, TB_DEFAULT);
    tb_set_cell(x, y + h - 1, 0x2514, bc, TB_DEFAULT);
    tb_set_cell(x + w - 1, y + h - 1, 0x2518, bc, TB_DEFAULT);
    if (title && title[0]) {
        char t[96]; snprintf(t, sizeof t, " %s ", title);
        draw_clip(x + 2, y, x + w - 1, t, active ? TB_CYAN | TB_BOLD : TB_WHITE | TB_BOLD, TB_DEFAULT);
    }
}

/* The ? help overlay: a centered popup listing keys + commands. */
static void draw_help(int W, int H) {
    static const char *L[] = {
        "Tab            next channel",
        "↑ ↓ PgUp PgDn  scroll the message pane",
        "type + Enter   send a message",
        "/command       run a command (below)   ·   ^K palette   ^R reconnect   ^Q quit",
        "^W             switch workspace (or add one)",
        "",
        "Channels & DMs:",
        "  /join <name>  /leave  /create <name>  /list  /dm <name>",
        "Messages (last message):",
        "  /react <emoji>  /reactions  /edit <text>  /delete  /thread",
        "  /search <query>  /close",
        "People & notifications:",
        "  /who  /away  /online  /prefs  /notify <lvl>  /dnd <win|off>",
        "Profile (yourself):",
        "  /profile  /nick <name>  /passwd <old> <new>",
        "Preferences (synced):",
        "  /set mouse|members|time|channels-width|members-width <value>",
        "Files, webhooks, admin:",
        "  /upload <path>  /download <id>  /webhook  /invite  /role  /remove",
        "  /storage — disk usage, retention policy, what was reclaimed (admin)",
        "Workspaces:",
        "  ^W or /workspaces — switch, add, or forget (d) a workspace",
        "Session:",
        "  /logout",
    };
    int n = (int)(sizeof L / sizeof L[0]);
    int bw = 64; if (bw > W - 4) bw = W - 4; if (bw < 24) bw = 24;
    int bh = n + 2; if (bh > H - 2) bh = H - 2; if (bh < 4) bh = 4;
    int x = (W - bw) / 2, y = (H - bh) / 2; if (x < 0) x = 0; if (y < 0) y = 0;
    for (int j = 0; j < bh; j++) fill_row(y + j, x, x + bw, TB_DEFAULT);   /* clear */
    draw_panel(x, y, bw, bh, "Help  ·  ? or Esc to close", 1);
    for (int i = 0; i < n && i + 1 < bh - 1; i++) {
        size_t len = strlen(L[i]);
        uintattr_t col = (len && L[i][len - 1] == ':') ? (TB_YELLOW | TB_BOLD)
                       : (L[i][0] == '/' || L[i][0] == ' ') ? TB_DEFAULT : (TB_WHITE | TB_BOLD);
        draw_clip(x + 2, y + 1 + i, x + bw - 1, L[i], col, TB_DEFAULT);
    }
}

/* The /profile modal (REQ-020): your identity + how to change it. Drawn over the
 * presented frame like the palette/picker, so it self-presents. */
static void draw_profile(const oc_model *m, int W, int H) {
    const char *me = m->user_id ? oc_model_user_name(m, m->user_id) : "";
    uint8_t role = OC_ROLE_MEMBER;
    for (size_t i = 0; i < m->n_users; i++)
        if (m->users[i].user_id == m->user_id) { role = m->users[i].role; break; }
    uint8_t pr = oc_model_presence_of(m, m->user_id);
    const char *rn = role == OC_ROLE_OWNER ? "owner" : role == OC_ROLE_ADMIN ? "admin" : "member";
    const char *pn = pr == OC_PRESENCE_ONLINE ? "online" : pr == OC_PRESENCE_AWAY ? "away" : "offline";
    char L[6][96];
    snprintf(L[0], sizeof L[0], "Name      %s", me[0] ? me : "(unknown)");
    snprintf(L[1], sizeof L[1], "User ID   %llu", (unsigned long long)m->user_id);
    snprintf(L[2], sizeof L[2], "Role      %s", rn);
    snprintf(L[3], sizeof L[3], "Presence  %s", pn);
    snprintf(L[4], sizeof L[4], "%s", "");
    snprintf(L[5], sizeof L[5], "%s", "/nick <name>   ·   /passwd <old> <new>");
    int n = 6;
    int bw = 52; if (bw > W - 4) bw = W - 4; if (bw < 24) bw = 24;
    int bh = n + 2; if (bh > H - 2) bh = H - 2; if (bh < 4) bh = 4;
    int x = (W - bw) / 2, y = (H - bh) / 2; if (x < 0) x = 0; if (y < 0) y = 0;
    for (int j = 0; j < bh; j++) fill_row(y + j, x, x + bw, TB_DEFAULT);
    draw_panel(x, y, bw, bh, "Profile  ·  any key to close", 1);
    for (int i = 0; i < n && i + 1 < bh - 1; i++) {
        uintattr_t col = L[i][0] == '/' ? TB_CYAN : (TB_WHITE | TB_BOLD);
        draw_clip(x + 2, y + 1 + i, x + bw - 1, L[i], col, TB_DEFAULT);
    }
    tb_present();
}

/* ---- composer autocomplete (commands, channels, @users, :emoji:) ----------- */

typedef struct { char repl[80]; char disp[96]; } ac_cand;

static int ci_prefix(const char *s, const char *pre) {
    for (; *pre; s++, pre++)
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*pre)) return 0;
    return 1;
}

static const char *AC_COMMANDS[] = {
    "/react", "/reactions", "/edit", "/delete", "/thread", "/search", "/close",
    "/create", "/join", "/leave", "/list", "/dm", "/who", "/away", "/online",
    "/prefs", "/notify", "/dnd", "/set", "/profile", "/nick", "/passwd", "/workspaces", "/storage",
    "/role", "/invite", "/remove", "/webhook",
    "/upload", "/download", "/logout", "/help",
};

static const struct { const char *name; const char *emoji; } AC_EMOJI[] = {
    {"+1","\xf0\x9f\x91\x8d"}, {"-1","\xf0\x9f\x91\x8e"}, {"thumbsup","\xf0\x9f\x91\x8d"},
    {"heart","\xe2\x9d\xa4\xef\xb8\x8f"}, {"fire","\xf0\x9f\x94\xa5"}, {"tada","\xf0\x9f\x8e\x89"},
    {"smile","\xf0\x9f\x98\x84"}, {"laughing","\xf0\x9f\x98\x86"}, {"joy","\xf0\x9f\x98\x82"},
    {"grin","\xf0\x9f\x98\x81"}, {"wink","\xf0\x9f\x98\x89"}, {"thinking","\xf0\x9f\xa4\x94"},
    {"eyes","\xf0\x9f\x91\x80"}, {"100","\xf0\x9f\x92\xaf"}, {"clap","\xf0\x9f\x91\x8f"},
    {"pray","\xf0\x9f\x99\x8f"}, {"rocket","\xf0\x9f\x9a\x80"}, {"star","\xe2\xad\x90"},
    {"sparkles","\xe2\x9c\xa8"}, {"check","\xe2\x9c\x85"}, {"x","\xe2\x9d\x8c"},
    {"warning","\xe2\x9a\xa0\xef\xb8\x8f"}, {"bulb","\xf0\x9f\x92\xa1"}, {"bug","\xf0\x9f\x90\x9b"},
    {"wave","\xf0\x9f\x91\x8b"}, {"ok","\xf0\x9f\x91\x8c"}, {"muscle","\xf0\x9f\x92\xaa"},
    {"cry","\xf0\x9f\x98\xa2"}, {"sob","\xf0\x9f\x98\xad"}, {"sweat","\xf0\x9f\x98\x85"},
    {"sunglasses","\xf0\x9f\x98\x8e"}, {"ghost","\xf0\x9f\x91\xbb"}, {"skull","\xf0\x9f\x92\x80"},
    {"coffee","\xe2\x98\x95"}, {"pizza","\xf0\x9f\x8d\x95"}, {"beer","\xf0\x9f\x8d\xba"},
    {"cake","\xf0\x9f\x8e\x82"}, {"gift","\xf0\x9f\x8e\x81"}, {"zap","\xe2\x9a\xa1"},
    {"question","\xe2\x9d\x93"},
};

/* Context-aware completions for the current (trailing) composer token. Returns
 * the candidate count, fills out[], and sets *repl_start to the byte offset where
 * the replacement begins. Recomputed live each frame and on Tab. */
static int ac_candidates(const oc_model *m, const char *s, ac_cand *out, int max, int *repl_start) {
    size_t len = strlen(s);
    int ws = 0;
    for (int i = (int)len - 1; i >= 0; i--) if (s[i] == ' ') { ws = i + 1; break; }
    const char *tok = s + ws;
    int n = 0;
    *repl_start = ws;

    if (ws == 0 && tok[0] == '/') {                          /* slash command */
        *repl_start = 0;
        for (size_t i = 0; i < sizeof AC_COMMANDS / sizeof *AC_COMMANDS && n < max; i++)
            if (ci_prefix(AC_COMMANDS[i] + 1, tok + 1)) {
                snprintf(out[n].repl, sizeof out[n].repl, "%s", AC_COMMANDS[i]);
                snprintf(out[n].disp, sizeof out[n].disp, "%s", AC_COMMANDS[i]);
                n++;
            }
        return n;
    }
    if (tok[0] == ':' && !strchr(tok + 1, ':')) {            /* :emoji: shortcode */
        for (size_t i = 0; i < sizeof AC_EMOJI / sizeof *AC_EMOJI && n < max; i++)
            if (ci_prefix(AC_EMOJI[i].name, tok + 1)) {
                snprintf(out[n].repl, sizeof out[n].repl, "%s", AC_EMOJI[i].emoji);
                snprintf(out[n].disp, sizeof out[n].disp, ":%s: %s", AC_EMOJI[i].name, AC_EMOJI[i].emoji);
                n++;
            }
        return n;
    }
    if (tok[0] == '@') {                                     /* @mention */
        for (size_t i = 0; i < m->n_users && n < max; i++)
            if (m->users[i].name[0] && ci_prefix(m->users[i].name, tok + 1)) {
                snprintf(out[n].repl, sizeof out[n].repl, "@%s", m->users[i].name);
                snprintf(out[n].disp, sizeof out[n].disp, "%s", m->users[i].name);
                n++;
            }
        return n;
    }
    if (tok[0] == '#') {                                     /* #channel in a message */
        for (size_t i = 0; i < m->n_channels && n < max; i++) {
            const oc_channel *c = &m->channels[i];
            if (c->kind != OC_CHANNEL_KIND_DM && c->name && ci_prefix(c->name, tok + 1)) {
                snprintf(out[n].repl, sizeof out[n].repl, "#%s", c->name);
                snprintf(out[n].disp, sizeof out[n].disp, "%s", c->name);
                n++;
            }
        }
        return n;
    }
    if (s[0] == '/') {                                       /* command argument */
        char cmd[24]; int ci = 0;
        for (; s[ci] && s[ci] != ' ' && ci < (int)sizeof cmd - 1; ci++) cmd[ci] = s[ci];
        cmd[ci] = '\0';
        int chan = (strcmp(cmd, "/join") == 0 || strcmp(cmd, "/leave") == 0);
        int usr  = (strcmp(cmd, "/dm") == 0 || strcmp(cmd, "/role") == 0 || strcmp(cmd, "/remove") == 0);
        if (chan)
            for (size_t i = 0; i < m->n_channels && n < max; i++) {
                const oc_channel *c = &m->channels[i];
                if (c->kind != OC_CHANNEL_KIND_DM && c->name && ci_prefix(c->name, tok)) {
                    snprintf(out[n].repl, sizeof out[n].repl, "%s", c->name);
                    snprintf(out[n].disp, sizeof out[n].disp, "%s", c->name);
                    n++;
                }
            }
        else if (usr)
            for (size_t i = 0; i < m->n_users && n < max; i++)
                if (m->users[i].name[0] && ci_prefix(m->users[i].name, tok)) {
                    snprintf(out[n].repl, sizeof out[n].repl, "%s", m->users[i].name);
                    snprintf(out[n].disp, sizeof out[n].disp, "%s", m->users[i].name);
                    n++;
                }
        return n;
    }
    return 0;
}

static void render(oc_client *cl, size_t focus, const char *composer,
                   size_t clen, int scroll, int help_open, int ac_idx,
                   int panel, int msg_sel, int mem_sel, uint64_t editing) {
    const oc_model *m = oc_client_model(cl);
    int ch_act = (panel == 1), msg_act = (panel == 0 || panel == 2), mem_act = (panel == 3);
    int W = tb_width(), H = tb_height();
    tb_clear();
    if (W < 24 || H < 8) { draw_clip(0, 0, W, " terminal too small ", TB_RED | TB_BOLD, TB_DEFAULT); tb_present(); return; }

    const oc_channel *fc = (focus < m->n_channels) ? &m->channels[focus] : NULL;
    const char *conn = m->authed ? "connected" : (m->connected ? "connecting…" : "offline");

    /* Header bar (row 0): workspace · you · presence · connection · unread. */
    int unread = 0;
    for (size_t i = 0; i < m->n_channels; i++) if (m->channels[i].unread > 0) unread += m->channels[i].unread;
    const char *me = m->user_id ? oc_model_user_name(m, m->user_id) : "";
    uint8_t pr = oc_model_presence_of(m, m->user_id);
    const char *dot = pr == OC_PRESENCE_ONLINE ? "\xe2\x97\x8f" : pr == OC_PRESENCE_AWAY ? "\xe2\x97\x90" : "\xe2\x97\x8b";
    char hdr[256];
    snprintf(hdr, sizeof hdr, " OpenChime · %.120s · %.48s %s · %s",
             active_label(), me[0] ? me : "…", dot, conn);
    fill_row(0, 0, W, TB_BLUE);
    draw_clip(0, 0, W, hdr, TB_WHITE | TB_BOLD, TB_BLUE);
    /* Unread here, and — so a background workspace isn't invisible — a count of
     * what's waiting in the others (REQ-014). */
    int other = 0;
    for (int i = 0; i < g_nws; i++) if (i != g_active) other += ws_unread(&g_ws[i]);
    char u[64];
    if (unread && other)   snprintf(u, sizeof u, "%d unread · %d elsewhere ", unread, other);
    else if (unread)       snprintf(u, sizeof u, "%d unread ", unread);
    else if (other)        snprintf(u, sizeof u, "%d elsewhere ", other);
    else                   u[0] = '\0';
    if (u[0]) draw_clip(W - (int)strlen(u) - 1, 0, W, u, TB_YELLOW | TB_BOLD, TB_BLUE);

    /* Rows: header=0; panels=[1, H-3); status=H-3; composer=H-2; hint=H-1. */
    int panels_top = 1, panels_h = H - 4;
    if (panels_h < 3) panels_h = 3;
    int ch_w, mem_w, msg_x, msg_w;
    layout(W, &ch_w, &mem_w, &msg_x, &msg_w);

    /* Channels panel. */
    draw_panel(0, panels_top, ch_w, panels_h, "Channels", ch_act);
    for (size_t i = 0, iy = panels_top + 1; i < m->n_channels && (int)iy < panels_top + panels_h - 1; i++, iy++) {
        const oc_channel *c = &m->channels[i];
        int sel = (i == focus);
        uintattr_t bg = sel ? TB_BLUE : TB_DEFAULT;
        uintattr_t fg = sel ? TB_WHITE | TB_BOLD : c->joined ? TB_DEFAULT : TB_BLACK | TB_BOLD;
        fill_row((int)iy, 1, ch_w - 1, bg);
        char title[96];
        if (c->kind == OC_CHANNEL_KIND_DM) {
            const char *pn = (c->peer_id == m->user_id) ? "you" : oc_model_user_name(m, c->peer_id);
            snprintf(title, sizeof title, "@%s", pn[0] ? pn : "dm");
        } else {
            snprintf(title, sizeof title, "%s%s", c->joined ? "# " : "+ ", c->name ? c->name : "…");
        }
        char label[128];
        if (c->unread > 0) snprintf(label, sizeof label, "%s%s (%d)", sel ? "\xe2\x96\xb8" : " ", title, c->unread);
        else               snprintf(label, sizeof label, "%s%s", sel ? "\xe2\x96\xb8" : " ", title);
        draw_clip(1, (int)iy, ch_w - 1, label, fg, bg);
    }

    /* Messages panel (title = channel, or the open overlay). */
    char mt[120];
    if (m->roster_open)          snprintf(mt, sizeof mt, "members");
    else if (m->search_open)     snprintf(mt, sizeof mt, "search");
    else if (m->reactlist_open)  snprintf(mt, sizeof mt, "reactions");
    else if (m->prefs_open)      snprintf(mt, sizeof mt, "notifications");
    else if (m->weblist_open)    snprintf(mt, sizeof mt, "webhooks");
    else if (m->thread_open)     snprintf(mt, sizeof mt, "thread · #%s", fc && fc->name ? fc->name : "…");
    else if (fc && fc->kind == OC_CHANNEL_KIND_DM) {
        const char *pn = (fc->peer_id == m->user_id) ? "you" : oc_model_user_name(m, fc->peer_id);
        snprintf(mt, sizeof mt, "@%s", pn[0] ? pn : "dm");
    } else if (fc)               snprintf(mt, sizeof mt, "#%s", fc->name ? fc->name : "…");
    else                         snprintf(mt, sizeof mt, "no channel");
    draw_panel(msg_x, panels_top, msg_w, panels_h, mt, msg_act);
    {
        int ix = msg_x + 1, iy = panels_top + 1, iw = msg_w - 2, ih = panels_h - 2;
        int normal = !(m->thread_open || m->search_open || m->roster_open || m->reactlist_open || m->prefs_open || m->weblist_open);
        if (ih > 0 && (fc || !normal)) {
            rows_t rows = {0};
            if (m->roster_open)         build_roster_rows(&rows, m, iw);
            else if (m->search_open)    build_search_rows(&rows, m, iw);
            else if (m->reactlist_open) build_reactlist_rows(&rows, m, iw);
            else if (m->prefs_open)     build_prefs_rows(&rows, m, iw);
            else if (m->weblist_open)   build_webhooks_rows(&rows, m, iw);
            else if (m->thread_open)    build_thread_rows(&rows, m, fc, m->user_id, iw);
            else                        build_rows(&rows, m, fc, m->user_id, iw);
            int total = (int)rows.n, end;
            if (panel == 2 && normal && msg_sel >= 0) {   /* keep the selection visible */
                int last = -1;
                for (int i = 0; i < total; i++) if (rows.v[i].mi == msg_sel) last = i;
                end = (last < 0) ? total : last + 1;
                if (end < ih) end = ih;
                if (end > total) end = total;
            } else {
                end = total - scroll;
                if (end > total) end = total;
                if (end < 0) end = 0;
            }
            int start = end - ih;
            if (start < 0) start = 0;
            int y = iy + (ih - (end - start));
            for (int i = start; i < end; i++, y++) {
                int selrow = (panel == 2 && normal && rows.v[i].mi >= 0 && rows.v[i].mi == msg_sel);
                uintattr_t bg = selrow ? TB_BLUE : TB_DEFAULT;
                if (selrow) fill_row(y, ix, ix + iw, bg);
                draw_clip(ix, y, ix + iw, rows.v[i].s, selrow ? (TB_WHITE | TB_BOLD) : rows.v[i].fg, bg);
            }
            rows_free(&rows);
        }
    }

    /* Members panel (roster + presence), on wide terminals. */
    if (mem_w) {
        int mx = msg_x + msg_w;
        draw_panel(mx, panels_top, mem_w, panels_h, "Members", mem_act);
        for (size_t i = 0, iy = panels_top + 1; i < m->n_users && (int)iy < panels_top + panels_h - 1; i++, iy++) {
            const oc_member *u = &m->users[i];
            uint8_t p = oc_model_presence_of(m, u->user_id);
            const char *d = p == OC_PRESENCE_ONLINE ? "\xe2\x97\x8f" : p == OC_PRESENCE_AWAY ? "\xe2\x97\x90" : "\xe2\x97\x8b";
            int sel = (mem_act && (int)i == mem_sel);
            uintattr_t bg = sel ? TB_BLUE : TB_DEFAULT;
            uintattr_t col = sel ? (TB_WHITE | TB_BOLD)
                           : u->disabled ? (TB_BLACK | TB_BOLD)
                           : p == OC_PRESENCE_ONLINE ? TB_GREEN : p == OC_PRESENCE_AWAY ? TB_YELLOW : TB_DEFAULT;
            const char *role = u->role == OC_ROLE_OWNER ? " *" : u->role == OC_ROLE_ADMIN ? " +" : "";
            char line[80]; snprintf(line, sizeof line, "%s %s%s", d, u->name[0] ? u->name : "?", role);
            if (sel) fill_row((int)iy, mx + 1, mx + mem_w - 1, bg);
            draw_clip(mx + 1, (int)iy, mx + mem_w - 1, line, col, bg);
        }
    }

    /* Row H-3: the autocomplete suggestion strip when completing, else the status
     * line (last status/error + typing indicator). */
    fill_row(H - 3, 0, W, TB_DEFAULT);
    ac_cand cands[16]; int rs, nc = 0;
    if (clen > 0) nc = ac_candidates(m, composer, cands, 16, &rs);
    if (nc > 0) {
        int active = ((ac_idx % nc) + nc) % nc;
        int cx = draw_clip(0, H - 3, W, " \xe2\x96\xb8 ", TB_CYAN | TB_BOLD, TB_DEFAULT);   /* ▸ */
        for (int i = 0; i < nc && cx < W - 2; i++) {
            char item[110]; snprintf(item, sizeof item, "%s", cands[i].disp);
            uintattr_t fg = (i == active) ? TB_BLACK : TB_WHITE | TB_BOLD;
            uintattr_t bg = (i == active) ? TB_CYAN : TB_DEFAULT;
            if (i == active) tb_set_cell(cx - 1, H - 3, ' ', fg, bg);
            cx = draw_clip(cx, H - 3, W, item, fg, bg);
            if (i == active && cx < W) tb_set_cell(cx, H - 3, ' ', fg, bg);
            cx = draw_clip(cx, H - 3, W, "  ", TB_DEFAULT, TB_DEFAULT);
        }
        draw_clip(cx, H - 3, W, nc > 1 ? "(Tab cycles)" : "(Tab)", TB_BLACK | TB_BOLD, TB_DEFAULT);
    } else {
        char st[220]; snprintf(st, sizeof st, " %s%s", m->status[0] ? m->status : "", scroll > 0 ? "   [scrolled]" : "");
        int sx = draw_clip(0, H - 3, W, st, TB_YELLOW, TB_DEFAULT);
        if (fc) {
            uint64_t tp[8]; size_t nt = oc_model_typing(m, fc->channel_id, m->user_id, tp, 8);
            if (nt) {
                char tl[140];
                if (nt == 1) snprintf(tl, sizeof tl, "  ✎ %s is typing…", name_for(fc, tp[0]));
                else         snprintf(tl, sizeof tl, "  ✎ %zu people are typing…", nt);
                draw_clip(sx, H - 3, W, tl, TB_CYAN, TB_DEFAULT);
            }
        }
    }

    /* Composer (row H-2). Editing an existing message shows a distinct prompt. */
    fill_row(H - 2, 0, W, TB_DEFAULT);
    int cx = editing ? draw_clip(0, H - 2, W, "\xe2\x9c\x8e edit \xe2\x80\xba ", TB_YELLOW | TB_BOLD, TB_DEFAULT)
                     : draw_clip(0, H - 2, W, "\xe2\x80\xba ", TB_GREEN | TB_BOLD, TB_DEFAULT);   /* › */
    cx = draw_clip(cx, H - 2, W, composer, TB_DEFAULT, TB_DEFAULT);
    if (cx < W) tb_set_cell(cx, H - 2, ' ', TB_DEFAULT, TB_REVERSE);   /* cursor */

    /* Context keybinding hint bar (row H-1), per focused panel. */
    const char *hint;
    if (help_open)        hint = " press ? or Esc to close help ";
    else if (panel == 1)  hint = " Channels  ·  j/k select  ·  Enter open  ·  Tab panel  ·  Esc composer ";
    else if (panel == 2)  hint = " Messages  ·  j/k select  ·  Enter/t thread  r react  e edit  x delete  w reactions  ·  Tab panel  ·  Esc composer ";
    else if (panel == 3)  hint = " Members  ·  j/k select  ·  Enter DM  ·  Tab panel  ·  Esc composer ";
    else                  hint = " Enter: send   Tab: complete   Esc: navigate   /: command   ?: help   ^Q: quit ";
    fill_row(H - 1, 0, W, TB_BLACK | TB_BOLD);
    draw_clip(0, H - 1, W, hint, TB_WHITE, TB_BLACK | TB_BOLD);

    if (help_open) draw_help(W, H);
    tb_present();
}

/* ---- command palette (:  or Ctrl-K) --------------------------------------- */

enum { PAL_CMD, PAL_CHAN, PAL_DM };
typedef struct { char label[96]; int kind; uint64_t id; const char *cmd; int needs_arg; } pal_item;

static const struct { const char *cmd; const char *desc; int arg; } PAL_CMDS[] = {
    {"/join","join a channel",1}, {"/leave","leave this channel",0}, {"/create","create a channel",1},
    {"/list","refresh channel list",0}, {"/dm","open a direct message",1}, {"/who","member roster",0},
    {"/away","set yourself away",0}, {"/online","set yourself online",0}, {"/search","search messages",1},
    {"/thread","open last message's thread",0}, {"/reactions","who reacted (last)",0}, {"/react","react to last",1},
    {"/edit","edit your last message",1}, {"/delete","delete your last message",0}, {"/prefs","notification settings",0},
    {"/notify","set channel notify level",1}, {"/dnd","do-not-disturb window",1},
    {"/set","synced pref (mouse/members/time/…)",1},
    {"/workspaces","switch workspace (^W)",0},
    {"/storage","server storage usage (admin)",0},
    {"/profile","your identity (name/role/id)",0}, {"/nick","change your display name",1},
    {"/passwd","change your password",1}, {"/role","set a user's role",1},
    {"/invite","mint an invite token",0}, {"/remove","remove a user",1}, {"/webhook","channel webhooks",0},
    {"/upload","upload a file",1}, {"/download","download an attachment",1}, {"/help","keys & commands",0},
    {"/logout","sign out",0}, {"/close","close the open overlay",0},
};

/* Case-insensitive subsequence ("fuzzy") match: do needle's chars appear in
 * order within hay? An empty needle matches everything. */
static int fuzzy(const char *hay, const char *needle) {
    if (!*needle) return 1;
    for (; *hay; hay++)
        if (tolower((unsigned char)*hay) == tolower((unsigned char)*needle)) {
            needle++;
            if (!*needle) return 1;
        }
    return 0;
}

/* Build the filtered palette list from commands, channels, and members. */
static int palette_build(const oc_model *m, const char *q, pal_item *out, int max) {
    int n = 0;
    for (size_t i = 0; i < sizeof PAL_CMDS / sizeof *PAL_CMDS && n < max; i++)
        if (fuzzy(PAL_CMDS[i].cmd, q) || fuzzy(PAL_CMDS[i].desc, q)) {
            snprintf(out[n].label, sizeof out[n].label, "%-11s %s", PAL_CMDS[i].cmd, PAL_CMDS[i].desc);
            out[n].kind = PAL_CMD; out[n].cmd = PAL_CMDS[i].cmd; out[n].needs_arg = PAL_CMDS[i].arg; n++;
        }
    for (size_t i = 0; i < m->n_channels && n < max; i++) {
        const oc_channel *c = &m->channels[i];
        if (c->kind != OC_CHANNEL_KIND_DM && c->name && fuzzy(c->name, q)) {
            snprintf(out[n].label, sizeof out[n].label, "# %-16s jump to channel", c->name);
            out[n].kind = PAL_CHAN; out[n].id = c->channel_id; n++;
        }
    }
    for (size_t i = 0; i < m->n_users && n < max; i++)
        if (m->users[i].name[0] && fuzzy(m->users[i].name, q)) {
            snprintf(out[n].label, sizeof out[n].label, "@ %-16s direct message", m->users[i].name);
            out[n].kind = PAL_DM; out[n].id = m->users[i].user_id; n++;
        }
    return n;
}

/* Draw the palette popup (query line + filtered list, selection highlighted). */
static void draw_palette(const oc_model *m, const char *q, int psel) {
    int W = tb_width(), H = tb_height();
    pal_item items[64]; int n = palette_build(m, q, items, 64);
    int show = n > 12 ? 12 : (n < 1 ? 1 : n);
    int bw = 64; if (bw > W - 4) bw = W - 4; if (bw < 24) bw = 24;
    int bh = show + 4; if (bh > H - 2) bh = H - 2;
    int x = (W - bw) / 2, y = (H - bh) / 2; if (x < 0) x = 0; if (y < 0) y = 0;
    for (int j = 0; j < bh; j++) fill_row(y + j, x, x + bw, TB_DEFAULT);
    draw_panel(x, y, bw, bh, "Command palette  \xc2\xb7  Esc to close", 1);
    char ql[128]; snprintf(ql, sizeof ql, "\xe2\x80\xba %s", q);
    draw_clip(x + 2, y + 1, x + bw - 1, ql, TB_GREEN | TB_BOLD, TB_DEFAULT);
    int cxq = x + 4 + (int)strlen(q);
    if (cxq < x + bw - 1) tb_set_cell(cxq, y + 1, ' ', TB_DEFAULT, TB_REVERSE);
    int top = (psel >= show) ? psel - show + 1 : 0;
    for (int i = 0; i < show && top + i < n; i++) {
        int idx = top + i, sel = (idx == psel);
        uintattr_t bg = sel ? TB_BLUE : TB_DEFAULT, fg = sel ? TB_WHITE | TB_BOLD : TB_DEFAULT;
        if (sel) fill_row(y + 2 + i, x + 1, x + bw - 1, bg);
        draw_clip(x + 2, y + 2 + i, x + bw - 1, items[idx].label, fg, bg);
    }
    if (n == 0) draw_clip(x + 2, y + 2, x + bw - 1, "(no matches)", TB_BLACK | TB_BOLD, TB_DEFAULT);
    tb_present();
}


/* The /storage overlay (REQ-214): usage, the active policy, and what
 * maintenance has reclaimed. Owner/admin only — a member's request is refused
 * server-side, which surfaces as an error line rather than an empty panel. */
static void draw_storage(const oc_model *m, int W, int H) {
    int bw = 62; if (bw > W - 4) bw = W - 4; if (bw < 30) bw = 30;
    int bh = 15; if (bh > H - 2) bh = H - 2;
    int x = (W - bw) / 2, y = (H - bh) / 2; if (x < 0) x = 0; if (y < 0) y = 0;
    for (int j = 0; j < bh; j++) fill_row(y + j, x, x + bw, TB_DEFAULT);
    draw_panel(x, y, bw, bh, "Storage  \xc2\xb7  any key to close", 1);

    char l[160];
    int row = y + 1;
    if (!m->storage_have) {
        draw_clip(x + 2, row, x + bw - 1, "(no report yet — owner/admin only)",
                  TB_BLACK | TB_BOLD, TB_DEFAULT);
        tb_present();
        return;
    }
    const oc_storage_view *s = &m->storage;
    const uint64_t MBv = 1024 * 1024;

    snprintf(l, sizeof l, "Disk        %llu MB free of %llu MB",
             (unsigned long long)(s->avail_bytes / MBv),
             (unsigned long long)(s->total_bytes / MBv));
    draw_clip(x + 2, row++, x + bw - 1, l,
              s->under_pressure ? (TB_RED | TB_BOLD) : TB_DEFAULT, TB_DEFAULT);

    snprintf(l, sizeof l, "Attachments %llu file(s), %llu MB",
             (unsigned long long)s->attach_count,
             (unsigned long long)(s->attach_bytes / MBv));
    draw_clip(x + 2, row++, x + bw - 1, l, TB_DEFAULT, TB_DEFAULT);

    if (s->under_pressure)
        draw_clip(x + 2, row++, x + bw - 1,
                  "UNDER PRESSURE — reclaiming storage", TB_RED | TB_BOLD, TB_DEFAULT);
    row++;

    draw_clip(x + 2, row++, x + bw - 1, "Policy", TB_YELLOW | TB_BOLD, TB_DEFAULT);
    if (s->max_age_days)
        snprintf(l, sizeof l, "  attachments expire after %llu day(s)",
                 (unsigned long long)s->max_age_days);
    else
        snprintf(l, sizeof l, "  attachments kept indefinitely");
    draw_clip(x + 2, row++, x + bw - 1, l, TB_DEFAULT, TB_DEFAULT);
    snprintf(l, sizeof l, "  eviction under pressure: %s",
             s->evict_enabled ? "on (oldest first)" : "off");
    draw_clip(x + 2, row++, x + bw - 1, l, TB_DEFAULT, TB_DEFAULT);
    snprintf(l, sizeof l, "  database reserve: %llu MB",
             (unsigned long long)(s->reserve_bytes / MBv));
    draw_clip(x + 2, row++, x + bw - 1, l, TB_DEFAULT, TB_DEFAULT);
    row++;

    draw_clip(x + 2, row++, x + bw - 1, "Reclaimed so far", TB_YELLOW | TB_BOLD, TB_DEFAULT);
    snprintf(l, sizeof l, "  %llu abandoned  %llu expired  %llu evicted",
             (unsigned long long)s->rec_orphan,
             (unsigned long long)s->rec_expired,
             (unsigned long long)s->rec_evicted);
    /* Evictions are the destructive ones, so they are called out in red when
     * any have happened — an operator should not have to read carefully to
     * notice that the daemon deleted files nobody approved individually. */
    draw_clip(x + 2, row++, x + bw - 1, l,
              s->rec_evicted ? (TB_RED | TB_BOLD) : TB_DEFAULT, TB_DEFAULT);
    tb_present();
}

/* ---- workspace switcher (REQ-013) -------------------------------------------
 * One list, the same on every platform: the workspaces this device remembers,
 * each with its unread + connection state, and an always-present "Log in to new
 * workspace" row at the bottom. Open sessions come first (they have live state
 * to show), then remembered-but-closed workspaces from the book. */

static const char *g_store_path = NULL;   /* set in main(); the book lives here */
static oc_secret  *g_secret = NULL;

typedef struct {
    char key[288];
    char label[256];
    char user[128];
    int  session;                 /* index into g_ws, or -1 when not open */
} sw_entry;

static sw_entry g_sw[MAX_WS + 16];
static int      g_nsw = 0;

static void sw_book_cb(void *ctx, const char *key, const char *label,
                       const char *user, uint64_t last_used) {
    (void)ctx; (void)last_used;
    if (g_nsw >= (int)(sizeof g_sw / sizeof g_sw[0])) return;
    if (!key || !key[0]) return;
    for (int i = 0; i < g_nsw; i++)                  /* already listed as open */
        if (strcmp(g_sw[i].key, key) == 0) return;
    sw_entry *e = &g_sw[g_nsw++];
    snprintf(e->key,   sizeof e->key,   "%s", key);
    snprintf(e->label, sizeof e->label, "%s", (label && label[0]) ? label : key);
    snprintf(e->user,  sizeof e->user,  "%s", user ? user : "");
    e->session = -1;
}

/* Rebuild the switcher list. Called when the switcher opens (not per frame) —
 * it touches the book's SQLite file. */
static void sw_build(void) {
    g_nsw = 0;
    for (int i = 0; i < g_nws && g_nsw < (int)(sizeof g_sw / sizeof g_sw[0]); i++) {
        sw_entry *e = &g_sw[g_nsw++];
        snprintf(e->key,   sizeof e->key,   "%s", g_ws[i].key);
        snprintf(e->label, sizeof e->label, "%s", g_ws[i].label[0] ? g_ws[i].label : g_ws[i].key);
        snprintf(e->user,  sizeof e->user,  "%s", g_ws[i].user);
        e->session = i;
    }
    oc_store *s = g_store_path ? oc_store_open(g_store_path) : NULL;
    if (s) { oc_store_workspace_each(s, sw_book_cb, NULL); oc_store_close(s); }
}

static void draw_switcher(int sel) {
    int W = tb_width(), H = tb_height();
    int rows = g_nsw + 1;                       /* + the "log in" row */
    int show = rows > 12 ? 12 : rows;
    int bw = 60; if (bw > W - 4) bw = W - 4; if (bw < 28) bw = 28;
    int bh = show + 4; if (bh > H - 2) bh = H - 2;
    int x = (W - bw) / 2, y = (H - bh) / 2; if (x < 0) x = 0; if (y < 0) y = 0;
    for (int j = 0; j < bh; j++) fill_row(y + j, x, x + bw, TB_DEFAULT);
    draw_panel(x, y, bw, bh, "Workspaces  \xc2\xb7  Enter switch  \xc2\xb7  Esc close", 1);

    int top = (sel >= show) ? sel - show + 1 : 0;
    for (int i = 0; i < show && top + i < rows; i++) {
        int idx = top + i, is_sel = (idx == sel);
        uintattr_t bg = is_sel ? TB_BLUE : TB_DEFAULT;
        uintattr_t fg = is_sel ? TB_WHITE | TB_BOLD : TB_DEFAULT;
        if (is_sel) fill_row(y + 1 + i, x + 1, x + bw - 1, bg);

        char line[320];
        if (idx == g_nsw) {                      /* the always-present last row */
            snprintf(line, sizeof line, "+ Log in to new workspace");
            draw_clip(x + 2, y + 1 + i, x + bw - 1, line,
                      is_sel ? fg : (TB_GREEN | TB_BOLD), bg);
            continue;
        }
        const sw_entry *e = &g_sw[idx];
        const char *mark = (e->session == g_active) ? "\xe2\x96\xb8" : " ";   /* ▸ active */
        if (e->session >= 0) {
            const oc_model *m = oc_client_model(g_ws[e->session].cl);
            int un = ws_unread(&g_ws[e->session]);
            /* ● connected · ◌ reconnecting — a dropped background workspace
             * should be visible here, not a silent gap. */
            const char *dot = m->authed ? "\xe2\x97\x8f" : "\xe2\x97\x8c";
            if (un) snprintf(line, sizeof line, "%s %s %-28.28s %-14.14s %d unread",
                             mark, dot, e->label, e->user, un);
            else    snprintf(line, sizeof line, "%s %s %-28.28s %-14.14s",
                             mark, dot, e->label, e->user);
        } else {
            snprintf(line, sizeof line, "%s   %-28.28s %-14.14s signed out",
                     mark, e->label, e->user);
        }
        draw_clip(x + 2, y + 1 + i, x + bw - 1, line,
                  is_sel ? fg : (e->session >= 0 ? TB_DEFAULT : (TB_BLACK | TB_BOLD)), bg);
    }
    draw_clip(x + 2, y + bh - 1, x + bw - 1,
              " ^W switch \xc2\xb7 d forget \xc2\xb7 Esc close ", TB_BLACK | TB_BOLD, TB_DEFAULT);
    tb_present();
}

/* ---- emoji picker (r on a selected message) ------------------------------- */

static int picker_build(const char *q, const char **emoji, const char **name, int max) {
    int n = 0;
    for (size_t i = 0; i < sizeof AC_EMOJI / sizeof *AC_EMOJI && n < max; i++)
        if (fuzzy(AC_EMOJI[i].name, q)) { emoji[n] = AC_EMOJI[i].emoji; name[n] = AC_EMOJI[i].name; n++; }
    return n;
}

static void draw_picker(const char *q, int esel) {
    int W = tb_width(), H = tb_height();
    const char *em[64], *nm[64]; int n = picker_build(q, em, nm, 64);
    int show = n > 12 ? 12 : (n < 1 ? 1 : n);
    int bw = 40; if (bw > W - 4) bw = W - 4; if (bw < 20) bw = 20;
    int bh = show + 4; if (bh > H - 2) bh = H - 2;
    int x = (W - bw) / 2, y = (H - bh) / 2; if (x < 0) x = 0; if (y < 0) y = 0;
    for (int j = 0; j < bh; j++) fill_row(y + j, x, x + bw, TB_DEFAULT);
    draw_panel(x, y, bw, bh, "React  \xc2\xb7  Esc to close", 1);
    char ql[64]; snprintf(ql, sizeof ql, "\xe2\x80\xba %s", q);
    draw_clip(x + 2, y + 1, x + bw - 1, ql, TB_GREEN | TB_BOLD, TB_DEFAULT);
    int cxq = x + 4 + (int)strlen(q);
    if (cxq < x + bw - 1) tb_set_cell(cxq, y + 1, ' ', TB_DEFAULT, TB_REVERSE);
    int top = (esel >= show) ? esel - show + 1 : 0;
    for (int i = 0; i < show && top + i < n; i++) {
        int idx = top + i, sel = (idx == esel);
        uintattr_t bg = sel ? TB_BLUE : TB_DEFAULT, fg = sel ? TB_WHITE | TB_BOLD : TB_DEFAULT;
        char line[64]; snprintf(line, sizeof line, "%s  :%s:", em[idx], nm[idx]);
        if (sel) fill_row(y + 2 + i, x + 1, x + bw - 1, bg);
        draw_clip(x + 2, y + 2 + i, x + bw - 1, line, fg, bg);
    }
    if (n == 0) draw_clip(x + 2, y + 2, x + bw - 1, "(no matches)", TB_BLACK | TB_BOLD, TB_DEFAULT);
    tb_present();
}

/* Whether `mid` in the focused channel already carries `emoji` as our reaction —
 * so the picker can toggle it off. */
static int my_reaction(const oc_model *m, uint64_t channel_id, uint64_t mid, const char *emoji) {
    const oc_channel *c = oc_model_channel((oc_model *)m, channel_id);
    if (!c) return 0;
    for (size_t i = 0; i < c->n_msgs; i++)
        if (c->msgs[i].message_id == mid)
            for (uint8_t k = 0; k < c->msgs[i].n_reactions; k++)
                if (strcmp(c->msgs[i].reactions[k].emoji, emoji) == 0) return c->msgs[i].reactions[k].mine;
    return 0;
}

/* ---- input helpers --------------------------------------------------------- */

/* Remove the last full UTF-8 character from `buf` (length *len). */
static void backspace_utf8(char *buf, size_t *len) {
    size_t n = *len;
    if (n == 0) return;
    n--;
    while (n > 0 && (buf[n] & 0xC0) == 0x80) n--;   /* back over continuation bytes */
    buf[n] = '\0';
    *len = n;
}

/* The focused channel in the model, or NULL. */
static const oc_channel *focused_channel(const oc_model *m, uint64_t cid) {
    for (size_t i = 0; i < m->n_channels; i++)
        if (m->channels[i].channel_id == cid) return &m->channels[i];
    return NULL;
}

/* The filename of an attachment by id (searching every channel's messages), or
 * "" if unknown. Used to pick a default download path. */
static const char *attach_filename(const oc_model *m, uint64_t id) {
    for (size_t ci = 0; ci < m->n_channels; ci++)
        for (size_t i = 0; i < m->channels[ci].n_msgs; i++) {
            const oc_msg *msg = &m->channels[ci].msgs[i];
            for (uint8_t k = 0; k < msg->n_attach; k++)
                if (msg->attach[k].id == id) return msg->attach[k].filename;
        }
    return "";
}

/* Our own most recent, non-deleted message in a channel (for /edit, /delete). */
static const oc_msg *my_last_message(const oc_channel *ch, uint64_t me) {
    for (size_t i = ch->n_msgs; i > 0; i--) {
        const oc_msg *msg = &ch->msgs[i - 1];
        if (msg->author_id == me && !msg->deleted) return msg;
    }
    return NULL;
}

/* Parse "HH:MM" into minutes-since-midnight, or -1 if malformed. */
static int parse_hhmm(const char *s) {
    int h = 0, mi = 0;
    if (sscanf(s, "%d:%d", &h, &mi) != 2) return -1;
    if (h < 0 || h > 23 || mi < 0 || mi > 59) return -1;
    return h * 60 + mi;
}

/* Handle a "/command" typed in the composer:
 *   /react <emoji>  toggle a reaction on the last message in the channel
 *   /edit  <text>   replace the text of your last message
 *   /delete         tombstone your last message
 */
/* Parse an on/off token (on/1/true/yes = 1; anything else = 0). */
static int setting_onoff(const char *v) {
    return v && (strcmp(v, "on") == 0 || strcmp(v, "1") == 0 ||
                 strcmp(v, "true") == 0 || strcmp(v, "yes") == 0);
}

static void handle_command(oc_client *cl, uint64_t cid, const char *line) {
    const oc_model *m = oc_client_model(cl);
    if (strcmp(line, "/close") == 0) {
        oc_client_close_thread(cl); oc_client_close_search(cl);
        oc_client_close_reactions(cl); oc_client_toggle_roster(cl, 0);
        oc_client_toggle_prefs(cl, 0); oc_client_close_webhooks(cl);
        return;
    }
    if (strncmp(line, "/webhook", 8) == 0) {   /* /webhook | /webhook create <label> | /webhook rm <id> */
        const char *a = line + 8;
        while (*a == ' ') a++;
        if (strncmp(a, "create ", 7) == 0) {
            const char *lb = a + 7;
            while (*lb == ' ') lb++;
            if (*lb) oc_client_create_webhook(cl, cid, lb);
        } else if (strncmp(a, "rm ", 3) == 0 || strncmp(a, "delete ", 7) == 0) {
            const char *id = a + (a[0] == 'r' ? 3 : 7);
            while (*id == ' ') id++;
            unsigned long long wid = strtoull(id, NULL, 10);
            if (wid) oc_client_delete_webhook(cl, wid);
        } else {
            oc_client_webhooks(cl, cid);   /* open the overlay + refresh */
        }
        return;
    }
    if (strncmp(line, "/upload ", 8) == 0) {   /* /upload <path> — post a file here */
        const char *path = line + 8;
        while (*path == ' ') path++;
        if (*path) oc_client_upload(cl, cid, path);
        return;
    }
    if (strncmp(line, "/download ", 10) == 0) {  /* /download <id> [dest path] */
        const char *a = line + 10;
        while (*a == ' ') a++;
        char *end = NULL;
        unsigned long long id = strtoull(a, &end, 10);
        if (!id) return;
        while (end && *end == ' ') end++;
        char dest[512];
        if (end && *end) {
            snprintf(dest, sizeof dest, "%s", end);   /* explicit destination */
        } else {
            const char *fn = attach_filename(m, id);   /* default to the filename in cwd */
            if (fn[0]) snprintf(dest, sizeof dest, "%s", fn);
            else       snprintf(dest, sizeof dest, "attachment-%llu", id);
        }
        oc_client_download(cl, id, dest);
        return;
    }
    if (strcmp(line, "/prefs") == 0) { oc_client_toggle_prefs(cl, 1); return; }
    if (strncmp(line, "/notify ", 8) == 0) {   /* set the focused channel's level */
        const char *lv = line + 8;
        while (*lv == ' ') lv++;
        uint8_t level;
        if      (strcmp(lv, "all") == 0)      level = OC_NOTIFY_ALL;
        else if (strcmp(lv, "mentions") == 0) level = OC_NOTIFY_MENTIONS;
        else if (strcmp(lv, "none") == 0)     level = OC_NOTIFY_NONE;
        else return;
        oc_client_set_notify_pref(cl, cid, level);
        return;
    }
    if (strncmp(line, "/dnd", 4) == 0) {       /* /dnd off | /dnd HH:MM HH:MM */
        const char *a = line + 4;
        while (*a == ' ') a++;
        if (strcmp(a, "off") == 0 || *a == '\0') { oc_client_set_dnd(cl, 0, 0, 0); return; }
        char s1[16] = {0}, s2[16] = {0};
        if (sscanf(a, "%15s %15s", s1, s2) != 2) return;
        int start = parse_hhmm(s1), end = parse_hhmm(s2);
        if (start < 0 || end < 0) return;
        oc_client_set_dnd(cl, 1, (uint16_t)start, (uint16_t)end);
        return;
    }
    if (strcmp(line, "/who") == 0)    { oc_client_toggle_roster(cl, 1); return; }
    if (strcmp(line, "/away") == 0)   { oc_client_set_presence(cl, OC_PRESENCE_AWAY); return; }
    if (strcmp(line, "/online") == 0) { oc_client_set_presence(cl, OC_PRESENCE_ONLINE); return; }
    if (strncmp(line, "/search ", 8) == 0) {
        const char *q = line + 8;
        while (*q == ' ') q++;
        if (*q) oc_client_search(cl, q);
        return;
    }
    if (strncmp(line, "/create ", 8) == 0) {
        const char *nm = line + 8;
        while (*nm == ' ') nm++;
        if (*nm) oc_client_create_channel(cl, nm);
        return;
    }
    if (strncmp(line, "/join ", 6) == 0) {
        const char *nm = line + 6;
        while (*nm == ' ') nm++;
        for (size_t i = 0; i < m->n_channels; i++)   /* resolve id by name */
            if (m->channels[i].name && strcmp(m->channels[i].name, nm) == 0) {
                oc_client_join_channel(cl, m->channels[i].channel_id); break;
            }
        return;
    }
    if (strcmp(line, "/leave") == 0) { oc_client_leave_channel(cl, cid); return; }
    if (strcmp(line, "/list") == 0)  { oc_client_list_channels(cl); return; }
    if (strncmp(line, "/dm ", 4) == 0) {          /* open a DM with a user by name */
        const char *nm = line + 4;
        while (*nm == ' ') nm++;
        uint64_t uid = oc_model_user_id(m, nm);
        if (uid) oc_client_open_dm(cl, uid);
        return;
    }
    if (strncmp(line, "/role ", 6) == 0) {        /* /role <name> owner|admin|member */
        char nm[64] = {0}, rl[16] = {0};
        if (sscanf(line + 6, "%63s %15s", nm, rl) != 2) return;
        uint8_t role;
        if      (strcmp(rl, "owner") == 0)  role = OC_ROLE_OWNER;
        else if (strcmp(rl, "admin") == 0)  role = OC_ROLE_ADMIN;
        else if (strcmp(rl, "member") == 0) role = OC_ROLE_MEMBER;
        else return;
        uint64_t uid = oc_model_user_id(m, nm);
        if (uid) oc_client_set_role(cl, uid, role);
        return;
    }
    if (strncmp(line, "/invite", 7) == 0) {       /* /invite [admin|member] (default member) */
        const char *rl = line + 7;
        while (*rl == ' ') rl++;
        uint8_t role = strcmp(rl, "admin") == 0 ? OC_ROLE_ADMIN : OC_ROLE_MEMBER;
        oc_client_invite_user(cl, role);
        return;
    }
    if (strncmp(line, "/remove ", 8) == 0) {      /* /remove <name> */
        const char *nm = line + 8;
        while (*nm == ' ') nm++;
        uint64_t uid = oc_model_user_id(m, nm);
        if (uid) oc_client_remove_user(cl, uid);
        return;
    }
    if (strncmp(line, "/nick ", 6) == 0) {        /* rename yourself (REQ-020) */
        const char *nm = line + 6;
        while (*nm == ' ') nm++;
        if (*nm) oc_client_set_display_name(cl, nm);
        return;
    }
    if (strncmp(line, "/passwd ", 8) == 0) {      /* /passwd <old> <new> — rotate password */
        char oldp[128] = {0}, newp[128] = {0};
        if (sscanf(line + 8, "%127s %127s", oldp, newp) == 2 && newp[0])
            oc_client_change_password(cl, oldp, newp);
        return;
    }
    if (strncmp(line, "/set", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
        /* Portable prefs that sync via the daemon bucket (see apply_synced_settings).
         *   /set mouse on|off · /set members off|on|auto · /set time 12h|24h
         *   /set channels-width N · /set members-width N · /set reset <key> */
        char key[24] = {0}, val[32] = {0};
        sscanf(line + 4, "%23s %31s", key, val);
        char num[8];
        if (strcmp(key, "mouse") == 0) {
            oc_client_set_setting(cl, OC_SYNC_KEY_MOUSE, setting_onoff(val) ? "1" : "0");
        } else if (strcmp(key, "members") == 0) {
            int v = strcmp(val, "auto") == 0 ? 2 : (setting_onoff(val) ? 1 : 0);
            snprintf(num, sizeof num, "%d", v);
            oc_client_set_setting(cl, OC_SYNC_KEY_MEMBERS, num);
        } else if (strcmp(key, "time") == 0) {
            oc_client_set_setting(cl, OC_SYNC_KEY_TIME24, strcmp(val, "24h") == 0 ? "1" : "0");
        } else if (strcmp(key, "channels-width") == 0 || strcmp(key, "members-width") == 0) {
            int n = atoi(val);
            if (n >= 10 && n <= 60) {
                snprintf(num, sizeof num, "%d", n);
                oc_client_set_setting(cl, key[0] == 'c' ? OC_SYNC_KEY_CHWIDTH : OC_SYNC_KEY_MEMWIDTH, num);
            }
        } else if (strcmp(key, "reset") == 0) {
            /* Delete a synced key (empty value) so the file default takes over. */
            if      (strcmp(val, "mouse") == 0)          oc_client_set_setting(cl, OC_SYNC_KEY_MOUSE, "");
            else if (strcmp(val, "members") == 0)        oc_client_set_setting(cl, OC_SYNC_KEY_MEMBERS, "");
            else if (strcmp(val, "time") == 0)           oc_client_set_setting(cl, OC_SYNC_KEY_TIME24, "");
            else if (strcmp(val, "channels-width") == 0) oc_client_set_setting(cl, OC_SYNC_KEY_CHWIDTH, "");
            else if (strcmp(val, "members-width") == 0)  oc_client_set_setting(cl, OC_SYNC_KEY_MEMWIDTH, "");
        }
        return;
    }

    const oc_channel *ch = focused_channel(m, cid);
    if (!ch || ch->n_msgs == 0) return;

    if (strcmp(line, "/thread") == 0) {       /* open the last message's thread */
        oc_client_open_thread(cl, cid, ch->msgs[ch->n_msgs - 1].message_id);
    } else if (strcmp(line, "/reactions") == 0) {   /* who reacted to the last message */
        oc_client_list_reactions(cl, cid, ch->msgs[ch->n_msgs - 1].message_id);
    } else if (strncmp(line, "/react ", 7) == 0) {
        const char *emoji = line + 7;
        while (*emoji == ' ') emoji++;
        if (!*emoji) return;
        const oc_msg *last = &ch->msgs[ch->n_msgs - 1];
        uint8_t op = 1;                       /* add, unless we already reacted */
        for (uint8_t k = 0; k < last->n_reactions; k++)
            if (strcmp(last->reactions[k].emoji, emoji) == 0 && last->reactions[k].mine) { op = 0; break; }
        oc_client_react(cl, cid, last->message_id, emoji, op);
    } else if (strncmp(line, "/edit ", 6) == 0) {
        const char *text = line + 6;
        while (*text == ' ') text++;
        if (!*text) return;
        const oc_msg *mine = my_last_message(ch, m->user_id);
        if (mine) oc_client_edit(cl, cid, mine->message_id, text);
    } else if (strcmp(line, "/delete") == 0) {
        const oc_msg *mine = my_last_message(ch, m->user_id);
        if (mine) oc_client_delete(cl, cid, mine->message_id);
    }
}

/* Resolve the local store path (session token + TOFU pin persistence): the
 * explicit $OPENCHIME_STATE if set, else $HOME/.local/state/openchime/state.db
 * (creating the dirs). Returns NULL when there is nowhere to put it (persistence
 * then disabled — the client still runs, just in-memory). */
static const char *resolve_store_path(void) {
    const char *explicit = getenv("OPENCHIME_STATE");
    if (explicit && explicit[0]) return explicit;
    const char *home = getenv("HOME");
    if (!home || !home[0]) return NULL;
    static char path[1200];
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/.local", home);            mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/.local/state", home);      mkdir(dir, 0700);
    snprintf(dir, sizeof dir, "%s/.local/state/openchime", home); mkdir(dir, 0700);
    snprintf(path, sizeof path, "%s/state.db", dir);
    return path;
}

static int all_digits(const char *s) {
    if (!s || !*s) return 0;
    for (; *s; s++) if (*s < '0' || *s > '9') return 0;
    return 1;
}

/* ---- connect / login dialog (REQ-020 local sign-in) --------------------------
 * A modal box collecting workspace + username + password (+ remember-me), shown
 * when no credential and no stored session token are available. The workspace is
 * resolved on submit (inline "not found"); the caller starts the client and, on
 * an auth/connect failure, comes back here with the reason. */

enum { LOGIN_SUBMIT, LOGIN_QUIT };
enum { AUTH_R_OK, AUTH_R_FAILED, AUTH_R_UNREACHABLE, AUTH_R_CANCELLED };

typedef struct {
    char workspace[256];
    char user[128];
    char pass[128];
    int  remember;
    oc_endpoint ep;                       /* filled by oc_resolve on submit */
} login_form;

/* Draw a single-line box border and clear its interior. */
static void draw_frame(int x, int y, int w, int h) {
    for (int j = 1; j < h - 1; j++)
        for (int i = 1; i < w - 1; i++) tb_set_cell(x + i, y + j, ' ', TB_DEFAULT, TB_DEFAULT);
    for (int i = 1; i < w - 1; i++) {
        tb_set_cell(x + i, y,         0x2500, TB_WHITE, TB_DEFAULT);
        tb_set_cell(x + i, y + h - 1, 0x2500, TB_WHITE, TB_DEFAULT);
    }
    for (int j = 1; j < h - 1; j++) {
        tb_set_cell(x,         y + j, 0x2502, TB_WHITE, TB_DEFAULT);
        tb_set_cell(x + w - 1, y + j, 0x2502, TB_WHITE, TB_DEFAULT);
    }
    tb_set_cell(x,         y,         0x250c, TB_WHITE, TB_DEFAULT);
    tb_set_cell(x + w - 1, y,         0x2510, TB_WHITE, TB_DEFAULT);
    tb_set_cell(x,         y + h - 1, 0x2514, TB_WHITE, TB_DEFAULT);
    tb_set_cell(x + w - 1, y + h - 1, 0x2518, TB_WHITE, TB_DEFAULT);
}

/* A labeled input row; `focused` highlights the field, `mask` renders dots. */
static void draw_field(int x, int y, int w, const char *label, const char *val,
                       int focused, int mask) {
    draw_clip(x, y, x + 11, label, TB_DEFAULT, TB_DEFAULT);
    int fx = x + 11, fw = w - 11;
    uintattr_t bg = focused ? TB_BLUE : (TB_BLACK | TB_BOLD);
    fill_row(y, fx, fx + fw, bg);
    if (mask) {
        char dots[130]; size_t n = strlen(val);
        if (n > sizeof dots - 1) n = sizeof dots - 1;
        memset(dots, '*', n); dots[n] = '\0';
        draw_clip(fx + 1, y, fx + fw, dots, TB_WHITE, bg);
    } else {
        draw_clip(fx + 1, y, fx + fw, val, TB_WHITE | (focused ? TB_BOLD : 0), bg);
    }
}

/* Run the login box until the user submits (fields filled, workspace resolved into
 * `f->ep`) or quits. `err` is an optional message to show (e.g. a prior auth
 * failure). Returns LOGIN_SUBMIT or LOGIN_QUIT. */
static int login_dialog(login_form *f, const char *err) {
    /* On a retry (err set) land on the password — that's what needs fixing;
     * otherwise start past a pre-filled workspace. */
    int focus = err ? 2 : (f->workspace[0] ? 1 : 0);
    char inl[160]; inl[0] = '\0';
    for (;;) {
        int W = tb_width(), H = tb_height();
        tb_clear();
        int bw = 56; if (bw > W - 2) bw = W - 2; if (bw < 24) bw = 24;
        int bh = 12;
        int x = (W - bw) / 2, y = (H - bh) / 2; if (x < 0) x = 0; if (y < 0) y = 0;
        draw_frame(x, y, bw, bh);
        draw_clip(x + 2, y, x + bw - 1, " Sign in to OpenChime ", TB_CYAN | TB_BOLD, TB_DEFAULT);
        int ix = x + 2, iw = bw - 4;
        draw_field(ix, y + 2, iw, "Workspace", f->workspace, focus == 0, 0);
        draw_field(ix, y + 4, iw, "Username", f->user, focus == 1, 0);
        draw_field(ix, y + 5, iw, "Password", f->pass, focus == 2, 1);
        char rem[40]; snprintf(rem, sizeof rem, "[%c] Remember me", f->remember ? 'x' : ' ');
        draw_clip(ix, y + 7, ix + iw, rem, focus == 3 ? TB_CYAN | TB_BOLD : TB_DEFAULT, TB_DEFAULT);
        const char *e = inl[0] ? inl : err;
        if (e) draw_clip(ix, y + 9, ix + iw, e, TB_RED | TB_BOLD, TB_DEFAULT);
        draw_clip(ix, y + bh - 1, ix + iw, " Enter connect · Tab next · Esc quit ",
                  TB_BLACK | TB_BOLD, TB_DEFAULT);
        tb_present();

        struct tb_event ev;
        if (tb_poll_event(&ev) != TB_OK) continue;
        if (ev.type != TB_EVENT_KEY) continue;
        if (ev.key == TB_KEY_ESC || ev.key == TB_KEY_CTRL_C || ev.key == TB_KEY_CTRL_Q)
            return LOGIN_QUIT;
        if (ev.key == TB_KEY_TAB || ev.key == TB_KEY_ARROW_DOWN) { focus = (focus + 1) & 3; continue; }
        if (ev.key == TB_KEY_ARROW_UP) { focus = (focus + 3) & 3; continue; }
        int is_space = (ev.ch == ' ' || ev.key == TB_KEY_SPACE);
        if (ev.key == TB_KEY_ENTER) {
            if (!f->workspace[0]) { snprintf(inl, sizeof inl, "enter a workspace (domain or name)"); focus = 0; continue; }
            if (!f->user[0])     { snprintf(inl, sizeof inl, "enter a username"); focus = 1; continue; }
            oc_resolve_status st = oc_resolve(f->workspace, getenv("OPENCHIME_SUFFIX"), &f->ep);
            if (st == OC_RESOLVE_BAD_WORKSPACE) { snprintf(inl, sizeof inl, "invalid workspace '%s'", f->workspace); focus = 0; continue; }
            if (st == OC_RESOLVE_NOT_FOUND)    { snprintf(inl, sizeof inl, "'%s' not found — does not resolve in DNS", f->workspace); focus = 0; continue; }
            return LOGIN_SUBMIT;
        }
        if (focus == 3) { if (is_space) f->remember = !f->remember; continue; }
        char *buf; size_t cap;
        if (focus == 0)      { buf = f->workspace; cap = sizeof f->workspace; }
        else if (focus == 1) { buf = f->user;     cap = sizeof f->user; }
        else                 { buf = f->pass;     cap = sizeof f->pass; }
        if (ev.key == TB_KEY_BACKSPACE || ev.key == TB_KEY_BACKSPACE2) {
            size_t n = strlen(buf); if (n) { buf[n - 1] = '\0'; inl[0] = '\0'; }
        } else if (is_space) {
            size_t n = strlen(buf); if (n + 1 < cap) { buf[n] = ' '; buf[n + 1] = '\0'; inl[0] = '\0'; }
        } else if (ev.ch >= 0x21 && ev.ch <= 0x7e) {   /* printable ASCII (credentials) */
            size_t n = strlen(buf); if (n + 1 < cap) { buf[n] = (char)ev.ch; buf[n + 1] = '\0'; inl[0] = '\0'; }
        }
    }
}

/* Tick a freshly-started client until it authenticates, fails, or is cancelled,
 * drawing a "connecting…" screen. Distinguishes auth-fail from unreachable via
 * the model's sticky last_error. */
static int await_auth(oc_client *cl, const char *host) {
    for (int i = 0; i < 1200; i++) {          /* ~18s at 15ms per tick */
        oc_client_tick(cl);
        const oc_model *m = oc_client_model(cl);
        if (m->authed) return AUTH_R_OK;
        if (m->last_error[0] && !m->connected)
            return strstr(m->last_error, "reach") ? AUTH_R_UNREACHABLE : AUTH_R_FAILED;
        int W = tb_width(), H = tb_height();
        tb_clear();
        char msg[320]; snprintf(msg, sizeof msg, "Connecting to %s …   (Esc to cancel)", host);
        draw_clip((W - (int)strlen(msg)) / 2, H / 2, W, msg, TB_WHITE | TB_BOLD, TB_DEFAULT);
        tb_present();
        struct tb_event ev;
        if (tb_peek_event(&ev, 15) == TB_OK && ev.type == TB_EVENT_KEY &&
            (ev.key == TB_KEY_ESC || ev.key == TB_KEY_CTRL_C || ev.key == TB_KEY_CTRL_Q))
            return AUTH_R_CANCELLED;
    }
    return AUTH_R_UNREACHABLE;
}

static int have_stored_token(const char *store_path, const char *host, int port,
                             oc_secret *secret);

/* Record a workspace in the book so the switcher can offer it later (REQ-012).
 * Best-effort: a store we can't open just means no switcher entry. */
static void remember_workspace(const char *key, const char *label, const char *user) {
    if (!g_store_path) return;
    oc_store *s = oc_store_open(g_store_path);
    if (!s) return;
    oc_store_workspace_remember(s, key, label, user, (uint64_t)time(NULL) * 1000);
    oc_store_close(s);
}

/* Drive the login box + connect, retrying on failure, until authenticated or the
 * user quits. On success fills `w` (client + key/label/user) and returns 1.
 * `initial_user` pre-fills the username when re-signing into a known workspace. */
static int run_login(const char *initial_workspace, const char *initial_user,
                     const char *store_path, oc_secret *secret, ws_session *w) {
    login_form f; memset(&f, 0, sizeof f);
    snprintf(f.workspace, sizeof f.workspace, "%s", initial_workspace ? initial_workspace : "");
    snprintf(f.user, sizeof f.user, "%s", initial_user ? initial_user : "");
    f.remember = 1;
    char err[320]; err[0] = '\0';
    for (;;) {
        if (login_dialog(&f, err[0] ? err : NULL) == LOGIN_QUIT) return 0;
        char cred[260]; snprintf(cred, sizeof cred, "%s:%s", f.user, f.pass);
        oc_client *cl = oc_client_start_secure(f.ep.host, f.ep.port, cred,
                                               f.remember ? store_path : NULL,
                                               f.remember ? secret : NULL);
        if (!cl) { snprintf(err, sizeof err, "could not start the client"); continue; }
        int res = await_auth(cl, f.ep.host);
        if (res == AUTH_R_OK) {
            memset(w, 0, sizeof *w);
            w->cl = cl;
            snprintf(w->key,   sizeof w->key,   "%s:%d", f.ep.host, f.ep.port);
            snprintf(w->label, sizeof w->label, "%s", f.workspace[0] ? f.workspace : f.ep.host);
            snprintf(w->user,  sizeof w->user,  "%s", f.user);
            /* Only remember it if the user asked us to keep the credential —
             * "remember me" off means leave no trace of this workspace. */
            if (f.remember) remember_workspace(w->key, w->label, w->user);
            return 1;
        }
        oc_client_stop(cl);
        if (res == AUTH_R_CANCELLED) return 0;
        if (res == AUTH_R_UNREACHABLE) snprintf(err, sizeof err, "could not reach %s", f.ep.host);
        else                          snprintf(err, sizeof err, "sign-in failed — check your username and password");
        f.pass[0] = '\0';   /* clear the password for the retry */
    }
}

/* Open (or re-focus) a workspace from the switcher. A stored session token
 * reconnects silently; without one we fall back to the login box pre-filled with
 * what the book knows. Returns the session index, or -1 if it didn't open. */
static int open_workspace(const char *key, const char *label, const char *user) {
    int existing = ws_find(key);
    if (existing >= 0) return existing;                  /* already open */
    if (g_nws >= MAX_WS) return -1;

    char host[256] = ""; int port = 0;
    const char *colon = strrchr(key, ':');
    if (!colon) return -1;
    size_t hl = (size_t)(colon - key);
    if (hl >= sizeof host) return -1;
    memcpy(host, key, hl); host[hl] = '\0';
    port = atoi(colon + 1);
    if (!port) return -1;

    ws_session *w = &g_ws[g_nws];
    if (have_stored_token(g_store_path, host, port, g_secret)) {
        oc_client *cl = oc_client_start_secure(host, port, "", g_store_path, g_secret);
        if (!cl) return -1;
        if (await_auth(cl, host) != AUTH_R_OK) {         /* token stale/rejected */
            oc_client_stop(cl);
        } else {
            memset(w, 0, sizeof *w);
            w->cl = cl;
            snprintf(w->key,   sizeof w->key,   "%s", key);
            snprintf(w->label, sizeof w->label, "%.255s", (label && label[0]) ? label : key);
            snprintf(w->user,  sizeof w->user,  "%s", user ? user : "");
            remember_workspace(w->key, w->label, w->user);
            return g_nws++;
        }
    }
    if (!run_login(label, user, g_store_path, g_secret, w)) return -1;
    return g_nws++;
}

/* Is a still-valid session token stored for this workspace? If so we skip the
 * login box and let the net thread reconnect silently. */
static int have_stored_token(const char *store_path, const char *host, int port,
                             oc_secret *secret) {
    oc_store *s = store_path ? oc_store_open(store_path) : NULL;
    if (!s && !secret) return 0;
    if (s) oc_store_set_secret(s, secret);   /* look in the keyring too */
    char inst[288]; snprintf(inst, sizeof inst, "%s:%d", host, port);
    uint8_t tok[OC_SESSION_TOKEN_LEN];
    int has = s ? oc_store_load_session(s, inst, tok, NULL, (uint64_t)time(NULL) * 1000) : 0;
    oc_store_close(s);
    return has;
}

int main(int argc, char **argv) {
    if (argc >= 2 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        fprintf(stderr, "usage: %s [<workspace>] [user:pass]\n"
                        "       %s <host> <port> [user:pass]\n"
                        "  With no workspace (or no credentials) a login box is shown.\n",
                        argv[0], argv[0]);
        return 0;
    }
    /* Adopt the environment's locale so termbox2's iswprint() check recognizes
     * non-ASCII codepoints as printable — without this it renders every emoji /
     * wide glyph as U+FFFD, defeating the whole point of utf8proc. */
    setlocale(LC_ALL, "");

    static oc_config cfg, file_cfg;
    oc_config_load(&cfg);
    file_cfg = cfg;          /* the machine-local base; synced settings layer over it */
    g_cfg = &cfg;

    const char *store_path = resolve_store_path();
    /* Prefer the OS keyring for the session token; NULL (headless / no keyring)
     * falls back to the SQLite store. Owned here, freed after the client stops. */
    oc_secret *secret = oc_tui_secret_open("openchime");
    g_store_path = store_path;    /* the switcher reads the book from here */
    g_secret = secret;

    /* Decide how to connect. Three shapes:
     *   <host> <port> [cred]  — dev/local direct connect (argv[2] is a port);
     *   <workspace> [cred]     — resolve by DNS (REQ-010), then connect;
     *   (no credentials)      — show the login box (REQ-020).
     * Connect directly when a credential is supplied or a session token is
     * already stored; otherwise prompt. A resolution failure is reported
     * distinctly from connect/auth failure (REQ-011). */
    char host[256] = ""; int port = 0; const char *cred = NULL;
    int direct = 0;
    const char *prefill = "";

    if (argc >= 3 && all_digits(argv[2])) {          /* dev host:port */
        snprintf(host, sizeof host, "%s", argv[1]);
        port = atoi(argv[2]);
        cred = argc > 3 ? argv[3] : getenv("OPENCHIME_CRED");
        direct = 1;
    } else if (argc >= 2 || cfg.workspace[0]) {       /* workspace mode (arg or config default) */
        const char *inst = (argc >= 2) ? argv[1] : cfg.workspace;
        const char *cli_cred = argc >= 3 ? argv[2] : getenv("OPENCHIME_CRED");
        oc_endpoint ep;
        oc_resolve_status st = oc_resolve(inst, getenv("OPENCHIME_SUFFIX"), &ep);
        if (st == OC_RESOLVE_BAD_WORKSPACE) { fprintf(stderr, "openchime: invalid workspace '%s'\n", inst); return 2; }
        if (st == OC_RESOLVE_NOT_FOUND)    { fprintf(stderr, "openchime: workspace '%s' not found — it does not resolve in DNS\n", inst); return 3; }
        snprintf(host, sizeof host, "%s", ep.host);
        port = ep.port;
        if (cli_cred && cli_cred[0])                               { cred = cli_cred; direct = 1; }
        else if (have_stored_token(store_path, host, port, secret)) { cred = "";      direct = 1; }  /* silent reconnect */
        else                                                       { prefill = inst; direct = 0; }  /* prompt */
    }                                                /* else: no args -> login box */
    if (!cred) cred = "";

    if (tb_init() != TB_OK) {
        fprintf(stderr, "failed to init terminal (need a tty)\n");
        return 1;
    }
    tb_set_input_mode(TB_INPUT_ESC | (cfg.mouse ? TB_INPUT_MOUSE : 0));

    /* The first session. Everything after this point works through g_ws, so the
     * command line is just one more way to open a workspace. */
    if (direct) {
        oc_client *c0 = oc_client_start_secure(host, port, cred, store_path, secret);
        if (!c0) { tb_shutdown(); oc_secret_free(secret); fprintf(stderr, "failed to start client\n"); return 1; }
        ws_session *w = &g_ws[0];
        memset(w, 0, sizeof *w);
        w->cl = c0;
        snprintf(w->key,   sizeof w->key,   "%s:%d", host, port);
        /* Fall back to the full "host:port", not the bare host — two dev
         * workspaces on the same host would otherwise be indistinguishable
         * in the switcher. */
        snprintf(w->label, sizeof w->label, "%.255s", prefill[0] ? prefill : w->key);
        /* `cred` is "user:pass" (or "" for a stored-token reconnect). */
        const char *colon = strchr(cred, ':');
        if (colon && colon > cred) {
            size_t ul = (size_t)(colon - cred);
            if (ul >= sizeof w->user) ul = sizeof w->user - 1;
            memcpy(w->user, cred, ul); w->user[ul] = '\0';
        }
        g_nws = 1;
        remember_workspace(w->key, w->label, w->user);
    } else if (run_login(prefill, NULL, store_path, secret, &g_ws[0])) {
        g_nws = 1;
    } else {
        tb_shutdown();       /* user quit the login box */
        oc_secret_free(secret);
        return 0;
    }
    g_active = 0;
    oc_client *cl = g_ws[0].cl;

    char composer[COMPOSER_CAP];
    size_t clen = 0;
    composer[0] = '\0';
    size_t focus = 0;
    int scroll = 0;
    int switcher_open = 0, wsel = 0;          /* workspace switcher (^W) */
    int storage_open = 0;                     /* /storage overlay (REQ-214) */
    int help_open = 0;
    int profile_open = 0;                     /* /profile identity modal */
    int ac_idx = 0;                           /* autocomplete cycle index */
    int panel = 0;                            /* 0 composer · 1 channels · 2 messages · 3 members */
    int msg_sel = -1, mem_sel = 0;            /* selection within the messages / members panel */
    uint64_t editing = 0;                     /* message id being edited (0 = none) */
    int palette_open = 0, psel = 0;           /* command palette (: or Ctrl-K) */
    char pq[64] = ""; size_t pqlen = 0;       /* palette query */
    int picker_open = 0, esel = 0;            /* emoji picker (r on a message) */
    char eq[32] = ""; size_t eqlen = 0;       /* picker query */
    uint64_t picker_cid = 0, picker_mid = 0;  /* message being reacted to */
    uint64_t last_focus_cid = 0;
    time_t last_typing = 0;                   /* throttle outbound TYPING signals */
    int running = 1, logging_out = 0;

    while (running) {
        /* Tick EVERY workspace, not just the visible one: a background session
         * has to keep receiving so its unread count is live in the switcher
         * (REQ-014). Only the active session is rendered, below. */
        for (int i = 0; i < g_nws; i++) {
            oc_client_tick(g_ws[i].cl);
            /* Each session pulls its own synced settings bucket once per
             * authenticated connection (a reconnect re-pulls). */
            const oc_model *wm = oc_client_model(g_ws[i].cl);
            if (!wm->connected) g_ws[i].settings_req = 0;
            else if (wm->authed && !g_ws[i].settings_req) {
                oc_client_list_settings(g_ws[i].cl);
                g_ws[i].settings_req = 1;
            }
        }
        cl = g_ws[g_active].cl;

        const oc_model *m = oc_client_model(cl);
        if (focus >= m->n_channels) focus = m->n_channels ? m->n_channels - 1 : 0;
        /* Layer the active workspace's synced settings over the file defaults,
         * re-arming mouse input if that pref flipped. */
        if (apply_synced_settings(&cfg, &file_cfg, m))
            tb_set_input_mode(TB_INPUT_ESC | (cfg.mouse ? TB_INPUT_MOUSE : 0));
        /* After /logout, close that workspace once the server has dropped it —
         * and only quit when it was the last one open. */
        if (logging_out && !m->connected) {
            oc_client_stop(cl);
            for (int i = g_active; i + 1 < g_nws; i++) g_ws[i] = g_ws[i + 1];
            g_nws--;
            logging_out = 0;
            if (g_nws == 0) { running = 0; continue; }
            if (g_active >= g_nws) g_active = g_nws - 1;
            /* Adopt the newly-active workspace's saved view state. */
            cl = g_ws[g_active].cl;
            focus = g_ws[g_active].focus; scroll = g_ws[g_active].scroll;
            snprintf(composer, sizeof composer, "%s", g_ws[g_active].composer);
            clen = strlen(composer);
            last_focus_cid = g_ws[g_active].last_focus_cid;
            msg_sel = -1;
            m = oc_client_model(cl);
            if (focus >= m->n_channels) focus = m->n_channels ? m->n_channels - 1 : 0;
        }

        /* Lazy backfill + keep the focused channel marked read. */
        if (focus < m->n_channels) {
            uint64_t cid = m->channels[focus].channel_id;
            oc_client_backfill(cl, cid);
            oc_client_mark_read(cl, cid);
            if (cid != last_focus_cid) { scroll = 0; msg_sel = -1; last_focus_cid = cid; }
        }

        render(cl, focus, composer, clen, scroll, help_open, ac_idx, panel, msg_sel, mem_sel, editing);
        if (palette_open) draw_palette(oc_client_model(cl), pq, psel);
        if (picker_open) draw_picker(eq, esel);
        if (profile_open) draw_profile(oc_client_model(cl), tb_width(), tb_height());
        if (switcher_open) draw_switcher(wsel);
        if (storage_open) draw_storage(oc_client_model(cl), tb_width(), tb_height());

        struct tb_event ev;
        int rc = tb_peek_event(&ev, 30);
        if (rc != TB_OK) continue;               /* timeout: loop to re-tick + redraw */
        if (ev.type == TB_EVENT_RESIZE) continue;
        if (ev.type == TB_EVENT_MOUSE) {       /* wheel scrolls; click focuses a channel/member */
            if (ev.key == TB_KEY_MOUSE_WHEEL_UP) scroll += 3;
            else if (ev.key == TB_KEY_MOUSE_WHEEL_DOWN) { scroll -= 3; if (scroll < 0) scroll = 0; }
            else if (ev.key == TB_KEY_MOUSE_LEFT && !help_open && !palette_open && !picker_open) {
                int Wm = tb_width(); int chw, memw, msgx, msgw;
                layout(Wm, &chw, &memw, &msgx, &msgw);
                int prow = ev.y - 2;           /* first panel content row */
                const oc_model *mm = oc_client_model(cl);
                if (prow >= 0 && ev.x < chw) {                      /* Channels */
                    if ((size_t)prow < mm->n_channels) { focus = (size_t)prow; panel = 0; }
                } else if (memw && prow >= 0 && ev.x >= msgx + msgw) {   /* Members */
                    if ((size_t)prow < mm->n_users) oc_client_open_dm(cl, mm->users[prow].user_id);
                }
            }
            continue;
        }
        if (ev.type != TB_EVENT_KEY) continue;

        size_t nch = oc_client_model(cl)->n_channels;
        if (switcher_open) {                   /* workspace switcher (REQ-013) */
            int rows = g_nsw + 1;              /* + the "log in to new" row */
            if (ev.key == TB_KEY_ESC || ev.key == TB_KEY_CTRL_C) switcher_open = 0;
            else if (ev.key == TB_KEY_CTRL_Q) running = 0;
            else if (ev.key == TB_KEY_ARROW_DOWN) { if (wsel + 1 < rows) wsel++; }
            else if (ev.key == TB_KEY_ARROW_UP)   { if (wsel > 0) wsel--; }
            else if (ev.ch == 'd' && wsel < g_nsw && g_sw[wsel].session < 0) {
                /* Forget a closed workspace — credentials + cache go with it.
                 * Only offered for closed ones; forgetting a live session would
                 * pull the store out from under its running net thread. */
                oc_store *s = g_store_path ? oc_store_open(g_store_path) : NULL;
                if (s) {
                    oc_store_set_secret(s, g_secret);
                    oc_store_workspace_forget(s, g_sw[wsel].key);
                    oc_store_close(s);
                }
                sw_build();
                if (wsel > g_nsw) wsel = g_nsw;
            } else if (ev.key == TB_KEY_ENTER) {
                /* Save the active workspace's view state before leaving it. */
                snprintf(g_ws[g_active].composer, sizeof g_ws[g_active].composer, "%s", composer);
                g_ws[g_active].clen = clen;
                g_ws[g_active].focus = focus;
                g_ws[g_active].scroll = scroll;
                g_ws[g_active].last_focus_cid = last_focus_cid;

                int target;
                if (wsel >= g_nsw) {           /* "+ Log in to new workspace" */
                    if (g_nws >= MAX_WS) { switcher_open = 0; continue; }
                    target = run_login(NULL, NULL, store_path, secret, &g_ws[g_nws])
                           ? g_nws++ : -1;
                } else if (g_sw[wsel].session >= 0) {
                    target = g_sw[wsel].session;
                } else {
                    target = open_workspace(g_sw[wsel].key, g_sw[wsel].label, g_sw[wsel].user);
                }
                switcher_open = 0;
                if (target < 0) continue;      /* cancelled / failed: stay put */
                g_active = target;
                /* Restore the target's view state (REQ-015). */
                cl = g_ws[g_active].cl;
                focus = g_ws[g_active].focus;
                scroll = g_ws[g_active].scroll;
                snprintf(composer, sizeof composer, "%s", g_ws[g_active].composer);
                clen = strlen(composer);
                last_focus_cid = g_ws[g_active].last_focus_cid;
                msg_sel = -1; panel = 0; editing = 0;
                remember_workspace(g_ws[g_active].key, NULL, NULL);   /* bump last-used */
            }
            continue;
        }
        if (storage_open) {                    /* storage overlay: any key closes */
            if (ev.key == TB_KEY_CTRL_Q || ev.key == TB_KEY_CTRL_C) running = 0;
            else storage_open = 0;
            continue;
        }
        if (help_open) {                       /* help overlay: any key closes it */
            if (ev.key == TB_KEY_CTRL_Q || ev.key == TB_KEY_CTRL_C) running = 0;
            else help_open = 0;
            continue;
        }
        if (profile_open) {                    /* profile modal: any key closes it */
            if (ev.key == TB_KEY_CTRL_Q || ev.key == TB_KEY_CTRL_C) running = 0;
            else profile_open = 0;
            continue;
        }
        if (palette_open) {                    /* command palette: type to filter */
            const oc_model *mm = oc_client_model(cl);
            pal_item items[64]; int n = palette_build(mm, pq, items, 64);
            if (ev.key == TB_KEY_ESC || ev.key == TB_KEY_CTRL_C || ev.key == TB_KEY_CTRL_Q) {
                palette_open = 0;
            } else if (ev.key == TB_KEY_ARROW_DOWN) { if (psel + 1 < n) psel++; }
            else if (ev.key == TB_KEY_ARROW_UP)     { if (psel > 0) psel--; }
            else if (ev.key == TB_KEY_BACKSPACE || ev.key == TB_KEY_BACKSPACE2) {
                if (pqlen) pq[--pqlen] = '\0';
                psel = 0;
            } else if (ev.key == TB_KEY_ENTER) {
                if (n > 0 && psel < n) {
                    pal_item *it = &items[psel];
                    uint64_t cid = (focus < mm->n_channels) ? mm->channels[focus].channel_id : 0;
                    if (it->kind == PAL_CHAN) {
                        for (size_t z = 0; z < mm->n_channels; z++)
                            if (mm->channels[z].channel_id == it->id) { focus = z; break; }
                    } else if (it->kind == PAL_DM) {
                        oc_client_open_dm(cl, it->id);
                    } else if (strcmp(it->cmd, "/logout") == 0) {
                        oc_client_logout(cl, OC_LOGOUT_THIS); logging_out = 1;
                    } else if (strcmp(it->cmd, "/help") == 0) {
                        help_open = 1;
                    } else if (!it->needs_arg) {
                        handle_command(cl, cid, it->cmd);
                    } else {                   /* command needs an argument: prefill composer */
                        snprintf(composer, sizeof composer, "%s ", it->cmd);
                        clen = strlen(composer); panel = 0;
                    }
                }
                palette_open = 0;
            } else if (ev.ch >= 0x20 && ev.ch <= 0x7e && pqlen + 1 < sizeof pq) {
                pq[pqlen++] = (char)ev.ch; pq[pqlen] = '\0'; psel = 0;
            }
            continue;
        }
        if (picker_open) {                     /* emoji picker: type to filter, Enter reacts */
            const char *em[64], *nm[64]; int n = picker_build(eq, em, nm, 64);
            if (ev.key == TB_KEY_ESC || ev.key == TB_KEY_CTRL_C || ev.key == TB_KEY_CTRL_Q) {
                picker_open = 0;
            } else if (ev.key == TB_KEY_ARROW_DOWN) { if (esel + 1 < n) esel++; }
            else if (ev.key == TB_KEY_ARROW_UP)     { if (esel > 0) esel--; }
            else if (ev.key == TB_KEY_BACKSPACE || ev.key == TB_KEY_BACKSPACE2) {
                if (eqlen) eq[--eqlen] = '\0';
                esel = 0;
            } else if (ev.key == TB_KEY_ENTER) {
                if (n > 0 && esel < n) {
                    int op = my_reaction(oc_client_model(cl), picker_cid, picker_mid, em[esel]) ? 0 : 1;
                    oc_client_react(cl, picker_cid, picker_mid, em[esel], op);   /* toggle */
                }
                picker_open = 0;
            } else if (ev.ch >= 0x20 && ev.ch <= 0x7e && eqlen + 1 < sizeof eq) {
                eq[eqlen++] = (char)ev.ch; eq[eqlen] = '\0'; esel = 0;
            }
            continue;
        }
        if (ev.key == TB_KEY_CTRL_K) {         /* open the palette from any mode */
            palette_open = 1; pq[0] = '\0'; pqlen = 0; psel = 0;
            continue;
        }
        if (ev.key == TB_KEY_CTRL_R) {         /* force an immediate reconnect */
            oc_client_reconnect(cl);
            continue;
        }
        if (ev.key == TB_KEY_CTRL_W) {         /* workspace switcher (REQ-013) */
            sw_build();
            wsel = (g_active < g_nsw) ? g_active : 0;
            switcher_open = 1;
            continue;
        }
        if (ev.key != TB_KEY_TAB) ac_idx = 0;  /* any non-Tab key restarts cycling */

        /* ---- navigation mode (a panel is focused; single keys act) ---- */
        if (panel != 0) {
            const oc_model *mm = oc_client_model(cl);
            const oc_channel *ch = (focus < mm->n_channels) ? &mm->channels[focus] : NULL;
            uint64_t cid = ch ? ch->channel_id : 0;
            int up   = (ev.ch == 'k' || ev.key == TB_KEY_ARROW_UP);
            int down = (ev.ch == 'j' || ev.key == TB_KEY_ARROW_DOWN);
            if (ev.key == TB_KEY_CTRL_Q || ev.key == TB_KEY_CTRL_C) { running = 0; }
            else if (ev.key == TB_KEY_ESC || ev.ch == 'i') { panel = 0; }   /* back to composing */
            else if (ev.key == TB_KEY_TAB) {                                /* cycle panels 1→2→3 */
                panel = (panel == 1) ? 2 : (panel == 2) ? 3 : 1;
                if (panel == 2 && ch && (msg_sel < 0 || msg_sel >= (int)ch->n_msgs))
                    msg_sel = ch->n_msgs ? (int)ch->n_msgs - 1 : -1;
                if (panel == 3 && mem_sel >= (int)mm->n_users)
                    mem_sel = mm->n_users ? (int)mm->n_users - 1 : 0;
            }
            else if (panel == 1) {                                          /* channels */
                if (up && focus > 0) focus--;
                else if (down && focus + 1 < mm->n_channels) focus++;
                else if (ev.key == TB_KEY_ENTER) panel = 0;   /* it's already focused; compose */
            }
            else if (panel == 3) {                                          /* members */
                if (up && mem_sel > 0) mem_sel--;
                else if (down && mem_sel + 1 < (int)mm->n_users) mem_sel++;
                else if (ev.key == TB_KEY_ENTER && mem_sel < (int)mm->n_users) {
                    oc_client_open_dm(cl, mm->users[mem_sel].user_id); panel = 0;
                }
            }
            else if (panel == 2 && ch) {                                    /* messages */
                int n = (int)ch->n_msgs;
                if (up)        { if (msg_sel > 0) msg_sel--; }
                else if (down) { if (msg_sel + 1 < n) msg_sel++; }
                else if (msg_sel >= 0 && msg_sel < n) {
                    const oc_msg *sel = &ch->msgs[msg_sel];
                    uint64_t mid = sel->message_id;
                    int own = (sel->author_id == mm->user_id) && !sel->deleted;
                    if (ev.key == TB_KEY_ENTER || ev.ch == 't') oc_client_open_thread(cl, cid, mid);
                    else if (ev.ch == 'w') oc_client_list_reactions(cl, cid, mid);
                    else if ((ev.ch == 'x' || ev.ch == 'd') && own) oc_client_delete(cl, cid, mid);
                    else if (ev.ch == 'r' && !sel->deleted) {   /* open the emoji picker */
                        picker_open = 1; eq[0] = '\0'; eqlen = 0; esel = 0;
                        picker_cid = cid; picker_mid = mid;
                    }
                    else if (ev.ch == 'e' && own) {             /* edit: prefill + drop to composer */
                        editing = mid;
                        snprintf(composer, sizeof composer, "%s", sel->body ? sel->body : "");
                        clen = strlen(composer);
                        panel = 0;
                    }
                }
            }
            continue;
        }

        if (ev.key == TB_KEY_CTRL_Q || ev.key == TB_KEY_CTRL_C) {
            running = 0;
        } else if (ev.key == TB_KEY_ESC) {    /* composer → navigation mode */
            if (editing) { editing = 0; composer[0] = '\0'; clen = 0; }
            else { panel = 2; if (focus < nch) msg_sel = m->channels[focus].n_msgs ? (int)m->channels[focus].n_msgs - 1 : -1; }
        } else if (ev.key == TB_KEY_ENTER) {
            if (editing) {                     /* apply the edit (or cancel if emptied) */
                if (clen > 0 && focus < nch) oc_client_edit(cl, m->channels[focus].channel_id, editing, composer);
                editing = 0; clen = 0; composer[0] = '\0'; scroll = 0;
            } else if (clen > 0 && focus < nch) {
                const oc_model *mm = oc_client_model(cl);
                uint64_t cid = mm->channels[focus].channel_id;
                if (strcmp(composer, "/help") == 0)           { help_open = 1; }
                else if (strcmp(composer, "/profile") == 0)   { profile_open = 1; }
                else if (strcmp(composer, "/storage") == 0) {
                    oc_client_storage_status(cl);   /* refuses for a member */
                    storage_open = 1;
                }
                else if (strcmp(composer, "/workspaces") == 0 || strcmp(composer, "/workspace") == 0) {
                    sw_build();
                    wsel = (g_active < g_nsw) ? g_active : 0;
                    switcher_open = 1;
                }
                else if (strcmp(composer, "/logout") == 0)    { oc_client_logout(cl, OC_LOGOUT_THIS); logging_out = 1; }
                else if (composer[0] == '/')                  handle_command(cl, cid, composer);
                else if (mm->search_open || mm->roster_open || mm->reactlist_open || mm->prefs_open || mm->weblist_open) { /* read-only overlay: ignore */ }
                else if (mm->thread_open)                     oc_client_reply(cl, mm->thread_channel, mm->thread_parent, composer);
                else                                          oc_client_send(cl, cid, composer);
                clen = 0; composer[0] = '\0'; scroll = 0;
            }
        } else if (ev.key == TB_KEY_BACKSPACE || ev.key == TB_KEY_BACKSPACE2) {
            backspace_utf8(composer, &clen);
        } else if (ev.key == TB_KEY_TAB) {
            if (clen > 0) {                    /* autocomplete the trailing token */
                ac_cand cands[16]; int rs;
                int n = ac_candidates(oc_client_model(cl), composer, cands, 16, &rs);
                if (n > 0) {
                    const char *repl = cands[((ac_idx % n) + n) % n].repl;
                    char nb[COMPOSER_CAP];
                    int k = snprintf(nb, sizeof nb, "%.*s%s", rs, composer, repl);
                    if (k > 0 && k < (int)sizeof nb) { memcpy(composer, nb, (size_t)k + 1); clen = (size_t)k; }
                    ac_idx++;
                }
            } else {                           /* empty composer: enter navigation mode */
                panel = 2;
                if (focus < nch) msg_sel = m->channels[focus].n_msgs ? (int)m->channels[focus].n_msgs - 1 : -1;
            }
        } else if (ev.key == TB_KEY_ARROW_UP) {
            scroll += 1;
        } else if (ev.key == TB_KEY_ARROW_DOWN) {
            if (scroll > 0) scroll -= 1;
        } else if (ev.key == TB_KEY_PGUP) {
            scroll += (tb_height() - 3) / 2;
        } else if (ev.key == TB_KEY_PGDN) {
            scroll -= (tb_height() - 3) / 2;
            if (scroll < 0) scroll = 0;
        } else if (ev.ch == '?' && clen == 0) {
            help_open = 1;                     /* ? on an empty composer opens help */
        } else if (ev.ch == ':' && clen == 0) {
            palette_open = 1; pq[0] = '\0'; pqlen = 0; psel = 0;   /* : opens the palette */
        } else if (ev.ch != 0) {
            /* A typed codepoint: append its UTF-8 encoding to the composer. */
            utf8proc_uint8_t enc[4];
            utf8proc_ssize_t k = utf8proc_encode_char((utf8proc_int32_t)ev.ch, enc);
            if (k > 0 && clen + (size_t)k + 1 < sizeof composer) {
                memcpy(composer + clen, enc, (size_t)k);
                clen += (size_t)k;
                composer[clen] = '\0';
            }
            /* Signal typing while composing a message (not a /command), throttled. */
            time_t now = time(NULL);
            if (focus < nch && composer[0] != '/' && now - last_typing >= 3) {
                oc_client_typing(cl, oc_client_model(cl)->channels[focus].channel_id);
                last_typing = now;
            }
        }
    }

    tb_shutdown();
    for (int i = 0; i < g_nws; i++) oc_client_stop(g_ws[i].cl);
    g_nws = 0;
    oc_secret_free(secret);
    return 0;
}
