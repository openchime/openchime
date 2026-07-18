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
#include "secret.h"

typedef struct oc_client oc_client;

/* Start the core: spawn the network thread and connect to host:port. `cred`
 * carries local credentials as "username:password" for now. Returns NULL if the
 * thread could not be spawned. */
oc_client *oc_client_start(const char *host, int port, const char *cred);

/* As oc_client_start, plus a local SQLite store at `store_path` (NULL = none)
 * that persists the session token + TOFU pin, so a relaunch reconnects silently
 * with the token instead of the password (REQ-100, ARCH-58). The parent
 * directory must exist; an unusable path just disables persistence. */
oc_client *oc_client_start_stored(const char *host, int port, const char *cred,
                                  const char *store_path);

/* As oc_client_start_stored, plus an OS secret store (borrowed; NULL = none) that
 * holds the session token in the platform keyring instead of the SQLite file
 * (ARCH-74). The caller owns `secret` and frees it after oc_client_stop. */
oc_client *oc_client_start_secure(const char *host, int port, const char *cred,
                                  const char *store_path, oc_secret *secret);

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

/* Open a thread on `parent_id` (requests its replies), post a reply to it, or
 * close the thread view. Replies stream into the model's thread buffer. */
void oc_client_open_thread(oc_client *c, uint64_t channel_id, uint64_t parent_id);
void oc_client_reply(oc_client *c, uint64_t channel_id, uint64_t parent_id, const char *body);
void oc_client_close_thread(oc_client *c);

/* Run a full-text search (results stream into the model's search buffer) / close
 * the search view. */
void oc_client_search(oc_client *c, const char *query);
void oc_client_close_search(oc_client *c);

/* Channel management: create a public channel, join / leave one. The server
 * answers with a CHANNEL_INFO that folds into the channel list. */
void oc_client_create_channel(oc_client *c, const char *name);
void oc_client_join_channel(oc_client *c, uint64_t channel_id);
void oc_client_leave_channel(oc_client *c, uint64_t channel_id);
/* Refresh the channel list (to discover channels created since login). */
void oc_client_list_channels(oc_client *c);

/* Refresh the tenant roster; set your own presence (OC_PRESENCE_ONLINE/_AWAY);
 * toggle the roster overlay (frontend view state). */
void oc_client_list_users(oc_client *c);
void oc_client_set_presence(oc_client *c, uint8_t status);
void oc_client_toggle_roster(oc_client *c, int open);

/* Open (or get) a 1:1 DM with `user_id`; the server answers with a CHANNEL_INFO
 * that carries the peer, so the DM titles itself from the roster. */
void oc_client_open_dm(oc_client *c, uint64_t user_id);

/* Inspect who reacted to a message (REQ-071): the reactors stream into the
 * model's reactor list (a "who reacted" overlay) / close that overlay. */
void oc_client_list_reactions(oc_client *c, uint64_t channel_id, uint64_t message_id);
void oc_client_close_reactions(oc_client *c);

/* Notification preferences (REQ-130/131). Set a channel's level
 * (OC_NOTIFY_ALL/_MENTIONS/_NONE) or the do-not-disturb window (minutes since
 * local midnight; enabled=0 clears it) — the server answers with a NOTIFY_PREFS
 * sync that folds into the channels + the model DND fields. Toggle the prefs
 * overlay (frontend view state; opening refreshes the sync). */
void oc_client_set_notify_pref(oc_client *c, uint64_t channel_id, uint8_t level);
void oc_client_set_dnd(oc_client *c, uint8_t enabled, uint16_t start_min, uint16_t end_min);
void oc_client_list_notify_prefs(oc_client *c);
void oc_client_toggle_prefs(oc_client *c, int open);

/* Admin / user management (REQ-030/033; owner/admin only, enforced server-side).
 * Set a user's tenant role (OC_ROLE_MEMBER/_ADMIN/_OWNER), mint a tenant invite
 * token for a role (the server answers with an INVITE_CREATED, shown once in the
 * model), or remove/disable a user. Each folds a USER_UPDATED into the roster. */
void oc_client_set_role(oc_client *c, uint64_t user_id, uint8_t role);
void oc_client_invite_user(oc_client *c, uint8_t role);
void oc_client_remove_user(oc_client *c, uint64_t user_id);

/* Incoming-webhook management (REQ-170). Open the webhook overlay for a channel
 * (refreshes the list), close it, mint a webhook (the server answers with a
 * WEBHOOK_INFO whose token is shown once in the model), or delete one by id. */
void oc_client_webhooks(oc_client *c, uint64_t channel_id);
void oc_client_close_webhooks(oc_client *c);
void oc_client_create_webhook(oc_client *c, uint64_t channel_id, const char *label);
void oc_client_delete_webhook(oc_client *c, uint64_t webhook_id);

/* Attachments (REQ-140/141). Upload a local file and post it to `channel_id`
 * (the core streams it, then links it into a message); download an attachment by
 * id to `dest_path`. Progress + completion arrive as OC_EV_XFER status lines. */
void oc_client_upload(oc_client *c, uint64_t channel_id, const char *path);
void oc_client_download(oc_client *c, uint64_t attachment_id, const char *dest_path);

/* Log out: revoke this session (scope OC_LOGOUT_THIS) or all of the user's
 * (OC_LOGOUT_ALL); the server closes the connection. */
void oc_client_logout(oc_client *c, uint8_t scope);

/* Stop the network thread, drain remaining events, and free everything. */
void oc_client_stop(oc_client *c);

#endif /* OC_CLIENT_H */
