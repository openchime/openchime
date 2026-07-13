#include "app.h"
#include "event.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void oc_app_init(oc_app *a, oc_queue *to_net, oc_queue *from_net) {
    memset(a, 0, sizeof *a);
    a->to_net = to_net;
    a->from_net = from_net;
    snprintf(a->status, sizeof a->status, "connecting…");
}

void oc_app_free(oc_app *a) {
    for (size_t i = 0; i < a->n_msgs; i++) free(a->msgs[i].body);
    free(a->msgs);
    a->msgs = NULL;
    a->n_msgs = a->cap_msgs = 0;
}

static void append_msg(oc_app *a, uint64_t author, uint64_t id, const char *body) {
    if (a->n_msgs == a->cap_msgs) {
        size_t nc = a->cap_msgs ? a->cap_msgs * 2 : 64;
        oc_msg *g = realloc(a->msgs, nc * sizeof *g);
        if (!g) return;
        a->msgs = g; a->cap_msgs = nc;
    }
    a->msgs[a->n_msgs].author_id = author;
    a->msgs[a->n_msgs].message_id = id;
    a->msgs[a->n_msgs].body = body ? strdup(body) : NULL;
    a->n_msgs++;
}

void oc_app_drain(oc_app *a) {
    oc_ev *e;
    while ((e = oc_queue_try_pop(a->from_net)) != NULL) {
        switch (e->type) {
        case OC_EV_CONNECTED:
            a->connected = true; a->user_id = e->user_id;
            snprintf(a->status, sizeof a->status, "connected as user %llu",
                     (unsigned long long)e->user_id);
            break;
        case OC_EV_AUTH_OK:
            snprintf(a->status, sizeof a->status, "authenticated (user %llu)",
                     (unsigned long long)e->user_id);
            break;
        case OC_EV_MESSAGE:
            /* Client-side dedup on the per-channel high-water mark (ARCH-45). */
            if (e->message_id > a->high_water) {
                a->high_water = e->message_id;
                append_msg(a, e->author_id, e->message_id, e->body);
            }
            break;
        case OC_EV_DISCONNECTED:
            a->connected = false;
            snprintf(a->status, sizeof a->status, "disconnected");
            break;
        case OC_EV_ERROR:
            snprintf(a->status, sizeof a->status, "error: %s", e->body ? e->body : "?");
            break;
        default: break;
        }
        oc_ev_free(e);
    }
}

void oc_app_input_char(oc_app *a, int codepoint) {
    /* Phase 1: ASCII printable only; the real IME/Unicode entry is a later phase. */
    if (codepoint < 0x20 || codepoint > 0x7e) return;
    if (a->composer_len < (int)sizeof a->composer - 1)
        a->composer[a->composer_len++] = (char)codepoint;
    a->composer[a->composer_len] = '\0';
}

void oc_app_backspace(oc_app *a) {
    if (a->composer_len > 0) a->composer[--a->composer_len] = '\0';
}

void oc_app_submit(oc_app *a) {
    if (a->composer_len == 0) return;
    oc_cmd *c = oc_cmd_new(OC_CMD_SEND);
    if (c) {
        c->channel_id = 1;             /* the default channel for now */
        c->body = strdup(a->composer);
        oc_queue_push(a->to_net, c);
    }
    a->composer_len = 0;
    a->composer[0] = '\0';
    /* The echoed BROADCAST adds the message to the list, so don't add locally. */
}

void oc_app_scroll(oc_app *a, float delta) { a->scroll += delta; }
