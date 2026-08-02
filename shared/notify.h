/*
 * When is a user quiet? (REQ-136, ARCH-103.)
 *
 * In shared/ for the reason the mention scanner is (ARCH-89): the DAEMON decides
 * whether to send a notification and the CLIENT decides whether to show itself as
 * quiet, and two implementations of the same rule disagree in a way neither side
 * can see. The Win32 client had grown two copies of the old one-window version
 * already — one for the tray, one for the header — and its own comment said that
 * if the rule ever grew it belonged here. It grew.
 */

#ifndef OC_NOTIFY_H
#define OC_NOTIFY_H

/* Is `now_min` inside the daily [start, end) range, wrapping past midnight when
 * start > end? An empty range (start == end) contains nothing. */
int oc_notify_in_window(int start_min, int end_min, int now_min);

/* Quiet under the schedule. `mode` is OC_DND_* (protocol.h); the window is the
 * hours notifications are ALLOWED, so quiet is everything outside it — the
 * opposite sense from the window REQ-131 stored, which is why those columns were
 * renamed rather than reused.
 *
 * `local_min` and `weekday` (0 = Sunday) are the USER'S, never the server's: a
 * per-weekday window against a UTC day puts a large part of the world's Friday
 * evening on Saturday.
 *
 * `day_present` distinguishes "this weekday has no row" from "its row says off",
 * because in custom mode both are quiet but only one is a thing the user set.
 */
int oc_notify_quiet(int mode, int base_start, int base_end,
                    int day_present, int day_enabled, int day_start, int day_end,
                    int local_min, int weekday);

#endif /* OC_NOTIFY_H */
