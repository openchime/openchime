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
| **Local** (§2) | The daemon manages accounts + passwords itself. | Nothing outside the box — fully air-gappable. |
| **Relay** (§3) | The client logs in with Google/MS/Apple through the project's central service, which re-issues a token the daemon trusts. | The project's central service, at login time only. |
| **Direct connection** | The client logs in at an OIDC provider the operator names, and the daemon is the relying party: it redeems the code and validates the provider's ID token. | The operator's own provider. No OpenChime-operated service. |

What a deployment may enable follows from the three deployment models of
ARCH-76, because the relay is one of the functions a deployment federates and
the other two sources need nothing from the project.

| Deployment model (ARCH-76) | Sources |
|---|---|
| **Self-hosted stand-alone** | **Local**, a **direct connection**, or both. No dependency on any OpenChime-operated service; on local accounts alone this model is fully air-gappable. |
| **Self-hosted federated** | Any of the three. Opting in to the federated OIDC function is what adds the relay; a federated deployment may equally decline it and federate only push, directory, SCIM, DNS, or packages. |
| **Hosted** | The **relay**, operated by the project alongside the daemons, or a **direct connection** to the customer's own provider. |

The two OIDC sources answer different needs. The relay keeps the daemon
maximally lean on that path (it never fetches JWKS or handles multiple providers
— it verifies one JWT from one pinned key, §3.3) and means a self-hoster never
registers provider apps or holds provider credentials; its price is a login-time
dependency on the project, and the project seeing who signs in where (§3.4). A
direct connection is for the operator who would rather hold those credentials
than pay that price, and it is the only single sign-on a stand-alone deployment
can have. **SAML is not a source** (REQ-027).

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
  already linked and has no argon2. Passwords travel only inside the TLS session (REQ-180), so a
  plaintext password in the `AUTH` frame is acceptable on the wire; it is never
  stored.
- **Bootstrapping the first owner:** the initial account (tenant **owner**) is
  created at first run from a one-time **setup token**. There is no config file
  and no configuration variable for it (ARCH-26): the daemon **mints** the token
  itself when a local-mode first run finds no owner, and prints it once to
  stderr. This avoids the chicken-and-egg of "you need an admin to create the
  first admin" without requiring email (air-gapped-safe).
- **Adding users:** an owner/admin creates an account and issues an **invite
  token**; the invitee sets their password by presenting the token. Email
  delivery is never required.
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
  counter but not the source counter.
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

For social login (Google / Microsoft Entra / Apple, per REQ-021/022) without
each operator registering provider apps.

### 3.1 The idea

The **central service** (maintainer-controlled) is the OIDC Relying Party: it
holds the Google/MS/Apple client credentials, runs the login flow, and
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
   on desktop (RFC 8252), or a configured `https` destination.
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

The client half of this is the browser launch, the loopback listener and, on
Apple platforms, the `ASWebAuthenticationSession` path.

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
compares with `strcmp` and never dereferences — there is no JWKS fetch and no
discovery document, because the key is pinned in configuration (§3.4). Any
value both sides agree on works, and an implementer should not read the example
as a URL that has to resolve, or as requiring a particular subdomain.

The daemon validates it by **pinning both the keys and the algorithm**
(`daemon/jwt.c`):

- it requires `alg = ES256` and rejects anything else — this closes JWT's classic
  footguns (`alg=none`, RS256/HS256 confusion) up front;
- it verifies the signature with mbedTLS (ES256 = ECDSA-P256, which mbedTLS
  supports directly; EdDSA/Ed25519 is not supported, so ES256 is the choice);
- it chooses the key by the header's `kid` — the signing key's RFC 7638
  thumbprint, which it computes for each key it pins — and refuses a token whose
  `kid` is absent or names none of them (§8.3);
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
- **Dependency is login-time only.** Once the daemon issues a session (§4), it
  never contacts central again; existing sessions survive a central outage. Only
  *new logins* need central up, and the message path never does. Local mode has
  no central dependency at all — which is what makes self-hosted stand-alone
  (ARCH-76) possible.
- **Privacy tradeoff:** in relay-OIDC the central service sees *who* logs into
  which workspace (identities, not message content — it never touches
  messages/channels). A self-hoster wanting zero project visibility declines the
  relay and uses local accounts or a direct connection to their own provider
  (§1); declining every federated function is exactly the self-hosted
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
`OPENCHIME_PUSH_URL` (the control-plane push gateway; `OPENCHIME_PUSH_CA_BUNDLE` optional) delivers
mobile push (REQ-132/133). The daemon owns a device-token registry
(`REGISTER_DEVICE_TOKEN`); a committed SEND drives an off-hot-path worker that selects
recipients (members − author, level=ALL, not in DND, holding a token), signs a
**contentless** batch with the enrollment key — the same request-signature scheme
central verifies (`openchime-machine-v1|<aud>|<ts>|<sha256(body)>`) — and POSTs it to
the gateway, which relays to APNs/FCM and returns stale tokens to prune. Absent in
self-hosted stand-alone (no enrollment / no `OPENCHIME_PUSH_URL`).

---

## 4. Sessions — the convergence point (ARCH-58)

However identity was proven (local password, OIDC token, or an existing session
token on reconnect), the daemon then does the same thing:

1. **Provision/look-up the user** (`users` table). `users.subject` is the unique
   identity key, namespaced by source: `oidc:<issuer>|<sub>` or `local:<username>`.
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
- **Revocation (REQ-182):** "log out" / "log out other devices" deletes the
  relevant `sessions` row(s); the next protocol interaction on a revoked session
  fails. This local revocation is exactly what a stateless provider JWT cannot
  provide, and is the reason the daemon issues its own sessions.

---

## 5. Mode selection and the auth handshake

After `WELCOME` and before `AUTH`, the daemon sends **`AUTH_CHALLENGE`**
(PROTOCOL.md §4) listing the sources this deployment signs people in with
(`OPENCHIME_AUTH_MODE`: `local`, `relay`, or both). The client draws one control
per source and replies with `AUTH`, whose `method` discriminator selects the path:

- `local` — username + password → verified against `local_credentials`.
- `oidc` — a browser sign-in. `AUTH_BEGIN` gets the authorize URL from the daemon;
  `AUTH` then carries the central-issued ES256 JWT (§3.3) and the verifier (§8.2).
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
- **≥1 owner invariant:** demoting the tenant's last owner is
  refused with `LAST_OWNER` (REQ-030), checked against a live `COUNT(*)` of owners.
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
- **Cert-vs-restore interaction** (the TOFU fingerprint changing when a database
  is restored onto a new box): handled by persisting the TLS identity in the
  database (ARCH-66b); orthogonal to auth.

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
  |  a local source:                              |
  | ---- AUTH{local, source, user, password} ---> |
  |                                               |
  |  a browser source (the relay, a direct connection):
  | ---- AUTH_BEGIN{source, redirect_uri, challenge} -> |
  | <--- AUTH_REDIRECT{authorize_url} ----------- |   the daemon builds the whole URL
  |        … the person signs in in their browser; the client may disconnect …
  | ---- AUTH{oidc, source, verifier, what came back} -> |
  |                                               |
  | <--- AUTH_OK | AUTH_CONTINUE | ERROR -------- |
```

- **`AUTH_CHALLENGE` lists sources** — `{id, kind, label}`, `kind` being `local`,
  `relay` or `oidc`. The client
  draws one control per source from the labels: fixed text for local accounts and
  the relay, the operator's own words for a direct connection ("Acme SSO").
  Resuming a session (§4) is always accepted and is not a listed source.
- **The daemon builds the authorize URL.** The client never assembles or parses an
  operator's or a provider's string, so it is the same client for the relay and
  for a direct connection, and the workspace's audience reaches the relay from the
  one party that knows it. The client opens the URL only if it is `https` (plain
  `http` to loopback, for development).
- **`redirect_uri` is loopback** (RFC 8252); the daemon refuses anything else
  before it echoes it into a URL.
- **No connection is held open while the browser is.** Nothing between
  `AUTH_REDIRECT` and `AUTH` lives on the connection (§8.2), so the client may
  disconnect and an unauthenticated connection can be short-lived.
- **`AUTH_CONTINUE`** answers a first step that is correct but not sufficient
  (§8.6). It is part of the exchange so that a second factor changes no handshake.
- **Errors a client can explain:** `AUTH_NOT_ALLOWED` (a valid identity that may
  not join, §8.4), `AUTH_SOURCE_UNAVAILABLE` (a provider that cannot be reached,
  §8.5), `AUTH_MFA_REQUIRED`, beside the codes that exist.
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
- **Through a direct connection,** the daemon keeps the challenge with the pending
  sign-in it created (§8.5) and requires the verifier with the code.

A token or a code lifted from the loopback redirect, from browser history, or by
another account on a shared machine is therefore useless: whoever presents it
must also hold the verifier, which never left the client. The relay path needs no
state in the daemon to check it, and the check survives the client reconnecting
between the two frames.

### 8.3 The relay's token

Standard JWT, ES256, as §3.3. The claims:

| Claim | Meaning |
|---|---|
| `iss`, `aud` | Central, and this workspace's opaque id (§3.3). |
| `sub` | `<upstream issuer>|<stable subject>`. For Google the provider's `sub`. For Microsoft, `oid` under the tenant's issuer — `https://login.microsoftonline.com/<tid>/v2.0|<oid>` — not the per-application `sub`. |
| `idp` | Which provider vouched: `google`, `microsoft`. |
| `tenant` | The organization the provider places the person in: Google's hosted domain, Microsoft's tenant id. Absent for a personal account. |
| `email`, `email_verified`, `name` | As the provider gave them. `email_verified` is true only when the provider says so; for Microsoft, only when the address's domain is verified by the tenant. |
| `nonce`, `jti` | §8.2. |
| `iat`, `nbf`, `exp` | `exp` at most 300 seconds after `iat`. |

`iss`, `aud`, `sub`, `nonce`, `jti`, `iat` and `exp` are **required**; a token
missing one is refused. Claim strings are JSON-unescaped, and an over-long claim is
refused rather than truncated — a truncated subject is a different person's
subject.

**The subject is the provider's stable one so that a person is the same identity
whichever way they arrive.** A workspace that starts on the relay's Microsoft
sign-in and later connects directly to its own tenant (§8.5) sees the same
`<issuer>|<oid>` and keeps its accounts, roles and history.

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
whichever source delivered them. One rule keeps the sources honest: an identity
whose issuer belongs to one of the deployment's direct connections is accepted
only from that connection, never from the relay.

**Who may join is one setting, `OPENCHIME_OIDC_ALLOW`** — a comma-separated list
of rules, default deny, evaluated only for an identity the workspace has not seen:

| Rule | Admits |
|---|---|
| `owner:<email>` | that verified address, created as **owner**. It also applies whenever the workspace has no active owner, which makes it the recovery path as well as the first-run one. |
| `tenant:google:<hosted domain>`, `tenant:microsoft:<tenant id>` | anyone the provider places in that organization, as member. |
| `domain:<domain>` | a **verified** address at that domain, as member. |

An identity that matches no rule joins only through an **invite bound to its
address** — tenant data an owner or admin creates (`INVITE_USER` carrying an
email), consumed at that address's first verified sign-in, setting the role.
Otherwise the answer is `AUTH_NOT_ALLOWED`, audited with the provider and tenant.
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

A first sign-in sets the display name and address from the token; later ones
update the identity row only, and never overwrite a name the person chose.

### 8.5 Direct connections

One setting per connection, `OPENCHIME_OIDC_CONNECT_<n>`, holding
`label=…;issuer=…;client_id=…;secret_file=…;subject=sub|oid`. The secret is
optional — a public client with PKCE where the provider allows one — and
`subject=oid` makes a Microsoft tenant's identities match the relay's (§8.3). Each
connection is a source in `AUTH_CHALLENGE`.

- **Discovery and keys** are fetched over CA-verified TLS at boot and on an unknown
  `kid`, rate-limited and cached. A provider that cannot be reached makes its
  source `AUTH_SOURCE_UNAVAILABLE`; sessions already issued are untouched (§3.4's
  login-time-only dependency, here on the operator's provider).
- **`AUTH_BEGIN`** creates a pending sign-in — `state`, the daemon's own PKCE
  verifier, a nonce, the client's challenge, ten minutes to live — and returns the
  provider's authorize URL.
- **`AUTH{oidc}`** carries the code, the `state` and the client's verifier. A
  worker of the push and unfurl kind redeems the code at the token endpoint;
  nothing blocking runs on the writer.
- **The ID token** is accepted under an algorithm allow-list (RS256, PS256, ES256),
  with `iss` exact, `aud` containing the client id (`azp` when there are several),
  `exp`, `iat`, `nbf`, and the nonce. §8.4 then applies unchanged.
- **The redirect** is `127.0.0.1` or `localhost` as the connection says, with an
  optional list of ports for a provider that matches the port exactly.

### 8.6 A second step for local accounts

A correct password on an account with a second factor (REQ-184) answers
`AUTH_CONTINUE{totp}`. The connection is then half-authenticated for two minutes
and accepts only `AUTH` carrying the code; attempts have a limiter of their own.
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
DER signature base64. The daemon retries while central cannot be reached, for a
bounded time, and not at all once the ticket is refused; a box that already holds
a different audience refuses to start rather than become a second workspace.
Central checks the ticket — single use, short-lived — and the signature, stores
the public key and activates. The ticket is the authorization the operator's
paste is in the self-hosted flow; the private key still never leaves the box.

Requests the daemon signs afterwards name what they are for: the canonical string
is `openchime-machine-v2|<aud>|<unix_ts>|<METHOD>|<path>|<sha256hex(body)>`,
so a signed request cannot be replayed at a second endpoint.

### 8.8 Configuration

| Setting | Meaning |
|---|---|
| `OPENCHIME_AUTH_MODE` | A list of the built-in sources — `local`, `relay`, or `local,relay` — with `oidc` read as `relay`. A direct connection is enabled by being configured. |
| `OPENCHIME_OIDC_PUBKEY[_FILE]` | May hold several keys (§8.3). |
| `OPENCHIME_OIDC_ALLOW` | Who may join (§8.4). |
| `OPENCHIME_OIDC_CONNECT_<n>` | One per direct connection (§8.5). |
| `OPENCHIME_ENROLL_TICKET` | Managed workspaces only (§8.7). |
