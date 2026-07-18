# OpenChime — Authentication

How users prove who they are, and how a session is established and revoked.
This is the authoritative design; it is cross-referenced from ARCHITECTURE.md
(ARCH-19, ARCH-55–ARCH-60), REQUIREMENTS.md (§1.2, §8.1), PROTOCOL.md (§4), and
SCHEMA.md (migration 0002).

**Status.** **Both modes are implemented.** Local mode verifies username +
password against PBKDF2-HMAC-SHA256 credentials (`local_credentials`); OIDC mode
verifies a central-issued ES256 JWT against a pinned key (`daemon/jwt.c`, jsmn
for the claims) and JIT-provisions the user. Both converge on a daemon-issued
session (§4) and accept session tokens on reconnect (`process_auth` in
`daemon/dbwriter.c`, crypto in `daemon/auth.c`, wire frames in PROTOCOL.md §4).
The mode is chosen at boot: default local (`OC_BOOTSTRAP_USERS` provisions the
owner, §2), or `OC_AUTH_MODE=oidc` with `OC_OIDC_ISSUER` / `OC_OIDC_AUDIENCE` /
`OC_OIDC_PUBKEY[_FILE]` — `AUTH_CHALLENGE` then advertises the matching methods
bitset (`local|session` or `oidc|session`). Failed local-auth is **rate-limited
per account** (REQ-191, `daemon/ratelimit.c`): after a burst of failures the
account is refused with `AUTH_RATE_LIMITED`, checked before the expensive PBKDF2.
Role **changes** are enforced in the writer (`SET_ROLE`, `daemon/roles.c`): the
owner/admin/member policy plus the ≥1-owner invariant (§6). Failed local-auth is
rate-limited both per account and per source IP (REQ-191). Tenant management is
**implemented and exposed over the wire** (PROTOCOL.md §5.8): `SET_ROLE`,
`LIST_USERS`, `INVITE_USER` + `REDEEM_INVITE` (invite-token account creation, §2),
and `REMOVE_USER` (which locks a member out via the `users.disabled` flag added in
migration 0003, checked in every auth path). Moderation-delete (REQ-032) and
channel management (REQ-031) also landed. **Remaining:** a configurable OIDC
bootstrap-owner subject, and email magic-link invite delivery (§7).

---

## 1. Two modes, one session

Authentication has **two modes, chosen per deployment** (ARCH-55). They differ
only in how identity is *proven*; both then **converge on a daemon-issued
session** (§4), so everything downstream — SEND, backfill, reconnect,
revocation — is identical regardless of mode.

The three rows below are exactly the three deployment models of ARCH-76 — OIDC
is one of the functions a deployment federates, so the auth mode follows from
the model rather than being an independent choice.

| Deployment model (ARCH-76) | Mode | How identity is proven |
|---|---|---|
| **Self-hosted stand-alone** | **Local** | The daemon manages accounts + passwords itself. No dependency on any OpenChime-operated service — this model is fully air-gappable. |
| **Self-hosted federated** | **Local** or **OIDC (via relay)** | Opting in to the federated OIDC function means the client logs in with Google/MS/Apple through the project's central service, which re-issues a token the daemon trusts. A federated deployment may equally decline OIDC and stay on local accounts while federating only push, directory, SCIM, DNS, or packages. |
| **Hosted** | **OIDC (in-house)** | The same central service, operated by the project alongside the daemons. |

There is deliberately **no "point the daemon straight at your own IdP" mode**:
OIDC always routes through the central service. This keeps the daemon maximally
lean (it never fetches JWKS or handles multiple providers — it verifies one JWT
from one pinned key, §3.3) and means self-hosters never register provider apps
or hold provider credentials. A self-hoster who wants social login
but no central involvement should instead run local mode. **v1 supports one mode
per tenant** (local XOR oidc); both-at-once is a future extension.

A deployment's mode is set in the daemon's static config (ARCH-26) and
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
  already linked and has no argon2; argon2 (a small vendored lib) is a noted
  future upgrade. Passwords travel only inside the TLS session (REQ-180), so a
  plaintext password in the `AUTH` frame is acceptable on the wire; it is never
  stored.
- **Bootstrapping the first owner:** the initial account (tenant **owner**) is
  created at first run from a one-time **setup token** — a value in the config
  file, or printed once to the daemon's log. This avoids the chicken-and-egg of
  "you need an admin to create the first admin" without requiring email
  (air-gapped-safe).
- **Adding users:** an owner/admin creates an account and issues an **invite
  token**; the invitee sets their password by presenting the token. Email
  magic-link delivery is an optional future enhancement, never required.
- **Brute-force protection (REQ-191):** the daemon rate-limits failed local-auth
  attempts per account **and** per source IP (`daemon/ratelimit.c`, fixed-window
  counters checked before PBKDF2 so a flood can't burn CPU), answering excess
  attempts with `ERROR AUTH_RATE_LIMITED`. The per-source cap is higher than
  per-account, so many users behind one NAT are tolerated while an account-spray
  from a single IP is still stopped; a successful login clears the account
  counter but not the source counter. This is why REQ-191 lives with the auth
  design — it is squarely a local-password concern.

---

## 3. OIDC via the central service and relay (ARCH-56)

For social login (Google / Microsoft Entra / Apple, per REQ-021/022) without
each operator registering provider apps.

### 3.1 The idea

The **central service** (maintainer-controlled) is the OIDC Relying Party: it
holds the Google/MS/Apple client credentials, runs the login flow, and
**re-issues** an OpenChime **ES256 JWT** (§3.3) that the daemon trusts. All the
OIDC machinery — JWKS fetching, provider quirks, key rotation, multi-provider
handling — lives in that service (a higher-level web service), **never in the C
daemon**. The daemon only verifies one JWT signature against one configured,
pinned public key (plus a tiny vendored JSON reader for the claims).

### 3.2 The flow — the client is the courier

The central service **never connects to a self-hosted daemon** — the client
carries the token from center to daemon. This preserves the island model
(ARCH-4): no central→daemon link.

```
  ┌────────┐   1. authorize (central's client_id,        ┌──────────┐
  │ client │──────redirect_uri, PKCE, state=workspace)───▶│ provider │
  │ (app)  │◀──────────────── 2. login ──────────────────│ (Google) │
  └───┬────┘                                              └────┬─────┘
      │                                    3. auth code        │
      │                                    ┌────────────────◀──┘
      │                                    ▼
      │                            ┌───────────────┐
      │  4. ES256 JWT              │   central     │  exchanges code (secret),
      │◀───(aud=acme.example)──────│   service      │  verifies, mints JWT
      │                            └───────────────┘
      │  5. AUTH{method=oidc, token}
      ▼
  ┌────────────────┐  verifies signature vs central's pinned key,
  │ daemon         │  checks audience==self + not expired,
  │ (acme.example) │  provisions/looks-up user, mints a session (§4)
  └────────────────┘
```

1. The client opens a browser to the provider's authorize endpoint using
   **central's** `client_id` and redirect URI (e.g.
   `https://auth.openchime.io/callback`), a PKCE challenge, and a `state` that
   encodes the **target workspace** + a client nonce.
2. The user authenticates at the provider.
3. The provider redirects to central's callback with an auth code.
4. Central exchanges the code (with its client secret), verifies the provider
   token, extracts identity (subject, email, name), and mints an **ES256 JWT
   scoped to that workspace** (`aud = acme.example`), returned to the *client*.
5. The client presents that token to the `acme.example` daemon in `AUTH`
   (method `oidc`). The daemon verifies it (§3.3) and mints a session.

### 3.3 The identity token — an ES256 JWT (ARCH-57)

Central issues a **standard JWT** signed with **ES256** (ECDSA P-256 + SHA-256),
carrying the identity claims:

```
{ "iss": "https://auth.openchime.io",   // the central service
  "aud": "acme.example",                 // the target workspace
  "sub": "<provider issuer>|<subject>",  // stable identity
  "email": "...", "name": "...",
  "iat": ..., "exp": ... }
```

The daemon validates it by **pinning both the key and the algorithm**:

- it requires `alg = ES256` and rejects anything else — this closes JWT's classic
  footguns (`alg=none`, RS256/HS256 confusion) up front;
- it verifies the signature with mbedTLS (ES256 = ECDSA-P256, which mbedTLS
  supports directly; EdDSA/Ed25519 is not supported, so ES256 is the choice);
- it checks `iss` (central), `aud` (== this workspace's configured id, so a token
  minted for one workspace cannot be replayed at another), and `exp`.

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
  and this workspace's **`audience` id**. A self-hoster enabling relay-OIDC
  registers their workspace with central once to obtain that id.
- **Dependency is login-time only.** Once the daemon issues a session (§4), it
  never contacts central again; existing sessions survive a central outage. Only
  *new logins* need central up, and the message path never does. Local mode has
  no central dependency at all — which is what makes self-hosted stand-alone
  (ARCH-76) possible.
- **Privacy tradeoff:** in relay-OIDC the central service sees *who* logs into
  which workspace (identities, not message content — it never touches
  messages/channels). A self-hoster wanting zero project visibility declines the
  OIDC function and uses local mode; declining every federated function is
  exactly the self-hosted stand-alone model (ARCH-76).

### 3.5 Reconciling with the island model (REQ-041)

REQ-041 states that **no shared runtime component sits in the message/data path,
in any deployment model** — and that holds here: data isolation (REQ-040) is
untouched. The central OIDC service is contacted at **login time only** (never
per-message), brokers **identity only** (it never sees message or channel
content), and is **absent entirely in local mode**, hence absent from every
self-hosted stand-alone deployment.

OIDC is not the only federated function, though — a self-hosted federated
deployment may also depend on the project for push (ARCH-16), the app directory
(REQ-175), SCIM (REQ-253), a DNS name and the workspace registry (ARCH-14), and
packages (ARCH-20). What unites them, and what REQ-041 actually guarantees, is
that each brokers identity, notification, discovery, or provisioning metadata
and **none carries message content**. So federating costs availability
independence, never message confidentiality.

### 3.6 The central service / relay (separate component)

The central service + relay are a **separate system** — a web service, not the C
daemon — and are built independently. Its contract with the daemon is narrow:

- run the Authorization-Code-+-PKCE flow against the configured providers;
- mint ES256 JWTs (§3.3) signed by the key the daemon pins, audience-scoped to
  the requesting workspace;
- maintain the workspace registry (which `audience` ids are valid) — a federated
  self-hoster registers once to obtain an id (§3.4). This registry is part of the
  OIDC function and is never consulted for workspace discovery, which is plain
  DNS in every model (ARCH-14).

Until it exists, the daemon's OIDC path is tested with a **test issuer**:
generate an ECDSA-P256 keypair in the test, mint central-style ES256 JWTs, and
configure the daemon with the test public key — the same faking approach as the
existing TLS/netloop integration tests.

---

## 4. Sessions — the convergence point (ARCH-58)

However identity was proven (local password, OIDC token, or an existing session
token on reconnect), the daemon then does the same thing:

1. **Provision/look-up the user** (`users` table). `users.subject` is the unique
   identity key, namespaced by source: `oidc:<issuer>|<sub>` or `local:<username>`.
   OIDC users are provisioned just-in-time on first login (role defaults to
   `member`; a configured bootstrap subject becomes `owner`). Local users are
   created by invite (§2).
2. **Mint a session:** a random 32-byte token, returned to the client. The daemon
   stores only **`SHA-256(token)`** (so a database leak does not expose live
   sessions), with `user_id`, `created_at`, `expires_at`, `last_seen`, and an
   optional device label (`sessions` table).
3. **Return `AUTH_OK`** with the session token, its expiry, the user id, and the
   user's role.

- **Reconnect (REQ-100):** the client re-presents its session token
  (`AUTH{method=session}`); the daemon hashes and looks it up, and resumes
  without a full re-auth. The session lifetime is the daemon's to set (REQ-181) —
  it is no longer tied to a provider token's expiry.
- **Revocation (REQ-182):** "log out" / "log out other devices" deletes the
  relevant `sessions` row(s); the next protocol interaction on a revoked session
  fails. This local revocation is exactly what a stateless provider JWT cannot
  provide, and is the reason the daemon issues its own sessions.

---

## 5. Mode selection and the auth handshake

After `WELCOME` and before `AUTH`, the daemon sends **`AUTH_CHALLENGE`**
(PROTOCOL.md §4) advertising the accepted method(s) and, in OIDC mode, the
central authorize parameters plus this workspace's `audience`. The client renders
the appropriate login UI and replies with `AUTH`, whose `method` discriminator
selects the path:

- `local` — username + password (first login) → server verifies against
  `local_credentials`.
- `oidc` — the central-issued ES256 JWT (§3.3).
- `session` — a previously issued session token (reconnect).

The daemon answers with `AUTH_OK` (session established) or a fatal `ERROR`
(`AUTH_INVALID_TOKEN`, `AUTH_RATE_LIMITED`, `AUTH_REQUIRED`).

---

## 6. Roles (ARCH-60)

Every user holds exactly one tenant-level role — `owner`, `admin`, or `member`
(REQ-030) — stored as a `role` column on `users`. Enforcement lives in the
DB-writer handlers (the single write path); the policy predicates are pure and
unit-tested in `daemon/roles.c`.

- **Role changes (implemented):** `SET_ROLE` applies the policy — only owner/admin
  may change roles, only an owner may grant/revoke owner, an admin may only
  promote/keep members — refusing with `FORBIDDEN`.
- **≥1 owner invariant (implemented):** demoting the tenant's last owner is
  refused with `LAST_OWNER` (REQ-030), checked against a live `COUNT(*)` of owners.
- **Moderation delete (REQ-032, pending):** an admin/owner may delete (not edit)
  others' messages in a channel they belong to; the existing `messages.deleted_by`
  column records that it was a non-author deletion. (`oc_role_can_moderate` is in
  place; the delete operation itself is future work.)
- **Invite/remove (REQ-033, implemented):** only owner/admin may invite or remove
  tenant members (`oc_role_can_manage_members`), and only an owner may invite at
  or remove an admin/owner. Invite mints a single-use token (`invites`);
  `REMOVE_USER` locks the member out via `users.disabled` (migration 0003) and
  revokes their sessions/credentials rather than deleting the row. Channel-level
  invite/remove for private channels is any member of that channel (PROTOCOL.md
  §5.7). Wire frames: PROTOCOL.md §5.8.

---

## 7. Deferred

Tracked so the omissions are deliberate:

- **Both auth modes in one tenant** (e.g. OIDC for staff + local for
  contractors) — v1 is one mode per tenant.
- **Email magic-link** local login (needs outbound email; not air-gapped-safe).
- **Argon2** password hashing (a small vendored lib; PBKDF2 ships first).
- **Cert-vs-restore interaction** (the TOFU fingerprint changing when a database
  is restored onto a new box) — addressed by persisting the TLS identity in the
  database (ARCH-66b); orthogonal to auth.
