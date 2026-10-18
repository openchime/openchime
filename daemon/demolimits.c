/* A demo workspace's daily limits (demolimits.h). */
#include "demolimits.h"

#include <stdlib.h>
#include <string.h>

static const struct { unsigned user, ws; const char *why; } LIMITS[OC_DL_N] = {
    [OC_DL_MESSAGE] = { 200, 5000, "This is a public demo: each person may post 200 messages a day, and the workspace 5,000." },
    [OC_DL_UPLOAD]  = {  20,  500, "This is a public demo: each person may share 20 files a day, of up to 10 MB each." },
    [OC_DL_CALL]    = {  30,  500, "This is a public demo: each person may join 30 calls a day, of up to 20 minutes each." },
    [OC_DL_INVITE]  = {  10,   50, "This is a public demo: invitations are limited to 10 a day each." },
    [OC_DL_SUMMARY] = {   5,  100, "This is a public demo: each person may ask for 5 summaries a day." },
    [OC_DL_PUSH]    = {   0, 5000, "This is a public demo: notifications are limited for the day." },
    [OC_DL_MAIL]    = {   0,   20, "This is a public demo: invitations are mailed up to 20 a day." },
};

#define DL_USERS 2048u   /* people counted in a day; one past it is counted as the workspace only */

struct oc_dl {
    uint64_t day;                               /* UTC days since the epoch the counts are for */
    unsigned ws[OC_DL_N];
    struct { uint64_t user; unsigned n[OC_DL_N]; } u[DL_USERS];
};

oc_dl *oc_dl_new(void) { return calloc(1, sizeof(oc_dl)); }
void   oc_dl_free(oc_dl *d) { free(d); }

const char *oc_dl_why(oc_dl_kind k) { return k < OC_DL_N ? LIMITS[k].why : ""; }

void oc_dl_limits(oc_dl_kind k, unsigned *per_user, unsigned *per_workspace) {
    *per_user = k < OC_DL_N ? LIMITS[k].user : 0;
    *per_workspace = k < OC_DL_N ? LIMITS[k].ws : 0;
}

/* The person's counts, found or made by open addressing; NULL when the table
 * is full or for the workspace's own (user 0). */
static unsigned *user_counts(oc_dl *d, uint64_t user) {
    if (!user) return NULL;
    size_t i = (size_t)((user * 0x9E3779B97F4A7C15ull) >> 40) % DL_USERS;
    for (size_t probe = 0; probe < DL_USERS; probe++, i = (i + 1) % DL_USERS) {
        if (d->u[i].user == user) return d->u[i].n;
        if (!d->u[i].user) { d->u[i].user = user; return d->u[i].n; }
    }
    return NULL;
}

int oc_dl_take(oc_dl *d, oc_dl_kind k, uint64_t user, uint64_t now_ms) {
    if (!d || k >= OC_DL_N) return 1;
    uint64_t day = now_ms / 86400000ull;
    if (day != d->day) { memset(d, 0, sizeof *d); d->day = day; }   /* a new day: nothing counted */
    if (LIMITS[k].ws && d->ws[k] >= LIMITS[k].ws) return 0;
    unsigned *n = LIMITS[k].user ? user_counts(d, user) : NULL;
    if (n && n[k] >= LIMITS[k].user) return 0;
    d->ws[k]++;
    if (n) n[k]++;
    return 1;
}
