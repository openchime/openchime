/*
 * Device codes (AUTH.md §8.11, RFC 8628): a client with no browser of its own --
 * the terminal client over SSH -- asks for a code, shows it, and polls; the
 * person enters it on the daemon's /device page, on any device, signs in there
 * and approves; the page's sign-in mints the ID token, bound to the client's
 * PKCE challenge, and the next poll hands it over.
 *
 * The pending requests, in the event loop's memory: bounded in all and per
 * source, ten minutes each, dropped by a restart. The loop's alone -- no locks.
 */
#ifndef OPENCHIME_DEVICECODES_H
#define OPENCHIME_DEVICECODES_H

#include <stddef.h>
#include <stdint.h>

#define OC_DEVICE_TTL_MS      (10u * 60u * 1000u)
#define OC_DEVICE_INTERVAL_S  5u
#define OC_DEVICE_MAX         256
#define OC_DEVICE_PER_SOURCE  5
#define OC_DEVICE_CODE_LEN    43    /* base64url of 32 random bytes: the client's secret */
#define OC_USER_CODE_LEN      9     /* "XXXX-XXXX": what the person types */

typedef struct oc_devcodes oc_devcodes;

oc_devcodes *oc_devcodes_new(void);
void         oc_devcodes_free(oc_devcodes *d);
/* A test's knob: how long a request lives. 0 restores OC_DEVICE_TTL_MS. */
void         oc_devcodes_set_ttl(oc_devcodes *d, uint64_t ms);
/* ...and the interval a client is asked to poll at, in seconds. 0 restores
 * OC_DEVICE_INTERVAL_S. */
void         oc_devcodes_set_interval(oc_devcodes *d, unsigned s);
unsigned     oc_devcodes_interval(const oc_devcodes *d);

/* A new request from `source` (its limiter key, oc_source_key) at `addr`, bound
 * to `challenge`: the secret the client polls with and the code the person
 * types. 0; -1 when the table is full; -2 when this source already has
 * OC_DEVICE_PER_SOURCE pending. */
int oc_devcodes_begin(oc_devcodes *d, const char *source, const char *addr, const char *challenge,
                      uint64_t now_ms, char device_code[OC_DEVICE_CODE_LEN + 1],
                      char user_code[OC_USER_CODE_LEN + 1]);

typedef enum {
    OC_DEV_PENDING,   /* not approved yet */
    OC_DEV_SLOW,      /* polled sooner than the interval: it is now longer */
    OC_DEV_GONE,      /* no such request, expired, or already collected */
    OC_DEV_DENIED,    /* refused on the page; the request is gone */
    OC_DEV_TOKEN      /* approved: *token is the caller's to free; the request is gone */
} oc_dev_poll;

/* A poll by the client's secret. `interval_s` gets the interval to keep to. */
oc_dev_poll oc_devcodes_poll(oc_devcodes *d, const char *device_code, uint64_t now_ms,
                             char **token, unsigned *interval_s);

/* What the page shows for a code a person typed -- upper or lower case, with or
 * without its dash -- if it is pending. */
typedef struct {
    char     user_code[OC_USER_CODE_LEN + 1];
    char     challenge[48];
    char     addr[46];
    uint64_t created_ms;
} oc_dev_info;
int oc_devcodes_find(oc_devcodes *d, const char *typed, uint64_t now_ms, oc_dev_info *out);

/* The page's answer for a pending code: approved with the token the sign-in
 * minted (copied), or denied. 0, or -1 if it is no longer pending. */
int oc_devcodes_approve(oc_devcodes *d, const char *user_code, const char *token, uint64_t now_ms);
int oc_devcodes_deny(oc_devcodes *d, const char *user_code, uint64_t now_ms);

/* The code a person typed, as stored: upper case, "XXXX-XXXX". 0, or -1 if it
 * cannot be one. */
int oc_user_code_normalise(const char *typed, char out[OC_USER_CODE_LEN + 1]);

#endif /* OPENCHIME_DEVICECODES_H */
