/*
 * When is a user quiet, and does a message notify? (REQ-135, REQ-136, ARCH-103.)
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

/* The notify decision itself: does THIS message notify THIS person? One
 * boolean, from every input that can veto or force it, in the one order the
 * product means (REQ-134, REQ-135, REQ-137):
 *
 *   your own message never notifies; a muted conversation never notifies, and
 *   nothing pierces mute; a priority person pierces everything else — the
 *   level, the schedule, the pause — because those say WHEN and a priority
 *   person is a WHO; then the schedule (REQ-136) and the pause (REQ-278)
 *   silence the rest; then the level: ALL passes, NONE passes nothing, and
 *   MENTIONS passes an @-mention or a keyword hit, which REQ-135 makes part
 *   of the level rather than a separate switch.
 *
 * In shared/ for ARCH-89's reason, taken one step further: the daemon's push
 * query and each client's toast gate answer the SAME question, and those
 * copies had already drifted — keywords, priority people and mute were being
 * honoured by push and ignored by the desktop toast. Callers compute the
 * inputs from whatever they hold (a client from its model, the daemon from
 * its rows). The daemon's push query still states this same order in SQL —
 * one test suite pins both against each other — and teaching it to fetch
 * rows and ask this function instead is the intended next step, not a
 * divergence to keep.
 *
 * `level` is OC_NOTIFY_* (protocol.h), already resolved against the user's
 * global default (REQ-134): the fallback is the caller's to apply, because
 * only the caller knows whether a per-channel preference exists. */
int oc_notify_decide(int own, int muted, int vip, unsigned level,
                     int mentioned, int keyword_hit, int quiet, int paused);

#endif /* OC_NOTIFY_H */
