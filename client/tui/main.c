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
 * active one. Ctrl+W opens the switcher: remembered workspaces with their unread +
 * connection state, and an always-present "Log in to new workspace" row. Each
 * session carries its own focused channel, scroll, and half-typed message, so
 * switching away and back doesn't lose work.
 *
 * Usage: openchime-tui [<workspace>] [user:pass]   (login box if omitted)
 *        openchime-tui <host> <port> [user:pass]   (dev/local direct connect)
 *        (credentials also read from $OPENCHIME_CRED = "user:pass")
 *
 * Keys: everything is menus, dialogs, and screens — no commands to type. The
 *       composer only sends: Enter posts your message (or, inside a thread, posts
 *       a reply). Tab completes only the trailing token — @mentions, #channels,
 *       :emoji: — or, on an empty composer, enters navigation mode; ↑/↓ + PgUp/PgDn
 *       scroll; ? help. Navigation is Esc/Tab plus Ctrl chords: Ctrl+F search ·
 *       Ctrl+W workspaces · Ctrl+R reconnect · Ctrl+Q quit. **Navigation mode**
 *       (Esc from the composer): a panel is focused — Tab cycles Channels /
 *       Messages / Members, ↑/↓ select, Esc returns to the composer. Every action
 *       lives on a pane action menu: Enter on the selected message, member, or
 *       channel opens the menu of things you can do to it (react via a filterable
 *       emoji picker, edit, delete, open a thread, who-reacted, open a DM, and so
 *       on); anything needing input opens a modal prompt dialog. **Command
 *       launcher** (Ctrl+K anywhere, or `:` on an empty composer): fuzzy-filter
 *       every action + jump to any channel/DM; Enter runs it (or opens its dialog).
 *       On-screen key hints are written in "Ctrl+X" form.
 */

#include "tuikit.h"      /* terminal layer + width-correct draw/style primitives (tuikit) */
#include "oc_port.h"

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

/* The width-correct draw primitives (tk_cp_width/tk_text/tk_fill) now live in
 * tuikit as tk_cp_width/tk_text/tk_fill; panels/frames as tk_panel/tk_box. */

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
            int w = tk_cp_width(cp);
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
        if (oc_localtime_r(&t, &tmv))
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
        uintattr_t acol = TB_CYAN | TB_BOLD;
        if (a->reclaimed) {
            /* The server removed the bytes by age or storage pressure
             * (REQ-215/217) and kept the row so the conversation still reads.
             * Say so in place, dimmed and without an id: offering a download
             * that is guaranteed to fail would look like a broken client. */
            snprintf(al, sizeof al, "    \xf0\x9f\x93\x8e %s \xe2\x80\x94 no longer available",
                     a->filename[0] ? a->filename : "file");
            acol = TB_BLACK | TB_BOLD;
        } else {
            /* 📎 filename (12.3 KB) #id — download with /download <id> */
            double kb = (double)a->size / 1024.0;
            if (a->size >= 1024ull * 1024)
                snprintf(al, sizeof al, "    \xf0\x9f\x93\x8e %s (%.1f MB) #%llu",
                         a->filename[0] ? a->filename : "file", kb / 1024.0, (unsigned long long)a->id);
            else
                snprintf(al, sizeof al, "    \xf0\x9f\x93\x8e %s (%.1f KB) #%llu",
                         a->filename[0] ? a->filename : "file", kb, (unsigned long long)a->id);
        }
        char *ar = malloc(strlen(al) + 1);
        if (ar) { strcpy(ar, al); rows_push(r, ar, acol); }
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
            if (oc_localtime_r(&t, &tv)) strftime(stamp, sizeof stamp, "%H:%M", &tv); }
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
/* Center a modal sized to `cw` content columns × `rows` content lines, fill and
 * frame it with a consistent title, and return the inner content region. Every
 * overlay uses this so padding, sizing, and the title style stay uniform, and the
 * box grows to fit its content (no truncation). */
static tk_rect modal_frame(int W, int H, int cw, int rows, const char *title) {
    int bw = cw + 6;                     /* 2-col padding inside the border */
    if (bw > W - 4) bw = W - 4;
    if (bw < 24) bw = 24;
    int bh = rows + 4;                   /* border + a blank pad top & bottom */
    if (bh > H - 2) bh = H - 2;
    if (bh < 5) bh = 5;
    int x = (W - bw) / 2, y = (H - bh) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    const tk_theme *th = tk_theme_active();
    for (int j = 0; j < bh; j++) tk_fill(y + j, x, x + bw, th->bg);
    tk_panel(x, y, bw, bh, title, 1);
    return (tk_rect){ x + 3, y + 2, bw - 6, bh - 4 };
}

/* The ? help overlay: a clean, centered two-column key reference. A row with a
 * NULL key is a section heading; a row with both fields NULL is a blank spacer.
 * The box is sized to its content so nothing is ever truncated. */
static void draw_help(int W, int H) {
    typedef struct { const char *key, *desc; } hrow;
    static const hrow R[] = {
        { NULL, "Basics" },
        { "Enter",      "send the typed message" },
        { "Tab · Esc",  "move between the panes and the composer" },
        { "Ctrl+K · :", "action menu — reach everything" },
        { "Ctrl+F",     "search messages" },
        { "Ctrl+W",     "switch workspace" },
        { "Ctrl+R",     "reconnect" },
        { "Ctrl+Q",     "quit" },
        { NULL, NULL },
        { NULL, "Lists   ↑/↓ move · Enter opens or acts" },
        { "Channels",   "n  new channel" },
        { "Members",    "n  new DM · Enter for message / role / remove" },
        { "Messages",   "t thread · r react · e edit · x delete" },
        { NULL, NULL },
        { NULL, "Everything else" },
        { "Ctrl+K",     "notifications, invites, webhooks, profile, upload, logout…" },
    };
    int n = (int)(sizeof R / sizeof R[0]);
    const tk_theme *th = tk_theme_active();

    /* Column widths, and the widest rendered row — size the box to fit them. */
    int keyw = 0, bodyw = 0;
    for (int i = 0; i < n; i++)
        if (R[i].key) { int kw = tk_str_width(R[i].key); if (kw > keyw) keyw = kw; }
    for (int i = 0; i < n; i++) {
        int w = R[i].key ? keyw + 2 + tk_str_width(R[i].desc)
              : R[i].desc ? tk_str_width(R[i].desc) : 0;
        if (w > bodyw) bodyw = w;
    }

    int bw = bodyw + 6;                       /* 2-col padding inside the border */
    if (bw > W - 4) bw = W - 4;
    int bh = n + 4;                           /* border + a blank pad top & bottom */
    if (bh > H - 2) bh = H - 2;
    int x = (W - bw) / 2, y = (H - bh) / 2;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    for (int j = 0; j < bh; j++) tk_fill(y + j, x, x + bw, th->bg);
    tk_panel(x, y, bw, bh, "Help  ·  Esc closes", 1);

    int cx = x + 3, xmax = x + bw - 3, ry = y + 2;
    for (int i = 0; i < n && ry < y + bh - 1; i++, ry++) {
        if (!R[i].key && !R[i].desc) continue;                 /* spacer */
        if (!R[i].key) {                                       /* section heading */
            tk_text(cx, ry, xmax, R[i].desc, th->accent2 | TB_BOLD, th->bg);
        } else {
            tk_text(cx, ry, xmax, R[i].key, th->accent | TB_BOLD, th->bg);
            tk_text(cx + keyw + 2, ry, xmax, R[i].desc, th->fg, th->bg);
        }
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
    const tk_theme *th = tk_theme_active();
    char idbuf[32];
    snprintf(idbuf, sizeof idbuf, "%llu", (unsigned long long)m->user_id);
    struct { const char *label, *value; } R[] = {
        { "Name",     me[0] ? me : "(unknown)" },
        { "User ID",  idbuf },
        { "Role",     rn },
        { "Presence", pn },
    };
    int n = (int)(sizeof R / sizeof R[0]);
    const char *hint = "Change your name or password from the action menu (Ctrl+K).";

    int labelw = 0, cw = tk_str_width(hint);
    for (int i = 0; i < n; i++) { int w = tk_str_width(R[i].label); if (w > labelw) labelw = w; }
    for (int i = 0; i < n; i++) {
        int w = labelw + 2 + tk_str_width(R[i].value);
        if (w > cw) cw = w;
    }
    tk_rect r = modal_frame(W, H, cw, n + 2, "Profile  ·  Esc closes");
    for (int i = 0; i < n; i++) {
        tk_text(r.x, r.y + i, r.x + r.w, R[i].label, th->muted, th->bg);
        tk_text(r.x + labelw + 2, r.y + i, r.x + r.w, R[i].value, th->fg | TB_BOLD, th->bg);
    }
    tk_text(r.x, r.y + n + 1, r.x + r.w, hint, th->faint, th->bg);
    tb_present();
}

/* ---- composer autocomplete (commands, channels, @users, :emoji:) ----------- */

typedef struct { char repl[80]; char disp[96]; } ac_cand;

static int ci_prefix(const char *s, const char *pre) {
    for (; *pre; s++, pre++)
        if (!*s || tolower((unsigned char)*s) != tolower((unsigned char)*pre)) return 0;
    return 1;
}


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

    /* No slash-command completion: the composer is menu-driven and doesn't run
     * commands. Tab still completes content — @mentions, #channels, :emoji:. */
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
    return 0;
}

/* ---- action menus (menu-driven; ARCH-83) ---------------------------------- *
 * A discoverable tk_list-in-a-modal that replaces cryptic nav keys and slash
 * commands. One generic menu holds {label,id} items; the SELECT handler
 * dispatches on the ACT_* id. Enter on a selected message/member opens it. */
enum { ACT_THREAD = 1, ACT_REACT, ACT_EDIT, ACT_DELETE, ACT_REACTORS, ACT_DOWNLOAD,
       ACT_DM, ACT_ROLE_ADMIN, ACT_ROLE_MEMBER, ACT_REMOVE,
       /* channel menu */
       ACT_OPEN, ACT_JOIN, ACT_NOTIFY_ALL, ACT_NOTIFY_MENTIONS, ACT_NOTIFY_NONE, ACT_WEBHOOK_CREATE,
       /* global action launcher (Ctrl-K) — replaces the remaining slash commands */
       ACT_NEWCHAN, ACT_NEWDM, ACT_SEARCH, ACT_NICK, ACT_AWAY, ACT_ONLINE, ACT_DND, ACT_PASSWD,
       ACT_PREFS, ACT_WEBHOOKS, ACT_LEAVE, ACT_INVITE, ACT_PROFILE, ACT_UPLOAD,
       ACT_STORAGE, ACT_AUDIT, ACT_WORKSPACES, ACT_HELP, ACT_LOGOUT };
typedef struct { const char *label; int id; } menuitem;
static menuitem g_menu[28];
static int      g_nmenu;
static int  menu_count(void *ud) { (void)ud; return g_nmenu; }
static void menu_text(int i, void *ud, char *buf, size_t cap, uintattr_t *fg) {
    (void)ud; *fg = TB_DEFAULT; snprintf(buf, cap, "%s", g_menu[i].label);
}
static void menu_build_msg(int own, int deleted, int has_attach) {
    g_nmenu = 0;
    if (!deleted) g_menu[g_nmenu++] = (menuitem){ "Reply in thread", ACT_THREAD };
    if (!deleted) g_menu[g_nmenu++] = (menuitem){ "Add reaction",    ACT_REACT };
    if (has_attach) g_menu[g_nmenu++] = (menuitem){ "Download file", ACT_DOWNLOAD };
    if (own)      g_menu[g_nmenu++] = (menuitem){ "Edit",            ACT_EDIT };
    if (own)      g_menu[g_nmenu++] = (menuitem){ "Delete",          ACT_DELETE };
    g_menu[g_nmenu++] = (menuitem){ "Who reacted", ACT_REACTORS };
}
static void menu_build_member(int is_self) {
    g_nmenu = 0;
    if (!is_self) g_menu[g_nmenu++] = (menuitem){ "Message",     ACT_DM };
    g_menu[g_nmenu++] = (menuitem){ "Make admin",  ACT_ROLE_ADMIN };
    g_menu[g_nmenu++] = (menuitem){ "Make member", ACT_ROLE_MEMBER };
    if (!is_self) g_menu[g_nmenu++] = (menuitem){ "Remove",      ACT_REMOVE };
}
static void menu_build_channel(int joined) {
    g_nmenu = 0;
    if (!joined) g_menu[g_nmenu++] = (menuitem){ "Join",              ACT_JOIN };
    g_menu[g_nmenu++] = (menuitem){ "Open",                ACT_OPEN };
    g_menu[g_nmenu++] = (menuitem){ "Notify: all",         ACT_NOTIFY_ALL };
    g_menu[g_nmenu++] = (menuitem){ "Notify: mentions",    ACT_NOTIFY_MENTIONS };
    g_menu[g_nmenu++] = (menuitem){ "Notify: none",        ACT_NOTIFY_NONE };
    g_menu[g_nmenu++] = (menuitem){ "Webhooks",            ACT_WEBHOOKS };
    g_menu[g_nmenu++] = (menuitem){ "Create webhook",      ACT_WEBHOOK_CREATE };
    if (joined)  g_menu[g_nmenu++] = (menuitem){ "Leave",  ACT_LEAVE };
}
/* The global action launcher (Ctrl-K / ':') — a tk_palette: incremental search,
 * sections, right-aligned key hints. Every action reachable without a command. */
static const tk_pal_item g_launcher_items[] = {
    { "Channel", "New channel",         NULL, "n",  ACT_NEWCHAN },
    { "Channel", "Leave this channel",  NULL, NULL, ACT_LEAVE },
    { "Channel", "Notifications",       NULL, NULL, ACT_PREFS },
    { "Channel", "Channel webhooks",    NULL, NULL, ACT_WEBHOOKS },
    { "Channel", "Upload a file",       NULL, NULL, ACT_UPLOAD },
    { "Messages","Search messages",     NULL, "Ctrl+F", ACT_SEARCH },
    { "You",     "New direct message",  NULL, NULL, ACT_NEWDM },
    { "You",     "Set away",            NULL, NULL, ACT_AWAY },
    { "You",     "Set online",          NULL, NULL, ACT_ONLINE },
    { "You",     "Change display name", NULL, NULL, ACT_NICK },
    { "You",     "Change password",     NULL, NULL, ACT_PASSWD },
    { "You",     "Do not disturb",      NULL, NULL, ACT_DND },
    { "You",     "Your profile",        NULL, NULL, ACT_PROFILE },
    { "Admin",   "Invite a user",       NULL, NULL, ACT_INVITE },
    { "Admin",   "Storage usage",       NULL, NULL, ACT_STORAGE },
    { "Admin",   "Audit log",           NULL, NULL, ACT_AUDIT },
    { "Workspace","Switch workspace",   NULL, "Ctrl+W", ACT_WORKSPACES },
    { "Session", "Help",                NULL, "?",  ACT_HELP },
    { "Session", "Log out",             NULL, NULL, ACT_LOGOUT },
};

/* ---- sidebar grouping (public / DM / private, each collapsible) ------------ */
enum { SBG_PUBLIC = 0, SBG_DM = 1, SBG_PRIVATE = 2, SBG_N = 3 };
static const char *SBG_LABEL[SBG_N] = { "Public", "Direct messages", "Private" };
static uint8_t g_grp_collapsed[SBG_N];   /* per-group fold state, persists across frames */
static int g_sb_hdr = -1;                /* if >=0, a group header is the selection (else a channel) */

static int chan_group(const oc_channel *c) {
    if (c->kind == OC_CHANNEL_KIND_DM) return SBG_DM;
    return c->is_public ? SBG_PUBLIC : SBG_PRIVATE;
}

/* One visible sidebar row: a group header, or a channel (idx into m->channels). */
typedef struct { uint8_t header; uint8_t group; size_t idx; } sb_row;

/* Build the ordered visible rows: for each non-empty group in fixed order, a
 * header, then (unless the group is collapsed) its channels in model order. */
static int sidebar_build(const oc_model *m, sb_row *out, int cap) {
    int n = 0;
    for (int g = 0; g < SBG_N; g++) {
        int has = 0;
        for (size_t i = 0; i < m->n_channels; i++)
            if (chan_group(&m->channels[i]) == g) { has = 1; break; }
        if (!has) continue;
        if (n < cap) out[n++] = (sb_row){ 1, (uint8_t)g, 0 };
        if (g_grp_collapsed[g]) continue;
        for (size_t i = 0; i < m->n_channels && n < cap; i++)
            if (chan_group(&m->channels[i]) == g) out[n++] = (sb_row){ 0, (uint8_t)g, i };
    }
    return n;
}

/* Marker + name for a channel row, e.g. "# general", "@alice", "\U0001F512 secret". */
static void sidebar_label(const oc_model *m, const oc_channel *c, char *buf, size_t cap) {
    if (c->kind == OC_CHANNEL_KIND_DM) {
        const char *pn = (c->peer_id == m->user_id) ? "you" : oc_model_user_name(m, c->peer_id);
        snprintf(buf, cap, "@%s", pn[0] ? pn : "dm");
    } else if (c->is_public) {
        snprintf(buf, cap, "# %s", c->name ? c->name : "…");
    } else {
        snprintf(buf, cap, "\xf0\x9f\x94\x92 %s", c->name ? c->name : "…");  /* 🔒 */
    }
}

/* Locate the current selection's display-row index (header or channel==focus). */
static int sidebar_cursor(const sb_row *rows, int n, size_t focus) {
    for (int i = 0; i < n; i++) {
        if (g_sb_hdr >= 0) { if (rows[i].header && rows[i].group == g_sb_hdr) return i; }
        else if (!rows[i].header && rows[i].idx == focus) return i;
    }
    return 0;
}

/* Move the selection to display row i, updating focus / g_sb_hdr accordingly. */
static void sidebar_select(const sb_row *rows, int i, size_t *focus) {
    if (rows[i].header) g_sb_hdr = rows[i].group;
    else { g_sb_hdr = -1; *focus = rows[i].idx; }
}

static void render(oc_client *cl, size_t focus, const char *composer,
                   size_t clen, int scroll, int help_open, int ac_idx,
                   int panel, int msg_sel, int mem_sel, uint64_t editing) {
    const oc_model *m = oc_client_model(cl);
    const tk_theme *th = tk_theme_active();
    int ch_act = (panel == 1), msg_act = (panel == 0 || panel == 2), mem_act = (panel == 3);
    int W = tb_width(), H = tb_height();
    tb_clear();
    if (W < 24 || H < 8) { tk_text(0, 0, W, " terminal too small ", TB_RED | TB_BOLD, TB_DEFAULT); tb_present(); return; }

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
    tk_fill(0, 0, W, th->header_bg);
    tk_text(0, 0, W, hdr, th->fg | TB_BOLD, th->header_bg);
    /* Unread here, and — so a background workspace isn't invisible — a count of
     * what's waiting in the others (REQ-014). */
    int other = 0;
    for (int i = 0; i < g_nws; i++) if (i != g_active) other += ws_unread(&g_ws[i]);
    char u[64];
    if (unread && other)   snprintf(u, sizeof u, "%d unread · %d elsewhere ", unread, other);
    else if (unread)       snprintf(u, sizeof u, "%d unread ", unread);
    else if (other)        snprintf(u, sizeof u, "%d elsewhere ", other);
    else                   u[0] = '\0';
    if (u[0]) tk_text(W - (int)strlen(u) - 1, 0, W, u, th->accent2 | TB_BOLD, th->header_bg);

    /* Rows: header=0; panels=[1, H-3); status=H-3; composer=H-2; hint=H-1. */
    int panels_top = 1, panels_h = H - 4;
    if (panels_h < 3) panels_h = 3;
    int ch_w, mem_w, msg_x, msg_w;
    layout(W, &ch_w, &mem_w, &msg_x, &msg_w);

    /* Channels panel — grouped (public / DMs / private), each group foldable,
     * channels indented under their header. */
    tk_panel(0, panels_top, ch_w, panels_h, "Channels", ch_act);
    {
        sb_row rows[256];
        int nrows = sidebar_build(m, rows, 256);
        for (int r = 0, iy = panels_top + 1;
             r < nrows && iy < panels_top + panels_h - 1; r++, iy++) {
            int hdr_sel = rows[r].header && g_sb_hdr == rows[r].group;
            int chn_sel = !rows[r].header && g_sb_hdr < 0 && rows[r].idx == focus;
            int sel = hdr_sel || chn_sel;
            uintattr_t bg = sel ? th->sel_bg : th->bg;
            tk_fill(iy, 1, ch_w - 1, bg);
            char label[160];
            if (rows[r].header) {
                /* count of this group's unread, so a collapsed group still speaks up */
                int gu = 0;
                for (size_t i = 0; i < m->n_channels; i++)
                    if (chan_group(&m->channels[i]) == rows[r].group) gu += m->channels[i].unread;
                const char *tri = g_grp_collapsed[rows[r].group] ? "\xe2\x96\xb8" : "\xe2\x96\xbe"; /* ▸ / ▾ */
                if (gu) snprintf(label, sizeof label, "%s %s (%d)", tri, SBG_LABEL[rows[r].group], gu);
                else    snprintf(label, sizeof label, "%s %s", tri, SBG_LABEL[rows[r].group]);
                uintattr_t hfg = sel ? (th->sel_fg | TB_BOLD) : (th->accent | TB_BOLD);
                tk_text(1, iy, ch_w - 1, label, hfg, bg);
            } else {
                const oc_channel *c = &m->channels[rows[r].idx];
                uintattr_t fg = sel ? (th->sel_fg | TB_BOLD) : c->joined ? th->fg : th->muted;
                char title[96];
                sidebar_label(m, c, title, sizeof title);
                if (c->unread > 0) snprintf(label, sizeof label, "  %s (%d)", title, c->unread);
                else               snprintf(label, sizeof label, "  %s", title);
                tk_text(1, iy, ch_w - 1, label, fg, bg);
            }
        }
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
    } else if (fc && !fc->is_public)
        snprintf(mt, sizeof mt, "\xf0\x9f\x94\x92 %s", fc->name ? fc->name : "…");  /* 🔒 private */
    else if (fc)                 snprintf(mt, sizeof mt, "#%s", fc->name ? fc->name : "…");
    else                         snprintf(mt, sizeof mt, "no channel");
    tk_panel(msg_x, panels_top, msg_w, panels_h, mt, msg_act);
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
                uintattr_t bg = selrow ? th->sel_bg : th->bg;
                if (selrow) tk_fill(y, ix, ix + iw, bg);
                tk_text(ix, y, ix + iw, rows.v[i].s, selrow ? (th->sel_fg | TB_BOLD) : rows.v[i].fg, bg);
            }
            rows_free(&rows);
        }
    }

    /* Members panel (roster + presence), on wide terminals. */
    if (mem_w) {
        int mx = msg_x + msg_w;
        tk_panel(mx, panels_top, mem_w, panels_h, "Members", mem_act);
        for (size_t i = 0, iy = panels_top + 1; i < m->n_users && (int)iy < panels_top + panels_h - 1; i++, iy++) {
            const oc_member *u = &m->users[i];
            uint8_t p = oc_model_presence_of(m, u->user_id);
            const char *d = p == OC_PRESENCE_ONLINE ? "\xe2\x97\x8f" : p == OC_PRESENCE_AWAY ? "\xe2\x97\x90" : "\xe2\x97\x8b";
            int sel = (mem_act && (int)i == mem_sel);
            uintattr_t bg = sel ? th->sel_bg : th->bg;
            uintattr_t col = sel ? (th->sel_fg | TB_BOLD)
                           : u->disabled ? th->muted
                           : p == OC_PRESENCE_ONLINE ? th->presence_on : p == OC_PRESENCE_AWAY ? th->presence_away : th->fg;
            const char *role = u->role == OC_ROLE_OWNER ? " *" : u->role == OC_ROLE_ADMIN ? " +" : "";
            char line[80]; snprintf(line, sizeof line, "%s %s%s", d, u->name[0] ? u->name : "?", role);
            if (sel) tk_fill((int)iy, mx + 1, mx + mem_w - 1, bg);
            tk_text(mx + 1, (int)iy, mx + mem_w - 1, line, col, bg);
        }
    }

    /* Row H-3: the autocomplete suggestion strip when completing, else the status
     * line (last status/error + typing indicator). */
    tk_fill(H - 3, 0, W, TB_DEFAULT);
    ac_cand cands[16]; int rs, nc = 0;
    if (clen > 0) nc = ac_candidates(m, composer, cands, 16, &rs);
    if (nc > 0) {
        int active = ((ac_idx % nc) + nc) % nc;
        int cx = tk_text(0, H - 3, W, " \xe2\x96\xb8 ", TB_CYAN | TB_BOLD, TB_DEFAULT);   /* ▸ */
        for (int i = 0; i < nc && cx < W - 2; i++) {
            char item[110]; snprintf(item, sizeof item, "%s", cands[i].disp);
            uintattr_t fg = (i == active) ? TB_BLACK : TB_WHITE | TB_BOLD;
            uintattr_t bg = (i == active) ? TB_CYAN : TB_DEFAULT;
            if (i == active) tb_set_cell(cx - 1, H - 3, ' ', fg, bg);
            cx = tk_text(cx, H - 3, W, item, fg, bg);
            if (i == active && cx < W) tb_set_cell(cx, H - 3, ' ', fg, bg);
            cx = tk_text(cx, H - 3, W, "  ", TB_DEFAULT, TB_DEFAULT);
        }
        tk_text(cx, H - 3, W, nc > 1 ? "(Tab cycles)" : "(Tab)", TB_BLACK | TB_BOLD, TB_DEFAULT);
    } else {
        char st[220]; snprintf(st, sizeof st, " %s%s", m->status[0] ? m->status : "", scroll > 0 ? "   [scrolled]" : "");
        int sx = tk_text(0, H - 3, W, st, th->muted, TB_DEFAULT);
        if (fc) {
            uint64_t tp[8]; size_t nt = oc_model_typing(m, fc->channel_id, m->user_id, tp, 8);
            if (nt) {
                char tl[140];
                if (nt == 1) snprintf(tl, sizeof tl, "  ✎ %s is typing…", name_for(fc, tp[0]));
                else         snprintf(tl, sizeof tl, "  ✎ %zu people are typing…", nt);
                tk_text(sx, H - 3, W, tl, th->accent, TB_DEFAULT);
            }
        }
    }

    /* Composer (row H-2). Editing an existing message shows a distinct prompt. */
    tk_fill(H - 2, 0, W, TB_DEFAULT);
    int cx = editing ? tk_text(0, H - 2, W, "\xe2\x9c\x8e edit \xe2\x80\xba ", TB_YELLOW | TB_BOLD, TB_DEFAULT)
                     : tk_text(0, H - 2, W, "\xe2\x80\xba ", TB_GREEN | TB_BOLD, TB_DEFAULT);   /* › */
    cx = tk_text(cx, H - 2, W, composer, TB_DEFAULT, TB_DEFAULT);
    if (cx < W) tb_set_cell(cx, H - 2, ' ', TB_DEFAULT, TB_REVERSE);   /* cursor */

    /* Context keybinding hint bar (row H-1), per focused panel — generated from
     * tuikit tk_binding tables (tk_help_footer) so the hints can't drift from the
     * keys. Phase 4 wires input matching to these same tables. */
    static const tk_binding KB_COMPOSER[] = {
        { TB_KEY_ENTER, 0, "Enter", "send", 1 }, { TB_KEY_ESC, 0, "Esc", "navigate", 1 },
        { TB_KEY_CTRL_F, 0, "Ctrl+F", "search", 1 }, { TB_KEY_CTRL_K, 0, "Ctrl+K", "actions", 1 },
        { 0, '?', "?", "help", 1 }, { TB_KEY_CTRL_Q, 0, "Ctrl+Q", "quit", 1 },
    };
    static const tk_binding KB_CHANNELS[] = {
        { TB_KEY_ARROW_DOWN, 0, "↑↓", "select", 1 }, { TB_KEY_ENTER, 0, "Enter", "open", 1 },
        { 0, 'n', "n", "new", 1 }, { TB_KEY_TAB, 0, "Tab", "panel", 1 },
        { TB_KEY_ESC, 0, "Esc", "composer", 1 },
    };
    static const tk_binding KB_MESSAGES[] = {
        { TB_KEY_ARROW_DOWN, 0, "↑↓", "select", 1 }, { TB_KEY_ENTER, 0, "Enter", "actions", 1 },
        { 0, 't', "t", "thread", 1 }, { 0, 'r', "r", "react", 1 }, { 0, 'e', "e", "edit", 1 },
        { TB_KEY_TAB, 0, "Tab", "panel", 1 }, { TB_KEY_ESC, 0, "Esc", "composer", 1 },
    };
    static const tk_binding KB_MEMBERS[] = {
        { TB_KEY_ARROW_DOWN, 0, "↑↓", "select", 1 }, { TB_KEY_ENTER, 0, "Enter", "actions", 1 },
        { 0, 'n', "n", "new DM", 1 }, { TB_KEY_TAB, 0, "Tab", "panel", 1 },
        { TB_KEY_ESC, 0, "Esc", "composer", 1 },
    };
    tk_fill(H - 1, 0, W, th->footer_bg);
    if (help_open) {
        tk_text(0, H - 1, W, " press ? or Esc to close help ", th->fg, th->footer_bg);
    } else {
        const char *label; const tk_binding *kb; int kn;
        if      (panel == 1) { label = " Channels"; kb = KB_CHANNELS; kn = (int)(sizeof KB_CHANNELS / sizeof *KB_CHANNELS); }
        else if (panel == 2) { label = " Messages"; kb = KB_MESSAGES; kn = (int)(sizeof KB_MESSAGES / sizeof *KB_MESSAGES); }
        else if (panel == 3) { label = " Members";  kb = KB_MEMBERS;  kn = (int)(sizeof KB_MEMBERS  / sizeof *KB_MEMBERS);  }
        else                 { label = "";          kb = KB_COMPOSER; kn = (int)(sizeof KB_COMPOSER / sizeof *KB_COMPOSER); }
        int hx = label[0] ? tk_text(0, H - 1, W, label, th->accent | TB_BOLD, th->footer_bg) : 0;
        tk_help_footer(H - 1, hx, W, kb, kn, th->muted, th->footer_bg);
    }

    if (help_open) draw_help(W, H);
}


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



/* The /storage overlay (REQ-214): usage, the active policy, and what
 * maintenance has reclaimed. Owner/admin only — a member's request is refused
 * server-side, which surfaces as an error line rather than an empty panel. */
static void draw_storage(const oc_model *m, int W, int H) {
    const tk_theme *th = tk_theme_active();
    if (!m->storage_have) {
        const char *e = "No report yet — owner/admin only.";
        tk_rect r = modal_frame(W, H, tk_str_width(e), 1, "Storage  ·  Esc closes");
        tk_text(r.x, r.y, r.x + r.w, e, th->muted, th->bg);
        tb_present();
        return;
    }
    const oc_storage_view *s = &m->storage;
    const uint64_t MBv = 1024 * 1024;

    struct { char t[128]; uintattr_t c; } L[16];
    int n = 0;
#define SROW(col, ...) do { snprintf(L[n].t, sizeof L[n].t, __VA_ARGS__); L[n].c = (col); n++; } while (0)
    SROW(s->under_pressure ? (TB_RED | TB_BOLD) : th->fg, "Disk         %llu MB free of %llu MB",
         (unsigned long long)(s->avail_bytes / MBv), (unsigned long long)(s->total_bytes / MBv));
    SROW(th->fg, "Attachments  %llu file(s), %llu MB",
         (unsigned long long)s->attach_count, (unsigned long long)(s->attach_bytes / MBv));
    if (s->under_pressure) SROW(TB_RED | TB_BOLD, "under pressure — reclaiming storage");
    SROW(th->bg, "%s", "");
    SROW(th->accent2 | TB_BOLD, "%s", "Policy");
    if (s->max_age_days) SROW(th->fg, "  attachments expire after %llu day(s)", (unsigned long long)s->max_age_days);
    else                 SROW(th->fg, "%s", "  attachments kept indefinitely");
    SROW(th->fg, "  eviction under pressure: %s", s->evict_enabled ? "on (oldest first)" : "off");
    SROW(th->fg, "  database reserve: %llu MB", (unsigned long long)(s->reserve_bytes / MBv));
    SROW(th->bg, "%s", "");
    SROW(th->accent2 | TB_BOLD, "%s", "Reclaimed so far");
    /* Evictions are the destructive reclaims; flag them in red once any happen. */
    SROW(s->rec_evicted ? (TB_RED | TB_BOLD) : th->fg, "  %llu abandoned   %llu expired   %llu evicted",
         (unsigned long long)s->rec_orphan, (unsigned long long)s->rec_expired, (unsigned long long)s->rec_evicted);
#undef SROW

    int cw = 0;
    for (int i = 0; i < n; i++) { int w = tk_str_width(L[i].t); if (w > cw) cw = w; }
    tk_rect r = modal_frame(W, H, cw, n, "Storage  ·  Esc closes");
    for (int i = 0; i < n && i < r.h; i++)
        tk_text(r.x, r.y + i, r.x + r.w, L[i].t, L[i].c, th->bg);
    tb_present();
}


/* The /audit overlay (REQ-251): administrative and security-relevant actions,
 * newest first. Owner/admin only — a member's request is refused server-side.
 * Denied and failed entries are drawn in red, since those are what an operator
 * is usually scanning for. */
static void draw_audit(const oc_model *m, int W, int H) {
    const tk_theme *th = tk_theme_active();
    if (!m->n_audit) {
        const char *e = m->audit_open ? "No entries, or you are not an admin." : "";
        tk_rect r = modal_frame(W, H, tk_str_width("No entries, or you are not an admin."),
                                1, "Audit log  ·  Esc closes");
        tk_text(r.x, r.y, r.x + r.w, e, th->muted, th->bg);
        tb_present();
        return;
    }
    static const char *FAM[] = { "?", "admin", "account", "security", "moder" };
    int want = (int)m->n_audit + 1;                       /* + a header row */
    tk_rect r = modal_frame(W, H, W, want > 22 ? 22 : want,
                            "Audit log  ·  newest first  ·  Esc closes");

    char hdr[256];
    snprintf(hdr, sizeof hdr, "%-12s %-8s %-22s %-14s %-16s %s",
             "when", "family", "action", "actor", "target", "detail");
    tk_text(r.x, r.y, r.x + r.w, hdr, th->accent2 | TB_BOLD, th->bg);

    int rows = r.h - 1;
    for (int i = 0; i < rows && (size_t)i < m->n_audit; i++) {
        const oc_audit_view *a = &m->audit[i];
        char when[32];
        time_t t = (time_t)(a->at_ms / 1000);
        struct tm tmv;
        oc_localtime_r(&t, &tmv);
        strftime(when, sizeof when, g_cfg && g_cfg->time_24h ? "%m-%d %H:%M" : "%m-%d %I:%M%p", &tmv);

        char line[256];
        const char *fam = (a->family <= 4) ? FAM[a->family] : "?";
        snprintf(line, sizeof line, "%-12s %-8s %-22.22s %-14.14s %-16.16s %s",
                 when, fam, a->action,
                 a->actor_name[0] ? a->actor_name : "-",
                 a->target[0] ? a->target : "-",
                 a->detail);
        /* A denial or failure is what an operator scans for — make it findable. */
        tk_text(r.x, r.y + 1 + i, r.x + r.w, line,
                a->outcome ? th->fg : (TB_RED | TB_BOLD), th->bg);
    }
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
    const tk_theme *th = tk_theme_active();
    for (int j = 0; j < bh; j++) tk_fill(y + j, x, x + bw, th->bg);
    tk_box(x, y, bw, bh);
    tk_text(x + 2, y, x + bw - 1, " Workspaces ", th->accent | TB_BOLD, th->bg);
    tk_text_right(y, x + 2, x + bw - 2, "esc", th->faint, th->bg);

    int top = (sel >= show) ? sel - show + 1 : 0;
    for (int i = 0; i < show && top + i < rows; i++) {
        int idx = top + i, is_sel = (idx == sel);
        uintattr_t bg = is_sel ? th->sel_bg : th->bg;
        uintattr_t fg = is_sel ? (th->sel_fg | TB_BOLD) : th->fg;
        if (is_sel) tk_fill(y + 1 + i, x + 1, x + bw - 1, bg);

        char line[320];
        if (idx == g_nsw) {                      /* the always-present last row */
            snprintf(line, sizeof line, "+ Log in to new workspace");
            tk_text(x + 2, y + 1 + i, x + bw - 1, line,
                      is_sel ? fg : (th->ok | TB_BOLD), bg);
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
        tk_text(x + 2, y + 1 + i, x + bw - 1, line,
                  is_sel ? fg : (e->session >= 0 ? th->fg : th->muted), bg);
    }
    tk_text(x + 2, y + bh - 1, x + bw - 1,
              " Ctrl+W switch \xc2\xb7 d forget \xc2\xb7 Esc close ", th->muted, th->bg);
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
    const tk_theme *th = tk_theme_active();
    for (int j = 0; j < bh; j++) tk_fill(y + j, x, x + bw, th->bg);
    tk_box(x, y, bw, bh);
    tk_text(x + 2, y, x + bw - 1, " React ", th->accent | TB_BOLD, th->bg);
    tk_text_right(y, x + 2, x + bw - 2, "esc", th->faint, th->bg);
    tk_text(x + 2, y + 1, x + bw - 1, q, th->fg, th->bg);
    if (!q[0]) tk_text(x + 2, y + 1, x + bw - 2, "Search", th->muted, th->bg);
    else { int cxq = x + 2 + (int)strlen(q); if (cxq < x + bw - 1) tb_set_cell(cxq, y + 1, ' ', TB_DEFAULT, TB_REVERSE); }
    int top = (esel >= show) ? esel - show + 1 : 0;
    for (int i = 0; i < show && top + i < n; i++) {
        int idx = top + i, sel = (idx == esel);
        uintattr_t bg = sel ? th->sel_bg : th->bg, fg = sel ? (th->sel_fg | TB_BOLD) : th->fg;
        char line[64]; snprintf(line, sizeof line, "%s  :%s:", em[idx], nm[idx]);
        if (sel) tk_fill(y + 2 + i, x + 1, x + bw - 1, bg);
        tk_text(x + 2, y + 2 + i, x + bw - 1, line, fg, bg);
    }
    if (n == 0) tk_text(x + 2, y + 2, x + bw - 1, "(no matches)", th->muted, th->bg);
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
    snprintf(dir, sizeof dir, "%s/.local", home);            oc_mkdir(dir);
    snprintf(dir, sizeof dir, "%s/.local/state", home);      oc_mkdir(dir);
    snprintf(dir, sizeof dir, "%s/.local/state/openchime", home); oc_mkdir(dir);
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


/* A labeled input row; `focused` highlights the field, `mask` renders dots. */
static void draw_field(int x, int y, int w, const char *label, const char *val,
                       int focused, int mask) {
    tk_text(x, y, x + 11, label, TB_DEFAULT, TB_DEFAULT);
    int fx = x + 11, fw = w - 11;
    uintattr_t bg = focused ? TB_BLUE : (TB_BLACK | TB_BOLD);
    tk_fill(y, fx, fx + fw, bg);
    if (mask) {
        char dots[130]; size_t n = strlen(val);
        if (n > sizeof dots - 1) n = sizeof dots - 1;
        memset(dots, '*', n); dots[n] = '\0';
        tk_text(fx + 1, y, fx + fw, dots, TB_WHITE, bg);
    } else {
        tk_text(fx + 1, y, fx + fw, val, TB_WHITE | (focused ? TB_BOLD : 0), bg);
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
        tk_box(x, y, bw, bh);
        tk_text(x + 2, y, x + bw - 1, " Sign in to OpenChime ", TB_CYAN | TB_BOLD, TB_DEFAULT);
        int ix = x + 2, iw = bw - 4;
        draw_field(ix, y + 2, iw, "Workspace", f->workspace, focus == 0, 0);
        draw_field(ix, y + 4, iw, "Username", f->user, focus == 1, 0);
        draw_field(ix, y + 5, iw, "Password", f->pass, focus == 2, 1);
        char rem[40]; snprintf(rem, sizeof rem, "[%c] Remember me", f->remember ? 'x' : ' ');
        tk_text(ix, y + 7, ix + iw, rem, focus == 3 ? TB_CYAN | TB_BOLD : TB_DEFAULT, TB_DEFAULT);
        const char *e = inl[0] ? inl : err;
        if (e) tk_text(ix, y + 9, ix + iw, e, TB_RED | TB_BOLD, TB_DEFAULT);
        tk_text(ix, y + bh - 1, ix + iw, " Enter connect · Tab next · Esc quit ",
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
            oc_resolve_status st = oc_resolve(f->workspace, oc_default_suffix(), &f->ep);
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
        tk_text((W - (int)strlen(msg)) / 2, H / 2, W, msg, TB_WHITE | TB_BOLD, TB_DEFAULT);
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
        oc_resolve_status st = oc_resolve(inst, oc_default_suffix(), &ep);
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
    tb_set_output_mode(TB_OUTPUT_256);   /* 256-color theme (tuikit tk_theme) */

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
    int switcher_open = 0, wsel = 0;          /* workspace switcher (Ctrl+W) */
    int storage_open = 0;                     /* /storage overlay (REQ-214) */
    int audit_open = 0;                       /* /audit overlay (REQ-251) */
    int help_open = 0;
    int profile_open = 0;                     /* /profile identity modal */
    int ac_idx = 0;                           /* autocomplete cycle index */
    int panel = 0;                            /* 0 composer · 1 channels · 2 messages · 3 members */
    int msg_sel = -1, mem_sel = 0;            /* selection within the messages / members panel */
    uint64_t editing = 0;                     /* message id being edited (0 = none) */
    int picker_open = 0, esel = 0;            /* emoji picker (r on a message) */
    char eq[32] = ""; size_t eqlen = 0;       /* picker query */
    uint64_t picker_cid = 0, picker_mid = 0;  /* message being reacted to */
    uint64_t last_focus_cid = 0;
    time_t last_typing = 0;                   /* throttle outbound TYPING signals */
    int running = 1, logging_out = 0;

    /* Action menu (tuikit, menu-driven) — messages, members, and the global
     * launcher (Ctrl-K). Filterable so the launcher is a fuzzy action finder. */
    int action_open = 0; uint64_t act_cid = 0, act_mid = 0, act_uid = 0;
    const char *act_title = "Actions";
    tk_list action_menu;
    tk_list_opts action_opts = { NULL, menu_count, menu_text, NULL, 1 };
    tk_list_init(&action_menu, &action_opts);

    /* Prompt dialog (tuikit tk_input in a modal) — replaces /create /search /dm
     * /nick with a discoverable text prompt. */
    enum { PROMPT_NONE = 0, PROMPT_NEWCHAN, PROMPT_SEARCH, PROMPT_DM, PROMPT_NICK, PROMPT_UPLOAD,
           PROMPT_DND, PROMPT_WEBHOOK, PROMPT_PASSWD_OLD, PROMPT_PASSWD_NEW };
    int prompt_kind = PROMPT_NONE; const char *prompt_title = "";
    char pw_old[128] = "";                 /* stashed between the two password prompts */
    tk_input prompt_input; tk_input_init(&prompt_input, 0, "");

    /* Global action launcher (tuikit tk_palette, Ctrl-K / ':'). */
    int launcher_open = 0;
    tk_palette launcher; tk_palette_init(&launcher, "Commands");

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

        /* One present per frame: render() and the overlays all draw into the back
         * buffer, then a single tb_present() below diffs it to the screen. (Two
         * presents per frame — base then overlay — flicker the overlay.) */
        render(cl, focus, composer, clen, scroll, help_open, ac_idx, panel, msg_sel, mem_sel, editing);
        if (action_open) {
            int mh = g_nmenu + 3; if (mh > tb_height() - 2) mh = tb_height() - 2;
            tk_rect in = tk_modal_begin(tb_width(), tb_height(), 34, mh, act_title);
            tk_list_draw(&action_menu, in);
        }
        if (prompt_kind) {
            tk_rect in = tk_modal_begin(tb_width(), tb_height(), 46, 3, prompt_title);
            tk_input_draw(&prompt_input, (tk_rect){ in.x, in.y, in.w, 1 }, 1);
        }
        if (launcher_open) tk_palette_draw(&launcher, tb_width(), tb_height());
        if (picker_open) draw_picker(eq, esel);
        if (profile_open) draw_profile(oc_client_model(cl), tb_width(), tb_height());
        if (switcher_open) draw_switcher(wsel);
        if (storage_open) draw_storage(oc_client_model(cl), tb_width(), tb_height());
        if (audit_open) draw_audit(oc_client_model(cl), tb_width(), tb_height());
        tb_present();

        struct tb_event ev;
        int rc = tb_peek_event(&ev, 30);
        if (rc != TB_OK) continue;               /* timeout: loop to re-tick + redraw */
        if (ev.type == TB_EVENT_RESIZE) continue;
        if (ev.type == TB_EVENT_MOUSE) {       /* wheel scrolls; click focuses a channel/member */
            if (ev.key == TB_KEY_MOUSE_WHEEL_UP) scroll += 3;
            else if (ev.key == TB_KEY_MOUSE_WHEEL_DOWN) { scroll -= 3; if (scroll < 0) scroll = 0; }
            else if (ev.key == TB_KEY_MOUSE_LEFT && !help_open && !action_open && !prompt_kind && !launcher_open && !picker_open) {
                int Wm = tb_width(); int chw, memw, msgx, msgw;
                layout(Wm, &chw, &memw, &msgx, &msgw);
                int prow = ev.y - 2;           /* first panel content row */
                const oc_model *mm = oc_client_model(cl);
                if (prow >= 0 && ev.x < chw) {                      /* Channels */
                    sb_row rows[256];
                    int nrows = sidebar_build(mm, rows, 256);
                    if (prow < nrows) {
                        if (rows[prow].header) {   /* click a header → fold/unfold */
                            g_grp_collapsed[rows[prow].group] = !g_grp_collapsed[rows[prow].group];
                            g_sb_hdr = rows[prow].group;
                        } else { g_sb_hdr = -1; focus = rows[prow].idx; panel = 0; }
                    }
                } else if (memw && prow >= 0 && ev.x >= msgx + msgw) {   /* Members */
                    if ((size_t)prow < mm->n_users) oc_client_open_dm(cl, mm->users[prow].user_id);
                }
            }
            continue;
        }
        if (ev.type != TB_EVENT_KEY) continue;

        /* Global quit — works from any context (was re-implemented per-overlay). */
        if (ev.key == TB_KEY_CTRL_Q || ev.key == TB_KEY_CTRL_C) { running = 0; continue; }

        /* Esc closes an open read-only overlay (thread/search/reactions/prefs/
         * webhooks/roster). Without this the old /close is gone and you get stuck. */
        {
            const oc_model *om = oc_client_model(cl);
            if (ev.key == TB_KEY_ESC && !switcher_open && !action_open && !launcher_open &&
                !prompt_kind && !picker_open && !help_open && !profile_open &&
                (om->thread_open || om->search_open || om->reactlist_open ||
                 om->prefs_open || om->weblist_open || om->roster_open)) {
                oc_client_close_thread(cl);  oc_client_close_search(cl);
                oc_client_close_reactions(cl); oc_client_toggle_prefs(cl, 0);
                oc_client_close_webhooks(cl); oc_client_toggle_roster(cl, 0);
                continue;
            }
        }

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
        if (audit_open) {                      /* audit overlay: any key closes */
            if (ev.key == TB_KEY_CTRL_Q || ev.key == TB_KEY_CTRL_C) running = 0;
            else audit_open = 0;
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
        if (prompt_kind) {                     /* text prompt dialog (tuikit) */
            tk_result pr = tk_input_handle(&prompt_input, &ev);
            if (pr == TK_CANCEL) prompt_kind = PROMPT_NONE;
            else if (pr == TK_SELECT) {
                const char *v = tk_input_value(&prompt_input);
                int k = prompt_kind;
                prompt_kind = PROMPT_NONE;
                if      (k == PROMPT_NEWCHAN && *v) oc_client_create_channel(cl, v);
                else if (k == PROMPT_SEARCH  && *v) oc_client_search(cl, v);
                else if (k == PROMPT_NICK    && *v) oc_client_set_display_name(cl, v);
                else if (k == PROMPT_WEBHOOK && *v) oc_client_create_webhook(cl, act_cid, v);
                else if (k == PROMPT_UPLOAD && *v) {
                    const oc_model *pm = oc_client_model(cl);
                    if (focus < pm->n_channels) oc_client_upload(cl, pm->channels[focus].channel_id, v);
                }
                else if (k == PROMPT_DM && *v) {
                    uint64_t u = oc_model_user_id(oc_client_model(cl), v);
                    if (u) oc_client_open_dm(cl, u);
                }
                else if (k == PROMPT_DND) {
                    if (strcmp(v, "off") == 0 || !*v) oc_client_set_dnd(cl, 0, 0, 0);
                    else {
                        char s1[16] = "", s2[16] = "";
                        int a = parse_hhmm((sscanf(v, "%15s %15s", s1, s2) == 2) ? s1 : "");
                        int b = parse_hhmm(s2);
                        if (a >= 0 && b >= 0) oc_client_set_dnd(cl, 1, (uint16_t)a, (uint16_t)b);
                    }
                }
                else if (k == PROMPT_PASSWD_OLD) {     /* step 1: stash, ask for the new one */
                    snprintf(pw_old, sizeof pw_old, "%s", v);
                    prompt_kind = PROMPT_PASSWD_NEW; prompt_title = "New password";
                    tk_input_init(&prompt_input, 1, "");
                }
                else if (k == PROMPT_PASSWD_NEW) {     /* step 2: change it */
                    if (*v && pw_old[0]) oc_client_change_password(cl, pw_old, v);
                    pw_old[0] = '\0';
                }
            }
            continue;
        }
        if (launcher_open) {                   /* global action launcher (tk_palette) */
            tk_result lr = tk_palette_handle(&launcher, &ev);
            if (lr == TK_CANCEL) launcher_open = 0;
            else if (lr == TK_SELECT) {
                int id = tk_palette_selected_id(&launcher);
                launcher_open = 0;
                if      (id == ACT_NEWCHAN) { prompt_kind = PROMPT_NEWCHAN; prompt_title = "New channel"; tk_input_init(&prompt_input, 0, "name"); }
                else if (id == ACT_NEWDM)   { prompt_kind = PROMPT_DM; prompt_title = "New direct message"; tk_input_init(&prompt_input, 0, "username"); }
                else if (id == ACT_SEARCH)  { prompt_kind = PROMPT_SEARCH; prompt_title = "Search messages"; tk_input_init(&prompt_input, 0, "query"); }
                else if (id == ACT_NICK)    { prompt_kind = PROMPT_NICK; prompt_title = "Change display name"; tk_input_init(&prompt_input, 0, "new name"); }
                else if (id == ACT_UPLOAD)  { prompt_kind = PROMPT_UPLOAD; prompt_title = "Upload a file"; tk_input_init(&prompt_input, 0, "path"); }
                else if (id == ACT_DND)     { prompt_kind = PROMPT_DND; prompt_title = "Do not disturb (HH:MM HH:MM, or 'off')"; tk_input_init(&prompt_input, 0, "e.g. 22:00 08:00"); }
                else if (id == ACT_PASSWD)  { prompt_kind = PROMPT_PASSWD_OLD; prompt_title = "Current password"; tk_input_init(&prompt_input, 1, ""); pw_old[0] = '\0'; }
                else if (id == ACT_AWAY)    oc_client_set_presence(cl, OC_PRESENCE_AWAY);
                else if (id == ACT_ONLINE)  oc_client_set_presence(cl, OC_PRESENCE_ONLINE);
                else if (id == ACT_PREFS)   oc_client_toggle_prefs(cl, 1);
                else if (id == ACT_LEAVE)   { if (act_cid) oc_client_leave_channel(cl, act_cid); }
                else if (id == ACT_WEBHOOKS){ if (act_cid) oc_client_webhooks(cl, act_cid); }
                else if (id == ACT_INVITE)  oc_client_invite_user(cl, OC_ROLE_MEMBER);
                else if (id == ACT_PROFILE) profile_open = 1;
                else if (id == ACT_STORAGE) { oc_client_storage_status(cl); storage_open = 1; }
                else if (id == ACT_AUDIT)   { oc_client_audit_query(cl, 0); audit_open = 1; }
                else if (id == ACT_WORKSPACES) { sw_build(); wsel = (g_active < g_nsw) ? g_active : 0; switcher_open = 1; }
                else if (id == ACT_HELP)    help_open = 1;
                else if (id == ACT_LOGOUT)  { oc_client_logout(cl, OC_LOGOUT_THIS); logging_out = 1; }
            }
            continue;
        }
        if (action_open) {                     /* message action menu (tuikit) */
            tk_result ar = tk_list_handle(&action_menu, &ev);
            if (ar == TK_CANCEL) action_open = 0;
            else if (ar == TK_SELECT) {
                int ai = tk_list_selected(&action_menu);
                action_open = 0;
                if (ai >= 0) {
                    int id = g_menu[ai].id;
                    if      (id == ACT_THREAD)      oc_client_open_thread(cl, act_cid, act_mid);
                    else if (id == ACT_REACTORS)    oc_client_list_reactions(cl, act_cid, act_mid);
                    else if (id == ACT_DELETE)      oc_client_delete(cl, act_cid, act_mid);
                    else if (id == ACT_DM)          { oc_client_open_dm(cl, act_uid); panel = 0; }
                    else if (id == ACT_ROLE_ADMIN)  oc_client_set_role(cl, act_uid, OC_ROLE_ADMIN);
                    else if (id == ACT_ROLE_MEMBER) oc_client_set_role(cl, act_uid, OC_ROLE_MEMBER);
                    else if (id == ACT_REMOVE)      oc_client_remove_user(cl, act_uid);
                    else if (id == ACT_REACT)       { picker_open = 1; eq[0] = '\0'; eqlen = 0; esel = 0; picker_cid = act_cid; picker_mid = act_mid; }
                    else if (id == ACT_EDIT) {
                        const oc_channel *ec = focused_channel(oc_client_model(cl), act_cid);
                        if (ec) for (size_t k = 0; k < ec->n_msgs; k++)
                            if (ec->msgs[k].message_id == act_mid) {
                                editing = act_mid;
                                snprintf(composer, sizeof composer, "%s", ec->msgs[k].body ? ec->msgs[k].body : "");
                                clen = strlen(composer); panel = 0; break;
                            }
                    }
                    else if (id == ACT_DOWNLOAD) {   /* first live attachment -> its filename in cwd */
                        const oc_channel *ec = focused_channel(oc_client_model(cl), act_cid);
                        if (ec) for (size_t k = 0; k < ec->n_msgs; k++)
                            if (ec->msgs[k].message_id == act_mid) {
                                for (uint8_t ak = 0; ak < ec->msgs[k].n_attach; ak++)
                                    if (!ec->msgs[k].attach[ak].reclaimed) {
                                        oc_client_download(cl, ec->msgs[k].attach[ak].id, ec->msgs[k].attach[ak].filename);
                                        break;
                                    }
                                break;
                            }
                    }
                    /* channel menu actions */
                    else if (id == ACT_OPEN)  panel = 0;
                    else if (id == ACT_JOIN)  { oc_client_join_channel(cl, act_cid); panel = 0; }
                    else if (id == ACT_WEBHOOK_CREATE) { prompt_kind = PROMPT_WEBHOOK; prompt_title = "Create webhook (label)"; tk_input_init(&prompt_input, 0, "label"); }
                    else if (id == ACT_NOTIFY_ALL)      oc_client_set_notify_pref(cl, act_cid, OC_NOTIFY_ALL);
                    else if (id == ACT_NOTIFY_MENTIONS) oc_client_set_notify_pref(cl, act_cid, OC_NOTIFY_MENTIONS);
                    else if (id == ACT_NOTIFY_NONE)     oc_client_set_notify_pref(cl, act_cid, OC_NOTIFY_NONE);
                    /* global launcher actions */
                    else if (id == ACT_NEWCHAN) { prompt_kind = PROMPT_NEWCHAN; prompt_title = "New channel"; tk_input_init(&prompt_input, 0, "name"); }
                    else if (id == ACT_NEWDM)   { prompt_kind = PROMPT_DM; prompt_title = "New direct message"; tk_input_init(&prompt_input, 0, "username"); }
                    else if (id == ACT_SEARCH)  { prompt_kind = PROMPT_SEARCH; prompt_title = "Search messages"; tk_input_init(&prompt_input, 0, "query"); }
                    else if (id == ACT_NICK)    { prompt_kind = PROMPT_NICK; prompt_title = "Change display name"; tk_input_init(&prompt_input, 0, "new name"); }
                    else if (id == ACT_AWAY)    oc_client_set_presence(cl, OC_PRESENCE_AWAY);
                    else if (id == ACT_ONLINE)  oc_client_set_presence(cl, OC_PRESENCE_ONLINE);
                    else if (id == ACT_PREFS)   { oc_client_toggle_prefs(cl, 1); }
                    else if (id == ACT_LEAVE)   { if (act_cid) oc_client_leave_channel(cl, act_cid); }
                    else if (id == ACT_WEBHOOKS){ if (act_cid) oc_client_webhooks(cl, act_cid); }
                    else if (id == ACT_INVITE)  oc_client_invite_user(cl, OC_ROLE_MEMBER);
                    else if (id == ACT_PROFILE) profile_open = 1;
                    else if (id == ACT_STORAGE) { oc_client_storage_status(cl); storage_open = 1; }
                    else if (id == ACT_AUDIT)   { oc_client_audit_query(cl, 0); audit_open = 1; }
                    else if (id == ACT_WORKSPACES) { sw_build(); wsel = (g_active < g_nsw) ? g_active : 0; switcher_open = 1; }
                    else if (id == ACT_HELP)    help_open = 1;
                    else if (id == ACT_LOGOUT)  { oc_client_logout(cl, OC_LOGOUT_THIS); logging_out = 1; }
                }
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
        if (ev.key == TB_KEY_CTRL_K) {         /* global action launcher (tuikit tk_palette) */
            const oc_model *mm = oc_client_model(cl);
            act_cid = focus < mm->n_channels ? mm->channels[focus].channel_id : 0;
            tk_palette_set(&launcher, g_launcher_items, (int)(sizeof g_launcher_items / sizeof *g_launcher_items));
            launcher_open = 1;
            continue;
        }
        if (ev.key == TB_KEY_CTRL_F) {         /* search dialog (tuikit) — replaces /search */
            prompt_kind = PROMPT_SEARCH; prompt_title = "Search messages";
            tk_input_init(&prompt_input, 0, "query");
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
            int up   = (ev.key == TB_KEY_ARROW_UP);
            int down = (ev.key == TB_KEY_ARROW_DOWN);
            if (ev.key == TB_KEY_CTRL_Q || ev.key == TB_KEY_CTRL_C) { running = 0; }
            else if (ev.key == TB_KEY_ESC) { panel = 0; }   /* back to composing */
            else if (ev.key == TB_KEY_TAB) {                                /* cycle panels 1→2→3 */
                panel = (panel == 1) ? 2 : (panel == 2) ? 3 : 1;
                if (panel == 2 && ch && (msg_sel < 0 || msg_sel >= (int)ch->n_msgs))
                    msg_sel = ch->n_msgs ? (int)ch->n_msgs - 1 : -1;
                if (panel == 3 && mem_sel >= (int)mm->n_users)
                    mem_sel = mm->n_users ? (int)mm->n_users - 1 : 0;
            }
            else if (panel == 1) {                                          /* channels */
                sb_row rows[256];
                int nrows = sidebar_build(mm, rows, 256);
                int cur = sidebar_cursor(rows, nrows, focus);
                if (up && cur > 0) sidebar_select(rows, cur - 1, &focus);
                else if (down && cur + 1 < nrows) sidebar_select(rows, cur + 1, &focus);
                else if (ev.key == TB_KEY_ARROW_LEFT) {                      /* collapse / go to header */
                    int g = g_sb_hdr >= 0 ? g_sb_hdr
                          : (focus < mm->n_channels ? chan_group(&mm->channels[focus]) : -1);
                    if (g >= 0) { g_grp_collapsed[g] = 1; g_sb_hdr = g; }
                }
                else if (ev.key == TB_KEY_ARROW_RIGHT) {                     /* expand a folded group */
                    if (g_sb_hdr >= 0 && g_grp_collapsed[g_sb_hdr]) g_grp_collapsed[g_sb_hdr] = 0;
                }
                else if (ev.ch == 'n') { prompt_kind = PROMPT_NEWCHAN; prompt_title = "New channel"; tk_input_init(&prompt_input, 0, "name"); }
                else if (ev.key == TB_KEY_ENTER) {
                    if (g_sb_hdr >= 0) {                                      /* toggle the group's fold */
                        g_grp_collapsed[g_sb_hdr] = !g_grp_collapsed[g_sb_hdr];
                    } else if (focus < mm->n_channels) {                     /* channel action menu */
                        menu_build_channel(mm->channels[focus].joined);
                        act_cid = mm->channels[focus].channel_id; act_uid = 0; act_mid = 0; act_title = "Channel";
                        action_open = 1; tk_list_reset_filter(&action_menu);
                    }
                }
            }
            else if (panel == 3) {                                          /* members */
                if (up && mem_sel > 0) mem_sel--;
                else if (down && mem_sel + 1 < (int)mm->n_users) mem_sel++;
                else if (ev.ch == 'n') { prompt_kind = PROMPT_DM; prompt_title = "New direct message"; tk_input_init(&prompt_input, 0, "username"); }
                else if (ev.key == TB_KEY_ENTER && mem_sel < (int)mm->n_users) {
                    /* discoverable member action menu (Message / role / remove) */
                    uint64_t uid = mm->users[mem_sel].user_id;
                    menu_build_member(uid == mm->user_id);
                    act_uid = uid; act_cid = 0; act_mid = 0; act_title = "Member";
                    action_open = 1; tk_list_reset_filter(&action_menu);
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
                    int has_attach = 0;
                    for (uint8_t ak = 0; ak < sel->n_attach; ak++)
                        if (!sel->attach[ak].reclaimed) { has_attach = 1; break; }
                    if (ev.key == TB_KEY_ENTER) {   /* open the discoverable action menu */
                        menu_build_msg(own, sel->deleted, has_attach);
                        act_cid = cid; act_mid = mid; act_uid = 0; act_title = "Message";
                        action_open = 1; tk_list_reset_filter(&action_menu);
                    }
                    else if (ev.ch == 't') oc_client_open_thread(cl, cid, mid);
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
                /* Menu-driven: the composer only SENDS (or replies in a thread) —
                 * it no longer parses slash commands. All actions are reachable
                 * via the pane action menus, the dialogs, and the Ctrl-K launcher. */
                const oc_model *mm = oc_client_model(cl);
                uint64_t cid = mm->channels[focus].channel_id;
                if (mm->search_open || mm->roster_open || mm->reactlist_open || mm->prefs_open || mm->weblist_open) { /* read-only overlay: ignore */ }
                else if (mm->thread_open) oc_client_reply(cl, mm->thread_channel, mm->thread_parent, composer);
                else                      oc_client_send(cl, cid, composer);
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
        } else if (ev.ch == ':' && clen == 0) {   /* : opens the action launcher */
            const oc_model *mm = oc_client_model(cl);
            act_cid = focus < mm->n_channels ? mm->channels[focus].channel_id : 0;
            tk_palette_set(&launcher, g_launcher_items, (int)(sizeof g_launcher_items / sizeof *g_launcher_items));
            launcher_open = 1;
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
    tk_list_free(&action_menu);
    tk_palette_free(&launcher);
    for (int i = 0; i < g_nws; i++) oc_client_stop(g_ws[i].cl);
    g_nws = 0;
    oc_secret_free(secret);
    return 0;
}
