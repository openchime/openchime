# Summaries

A summary of a channel or DM, made on the tenant's own box by a small model
(REQ-310–313, ARCH-116). This document is the design; the decision and what was
rejected are ARCH-116.

## 1. What a reader gets

A client that was told the `summarize` capability may ask for a summary of any
channel or DM the person can read, over one of four spans (PROTOCOL.md §5.16m):

| Scope | Span |
|---|---|
| Unread | From the message the reader last read to the end of their today |
| Week | The last seven of the reader's days, today included |
| Range | `[start_ms, end_ms)` as given, at most 31 days |
| Daily | The reader's yesterday, or every day since they were last about, up to a week |

Days are the reader's, by the time-zone offset their client reports
(`users.tz_offset_min`). The answer is the summary as JSON (§3); a summary not
yet made is made while the request waits.

## 2. From messages to a summary

**What is summarized.** People's messages: not integration posts
(`author_name` set), not call events (`kind` other than 0), not deleted ones.
Markup is reduced to plain text by the same code read-aloud uses
(`shared/speakable.c`).

**Units.** A top-level message with no replies is a unit; so is a whole thread,
root and replies, placed at its **last** reply. A thread therefore belongs to the
period of its last activity.

**Chunks** (`daemon/sum_core.c`). Units, in order of their place, are grouped
into chunks of whole messages. A new chunk starts when the next unit is more
than `SUM_GAP_MS` after the last, or would take the chunk past
`SUM_THRESHOLD_TOKENS`. A thread bigger than the limit is cut into chunks of its
own, whole messages each. Size is one estimate for every model — a token per
four bytes of rendered text — so where pieces are cut never depends on the model,
and appending messages never moves an earlier cut.

**The tree** (`daemon/sum_worker.c`). Each chunk is summarized. A big thread's
chunks are rolled up into a thread summary. A day's chunk and thread summaries
are rolled up into the day's. A longer span is its days rolled up. Wherever a
list of summaries is longer than `SUM_THRESHOLD_TOKENS`, it is cut into sections,
each rolled up, and the sections rolled up in turn. A day with nothing to say
adds nothing to a rollup.

**What the model reads.** A chunk:

```
Chat excerpt from #support (times UTC). People: P1 Ann, P2 Bob.
Indented lines are replies in a thread.

[m1] P1 (Fri 04 Sep 12:04): Ticket 6701 is open; Bob please look.
  [m2] P2 (Fri 04 Sep 12:10): On it.
```

A rollup lists each child's overview and items, numbered `i1`, `i2`, …, with
the people they name as `P#`. Each prompt ends with a line saying what each field
holds.

**What the model writes.** One shape at every level, held to it by a GBNF
grammar generated for the piece (`oc_sum_grammar`), which admits only the
piece's own `P#` and `m#` (or `i#`), at most `SUM_MAX_ITEMS` items of each kind
and `SUM_TEXT_MAX` characters of text, so an answer fits its budget of
`SUM_MAX_OUT` tokens; one that still runs past it is asked for again with room
for one item of each kind, rather than losing the piece:

```
{"overview": "...",
 "decisions": [{"text": "...", "by": ["P1"], "refs": ["m1"]}],
 "actions":   [{"who": "P2" | "team", "what": "...", "refs": ["m1"], "status": "open" | "done"}],
 "problems":  [{"text": "...", "refs": ["m2"], "status": "open" | "resolved"}],
 "facts":     [{"text": "...", "refs": ["m1"]}]}
```

**The check** (`oc_sum_check`). An item is dropped when a ref is not one of the
piece's, a person is not one of its people, or a run of digits in its text appears
in none of the sources it cites; a repeat of a kept item is dropped too. `P#` in
text becomes the person's name. What is kept is stored with real ids (§3).

## 3. The stored shape, and what clients get

A node's `body` is the checked shape with message ids for refs and user ids for
people (`who` 0 is the team):

```
{"overview": "Ann asked Bob about ticket 6701.",
 "decisions": [{"text": "...", "by": [10], "refs": [100]}],
 "actions":   [{"who": 11, "what": "look at ticket 6701", "refs": [100], "status": "open"}],
 "problems":  [...], "facts": [...]}
```

A rollup's refs are the union of the message ids its cited items cite, so every
item at every level points at messages. A client is sent
`{"summary": <body>, "people": {"<user id>": "<display name>", …}}`.

## 4. Storage, reuse and purging

`summary_nodes` holds one row per node: its channel, kind (chunk, thread,
section, period), `ikey` (a hash of its inputs), span, time-zone offset (periods),
`version`, body, and the prompt tokens and CPU time it cost. `summary_inputs`
lists each node's inputs in order: a message with the `edited_at_ms` it had when
read, or another node (SCHEMA.md migration 0057).

**Reuse.** A chunk or rollup is found again by its `ikey` and `version`; a period
by channel, span, offset and `version`. Building a span asks the model only for
what is not stored.

**Version.** `version` names the model, `SUM_PROMPT_VERSION` and the two
constants. A period made by an older version is served until the new one is made;
the worker rebuilds such periods in idle time.

**Purging** (`oc_sum_purge`). In the same transaction as a send, reply, edit,
delete or restore, the writer deletes every node built on the message or on its
thread's root, every period whose span holds the message's time, and everything
built on those, up the tree. Removing a user's DMs removes their summaries.

**The guard.** The worker reads through its own read-only connection and hands
new nodes to the writer in one batch (`OC_JOB_SUMMARY_STORE`). The writer stores
them only if every message is still there with the stamp it was read with and
every node input still exists; otherwise the worker builds again.

**Housekeeping.** Nodes nothing is built on, unused for `SUM_KEEP_MS` (30 days),
are deleted in idle time.

## 5. The worker and the load gate

One thread (`daemon/sum_worker.c`), set to `SCHED_IDLE` and nice 19, so the
kernel gives it CPU only when nothing else wants it. It holds the engine, loaded
on first use and unloaded after `SUM_UNLOAD_IDLE_MS` without work. One model call
at a time, computing on one thread.

**Requests first.** A SUMMARIZE with no stored answer goes on the worker's queue
(`SUM_QUEUE_MAX`). With the queue empty and the machine quiet for
`SUM_IDLE_SETTLE_MS`, the worker looks every `SUM_IDLE_SCAN_MS` for channels
with messages in the last `SUM_IDLE_DAYS` days and builds their past days, in
each member's time zone, that have no summary of the current version.

**The gate** (`daemon/sum_load.c`). Asked before each piece and every 16 tokens
while the model writes:

| Reading | Pause when | Resume when |
|---|---|---|
| Whole-machine CPU, `/proc/stat` | above `SUM_CPU_BUSY_PCT` (70%) | below `SUM_CPU_RESUME_PCT` (50%) |
| Available memory, `/proc/meminfo` | under `SUM_MEM_MIN_MB` (256 MB) | above it |
| Bytes the daemon read from people | above `SUM_NET_BUSY_BPS` a second | below it |

A paused generation keeps its place. Memory low for `SUM_MEM_UNLOAD_MS` abandons
the generation and unloads the model; the work is retried once the machine is
quiet. A request someone is waiting on is gated too: it waits longer rather than
pushing the machine.

## 6. The model

**Engine.** llama.cpp, from pinned source, CPU only, one static archive
(`scripts/build_llamacpp.sh`; VENDORS.md). On x86-64 it targets x86-64-v3 (AVX2).
The context is `SUM_CTX_TOKENS`: one piece, the prompt and the answer. The model
file is memory-mapped. Decoding is greedy, with the presence penalty the model's
card recommends.

**The model** (`daemon/sum_fetch.h`). Qwen3.5 0.8B, 4-bit (`Q4_K_M`), Apache-2.0:
about 530 MB on disk. On the first start with summaries on, the daemon fetches it
from its pinned address into `summary/` beside the database, checks it against
the pinned SHA-256, and writes a marker; later starts check the marker and the
size. Then the model answers one small question under its grammar. Any failure
leaves summaries off with a line in the log saying why.

## 7. Evaluating

`scripts/sumeval.c` runs the same code outside the daemon, and
`scripts/slack_to_db.py` loads a Slack export into a database at the daemon's
schema. Both are development tools and never installed; exported data never
enters the repository.

```
make build/sumeval
build/sumeval init dev.db
scripts/slack_to_db.py <export dir> dev.db
build/sumeval run dev.db <model.gguf> <channel> <start_ms> <end_ms> [threshold] [gap_minutes]
```

`run` prints the summary as a client receives it, then one line of measurements:
nodes stored, CPU seconds, wall seconds and peak memory. Varying the model and
the two constants is how they are chosen; people read the summaries side by side
and judge them.

## 8. Settings

| Variable | Values |
|---|---|
| `OPENCHIME_SUMMARY` | `off` (default) or `local`. `cloud` is accepted and leaves summaries off. |
| `OPENCHIME_SUMMARY_MODEL` | For `cloud`: the hosted model's name. |
| `OPENCHIME_SUMMARY_API_KEY` | For `cloud`: its key. |

Everything else — the size limit, the gap, the gate's thresholds — is a constant
in the code (`daemon/sum_core.h`, `daemon/sum_load.h`, `daemon/sum_worker.h`).
