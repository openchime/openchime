/*
 * OpenChime — notifications from the operating system (REQ-138), one interface
 * for every platform.
 *
 * What a notification IS lives here, apart from how any one platform raises it:
 * who and what (title, body, source), which conversation it is about (`key`,
 * within the workspace `group`, so a second one for the same conversation
 * replaces the first and a read conversation's can be withdrawn), what clicking
 * it opens (`open_url`), the sound it asks for, and what can be done from it
 * (a reply box, buttons). WHETHER to notify is decided before this is reached
 * (shared/notify.c, ARCH-103); WHERE is the frontend's -- this is only the OS
 * surface, and a frontend falls back to its own when it is unavailable.
 *
 *   Windows   the Notification Center toast, then the tray balloon
 *             (client/shared/osnotify_win.c over client/gui/win32/wintoast.c)
 *   others    unavailable for now (client/shared/osnotify_null.c): nothing is
 *             raised, and the frontend's fallback and the badges carry on
 *
 * Every entry point fails soft: no platform guarantees a notification can be
 * raised (no identity, a denied permission, a missing service).
 */

#ifndef OC_OSNOTIFY_H
#define OC_OSNOTIFY_H

#include <stdint.h>

/* The sound a notification asks for, by meaning. The platform names its own
 * (Windows' notification events, a freedesktop sound name), so a person who
 * has changed or silenced their system's sounds is followed. */
enum { OC_OSN_SOUND_DEFAULT = 0, OC_OSN_SOUND_IM, OC_OSN_SOUND_MAIL, OC_OSN_SOUND_REMINDER,
       OC_OSN_SOUND_SILENT, OC_OSN_SOUND_COUNT };

/* What this platform's notifications can do. */
#define OC_OSN_CAP_ACTIONS  0x01u   /* buttons */
#define OC_OSN_CAP_REPLY    0x02u   /* a reply box */
#define OC_OSN_CAP_REPLACE  0x04u   /* a second for the same key replaces the first */
#define OC_OSN_CAP_WITHDRAW 0x08u   /* a raised one can be taken back */
#define OC_OSN_CAP_SOUND    0x10u   /* the platform plays the named sound */

typedef struct {
    const char *title, *body;
    const char *source;             /* where: a channel's name, NULL for a direct message */
    const char *key, *group;        /* the conversation, and its workspace */
    const char *open_url;           /* what a click on it opens */
    int         sound;              /* OC_OSN_SOUND_* */
    const char *reply_placeholder;  /* NULL: no reply box */
    const char *const *btn_label;   /* up to n_btn buttons, each handing back btn_arg[i] */
    const char *const *btn_arg;
    int         n_btn;
} oc_osn;

/* A button or the reply box, used: the button's argument, and the typed text
 * ("" when none). Called on whatever thread the platform delivers it on, so it
 * must hand the work to the UI thread rather than act. */
typedef void (*oc_osn_action_cb)(const char *arg, const char *reply);

/* Bind this process to `app_id`, as the installed application is known to the
 * platform (`display_name` where the platform shows one), and take actions
 * through `cb`. `window` is the platform's handle for the window notifications
 * belong to, NULL where there is none; on Windows it is the window that owns
 * the tray icon `tray_id`, which carries the balloon fallback. Returns 1 when
 * notifications can be raised. Safe to call more than once. */
int      oc_osn_init(const char *app_id, const char *display_name, void *window, unsigned tray_id,
                     oc_osn_action_cb cb);
int      oc_osn_available(void);
unsigned oc_osn_caps(void);

/* Raise `n`. OC_OSN_SHOWN when the platform raised it and plays its sound;
 * OC_OSN_SHOWN_SILENT when it was raised on a surface that cannot play one,
 * so the caller plays it; OC_OSN_NOT_SHOWN when the caller must fall back. */
enum { OC_OSN_NOT_SHOWN = 0, OC_OSN_SHOWN = 1, OC_OSN_SHOWN_SILENT = 2 };
int  oc_osn_show(const oc_osn *n);

/* Take back what was raised for `key` in `group`, or for all of `group`.
 * 1 when the platform took the request, 0 when it could not be asked. */
int  oc_osn_withdraw(const char *key, const char *group);
int  oc_osn_withdraw_group(const char *group);

/* Tests: behave as if the platform's own notification could not be raised, so
 * the fallback is reached through the real path (1), or as found (0). */
void oc_osn_test_unavailable(int on);

void oc_osn_done(void);

#endif /* OC_OSNOTIFY_H */
