# Local federated demo

Runs the whole federated system on one machine — the C daemon enrolled against the
.NET control plane (`openchime-saas`), with a client driving the two daemon→central
outbound wires (enrollment, ARCH-84; push, ARCH-85). Verified end to end.

## What it proves

1. **Enrollment** — the daemon generates a keypair + opaque audience, prints an `oce1.`
   code, the operator reserves it in the console, and the daemon proves possession and
   activates (ARCH-84 ↔ control-plane CP-5).
2. **Push** — a client registers a device token; a message send drives the daemon's push
   emitter, which signs a contentless batch with the enrollment key and POSTs it to the
   control-plane gateway, which verifies the signature (CP-12) and relays it (ARCH-85 ↔
   CP-13). The cross-language crypto (mbedTLS sign ↔ .NET verify) matches by construction.

## Run it

1. Start the control plane (from the `openchime-saas` repo) with the dev log push
   provider and Postgres reachable:

   ```sh
   docker compose up -d db
   ConnectionStrings__ControlPlane="Host=localhost;Port=5432;Database=openchime_cp;Username=openchime;Password=openchime_dev" \
   Database__MigrateOnStartup=true Push__Log__Enabled=true \
   ASPNETCORE_URLS=http://localhost:5176 \
   dotnet run --project src/OpenChime.ControlPlane.Web
   ```

2. From this repo:

   ```sh
   scripts/demo-federated.sh http://localhost:5176
   ```

   It builds the daemon + `demo_client`, boots the daemon federated, scripts the console
   reserve, re-boots to activate, then registers a token and sends a message. Watch the
   control-plane log for:

   ```
   push[Apns] would notify token tok-bob-demo (channel 1, ...)
   ```

## Notes / gotchas

- **`OPENCHIME_BLOB_DIR`** must point at a writable directory. Outside the container the
  default `/data/blobs` doesn't exist, and `oc_netloop_run` refuses to start without a
  usable blob store — the daemon logs `blob storage = local disk` then exits before
  `netloop: listening`. The script sets it to a temp dir.
- The demo uses **local auth** for the client (bootstrap users), independent of push.
  Push only needs the box **enrolled** (active audience + key) and `OC_PUSH_URL` set.
- **OIDC-relay login** against a live daemon is *not* covered here — it needs real Google
  credentials on the control plane. The mint↔verify contract is unit-tested on both sides.
- `demo_client` (`make demo-client`) is the flexible black-box tool: `token <apns|fcm>
  <tok>` or `send <channel> <text>` against a running daemon.
