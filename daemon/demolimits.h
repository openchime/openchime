/* A demo workspace's daily limits (OPENCHIME_MODE=demo, AUTH.md §8.12): what
 * each visitor, and the workspace as a whole, may do in a UTC day, so a public
 * demo stays usable. Everything works; past a
 * limit the thing is refused with OC_ERR_DEMO_LIMIT and a sentence saying so.
 *
 * Counted in memory on the event loop's thread, which owns it: a restart
 * forgets the day, and the nightly reset forgets everything anyway. */
#ifndef OC_DEMOLIMITS_H
#define OC_DEMOLIMITS_H

#include <stdint.h>

typedef enum {
    OC_DL_MESSAGE = 0,   /* a message or a reply */
    OC_DL_UPLOAD,        /* a file sent */
    OC_DL_CALL,          /* a call joined or started */
    OC_DL_INVITE,        /* an invitation made */
    OC_DL_SUMMARY,       /* a summary asked for */
    OC_DL_PUSH,          /* a push notification sent (the workspace's only) */
    OC_DL_MAIL,          /* an invitation mailed (the workspace's only) */
    OC_DL_N
} oc_dl_kind;

/* The bounds a demo's limits do not count, but cap. */
#define OC_DEMO_MAX_UPLOAD   (10u * 1024u * 1024u)    /* bytes in one file */
#define OC_DEMO_CALL_MAX_MS  (20u * 60u * 1000u)      /* how long one call may last */

typedef struct oc_dl oc_dl;

oc_dl *oc_dl_new(void);
void   oc_dl_free(oc_dl *d);

/* Count one `k` by `user` (0: the workspace's alone) at `now_ms`. 1 when it is
 * within both the person's and the workspace's limit for the day, and counted;
 * 0 when not, and nothing counted. */
int oc_dl_take(oc_dl *d, oc_dl_kind k, uint64_t user, uint64_t now_ms);

/* The sentence a refusal of `k` carries. */
const char *oc_dl_why(oc_dl_kind k);

/* The limits, for the docs' table and the tests: per person and per workspace
 * (0: none of that kind). */
void oc_dl_limits(oc_dl_kind k, unsigned *per_user, unsigned *per_workspace);

#endif
