# OpenChime — TLS

How the daemon terminates TLS and how clients trust it. Realizes ARCH-10 (TOFU
self-signed certs) and records the library choice (ARCH-51). The wire protocol
runs entirely inside this TLS session ([PROTOCOL.md](./PROTOCOL.md) §1).

## Library: vendored mbedTLS (ARCH-51)

TLS is mbedTLS (Apache-2.0), pinned at **3.6 (LTS)** and built from source by
`scripts/build_mbedtls.sh` into `third_party/` (gitignored). It was chosen over
OpenSSL and LibreSSL for fit with this project's constraints:

- Lean, pure-C, and small-footprint — matches the 256MB-per-tenant target
  (ARCH-4, REQ-210).
- Trivially vendored and cross-compiled to Alpine, Windows, and macOS, which the
  pure-C client (ARCH-11) needs across all three; no reliance on a system TLS.
- In-process X.509 certificate *writing* (`x509write`), so the daemon mints its
  own self-signed cert without shelling out to `openssl`.
- A verify callback for client-side TOFU pinning, and standard CA verification
  available for the webhook endpoint (ARCH-34).

**Why vendored, not the distro package:** Ubuntu ships mbedTLS 2.28 and Alpine
ships 3.6.x, whose APIs are not source-compatible. Pinning one version from
source gives local, CI, and the Docker image an identical library and avoids
`#ifdef` version shims. This mirrors how the sibling project openblocks vendors
raylib via a build script.

## Trust model (ARCH-10)

- **Daemon:** on first run generates a P-256 self-signed certificate and
  persists the PEM cert+key next to its data, reusing them on restart. No CA, no
  ACME, no renewal machinery.
- **Client:** TOFU pinning. On first connect the client records the cert's
  SHA-256 fingerprint; thereafter it requires an exact match. Because trust is
  the pin, not a CA chain, a cert under any hostname "just works" — which is what
  makes the free self-hoster vanity CNAME (ARCH-14) cost nothing to support.
- **Fingerprint** = SHA-256 of the certificate DER; it may be published
  out-of-band in `.well-known` metadata for verification (ARCH-10/14).

### How pinning is enforced with mbedTLS

There is no CA chain, so `MBEDTLS_SSL_VERIFY_REQUIRED` can't be used (it refuses
to run without one). Instead the client uses `VERIFY_OPTIONAL` plus a verify
callback that clears the chain-trust flag only when the leaf's fingerprint
matches the pin. After the handshake, `oc_tls_handshake` inspects
`mbedtls_ssl_get_verify_result()`: a pinned mismatch leaves `BADCERT_NOT_TRUSTED`
set and the connection is rejected. A **server** connection performs no peer
verification, so its result is exactly `BADCERT_SKIP_VERIFY`, which is masked off
— everything else is a real failure. (Both of these were live bugs found while
building `src/tls.c`; the test guards against regressions.)

## Non-blocking integration

`src/tls.c` is written for the epoll event loop (ARCH-22): custom BIO callbacks
translate socket `EAGAIN` into `MBEDTLS_ERR_SSL_WANT_READ/WRITE`, and
`oc_tls_handshake` / `oc_tls_read` / `oc_tls_write` surface those as
`OC_TLS_WANT_READ` / `OC_TLS_WANT_WRITE` for the caller to re-arm epoll interest.
A `recv()` of 0 (EOF) is returned to mbedTLS as a connection error rather than 0,
which would otherwise spin its input loop forever.

## Testing

`tests/itest_tls.c` (run by `make test`) is hermetic: it stands up a loopback
TLS server that generates a self-signed cert, connects a client that pins the
server's fingerprint, round-trips a byte through the tunnel, and asserts that a
**wrong** pin makes the handshake fail. The client-side integration scenarios
that TESTING.md §3.1 had gated on this library decision are now unblocked.
