# OpenChime — A Managed Box

What the control plane writes into a daemon's environment when it provisions a
managed workspace (ARCH-76, model 3), and nothing else: an operator never sets
these, and a daemon started with one of them and no ticket refuses to start,
naming this page. The settings an operator makes are in [CONFIG.md](./CONFIG.md).

## The rows

| Variable | Meaning |
|---|---|
| `OPENCHIME_MANAGED_TICKET` | The one-time claim ticket (AUTH.md §8.7). Its presence is what makes the box managed: the deployment model is derived from it, the certificate comes through central unless `OPENCHIME_TLS` says otherwise, and the rows below may be set. Needs `OPENCHIME_ENROLL_URL` and `OPENCHIME_MANAGED_AUDIENCE` beside it. |
| `OPENCHIME_MANAGED_AUDIENCE` | The audience central minted for the workspace. The daemon adopts it, generates its own key and claims the binding; a database holding a different audience stops the boot rather than becoming two workspaces. |
| `OPENCHIME_MANAGED_RELAY_KEY` | A relay public key, PEM, pinned beneath the keys the relay publishes at `<enroll origin>/oidc/jwks` (AUTH.md §3.3). Optional: the published keys alone are the ordinary case. |
| `OPENCHIME_MANAGED_MAX_USERS` | The plan's registered-user cap (CP-7); `0` is unlimited. |
| `OPENCHIME_MANAGED_ADDRESS` | The workspace's canonical address when it has moved and still answers at the old one; clients re-file it under this (PROTOCOL.md §4.3b), and the certificate through central is asked for both names during the move (AUTH.md §8.9). |
| `OPENCHIME_MANAGED_AUDIO_ADVERTISE_PORT` | The UDP port clients are told for calls, where a front door maps it (AUDIO.md §4); `0` is the bound one. |
| `OPENCHIME_MANAGED_AUDIO_TOKEN_PREFIX` | Hex bytes, at most 16, that lead every call relay token, so the front door can route by them. |

With them, the control plane sets the operator rows a managed box needs as any
deployment would: `OPENCHIME_ENROLL_URL` (central's own origin, which is also
the relay's issuer), `OPENCHIME_AUTH=relay`, `OPENCHIME_JOIN` (an `owner:` or
`subject:` rule from the owner's console sign-in, AUTH.md §8.4),
`OPENCHIME_PUSH_URL`, `OPENCHIME_INVITE_MAIL=on`, `OPENCHIME_WORKSPACE_NAME`,
`OPENCHIME_DATA_DIR`, `OPENCHIME_PORT` and `OPENCHIME_HEALTH_PORT`.

## What is derived

- **The deployment model** is `managed` when the ticket is set, `federated`
  when only `OPENCHIME_ENROLL_URL` is, else `standalone`; nothing declares it.
- **The relay** needs no issuer, key file or audience of its own: the issuer
  is the enroll URL's origin, the keys are fetched from it, and the audience is
  the enrollment's — which on a managed box is the one above.
- **The certificate** comes through central (`OPENCHIME_TLS=central`) unless
  told otherwise.
- **The claim** is made once the daemon serves and retried for two minutes
  (`OC_CLAIM_WAIT_SECS`) while central cannot be reached; a refused ticket is
  not retried.

## Development mode

Development mode (`OPENCHIME_MODE=development`) accepts the managed rows without the ticket, so
the fixtures can stand in for a managed box (a user cap in the menu, a relay
port nothing answers on); it still never enrols anywhere but loopback.
