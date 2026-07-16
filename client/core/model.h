/*
 * OpenChime client — the view-model + reducers (ARCH-74). Frontend-agnostic C:
 * a frontend owns an oc_model, folds net→UI events into it each tick with
 * oc_model_apply, and renders it. All logic/state lives here so a frontend is
 * pure view + input. Single-threaded on the frontend, so no locking.
 */

#ifndef OC_MODEL_H
#define OC_MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "event.h"

/* One emoji's aggregate on a message: the running count and whether we reacted. */
typedef struct { char emoji[40]; uint32_t count; uint8_t mine; } oc_reaction;

typedef struct {
    char    *body;         /* heap */
    char     author_name[64]; /* author display name ("" = fall back to id) */
    uint64_t author_id;
    uint64_t message_id;
    uint64_t server_time;
    oc_reaction *reactions;   /* heap, NULL until the message gets a reaction */
    uint8_t      n_reactions, cap_reactions;
} oc_msg;

typedef struct {
    uint64_t channel_id;
    char    *name;         /* heap; NULL until a CHANNEL_LIST entry names it */
    oc_msg  *msgs;
    size_t   n_msgs, cap_msgs;
    uint64_t high_water;   /* dedup mark: ignore message_id <= this (ARCH-45) */
    uint64_t read_marker;  /* high_water as of the last mark-read; drives unread */
    int      unread;       /* messages from others since the last mark-read */
    uint8_t  joined;
    uint8_t  history_requested; /* a backfill has been asked for (once per open) */
} oc_channel;

typedef struct { uint64_t user_id; uint8_t status; } oc_presence_row;

typedef struct {
    bool     connected;
    bool     authed;
    uint64_t user_id;                 /* our own id, from AUTH_OK */
    oc_channel      *channels;
    size_t           n_channels, cap_channels;
    oc_presence_row *presence;
    size_t           n_presence, cap_presence;
    char     status[160];             /* last status / error line */
} oc_model;

void oc_model_init(oc_model *m);
void oc_model_free(oc_model *m);

/* Fold one net event into the model. Takes ownership of `e->body` (moves it into
 * the model and clears it); the caller still frees the event struct itself. */
void oc_model_apply(oc_model *m, oc_ev *e);

/* Find a channel by id, or NULL. */
oc_channel *oc_model_channel(oc_model *m, uint64_t channel_id);
/* Clear a channel's unread count and advance its read marker to high_water. */
void oc_model_mark_read(oc_model *m, uint64_t channel_id);
/* A user's presence (OC_PRESENCE_OFFLINE if unknown). */
uint8_t oc_model_presence_of(const oc_model *m, uint64_t user_id);

#endif /* OC_MODEL_H */
