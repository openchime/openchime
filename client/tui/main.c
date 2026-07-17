/*
 * OpenChime TUI — the first frontend over the app-core (ARCH-74/75). termbox2
 * gives the cell grid + input; utf8proc gives correct display width so emoji and
 * CJK don't corrupt the layout. This is the lean core loop: a channel sidebar
 * (with presence + unread), a wrapped scrolling message pane, and a modeless
 * composer. All state/logic lives in oc_client; this file is pure view + input.
 *
 * Usage: openchime-tui <host> <port> [user:pass]
 *        (credentials also read from $OPENCHIME_CRED = "user:pass")
 *
 * Keys: type + Enter to send · Tab next channel · ↑/↓ + PgUp/PgDn scroll ·
 *       Ctrl-Q quit. Commands (on the last message in the focused channel):
 *       /react <emoji> · /reactions (who reacted to it) · /edit <text> ·
 *       /delete · /thread (open its thread; then Enter posts a reply) ·
 *       /search <query> · /close (leave a thread/search/roster/reactions
 *       overlay) · /create <name> · /join <name> · /leave ·
 *       /who (member roster + presence) · /away · /online · /dm <name> (open a
 *       direct message) · /prefs (notification settings) · /notify
 *       all|mentions|none (this channel's level) · /dnd HH:MM HH:MM | off
 *       (do-not-disturb window) · /role <name> owner|admin|member ·
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
#include "protocol.h"   /* OC_PRESENCE_* */

#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define SIDEBAR_W 22
#define COMPOSER_CAP 1024

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

typedef struct { char *s; uintattr_t fg; } row_t;
typedef struct { row_t *v; size_t n, cap; } rows_t;

static void rows_push(rows_t *r, char *s, uintattr_t fg) {
    if (r->n == r->cap) {
        size_t cap = r->cap ? r->cap * 2 : 64;
        row_t *nv = realloc(r->v, cap * sizeof *nv);
        if (!nv) { free(s); return; }
        r->v = nv; r->cap = cap;
    }
    r->v[r->n].s = s; r->v[r->n].fg = fg; r->n++;
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
                            const oc_channel *ch, int show_replies) {
    char line[256], nick[80];
    char stamp[8] = "--:--";
    if (m->server_time) {
        time_t t = (time_t)(m->server_time / 1000);
        struct tm tmv;
        if (localtime_r(&t, &tmv)) strftime(stamp, sizeof stamp, "%H:%M", &tmv);
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
}

/* Build the wrapped rows for a channel's messages (oldest→newest). */
static void build_rows(rows_t *r, const oc_channel *ch, uint64_t me, int width) {
    for (size_t i = 0; i < ch->n_msgs; i++)
        append_msg_rows(r, &ch->msgs[i], me, width, ch, 1);
}

/* Build the rows for the open thread: the parent (found in `ch`) then replies. */
static void build_thread_rows(rows_t *r, const oc_model *m, const oc_channel *ch,
                              uint64_t me, int width) {
    if (ch) for (size_t i = 0; i < ch->n_msgs; i++)
        if (ch->msgs[i].message_id == m->thread_parent) {
            append_msg_rows(r, &ch->msgs[i], me, width, ch, 0);
            break;
        }
    for (size_t i = 0; i < m->n_thread_msgs; i++)
        append_msg_rows(r, &m->thread_msgs[i], me, width, ch, 0);
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

static void render(oc_client *cl, size_t focus, const char *composer,
                   size_t clen, int scroll) {
    (void)clen;
    const oc_model *m = oc_client_model(cl);
    int W = tb_width(), H = tb_height();
    if (W < SIDEBAR_W + 10 || H < 4) { tb_clear(); return; }
    tb_clear();

    /* Sidebar. */
    draw_clip(1, 0, SIDEBAR_W, "CHANNELS", TB_CYAN | TB_BOLD, TB_DEFAULT);
    for (size_t i = 0; i < m->n_channels && (int)i + 1 < H - 1; i++) {
        const oc_channel *c = &m->channels[i];
        uintattr_t bg = (i == focus) ? TB_BLUE : TB_DEFAULT;
        /* Not-joined channels (public ones you can /join) render dim. */
        uintattr_t fg = (i == focus) ? TB_WHITE | TB_BOLD
                      : c->joined    ? TB_DEFAULT
                                     : TB_BLACK | TB_BOLD;
        int y = (int)i + 1;
        fill_row(y, 0, SIDEBAR_W, bg);
        char title[96];
        if (c->kind == OC_CHANNEL_KIND_DM) {   /* a DM: title by the other participant */
            const char *pn = (c->peer_id == m->user_id) ? "you" : oc_model_user_name(m, c->peer_id);
            if (pn[0]) snprintf(title, sizeof title, "@%s", pn);
            else       snprintf(title, sizeof title, "@user%llu", (unsigned long long)c->peer_id);
        } else {
            snprintf(title, sizeof title, "%s%s", c->joined ? "# " : "+ ", c->name ? c->name : "…");
        }
        char label[128];
        if (c->unread > 0) snprintf(label, sizeof label, "%s (%d)", title, c->unread);
        else               snprintf(label, sizeof label, "%s", title);
        draw_clip(1, y, SIDEBAR_W, label, fg, bg);
    }
    for (int y = 0; y < H; y++) tb_set_cell(SIDEBAR_W, y, '|', TB_DEFAULT, TB_DEFAULT);

    int px0 = SIDEBAR_W + 1, pxmax = W;          /* message pane columns */
    int pane_top = 1, pane_bot = H - 2;          /* rows: [top, bot) */
    int pane_h = pane_bot - pane_top;
    if (pane_h < 1) pane_h = 1;

    /* Title bar (row 0 over the pane) — a thread view overlays the channel. */
    const oc_channel *fc = (focus < m->n_channels) ? &m->channels[focus] : NULL;
    char title[160];
    if (m->roster_open)          snprintf(title, sizeof title, " members (/close to exit)");
    else if (m->search_open)     snprintf(title, sizeof title, " search (/close to exit)");
    else if (m->reactlist_open)  snprintf(title, sizeof title, " reactions (/close to exit)");
    else if (m->prefs_open)      snprintf(title, sizeof title, " notifications (/close to exit)");
    else if (m->weblist_open)    snprintf(title, sizeof title, " webhooks (/close to exit)");
    else if (m->thread_open) snprintf(title, sizeof title, " thread · #%s (/close to exit)",
                                      fc && fc->name ? fc->name : "…");
    else if (fc)             snprintf(title, sizeof title, " #%s", fc->name ? fc->name : "…");
    else                     snprintf(title, sizeof title, " (no channel)");
    fill_row(0, px0, W, TB_BLUE);
    draw_clip(px0, 0, pxmax, title, TB_WHITE | TB_BOLD, TB_BLUE);

    /* Message rows for the focused channel, or an open overlay (thread / search /
     * roster), showing the window ending `scroll` rows above the bottom. */
    if (fc || m->thread_open || m->search_open || m->roster_open || m->reactlist_open || m->prefs_open || m->weblist_open) {
        rows_t rows = {0};
        if (m->roster_open)         build_roster_rows(&rows, m, pxmax - px0);
        else if (m->search_open)    build_search_rows(&rows, m, pxmax - px0);
        else if (m->reactlist_open) build_reactlist_rows(&rows, m, pxmax - px0);
        else if (m->prefs_open)     build_prefs_rows(&rows, m, pxmax - px0);
        else if (m->weblist_open)   build_webhooks_rows(&rows, m, pxmax - px0);
        else if (m->thread_open)    build_thread_rows(&rows, m, fc, m->user_id, pxmax - px0);
        else                        build_rows(&rows, fc, m->user_id, pxmax - px0);
        int total = (int)rows.n;
        int end = total - scroll;                /* one past the last visible row */
        if (end > total) end = total;
        if (end < 0) end = 0;
        int start = end - pane_h;
        if (start < 0) start = 0;
        int y = pane_top + (pane_h - (end - start)); /* bottom-align when few rows */
        for (int i = start; i < end; i++, y++)
            draw_clip(px0, y, pxmax, rows.v[i].s, rows.v[i].fg, TB_DEFAULT);
        rows_free(&rows);
    }

    /* Status line (row H-2). */
    char status[200];
    const char *conn = m->authed ? "online" : (m->connected ? "connecting…" : "offline");
    snprintf(status, sizeof status, " %s · %s%s", conn, m->status,
             scroll > 0 ? "  [scrolled]" : "");
    fill_row(H - 2, 0, W, TB_DEFAULT);
    int sx = draw_clip(0, H - 2, W, status, TB_YELLOW, TB_DEFAULT);

    /* Typing indicator for the focused channel (excluding ourselves). */
    if (fc) {
        uint64_t typers[8];
        size_t nt = oc_model_typing(m, fc->channel_id, m->user_id, typers, 8);
        if (nt) {
            char tline[140];
            if (nt == 1)
                snprintf(tline, sizeof tline, "  ✎ %s is typing…", name_for(fc, typers[0]));
            else
                snprintf(tline, sizeof tline, "  ✎ %zu people are typing…", nt);
            draw_clip(sx, H - 2, W, tline, TB_CYAN, TB_DEFAULT);
        }
    }

    /* Composer (row H-1). */
    fill_row(H - 1, 0, W, TB_DEFAULT);
    int cx = draw_clip(0, H - 1, W, "> ", TB_GREEN | TB_BOLD, TB_DEFAULT);
    cx = draw_clip(cx, H - 1, W, composer, TB_DEFAULT, TB_DEFAULT);
    if (cx < W) tb_set_cell(cx, H - 1, ' ', TB_DEFAULT, TB_REVERSE);   /* cursor */

    tb_present();
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

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <host> <port> [user:pass]\n", argv[0]);
        return 2;
    }
    /* Adopt the environment's locale so termbox2's iswprint() check recognizes
     * non-ASCII codepoints as printable — without this it renders every emoji /
     * wide glyph as U+FFFD, defeating the whole point of utf8proc. */
    setlocale(LC_ALL, "");

    const char *host = argv[1];
    int port = atoi(argv[2]);
    const char *cred = argc > 3 ? argv[3] : getenv("OPENCHIME_CRED");
    if (!cred) cred = "";

    oc_client *cl = oc_client_start(host, port, cred);
    if (!cl) { fprintf(stderr, "failed to start client\n"); return 1; }

    if (tb_init() != TB_OK) {
        fprintf(stderr, "failed to init terminal (need a tty)\n");
        oc_client_stop(cl);
        return 1;
    }

    char composer[COMPOSER_CAP];
    size_t clen = 0;
    composer[0] = '\0';
    size_t focus = 0;
    int scroll = 0;
    uint64_t last_focus_cid = 0;
    time_t last_typing = 0;                   /* throttle outbound TYPING signals */
    int running = 1, logging_out = 0;

    while (running) {
        oc_client_tick(cl);

        const oc_model *m = oc_client_model(cl);
        if (focus >= m->n_channels) focus = m->n_channels ? m->n_channels - 1 : 0;
        /* After /logout, quit once the server has closed the connection. */
        if (logging_out && !m->connected) running = 0;

        /* Lazy backfill + keep the focused channel marked read. */
        if (focus < m->n_channels) {
            uint64_t cid = m->channels[focus].channel_id;
            oc_client_backfill(cl, cid);
            oc_client_mark_read(cl, cid);
            if (cid != last_focus_cid) { scroll = 0; last_focus_cid = cid; }
        }

        render(cl, focus, composer, clen, scroll);

        struct tb_event ev;
        int rc = tb_peek_event(&ev, 30);
        if (rc != TB_OK) continue;               /* timeout: loop to re-tick + redraw */
        if (ev.type == TB_EVENT_RESIZE) continue;
        if (ev.type != TB_EVENT_KEY) continue;

        size_t nch = oc_client_model(cl)->n_channels;
        if (ev.key == TB_KEY_CTRL_Q || ev.key == TB_KEY_CTRL_C) {
            running = 0;
        } else if (ev.key == TB_KEY_ENTER) {
            if (clen > 0 && focus < nch) {
                const oc_model *mm = oc_client_model(cl);
                uint64_t cid = mm->channels[focus].channel_id;
                if (strcmp(composer, "/logout") == 0)         { oc_client_logout(cl, OC_LOGOUT_THIS); logging_out = 1; }
                else if (composer[0] == '/')                  handle_command(cl, cid, composer);
                else if (mm->search_open || mm->roster_open || mm->reactlist_open || mm->prefs_open || mm->weblist_open) { /* read-only overlay: ignore */ }
                else if (mm->thread_open)                     oc_client_reply(cl, mm->thread_channel, mm->thread_parent, composer);
                else                                          oc_client_send(cl, cid, composer);
                clen = 0; composer[0] = '\0'; scroll = 0;
            }
        } else if (ev.key == TB_KEY_BACKSPACE || ev.key == TB_KEY_BACKSPACE2) {
            backspace_utf8(composer, &clen);
        } else if (ev.key == TB_KEY_TAB) {
            if (nch) focus = (focus + 1) % nch;
        } else if (ev.key == TB_KEY_ARROW_UP) {
            scroll += 1;
        } else if (ev.key == TB_KEY_ARROW_DOWN) {
            if (scroll > 0) scroll -= 1;
        } else if (ev.key == TB_KEY_PGUP) {
            scroll += (tb_height() - 3) / 2;
        } else if (ev.key == TB_KEY_PGDN) {
            scroll -= (tb_height() - 3) / 2;
            if (scroll < 0) scroll = 0;
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
    oc_client_stop(cl);
    return 0;
}
