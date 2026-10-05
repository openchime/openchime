# Summaries

A summary of a channel or DM, made on the tenant's own box by a small model
(REQ-310–313, ARCH-116). This document is the design; the decision and what was
rejected are ARCH-116.

## 1. What a reader gets

A client that was told the `summarize` capability may ask for a summary of any
channel or DM the person can read, over one of four spans (PROTOCOL.md §5.16m):

| Scope | Span |
|---|---|
| Unread | From the message the reader last read to now |
| Week | The last seven of the reader's days, today included |
| Range | `[start_ms, end_ms)` as given |
| Daily | The reader's yesterday, or every day since they were last about |

Days are the reader's, by the time-zone offset their client reports
(`users.tz_offset_min`). The answer is the summary as JSON (§3); a summary not
yet made is made while the request waits.

## 2. One summarize step, recursively

**What is summarized.** People's messages: not integration posts
(`author_name` set), not call events (`kind` other than 0), not deleted ones.
Markup is reduced to plain text by the same code read-aloud uses
(`shared/speakable.c`), whole: a summary never reads a message cut short.

**One step.** Every summary at every level is made the same way
(`daemon/sum_worker.c`, `summarize`): numbered lines go in, a summary comes out.
The same system prompt, the same instructions, the same decoding and the same
checks, whatever the lines are. At the bottom a line is a
message:

```
Messages from #support, oldest first. Indented lines are replies in a thread.

[1] Ann: Ticket 6701 is open; Bob please look.
  [2] Bob: On it.
```

Above it, a line is one line of a child summary — its overview, or one of its
items — and stands for the messages that line cited:

```
Summaries of consecutive parts of #support, oldest first.

On Fri 04 Sep:
[1] Overview: Ann asked Bob about ticket 6701.
[2] Action (open): Bob: look at ticket 6701
[3] Fact: Ticket 6701
```

Lines carry no clock times. A chunk's lines are the same for every reader, so its
summary is shared; where a span is put together, each child is headed with its
date in the reader's zone, and that summary is the reader's zone's.

**The one size limit.** `SUM_THRESHOLD_TOKENS` (`daemon/sum_core.h`) is the
only limit on size, and the recursion is the only way anything bigger is
handled. Size is one estimate for every model — a token per four bytes of text —
so where things are cut never depends on the model.

**Chunks.** A channel's messages are grouped into units: a top-level message
with no replies, or a whole thread (root and replies) placed at its **last**
reply, so a thread belongs to the period of its last activity. Units, in order,
are grouped into chunks of whole messages; a new chunk starts when the next unit
is more than `SUM_GAP_MS` after the last, or would take the chunk past the limit.
A cut always begins at the first message after the last quiet gap before what is
wanted (`oc_sum_anchor`), so every cut over the same messages makes the same
chunks and finds them again.

**The recursion** (`reduce`). Given summaries to put together: if their lines
fit the limit, summarize them once. If not, cut them, in order, into sections
whose lines fit, summarize each section, and put the section summaries through
the recursion again. A child that fits, alone in its section, goes up as it is
— unless every section is one child, when only summarizing them gets shorter.
Each level must come out shorter than what went in; one that does not stops the
build, and the failure is reported with what the model wrote. Nothing is ever
cut to make it fit.

The recursion makes three things:

- **A thread too big for a chunk**: its own chunks of whole messages, each
  summarized, then put together.
- **A message too big for a chunk**: its text split into parts at paragraphs,
  then lines, then sentences, then words, and only as a last resort mid-word
  (never mid-character); each part summarized, then put together. No byte of the
  message is lost.
- **A span**: the summaries of the chunks and threads whose last activity is in
  it, put together. A span of one piece is that piece's summary.

**What the model writes.** Plain text under fixed headings; any heading may be
empty:

```
Overview: <what happened>
Decisions:
- <text> [3][7]
Actions:
- <who>: <what> (open|done) [5]
Problems:
- <text> (open|resolved) [9]
Facts:
- <text> [2]
```

Nothing holds the model to this while it writes. Holding a model to JSON, or to
any grammar, while it writes lowers what it writes, most of all in small models;
and a small model held to a line shape writes a status or a citation of its own
inside the line, cannot end it, and writes on until its context is full. Code
reads the answer as written: a heading on its own line or inside one, in any
case or in markdown; citations as `[3]`, `[3][7]`, `[3, 7]` or `[1-4]`, or the
same in parentheses, anywhere in a bullet; a status anywhere in it; under a
heading, each line a bullet, with or without its mark.

The prompt asks for at most 30% (`SUM_WORDS_PCT`) of the lines' words: a word
budget is the length instruction small models follow best, and written chat
summaries run at 20–30% of the conversation. It is guidance only; no answer is
cut.

**The check** (`oc_sum_parse`). A bullet is dropped when it cites no line or a
line that is not there (a number in parentheses that is not a line is read as
text), gives an action to someone not in the lines or the team, lacks the status
its heading asks for, states a run of digits none of its cited lines contains,
or repeats one already kept; the overview is dropped when it cites a line not
there or states a number no line contains. An answer with no heading at all is a
failure, logged with what was written. Nothing else is dropped. Each
kept item cites the message ids its lines stand for.

## 3. The stored shape, and what clients get

A node's `body` is the checked summary with message ids for refs and user ids
for people (`who` 0 is the team):

```
{"overview": "Ann asked Bob about ticket 6701.",
 "decisions": [{"text": "...", "by": [], "refs": [100]}],
 "actions":   [{"who": 11, "what": "look at ticket 6701", "refs": [100, 101], "status": "open"}],
 "problems":  [{"text": "...", "refs": [...], "status": "open"}],
 "facts":     [{"text": "Ticket 6701", "refs": [100]}]}
```

Every item at every level points at messages. A client is sent
`{"summary": <body>, "people": {"<user id>": "<display name>", …}}`.

## 4. Storage, reuse and purging

`summary_nodes` holds one row per node: its channel, kind (chunk, thread,
section, period), `ikey`, span, time-zone offset (where its input carried dates),
`version`, body, and the prompt tokens and CPU time it cost. `summary_inputs`
lists each node's inputs in order: a message with the `edited_at_ms` it had when
read, or another node (SCHEMA.md migration 0057).

**Reuse.** A chunk's `ikey` is made from its messages as read; any other node's
from its children's keys (and the zone, when dated), so the same children give
the same key however they were made. A node is found again by `ikey` and
`version`; a period by channel, span, zone and `version`. Building anything asks
the model only for what is not stored.

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

## 5. The worker and the load gate

One thread (`daemon/sum_worker.c`), set to `SCHED_IDLE` and nice 19, so the
kernel gives it CPU only when nothing else wants it. It holds the engine, loaded
on first use and unloaded after `SUM_UNLOAD_IDLE_MS` without work. One model call
at a time, computing on one thread.

**Requests first.** A SUMMARIZE with no stored answer goes on the worker's queue
(`SUM_QUEUE_MAX`) and is made while the reader waits.

**Idle work.** With the queue empty and the machine quiet for
`SUM_IDLE_SETTLE_MS`, the worker, in this order:

1. summarizes the chunks a changed message left unsummarized — every purge tells
   the worker the channel and moment (`oc_sum_on_change`);
2. summarizes every channel's chunks, the newest first, a day at a time, back to
   each channel's first message;
3. makes again the periods of whole days someone asked for that a change has
   since purged;
4. rebuilds the periods an older version made.

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
(`scripts/build_llamacpp.sh`; VENDORS.md). On x86-64 it targets x86-64-v3
(AVX2, FMA, F16C, BMI2); before anything from llama.cpp runs, the daemon checks
the CPU has them (`oc_sum_cpu_ok`), and on one that does not, local summaries
are off with the reason logged. The model file is memory-mapped.

**Context.** `SUM_CTX_TOKENS`, three times the size limit: the prompt, one input
of up to the limit, and an answer up to the same size again. An answer may use
whatever the context has left; one that fills it without ending is a failure,
logged with what was written.

**Decoding.** Greedy: for summarizing, the most likely word is the most faithful
choice, and sampling raises what a model invents. Greedy decoding on its own can
repeat itself, so llama.cpp's DRY sampler runs before the choice at its published
defaults (multiplier 0.8, base 1.75, allowed length 2): a token that would extend
a sequence already written is penalized, more steeply the longer the sequence.
It does not penalize single tokens a summary must repeat — names, line numbers,
headings. Colons, quotes and asterisks end a sequence; a line break does not, so
a line repeated in a list is penalized.

**Prompt format.** The model's own chat format, as llama.cpp knows it. For a
model that thinks before answering (Qwen3.5 does), the prompt ends with the empty
thought its own template writes when thinking is not asked for, so it answers
straight away.

**The model** (`daemon/sum_fetch.h`). Qwen3.5 2B, 4-bit (`Q4_K_M`), Apache-2.0:
about 1.3 GB on disk. On the first start with summaries on, the daemon fetches it
from its pinned address into `summary/` beside the database, checks it against
the pinned SHA-256, and writes a marker; later starts check the marker and the
size, then load the model. Any failure leaves summaries off with a line in the
log saying why.

## 7. Evaluating

`scripts/sumeval.c` runs the same code outside the daemon, and
`scripts/slack_to_db.py` loads a Slack export into a database at the daemon's
schema. Both are development tools and never installed; exported data never
enters the repository.

```
make build/sumeval
build/sumeval init dev.db
scripts/slack_to_db.py <export dir> dev.db
build/sumeval run dev.db <model.gguf> <channel> <start_ms> <end_ms> [threshold] [gap_minutes] [tree.md]
```

`run` prints the summary as a client receives it, then one line of measurements:
nodes stored, model calls, CPU seconds, wall seconds and peak memory. With
`tree.md`, every model call — the prompt, the answer as written, and what was
stored — is written there in the order made, to see what each level did. Varying
the model and the two constants is how they are chosen; people read the
summaries side by side and judge them.

## 8. Settings

| Variable | Values |
|---|---|
| `OPENCHIME_SUMMARY` | `off` (default) or `local`. `cloud` is accepted and leaves summaries off. |
| `OPENCHIME_SUMMARY_MODEL` | For `cloud`: the hosted model's name. |
| `OPENCHIME_SUMMARY_API_KEY` | For `cloud`: its key. |

Everything else — the size limit, the gap, the word budget, the gate's
thresholds — is a constant in the code (`daemon/sum_core.h`, `daemon/sum_load.h`,
`daemon/sum_worker.h`).
