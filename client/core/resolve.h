/*
 * OpenChime client — instance resolution (REQ-010/011, ARCH-14). Turns a
 * user-typed *instance* (a self-hosted domain like `chat.acme.com`, or a bare
 * hosted-tier name like `acme`) into a daemon `host:port` by **plain DNS**, with
 * no hosted resolution service: a bare name gets the configured DNS suffix
 * appended, then the domain is resolved via an SRV record
 * (`_openchime._tcp.<domain>`), falling back to the domain's A/AAAA record at
 * 443. Resolution failure is reported as a distinct status so the caller can tell
 * "this org doesn't exist" apart from "unreachable" and "login failed" (REQ-011).
 *
 * `.well-known` metadata (the optional half of REQ-010) is not consulted yet.
 */

#ifndef OC_RESOLVE_H
#define OC_RESOLVE_H

#include <stddef.h>

typedef enum {
    OC_RESOLVE_OK = 0,
    OC_RESOLVE_BAD_INSTANCE,   /* empty / malformed instance string */
    OC_RESOLVE_NOT_FOUND       /* the instance does not resolve in DNS (REQ-011) */
} oc_resolve_status;

typedef struct { char host[256]; int port; } oc_endpoint;

/* Normalize an instance to a DNS domain: strip a leading scheme (`x://`) and any
 * trailing `:port`/path; a bare name (no dot) gets `.<suffix>` appended when a
 * non-empty suffix is given (`acme` -> `acme.openchime.example`), while a name
 * that already contains a dot passes through. Returns 0 on success (domain in
 * `out`), -1 if the instance is empty or does not fit. */
int oc_resolve_domain(const char *instance, const char *suffix, char *out, size_t cap);

/* Parse a raw DNS SRV answer, choosing the lowest-priority (most-preferred)
 * target. Returns 0 and fills host/port, or -1 if the answer has no usable SRV
 * record. Exposed for testing with a canned answer (no live DNS). */
int oc_srv_parse(const unsigned char *answer, int len, char *host, size_t hostcap, int *port);

/* Resolve an instance to a daemon endpoint. `suffix` is the hosted-tier DNS
 * suffix for bare names (NULL/"" disables suffixing). See the header comment for
 * the SRV -> A fallback order and the distinct failure statuses. */
oc_resolve_status oc_resolve(const char *instance, const char *suffix, oc_endpoint *out);

#endif /* OC_RESOLVE_H */
