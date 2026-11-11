/*
 * OpenChime client — one HTTPS GET, verified against the built-in roots.
 *
 * What the `.well-known` fetch (wellknown.c) and the release notes
 * (releasenotes.c) share: dial, TLS handshake against the roots compiled into
 * the client (tls.h) -- refused, not downgraded, when the chain does not reach
 * one -- an HTTP/1.0 GET, and the response read to the connection's close.
 * HTTP/1.0 deliberately: a 1.1 server may answer chunked, whose framing would
 * arrive inside the body; 1.0 has no chunked encoding to choose. Blocking, and
 * bounded by its own deadline.
 */
#ifndef OC_HTTPS_GET_H
#define OC_HTTPS_GET_H

#include <stddef.h>

/* GET https://<host><path> on port 443, sending `accept` as Accept. The whole
 * response -- status line, headers and body -- goes into `out`, NUL-terminated,
 * at most `cap` - 1 bytes (the rest is not read). Returns how many bytes were
 * read, or -1 when there was no verified connection, or no answer within
 * `deadline_ms` of the start. */
long oc_https_get(const char *host, const char *path, const char *accept, int deadline_ms,
                  char *out, size_t cap);

/* The status of an HTTP response in `resp` (`len` bytes): 200, 404..., or 0 when
 * it does not start as one. *body is set past its headers, or NULL when they
 * never end. */
int oc_http_response_status(const char *resp, size_t len, const char **body);

#endif /* OC_HTTPS_GET_H */
