/*
 * Small POSIX-vs-Windows portability shims for the client frontends (ARCH-81):
 * the couple of libc calls whose signatures differ. Threads live in
 * oc_thread.h; sockets in sock.h. This is just the leftovers.
 */

#ifndef OC_PORT_H
#define OC_PORT_H

#include <time.h>

#ifdef _WIN32
#  include <direct.h>
   /* mingw's mkdir takes no mode argument. */
#  define oc_mkdir(path) _mkdir(path)
#  define OC_PATH_SEP '\\'
   /* localtime_s has reversed argument order and returns errno_t. */
static inline struct tm *oc_localtime_r(const time_t *t, struct tm *out) {
    return localtime_s(out, t) == 0 ? out : (struct tm *)0;
}
   /* nanosleep -> Sleep(ms). Rounds up so a sub-ms request still yields. */
#  include <windows.h>
static inline int oc_nanosleep(long ns) {
    Sleep((DWORD)((ns + 999999L) / 1000000L));
    return 0;
}
#else
#  include <sys/stat.h>
#  define oc_mkdir(path) mkdir((path), 0700)
#  define OC_PATH_SEP '/'
#  define oc_localtime_r(t, out) localtime_r((t), (out))
   static inline int oc_nanosleep(long ns) {
       struct timespec ts = { ns / 1000000000L, ns % 1000000000L };
       return nanosleep(&ts, (struct timespec *)0);
   }
#endif

/* Minutes east of UTC, right now. The daemon stores this per user because a
 * per-weekday quiet-hours schedule has to be evaluated on the user's own
 * calendar day and only the client side knows the zone (ARCH-103). Computed
 * from the difference the C library already knows about rather than parsed from
 * a zone name, so it needs no tzdata and follows daylight saving by itself.
 *
 * Lives here, beside the localtime shim it is built on, because both the
 * app-core (which sends it on connect) and a frontend (which sends it with a
 * schedule edit) need it, and they had no other header in common. */
static inline int oc_utc_offset_min(void) {
    time_t now = time((time_t *)0);
    struct tm lt, gt;
    if (!oc_localtime_r(&now, &lt)) return 0;
#ifdef _WIN32
    if (gmtime_s(&gt, &now) != 0) return 0;
#else
    if (!gmtime_r(&now, &gt)) return 0;
#endif
    int diff = (lt.tm_hour * 60 + lt.tm_min) - (gt.tm_hour * 60 + gt.tm_min);
    int dday = lt.tm_yday - gt.tm_yday;
    /* A year boundary makes tm_yday jump the wrong way (e.g. 0 - 364); only the
     * sign of the day difference is meaningful, not its size. */
    if (dday ==  1 || dday < -1) diff += 1440;
    if (dday == -1 || dday >  1) diff -= 1440;
    return diff;
}

#endif /* OC_PORT_H */
