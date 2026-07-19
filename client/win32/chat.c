/*
 * OpenChime Win32 GUI — chat surface. See chat.h.
 *
 * Layout (native comctl32 controls, laid out by hand on WM_SIZE):
 *
 *   +----------------+-------------------------------+---------------+
 *   |  Channels      |  Messages / open overlay      |  Members      |
 *   |  (list box)    |  (read-only multiline edit)   |  (list box)   |
 *   +----------------+-------------------------------+---------------+
 *   |  › composer (single-line edit) ................  [ Send ]      |
 *   +--------------------------------------------------------------- +
 *   |  status line (static)                                          |
 *   +---------------------------------------------------------------+
 *
 * The three panels are refilled from oc_model only when their content actually
 * changes (diffed against a cached signature) so selection and scroll survive
 * the 30 Hz tick. The composer parses the exact slash-command grammar the TUI
 * uses (see chat_do_command), so every engine feature is reachable here too.
 */

#include "chat.h"
#include "model.h"
#include "resolve.h"
#include "protocol.h"     /* OC_CHANNEL_KIND_DM, OC_PRESENCE_*, OC_NOTIFY_*, OC_ROLE_*, OC_LOGOUT_* */
#include "oc_port.h"      /* oc_localtime_r */

#include <commctrl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Synced-settings keys (must match the daemon bucket + the TUI). */
#define OC_SYNC_KEY_MOUSE     "mouse"
#define OC_SYNC_KEY_MEMBERS   "members_panel"
#define OC_SYNC_KEY_TIME24    "time_24h"
#define OC_SYNC_KEY_CHWIDTH   "channels_width"
#define OC_SYNC_KEY_MEMWIDTH  "members_width"

enum { ID_CHAN = 200, ID_MSG, ID_MEMBERS, ID_INPUT, ID_SEND, ID_STATUSBAR };

static oc_client *CL;
static HWND hChan, hMsg, hMembers, hInput, hSend, hBar;
static HFONT gFont, gMono;

static size_t   g_focus;        /* index into model->channels of the open channel */
static uint64_t g_focus_cid;    /* its channel_id (0 = none) */
static int      g_built;
static DWORD    g_last_typing;  /* throttle TYPING_UPDATE */

static int can_send(void);      /* forward decl */

/* Content caches: a panel is refilled only when its signature changes. */
static char *sig_chan, *sig_mem, *sig_msg, *sig_bar;

int chat_active(void) { return g_built; }

/* ---- tiny growable string ------------------------------------------------- */
typedef struct { char *b; size_t n, cap; } sb;
static void sb_ensure(sb *s, size_t extra) {
    if (s->n + extra + 1 <= s->cap) return;
    size_t cap = s->cap ? s->cap * 2 : 1024;
    while (cap < s->n + extra + 1) cap *= 2;
    s->b = (char *)realloc(s->b, cap);
    s->cap = cap;
}
static void sb_add(sb *s, const char *t) {
    if (!t) return;
    size_t len = strlen(t);
    sb_ensure(s, len);
    memcpy(s->b + s->n, t, len);
    s->n += len;
    s->b[s->n] = '\0';
}
static void sb_addf(sb *s, const char *fmt, ...) {
    char tmp[1024];
    va_list ap; va_start(ap, fmt);
    vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    sb_add(s, tmp);
}

/* Replace the cache string *slot with a copy of `now`, returning 1 if it
 * changed (so the caller repaints), 0 if identical. Takes ownership of `now`. */
static int cache_swap(char **slot, char *now) {
    if (*slot && strcmp(*slot, now) == 0) { free(now); return 0; }
    free(*slot);
    *slot = now;
    return 1;
}

/* ---- model helpers -------------------------------------------------------- */
static const oc_channel *find_chan(const oc_model *m, uint64_t cid) {
    for (size_t i = 0; i < m->n_channels; i++)
        if (m->channels[i].channel_id == cid) return &m->channels[i];
    return NULL;
}
static const oc_msg *my_last(const oc_channel *c, uint64_t uid) {
    for (size_t i = c->n_msgs; i-- > 0; )
        if (c->msgs[i].author_id == uid && !c->msgs[i].deleted) return &c->msgs[i];
    return NULL;
}
static const char *attach_filename(const oc_model *m, uint64_t aid) {
    for (size_t i = 0; i < m->n_channels; i++) {
        const oc_channel *c = &m->channels[i];
        for (size_t j = 0; j < c->n_msgs; j++)
            for (uint8_t k = 0; k < c->msgs[j].n_attach; k++)
                if (c->msgs[j].attach[k].id == aid) return c->msgs[j].attach[k].filename;
    }
    return "";
}
static int parse_hhmm(const char *s) {
    int h, mn;
    if (sscanf(s, "%d:%d", &h, &mn) != 2) return -1;
    if (h < 0 || h > 23 || mn < 0 || mn > 59) return -1;
    return h * 60 + mn;
}
static int onoff(const char *v) {
    return strcmp(v, "on") == 0 || strcmp(v, "1") == 0 || strcmp(v, "true") == 0 || strcmp(v, "yes") == 0;
}
static const char *presence_tag(uint8_t p) {
    return p == OC_PRESENCE_ONLINE ? "online" : p == OC_PRESENCE_AWAY ? "away" : "offline";
}
static void hhmm(char *out, size_t cap, uint64_t server_ms) {
    time_t t = (time_t)(server_ms / 1000);
    struct tm tmv;
    if (oc_localtime_r(&t, &tmv)) strftime(out, cap, "%H:%M", &tmv);
    else snprintf(out, cap, "--:--");
}

/* ---- message-pane text builders ------------------------------------------ */
/* One rendered message line (author, body, edit/delete/reactions/attachments). */
static void append_msg(sb *s, const oc_model *m, const oc_msg *msg) {
    char ts[16]; hhmm(ts, sizeof ts, msg->server_time);
    const char *who = msg->author_name[0] ? msg->author_name
                    : (oc_model_user_name(m, msg->author_id)[0] ? oc_model_user_name(m, msg->author_id) : "?");
    if (msg->deleted) {
        sb_addf(s, "%s  %s: (message deleted)\r\n", ts, who);
        return;
    }
    sb_addf(s, "%s  %s: %s%s\r\n", ts, who, msg->body ? msg->body : "", msg->edited ? "  (edited)" : "");
    for (uint8_t k = 0; k < msg->n_attach; k++) {
        const oc_attachment *a = &msg->attach[k];
        if (a->reclaimed)
            sb_addf(s, "        [attachment #%llu %s — no longer available]\r\n",
                    (unsigned long long)a->id, a->filename);
        else
            sb_addf(s, "        [attachment #%llu %s (%llu bytes) — /download %llu]\r\n",
                    (unsigned long long)a->id, a->filename,
                    (unsigned long long)a->size, (unsigned long long)a->id);
    }
    if (msg->n_reactions) {
        sb_add(s, "        ");
        for (uint8_t k = 0; k < msg->n_reactions; k++)
            sb_addf(s, "%s %u%s ", msg->reactions[k].emoji, msg->reactions[k].count,
                    msg->reactions[k].mine ? "*" : "");
        sb_add(s, "\r\n");
    }
    if (msg->reply_count)
        sb_addf(s, "        \xE2\x86\xB3 %u repl%s  (/thread on the last message)\r\n",
                msg->reply_count, msg->reply_count == 1 ? "y" : "ies");
}

/* Build the message-pane text for the current view (channel or open overlay). */
static char *build_msg_text(const oc_model *m) {
    sb s = {0};
    const oc_channel *fc = find_chan(m, g_focus_cid);

    if (m->roster_open) {
        sb_add(&s, "MEMBERS  (/close to return)\r\n\r\n");
        for (size_t i = 0; i < m->n_users; i++) {
            const oc_member *u = &m->users[i];
            const char *role = u->role == OC_ROLE_OWNER ? " (owner)" : u->role == OC_ROLE_ADMIN ? " (admin)" : "";
            sb_addf(&s, "  %-8s %s%s%s\r\n", presence_tag(oc_model_presence_of(m, u->user_id)),
                    u->name[0] ? u->name : "?", role, u->disabled ? "  [disabled]" : "");
        }
    } else if (m->search_open) {
        sb_addf(&s, "SEARCH  \"%s\"  (%zu hit%s · /close to return)\r\n\r\n",
                m->search_query, m->n_search, m->n_search == 1 ? "" : "s");
        for (size_t i = 0; i < m->n_search; i++) {
            const oc_search_result *r = &m->search_results[i];
            char ts[16]; hhmm(ts, sizeof ts, r->server_time);
            const oc_channel *rc = find_chan(m, r->channel_id);
            sb_addf(&s, "  %s  #%s  %s: %s\r\n", ts, rc && rc->name ? rc->name : "?",
                    oc_model_user_name(m, r->author_id), r->snippet ? r->snippet : "");
        }
    } else if (m->reactlist_open) {
        sb_add(&s, "WHO REACTED  (/close to return)\r\n\r\n");
        for (size_t i = 0; i < m->n_reactors; i++)
            sb_addf(&s, "  %s  %s\r\n", m->reactors[i].emoji, oc_model_user_name(m, m->reactors[i].user_id));
    } else if (m->prefs_open) {
        sb_add(&s, "NOTIFICATIONS  (/notify all|mentions|none · /dnd HH:MM HH:MM|off · /close)\r\n\r\n");
        if (m->dnd_enabled)
            sb_addf(&s, "  Do-not-disturb: %02u:%02u – %02u:%02u\r\n\r\n",
                    m->dnd_start_min / 60, m->dnd_start_min % 60, m->dnd_end_min / 60, m->dnd_end_min % 60);
        else
            sb_add(&s, "  Do-not-disturb: off\r\n\r\n");
        for (size_t i = 0; i < m->n_channels; i++) {
            const oc_channel *c = &m->channels[i];
            if (c->kind == OC_CHANNEL_KIND_DM || !c->name) continue;
            const char *lv = c->notify_level == OC_NOTIFY_ALL ? "all"
                           : c->notify_level == OC_NOTIFY_MENTIONS ? "mentions" : "none";
            sb_addf(&s, "  #%-20s %s\r\n", c->name, lv);
        }
    } else if (m->weblist_open) {
        const oc_channel *wc = find_chan(m, m->weblist_channel);
        sb_addf(&s, "WEBHOOKS  #%s  (/webhook create <label> · /webhook rm <id> · /close)\r\n\r\n",
                wc && wc->name ? wc->name : "?");
        if (m->webhook_token[0])
            sb_addf(&s, "  New webhook #%llu token (shown once): %s\r\n\r\n",
                    (unsigned long long)m->webhook_new_id, m->webhook_token);
        for (size_t i = 0; i < m->n_webhooks; i++)
            sb_addf(&s, "  #%llu  %s%s\r\n", (unsigned long long)m->webhooks[i].webhook_id,
                    m->webhooks[i].label, m->webhooks[i].disabled ? "  [disabled]" : "");
    } else if (m->storage_open) {
        sb_add(&s, "STORAGE  (/close to return)\r\n\r\n");
        if (m->storage_have) {
            const oc_storage_view *sv = &m->storage;
            sb_addf(&s, "  attachments: %llu bytes in %llu file%s\r\n",
                    (unsigned long long)sv->attach_bytes, (unsigned long long)sv->attach_count,
                    sv->attach_count == 1 ? "" : "s");
            sb_addf(&s, "  disk: %llu bytes used of %llu total, %llu available\r\n",
                    (unsigned long long)sv->total_bytes, (unsigned long long)(sv->total_bytes + sv->avail_bytes),
                    (unsigned long long)sv->avail_bytes);
            sb_addf(&s, "  reclaimed: %llu orphan · %llu expired · %llu evicted%s\r\n",
                    (unsigned long long)sv->rec_orphan, (unsigned long long)sv->rec_expired,
                    (unsigned long long)sv->rec_evicted, sv->under_pressure ? "  [under pressure]" : "");
        } else {
            sb_add(&s, "  (no report — owner/admin only)\r\n");
        }
    } else if (m->audit_open) {
        sb_add(&s, "AUDIT LOG  (newest first · /close to return)\r\n\r\n");
        for (size_t i = 0; i < m->n_audit; i++) {
            const oc_audit_view *a = &m->audit[i];
            char ts[16]; hhmm(ts, sizeof ts, a->at_ms);
            sb_addf(&s, "  %s  %s %s %s%s%s\r\n", ts,
                    a->actor_name[0] ? a->actor_name : "?", a->action,
                    a->target[0] ? a->target : "", a->detail[0] ? "  " : "", a->detail);
        }
    } else if (m->thread_open) {
        sb_addf(&s, "THREAD  (/reply-less: type to reply · /close to return)\r\n\r\n");
        const oc_channel *tc = find_chan(m, m->thread_channel);
        if (tc) {
            const oc_msg *parent = NULL;
            for (size_t i = 0; i < tc->n_msgs; i++)
                if (tc->msgs[i].message_id == m->thread_parent) { parent = &tc->msgs[i]; break; }
            if (parent) { append_msg(&s, m, parent); sb_add(&s, "  ----\r\n"); }
        }
        for (size_t i = 0; i < m->n_thread_msgs; i++) append_msg(&s, m, &m->thread_msgs[i]);
    } else if (fc) {
        for (size_t i = 0; i < fc->n_msgs; i++) append_msg(&s, m, &fc->msgs[i]);
        if (fc->n_msgs == 0) sb_add(&s, "  (no messages yet — say hello)\r\n");
        uint64_t tp[8];
        size_t nt = oc_model_typing(m, fc->channel_id, m->user_id, tp, 8);
        if (nt == 1) sb_addf(&s, "\r\n  \xE2\x9C\x8E %s is typing…\r\n", oc_model_user_name(m, tp[0]));
        else if (nt) sb_addf(&s, "\r\n  \xE2\x9C\x8E %zu people are typing…\r\n", nt);
    } else {
        sb_add(&s, "  Select a channel on the left, or /join one.\r\n");
    }
    if (!s.b) { s.b = _strdup(""); }
    return s.b;
}

/* ---- rendering ------------------------------------------------------------ */
static void set_edit_text_scrolled(HWND h, const char *utf8) {
    /* Convert UTF-8 -> UTF-16 so glyphs beyond ASCII render, then set + scroll
     * to the bottom (newest message). */
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc((size_t)wn * sizeof(wchar_t));
    if (!w) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, wn);
    SetWindowTextW(h, w);
    free(w);
    int lines = (int)SendMessageW(h, EM_GETLINECOUNT, 0, 0);
    SendMessageW(h, EM_LINESCROLL, 0, lines);
}
static void set_static_utf8(HWND h, const char *utf8) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc((size_t)wn * sizeof(wchar_t));
    if (!w) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, wn);
    SetWindowTextW(h, w);
    free(w);
}
static void listbox_add_utf8(HWND h, const char *utf8) {
    int wn = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, NULL, 0);
    wchar_t *w = (wchar_t *)malloc((size_t)wn * sizeof(wchar_t));
    if (!w) return;
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, w, wn);
    SendMessageW(h, LB_ADDSTRING, 0, (LPARAM)w);
    free(w);
}

static void render_channels(const oc_model *m) {
    sb items = {0}, sig = {0};
    for (size_t i = 0; i < m->n_channels; i++) {
        const oc_channel *c = &m->channels[i];
        char title[160];
        if (c->kind == OC_CHANNEL_KIND_DM) {
            const char *pn = (c->peer_id == m->user_id) ? "you" : oc_model_user_name(m, c->peer_id);
            snprintf(title, sizeof title, "@%s", pn[0] ? pn : "dm");
        } else {
            snprintf(title, sizeof title, "%s%s", c->joined ? "# " : "+ ", c->name ? c->name : "…");
        }
        char line[200];
        if (c->unread > 0) snprintf(line, sizeof line, "%s (%d)", title, c->unread);
        else               snprintf(line, sizeof line, "%s", title);
        /* one entry per line, NUL-joined into a single blob for the listbox */
        sb_add(&items, line); sb_add(&items, "\n");
        sb_addf(&sig, "%s|", line);
    }
    if (cache_swap(&sig_chan, sig.b ? sig.b : _strdup(""))) {
        SendMessageW(hChan, WM_SETREDRAW, FALSE, 0);
        SendMessageW(hChan, LB_RESETCONTENT, 0, 0);
        char *save = items.b, *p = items.b;
        while (p && *p) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            listbox_add_utf8(hChan, p);
            if (!nl) break;
            p = nl + 1;
        }
        (void)save;
        SendMessageW(hChan, LB_SETCURSEL, (WPARAM)g_focus, 0);
        SendMessageW(hChan, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(hChan, NULL, TRUE);
    }
    free(items.b);
}

static void render_members(const oc_model *m) {
    sb items = {0}, sig = {0};
    for (size_t i = 0; i < m->n_users; i++) {
        const oc_member *u = &m->users[i];
        uint8_t p = oc_model_presence_of(m, u->user_id);
        const char *role = u->role == OC_ROLE_OWNER ? " (owner)" : u->role == OC_ROLE_ADMIN ? " (admin)" : "";
        char line[160];
        snprintf(line, sizeof line, "%-8s %s%s%s", presence_tag(p),
                 u->name[0] ? u->name : "?", role, u->disabled ? " [x]" : "");
        sb_add(&items, line); sb_add(&items, "\n");
        sb_addf(&sig, "%s|", line);
    }
    if (cache_swap(&sig_mem, sig.b ? sig.b : _strdup(""))) {
        SendMessageW(hMembers, WM_SETREDRAW, FALSE, 0);
        SendMessageW(hMembers, LB_RESETCONTENT, 0, 0);
        char *p = items.b;
        while (p && *p) {
            char *nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            listbox_add_utf8(hMembers, p);
            if (!nl) break;
            p = nl + 1;
        }
        SendMessageW(hMembers, WM_SETREDRAW, TRUE, 0);
        InvalidateRect(hMembers, NULL, TRUE);
    }
    free(items.b);
}

static void render_messages(const oc_model *m) {
    char *text = build_msg_text(m);
    if (cache_swap(&sig_msg, text))   /* cache_swap takes ownership of text */
        set_edit_text_scrolled(hMsg, sig_msg);
}

static void render_statusbar(const oc_model *m) {
    const char *me = m->user_id ? oc_model_user_name(m, m->user_id) : "";
    uint8_t pr = oc_model_presence_of(m, m->user_id);
    int unread = 0;
    for (size_t i = 0; i < m->n_channels; i++) if (m->channels[i].unread > 0) unread += m->channels[i].unread;
    const oc_channel *fc = find_chan(m, g_focus_cid);
    char title[96];
    if (!fc) snprintf(title, sizeof title, "no channel");
    else if (fc->kind == OC_CHANNEL_KIND_DM)
        snprintf(title, sizeof title, "@%s", oc_model_user_name(m, fc->peer_id));
    else snprintf(title, sizeof title, "#%s", fc->name ? fc->name : "…");

    sb s = {0};
    sb_addf(&s, " %s  ·  %s (%s)  ·  %s  ·  %d unread%s%s",
            title, me[0] ? me : "…", presence_tag(pr),
            m->authed ? "connected" : (m->connected ? "connecting…" : "offline"),
            unread,
            m->status[0] ? "  ·  " : "", m->status[0] ? m->status : "");
    if (cache_swap(&sig_bar, s.b ? s.b : _strdup(""))) set_static_utf8(hBar, sig_bar);
}

void chat_render(void) {
    if (!g_built || !CL) return;
    const oc_model *m = oc_client_model(CL);
    render_channels(m);
    render_members(m);
    render_messages(m);
    render_statusbar(m);
}

/* ---- opening a channel ---------------------------------------------------- */
static void open_channel(size_t idx) {
    const oc_model *m = oc_client_model(CL);
    if (idx >= m->n_channels) return;
    g_focus = idx;
    g_focus_cid = m->channels[idx].channel_id;
    if (!m->channels[idx].history_requested) oc_client_backfill(CL, g_focus_cid);
    oc_client_mark_read(CL, g_focus_cid);
    sig_msg = (free(sig_msg), NULL);   /* force a message-pane repaint */
    chat_render();
}

/* ---- command dispatch (ported from the TUI's handle_command) -------------- */
static void help_box(HWND hwnd) {
    MessageBoxW(hwnd,
        L"Type a message and press Enter to send, or a slash command:\r\n\r\n"
        L"/join <name>   /leave   /create <name>   /list   /dm <user>\r\n"
        L"/who   /away   /online   /search <text>   /thread   /close\r\n"
        L"/react <emoji>   /reactions   /edit <text>   /delete\r\n"
        L"/prefs   /notify all|mentions|none   /dnd HH:MM HH:MM|off\r\n"
        L"/upload <path>   /download <id> [dest]   /webhook [create <label>|rm <id>]\r\n"
        L"/profile   /nick <name>   /passwd <old> <new>   /role <user> owner|admin|member\r\n"
        L"/invite [admin]   /remove <user>   /storage   /audit\r\n"
        L"/set mouse|members|time|channels-width|members-width <value>\r\n"
        L"/logout   /help",
        L"OpenChime — commands", MB_OK | MB_ICONINFORMATION);
}

/* Returns 1 if the line was a command (handled), 0 if it is a plain message. */
static int chat_do_command(HWND hwnd, uint64_t cid, const char *line) {
    const oc_model *m = oc_client_model(CL);
    if (line[0] != '/') return 0;

    if (strcmp(line, "/help") == 0) { help_box(hwnd); return 1; }
    if (strcmp(line, "/close") == 0) {
        oc_client_close_thread(CL); oc_client_close_search(CL);
        oc_client_close_reactions(CL); oc_client_toggle_roster(CL, 0);
        oc_client_toggle_prefs(CL, 0); oc_client_close_webhooks(CL);
        oc_client_toggle_storage(CL, 0); oc_client_toggle_audit(CL, 0);
        return 1;
    }
    if (strncmp(line, "/webhook", 8) == 0) {
        const char *a = line + 8; while (*a == ' ') a++;
        if (strncmp(a, "create ", 7) == 0) {
            const char *lb = a + 7; while (*lb == ' ') lb++;
            if (*lb) oc_client_create_webhook(CL, cid, lb);
        } else if (strncmp(a, "rm ", 3) == 0 || strncmp(a, "delete ", 7) == 0) {
            const char *id = a + (a[0] == 'r' ? 3 : 7); while (*id == ' ') id++;
            unsigned long long wid = strtoull(id, NULL, 10);
            if (wid) oc_client_delete_webhook(CL, wid);
        } else oc_client_webhooks(CL, cid);
        return 1;
    }
    if (strncmp(line, "/upload ", 8) == 0) {
        const char *path = line + 8; while (*path == ' ') path++;
        if (*path) oc_client_upload(CL, cid, path);
        return 1;
    }
    if (strncmp(line, "/download ", 10) == 0) {
        const char *a = line + 10; while (*a == ' ') a++;
        char *end = NULL;
        unsigned long long id = strtoull(a, &end, 10);
        if (!id) return 1;
        while (end && *end == ' ') end++;
        char dest[512];
        if (end && *end) snprintf(dest, sizeof dest, "%s", end);
        else {
            const char *fn = attach_filename(m, id);
            if (fn[0]) snprintf(dest, sizeof dest, "%s", fn);
            else snprintf(dest, sizeof dest, "attachment-%llu", id);
        }
        oc_client_download(CL, id, dest);
        return 1;
    }
    if (strcmp(line, "/prefs") == 0)   { oc_client_toggle_prefs(CL, 1); return 1; }
    if (strcmp(line, "/storage") == 0) { oc_client_toggle_storage(CL, 1); oc_client_storage_status(CL); return 1; }
    if (strcmp(line, "/audit") == 0)   { oc_client_toggle_audit(CL, 1); oc_client_audit_query(CL, 0); return 1; }
    if (strncmp(line, "/notify ", 8) == 0) {
        const char *lv = line + 8; while (*lv == ' ') lv++;
        uint8_t level;
        if      (strcmp(lv, "all") == 0)      level = OC_NOTIFY_ALL;
        else if (strcmp(lv, "mentions") == 0) level = OC_NOTIFY_MENTIONS;
        else if (strcmp(lv, "none") == 0)     level = OC_NOTIFY_NONE;
        else return 1;
        oc_client_set_notify_pref(CL, cid, level);
        return 1;
    }
    if (strncmp(line, "/dnd", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
        const char *a = line + 4; while (*a == ' ') a++;
        if (strcmp(a, "off") == 0 || *a == '\0') { oc_client_set_dnd(CL, 0, 0, 0); return 1; }
        char s1[16] = {0}, s2[16] = {0};
        if (sscanf(a, "%15s %15s", s1, s2) != 2) return 1;
        int start = parse_hhmm(s1), end = parse_hhmm(s2);
        if (start < 0 || end < 0) return 1;
        oc_client_set_dnd(CL, 1, (uint16_t)start, (uint16_t)end);
        return 1;
    }
    if (strcmp(line, "/who") == 0)    { oc_client_toggle_roster(CL, 1); oc_client_list_users(CL); return 1; }
    if (strcmp(line, "/away") == 0)   { oc_client_set_presence(CL, OC_PRESENCE_AWAY); return 1; }
    if (strcmp(line, "/online") == 0) { oc_client_set_presence(CL, OC_PRESENCE_ONLINE); return 1; }
    if (strncmp(line, "/search ", 8) == 0) {
        const char *q = line + 8; while (*q == ' ') q++;
        if (*q) oc_client_search(CL, q);
        return 1;
    }
    if (strncmp(line, "/create ", 8) == 0) {
        const char *nm = line + 8; while (*nm == ' ') nm++;
        if (*nm) oc_client_create_channel(CL, nm);
        return 1;
    }
    if (strncmp(line, "/join ", 6) == 0) {
        const char *nm = line + 6; while (*nm == ' ') nm++;
        for (size_t i = 0; i < m->n_channels; i++)
            if (m->channels[i].name && strcmp(m->channels[i].name, nm) == 0) {
                oc_client_join_channel(CL, m->channels[i].channel_id); break;
            }
        return 1;
    }
    if (strcmp(line, "/leave") == 0) { oc_client_leave_channel(CL, cid); return 1; }
    if (strcmp(line, "/list") == 0)  { oc_client_list_channels(CL); return 1; }
    if (strncmp(line, "/dm ", 4) == 0) {
        const char *nm = line + 4; while (*nm == ' ') nm++;
        uint64_t uid = oc_model_user_id(m, nm);
        if (uid) oc_client_open_dm(CL, uid);
        return 1;
    }
    if (strncmp(line, "/role ", 6) == 0) {
        char nm[64] = {0}, rl[16] = {0};
        if (sscanf(line + 6, "%63s %15s", nm, rl) != 2) return 1;
        uint8_t role;
        if      (strcmp(rl, "owner") == 0)  role = OC_ROLE_OWNER;
        else if (strcmp(rl, "admin") == 0)  role = OC_ROLE_ADMIN;
        else if (strcmp(rl, "member") == 0) role = OC_ROLE_MEMBER;
        else return 1;
        uint64_t uid = oc_model_user_id(m, nm);
        if (uid) oc_client_set_role(CL, uid, role);
        return 1;
    }
    if (strncmp(line, "/invite", 7) == 0) {
        const char *rl = line + 7; while (*rl == ' ') rl++;
        uint8_t role = strcmp(rl, "admin") == 0 ? OC_ROLE_ADMIN : OC_ROLE_MEMBER;
        oc_client_invite_user(CL, role);
        return 1;
    }
    if (strncmp(line, "/remove ", 8) == 0) {
        const char *nm = line + 8; while (*nm == ' ') nm++;
        uint64_t uid = oc_model_user_id(m, nm);
        if (uid) oc_client_remove_user(CL, uid);
        return 1;
    }
    if (strcmp(line, "/profile") == 0) {
        const char *me = oc_model_user_name(m, m->user_id);
        char buf[256];
        snprintf(buf, sizeof buf, "You are %s (id %llu).\nUse /nick <name> to rename, /passwd <old> <new> to rotate your password.",
                 me[0] ? me : "?", (unsigned long long)m->user_id);
        set_static_utf8(hBar, buf);   /* transient; overwritten next status refresh */
        MessageBoxA(hwnd, buf, "OpenChime — profile", MB_OK | MB_ICONINFORMATION);
        return 1;
    }
    if (strncmp(line, "/nick ", 6) == 0) {
        const char *nm = line + 6; while (*nm == ' ') nm++;
        if (*nm) oc_client_set_display_name(CL, nm);
        return 1;
    }
    if (strncmp(line, "/passwd ", 8) == 0) {
        char oldp[128] = {0}, newp[128] = {0};
        if (sscanf(line + 8, "%127s %127s", oldp, newp) == 2 && newp[0])
            oc_client_change_password(CL, oldp, newp);
        return 1;
    }
    if (strncmp(line, "/set", 4) == 0 && (line[4] == '\0' || line[4] == ' ')) {
        char key[24] = {0}, val[32] = {0};
        sscanf(line + 4, "%23s %31s", key, val);
        char num[8];
        if (strcmp(key, "mouse") == 0) {
            oc_client_set_setting(CL, OC_SYNC_KEY_MOUSE, onoff(val) ? "1" : "0");
        } else if (strcmp(key, "members") == 0) {
            int v = strcmp(val, "auto") == 0 ? 2 : (onoff(val) ? 1 : 0);
            snprintf(num, sizeof num, "%d", v);
            oc_client_set_setting(CL, OC_SYNC_KEY_MEMBERS, num);
        } else if (strcmp(key, "time") == 0) {
            oc_client_set_setting(CL, OC_SYNC_KEY_TIME24, strcmp(val, "24h") == 0 ? "1" : "0");
        } else if (strcmp(key, "channels-width") == 0 || strcmp(key, "members-width") == 0) {
            int n = atoi(val);
            if (n >= 10 && n <= 60) {
                snprintf(num, sizeof num, "%d", n);
                oc_client_set_setting(CL, key[0] == 'c' ? OC_SYNC_KEY_CHWIDTH : OC_SYNC_KEY_MEMWIDTH, num);
            }
        }
        return 1;
    }
    if (strcmp(line, "/logout") == 0) { oc_client_logout(CL, OC_LOGOUT_THIS); return 1; }

    /* Commands that act on the focused channel's messages. */
    const oc_channel *ch = find_chan(m, cid);
    if (!ch || ch->n_msgs == 0) return 1;

    if (strcmp(line, "/thread") == 0) {
        oc_client_open_thread(CL, cid, ch->msgs[ch->n_msgs - 1].message_id);
    } else if (strcmp(line, "/reactions") == 0) {
        oc_client_list_reactions(CL, cid, ch->msgs[ch->n_msgs - 1].message_id);
    } else if (strncmp(line, "/react ", 7) == 0) {
        const char *emoji = line + 7; while (*emoji == ' ') emoji++;
        if (!*emoji) return 1;
        const oc_msg *last = &ch->msgs[ch->n_msgs - 1];
        uint8_t op = 1;
        for (uint8_t k = 0; k < last->n_reactions; k++)
            if (strcmp(last->reactions[k].emoji, emoji) == 0 && last->reactions[k].mine) { op = 0; break; }
        oc_client_react(CL, cid, last->message_id, emoji, op);
    } else if (strncmp(line, "/edit ", 6) == 0) {
        const char *text = line + 6; while (*text == ' ') text++;
        if (!*text) return 1;
        const oc_msg *mine = my_last(ch, m->user_id);
        if (mine) oc_client_edit(CL, cid, mine->message_id, text);
    } else if (strcmp(line, "/delete") == 0) {
        const oc_msg *mine = my_last(ch, m->user_id);
        if (mine) oc_client_delete(CL, cid, mine->message_id);
    }
    return 1;
}

/* Read the composer, dispatch (command or message), clear it. */
static void submit_input(HWND hwnd) {
    wchar_t wbuf[2048];
    GetWindowTextW(hInput, wbuf, 2048);
    char line[4096];
    WideCharToMultiByte(CP_UTF8, 0, wbuf, -1, line, sizeof line, NULL, NULL);
    /* trim trailing whitespace */
    size_t n = strlen(line);
    while (n && (line[n-1] == ' ' || line[n-1] == '\r' || line[n-1] == '\n')) line[--n] = '\0';
    if (!n) return;

    if (line[0] == '/') {
        chat_do_command(hwnd, g_focus_cid, line);
    } else if (can_send()) {
        oc_client_send(CL, g_focus_cid, line);
    }
    SetWindowTextW(hInput, L"");
    sig_msg = (free(sig_msg), NULL);   /* repaint promptly */
    chat_render();
}

/* Can we post to the focused channel right now? */
static int can_send(void) {
    if (!g_focus_cid) return 0;
    const oc_model *m = oc_client_model(CL);
    const oc_channel *c = find_chan(m, g_focus_cid);
    return c && (c->joined || c->kind == OC_CHANNEL_KIND_DM);
}

int chat_command(HWND hwnd, WPARAM wp, LPARAM lp) {
    (void)lp;
    if (!g_built) return 0;
    WORD id = LOWORD(wp), code = HIWORD(wp);
    if (id == ID_SEND && code == BN_CLICKED) { submit_input(hwnd); return 1; }
    if (id == ID_CHAN && code == LBN_SELCHANGE) {
        int sel = (int)SendMessageW(hChan, LB_GETCURSEL, 0, 0);
        if (sel >= 0) open_channel((size_t)sel);
        return 1;
    }
    if (id == ID_MEMBERS && code == LBN_DBLCLK) {
        int sel = (int)SendMessageW(hMembers, LB_GETCURSEL, 0, 0);
        const oc_model *m = oc_client_model(CL);
        if (sel >= 0 && (size_t)sel < m->n_users) oc_client_open_dm(CL, m->users[sel].user_id);
        return 1;
    }
    if (id == ID_INPUT && code == EN_CHANGE) {
        /* Throttled typing indicator: only when composing a real message. */
        DWORD now = GetTickCount();
        if (g_focus_cid && now - g_last_typing > 1500) {
            wchar_t wc[8]; GetWindowTextW(hInput, wc, 8);
            if (wc[0] && wc[0] != L'/') { oc_client_typing(CL, g_focus_cid); g_last_typing = now; }
        }
        return 1;
    }
    return 0;
}

/* ---- construction + layout ------------------------------------------------ */
void chat_build(HWND hwnd, HINSTANCE inst, oc_client *cl) {
    CL = cl;
    gFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
    LOGFONTW lf; memset(&lf, 0, sizeof lf);
    lf.lfHeight = -14; wcscpy(lf.lfFaceName, L"Consolas");
    gMono = CreateFontIndirectW(&lf);

    hChan = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_CHAN, inst, NULL);
    hMsg = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_MSG, inst, NULL);
    hMembers = CreateWindowExW(WS_EX_CLIENTEDGE, L"LISTBOX", NULL,
        WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_MEMBERS, inst, NULL);
    hInput = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", NULL,
        WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_INPUT, inst, NULL);
    hSend = CreateWindowExW(0, L"BUTTON", L"Send",
        WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_SEND, inst, NULL);
    hBar = CreateWindowExW(0, L"STATIC", L"",
        WS_CHILD | WS_VISIBLE | SS_LEFTNOWORDWRAP,
        0, 0, 10, 10, hwnd, (HMENU)(INT_PTR)ID_STATUSBAR, inst, NULL);

    SendMessageW(hChan, WM_SETFONT, (WPARAM)gFont, TRUE);
    SendMessageW(hMsg, WM_SETFONT, (WPARAM)gMono, TRUE);
    SendMessageW(hMembers, WM_SETFONT, (WPARAM)gFont, TRUE);
    SendMessageW(hInput, WM_SETFONT, (WPARAM)gFont, TRUE);
    SendMessageW(hSend, WM_SETFONT, (WPARAM)gFont, TRUE);
    SendMessageW(hBar, WM_SETFONT, (WPARAM)gFont, TRUE);

    g_built = 1;

    /* Ask for the roster + a fresh channel list, announce presence, and open the
     * first joined channel so there's something to see. */
    oc_client_list_users(CL);
    oc_client_list_channels(CL);
    oc_client_set_presence(CL, OC_PRESENCE_ONLINE);

    const oc_model *m = oc_client_model(CL);
    for (size_t i = 0; i < m->n_channels; i++)
        if (m->channels[i].joined) { open_channel(i); break; }

    chat_layout(hwnd);
    SetFocus(hInput);
}

void chat_layout(HWND hwnd) {
    if (!g_built) return;
    RECT rc; GetClientRect(hwnd, &rc);
    int W = rc.right, H = rc.bottom;
    int pad = 6;
    int barH = 22, inputH = 26, botH = barH + inputH + pad * 2;
    int chanW = 200, memW = 190;
    int panelsTop = pad, panelsH = H - botH - pad;
    if (panelsH < 40) panelsH = 40;
    int msgX = chanW + pad * 2;
    int msgW = W - chanW - memW - pad * 4;
    if (msgW < 120) msgW = 120;
    int memX = W - memW - pad;

    MoveWindow(hChan, pad, panelsTop, chanW, panelsH, TRUE);
    MoveWindow(hMsg, msgX, panelsTop, msgW, panelsH, TRUE);
    MoveWindow(hMembers, memX, panelsTop, memW, panelsH, TRUE);

    int inputY = panelsTop + panelsH + pad;
    int sendW = 80;
    MoveWindow(hInput, pad, inputY, W - sendW - pad * 3, inputH, TRUE);
    MoveWindow(hSend, W - sendW - pad, inputY, sendW, inputH, TRUE);
    MoveWindow(hBar, pad, inputY + inputH + pad / 2, W - pad * 2, barH, TRUE);
}
