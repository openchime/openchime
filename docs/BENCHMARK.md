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
| Message round-trip (persist + `SEND_ACK`), ~32 concurrent senders | **p50 ~4 ms, p90 ~15–30 ms, p99 ~20–40 ms** |
| Connection *setup* throughput | **~6–7 logins/sec** (see bottleneck below) |

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

**The latency phase measures a fraction of its intended load.** A 32-connection
burst reliably yields only ~10 authenticated connections, even with a 20-second
window. Auth is a 600k-iteration PBKDF2 on the single writer (~6–7 logins/s), so
some queueing is expected — but that alone does not explain the shortfall, and
the cause is **not yet diagnosed**. The reported latency figures are therefore
real but drawn from roughly a third of the intended concurrency.

**This was previously invisible.** `Scripts/bench.sh` piped the result through a
`sed` that extracted only the `rtt_ms...` portion, hiding the `connections_ok=`
prefix. Failed connections report `rtt 0.00`, so a degenerate run printed
`p50=0.00 p90=0.00 p99=0.00` — which reads like an outstanding result rather than
a broken measurement. The script now prints the whole line.
