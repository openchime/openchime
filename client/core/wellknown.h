/*
 * OpenChime client — `.well-known` discovery metadata (REQ-010, REQ-011).
 *
 * REQ-010 resolves a full domain "via SRV records plus optional `.well-known`
 * metadata"; this is that second half. SRV is still what answers first — the
 * port precedence is **SRV record > `.well-known` `port` > 443** (ARCH-54,
 * PROTOCOL.md §1.1) — so the document is fetched only where SRV said nothing,
 * which is the deployment that can publish a web document but not an SRV record.
 *
 * WHAT THE REQUIREMENTS FIX, AND WHAT THEY DO NOT. They name the document and
 * two of its fields: a `port` (ARCH-54) and, optionally, the daemon's
 * certificate fingerprint for out-of-band verification (ARCH-10). They do not
 * state the transport, the media type, or the path. Those are decided here:
 *
 *   - **HTTPS at `/.well-known/openchime`**, because `.well-known` is an HTTPS
 *     path by construction (RFC 8615) and the name in the requirement is the
 *     only thing it could mean.
 *   - **JSON**, an object of scalars, because the project already speaks it on
 *     its other HTTP surfaces and vendors a parser for them.
 *   - **CA-verified**, against the roots built into the client (tls.h), and
 *     refused rather than downgraded when the chain does not reach one. This
 *     is the one that deserves the argument: ARCH-10 keeps the client off CA
 *     trust for the DAEMON connection, which is pinned (TOFU). That is a
 *     statement about the daemon's self-signed certificate, not about an
 *     ordinary web server at the tenant's domain, and it cannot be stretched
 *     to cover this: a document that may carry the fingerprint a client is
 *     about to pin is worth nothing if anyone on the path may write it. So the
 *     fetch verifies a chain, and where it does not verify, there is no
 *     metadata — the resolution falls back to 443. The feature is optional in
 *     the requirement, and this is what optional means in practice.
 *
 * A document that is present but not what it claims to be is a DISTINCT failure
 * (REQ-011: "malformed `.well-known` metadata"), told apart from "there is no
 * document" so the caller can say which happened.
 */

#ifndef OC_WELLKNOWN_H
#define OC_WELLKNOWN_H

#include <stddef.h>

/* The path the document is served from, relative to the workspace domain.
 * Overridable at compile time only so the fetch can be exercised against a
 * server that exists; nothing in the product changes it. */
#ifndef OC_WK_PATH
#define OC_WK_PATH "/.well-known/openchime"
#endif
/* The most we will read of it. A discovery document is a handful of scalars;
 * anything larger is either not one or is not worth blocking a sign-in on. */
#define OC_WK_MAX 4096u
/* Long enough for a SHA-256 fingerprint written as hex, with or without the
 * colons people paste it with. */
#define OC_WK_FP_MAX 96u

typedef struct {
    int  port;                        /* 0 when the document does not say */
    char fingerprint[OC_WK_FP_MAX];   /* "" when it does not say (ARCH-10) */
} oc_wellknown;

enum {
    OC_WK_OK = 0,
    OC_WK_NONE = -1,        /* no document, or no way to verify one */
    OC_WK_MALFORMED = -2    /* there is one, and it is not this (REQ-011) */
};

/* Parse the document. Unknown keys are ignored — a newer daemon may publish
 * more than this client reads — but a key this client DOES know, carrying the
 * wrong type or an impossible value, is malformed rather than absent. */
int oc_wellknown_parse(const char *doc, size_t len, oc_wellknown *out);

/* The same answer, from a whole HTTP response rather than from a body: 200 with
 * a document is OC_WK_OK, any other status is OC_WK_NONE (a 404 or an error page
 * does not claim to be this), and a 200 whose body is not the document is
 * OC_WK_MALFORMED. Exposed because a server is a poor place to keep a parser's
 * edge cases. */
int oc_wellknown_read_response(const char *resp, size_t len, oc_wellknown *out);

/* Fetch and parse `https://<domain>/.well-known/openchime`. The server must
 * present a chain to the built-in roots (tls.h), or the answer is OC_WK_NONE.
 * Blocking, and bounded by its own deadline, because a sign-in waits on it. */
int oc_wellknown_fetch(const char *domain, oc_wellknown *out);

/* The fingerprint as BYTES: 32 of them, from 64 hex digits, with or without the
 * colons people paste between them. Returns 0 on success, -1 if it is not that
 * -- and a fingerprint that cannot be read is not a weaker check, so the caller
 * treats it as no fingerprint at all rather than as a pin it half-understood. */
int oc_wellknown_fingerprint_bytes(const char *hex, unsigned char out[32]);

#endif /* OC_WELLKNOWN_H */
