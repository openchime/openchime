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
#  define oc_localtime_r(t, out) localtime_r((t), (out))
   static inline int oc_nanosleep(long ns) {
       struct timespec ts = { ns / 1000000000L, ns % 1000000000L };
       return nanosleep(&ts, (struct timespec *)0);
   }
#endif

#endif /* OC_PORT_H */
