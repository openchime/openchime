# Capacity Benchmark (REQ-210/211)

Measured answers to "how much memory does the daemon use, and how many
concurrent connections does it hold" — the two requirements that were previously
plausible-but-unmeasured. Reproduced by `Scripts/bench.sh`, which drives the
running daemon with the `tests/bench_load.c` load client and samples the
daemon's resident memory (`/proc/<pid>/status` `VmRSS`) while the load runs.

**Environment.** Localhost, single box, one daemon process (single-threaded net
loop + one DB-writer thread), mbedTLS, glibc. These are therefore an *upper
bound on latency* and a *lower bound on capacity per unit RAM*: a real
deployment on dedicated hardware with clients over a network sees no worse
memory behavior, and network latency dominates the sub-millisecond local
scheduling costs measured here.

## How to run

```
Scripts/bench.sh                 # default: idle memory at 50, 100, 200 conns + latency
Scripts/bench.sh 50 100 200 400  # custom connection counts
```

It bootstraps a set of local accounts (auth is a 600k-iteration PBKDF2, so it
keeps the count modest), starts a throwaway daemon, and prints a table of
connections vs. peak RSS vs. round-trip latency, tearing everything down after.

## Results

Representative figures (they vary a few KB/ms run to run):

| Metric | Value |
|--------|-------|
| Baseline daemon RSS (0 connections) | **~5 MB** |
| RSS per idle authenticated connection | **~50 KB** |
| 100 idle connections | ~10–12 MB total |
| 200 idle connections | ~15–18 MB total |
| Message round-trip (persist + `SEND_ACK`), 32 concurrent senders | **p50 ~2–3 ms, p90 ~80 ms, p99 ~130 ms** |
| Connection *setup* throughput | **~2 logins/sec** (see bottleneck below) |

> **Corrected 2026-07-19.** The two figures above were previously recorded as
> *p99 ~20–40 ms* and *~6–7 logins/sec*. Both were measured against a harness
> that was silently dropping most of its connections: `bench_load` used a 10 s
> read timeout while authentication is a 600k-iteration PBKDF2 serialized on the
> single writer, so a 32-connection burst only ever landed ~10 authenticated
> clients — and `Scripts/bench.sh` stripped the `connections_ok=` prefix that
> would have shown it. The latency percentiles were therefore measured at about a
> third of the intended concurrency, which flattered them by roughly 5×. With the
> timeout raised so all 32 connect, p99 is ~130 ms. The per-connection memory
> figure survived the correction largely unchanged (~58–60 KB), because peak RSS
> had included the connections that later timed out.

The per-connection cost is dominated by the mbedTLS session buffers plus the
per-connection frame reassembler and output buffer — flat and predictable, with
no per-connection growth beyond that at rest.

## What this means for capacity

Working from ~5 MB baseline + ~50 KB per idle connection:

- **By memory alone:** `(256 − 5) MB ÷ 50 KB ≈ ~5,000 idle connections` before
  RAM is the constraint in the lean profile (REQ-210).
- **The binding limit is the fd cap, not memory:** `OC_NETLOOP_MAX_FD = 4096`
  (a compile-time constant in `daemon/netloop.c`) caps the daemon at ~4,000
  connections regardless of RAM — and ~4,000 idle connections is still only
  ~200 MB, within the lean profile.
- **Comfortable planning number:** budget **~2,000–3,000 concurrent connections**
  on a 256 MB box, reserving headroom for SQLite's page cache, attachment
  transfer buffers, and the per-connection output buffers that grow (up to a
  1 MiB cap) while a connection drains a broadcast burst — the ~50 KB figure is
  measured on *idle* connections.
- **In users:** at ~1–2 connections per user (laptop + phone) and the fact that
  not everyone is online at once, that is roughly **1,500–3,000 concurrent
  users**, backing a registered tenant of **5,000–10,000+**.

Relative to REQ-211's stated target — "low-hundreds of concurrent connections…
sufficient for the 50–100 target customer scale" — a single 256 MB box clears it
by **one to two orders of magnitude**. At the 50-user scale specifically
(~100 connections ≈ 10 MB total, ~4% of the lean profile), the daemon is nowhere
near any limit; it would run on a 64 MB box.

Scaling further needs no architecture change (REQ-210): raise
`OC_NETLOOP_MAX_FD` and move to the ~512 MB standard profile.

## The one bottleneck: connection setup

Steady-state connection *count* is cheap; connection *establishment* is not. Each
login runs a **600k-iteration PBKDF2-HMAC-SHA256** password verification
(OWASP-tier, `OC_PW_ITERATIONS` in `daemon/auth.h`), and — like all mutations —
it runs on the **single DB-writer thread** (ARCH-5). That serializes logins at
**~6–7 per second**.

This is a deliberate security cost paid once per login, not a memory or
steady-state limit: a box holding thousands of connections is fine, but a
**thundering-herd reconnect** (e.g. every client re-authenticating at once after
a daemon restart) drains at ~7/sec — ~7 minutes for 3,000 clients. If that ever
matters at scale, the fix is to parallelize the PBKDF2 across a small worker pool
off the single writer (the auth verification is CPU-bound and independent per
login), not to add RAM. It is a non-issue at the low-hundreds target scale
(50 logins ≈ 8 seconds).

## Caveats

- Idle-connection memory; active fan-out uses more (bounded by the 1 MiB
  per-connection output cap and recoverable via reconnect + backfill).
- Localhost measurement — excludes real network RTT (which would dominate the
  ~4 ms local figure) and NIC/kernel socket-buffer memory under real load.
- No periodic large-scale soak test yet; these are point-in-time measurements.

## Slow-backend isolation (ARCH-69)

`itest_slow_blob` (in `make test`) answers the question the transfer pool exists
for and that nothing previously tested: **does a slow blob backend stall message
delivery?** Every earlier test ran against the local filesystem, where a blob
operation completes in microseconds — the ~100 ms/op regime the pool was built
for was never exercised.

The test points the daemon's S3 backend at a loopback endpoint that dribbles a
download 16 KB at a time with a 120 ms delay between pieces, then measures one
client's message round-trips while another client's download crawls through it.

```
bob round-trip: idle median 57ms | during slow-download median 52ms, max 62ms
backend 120 ms/op, 3 slow segment(s) served DURING the measurement
  (= 360 ms of backend stall the loop did not absorb)
```

**The test discriminates.** Temporarily reverting `download_pump` to read inline
on the epoll thread (pre-ARCH-69 behavior) makes the run **time out entirely** —
the loop freezes on the slow reads and the daemon stops serving anyone. A test
that cannot fail proves nothing, so this was verified rather than assumed.

Two implementation notes worth keeping, both learned by getting it wrong first:

- The slow path must be a **download**, not an upload. Making a *write* block
  requires exceeding the sender's socket send buffer (~2.5 MB), so an
  upload-based version needs a multi-megabyte payload and runs for minutes. A
  slow *reader* blocks trivially — there is no data yet.
- The concurrent client must run on **its own thread**. Ticking it between the
  measured client's sends makes the two barely overlap, and the measurement
  becomes a no-op that passes either way.

## Maintenance-pass overhead (ARCH-78)

Running the storage maintenance pass every 200 ms — 25× more often than the
5-minute default — against the same load shows no measurable cost:

| | baseline | maint every 200 ms |
|---|---|---|
| Daemon RSS | 5.0 MB | 5.1 MB |
| KB per connection | 55–57 | 52–65 |
| Round-trip latency | unchanged | unchanged |

## Known harness limitations

**Diagnosed and fixed.** The shortfall was `bench_load`'s own 10-second read
timeout, not a daemon limit. Authentication is a 600k-iteration PBKDF2 that runs
**serialized on the single writer thread**, measured here at **~2 logins/sec**
(≈500 ms each — the expected cost of 600k iterations). A burst of N clients
therefore takes N/2 seconds to drain, so everything past roughly the 20th client
gave up mid-authentication and was counted as a connection failure.

The knee is sharp and reproducible: 8/8 and 16/16 connect, 24 drops to 22/24.

Three fixes, all in the harness — the daemon was behaving exactly as designed:

- `bench_load`'s read timeout is now 180 s, so the auth ramp is never the limit.
- `Scripts/bench.sh` prints the whole result line. It previously piped through a
  `sed` that kept only `rtt_ms...` and dropped the `connections_ok=` prefix —
  and failed connections report `rtt 0.00`, so a degenerate run printed
  `p50=0.00 p90=0.00 p99=0.00`, which reads like an outstanding result rather
  than no result at all.
- The memory table now reports **requested vs connected** and divides by the
  connections that actually established, rather than by the number requested.

**The real constraint this exposes** is that connection setup is bounded at
~2/sec by design (REQ-191 wants PBKDF2 expensive). A server restart with a few
hundred clients reconnecting takes minutes to fully re-authenticate them, and
session-token reconnect (ARCH-58) — which skips PBKDF2 entirely — is what makes
that tolerable in practice. Worth remembering before quoting a connection-count
capacity number: the daemon *holds* thousands of connections, but *establishes*
them at two per second.
