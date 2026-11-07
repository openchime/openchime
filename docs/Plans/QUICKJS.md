# QuickJS in the daemon — what it costs and where its limits hold

A research note. In-workspace apps are to run on QuickJS inside the daemon's
one process (ARCH-28), within the lean memory profile of about 256 MB
(REQ-210). Their sandbox depends on the following:
- a memory cap for each app;
- a CPU deadline enforced through QuickJS's interrupt handler;
- surviving an app that runs away or runs out of memory;
- room for many small apps.

This note measures those on both maintained lines of QuickJS and ends in a
recommendation. The decision to run apps in-process is not reopened. WASM is
not measured.

## 1. Method

- **Lines and releases:**
  - Bellard's QuickJS **2026-06-04** (`bellard.org/quickjs`);
  - **quickjs-ng v0.18.0** (2026-11-01, `github.com/quickjs-ng/quickjs`).

  Both are MIT-licensed, which VENDORS.md's rule allows.
- **Build:** each line's engine sources were compiled with the daemon's compiler and level: `cc` (GCC 13.3), `-std=gnu11 -O2 -D_GNU_SOURCE`. The sources were `quickjs.c`, `libregexp.c`, `libunicode.c` and `dtoa.c`, plus `cutils.c` for Bellard's. `quickjs-libc.c`, which supplies the `std` and `os` modules with file and process access, was not built and is not linked.
- **Harness:** one C program, built against each line. The two APIs differ for this purpose only in the interrupt handler's first argument (a `JSRuntime *` in Bellard's line, a `JSContext *` in quickjs-ng). The harness was not committed.
- **The app** used throughout is a listener-style integration of about 30 lines:
  - message, mention, reaction, command, button and modal handlers;
  - a `dispatch(json)` entry point that parses a Bolt-shaped event payload and returns the calls the app makes as JSON.
- **The event** is a 635-byte message payload in the shape `docs/Plans/BOLT.md` §1.4 describes.
- **A minimal context** is `JS_NewContextRaw` plus the base objects, `eval`, RegExp, JSON, Map/Set and Promise. A full context is `JS_NewContext`.
- **Machine:** 13th Gen Intel Core i5-1335U (12 logical CPUs: 2 performance cores with 2 threads each, 8 efficiency cores), WSL2, 4.9 GB RAM. Times are wall-clock from `CLOCK_MONOTONIC`. Resident sizes come from `/proc/self/statm`, read after `malloc_trim(0)`. Figures vary by machine; the shape of the results is what this note relies on.

## 2. Results

### 2.1 Code size

| | Bellard 2026-06-04 | quickjs-ng 0.18.0 |
|---|---|---|
| `.text` | 881 537 B | 1 053 156 B |
| `.data` + `.bss` | 24 116 B | 29 932 B |

The daemon grows by under 1 MB with Bellard's line, and by about 1.1 MB with quickjs-ng.

### 2.2 Memory per app, one runtime each

| | Bellard | quickjs-ng |
|---|---|---|
| Runtime + minimal context, empty (QuickJS's own count, `malloc_size`) | 127 KB | 78 KB |
| Runtime + minimal context, app loaded, resident per app (100 apps) | **152 KB** | **163 KB** |
| Runtime + full context, app loaded, resident per app (100 apps) | 175 KB | 191 KB |
| 500 apps, minimal context, resident total | 75 MB | 80 MB |
| Resident after freeing every runtime | within 50 KB of before | within 60 KB of before |

The first runtime in a process costs about 1 MB more (964 KB and 1120 KB with
a full context), most of it the engine's code paged in once. A minimal context
saves 20–30 KB an app over a full one.

### 2.3 Start-up

The figure is a runtime, a minimal context and the app loaded, averaged over 200.

| | Bellard | quickjs-ng |
|---|---|---|
| From source | 0.14 ms | 0.17 ms |
| From precompiled bytecode | 0.045 ms | 0.062 ms |
| Bytecode size | 4.5 KB | 4.4 KB |

### 2.4 Calling a handler

`dispatch` was called 100 000 times with the 635-byte event, each call
parsing it, matching a listener, and building and serialising the reply.

| | Bellard | quickjs-ng |
|---|---|---|
| Per call | 6.5 µs | 7.6 µs |
| Calls per second, one thread | 155 000 | 132 000 |

### 2.5 The interrupt handler as a CPU deadline

The handler returns "stop" once a 50 ms deadline has passed. QuickJS calls it
every 10 000 bytecode steps (`JS_INTERRUPT_COUNTER_INIT`), and also from the
regular-expression engine (`lre_check_timeout`). The overrun is the time past
the deadline before control came back.

| Case (memory cap) | Bellard: ran / overrun | quickjs-ng: ran / overrun | Stopped? |
|---|---|---|---|
| Busy loop (64 MB) | 50.0 / 0.0 ms | 50.0 / 0.0 ms | yes |
| Catastrophically backtracking RegExp (64 MB) | 50.1 / 0.1 ms | 50.1 / 0.1 ms | yes |
| `sort` of 10⁶ with a JS comparator (64 MB) | 52.3 / 2.3 ms | 52.3 / 2.3 ms | yes |
| `replace(/b/g, …)` over 16 MB (256 MB) | 58.6 / 8.6 ms | 54.3 / 4.3 ms | yes |
| Default `sort` of 10⁶ (128 MB) | 422 / **372 ms** | 457 / **407 ms** | no, ran to the end |
| Building and `JSON.parse` of 20 MB (256 MB) | 216 / **166 ms** | 195 / **145 ms** | no |
| `Array(5e6).join` (128 MB) | 110 / **60 ms** | 126 / **76 ms** | no |
| `repeat` to past the cap (64 MB) | out of memory at once | out of memory at once | — |

**JavaScript code is stopped within about 2 ms, and regular expressions within
0.1 ms.** Long **native** operations (the default sort, `JSON.parse`, `join`)
make no interrupt checks, so they run to the end. They can only be as long as
the memory cap lets their input be. Under a **16 MB** cap, the longest that fit
were:

| Under a 16 MB cap | Bellard | quickjs-ng |
|---|---|---|
| `JSON.parse` of the largest input that fit | 4 MB in 42.6 ms | 2 MB in 18.4 ms (4 MB: out of memory) |
| Default `sort`, the largest that fit | 10⁵ in 26.5 ms | 10⁵ in 31.4 ms |
| `join`, the largest that fit | 5·10⁵ in 7.0 ms | 5·10⁵ in 9.1 ms |

A native operation can overrun a deadline by at most its own duration, so
under a 16 MB cap the overrun is **at most about 45 ms** on this machine. Under 64 MB it can overrun by about 400 ms.

The app **cannot swallow the interrupt** in either line. A `catch` that loops
again, and a `finally` that `continue`s, were both stopped at the deadline with
`InternalError: interrupted`.

### 2.6 Out of memory and stack overflow

| | Bellard | quickjs-ng |
|---|---|---|
| Memory cap (16 MB) reached | `InternalError: out of memory`, **catchable in JavaScript**: an app caught it and tried again five times, each failing at the cap | `InternalError: out of memory`, **not catchable** |
| Stack cap (256 KB) reached | `InternalError: stack overflow`, catchable | `RangeError: Maximum call stack size exceeded`, catchable |
| Runtime usable after either | yes | yes |
| Memory returned when the runtime is freed | yes (peak +16 MB, then +0) | yes (peak +16 MB, then +8 KB) |

The 250 KB that stayed resident after the stack test is the calling thread's own stack, which the recursion touched, not heap.

At the very edge of the cap, an allocation failure while throwing can make the
thrown value `null` rather than an error object. Both lines showed this once.

In neither line can an app raise its memory above the cap or break the
runtime. The difference is only whether its own code sees the failure.

### 2.7 Threads

Eight runtimes on eight threads at once gave identical results to one run alone:

| | Bellard | quickjs-ng |
|---|---|---|
| One runtime alone | 358 ms | 418 ms |
| Eight at once, wall time | 624 ms (4.6× throughput) | 947 ms (3.5×) |

The speed-up is limited by this machine's mix of performance and efficiency cores. A runtime is not thread-safe. It must be used by one thread at a time, and both lines provide `JS_UpdateStackTop` for a runtime that moves between threads. Moving one between threads was not measured.

### 2.8 Upkeep and security

| | Bellard | quickjs-ng |
|---|---|---|
| Releases since 2025 | 2025-04-26, 2025-09-13, 2026-06-04 | 14, from 0.10.0 (2025-05) to 0.18.0 (2026-11-01) |
| Security reports | no stated policy | public issue tracker only; "no longer accept private security reports nor issue CVEs" (`SECURITY.md`) |
| Fuzzing harness in the tree | none | `fuzz.c` (libFuzzer) |
| Bytecode from outside | not validated | out of scope by policy: loading untrusted bytecode "is equivalent to executing untrusted native code" (`SECURITY.md`) |
| Custom allocator hook (`JS_NewRuntime2`) | yes | yes |

## 3. Comparison

|  | Bellard 2026-06-04 | quickjs-ng 0.18.0 |
|---|---|---|
| Code | 0.9 MB | 1.1 MB |
| Resident per small app | 152 KB | 163 KB |
| Start from bytecode | 0.045 ms | 0.062 ms |
| Handler call | 6.5 µs | 7.6 µs |
| JS stopped at a deadline | ≤ 2.3 ms | ≤ 2.3 ms |
| Longest native operation under 16 MB (its worst overrun) | ~43 ms | ~31 ms |
| Out of memory | catchable by the app | not catchable |
| Release cadence | about two a year | about monthly |
| Fuzzing harness | no | yes |
| Licence | MIT | MIT |

Bellard's line is about 10 % smaller, lighter and faster. quickjs-ng is
released far more often and ships a fuzzing harness. It also treats running
out of memory as final, which is the stricter behaviour for a sandbox. Neither
offers private disclosure of vulnerabilities. Their APIs are close enough that
the harness built against both with one `typedef`, so switching later is cheap.

## 4. Recommendation

1. **Adopt quickjs-ng, pinned at 0.18.0**, and track its releases.
   - The deciding reasons are upkeep and sandbox behaviour: monthly releases, an in-tree fuzzing harness, and out-of-memory that an app cannot catch.
   - The cost against Bellard's line is about 170 KB of code, about 11 KB per app and about 1 µs per call. That matters less than staying current with an engine that runs untrusted code in the process that holds the tenant's data.
   - Because security reports are public, the daemon should follow the project's issue tracker, as well as its releases.
2. **One runtime per app, not contexts sharing one runtime.**
   - QuickJS sets the memory cap, the stack cap and the interrupt handler **per runtime**. Contexts in one runtime share one heap and one collector, so one app could exhaust the memory of all of them.
   - A runtime per app costs about 160 KB resident, already counted in §2.2.
   - Separate runtimes also run in parallel on separate threads (§2.7).
3. **Minimal contexts.** Build each app's context from `JS_NewContextRaw` with only the intrinsics the runtime's API needs. Never link `quickjs-libc`.
4. **Compile in the daemon, cache the bytecode, and never accept bytecode from a package.**
   - A package carries source. The daemon compiles it at install, signs or hashes the bytecode, and stores it.
   - Each start then costs about 0.06 ms (§2.3).
   - Bytecode from outside is outside both lines' security model (§2.8).
5. **Run apps on a small pool of worker threads, never on the net loop or the database writer.** A native operation can overrun its deadline by tens of milliseconds (§2.5). On a worker, that delays only that app, never chat.
6. **Default limits per app, proposed for review:**
   - **heap 16 MB**, which bounds native overruns to about 45 ms (§2.5);
   - **stack 256 KB**;
   - **CPU deadline 100 ms an invocation**, counted only while JavaScript runs, so time spent waiting on an outbound call does not count;
   - **a CPU budget of 1 s per minute.**
   
   An app that exceeds a deadline or its budget three times in an hour is quarantined.
7. **A combined heap budget for all apps**, enforced through the allocator hook (`JS_NewRuntime2`), so that many apps at their caps cannot exhaust the daemon together. Proposed: **48 MB in the lean profile and 160 MB in the standard one.**
8. **How many apps fit.** The daemon is about 4 MB with no connections and about 65 MB with 200 idle connections (TESTING.md, load bench).
   - Idle apps at about 160 KB each, plus the combined budget for apps that are running, leave room for **about 150 apps installed** within the lean profile beside 200 connections: about 24 MB idle, plus 48 MB of working budget.
   - The standard profile has room for **about 500 apps**.

   A small workspace will install a handful, so the limit that matters in practice is the combined working budget, not the per-app cost.

## 5. The runtime's sandbox requirements, checked

| Requirement | Result |
|---|---|
| No ambient authority: only the generated host bindings | **Met, with a condition.** The engine itself has no file, network, process or environment access; all of that is in `quickjs-libc`, which must not be linked (§1). |
| One context and memory cap per app | **Not met as written.** QuickJS caps memory per runtime, not per context. What meets it: one runtime per app (§4.2), at the cost measured in §2.2. |
| CPU deadlines and budgets through the interrupt handler | **Met for JavaScript and regular expressions** (≤ 2.3 ms past the deadline, and the app cannot catch it). **Met with a condition for native built-ins,** which do not check: their overrun is bounded by the heap cap (about 45 ms at 16 MB). Condition: apps run on worker threads (§4.5), with the heap cap and the deadline sized together (§4.6). |
| Arguments validated at the boundary | Not measurable here. It belongs to the bindings. |
| Fuzzing | **Met with a condition.** quickjs-ng ships a fuzzing harness for the engine. The daemon's bindings and the app-loading path need harnesses of their own, in the same CI gate as the other parsers. |
| Runtime switch per workspace and per app; quarantine | Not an engine matter, but supported: a runtime can be stopped at any time by the interrupt handler, freed, and its memory returned (§2.6). |
| QuickJS kept current with security fixes | **Met with a condition** for quickjs-ng: about monthly releases. Fixes are reported publicly, so the daemon must watch the issue tracker as well as releases. |
