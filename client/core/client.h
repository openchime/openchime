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
/* Page BACKWARDS through a channel (WIN-16): the newest page strictly older
 * than `before_message_id`. Replies arrive as ordinary messages, so the model
 * folds them in exactly like a forward replay. */
void oc_client_history(oc_client *c, uint64_t channel_id, uint64_t before_message_id);

/* Channel membership (REQ-033, WIN-31). Both frames have always existed on the
 * wire; nothing exposed them. */
void oc_client_channel_invite(oc_client *c, uint64_t channel_id, uint64_t user_id);
void oc_client_channel_kick(oc_client *c, uint64_t channel_id, uint64_t user_id);

/* Signup (WIN-32, REQ-268): redeem an invite on this connection instead of
 * authenticating. Call immediately after a start_* with `cred` = "user:password"
 * — the pair the new account will have. */
void oc_client_redeem_invite(oc_client *c, const char *invite_token);

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
void oc_client_open_search(oc_client *c);
void oc_client_search(oc_client *c, const char *query);
void oc_client_close_search(oc_client *c);

/* Channel management: create a public channel, join / leave one. The server
 * answers with a CHANNEL_INFO that folds into the channel list. */
void oc_client_create_channel(oc_client *c, const char *name);          /* public */
void oc_client_create_channel_ex(oc_client *c, const char *name, int is_public);
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
/* Pin or unpin a message (REQ-230, ARCH-90). Any member may do either, so a
 * frontend does not need to check who placed the pin. op is OC_PIN_ADD/REMOVE;
 * the resulting PIN_UPDATED reaches every member and folds into the model. */
void oc_client_pin(oc_client *c, uint64_t channel_id, uint64_t message_id, uint8_t op);

/* Open / close a channel's pins list. The entries carry their bodies, so the
 * list renders without those messages being in loaded history. */
void oc_client_list_pins(oc_client *c, uint64_t channel_id);
void oc_client_close_pins(oc_client *c);

/* A channel's OWN member roster (REQ-031) — not the tenant roster, which is
 * oc_client_list_users. Showing the latter beside a channel name was wrong the
 * moment a workspace held more people than one channel did. */
/* Change a channel (REQ-034/035/036, ARCH-93): op is OC_CHUP_TOPIC / _RENAME /
 * _ARCHIVE / _UNARCHIVE, `value` the new topic or name (ignored for archive).
 * Any member may set a topic; rename and archive are owner/admin and the daemon
 * enforces that — a frontend may hide the item, but must not rely on hiding it.
 * The result arrives as a CHANNEL_INFO fanned to every member. */
void oc_client_update_channel(oc_client *c, uint64_t channel_id, uint8_t op, const char *value);

void oc_client_list_members(oc_client *c, uint64_t channel_id);

/* Files shared in a channel, or (channel_id 0) across every channel the user
 * can read (REQ-143, ARCH-91). */
void oc_client_list_files(oc_client *c, uint64_t channel_id);
void oc_client_close_files(oc_client *c);

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

/* Synced client settings (the daemon-side config bucket). Identify this
 * frontend's bucket (default "tui"; call once at startup before listing), then
 * upsert a key (empty value deletes) or request the whole bucket — the server
 * answers with a CLIENT_SETTINGS snapshot that folds into the model (read with
 * oc_model_setting) and syncs to the user's other same-type devices. */
void oc_client_set_client_type(oc_client *c, const char *client_type);
void oc_client_set_setting(oc_client *c, const char *key, const char *value);
void oc_client_list_settings(oc_client *c);

/* Storage usage report (REQ-214; owner/admin only — the daemon refuses it for a
 * member). The answer folds into the model as `storage`/`storage_have`. Toggle
 * the overlay is frontend view state. */
void oc_client_storage_status(oc_client *c);
void oc_client_toggle_storage(oc_client *c, int open);

/* Audit log (REQ-251; owner/admin only). Request a page ending before
 * `before_ms` (0 = newest); entries fold into the model, newest first. */
void oc_client_audit_query(oc_client *c, uint64_t before_ms);
void oc_client_toggle_audit(oc_client *c, int open);

/* Self-service profile (REQ-020). Change your own display name (fans to every
 * roster) or rotate your local password (the server verifies `old_pw`). Each
 * folds a PROFILE_UPDATED into the model on success; a failure (e.g. wrong old
 * password) surfaces as a status/error line. */
void oc_client_set_display_name(oc_client *c, const char *name);
void oc_client_change_password(oc_client *c, const char *old_pw, const char *new_pw);

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

/* Fetch an attachment INTO MEMORY (WIN-17): the bytes arrive as an
 * OC_EV_ATTACHMENT_DATA the frontend consumes, with no file written anywhere
 * (ARCH-88). For inline images; anything over the core's inline cap is dropped
 * rather than buffered, so a caller should fall back to a real download. */
void oc_client_fetch_attachment(oc_client *c, uint64_t attachment_id);

/* Log out: revoke this session (scope OC_LOGOUT_THIS) or all of the user's
 * (OC_LOGOUT_ALL); the server closes the connection. */
void oc_client_logout(oc_client *c, uint8_t scope);

/* Force an immediate reconnect if the client is currently backing off after a
 * dropped connection (otherwise a no-op). */
void oc_client_reconnect(oc_client *c);

/* How many sends are queued but unacknowledged (REQ-102). The outbox lives in
 * memory only (ARCH-88), so a frontend should warn before quitting while this is
 * non-zero — otherwise the user silently loses what they typed. */
int oc_client_outbox_pending(oc_client *c);

/* Stop the network thread, drain remaining events, and free everything. */
void oc_client_stop(oc_client *c);

#endif /* OC_CLIENT_H */
