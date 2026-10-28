/* Bare-address detection (MARKDOWN.md §4) — where a URL starts and, harder,
 * where it ends. One implementation linked by BOTH sides, for ARCH-89's reason
 * arriving a third time (ARCH-105): the client autolinks these addresses and
 * the daemon unfurls them, and two answers to "where does the address end"
 * drift invisibly until an unfurl sits under a link that renders differently.
 *
 * ONLY http and https. A link span is what a frontend hands to the OS, so the
 * scheme list is the set of things a message can ask a reader's machine to
 * open; the daemon's fetcher inherits the same list for the same reason.
 *
 * No allocation; everything is offsets into the caller's bytes. */

#ifndef OC_URL_H
#define OC_URL_H

#include <stddef.h>

/* The length of the address starting at `i`, or 0 if none starts there.
 * `end` bounds the scan (exclusive); the opening-boundary check looks back
 * from `i` toward 0 unbounded, which is what lets `*https://x.com*` link
 * (the run of delimiters is looked through, exactly as the formatting
 * parser's own boundary rule does). Applies the trailing-trim and
 * bracket-balance rules of MARKDOWN.md §4. */
size_t oc_url_len(const char *b, size_t end, size_t i);

typedef struct { size_t start, len; } oc_url_span;

/* Every address in `b[0..len)`, in order, up to `max` — the daemon-facing
 * whole-body walk (ARCH-105). Skips what the formatting parser would render
 * literally, so a URL inside `inline code` or a ```fenced block``` is not
 * extracted, matching what a client links. Returns the number found (never
 * more than `max`; the walk stops at the cap). */
size_t oc_url_extract(const char *b, size_t len, oc_url_span *out, size_t max);

/* The host and port of a URL's authority -- `host`, `host:port`, `[v6]` or
 * `[v6]:port`, `len` bytes of it -- for the daemon's outbound HTTP (push,
 * enrolment, invitation mail, S3, link previews). The host comes back without
 * brackets, as getaddrinfo wants it; the port is left as the caller set it when
 * none is given. 0, or -1 when the authority is malformed or does not fit. */
int oc_url_authority(const char *auth, size_t len, char *host, size_t hcap, char *port, size_t pcap);
/* Whether the daemon may send to `url`: `https://` with a well-formed authority,
 * or plain `http://` only to loopback (127.0.0.1, ::1, localhost) -- a test's
 * fake. Anything else would carry signed requests and their answers in the
 * clear. 1 or 0. */
int oc_url_transport_ok(const char *url);
/* A Host header's value: `host`, bracketed when it is an IPv6 address, then
 * `:port` unless `port` is NULL or empty. 0, or -1 if it does not fit. */
int oc_url_hostheader(const char *host, const char *port, char *out, size_t cap);
/* The percent-decoded value of `key` in a query string or a form body
 * (application/x-www-form-urlencoded; '+' is a space). 1 found, 0 absent, -1
 * malformed or too long for `cap`. */
int oc_query_get(const char *query, const char *key, char *out, size_t cap);

#endif /* OC_URL_H */
