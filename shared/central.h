/*
 * Where a workspace reaches central (docs/details/AUTH.md §3): every path the
 * daemon calls the OpenChime API at, under the API's workspace prefix. The
 * origin is OPENCHIME_ENROLL_URL's; the paths are joined to it here and nowhere
 * else, so no other file spells one.
 */

#ifndef OC_CENTRAL_H
#define OC_CENTRAL_H

#define OC_CENTRAL_WORKSPACE      "/v1/workspace"

/* Enrollment (AUTH.md §3.1). */
#define OC_CENTRAL_ENROLL_CLAIM     OC_CENTRAL_WORKSPACE "/machine/enroll/claim"
#define OC_CENTRAL_ENROLL_CHALLENGE OC_CENTRAL_WORKSPACE "/machine/enroll/challenge"
#define OC_CENTRAL_ENROLL_CONFIRM   OC_CENTRAL_WORKSPACE "/machine/enroll/confirm"

/* Signed machine requests: each signs the method and this path. */
#define OC_CENTRAL_ADDRESS        OC_CENTRAL_WORKSPACE "/machine/address"
#define OC_CENTRAL_INVITE_NOTIFY  OC_CENTRAL_WORKSPACE "/machine/invite/notify"
#define OC_CENTRAL_PUSH_NOTIFY    OC_CENTRAL_WORKSPACE "/machine/push/notify"
#define OC_CENTRAL_TLS_NAMES      OC_CENTRAL_WORKSPACE "/machine/tls/names"
#define OC_CENTRAL_TLS_CERTIFICATE OC_CENTRAL_WORKSPACE "/machine/tls/certificate"
/* The invitations central queued for this workspace (ARCH-85): taken on a
 * lease, then reported, made or refused. */
#define OC_CENTRAL_INVITATIONS_TAKE   OC_CENTRAL_WORKSPACE "/machine/invitations/take"
#define OC_CENTRAL_INVITATIONS_REPORT OC_CENTRAL_WORKSPACE "/machine/invitations/report"

/* A build's release notes, followed by the build id; a signed GET. */
#define OC_CENTRAL_RELEASE_BUILD  OC_CENTRAL_WORKSPACE "/releases/build/"

/* The sign-in relay: its issuer is the origin followed by this, its keys at
 * <issuer>/jwks, a browser sent to <issuer>/authorize. */
#define OC_CENTRAL_OIDC           OC_CENTRAL_WORKSPACE "/oidc"

#endif
