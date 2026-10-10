/*
 * OpenChime — in-app feedback: what the client says about what just happened,
 * and about the state it is in, inside its own window (REQ-263).
 *
 * Each kind of message has one surface, decided by what it is rather than by
 * who raised it:
 *
 *   a confirmation (copied, saved)   a toast that leaves by itself, said politely
 *   something undoable (deleted)     a toast with one action, Undo, that stays
 *                                    longer and while it is pointed at
 *   progress (sending a video)       one toast, updated in place, ending as a
 *                                    confirmation or a failure
 *   a failure with nothing to mark   a toast that stays until it is dismissed,
 *                                    said assertively -- a failure that leaves
 *                                    by itself is a failure nobody saw
 *   an ongoing state (offline)       a banner, the most severe on top, gone when
 *                                    the state ends
 *
 * A hint about a field, or about what the composer holds, is inline at that
 * field -- the frontend's own, not here. So is a failure that has an item to
 * mark (a message that did not send).
 *
 * This is the model only: what is showing, until when, and what was last
 * announced. Time is passed in, so it is tested without a clock, and a frontend
 * draws it however it draws. Not thread-safe: the UI thread's.
 */

#ifndef OC_FEEDBACK_H
#define OC_FEEDBACK_H

#include <stdint.h>

/* NOTICE: something good that is ready for the person to act on (a summary
 * made while they were away): shown as a confirmation, but it stays until it
 * is acted on or dismissed. */
enum { OC_FB_CONFIRM = 0, OC_FB_UNDO, OC_FB_PROGRESS, OC_FB_FAILED, OC_FB_NOTICE };
enum { OC_FB_INFO = 0, OC_FB_WARN, OC_FB_ERROR };      /* a banner's severity */

#define OC_FB_TOASTS   3        /* on screen at once */
#define OC_FB_BANNERS  8
#define OC_FB_TEXT   256

typedef struct {
    uint32_t id;
    int      kind;                  /* OC_FB_CONFIRM..FAILED */
    char     text[OC_FB_TEXT];
    char     action[32];            /* its one action's label, "" for none */
    int      action_id;             /* the frontend's, handed back when it is used */
    uint64_t deadline_ms;           /* 0: stays until dismissed */
    uint64_t held_left_ms;          /* while held: the time it had left */
    int      held;
} oc_fb_toast;

typedef struct {
    int      id;                    /* the frontend's: one per state it can be in */
    int      severity;
    char     text[OC_FB_TEXT];
    char     action[32];
    int      action_id;
    uint32_t seq;                   /* when it was last set, for "most recent" among equals */
} oc_fb_banner;

typedef struct oc_fb {
    oc_fb_toast  t[OC_FB_TOASTS];   /* oldest first */
    int          n;
    uint32_t     next_id;
    oc_fb_banner b[OC_FB_BANNERS];
    int          nb;
    uint32_t     banner_seq;
    /* Every toast, and every banner as it changes, is spoken: politely, or
     * assertively for a failure or an error banner. NULL: not spoken. */
    void       (*say)(const char *text, int assertive);
} oc_fb;

void oc_fb_init(oc_fb *f, void (*say)(const char *text, int assertive));

/* Show a toast; its id. The same kind and text already showing is that toast
 * again: its time restarts and it is said again, rather than stacking. When
 * full, the oldest confirmation goes first, then the oldest of any kind but
 * progress. `action` NULL or "" for none. */
uint32_t oc_fb_show(oc_fb *f, int kind, const char *text, const char *action, int action_id, uint64_t now_ms);

/* Progress moved on, or ended: the toast `id` now says `text` as `kind` (a
 * finished progress becomes a confirmation or a failure). 0 if it is gone. */
int  oc_fb_update(oc_fb *f, uint32_t id, int kind, const char *text, uint64_t now_ms);

void oc_fb_dismiss(oc_fb *f, uint32_t id);

/* The pointer or the keyboard is on toast `id` (0: on none): its time stands
 * still until it leaves, and then it has at least a little left to finish
 * reading. */
void oc_fb_hold(oc_fb *f, uint32_t id, uint64_t now_ms);

/* Let the time pass: expired toasts go. 1 if anything changed. */
int  oc_fb_tick(oc_fb *f, uint64_t now_ms);

const oc_fb_toast *oc_fb_find(const oc_fb *f, uint32_t id);

/* How long a toast of `kind` saying `text` stays: a confirmation long enough to
 * read (4 s, plus some for a longer sentence, up to 10 s), Undo 10 s, progress,
 * failures and notices until they end, are acted on, or are dismissed (0). */
uint64_t oc_fb_duration_ms(int kind, const char *text);

/* Banners: set (or change) state `id`, or clear it. The one showing is the
 * most severe, the most recently set among equals; NULL for none. */
void oc_fb_banner_set(oc_fb *f, int id, int severity, const char *text, const char *action, int action_id);
void oc_fb_banner_clear(oc_fb *f, int id);
const oc_fb_banner *oc_fb_banner_top(const oc_fb *f);

#endif /* OC_FEEDBACK_H */
