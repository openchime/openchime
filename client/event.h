/*
 * OpenChime client — the events/commands crossing the UI↔net queues (ARCH-62).
 * net→UI events report connection + protocol state; UI→net commands carry user
 * actions. Variable-length bodies are heap-owned by the struct.
 */

#ifndef OC_EVENT_H
#define OC_EVENT_H

#include <stdint.h>

/* net thread -> UI thread */
enum {
    OC_EV_CONNECTED = 1,   /* TLS + handshake up */
    OC_EV_AUTH_OK,         /* authenticated; user_id set */
    OC_EV_MESSAGE,         /* a BROADCAST: channel/author/message_id/time + body */
    OC_EV_DISCONNECTED,    /* connection dropped/closed */
    OC_EV_ERROR            /* protocol/transport error; body = human message */
};

typedef struct {
    int      type;
    uint64_t user_id;
    uint64_t channel_id;
    uint64_t author_id;
    uint64_t message_id;
    uint64_t server_time;
    char    *body;         /* heap; MESSAGE/ERROR only, else NULL */
} oc_ev;

oc_ev *oc_ev_new(int type);
void   oc_ev_free(oc_ev *e);

/* UI thread -> net thread */
enum {
    OC_CMD_SEND = 1,       /* send `body` to `channel_id` */
    OC_CMD_QUIT
};

typedef struct {
    int      type;
    uint64_t channel_id;
    char    *body;         /* heap; SEND only */
} oc_cmd;

oc_cmd *oc_cmd_new(int type);
void    oc_cmd_free(oc_cmd *c);

#endif /* OC_EVENT_H */
