# OpenChime — Authentication

How users prove who they are, and how a session is established and revoked.
This is the authoritative design; it is cross-referenced from ARCHITECTURE.md
(ARCH-19, ARCH-55–ARCH-60), REQUIREMENTS.md (§1.2, §8.1), PROTOCOL.md (§4), and
SCHEMA.md (migration 0002).

**Status.** Design. As of this writing authentication is **stubbed** — the
daemon accepts any token string as a user subject (`process_auth` in
`src/dbwriter.c`) so the messaging vertical could be built. This document
defines the real design that replaces the stub; the implementation is a
follow-on milestone.

---

## 1. Two modes, one session

Authentication has **two modes, chosen per deployment** (ARCH-55). They differ
only in how identity is *proven*; both then **converge on a daemon-issued
session** (§4), so everything downstream — SEND, backfill, reconnect,
revocation — is identical regardless of mode.

| Deployment | Mode | How identity is proven |
|---|---|---|
| Self-hosted, standalone / air-gapped | **Local** | The daemon manages accounts + passwords itself. No external dependency. |
| Self-hosted, wants social login | **OIDC (via relay)** | The client logs in with Google/MS/Apple through the maintainer's central service, which re-issues a token the daemon trusts. |
| Hosted tier | **OIDC (in-house)** | Same central service, operated by the maintainer alongside the daemons. |

There is deliberately **no "point the daemon straight at your own IdP" mode**:
OIDC always routes through the central service. This keeps the daemon maximally
lean (it never speaks OIDC/JWKS/JSON) and means self-hosters never register
provider apps or hold provider credentials. A self-hoster who wants social login
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
  attempts (per account and per source) and answers excess attempts with
  `ERROR AUTH_RATE_LIMITED`. This is why REQ-191 lives with the auth design — it
  is squarely a local-password concern.

---

## 3. OIDC via the central service and relay (ARCH-56)

For social login (Google / Microsoft Entra / Apple, per REQ-021/022) without
each operator registering provider apps.

### 3.1 The idea

The **central service** (maintainer-controlled) is the OIDC Relying Party: it
holds the Google/MS/Apple client credentials, runs the login flow, and
**re-issues** a compact OpenChime identity token (§3.3) that the daemon trusts.
All the OIDC machinery — JSON, JWKS, provider quirks, key rotation — lives in
that service (a higher-level web service), **never in the C daemon**. The daemon
only verifies one signature against one configured public key.

### 3.2 The flow — the client is the courier

The central service **never connects to a self-hosted daemon** — the client
carries the token from center to daemon. This preserves the island model
(ARCH-4): no central→daemon link.

```
  ┌────────┐   1. authorize (central's client_id,        ┌──────────┐
  │ client │──────redirect_uri, PKCE, state=instance)───▶│ provider │
  │ (app)  │◀──────────────── 2. login ──────────────────│ (Google) │
  └───┬────┘                                              └────┬─────┘
      │                                    3. auth code        │
      │                                    ┌────────────────◀──┘
      │                                    ▼
      │                            ┌───────────────┐
      │  4. compact identity token │   central     │  exchanges code (secret),
      │◀───(audience=acme.example)─│   service      │  verifies, mints token
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
   encodes the **target instance** + a client nonce.
2. The user authenticates at the provider.
3. The provider redirects to central's callback with an auth code.
4. Central exchanges the code (with its client secret), verifies the provider
   token, extracts identity (subject, email, name), and mints a **compact
   identity token scoped to that instance** (`audience = acme.example`),
   returned to the *client*.
5. The client presents that token to the `acme.example` daemon in `AUTH`
   (method `oidc`). The daemon verifies it (§3.3) and mints a session.

### 3.3 The compact identity token (ARCH-57)

Central issues a **compact, field-encoded, ECDSA-P256-signed** token — not a
JSON JWT — so the daemon needs no JSON parser:

```
version (u16) | subject (str) | email (str) | display_name (str) |
audience (str) | issued_at_ms (u64) | expires_at_ms (u64) | signature (ECDSA-P256)
```

(Field encodings are the same primitives as the wire protocol, PROTOCOL.md §7.)
The daemon verifies the signature with mbedTLS (already linked; ECDSA-P256 is
fully supported — Ed25519 is not, hence P-256), then checks `audience` equals
its own configured instance id (so a token minted for one instance cannot be
replayed at another) and that it is unexpired. A JSON JWT was rejected: it would
force a JSON parser into the lean daemon for no benefit when both ends (central
and daemon) are ours — the same reasoning as ARCH-6.

### 3.4 Trust setup and dependency

- The daemon config (ARCH-26) carries **central's public key** (bundled/pinned
  in the OpenChime distribution — central is maintainer-controlled and stable)
  and this instance's **`audience` id**. A self-hoster enabling relay-OIDC
  registers their instance with central once to obtain that id.
- **Dependency is login-time only.** Once the daemon issues a session (§4), it
  never contacts central again; existing sessions survive a central outage. Only
  *new logins* need central up. Local mode has no central dependency at all.
- **Privacy tradeoff:** in relay-OIDC the central service sees *who* logs into
  which instance (identities, not message content — it never touches
  messages/channels). A self-hoster wanting zero maintainer visibility uses
  local mode.

### 3.5 Reconciling with the island model (REQ-041)

REQ-041 states the message/data path has no shared cross-tenant runtime
component, and that remains true — data isolation (REQ-040) is untouched. The
central OIDC service is a **login-time** shared component that exists **only in
OIDC mode**, brokers **identity only**, and is **absent entirely in local mode**.
REQ-041 is updated to scope its claim to the data path and acknowledge this
login-time broker.

### 3.6 The central service / relay (separate component)

The central service + relay are a **separate system** — a web service, not the C
daemon — and are built independently. Its contract with the daemon is narrow:

- run the Authorization-Code-+-PKCE flow against the configured providers;
- mint compact identity tokens (§3.3) signed by the key the daemon pins,
  audience-scoped to the requesting instance;
- maintain the instance registry (which `audience` ids are valid).

Until it exists, the daemon's OIDC path is tested with a **test issuer**:
generate an ECDSA-P256 keypair in the test, mint central-style tokens, and
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
central authorize parameters plus this instance's `audience`. The client renders
the appropriate login UI and replies with `AUTH`, whose `method` discriminator
selects the path:

- `local` — username + password (first login) → server verifies against
  `local_credentials`.
- `oidc` — the compact central token (§3.3).
- `session` — a previously issued session token (reconnect).

The daemon answers with `AUTH_OK` (session established) or a fatal `ERROR`
(`AUTH_INVALID_TOKEN`, `AUTH_RATE_LIMITED`, `AUTH_REQUIRED`).

---

## 6. Roles (ARCH-60)

Every user holds exactly one tenant-level role — `owner`, `admin`, or `member`
(REQ-030) — stored as a `role` column on `users`. Enforcement lives in the
DB-writer handlers (the single write path):

- **≥1 owner invariant:** an action that would remove or demote the last owner
  is refused (REQ-030).
- **Moderation delete (REQ-032):** an admin/owner may delete (not edit) others'
  messages in a channel they belong to; the existing `messages.deleted_by`
  column records that it was a non-author deletion.
- **Invite/remove (REQ-033):** only owner/admin may invite or remove tenant
  members; channel-level invite/remove for private channels is any member of
  that channel.

---

## 7. Deferred

Tracked so the omissions are deliberate:

- **Both auth modes in one tenant** (e.g. OIDC for staff + local for
  contractors) — v1 is one mode per tenant.
- **Email magic-link** local login (needs outbound email; not air-gapped-safe).
- **Argon2** password hashing (a small vendored lib; PBKDF2 ships first).
- **Cert-vs-restore interaction** (the TOFU fingerprint changes after
  restore-on-boot) — a separate, already-noted open item, orthogonal to auth.
