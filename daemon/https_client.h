/*
 * One outbound HTTPS request, blocking, for the daemon's certificate machinery
 * (ACME, RFC 8555; and certificates through central, AUTH.md §8.9): any method,
 * any request headers, and the response's status, headers and whole body --
 * ACME is read from its headers (Replay-Nonce, Location, Retry-After) as much
 * as from its bodies, which the older single-purpose clients here discard.
 *
 * HTTPS is verified against the built-in roots and OPENCHIME_EXTRA_CA with the
 * host's name checked (tls.h, oc_tls_client_init_ca). Plain http:// is refused
 * except to a loopback address, which is a test's fake server and nothing else.
 */
#ifndef OPENCHIME_HTTPS_CLIENT_H
#define OPENCHIME_HTTPS_CLIENT_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
    int    status;          /* the HTTP status, 0 if none was read */
    char  *head;            /* the status line and headers, NUL-terminated */
    char  *body;            /* the body, de-chunked, NUL-terminated */
    size_t body_len;
} oc_https_resp;

/* Send `method` `url` with `body` (may be NULL) of type `ctype` (may be NULL),
 * plus `extra` header lines (each ending "\r\n"; may be NULL), and read the
 * whole response. Each address the host has gets OC_CONNECT_PER_ADDR_MS to
 * connect and the exchange `timeout_ms` for each read. Returns 0 with `r`
 * filled (free it with oc_https_resp_free), or -1 with `err` saying why. */
int oc_https_request(const char *method, const char *url, const char *ctype,
                     const void *body, size_t body_len, const char *extra,
                     int timeout_ms, oc_https_resp *r, char *err, size_t errcap);

/* The value of response header `name` (case-insensitive), trimmed, into `out`;
 * 1 if present, 0 if not. */
int oc_https_header(const oc_https_resp *r, const char *name, char *out, size_t cap);

void oc_https_resp_free(oc_https_resp *r);

/* GET `url` into the file at `path` (created or truncated), following up to
 * five redirects, refusing more than `max_bytes`, hashing what is written:
 * its SHA-256 into `sha256`. For files too big to hold in memory (the summary
 * model, ARCH-116). `stop` (may be NULL), once set, ends the download. 0, or -1
 * with `err` saying why. */
int oc_https_download(const char *url, const char *path, uint64_t max_bytes, int timeout_ms,
                      const volatile int *stop, unsigned char sha256[32], char *err, size_t errcap);

#endif /* OPENCHIME_HTTPS_CLIENT_H */
