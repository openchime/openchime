/* Federated enrollment client (CP-8, the daemon side of the control plane's
 * enrollment handshake). On first boot the daemon generates a P-256 keypair + an
 * opaque audience id, persists them, and prints an "oce1." enrollment code for an
 * operator to courier into the control-plane console. It then calls out to central
 * (CA-verified HTTPS) to prove possession of the private key and activate the
 * binding. Central never dials the daemon (ARCH-56); the daemon only ever calls
 * out, and only for its own enrollment function (ARCH-26 is about *config*, which
 * this is not). See docs/AUTH.md §3.6.
 */
#ifndef OPENCHIME_ENROLL_H
#define OPENCHIME_ENROLL_H

#include <stddef.h>

/* Generate a fresh ECDSA P-256 keypair and an opaque audience id ("ws_" + base64url
 * of >=128 random bits). Writes the private key as PEM into privkey_pem and the
 * audience into audience. Returns 0 on success, -1 on failure. */
int oc_enroll_generate(char *privkey_pem, size_t privkey_cap,
                       char *audience, size_t audience_cap);

/* Build the "oce1." enrollment code the operator couriers into the console, from a
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

typedef enum {
    OC_ENROLL_ACTIVE  =  0,  /* central acked activation */
    OC_ENROLL_PENDING =  1,  /* not yet reserved by the operator — retry later */
    OC_ENROLL_FAILED  = -1
} oc_enroll_result;

/* Run the outbound challenge/confirm against central to activate the binding.
 * central_url is e.g. "https://central.example[:port]"; ca_bundle may be NULL to
 * probe the system CA locations. Returns an oc_enroll_result. */
oc_enroll_result oc_enroll_activate(const char *central_url, const char *ca_bundle,
                                    const char *audience, const char *privkey_pem);

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
oc_enroll_result oc_enroll_claim(const char *central_url, const char *ca_bundle,
                                 const char *audience, const char *privkey_pem,
                                 const char *ticket_b64url);

#endif /* OPENCHIME_ENROLL_H */
