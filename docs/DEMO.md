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

## Run it — one command (recommended)

Assuming the sibling layout `../openchime-saas`:

```sh
docker compose -f docker-compose.federated.yml up --build
```

That stands up Postgres + the control plane (with the dev log push provider + the dev
enrollment-reserve shortcut) + a daemon that **enrolls hands-off**: the daemon writes its
`oce1` code to a shared volume (`OC_ENROLL_CODE_FILE`), an `enroll-init` step reserves it
via the dev endpoint, and the daemon activates (`OC_ENROLL_WAIT_SECS` lets it wait for the
reserve, then come up Active with push enabled). The daemon serves on `localhost:8443`.

Then drive a client against it:

```sh
make demo-client
build/demo_client 127.0.0.1 8443 bob   pw token apns tok-bob-demo
build/demo_client 127.0.0.1 8443 alice pw send  1 "hello from the federated stack"
```

Watch the control-plane log for:

```
push[Apns] would notify token tok-bob-demo (channel 1, ...)
```

That is the daemon→central push wire firing end to end (signed CP-12, contentless).

## Run it — scripted, against a control plane you started yourself

If you're running the control plane directly (e.g. `dotnet run` with
`Push:Log:Enabled=true`), `scripts/demo-federated.sh http://localhost:5176` does the same
flow against it (it scripts the real console reserve, no dev endpoint needed).

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
