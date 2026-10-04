# OpenChime — Authentication

How users prove who they are, and how a session is established and revoked.
This is the authoritative design; it is cross-referenced from ARCHITECTURE.md
(ARCH-19, ARCH-55–ARCH-60), REQUIREMENTS.md (§1.2, §8.1), PROTOCOL.md (§4), and
SCHEMA.md (migration 0002).

---

## 1. Identity sources, one session

A deployment proves identity through one or more **identity sources**, enabled
in its configuration (ARCH-55). They differ only in how identity is *proven*;
all then **converge on a daemon-issued session** (§4), so everything downstream
— SEND, backfill, reconnect, revocation — is identical whichever source was
used.

| Source | How identity is proven | Depends on |
|---|---|---|
| **Local** (§2) | The daemon manages accounts + passwords itself; the password is typed into the daemon's own sign-in page, in the browser (§8.10). | Nothing outside the box — fully air-gappable. |
| **Relay** (§3) | The client signs in with Google, Microsoft or a code mailed to their address through the project's central service, which re-issues a token the daemon trusts. | The project's central service, at login time only. |
| **Direct connection** (§8.5) | The client signs in at an OIDC provider the operator names, and the daemon is the relying party: it exchanges the code and checks the provider's ID token. | The operator's own provider, at login time only. No OpenChime-operated service. |

What a deployment may enable follows from the three deployment models of
ARCH-76, because the relay is one of the functions a deployment federates and
the other two sources need nothing from the project.

| Deployment model (ARCH-76) | Sources |
|---|---|
| **Self-hosted stand-alone** | **Local**, a **direct connection**, or both. No dependency on any OpenChime-operated service; on local accounts alone this model is fully air-gappable. |
| **Self-hosted federated** | Any of the three. Opting in to the federated OIDC function is what adds the relay; a federated deployment may equally decline it and federate only push, directory, SCIM, DNS, or packages. |
| **Hosted** | The **relay**, operated by the project alongside the daemons. |

The relay keeps the daemon lean on that path (it never fetches JWKS or handles
multiple providers — it verifies one JWT from one pinned key, §3.3) and means a
self-hoster never registers provider apps or holds provider credentials; its
price is a login-time dependency on the project, and the project seeing who
signs in where (§3.4). A direct connection is for the operator who would rather
hold those credentials than pay that price, and it is the only single sign-on a
stand-alone deployment can have. **SAML is not a source** (REQ-027).

**Sources may be enabled together** — an organization's provider for staff
beside local accounts for contractors or a break-glass owner.

A deployment's sources are set in the daemon's static config (ARCH-26) and
advertised to the client in-protocol via `AUTH_CHALLENGE` (§5).

---

## 2. Local authentication (ARCH-59)

For deployments that run no identity provider. The daemon is the identity
authority.

- **Credentials:** username + password. Passwords are hashed with
  **PBKDF2-HMAC-SHA256** (via mbedTLS `mbedtls_pkcs5_pbkdf2_hmac`) using a
  per-user random salt and a high iteration count (~600k, OWASP-tier). Only the
  derived hash + salt + iteration count are stored (`local_credentials` table).
  PBKDF2 was chosen over argon2/bcrypt to add **no new dependency** — mbedTLS is
  already linked and has no argon2. **No OpenChime client collects a password.**
  It is typed only into the daemon's own sign-in page, in the system browser, and
  reaches the daemon only inside TLS — directly, or through the client's loopback
  tunnel (§8.10). It is never stored. A password in a frame is refused, but for a
  test knob.
- **Bootstrapping the first owner:** the initial account (tenant **owner**) is
  created at first run from a one-time **setup token**. There is no config file
  and no configuration variable for it (ARCH-26): the daemon **mints** the token
  itself when a local-mode start finds no owner who can sign in, and prints it
  once to stderr. The owner is created with it on the sign-up page (§8.10). Only
  the newest token works — each start prints one, and a log keeps them all — and
  none works once an owner can sign in. This avoids the chicken-and-egg of "you
  need an admin to create the first admin" without requiring email
  (air-gapped-safe); and since a removed owner is no owner, a workspace whose
  owners were all removed gets a token again.
- **Adding users:** an owner/admin issues an **invite token**; the invitee
  creates the account — username and password — by presenting it on the sign-up
  page (§8.10). Email delivery is never required.
- **Resetting a password:** an owner or admin makes a one-time link for a local
  account (`RESET_CREDENTIAL`; who may reset whom is who may remove whom) and
  sends it to its person, who sets a new password on `/account/reset`: by its
  link where a CA vouches for the workspace's certificate, or by entering its
  code on the sign-in card, whose client opens the page through its own tunnel
  and carries a sign-in the page goes on to once the password is set. The link
  is kept as the SHA-256 of its token, is good for a day and once; using it
  stores the new password under a new version, signs the account out on every
  device, and — if the reset said so — turns its second step off (§8.6), for a
  person who lost their phone and their recovery codes. The new password is
  derived on the auth pool, with no old one to check. Issuing and using a link
  are both audited.
- **Registered-user cap:** the daemon honors `OPENCHIME_MAX_USERS` (0/unset
  = unlimited). Creating a *new* user past the cap — via invite redeem, direct
  register, bootstrap, or a first-time OIDC login — is refused with
  `ERROR USER_LIMIT`; an existing user still logs in, and a removed member
  (`disabled=1`) frees a seat (the count is active users only). This enforces the
  hosted plan's per-workspace seat limit, written into the box config at provision
  time; only the daemon can enforce it since only it holds the users table.
- **Brute-force protection (REQ-191):** the daemon rate-limits failed local-auth
  attempts per account **and** per source IP (`daemon/ratelimit.c`, fixed-window
  counters checked before PBKDF2 so a flood can't burn CPU), answering excess
  attempts with `ERROR AUTH_RATE_LIMITED`. The per-source cap is higher than
  per-account, so many users behind one NAT are tolerated while an account-spray
  from a single IP is still stopped; a successful login clears the account
  counter but not the source counter. The sign-in and password pages (§8.10)
  go through the same limiters, by the address the request came from — the
  browser's, or the client's through its tunnel — and a sign-up with an
  invitation that is not valid counts against its source.
- **Where the derivation runs:** not on the database writer. The writer checks
  the limiters, a reader fetches the stored credential and its version, a
  two-thread auth pool derives and compares, and the writer finishes the
  sign-in — session, audit — only if the credential still carries that version
  (SCHEMA.md migration 0047). Every write of a password takes a new version from
  a counter that only rises, so a password changed meanwhile, or an account
  removed and its id given to another, fails the sign-in (ARCH-5). A burst of
  sign-ins therefore does not hold up everyone's sends. Changing a password
  goes the same way — the pool checks the old password and derives the new one,
  and the writer stores the new one only if the credential still carries the
  version the old one was checked against; a change that lost that race is
  refused, and the password is whatever the change that won it set. A name
  with no account is checked the same way, against a credential no password
  matches, so how long a refusal takes says nothing of which names exist.
- **A connection that has not signed in is let go:** a failed `AUTH` is a fatal
  `ERROR` and the connection is closed, so another try is a new connection the
  limiters count afresh; and one not signed in within 60 seconds of being
  accepted (`OC_UNAUTHED_MS`) is closed, whether it is still in its TLS
  handshake or never said `HELLO`. A connection polling a device code that is
  still live (§8.11) is waiting, not idle: each answered poll starts its 60
  seconds again, so an approval that takes longer signs in on the connection
  that waited, and the code's own life bounds how long that can be.
  The per-source counter stands in front of **every** source: a refused relay
  token and a wrong session token count against the address they came from, the
  check runs before any signature work, and a refused relay sign-in is audited as
  `auth.failed` (or `auth.denied`, when the identity was valid and no rule admits
  it) with its reason and address — never the token. `auth.success` records the
  source and the provider. Behind a TCP forwarder the address is the client's,
  taken from the forwarder's PROXY protocol v2 header — believed only from the
  peers `OPENCHIME_TRUSTED_PROXIES` names (`daemon/proxyproto.c`), since anybody
  else who sent one could claim any address and walk past the limiter.

---

## 3. OIDC via the central service and relay (ARCH-56)

For social login (Google and Microsoft Entra, per REQ-021/022, and a code
mailed to the address) without
each operator registering provider apps.

### 3.1 The idea

The **central service** (maintainer-controlled) is the OIDC Relying Party: it
holds the Google and Microsoft client credentials, runs the login flow, and
**re-issues** an OpenChime **ES256 JWT** (§3.3) that the daemon trusts. For this
source all the OIDC machinery — JWKS fetching, provider quirks, key rotation,
multi-provider handling — lives in that service (a higher-level web service),
**not in the C daemon**. The daemon only verifies one JWT signature against one
configured, pinned public key (plus a tiny vendored JSON reader for the claims).

### 3.2 The flow — the client is the courier

The central service **never connects to a self-hosted daemon** — the client
carries the token from center to daemon. This preserves the island model
(ARCH-4): no central→daemon link.

**The client never talks to the provider directly, and never mints PKCE.** It
starts at central, and central runs the whole Authorization-Code-+-PKCE exchange
as the Relying Party. That is what makes the flow completable: the verifier stays
with the party that redeems the code, which is central. A client-minted challenge
would have to be handed over to be of any use, and there would be nothing to hand
it over on.

```
  ┌────────┐  1. GET central /oidc/authorize        ┌───────────────┐
  │ client │─────(workspace, redirect_uri)─────────▶│    central    │
  │ (app)  │                                        │    service    │
  └───┬────┘                                        └───────┬───────┘
      │                              2. redirect to provider │
      │                                 (central's client_id,│
      │                                  central's PKCE,     │
      │                                  state = {workspace, │
      │                                    client redirect}) │
      │                                        ┌─────────────┘
      │                                        ▼
      │                                  ┌──────────┐
      │                3. user logs in   │ provider │
      │                                  │ (Google) │
      │                                  └────┬─────┘
      │                    4. auth code ──────┘
      │                       to central's callback
      │                            │
      │  5. redirect to the client's own redirect_uri,
      │◀─────carrying the ES256 JWT (aud=<opaque ws id>)
      │
      │  6. AUTH{method=oidc, token}
      ▼
  ┌────────────────┐  verifies signature vs central's pinned key,
  │ daemon         │  checks audience==self + not expired,
  │ (acme.example) │  provisions/looks-up user, mints a session (§4)
  └────────────────┘
```

1. The client opens a browser at **central's** `/oidc/authorize`, naming the
   target **workspace** and its own `redirect_uri` — a loopback `127.0.0.1` URI
   on desktop (RFC 8252), or `[::1]` on a host with no IPv4 loopback, or a configured `https` destination.
2. **Central validates that `redirect_uri` before the user authenticates**,
   against an allowlist: loopback per RFC 8252, or exact-match https. Doing it
   at authorize rather than at callback is the point — an unvalidated
   destination is an open redirect that would carry a live token, and by
   callback the user has already logged in.
   Central then redirects to the provider with **its own** `client_id` and
   **its own** PKCE challenge, sealing the workspace and the client's
   `redirect_uri` into `state`.
3. The user authenticates at the provider.
4. The provider redirects to central's callback with an auth code.
5. Central exchanges the code (its own verifier plus its client secret),
   verifies the provider token, extracts identity (subject, email, name), and
   mints an **ES256 JWT scoped to that workspace** (`aud = <the workspace's
   opaque id>`, not its DNS name — see §3.3). It returns it by redirecting to
   the client's `redirect_uri`: **in the query for loopback**, and **in the
   fragment for a configured web destination**, so a browser destination never
   puts the token in a server log.
6. The client presents that token to the `acme.example` daemon in `AUTH`
   (method `oidc`). The daemon verifies it (§3.3) and mints a session.

The client half of this is the browser launch and the loopback listener.

### 3.3 The identity token — an ES256 JWT (ARCH-57)

Central issues a **standard JWT** signed with **ES256** (ECDSA P-256 + SHA-256),
carrying the identity claims:

```
{ "iss": "https://central.example",     // the central service — see below
  "aud": "ws_7f3a…9c21",                 // the target workspace's opaque id
  "sub": "<provider issuer>|<subject>",  // stable identity
  "email": "...", "email_verified": true, "name": "...",
  "idp": "google", "tenant": "acme.example",
  "nonce": "…", "jti": "…",
  "iat": ..., "nbf": ..., "exp": ... }
```

The claims and what each means are §8.3.

**The audience is an opaque random id — not the DNS name.** The `aud` value is a
random identifier (≥128-bit) established when the workspace **enrolls** with
central and recorded in central's workspace registry (§3.6); central mints a
token only for a **registered** audience. It is deliberately **decoupled from the
workspace address** — a workspace can change its domain or add a vanity name
(ARCH-14) without invalidating its OIDC identity, and the audience registry is
never consulted for discovery. (The enrollment
flow itself — how the id is proposed and bound — is a control-plane concern,
out of scope for this doc.)

**The issuer above is illustrative, not normative.** It is a string the daemon
compares with `strcmp` and never dereferences — there is no discovery document,
because the keys are pinned in configuration (§3.4). Any
value both sides agree on works, and an implementer should not read the example
as a URL that has to resolve, or as requiring a particular subdomain.

The daemon validates it by **pinning both the keys and the algorithm**
(`daemon/jwt.c`):

- it requires `alg = ES256` and rejects anything else — this closes JWT's classic
  footguns (`alg=none`, RS256/HS256 confusion) up front;
- it verifies the signature with mbedTLS (ES256 = ECDSA-P256, which mbedTLS
  supports directly; EdDSA/Ed25519 is not supported, so ES256 is the choice);
- it chooses the key by the header's `kid` — the signing key's RFC 7638
  thumbprint, which it computes for each key it holds — and refuses a token whose
  `kid` is absent or names none of them (§8.3). The keys it holds are the pinned
  ones and, on an enrolled box, the ones the relay publishes (§3.4);
- it checks `iss` (central) and `aud` (== this workspace's configured opaque id —
  not its hostname — so a token minted for one workspace cannot be replayed at
  another);
- it requires `sub`, `nonce`, `jti`, `iat` and `exp`, refuses a token that is
  expired, not yet valid, or whose life is longer than 300 seconds, and accepts a
  `jti` once — the ids of accepted tokens are held in memory until they expire;
- it JSON-unescapes claim strings and refuses an over-long claim rather than
  truncating it.

The JWT payload is JSON, so the daemon vendors a single-file JSON tokenizer
(**jsmn** — MIT, zero-allocation, ~300 lines, in the same spirit as
picohttpparser) to read the claims, plus a small base64url decoder. A bespoke
compact binary token was considered — to avoid the JSON parser — and rejected:
JWT is a standard, battle-tested format with well-understood mitigations, which
is exactly what a security-critical validation path wants; and the JSON cost is
negligible for a **once-per-login** token. (ARCH-6's "no JSON overhead" rule
targets the high-frequency message path, not the auth bootstrap.)

### 3.4 Trust setup and dependency

- The daemon config (ARCH-26) carries **central's public key** (bundled/pinned
  in the OpenChime distribution — central is maintainer-controlled and stable)
  and this workspace's **`audience` id** — the opaque registered id described in
  §3.3. A self-hoster enabling relay-OIDC enrolls their workspace with central
  once, after which central will mint tokens for that audience.
- **An enrolled box follows a rotation by itself.** It reads
  `<relay origin>/oidc/jwks` at boot and daily — every minute while it gets no
  good answer — over HTTPS verified with the built-in roots, and trusts the ES256
  P-256 signing keys there beside the pinned ones. Each answer replaces the last;
  none removes a pinned key, which is the floor whatever central publishes. So
  central rotates by publishing the next key, signing with it a day later, and
  dropping the old one after that, and no box's operator does anything. A box
  that is not enrolled — no `OPENCHIME_ENROLL_URL` — has only its pinned keys.
- **Dependency is login-time only.** Once the daemon issues a session (§4), it
  never contacts central again; existing sessions survive a central outage. Only
  *new logins* need central up, and the message path never does. Local mode has
  no central dependency at all — which is what makes self-hosted stand-alone
  (ARCH-76) possible.
- **Privacy tradeoff:** in relay-OIDC the central service sees *who* logs into
  which workspace (identities, not message content — it never touches
  messages/channels). A self-hoster wanting zero project visibility declines the
  relay and uses local accounts (§1); declining every federated function is exactly the self-hosted
  stand-alone model (ARCH-76).

### 3.5 Reconciling with the island model (REQ-041)

REQ-041 states that **a tenant's messages are stored only by its own daemon and
by default go nowhere else, in any deployment model** — and that holds here: data
isolation (REQ-040) is untouched. The central OIDC service is contacted at **login time only** (never
per-message), brokers **identity only** (it never sees message or channel
content), and is **absent entirely in local mode**, hence absent from every
self-hosted stand-alone deployment.

OIDC is not the only federated function, though — a self-hosted federated
deployment may also depend on the project for push (ARCH-16), the app directory
(REQ-175), SCIM (REQ-253), a DNS name and the workspace registry (ARCH-14), and
packages (ARCH-20). What unites them, and what REQ-041 actually guarantees, is
that each brokers identity, notification, discovery, or provisioning metadata
and **none carries message content**. So federating costs availability
independence and moves no message content; content leaves a daemon only toward
an endpoint the tenant itself configured (REQ-276, REQ-277).

### 3.6 The central service / relay (separate component)

The central service + relay are a **separate system** — a web service, not the C
daemon. Its contract with the daemon is narrow:

- run the Authorization-Code-+-PKCE flow against the configured providers;
- mint ES256 JWTs (§3.3) signed by the key the daemon pins, audience-scoped to
  the requesting workspace;
- maintain the workspace registry (which `audience` ids are valid), binding each
  workspace's opaque id at enrollment (§3.3) and minting a token only for a
  registered audience. This registry is part of the OIDC function and is never
  consulted for workspace discovery, which is plain DNS in every model (ARCH-14).

The daemon's OIDC path is tested with a **test issuer**:
generate an ECDSA-P256 keypair in the test, mint central-style ES256 JWTs, and
configure the daemon with the test public key — the same faking approach as the
existing TLS/netloop integration tests.

**The daemon's enrollment client (ARCH-84).** How a federated box *obtains* its
audience id is implemented daemon-side in `daemon/enroll.c`, gated on
`OPENCHIME_ENROLL_URL`. On first boot the daemon generates its own ECDSA-P256 keypair +
a random `audience` (`ws_…`), persists them (so they survive restarts), and prints
an `oce1.` **enrollment code** the operator couriers into the control-plane
console. The daemon then calls out (CA-verified HTTPS — it dials central, never the
reverse, ARCH-56) to prove possession of the private key and activate the binding.
The enrolled audience then feeds the OIDC configuration above (§3.4) automatically,
so an enrolled box need not be given `OPENCHIME_OIDC_AUDIENCE` by hand. The enrollment
*flow/registry* remains a control-plane concern; only the client half is here.
Two optional knobs support unattended bring-up (see [TESTING.md §6](./TESTING.md)):
`OPENCHIME_ENROLL_CODE_FILE` also writes the `oce1.` code to a file (so orchestration can
reserve it without scraping the log), and `OPENCHIME_ENROLL_WAIT_SECS` retries activation for
that many seconds before serving (default 0 = one attempt, retry next boot), so a box
can come up already-Active once the operator reserves the code.

**The daemon's push emitter (ARCH-85).** An enrolled box additionally set with
`OPENCHIME_PUSH_URL` (the control-plane push gateway) delivers
mobile push (REQ-132/133). The daemon owns a device-token registry
(`REGISTER_DEVICE_TOKEN`); a committed SEND drives an off-hot-path worker that selects
recipients (members − author, level=ALL, not in DND, holding a token), signs a
**contentless** batch with the enrollment key — the same request-signature scheme
central verifies (§8.7) — and POSTs it to
the gateway, which relays to APNs/FCM and returns stale tokens to prune. Absent in
self-hosted stand-alone (no enrollment / no `OPENCHIME_PUSH_URL`).

---

## 4. Sessions — the convergence point (ARCH-58)

However identity was proven (local password, OIDC token, or an existing session
token on reconnect), the daemon then does the same thing:

1. **Provision/look-up the user** (`users` table). A local user is keyed by
   `users.subject`, `local:<username>`; a person signing in by OIDC by their
   identities (§8.4), of which they may have several — one per way they sign in.
   OIDC users are provisioned just-in-time on first login, always with the
   schema's default role `member` — there is no bootstrap-subject setting, and
   promotion to owner or admin is a separate administrative action. Local users
   are created by invite (§2).
2. **Mint a session:** a random 32-byte token, returned to the client. The daemon
   stores only **`SHA-256(token)`** (so a database leak does not expose live
   sessions), with `user_id`, `created_at`, `expires_at`, `last_seen`, and an
   optional device label (`sessions` table).
3. **Return `AUTH_OK`** with the session token, its expiry, the user id, and the
   user's role.

- **Reconnect (REQ-100):** the client re-presents its session token
  (`AUTH{method=session}`); the daemon hashes and looks it up, and resumes
  without a full re-auth. The session lifetime is the daemon's to set (REQ-181) —
  it is not tied to a provider token's expiry.
- **Lifetime (REQ-181):** a session lives `OPENCHIME_SESSION_DAYS` (30) from
  its sign-in, and with `OPENCHIME_SESSION_IDLE_DAYS` set, one unused that long
  is refused — and deleted — at its next use. "Unused" is measured: a session's
  `last_seen_ms` is written at its sign-in and then, while its connection sends
  anything, at most hourly, in one batched writer job, so it costs nothing per
  message.
- **Revocation (REQ-182):** signing out deletes the session of this device
  alone, or of every device the user has (`LOGOUT`'s scope), or one other device
  picked from the sessions list (`REVOKE_SESSION`); a password change deletes
  them all. Each connection on a deleted session is closed at
  once — it is authenticated in memory, so the row alone would not stop it — and
  re-presenting the token fails. This local revocation is exactly what a stateless provider JWT cannot
  provide, and is the reason the daemon issues its own sessions.

---

## 5. Mode selection and the auth handshake

After `WELCOME` and before `AUTH`, the daemon sends **`AUTH_CHALLENGE`**
(PROTOCOL.md §4) listing the sources this deployment signs people in with
(`OPENCHIME_AUTH_MODE`: `local`, `relay`, or both). The client draws one control
per source and replies with `AUTH`, whose `method` discriminator selects the path:

- `local` — username + password in the frame. Refused (`AUTH_SOURCE_UNAVAILABLE`)
  unless the daemon runs with the test knob `OPENCHIME_TEST_PASSWORD_AUTH=1`;
  local accounts sign in in the browser (§8.10).
- `oidc` — a browser sign-in. `AUTH_BEGIN` gets the authorize URL from the daemon;
  `AUTH` then carries an ES256 JWT and the verifier (§8.2): the central-issued one
  (§3.3) for the relay, or, with source `local`, the daemon's own (§8.10).
- `session` — a previously issued session token (reconnect).

The daemon answers with `AUTH_OK` (session established) or an `ERROR`
(`AUTH_INVALID_TOKEN`, `AUTH_RATE_LIMITED`, `AUTH_NOT_ALLOWED`, `AUTH_REQUIRED`).
The whole exchange is §8.1.

---

## 6. Roles (ARCH-60)

Every user holds exactly one tenant-level role — `owner`, `admin`, or `member`
(REQ-030) — stored as a `role` column on `users`. Enforcement lives in the
DB-writer handlers (the single write path); the policy predicates are pure and
unit-tested in `daemon/roles.c`.

- **Role changes:** `SET_ROLE` applies the policy — only owner/admin
  may change roles, only an owner may grant/revoke owner, an admin may only
  promote/keep members — refusing with `FORBIDDEN`.
- **≥1 owner invariant:** demoting or removing the tenant's last owner is
  refused with `LAST_OWNER` (REQ-030), checked against a live `COUNT(*)` of owners
  who can sign in — a removed owner keeps the role on its row but counts for
  nothing.
- **Moderation delete (REQ-032):** an admin/owner who belongs to the
  channel may delete (not edit) others' messages; `process_delete` performs the
  tombstone after an `oc_role_can_moderate` check for a non-author and records it
  as a moderator deletion via `messages.deleted_by` (and in the audit log).
- **Invite/remove (REQ-033):** only owner/admin may invite or remove
  tenant members (`oc_role_can_manage_members`), and only an owner may invite at
  or remove an admin/owner. Invite mints a single-use token (`invites`);
  `REMOVE_USER` locks the member out via `users.disabled` (migration 0003) and
  revokes their sessions/credentials rather than deleting the row. Channel-level
  invite/remove for private channels is any member of that channel (PROTOCOL.md
  §5.7). Wire frames: PROTOCOL.md §5.8.

---

## 7. Excluded

Deliberate omissions from this design:

- **Email magic-link** local login: it needs outbound email and is not
  air-gapped-safe.
- **Argon2** password hashing: mbedTLS has none, so it would add a vendored
  dependency (§2).
- **Cert-vs-restore interaction** (a self-signed daemon's certificate changing
  when a database is restored onto a new box): handled by persisting the TLS
  identity in the database (ARCH-66b); orthogonal to auth.

---

## 8. The sign-in contract

One exchange serves every identity source of §1, so a client is written once and
adding a source changes no frame. This is the contract the daemon, the clients
and the central service each implement, stated once so the three cannot drift
apart.

### 8.1 The exchange

```
client                                            daemon
  | ---- HELLO ---------------------------------> |
  | <--- WELCOME, AUTH_CHALLENGE{sources} ------- |   each source: id, kind, label
  |                                               |
  |  every source signs in in the browser — local accounts on the
  |  daemon's own pages (§8.10), the relay at central (§3), a direct
  |  connection at the operator's provider (§8.5):
  | ---- AUTH_BEGIN{source, redirect_uri, challenge} -> |
  | <--- AUTH_REDIRECT{authorize_url} ----------- |   the daemon builds the whole URL
  |        … the person signs in in their browser; the client may disconnect …
  | ---- AUTH{oidc, source, verifier, what came back, state} -> |
  |                                               |
  | <--- AUTH_OK | ERROR ------------------------ |
```

- **`AUTH_CHALLENGE` lists sources** — `{id, kind, label}`, `kind` being `local`,
  `relay` or `oidc`, the last a direct connection with the operator's label. The
  client draws one control per source, with fixed text for local accounts and
  the relay.
  Resuming a session (§4) is always accepted and is not a listed source.
- **The daemon builds the authorize URL.** The client never assembles or parses an
  operator's or a provider's string, so it is the same client for the relay and
  for local accounts, and the workspace's audience reaches the relay from the one
  party that knows it. The client opens the URL only if it is `https` (plain
  `http` to loopback, for development).
- **`redirect_uri` is loopback** (RFC 8252); the daemon refuses anything else
  before it echoes it into a URL.
- **No connection is held open while the browser is.** Nothing between
  `AUTH_REDIRECT` and `AUTH` lives on the connection (§8.2), so the client may
  disconnect and an unauthenticated connection can be short-lived.
- **Errors a client can explain:** `AUTH_NOT_ALLOWED` (a valid identity that may
  not join, §8.4), `AUTH_SOURCE_UNAVAILABLE` (a provider that cannot be reached,
  or that this deployment does not offer), beside the codes that exist.
- **Protocol versions.** The daemon accepts the previous protocol version as
  well as its own, so a daemon upgrade is never a flag day for its clients.

### 8.2 Proof of possession

The client makes a random 32-byte **verifier** per attempt and sends
`challenge = base64url(SHA-256(verifier))` in `AUTH_BEGIN` — RFC 7636's
construction, used here between the client and the daemon.

- **Through the relay,** the daemon puts the challenge in the authorize URL as
  `nonce`, central copies it into the token it mints, and the daemon accepts a
  token only when it arrives with the verifier whose hash is that `nonce` — and
  only once: a token's `jti` is remembered until its `exp`.
- **Through a direct connection,** the daemon is the relying party, so it keeps
  the challenge with the sign-in it began (§8.5) and checks the verifier against
  it before the code goes anywhere. The provider sees a PKCE challenge of the
  daemon's own, never the client's.

A token or a code lifted from the loopback redirect, from browser history, or by
another account on a shared machine is therefore useless: whoever presents it
must also hold the verifier, which never left the client. The check survives the
client reconnecting between the two frames: the relay path needs no state in the
daemon, and a direct connection's is keyed by `state`, not by the connection.

### 8.3 The relay's token

Standard JWT, ES256, as §3.3. The claims:

| Claim | Meaning |
|---|---|
| `iss`, `aud` | Central, and this workspace's opaque id (§3.3). |
| `sub` | `<upstream issuer>|<stable subject>`. For Google the provider's `sub`. For Microsoft, `oid` under the tenant's issuer — `https://login.microsoftonline.com/<tid>/v2.0|<oid>` — not the per-application `sub`. |
| `idp` | Which provider vouched: `google`, `microsoft`, or `email` for a sign-in by a code central mailed to the address. |
| `tenant` | The organization the provider places the person in: Google's hosted domain, Microsoft's tenant id. Absent for a personal account and for an emailed code. |
| `email`, `email_verified`, `name` | As the provider gave them. `email_verified` is true only when the provider says so; for Microsoft, only when the address's domain is verified by the tenant. |
| `nonce`, `jti` | §8.2. |
| `iat`, `nbf`, `exp` | `exp` at most 300 seconds after `iat`. |

`iss`, `aud`, `sub`, `nonce`, `jti`, `iat` and `exp` are **required**; a token
missing one is refused. Claim strings are JSON-unescaped, and an over-long claim is
refused rather than truncated — a truncated subject is a different person's
subject.

**The subject is the provider's stable one so that a person is the same identity
whichever way they arrive.** A Microsoft identity is `<issuer>|<oid>` — the
tenant's own stable id for the person — so it would be the same identity from any
other relying party of that tenant.

**Keys.** `OPENCHIME_OIDC_PUBKEY[_FILE]` may hold several PEM keys. A token's
`kid` is the signing key's RFC 7638 thumbprint; the daemon computes the same
thumbprint for each key it pins and verifies with the one that matches, refusing
an unknown `kid`. Rotation is an overlap — ship the new key beside the old,
switch the signer, retire the old — with still no online key fetch (ARCH-26).

**Which provider.** `/oidc/authorize` takes `workspace`, `redirect_uri` and
`nonce`. Central sends the person straight to the workspace's provider when it has
one, and asks when it has several; which providers a workspace offers is a fact in
central's registry, because it configures central's page and not the box. A new
provider at central therefore needs no daemon or client release.

**Where the relay is.** At the origin of `OPENCHIME_ENROLL_URL` — a workspace must
be enrolled for central to mint for it, so the relay needs no address of its own.

### 8.4 Identity, and who may join

**A person is `(upstream issuer, subject)`**, held in a `user_identities` table,
whichever source delivered them.

**Who may join is one setting, `OPENCHIME_OIDC_ALLOW`** — a comma-separated list
of rules, default deny, evaluated only for an identity the workspace has not seen:

| Rule | Admits |
|---|---|
| `owner:<email>` | that verified address, created as **owner**. It also applies whenever the workspace has no active owner, which makes it the recovery path as well as the first-run one. |
| `subject:<issuer>\|<subject>` | that one identity — the relay's `sub`, matched exactly — as **owner**, on the same terms as `owner:`. It needs no address, so it names an owner whose provider verifies none: a Microsoft work account in a tenant that sends no `xms_edov`. A managed workspace's provisioning writes it beside `owner:` from the owner's console sign-in. |
| `tenant:google:<hosted domain>`, `tenant:microsoft:<tenant id>` | anyone the provider places in that organization, as member. |
| `domain:<domain>` | a **verified** address at that domain, as member. |

An identity that matches no rule joins only through an **invite bound to its
address** — tenant data an owner or admin creates (`INVITE_USER` carrying an
email), consumed at that address's first verified sign-in, setting the role.
Otherwise the answer is `AUTH_NOT_ALLOWED`, audited with the provider and tenant.
Only a provider sign-in spends such an invite, so where no provider source is on
the daemon refuses to make one (`INVITE_UNREDEEMABLE`) rather than store a row
nothing could redeem. What the inviter sends the invited person is the invitation
text their client shows — the workspace, the provider and the address, the
expiry — or, for an enrolled workspace with `OPENCHIME_INVITE_MAIL=on`, a message
central mails (ARCH-85).
A known identity signs in without consulting the rules, unless disabled; the seat
cap is unchanged.

Rules on an address need `email_verified`, because an unverified address is
whatever its holder typed. Tenant rules exist because that is not enough: a
Microsoft address is often absent or unverified, so a domain rule alone cannot say
"only our organization". A `domain:` rule needs no proof that the operator owns
the domain — it admits that domain's people into the operator's *own* workspace,
so a false claim harms nobody else.

Bearer invite tokens (§2) remain for the local source only, and `REDEEM_INVITE` is
refused where local accounts are not enabled: with a provider in charge, a bearer
token would be the phishable credential the provider exists to remove.

**A person who signs in a second way is the same person.** An identity the
workspace has not seen, whose provider verified its address, signs in as the
member one of whose identities has that address verified too, compared without
case — Google one day and Microsoft the next is one account, with its channels,
history and role — and the new identity is recorded beside the old
(`auth.subject_linked` in the audit log, with the provider, never the token). The
join rules are not consulted, as for any known identity, and no seat is taken.
There is no link on an address its provider did not verify, into a member who is
disabled or removed — a new way in does not revive an account — or where the
address is verified for more than one member, since it cannot then say which;
each of those is a first sign-in, which the rules and invites decide. Linking
comes after every check on the token and its source, so it never admits an
identity the relay may not deliver.

**A direct connection's people are its provider's.** An identity from a direct
connection (§8.5) is never linked by address, in either direction: the operator
named that provider as the authority for its people, and an address another
provider vouches for is not that authority's word. Nor does the relay deliver a
direct connection's issuer — that would be a second way in to the same accounts
that bypasses the operator's own client registration.

**Never downward.** An emailed code proves only that its holder reads the
mailbox, so it does not sign into a person who signs in here with a provider —
and that provider's second factor: it is refused with `AUTH_USE_PROVIDER`
(`auth.denied`, `reason=downgrade`), and the client says to use the provider. An
emailed code for an address is always the same identity, so only a person who
came by emailed code signs in by one. The other way links: a person known by
emailed code who later signs in with Google or Microsoft is the same person, by
the stronger proof. A workspace may allow the downward link with
`OPENCHIME_OIDC_EMAIL_LINK=any`.

A first sign-in sets the display name and address from the token; later ones
update the identity row only, and never overwrite a name the person chose.

### 8.5 Direct connections

A **direct connection** signs people in at the operator's own OpenID Connect
provider — Google Workspace, Entra ID, Okta, Keycloak — with the daemon as the
relying party and no OpenChime-operated service involved. Each one is a source in
`AUTH_CHALLENGE`, `oidc-<n>`, drawn with the operator's label. The operator's
guide to registering the application at each provider is docs/SSO.md.

**Discovery and keys.** At boot, and daily, the daemon reads
`<issuer>/.well-known/openid-configuration`, whose `issuer` must equal the
configured one exactly, and the `jwks_uri` it names — over HTTPS verified with the
built-in roots and `OPENCHIME_EXTRA_CA`. A token signed by a key it does not hold
fetches the set again, at most once a minute. A provider not yet reached is
retried every minute, and until it answers its source returns
`AUTH_SOURCE_UNAVAILABLE` at `AUTH_BEGIN`; sessions already made are untouched.

**The exchange.**
1. `AUTH_BEGIN{oidc-n, redirect_uri, challenge}` makes a **pending sign-in**:
   a random `state`, the daemon's own PKCE verifier, a random `nonce`, the client's
   challenge and its `redirect_uri`, for ten minutes, at most sixteen per source.
   `AUTH_REDIRECT` carries the provider's authorize URL — `response_type=code`,
   `scope=openid email profile`, S256 from the daemon's verifier. With
   `redirect=localhost` the loopback address is written as `localhost`, for a
   provider that registers that name.
2. The provider sends the browser back to the client's loopback with `code` and
   `state`. The client sends `AUTH{oidc, oidc-n, credential = the code, proof =
   its verifier, state}`.
3. The daemon finds the pending sign-in by `state` and spends it, whatever comes
   of the attempt; checks the verifier against the challenge kept with it; then
   POSTs the code to the provider's token endpoint with its own verifier and the
   client secret (`client_secret_basic`, or none for a public client). Neither a
   wrong `state` nor a wrong verifier lets a code reach the provider.
4. **The ID token** is checked by the daemon itself: RS256, PS256 or ES256 only,
   the key chosen by `kid`; `iss` exact; `aud` a string or a list holding the
   client id, with `azp` equal to it when there are several; `exp`, `nbf` and
   `iat` with sixty seconds of skew; `nonce` the one kept; `sub` present. No `jti`
   is needed: the code is single-use at the provider and the pending sign-in at
   the daemon.

**The identity** is `(issuer, sub)` — or `oid`, Microsoft's tenant-wide id, with
`subject=oid`, as the relay keys a Microsoft account (§8.3) — and from there it is
§8.4's: the join rules, invites, `subject:` for a first owner. The provider is
`google` for `https://accounts.google.com`, `microsoft` for an Entra issuer,
`oidc` otherwise, and the tenant Google's `hd` or Microsoft's `tid` (none for a
personal Microsoft account), so the `tenant:` rules read the same as through the
relay. Microsoft's address counts as verified only for a personal account or with
`xms_edov`, as the relay decides it. Join rules apply whenever any provider
source is on, with or without the relay.

**What a direct connection does not have.** Redirect ports: the provider must
accept any loopback port, which each provider's desktop or native application
type does (docs/SSO.md). A terminal: the provider's page needs a browser on the
machine the client runs on, so a headless terminal signs in by device code
(§8.11) with a local account.

### 8.6 A second step for local accounts

An account with a second step (REQ-184) passes it on the daemon's own pages,
after its password: the step happens where the password does, so no client
changes and no frame carries it.

- **When.** A right password on `/signin`, on `/account/password`, or approving a
  device code on `/device` (§8.11), for an account with a confirmed TOTP secret,
  is answered with the step page instead of a token, a stored password or an
  approval. Nothing is minted or changed until the step is passed.
- **The ticket.** What the password proved — the account, the version of its
  password, and what the page was doing: a sign-in's callback and challenge, a
  device code, a password change's new key, already derived — is kept in the
  event loop's memory against a random ticket the step page carries. Only its
  hash is kept; it lives five minutes and five wrong codes; a source holds a
  few at once; a restart drops them.
- **The code.** `POST /signin/verify` with the ticket and a code: six digits are
  a TOTP code (RFC 6238, HMAC-SHA1, thirty-second steps, one step either side,
  never a step already used); anything else is tried as a recovery code, which
  is spent. The writer checks the password is still the one checked, then the
  code, then does what the page was doing. Wrong codes have a limiter of their
  own — five per account in five minutes — apart from the password's, and are
  audited (`auth.failed`, `step=code`); a recovery code spent is audited too.
- **The secret is sealed** under the factor key, outside the database (SCHEMA.md
  migration 0053). Without the key, or with the wrong one, the step cannot be
  passed: the sign-in is refused and the log says why, rather than letting the
  person in without it.

**Setting it up** is a page too, `/account/security`, reached from the sign-in
page and from each client's account menu. The password first, as on every page;
then a new secret — sealed, kept unconfirmed — shown as a key to type and a QR
code (inline SVG, so the page still fetches nothing) of its `otpauth://` URI. The
first code it shows turns the step on and hands out ten recovery codes, shown
once (only salted hashes are kept), with a link that downloads them as a text
file the page itself holds and a button that copies them. On, the same page turns it off with a code
or a recovery code, which removes the secret and the codes. Wrong codes count
as a sign-in step's do.

**Passkeys** (WebAuthn) answer the step in place of a code, as a second
factor: an account adds one on `/account/security` once a code from its
authenticator or a recovery code allows it, so a password alone never adds a
way past the step. They are offered only on the workspace's own name where a CA
vouches for its certificate — the workspace address, or an ACME or operator's
certificate name — never through the loopback tunnel, where a passkey would
bind to `127.0.0.1`. The relying party is that name and the origin
`https://<name>`; attestation is `none`; ES256 and RS256 keys are taken; a
signature counter that is not zero must rise. The page's script,
`/webauthn.js`, is served by the daemon and named by its SRI hash. Scripts
(`script-src 'self'`) are allowed only on a page that offers a passkey and on
the recovery codes, whose copy button is `/codes.js`, served and named the same
way, and carried by the tunnel. A passkey is
bound to its name, so one made before a rename does not answer after it; the
page says to add it again. Turning the step off, or a reset that clears it,
removes them. Passkey-only accounts, with no password, are not offered.

**Policy** is `OPENCHIME_LOCAL_MFA`: `optional` (the default) asks for the step
where one is set up; `required` also refuses the sign-in of an account with none
until it sets one up — the page says where; `off` asks for none and closes the
setup page.

A provider's own second factor is the provider's business and never reaches this
step.

### 8.7 Enrolling a managed workspace

Exactly one party mints a workspace's audience. **Self-hosted:** the daemon, as
§3.6 — an operator couriers the code. **Managed:** central, which starts the box
with `OPENCHIME_OIDC_AUDIENCE` and a one-time `OPENCHIME_ENROLL_TICKET`. The daemon
adopts that audience, generates its own key as it always does, and claims the
binding: the ticket, its public key, and a signature over
`openchime-claim-v1|<aud>|<base64url(SHA-256(ticket))>|<base64url(SHA-256(public key))>`,
the key hashed as its SubjectPublicKeyInfo DER, posted to `<enroll url>/claim` as
`{audienceId, ticket, publicKey, signature}` — the ticket base64url, the key and the
DER signature base64. The claim is made once the daemon is serving — its protocol
listener bound and its loop about to serve — because central reads an activated
binding as a workspace that is up; the push emitter and invitation mail start when
it succeeds. The daemon retries while central cannot be reached, for a
bounded time, and not at all once the ticket is refused; a box that already holds
a different audience refuses to start rather than become a second workspace.
Central checks the ticket — single use, short-lived — and the signature, stores
the public key and activates. The ticket is the authorization the operator's
paste is in the self-hosted flow; the private key still never leaves the box.

Requests the daemon makes afterwards — push batches, invitation reports and
certificates through central — are each signed on their own with the same key.
The canonical string is
`openchime-machine-v2|<METHOD>|<path>|<aud>|<unix_ts>|<sha256hex(body)>`: the
method, the path and query as sent, and the lowercase hex SHA-256 of the exact
request body, so a signature is good for one request to one endpoint and cannot
be replayed at another that takes the same body. The signature is ASN.1-DER
ECDSA P-256 over the SHA-256 of that string, base64. The request carries it in
four headers — `X-OpenChime-Audience`, `X-OpenChime-Timestamp` (unix seconds),
`X-OpenChime-Signature` and `X-OpenChime-Signature-Version: 2`. Central verifies it against the stored public key,
refuses a timestamp more than 300 seconds from its own clock, and requires the
binding to be active. The string names neither the method nor the path, so the
freshness window is what bounds a replay.

### 8.8 Configuration

| Setting | Meaning |
|---|---|
| `OPENCHIME_AUTH_MODE` | A list of the sources — `local`, `relay`, or `local,relay` — with `oidc` read as `relay`. |
| `OPENCHIME_OIDC_PUBKEY[_FILE]` | May hold several keys (§8.3). |
| `OPENCHIME_OIDC_ALLOW` | Who may join (§8.4). |
| `OPENCHIME_OIDC_CONNECT_<n>` | A direct connection, `n` 1 to 4 (§8.5): `label=…;issuer=https://…;client_id=…;secret_file=…;subject=sub\|oid;redirect=127.0.0.1\|localhost`. Any error in one stops the boot. |
| `OPENCHIME_ENROLL_TICKET` | Managed workspaces only (§8.7). |

### 8.9 Certificates through central

A daemon bound to central (§8.7) with `OPENCHIME_TLS_SOURCE=central` gets its
TLS certificate for its name under the service suffix through central, which
holds that DNS zone and so completes an ACME DNS-01 challenge on the daemon's
behalf; the daemon needs no inbound access and no DNS of its own (ARCH-10). Both
requests are machine requests, signed as §8.7 says, to the enrollment origin:

- `POST /api/machine/tls/names` with body `{}` answers `200 {"names":"<a,b>"}`:
  the names this workspace may have, which are central's to say.
- `POST /api/machine/tls/certificate` with body `{"csr":"<base64url DER>"}` — a
  CSR for those names over a key the daemon has just made, which never leaves it
  — answers `200 {"names":"<a,b>","chain_pem":"<leaf then intermediates>"}`;
  `202` with `Retry-After` (seconds) while central is still completing the
  challenge; or a `4xx`, which is final until the next boot.

A box whose `OPENCHIME_WORKSPACE_ADDRESS` its kept certificate does not name — it
moved to a new address, and central restarted it — asks for a new one at boot
rather than presenting the old one until renewal.

The daemon asks again at a random moment from 60% to two-thirds of the
certificate's life, and after a failure a minute later, then ten, a hundred, and
daily.

### 8.10 Local accounts in the browser

A local account signs in on the daemon's own pages, in the system browser; no
OpenChime client collects a password, and the sign-in ends, as the relay's does,
with the client holding an ID token.

**The daemon as an issuer.** At first start the daemon makes a P-256 key and an
issuer name (`openchime-local:<random>`) and keeps them in the database
(`local_issuer`, SCHEMA.md migration 0050), so a restored database still verifies
what it signed. Its token has the relay token's shape (§8.3): `iss` its name,
`aud` `openchime-client`, `sub` `local|<user id>`, `nonce` the client's PKCE
challenge, a fresh `jti`, and a life of 120 seconds; ES256, `kid` the key's
thumbprint.

**The exchange.** `AUTH_BEGIN{source: "local", redirect_uri, challenge}`, with the
same loopback and challenge checks as §8.1, is answered with a **path**:
`/signin?redirect_uri=…&nonce=…`. The client decides where the browser goes:

- **Directly**, when a trusted authority vouched for the daemon's certificate
  under the workspace's name (ARCH-10) — every managed workspace, and a
  self-hosted one with a certificate from ACME, a file or an internal CA:
  `https://<workspace>[:port]/signin…`. The page has the workspace's own origin,
  which password managers and passkeys key on.
- **Through the client's loopback tunnel** otherwise — a self-signed daemon
  trusted by fingerprint, or one on loopback: `http://127.0.0.1:<port>/p/<secret>/signin…`.
  A loopback origin is one the browser treats as secure, so there is no warning.
  The tunnel carries only `/signin`, `/signin/verify`, `/signup`,
  `/account/password` and `/account/security`; only a
  request whose `Host` is the listener's own (DNS rebinding); and only to the
  certificate the client's own connection accepted — over TLS with ALPN
  `http/1.1`, as the daemon's origin: its `Host`, and for a post from the
  tunnel's own page its `Origin`. A post from any other page keeps its own
  `Origin`, which the daemon refuses. Each request gets fifteen seconds to reach
  the daemon and hand it over, and then waits for the answer on a budget of its
  own — up to two minutes, for as long as the browser is still connected — since
  a page that sets a password derives hashes before it answers, which on a busy
  server is longer than any fixed few seconds. It closes after the callback, on
  cancel, or after five minutes (ten, for a page opened on its own).

The person signs in on the page; the daemon checks the password on the same
staged path as §2 — the limiters, the pool, the credential's version, the audit
— and answers `303` to `redirect_uri?token=<jwt>`. The client presents
`AUTH{oidc, source "local", token, verifier}`; the daemon verifies it against its
own key, name and audience, then the verifier (§8.2), then single use, and takes
the user by id from `sub` — no join rules, nothing created; a removed member is
refused. The session is minted as for any sign-in.

**The pages.** Served on the TLS port to a peer that negotiates HTTP (ARCH-54),
from `daemon/webpages.c`:

- `/signin` — username and password. A sign-in link carrying `invite=` opens the
  sign-up form instead.
- `/signup` — an invitation or the setup token, a username and a password twice.
  It makes the account, then goes on as a sign-in.
- `/account/password` — username, current password and a new one twice. It
  needs no session, so the limiters stand in front of it as of a sign-in. A
  change revokes every session the account has and closes each connection on
  one: a device signed in with the old password signs in again (REQ-182).

Every page is self-contained HTML with its output escaped, no script, no
cookies and no state kept between requests, with
`Content-Security-Policy: default-src 'none'; style-src 'unsafe-inline';
frame-ancestors 'none'; base-uri 'none'; form-action 'self' <the callback's
origin>` (browsers hold a form's redirect to `form-action`),
`X-Frame-Options: DENY`, `X-Content-Type-Options: nosniff`,
`Cache-Control: no-store` and `Referrer-Policy: same-origin` — no other site is
told a page's URL, and the page's own posts carry its origin (under
`no-referrer` a browser sends `Origin: null`, which the check below refuses).
Every URL on a page is relative, so the same pages serve both ways in — but the
passkey script's, `/webauthn.js`, which only the workspace's own name serves a
page for. A post must be a form
whose `Origin` is `https://` + its `Host`, and a sign-in's `redirect_uri` must be
loopback and its `nonce` a challenge. A refusal shows the form again and never
says which half of a credential was wrong.

**Passwords in frames.** `AUTH{local}`, `REDEEM_INVITE` and `CHANGE_PASSWORD` are
refused unless the daemon runs with `OPENCHIME_TEST_PASSWORD_AUTH=1`, a test
knob (CONFIG.md) that it warns about at start; the test suites and the GUI
scripts sign in with it.

**The terminal client** signs in to a local account with a device code (§8.11),
which works where it has no browser of its own.

### 8.11 A terminal signs in with a code

A client with no browser of its own — the terminal client over SSH, on a
headless host — signs in to a local account with a **device code** (RFC 8628,
as the GitHub CLI and AWS SSO do), on its protocol connection:

1. `AUTH_DEVICE_BEGIN{source: "local", challenge}` — the client's PKCE challenge
   (§8.2) — is answered with `AUTH_DEVICE{device_code, user_code,
   verification_path: "/device", interval_s, expires_in_s}`. The **user code** is
   eight letters from `BCDFGHJKLMNPQRSTVWXZ`, shown `XXXX-XXXX` — no vowels, none
   people confuse. The **device code** is 32 random bytes, base64url: the
   client's secret, never shown, and the daemon keeps only its SHA-256.
2. The client shows `https://<workspace>/device?code=<user code>`, the code, and
   the URL as a QR code; where no authority vouches for the daemon's certificate
   it shows the fingerprint it accepted, for the person to check against the
   browser's warning.
3. The person opens the page on any device. It shows **who is asking** — the
   requesting connection's address and how long ago — and asks for the username
   and password, with **Sign in the terminal** and **That wasn't me**. Approving
   runs the sign-in of §8.10 — the same staged check and limiters — whose token
   is bound to the terminal's challenge rather than a callback's, and kept for
   the terminal to collect.
4. The client polls with `AUTH_DEVICE_POLL{device_code}` at the interval:
   `AUTH_PENDING` until then; `AUTH_SLOW_DOWN` for a poll sooner than the
   interval, which grows by five seconds (RFC 8628 §3.5); `AUTH_DENIED` once, for
   a refusal; `AUTH_EXPIRED` for a code that ran out, was collected, or never
   was. Approved, it is answered `AUTH_DEVICE_TOKEN{token}`, once, and the client
   presents `AUTH{oidc, "local", token, verifier}` on the same connection. A
   dropped connection keeps the device code, and the next one polls on.

**Bounds.** The pending requests live in the event loop's memory — ten minutes
each, dropped by a restart — at most 256, and at most five waiting from one
source. A lookup of a code that is not waiting counts against its source, ten a
minute, so the codes cannot be walked (20⁸ of them against ten guesses a
minute). A code is found only while pending; the page never says whose it is.

**Phishing.** A device code can be sent to someone to approve for an attacker,
so the page says where the request came from and when, tells the person to go on
only if it was them, and offers a refusal the terminal is told about.
