/* Enrollment client (ARCH-84, the daemon side of central's
 * enrollment handshake). On first boot the daemon generates a P-256 keypair + an
 * opaque audience id, persists them, and prints an "oce1." enrollment code for an
 * operator to enter on central's enrollment page. It then calls out to central
 * (CA-verified HTTPS) to prove possession of the private key and activate the
 * binding. Central never dials the daemon (ARCH-56); the daemon only ever calls
 * out, and only for its own enrollment function (ARCH-26 is about *config*, which
 * this is not). See docs/details/AUTH.md §3.6.
 */
#ifndef OPENCHIME_ENROLL_H
#define OPENCHIME_ENROLL_H

#include <stddef.h>

/* Generate a fresh ECDSA P-256 keypair and an opaque audience id ("ws_" + base64url
 * of >=128 random bits). Writes the private key as PEM into privkey_pem and the
 * audience into audience. Returns 0 on success, -1 on failure. */
int oc_enroll_generate(char *privkey_pem, size_t privkey_cap,
                       char *audience, size_t audience_cap);

/* Build the "oce1." enrollment code the operator enters on central's enrollment page, from a
 * private key PEM + audience: oce1.<base64url(JSON {"aud":..,"pk":<base64 SPKI DER>})>.
 * Returns 0 on success, -1 on failure. */
int oc_enroll_build_code(const char *privkey_pem, const char *audience,
                         char *code, size_t code_cap);

/* Sign the proof-of-possession: SHA-256("openchime-enroll-v1|<aud>|<nonce>")
 * signed with the private key (ASN.1 DER ECDSA), base64 (standard) into sig_b64.
 * This is exactly the value central verifies against the public key. Returns 0 on
 * success, -1 on failure. Exposed for testing. */
int oc_enroll_sign_proof(const char *privkey_pem, const char *audience, const char *nonce,
                         char *sig_b64, size_t sig_cap);

/* The address proof (AUTH.md §3.6): central, asked by the daemon to record the
 * address it is reached at -- its DNS name, or with none, the address the report
 * came from -- POSTs {"nonce": <base64url of 32 random bytes>} to /enroll-proof at
 * that address, and the daemon answers with its enrollment key's signature over
 *   openchime-address-proof-v1|<aud>|<nonce>
 * (ASN.1 DER ECDSA, base64), so central knows the server there holds the key. */
#define OC_ADDRESS_PROOF_PREFIX "openchime-address-proof-v1"
#define OC_ADDRESS_PROOF_PATH   "/enroll-proof"
#define OC_ADDRESS_REPORT_PATH  "/api/machine/address"
#define OC_ADDRESS_NONCE_LEN    43u   /* base64url, no padding, of 32 bytes */
int oc_enroll_sign_address_proof(const char *privkey_pem, const char *audience, const char *nonce,
                                 char *sig_b64, size_t sig_cap);
/* 1 when `nonce` (len bytes, not NUL-terminated) is the shape central sends:
 * OC_ADDRESS_NONCE_LEN base64url characters. */
int oc_enroll_nonce_ok(const char *nonce, size_t len);
/* The body of this box's address report to central (POST /api/machine/address).
 * With a DNS name, {"address":"<host>[:<port>]"}: OPENCHIME_MANAGED_ADDRESS when
 * set, else the first name it is certified for (OPENCHIME_TLS=acme:<names>),
 * lower-cased, with ":<port>" unless the port is 443. With neither, {}: central
 * then proves the address the report came from, on 443, and the daemon sends no
 * port. 0, or -1 for a name that is set but is not a usable DNS name, or an
 * `out` too small. */
int oc_enroll_address_body(const char *workspace_address, const char *tls_names, int port,
                           char *out, size_t cap);

typedef enum {
    OC_ENROLL_ACTIVE  =  0,  /* central acked activation */
    OC_ENROLL_PENDING =  1,  /* not yet reserved by the operator — retry later */
    OC_ENROLL_FAILED  = -1
} oc_enroll_result;

/* Run the outbound challenge/confirm against central to activate the binding.
 * central_url is e.g. "https://central.example[:port]"; HTTPS is verified against
 * the built-in roots (tls.h). Returns an oc_enroll_result. */
oc_enroll_result oc_enroll_activate(const char *central_url, const char *audience,
                                    const char *privkey_pem);

/* A managed box's claim (AUTH.md §8.7). `ticket_b64url` is the one-time ticket
 * the box was started with. Writes the SubjectPublicKeyInfo DER and the ASN.1 DER
 * signature, both base64, over
 *   openchime-claim-v1|<aud>|<b64url(SHA-256(ticket))>|<b64url(SHA-256(public key DER))>
 * so the signature covers the ticket and the key without carrying either. 0 / -1. */
int oc_enroll_sign_claim(const char *privkey_pem, const char *audience, const char *ticket_b64url,
                         char *pubkey_b64, size_t pubkey_cap, char *sig_b64, size_t sig_cap);

/* Claim the binding central reserved for `audience`. ACTIVE when central
 * activated it; FAILED when it refused the ticket (spent, expired, or not this
 * workspace's — retrying cannot help); PENDING when central could not be reached
 * or was busy, which is worth another try. */
oc_enroll_result oc_enroll_claim(const char *central_url, const char *audience,
                                 const char *privkey_pem, const char *ticket_b64url);

#endif /* OPENCHIME_ENROLL_H */
