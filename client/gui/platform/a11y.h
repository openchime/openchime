/* OpenChime GUI -- the accessibility provider (REQ-269, ARCH-99).
 *
 * The application publishes what is on screen as a flat list of items with
 * their rects, names and what invoking them does; the platform exposes that
 * to assistive technology its own way (UIA on Windows, AT-SPI on Linux,
 * NSAccessibility on the Mac, ARIA on the web) and hands an invoke back as an
 * OC_PLAT_EV_A11Y_INVOKE event. The provider cannot see the application's
 * state, so the only thing it can serve is what it was given. */
#ifndef OC_A11Y_H
#define OC_A11Y_H

#include <stdint.h>
#include <SDL3/SDL.h>

typedef enum {
    OC_ACC_CONVERSATION = 0,   /* a sidebar row: channel or DM */
    OC_ACC_MESSAGE,            /* one message in the transcript */
    OC_ACC_COMPOSER,           /* the message box (exactly one) */
    OC_ACC_BUTTON,
    OC_ACC_TAB,                /* a tab or filter chip: one of a set, selectable */
    OC_ACC_LISTITEM            /* a row in a pane's list (threads, people, drafts) */
} oc_acc_kind;

enum { OC_ACC_NAME_MAX = 320, OC_ACC_MAX = 800, OC_ACC_AID_MAX = 64 };

typedef struct {
    oc_acc_kind kind;
    uint64_t    id;                      /* channel id / message id */
    int         l, t, r, b;              /* device pixels, window-relative */
    char        name[OC_ACC_NAME_MAX];   /* UTF-8, what a screen reader speaks */
    char        aid[OC_ACC_AID_MAX];     /* a stable automation id */
    uint64_t    invoke;                  /* what pressing it does; 0 = nothing */
    int         layer;
} oc_acc_item;

void oc_a11y_init(SDL_Window *w);
void oc_a11y_shutdown(void);
int  oc_a11y_available(void);
/* The tree, replaced whole; `composer` is the message box's text in UTF-16
 * with the caret and the selection's anchor as unit offsets. */
void oc_a11y_publish(const oc_acc_item *items, int n,
                     const uint16_t *composer, int caret, int anchor);
void oc_a11y_announce(const char *utf8);
void oc_a11y_announce_assertive(const char *utf8);
unsigned oc_a11y_announced(void);
void oc_a11y_focus(oc_acc_kind kind, uint64_t id);

#endif /* OC_A11Y_H */
