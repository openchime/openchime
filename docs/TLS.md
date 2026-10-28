# OpenChime — TLS

How the daemon terminates TLS, where its certificate comes from, and how clients
trust it. Realizes ARCH-10 and records the library choice (ARCH-51). The wire protocol
runs entirely inside this TLS session ([PROTOCOL.md](./PROTOCOL.md) §1).

## Library: vendored mbedTLS (ARCH-51)

TLS is mbedTLS (Apache-2.0), pinned at **3.6.2** (the 3.6 LTS line) and built from source by
`scripts/build/mbedtls.sh` into `third_party/` (gitignored). It was chosen over
OpenSSL and LibreSSL for fit with this project's constraints:

- Lean, pure-C, and small-footprint — matches the 256MB-per-tenant target
  (ARCH-4, REQ-210).
- Trivially vendored and portable across the platforms the client targets: the
  shared C app-core (ARCH-74) links it on every host, and each native frontend
  builds it with its own toolchain (the TUI on the host, like the daemon); no
  reliance on a system TLS.
- In-process X.509 *writing* (`x509write`): the daemon's self-signed identity,
  ACME's challenge certificate and its CSRs, without shelling out to `openssl`.
- A per-handshake certificate callback (`mbedtls_ssl_conf_cert_cb`), which runs
  once the whole ClientHello is read and so knows both the name asked for and
  the ALPN chosen.

**Thread safety.** The vendored build enables **`MBEDTLS_THREADING_C` +
`MBEDTLS_THREADING_PTHREAD`** (`scripts/build/mbedtls.sh`). OpenChime uses mbedTLS
from multiple threads — the client app-core runs a network thread per connection
and the test suite drives several TLS clients concurrently — and mbedTLS has
internal shared state that is not otherwise safe under concurrency; without the
threading layer, concurrent context setup/handshake races and fails
intermittently. The daemon's I/O threads each own their connections' contexts and
share only the read-only configuration and the thread-safe RNG (below). The final binaries already link `-lpthread`.

**Why vendored, not the distro package:** Ubuntu ships mbedTLS 2.28 and Alpine
ships 3.6.x, whose APIs are not source-compatible. Pinning one version from
source gives local, CI, and the container image an identical library and avoids
`#ifdef` version shims. The client's TUI libraries (termbox2, utf8proc) are
likewise vendored — as committed single-file source, both MIT (ARCH-75).

## Trust model (ARCH-10)

- **Client:** a daemon is verified as any HTTPS server is. The certificate must
  chain to a trusted root and name the workspace — the domain the client sends as
  SNI, or, for a workspace that is an address, the address among the
  certificate's iPAddress names (which is never sent as SNI, RFC 6066 §3). The
  trusted roots are the built-in ones (below) and **the operating system's**
  (`OPENCHIME_EXTRA_CA` is the daemon's, for its own outbound connections; no
  client reads it): Windows' ROOT store (`CertOpenSystemStoreW`), or the
  distribution's bundle (`/etc/ssl/certs/ca-certificates.crt`, then
  `/etc/pki/tls/certs/ca-bundle.crt`), which is where an organisation installs
  its internal CA. They are parsed once and shared by every client connection;
  setting new extra roots gives later connections a new set
  (`oc_tls_client_init_verify`).
- **A certificate no root vouches for** is accepted only by one of:
  - **the person's say-so.** The client shows its SHA-256 fingerprint ("check
    this with the workspace's administrator") and, if they trust it, keeps it
    with the workspace's credential; the next connection presents it without a
    question. A different certificate later is shown as a *change*, and not
    taken on the old trust.
  - **a published fingerprint.** The workspace's `.well-known` document
    (ARCH-14), fetched over verified HTTPS, may name its certificate's
    fingerprint; the certificate must then be that one or the connection is
    refused, in its own words ("this workspace publishes the certificate its
    server should present, and the server presented a different one"). Text that
    is not 32 bytes of hex is no fingerprint at all.

    The document is `https://<domain>/.well-known/openchime`, published by the
    domain's own web server (the daemon does not serve it), and fetched only for
    a workspace named by a domain with no `:port` and no
    `_openchime._tcp.<domain>` SRV record. It is a JSON object of at most 4096
    bytes, answered `200` directly (a redirect counts as no document):
    `{"port": 8443, "fingerprint": "3A:5F:…"}`. `port` is a bare integer
    1–65535, used instead of 443. `fingerprint` is a string, the SHA-256 of the
    daemon's certificate DER as 64 hex digits, with or without colons. Both are
    optional and other keys are ignored, but a `port` that is not such a number,
    or a `fingerprint` that is not a string, makes the workspace fail as
    malformed metadata (REQ-011).
  - **loopback.** A daemon on the same host is accepted: nothing can sit in that
    path, and a local daemon restarted with a fresh self-signed certificate would
    otherwise ask every time (`client/core/net.c:is_loopback`).

  Nothing is trusted merely for having been seen before. **A CA-issued
  certificate replaces a trust the person gave** a self-signed one: it is
  forgotten, and from then on only a root vouches.
- **The daemon** presents one certificate to every connection — the binary
  protocol and HTTP alike (ARCH-54; webhooks too, ARCH-34) — from the source it was told
  (`OPENCHIME_TLS`, "Certificates" below): its own self-signed identity,
  the operator's files, or a CA-issued certificate it obtains and renews.
- **Fingerprint** = SHA-256 of the certificate DER.

### How the client judges a certificate

The client verifies with `VERIFY_OPTIONAL` against the roots above and defers
the verdict (`oc_tls_conn_defer_verify`): the handshake completes, and
`oc_tls_conn_ca_trusted` then says whether the chain and the name held. If not,
`cert_judge` (`client/core/net.c`) applies the fallbacks in order — a published
fingerprint (decisive either way), a fingerprint the person trusted, loopback —
and otherwise ends the attempt with the fingerprint for the person to judge
(`OC_EV_CERT_UNTRUSTED`; `oc_client_trust_cert` accepts it). The sign-in probe
(`oc_net_probe_ex`) judges the same way, before anything is typed. A **server**
connection performs no peer verification, so its result is exactly
`BADCERT_SKIP_VERIFY`, which is masked off.

## Outbound HTTPS: the built-in roots

Where the daemon or a client is itself the HTTPS client of someone else's
service — S3, central, a linked page, a workspace's `.well-known`
document, an ACME CA — trust is an ordinary CA chain (ARCH-10).
`oc_tls_client_init_ca` verifies the chain and the hostname
(`MBEDTLS_SSL_VERIFY_REQUIRED`) against **Mozilla's trusted roots, compiled
into the binary**, plus, for the daemon, `OPENCHIME_EXTRA_CA`. The daemon never reads the host's
store, so what a daemon build trusts is the same on every distribution and in a
container with none; a client connecting to a daemon adds the operating
system's roots (above).

- **Where they come from.** `third_party/ca-roots/ca_roots.c` is generated by
  `scripts/gen/ca_roots.sh` from curl's extract of Mozilla's `certdata.txt`
  — the set Firefox trusts for websites — fetched by date and checked against a
  pinned SHA-256. The file is committed; the build fetches nothing. Each root
  carries its name as a comment, so a refresh's diff is the list of roots added
  and removed.
- **Why not the host's.** A static binary that reads trust material from the
  host trusts whatever that distribution's `ca-certificates` holds, or nothing
  in a container without it, and Windows keeps no bundle on disk at all. Built
  in, the set is a fact of the build and the same everywhere.
- **What it costs.** The set is a snapshot: a root Mozilla removes stays
  trusted until the next release carries the refresh (`scripts/gen/ca_roots.sh`).
- **A private CA.** `OPENCHIME_EXTRA_CA` names a PEM file whose roots the
  daemon trusts **as well**, never instead (`oc_tls_set_extra_ca`); a client
  trusts a private CA only from the operating system's store. The daemon reads
  it once at startup, and a file that is missing, empty, or holds a certificate
  that does not parse stops the boot, rather than trusting less than its
  operator wrote.

`tests/itest_tls.c` holds that every built-in root parses with the mbedTLS we
ship, that a server under a private CA is refused by the built-in roots alone
and accepted once its root is added, that the hostname is still checked, and
that a bad file is refused and leaves no extra roots behind.

## Non-blocking integration

TLS runs on the daemon's I/O threads (`daemon/ioloop.c`, ARCH-22), never on the
event loop: each connection belongs to one I/O thread for its life, so its
`mbedtls_ssl_context` is used by one thread only, while the server configuration
is shared read-only and its random generator is mbedTLS's thread-safe one
(`MBEDTLS_THREADING_C`). The loop sees plaintext frames.

`shared/tls.c` is written for an epoll loop: custom BIO callbacks
translate socket `EAGAIN` into `MBEDTLS_ERR_SSL_WANT_READ/WRITE`, and
`oc_tls_handshake` / `oc_tls_read` / `oc_tls_write` surface those as
`OC_TLS_WANT_READ` / `OC_TLS_WANT_WRITE` for the caller to re-arm epoll interest.
A `recv()` of 0 (EOF) is returned to mbedTLS as a connection error rather than 0,
which would otherwise spin its input loop forever.

## Certificates

`OPENCHIME_TLS` (CONFIG.md), one value, says where the daemon's certificate
comes from:

- **`self`** (the default): a P-256 self-signed certificate made on first run,
  kept in the identity database (ARCH-66b, `identity/identity.db`) and as
  `cert.pem` / `key.pem` in the data directory.
- **`file`**: the operator's own `cert.pem` and `key.pem` in the data directory
  (`OPENCHIME_DATA_DIR`), as given; refused at start if either is unreadable.
- **`acme:<names>[;email=<address>][;directory=<url>]`**: ACME (RFC 8555) for
  the comma-separated DNS names, from the CA at `directory` — Let's Encrypt
  unless given, or an internal CA's ACME directory for a network with no
  internet — with `email` as the account's contact. The CA connects to port
  443 at each name: set `OPENCHIME_PORT=443` (the packaged unit holds
  `CAP_NET_BIND_SERVICE`) or forward 443 to the daemon's port, and point each
  name's DNS at the box.
- **`central`**: through central, for a name under the service suffix (AUTH.md
  §8.9) — the default on a managed box, and refused without an enrollment.

Until a CA-issued certificate is obtained the daemon presents its self-signed
one; once one is, it is kept (migration 0049, in the identity since 0064) and
presented at once on every restart that still has the same source and names and
has not run out. The identity is replicated apart from the data (ARCH-3), so a
box whose data is begun again or restored keeps its certificate rather than
asking a CA for another.

**ACME, with TLS-ALPN-01 (RFC 8737).** A worker thread (`daemon/certs.c`), started
once the listener accepts, runs `oc_acme_issue` (`daemon/acme.c`): the directory
and a nonce; the account — made once, with `termsOfServiceAgreed` (turning ACME on
is the operator's agreement to the CA's subscriber terms), and kept with the
directory it belongs to, so a different CA gets a different account; an order for
the names; for each name, a **challenge certificate** — self-signed, naming it, with a
random 16-byte serial whose first byte is 0x40–0x7f (a positive, minimal DER
INTEGER: a CA's parser refuses a leading zero byte, and one serial in 256 drawn
without that rule would have one), carrying the critical acmeIdentifier extension (1.3.6.1.5.5.7.1.31) that holds
SHA-256 of the key authorization — installed in the listener, then the challenge
posted and the authorization polled; the finalization with a CSR for a new
P-256 key; and the chain. Every request is a JWS (ES256) with a fresh nonce; a
`badNonce` is retried with the one it brings. The CA validates by connecting to
port **443** at the name with ALPN `acme-tls/1` only: the listener then presents
the challenge certificate for that name — only to that ALPN, and a validation
naming a name with no challenge pending is refused — and ends the connection after
the handshake (`daemon/ioloop.c`). A challenge certificate is parsed with an
extension callback that admits exactly the acmeIdentifier extension; any other
unknown critical extension is still refused (RFC 5280).

**Renewal and the swap.** The worker renews each certificate at a random moment
in a window, drawn once for that certificate so daemons issued together renew
apart: the CA's, where it offers **ACME Renewal Information** (RFC 9773) —
asked with the certificate's identifier (its authority key identifier and serial)
at the interval the CA's `Retry-After` names, kept between one and twelve
hours (six when it names none) — and
otherwise its own, from 60% to two-thirds of the life, which leaves the third
the CA asks for. A window the CA has already opened is renewed at once: that is
how an incident or a revocation reaches the daemon. A replacement order names
the certificate it replaces (`replaces`, §5). A failure is retried after a
minute, ten, a hundred, then daily, as Let's Encrypt's integration guide asks.
And, as it also asks, every failure reaches a person: besides the log, the worker
raises it for the workspace's owners and admins (PROTOCOL.md §5.16c) — a
certificate not obtained or renewed, with the CA's or the network's words; the CA
asking for one to be replaced now; one within a week of expiry and still not
renewed — and the next certificate obtained clears all three.
Through central, where there is no ARI, the daemon's own window applies. A new
certificate is swapped in whole (`oc_tls_server_use`): each
handshake takes a reference to the certificate it presents
(`mbedtls_ssl_set_hs_own_cert`), so handshakes under way and connections already
up keep theirs, and a retired certificate is freed with the last connection
that holds it.

**Through central.** A daemon bound to central asks it for its names and sends a
CSR for them, both as signed machine requests (AUTH.md §8.9); central completes
the DNS-01 challenge in the zone it controls and returns the chain. A `202` asks
it to come back; a `4xx` is final.

**Renewing a certificate you provide (`file`).** With `acme:` or `central`
there is nothing to do: the worker renews and swaps in place, without a
restart. Your own certificate is read once, at start: nothing watches the files
and the daemon has no reload (it does not handle SIGHUP, which stops it). After
renewing, copy the new chain and key into the data directory and restart. Under
the packaged unit the files must be readable by its dynamic user, which has no
name to `chown` to, so give them the owner of the directory systemd made
(`/var/lib/openchime` is a link to `/var/lib/private/openchime`):

```
install -m 0600 fullchain.pem /var/lib/openchime/cert.pem
install -m 0600 privkey.pem   /var/lib/openchime/key.pem
chown --reference=/var/lib/private/openchime /var/lib/openchime/cert.pem /var/lib/openchime/key.pem
systemctl restart openchimed
```

A certbot `--deploy-hook` can run exactly this. A file the daemon cannot read
refuses the start (`OPENCHIME_TLS=file: needs … to be readable`).

## Behind a load balancer

The daemon terminates TLS itself and has to: native clients speak `oc/1` inside
its TLS on the same port as HTTP ([PROTOCOL.md](./PROTOCOL.md) §1.1) and judge
the certificate it presents, and ACME's TLS-ALPN-01 is answered by its own
listener. A front door is therefore a TCP (layer-4) pass-through, never a
TLS-terminating HTTP proxy. Forward TCP `OPENCHIME_PORT` unchanged, and UDP on
the same port number for calls ([AUDIO.md](./AUDIO.md) §4).

To keep clients' own addresses for the per-address connection cap and the
sign-in limits, have the forwarder send a **PROXY protocol v2** header (HAProxy
`send-proxy-v2`, or a cloud network load balancer's proxy protocol v2; not
nginx's `proxy_protocol`, which sends v1) and list its addresses in
`OPENCHIME_TRUSTED_PROXIES`. A connection from a listed peer must begin with a
v2 header (TCP over IPv4 or IPv6, or LOCAL for the forwarder's own health check)
or it is closed; a header from any other peer is never read. HTTP headers such
as `X-Forwarded-For` are never read, and the health port
(`OPENCHIME_HEALTH_PORT`) takes no PROXY header.

## Session resumption

The daemon issues **session tickets** (`mbedtls_ssl_ticket`): after a full
handshake it gives the client a ticket, sealed with AES-256-GCM under a key the
daemon makes at start and keeps only in memory, valid for a day
(`OC_TLS_TICKET_LIFETIME_S`). A client keeps the last ticket for its workspace
(`oc_tls_conn_resume`) and offers it when it reconnects; the daemon opens it and
resumes the session, skipping the certificate exchange and the signature a full
handshake costs, which is most of what a reconnect storm spends. A ticket the
daemon cannot open — it has restarted since, or the ticket has expired — costs
nothing: the handshake is simply a full one. The ticket context locks for itself,
so the I/O threads share it. The daemon counts resumptions in
`oc_tls_server.resumed`.

A resumed session was judged on the connection the ticket came from, and only
the daemon holding the ticket key can resume it. A client whose judgement
refuses a certificate forgets its ticket, so the next attempt is a full
handshake.

## Testing

`tests/itest_tls.c` (run by `make test`) is hermetic: it stands up a loopback
TLS server that generates a self-signed cert, connects a client that checks the
server's fingerprint, round-trips a byte through the tunnel, and asserts that a
**wrong** fingerprint makes the handshake fail; it also resumes a session with
the ticket the first connection was given, and checks that a restarted server —
a new ticket key — falls back to a full handshake that still succeeds. `tests/test_acme.c` runs ACME
against a fake CA that checks every JWS and nonce and validates as a CA does —
with an independent TLS client (OpenSSL's), since mbedTLS's own refuses the
challenge certificate's extension before anything can look at it — plus the
listener's choice of certificate, renewal with a connection kept through the
swap, and certificates through a fake central. Its fake CA also answers ACME
Renewal Information: a window already open renews at once, the replacement
naming the certificate it replaces by the identifier RFC 9773's own example
fixes; a window set later holds a renewal the daemon's own would have made; and a
CA failing every order is asked again after the base wait, ten times it, then at
the ceiling. The window picks and the retry ladder are checked as values, and the challenge
certificate's serial as DER on 32 of them. The suite resets its state at the
start of each run, so `OC_TEST_REPEAT` runs it again in one process. The worker's
alerts are recorded as it raises and clears them: each failed order raised under
one key, the CA's "replace now" raised and then cleared by the replacement, and a
certificate near expiry raised once for its day and cleared by its renewal.
`test_dbwriter` holds the store to one entry per failure, the counts, the
owners-and-admins-only list and acknowledgement and the bound; `itest_netloop`
sends it over the wire to an owner at sign-in and refuses a member. `test_client_core` covers the
client's judgement at this machine's LAN address, where loopback's exemption does
not apply, and that a restarted box resumes no session.
