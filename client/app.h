/*
 * OpenChime client — application/view state (ARCH-62), deliberately raylib-free
 * so it stays testable and the rendering backend is swappable. The UI thread
 * owns this; it drains net→UI events into it each frame and pushes user actions
 * to the net thread.
 */

#ifndef OC_APP_H
#define OC_APP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "queue.h"

typedef struct { char *body; uint64_t author_id; uint64_t message_id; } oc_msg;

typedef struct {
    bool      connected;
    uint64_t  user_id;
    oc_msg   *msgs;
    size_t    n_msgs, cap_msgs;
    uint64_t  high_water;          /* per-default-channel dedup mark (ARCH-45) */
    char      composer[512];
    int       composer_len;
    char      status[160];
    float     scroll;
    oc_queue *to_net;              /* UI -> net commands */
    oc_queue *from_net;            /* net -> UI events */
} oc_app;

void oc_app_init(oc_app *a, oc_queue *to_net, oc_queue *from_net);
void oc_app_free(oc_app *a);
void oc_app_drain(oc_app *a);      /* apply queued net events; call each frame */
void oc_app_input_char(oc_app *a, int codepoint);
void oc_app_backspace(oc_app *a);
void oc_app_submit(oc_app *a);     /* send the composer as an OC_CMD_SEND */
void oc_app_scroll(oc_app *a, float delta);

#endif /* OC_APP_H */
