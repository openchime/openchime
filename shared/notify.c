/* When is a user quiet? (REQ-136, ARCH-103.) See notify.h. */

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
