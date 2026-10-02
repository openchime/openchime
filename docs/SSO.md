# Signing in with your own provider

A daemon can sign people in with the organization's own OpenID Connect provider
— Google Workspace, Microsoft Entra ID, Okta, Keycloak, or any other that
publishes discovery — with nothing between the two but the person's browser. The
daemon is the relying party: it holds the client registration, exchanges the code
and checks the ID token itself. How it works is AUTH.md §8.5; this is the
operator's half.

## What every provider needs

- **An application of the desktop or native kind**, using the authorization code
  flow with PKCE. The client signs in in the system browser and listens on this
  machine's loopback, so the registration is a desktop one even though the
  daemon, not the desktop, redeems the code.
- **A loopback redirect, any port, no path.** The client chooses a free port each
  time and the redirect is `http://127.0.0.1:<port>` — or `http://localhost:<port>`
  with `redirect=localhost`, for a provider that registers that name. The
  provider has to accept any port on the address registered (RFC 8252 §7.3).
- **Scopes** `openid email profile`.
- **The issuer exactly as the provider's discovery document states it.** The
  daemon fetches `<issuer>/.well-known/openid-configuration` and refuses a
  document whose `issuer` differs by a character.
- **The daemon reaching the provider** over HTTPS at boot and at each sign-in. A
  provider whose certificate chains to a private root needs that root in
  `OPENCHIME_EXTRA_CA`.

Then, in the daemon's environment (CONFIG.md):

```
OPENCHIME_OIDC_CONNECT_1=label=Acme SSO;issuer=https://…;client_id=…;secret_file=/etc/openchime/sso.secret
OPENCHIME_OIDC_ALLOW=owner:dana@acme.example,domain:acme.example
```

The secret file holds the client secret and nothing else; leave `secret_file` out
for a public client. Up to four connections may be named, `_1` to `_4`, and each
is its own button in the client, with its label. Who may join is the join rules
of AUTH.md §8.4, the same as for the relay.

## Google Workspace

- Google Cloud console → APIs & Services → Credentials → Create credentials →
  OAuth client ID, type **Desktop app**. A desktop client accepts any loopback
  port on `127.0.0.1` with no redirect registered.
- The OAuth consent screen's user type **Internal** limits sign-in to the
  Workspace's own accounts. External works too, with the join rules deciding.
- `issuer=https://accounts.google.com`. Google gives a desktop client a secret;
  put it in the secret file.
- `tenant:google:<your domain>` admits the Workspace's people by its hosted
  domain.

## Microsoft Entra ID

- Entra admin center → App registrations → New registration, **single tenant**.
  Under Authentication, add the platform **Mobile and desktop applications** with
  the redirect URI `http://localhost`. Entra ignores the port of a `localhost`
  redirect.
- `issuer=https://login.microsoftonline.com/<tenant id>/v2.0`. The `common` and
  `organizations` endpoints publish a templated issuer, which no ID token
  carries, so a connection names one tenant.
- `subject=oid`, so a person is keyed as the relay keys them and a `subject:`
  owner rule names the same identity either way; `redirect=localhost`, to match
  the registration. A desktop registration is a public client: no secret file.
- `tenant:microsoft:<tenant id>` admits the tenant's people. Entra vouches for a
  work account's address only with the optional claim `xms_edov` (Token
  configuration → Add optional claim → ID → `xms_edov`), so without it an owner
  is named by `subject:` rather than `owner:`, and `domain:` rules admit nobody.

## Okta, Keycloak and others

- Register a **native** application with the authorization code grant and PKCE,
  and a loopback redirect `http://127.0.0.1`. Check the provider's documentation
  for how it treats the port: the daemon cannot sign anyone in through a provider
  that matches the port exactly.
- `issuer=` the discovery issuer: for Okta the authorization server's
  (`https://<org>.okta.com/oauth2/default`, or the org's own for the org
  server); for Keycloak `https://<host>/realms/<realm>`.
- Such a provider has no tenant the join rules can name, so people are admitted by
  `domain:` on an address the provider marks `email_verified`, by invite, or
  named one at a time with `subject:<issuer>|<sub>`.

## What it cannot do

- **A terminal with no browser on its machine** cannot use a direct connection;
  it signs in by device code (AUTH.md §8.11) with a local account.
- **Revocation at the provider** takes effect at the next sign-in, not at once:
  a session the daemon granted lasts its own lifetime (AUTH.md §4). Remove the
  member in OpenChime as well.
