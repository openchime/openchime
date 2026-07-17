/*
 * OpenChime client — the app-core facade (ARCH-74). A frontend uses ONLY this:
 * start a client, tick it once per frame (drains net events into the model),
 * read the model to render, send messages, and stop. The facade owns the two
 * UI↔net queues, the network thread, and the view-model; a frontend never
 * touches a socket or a queue directly.
 */

#ifndef OC_CLIENT_H
#define OC_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "model.h"

typedef struct oc_client oc_client;

/* Start the core: spawn the network thread and connect to host:port. `cred`
 * carries local credentials as "username:password" for now. Returns NULL if the
 * thread could not be spawned. */
oc_client *oc_client_start(const char *host, int port, const char *cred);

/* Drain all queued net events into the model. Call once per frame/tick. */
void oc_client_tick(oc_client *c);

/* The current view-model (owned by the client; valid until oc_client_stop). */
const oc_model *oc_client_model(oc_client *c);

/* Queue a message to `channel_id` for the network thread to send. */
void oc_client_send(oc_client *c, uint64_t channel_id, const char *body);

/* Request history for `channel_id` (once per channel — a frontend calls this the
 * first time a channel is opened). Replayed messages fold into the model. */
void oc_client_backfill(oc_client *c, uint64_t channel_id);

/* Mark `channel_id` read (clear its unread count). A frontend calls this for the
 * focused channel. */
void oc_client_mark_read(oc_client *c, uint64_t channel_id);

/* React to a message: `op` is 1 (add) or 0 (remove). The server fans back a
 * REACTION_UPDATED that folds into the message's aggregates. */
void oc_client_react(oc_client *c, uint64_t channel_id, uint64_t message_id,
                     const char *emoji, uint8_t op);

/* Edit / delete a message (your own, or as a moderator). The server fans back a
 * MSG_EDITED / MSG_DELETED that updates the model. */
void oc_client_edit(oc_client *c, uint64_t channel_id, uint64_t message_id, const char *body);
void oc_client_delete(oc_client *c, uint64_t channel_id, uint64_t message_id);

/* Signal "I am typing" in `channel_id`. The frontend calls this (throttled) as
 * the user composes; the server relays a TYPING_UPDATE to other members. */
void oc_client_typing(oc_client *c, uint64_t channel_id);

/* Stop the network thread, drain remaining events, and free everything. */
void oc_client_stop(oc_client *c);

#endif /* OC_CLIENT_H */
