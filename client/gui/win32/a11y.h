/*
 * OpenChime Win32 — the accessibility surface (REQ-269, ARCH-99).
 *
 * The client draws its own UI, so nothing about it is legible to a screen reader
 * unless we say so. This is the seam that says so: a UI Automation provider,
 * served from a SNAPSHOT that the paint pass publishes.
 *
 * Why a snapshot rather than letting the provider read the model: the paint pass
 * already computes every rectangle it hit-tests against, and an accessible tree
 * is a description of what was *drawn*. Deriving it a second time from the model
 * would produce two descriptions of one layout, which drift — and the drift is
 * invisible until somebody who depends on it cannot use the app. Same reason the
 * test harness reads hit-boxes the app reports rather than measuring pixels.
 *
 * It also keeps this file honest: a11y.c is a separate translation unit and
 * cannot see winmain.c's statics, so the only thing it can serve is what it was
 * given.
 */
#ifndef OC_A11Y_H
#define OC_A11Y_H

#include <windows.h>
#include <stdint.h>

typedef enum {
    OC_ACC_CONVERSATION = 0,   /* a sidebar row: channel or DM */
    OC_ACC_MESSAGE,            /* one message in the transcript */
    OC_ACC_COMPOSER            /* the message box (exactly one) */
} oc_acc_kind;

enum { OC_ACC_NAME_MAX = 320, OC_ACC_MAX = 800 };

/* Rects are DEVICE PIXELS, client-relative — the units the paint pass already
 * has after PX(). Keeping the DPI conversion on the publisher's side means this
 * file never needs to know about scaling, and there is one place that can be
 * wrong about it instead of two. */
typedef struct {
    oc_acc_kind kind;
    uint64_t    id;                      /* channel id / message id */
    int         l, t, r, b;
    char        name[OC_ACC_NAME_MAX];   /* UTF-8, what a screen reader speaks */
} oc_acc_item;

/* Lifecycle. init() resolves the provider entry points; everything below is a
 * no-op when they are absent, so a Windows without UIAutomationCore.dll simply
 * gets the app as it was. */
void oc_a11y_init(HWND hwnd);
void oc_a11y_shutdown(void);
int  oc_a11y_available(void);

/* WM_GETOBJECT. Returns 0 when it did not handle it, so the caller falls through
 * to DefWindowProc. */
LRESULT oc_a11y_get_object(HWND hwnd, WPARAM wp, LPARAM lp, int *handled);

/* Publish what was just drawn. `composer` is the composer's text (UTF-16, may be
 * NULL), with the caret/anchor as UTF-16 offsets, so the composer element can
 * answer text and selection queries. */
void oc_a11y_publish(const oc_acc_item *items, int n,
                     const WCHAR *composer, int caret, int anchor);

/* Speak something that is not a focus change: an arriving message, a send that
 * failed, a connection that dropped. */
void oc_a11y_announce(const char *utf8);

/* How many announcements have been raised — the harness's only handle on a path
 * whose effect is audible and therefore unobservable here (the same honest limit
 * as the tray balloon in WIN-18). */
unsigned oc_a11y_announced(void);

/* Focus moved to a published item, so a screen reader follows the app rather
 * than being told only what changed. */
void oc_a11y_focus(oc_acc_kind kind, uint64_t id);

#endif /* OC_A11Y_H */
