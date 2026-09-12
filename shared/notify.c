/* When is a user quiet, and does a message notify? (REQ-135, REQ-136,
 * ARCH-103.) See notify.h. */

#include "notify.h"
#include "protocol.h"

int oc_notify_in_window(int start_min, int end_min, int now_min) {
    if (start_min == end_min) return 0;             /* empty window */
    if (start_min < end_min) return now_min >= start_min && now_min < end_min;
    return now_min >= start_min || now_min < end_min;   /* wraps past midnight */
}

int oc_notify_quiet(int mode, int base_start, int base_end,
                    int day_present, int day_enabled, int day_start, int day_end,
                    int local_min, int weekday) {
    if (mode == OC_DND_OFF) return 0;
    /* Weekdays: the weekend is quiet whatever the hour. This is the case a single
     * daily window could not express at all, and the reason REQ-136 replaced it. */
    if (mode == OC_DND_WEEKDAYS && (weekday == 0 || weekday == 6)) return 1;
    if (mode == OC_DND_CUSTOM) {
        /* No row for today is quiet, not always-on: a custom schedule listing
         * Monday to Friday is a statement about the weekend too. */
        if (!day_present || !day_enabled) return 1;
        return !oc_notify_in_window(day_start, day_end, local_min);
    }
    return !oc_notify_in_window(base_start, base_end, local_min);
}

int oc_notify_decide(int own, int muted, int vip, unsigned level,
                     int mentioned, int keyword_hit, int thread_reply,
                     int quiet, int paused) {
    if (own) return 0;              /* your own words are not news */
    if (muted) return 0;            /* mute is absolute: nothing pierces it (REQ-137) */
    if (vip) return 1;              /* a priority person pierces all of the below (REQ-135) */
    if (quiet || paused) return 0;  /* the schedule and the pause (REQ-136, REQ-278) */
    if (level == OC_NOTIFY_ALL) return 1;
    if (level == OC_NOTIFY_MENTIONS) return mentioned || keyword_hit || thread_reply;
    return 0;                       /* OC_NOTIFY_NONE, and anything unrecognised */
}
