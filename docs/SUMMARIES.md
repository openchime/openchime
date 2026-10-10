# Summaries

A summary of a channel or DM, made on the tenant's own box by a small model
(REQ-310–313, ARCH-116). This document is the design; the decision and what was
rejected are ARCH-116.

## 1. What a reader gets

A client that was told the `summarize` capability may ask for a summary of any
channel or DM the person can read, over one of five spans (PROTOCOL.md §5.16m):

| Scope | Span |
|---|---|
| Unread | From the message the reader last read to now |
| Today | The reader's today, up to now |
| Since yesterday | The reader's yesterday and today, up to now |
| Last 7 days | The last seven of the reader's days, today included |
| Custom date range | `[start_ms, end_ms)` as given |

Days are the reader's, by the time-zone offset their client reports
(`users.tz_offset_min`). The answer is the summary as JSON (§3); a summary not
yet made is made while the request waits.

**The standard.** Every summary has the same shape, sized to the span it
covers (`daemon/sum_core.h`):

| | |
|---|---|
| In all | about a word to every `SUM_WORDS_PER_WORD` (10) of the span's, between `SUM_MIN_WORDS` (100) and `SUM_MAX_WORDS` (600), and never more than the span's own |
| Overview | the most important outcome first |
| Topics | one to every hundred words of the whole, between 2 and `SUM_MAX_TOPICS` (6), the most important first; from 200 words up at least all but two of them are written |
| A topic | a title of 2–6 words; who was in it and how many messages it rests on; what happened and where it stands; up to 3 details, at least 2 from 300 words up |
| Needs attention | ≤5: an action someone has, a question not answered |
| More topics | ≤10 titles of topics that did not fit, from 200 words up |

The size is asked for as shape — how many topics and details — not as a word
count, which a model undershoots or pads to. The parts have caps too
(`SUM_OVERVIEW_WORDS` 60, `SUM_PARA_WORDS` 80, `SUM_DETAIL_WORDS` 40), but as
bounds on a runaway line, not lengths to write to; the whole is held to its
size by code (§2).

Every overview, topic, detail and item cites the messages it rests on — at most
`SUM_CITES_MAX` (3) shown for it. A span with no messages is "No messages";
any span with one is summarized, however short — except that a thread belongs
to the period of its last activity (§2), so a span holding only replies in
threads that go on past it has nothing of its own to summarize, and says so. The
size follows the span because a summary that is the same size whatever it covers
empties out as the span grows: ten thousand words summarized in three hundred
is a list of topic names. The ceiling keeps the longest to about a screen; the
bottom line first, a handful of topics, and actions and open questions
labelled — as most chat users asked for in Microsoft's study of chat
summaries — set the rest.

In the TUI and the Windows client (CLIENT.md §2–3), a summary is asked for from
the Summarize button in the conversation's header (the same five spans from the
conversation's menu and the launcher), laid out as Slack's: what it covers --
the dates, how many messages, who posted -- the overview, each topic with who
was in it, what happened and its details, what needs attention, and the titles
of further topics; each line cites its messages as [n], a citation showing the
message it cites and going to it. It is shown only to the person who asked, and
forgotten when they close it or leave the conversation.

## 2. How a summary is made

**What is summarized.** People's messages and integrations' posts, an
integration's under the name it signs with (`author_name`); not call events
(`kind` other than 0), not deleted messages. Markup is reduced to plain text by
the same code read-aloud uses (`shared/speakable.c`), whole: a summary never
reads a message cut short. A run of messages that say the same but for their
numbers, whoever wrote them, reads as one line with how many — "Order 55
cancelled. (x14)" — standing for every one of them.

**Size and the end are the code's, not the model's.** No model keeps to a
word count: measured across models, "at most N words" is overshot by a tenth
to a half of the time, always over, and the more so the shorter the limit;
what models do keep to is a count of sentences or bullets, and "at most" as a
ceiling rather than "exactly". So the length is asked for in those terms, and
held by code, never by the token cap:

- the prompt asks for the shape in parts and sentences -- the overview one
  sentence, a topic one to three, a detail one -- and names no word count.
  Measured on the same spans with Llama 3.3 70B, "At most N words in all"
  halved what it wrote on small spans (facts went with the words), "About N
  words" cost a fifth, and the sentence counts alone wrote as much as before;
  a model that overshoots ignored the figure either way. The total is held
  below, by code;
- a grammar (llama.cpp GBNF) bounds every answer: how many lines of each part,
  how long each line is in characters, and that every line starts with the
  numbers of lines that exist. The words are the model's;
- every answer has a rail in tokens, sized to what its shape can hold at most
  (`oc_sum_caps.room`, `SUM_TOKENS_PER_WORD` to a word, measured at 1.9 to 4.4
  tokens to a kept word across models), never to the length asked for: an
  answer that keeps the form always ends, and one that reaches the rail is a
  runaway: it is asked for once more, the same request (a provider's or a
  sampler's bad moment, not the prompt's), and a second runaway fails the
  call. Notes have room for `SUM_NOTES_SLACK` past what was asked;
- a summary over its total by more than `SUM_SHORTEN_OVER_PCT` (20%) is handed
  back to the model once, with its word count and the total, to be written
  again shorter in the same form; the shorter answer stands when it is one
  (one revision moves most models from under a third within the limit to over
  half; it never costs quality), never a second;
- every call writes notes in proportion to what it read — one to every
  `SUM_TOKENS_PER_NOTE` (250) tokens, at least `SUM_NOTES_MIN` (4), at most
  what the answer room holds — and never more than half the lines it read
  (`oc_sum_notes_for`), though never fewer than `SUM_NOTES_FLOOR` (3) when
  there are that many lines, so a few unrelated lines are not forced into one
  note. Every merge of two or more sets of notes gives back fewer than it
  took, every level of the tree is smaller than the one under it, and the
  recursion ends;
- code holds each part of the summary, and the whole, to its cap (below): a
  part over its cap is rewritten by the model, then cut at a sentence, and the
  whole is trimmed to its total.

**Two paths, by size.** A span whose messages fit one prompt —
`SUM_INPUT_TOKENS` (6,000) of the model's `SUM_CTX_TOKENS` (8,192) — is
summarized from its messages in one call: when the input fits, one call is more
faithful than any hierarchy. A larger span goes through notes:

1. **Notes on a chunk.** A channel's messages are cut into chunks of whole
   messages (below); the model writes notes on each, as many as its size
   allows (above), one line each:
   `- [line numbers] kind | topic | what happened`, the kind an event, decision,
   action or question, the topic 2–5 words shared by notes on the same thing,
   what happened one or two sentences saying who did what, with the
   particulars the lines give.
2. **Merging.** While the notes of a span do not fit one prompt, they are cut,
   in order, into sections of as many as fit together, and each section of two
   or more is merged — the same instructions — into fewer notes than it holds.
   A section of one goes up as it is. Every round merges at least two into one, so it
   ends; a thread or a message too big for a chunk is merged to one node the
   same way.
3. **The summary.** The notes, fitted, go into one call that writes the
   summary — the notes alone, not the messages they cite, so the last call,
   the one a reader waits for, stays small however large the span.

The one system prompt is the same for every call.

```
Messages from #support, oldest first. Indented lines are replies in a thread.

[1] Ann: Ticket 6701 is open; Bob please look.
  [2] Bob: On it.
[3] Bot: Order 55 cancelled. (x4)
```

Notes on that, as stored and as the next level reads them:

```
- [1][2] action | Ticket 6701 | Ann asked Bob to look at ticket 6701, and he took it.
[1] (action) Ticket 6701: Ann asked Bob to look at ticket 6701, and he took it.
```

A note stands for the messages its lines stand for; it shows at most three of
them (each cited line's first in turn) and keeps them all, with their authors,
so the summary can say how many messages a topic rests on and who was in it.
Lines carry no clock times. A chunk's notes are the same for every reader, so
they are shared; where a span is put together, each child is headed with its
date in the reader's zone, and what is built on them is that zone's.

**The summary's answer**, as the grammar holds it:

```
Overview: [line number] <the most important outcome or change>
## <what a topic is about>
[line number] <what happened in it, and where it stands now>
- [line number] <one detail: who did or said what>
Needs attention:
- [line number] action: <who has to do what, and by when if it was said>
- [line number] question: <a question that was asked and not answered>
More topics: <topic>; <topic>
```

**Chunks.** A channel's messages are grouped into units: a top-level message
with no replies, or a whole thread (root and replies) placed at its **last**
reply, so a thread belongs to the period of its last activity. Units, in order,
are grouped into chunks of whole messages; a new chunk starts when the next unit
is more than `SUM_GAP_MS` after the last, or would take the chunk past
`SUM_THRESHOLD_TOKENS` — the one size constant, chosen by evaluation (§7). A cut
always begins at the first message after the last quiet gap before what is
wanted (`oc_sum_anchor`), so every cut over the same messages makes the same
chunks and finds them again. A message too big for a chunk is split into parts at
paragraphs, then lines, then sentences, then words, and only as a last resort
mid-word (never mid-character); no byte of it is lost.

**The checks** (`oc_sum_parse_notes`, `oc_sum_parse_final`). Code reads every
answer as written and drops only what is invented or repeated: a sentence
stating a run of digits none of the lines it cites contains; a sentence naming a
person — anyone the lines show as an author — who neither wrote nor is named in
a line it cites (the commonest error in dialogue summaries is the wrong
person); a note or detail every word of which is in one already kept; a line
citing nothing. Something that says there is nothing ("None", "N/A") is nothing.
A topic left with nothing is no topic; with no overview, the first topic's
account stands in. An answer with nothing left is a failure, logged with what
was written.

**Holding the caps.** The topics are ranked — those with a decision or an
action first, then a question, then by how many messages they rest on, then how
recent — then each part over its cap is rewritten by the model, told its length
and its cap, at most `SUM_REWRITES` (2) times, each rewrite checked as the
first; one still over is cut at its last whole sentence that fits. The whole is
then held to its cap: the lowest topics move to "More topics", then the last
details and attention items go, then the account and the overview are cut at a
sentence. Nothing is cut mid-sentence while a sentence boundary fits.

## 3. The stored shapes, and what clients get

A chunk, section or thread node's `body` is its notes:

```
{"notes": [{"kind": "action", "topic": "Ticket 6701",
            "text": "Ann asked Bob to look at ticket 6701, and he took it.",
            "refs": [100, 101], "all": [100, 101], "by": ["Ann", "Bob"]}]}
```

A period's `body` is the summary:

```
{"overview": {"text": "...", "refs": [101]},
 "topics": [{"title": "Ticket 6701", "text": "...", "refs": [101], "count": 2,
             "people": ["Ann", "Bob"], "details": [{"text": "...", "refs": [100]}]}],
 "attention": [{"kind": "action", "text": "...", "refs": [101]}],
 "more": ["Lunch", "Parking"], "words": 1840}
```

`count` is how many messages a topic rests on and `people` who wrote them;
`words` is how many words what it summarizes holds — whole threads included —
which chose its size (§1). A
client is sent the summary with what the span holds, read when the answer is
sent (`oc_sum_client_body`):

```
{"summary": <body>,
 "posters": [{"id": 11, "name": "Bob"}, {"id": 0, "name": "PartsFisher"}],
 "count": 525,
 "sources": {"100": {"author": "Ann", "author_id": 10, "at": <ms>, "parent": 0,
                     "text": "Ticket 6701 is open; Bob please look."}}}
```

`posters` is everyone who posted in the span, most messages first (`id` 0 for
an integration); `count` the messages in it; `sources` each message the summary
cites, anywhere in it, as plain text, whole, so a client shows a citation's message without
loading the history around it. A deleted message is no source.

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
constants. A period made by an older version is served until the new one is made,
when it has the shape this version writes; the worker rebuilds such periods in
idle time.

**Purging** (`oc_sum_purge`). In the same transaction as a send, reply, edit,
delete or restore, the writer deletes every node built on the message or on its
thread's root, every period whose span holds the message's time, and everything
built on those, up the tree. Removing a user's DMs removes their summaries.

**The guard.** The worker reads through its own read-only connection and hands
new nodes to the writer in one batch (`OC_JOB_SUMMARY_STORE`). The writer stores
them only if every message is still there with the stamp it was read with and
every node input still exists; otherwise the worker builds again.

**A build that fails keeps what it made.** A failed call fails the build, but
the nodes finished before it -- chunk notes, merges -- go to the writer all the
same and are stored under the same guard; the period is not. The next build of
anything over those messages finds them by `ikey` and asks the model only for
the rest, so a rate limit or an outage part way through a large span costs the
calls it interrupted, not the whole span. The request is answered with the
error whatever became of the pieces.

## 5. The worker and the load gate

One thread (`daemon/sum_worker.c`), set to `SCHED_IDLE` and nice 19, so the
kernel gives it CPU only when nothing else wants it. It holds the engine, loaded
on first use and unloaded after `SUM_UNLOAD_IDLE_MS` without work. One model call
at a time, computing on one thread.

**Requests first.** A SUMMARIZE with no stored answer is a row of
`summary_requests` (SCHEMA.md §3ax), its asker's, not their connection's:
`queued` until the worker takes it, `running` while it is made, deleted when it
is answered or cancelled. The writer adds the row; the worker takes the oldest
through its own connection and has the writer mark it running; the store that
answers it deletes it. At most `SUM_QUEUE_MAX` (64) wait; more are told to try
again shortly. The same person asking for the same span again joins the row
already there. Each request is told where it is — `SUMMARY_QUEUED` (PROTOCOL.md
§5.16m), how many are ahead of it, 0 when it is being made — when it joins and
whenever the queue moves, and a line in the log marks it queued, started and
answered with how long it took. `SELECT * FROM summary_requests` shows the queue
as it is.

**Nobody need wait.** A request goes on when its asker stops watching it — they
asked to be told instead (`SUMMARY_DETACH`), closed the summary or the app, or
the daemon restarted (every request is queued again at start, watched by
nobody). Every finished request leaves a notice (`summary_notices`): the summary
as it was sent, or why it was not made. The connection still watching gets the
`SUMMARY`, and the notice is seen; otherwise the person is told
(`SUMMARY_READY`) on every connection they have, and again on every sign-in
until they open it (`SUMMARY_OPEN`, which answers with the summary as it was
made) or dismiss it. A seen notice is deleted a day later
(`SUM_NOTICE_KEEP_MS`); an unseen one is kept. Notices are in the person's
Activity. A request can be cancelled (`SUMMARY_CANCEL`): out of the queue, or,
being made, stopped at its next pause (the gate asks), leaving nothing.

**A request stops idle work.** The writer adding a row wakes the worker and sets
a flag; the gate, asked between prompt batches and every 16 tokens, stops idle
work (never another request) when it is set. What idle work was writing is
dropped and done again later; nothing of it was stored.

**Idle work.** With the queue empty and the machine quiet for
`SUM_IDLE_SETTLE_MS`, the worker, in this order:

1. takes notes on the chunks a changed message left without — every purge tells
   the worker the channel and moment (`oc_sum_on_change`);
2. takes notes on every channel's chunks of the last `SUM_BACKGROUND_DAYS` (7,
   the longest span offered as a preset), the newest first, a day at a time;
   older spans are made when someone asks for them;
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
pushing the machine. A hosted model (§6) is not gated: it does not run on this
machine, so only stopping and a waiting request hold its calls.

## 6. The model

Which models have been run, and how each did, is in docs/MODELS.md.

**Engine.** llama.cpp, from pinned source, CPU only, one static archive
(`scripts/build_llamacpp.sh`; VENDORS.md). On x86-64 it targets x86-64-v3
(AVX2, FMA, F16C, BMI2); before anything from llama.cpp runs, the daemon checks
the CPU has them (`oc_sum_cpu_ok`), and on one that does not, local summaries
are off with the reason logged. The model file is memory-mapped.

**Context.** `SUM_CTX_TOKENS` (8,192): a call's lines take at most
`SUM_INPUT_TOKENS` (6,000), the rest holds the instructions and the answer. Every
answer has a rail in tokens (§2): notes `SUM_NOTE_TOKENS` each, with room for
`SUM_NOTES_SLACK` more than asked, which the parser then leaves out; a summary
`SUM_TOKENS_PER_WORD` to a word of what its shape can hold plus
`SUM_FINAL_SPARE_TOKENS`, and never more than the context has left after the
prompt. One that reaches it without ending is a runaway and a failure, logged
with what was written. The context cache is kept at 8 bits (`q8_0`, with flash
attention): about half the memory of 16, so a model and its context fit the
memory budget (§7).

**Decoding.** Greedy, inside each call's grammar: for summarizing, the most
likely word is the most faithful choice, and sampling raises what a model invents. Greedy decoding on its own can
repeat itself, so llama.cpp's DRY sampler runs before the choice at its published
defaults (multiplier 0.8, base 1.75, allowed length 2): a token that would extend
a sequence already written is penalized, more steeply the longer the sequence.
It does not penalize single tokens a summary must repeat — names, line numbers,
headings. Colons, quotes and asterisks end a sequence; a line break does not, so
a line repeated in a list is penalized.

**Prompt format.** The model's own chat format, as llama.cpp knows it. For a
model that thinks before answering, the prompt ends with the empty thought its
own template writes when thinking is not asked for, so it answers straight away;
thinking lowers what small models write in dialogue summaries and in exact
formats, and the grammar would not let it start anyway.

**The model** (`daemon/sum_fetch.h`). Qwen3.5 2B, 4-bit (`Q4_K_M`), Apache-2.0:
about 1.3 GB on disk. On the first start with summaries on, the daemon fetches it
from its pinned address into `summary/` beside the database, checks it against
the pinned SHA-256, and writes a marker; later starts check the marker and the
size, then load the model. Any failure leaves summaries off with a line in the
log saying why.

**While they come up.** Fetching, checking and loading the model (or the hosted
model's first answer) can take minutes, and the daemon serves meanwhile.
Summaries are offered from the start: a connection signed in then is sent the
`summarize` capability, and a SUMMARIZE waits in the queue (§5) — told its place,
nothing stored found yet — for the worker to take once they are on. If they do
not come up, every request that waited is answered UNAVAILABLE ("Summaries
could not be started on this server.") and they are offered no more.

**A hosted model** (`OPENCHIME_SUMMARY=cloud`, `daemon/sum_cloud.c`). The same
pipeline, with each call sent to an OpenAI-style chat-completions API instead of
the model in the daemon: the same system and user prompt, `temperature` 0 and
the call's answer cap (`max_tokens`), and nothing else, so any such API answers
it. The grammar is not sent: the shape is asked for in the prompt and held by the
parser, as for every answer. The prompt is the same for every model: nothing in
it is written for one. A server's context per request must hold `SUM_CTX_TOKENS`.
The prompt and the span's messages leave the box.

At startup the API is asked a one-line prompt, and must answer it or summaries
are off with the reason logged. A model that reasons before it answers would
spend every answer's cap on that; the API says so in the usage it reports
(`completion_tokens_details.reasoning_tokens`, OpenAI's shape) or by answering
nothing. Such a model is asked the prompt again with each of the fields APIs
have for turning reasoning off -- OpenAI's `reasoning_effort: "none"`,
OpenRouter's `reasoning: {enabled: false}`, llama.cpp's and vLLM's
`chat_template_kwargs: {enable_thinking: false}` -- and the first the API
honours is sent with every request from then on, logged once. A model that
reasons only when it finds a prompt worth it answers the one-line prompt plainly;
the first request it reasons on is asked again the same way, and that answer is
the call's. Nothing is keyed to a model's name: what the server answers decides. A model that cannot be told
not to reason (some APIs refuse to) leaves summaries off, with that as the reason.
A `<think>` block a model opens its answer with anyway is dropped.

A server that asks for a pause (429) or fails (500, 502, 503, 504), or cannot
be reached, is sent the same request again after a wait: the seconds its
`Retry-After` names, else 2, 4, 8, 16 and 32, never more than
`SUM_CLOUD_WAIT_MAX_S` (60), at most `SUM_CLOUD_RETRIES` (5) times, so under
three minutes in all; one log line a wait, and the gate is asked through it.
Still failing after the last, the server's answer is the error. Anything else
the server answers is not tried again. A request someone is waiting on waits
the same way. A provider's rate limit therefore slows a large span rather than
failing it (and what a failure does interrupt is kept, §4).

**Where the time goes.** Every call logs its stage (`chunk`, `long message`,
`merge`, `final`, `shorten`, `rewrite`), the tokens read and written, and the
seconds spent reading the prompt, writing the answer and in all -- for a hosted
model, reading and writing as the server reports them, and the whole including
the network. Each request logs the same added up over its calls, with how many
calls wrote something again shorter, how many answers ran away and were asked
for once more, and how many requests were sent again after a 429 or a 5xx, and
the seconds waited.

## 7. Evaluating

`scripts/sumeval.c` runs the same code outside the daemon, and
`scripts/slack_to_db.py` loads a Slack export into a database at the daemon's
schema. They and the scripts below are development tools and never installed;
exported data never enters the repository.

```
make build/sumeval
build/sumeval init dev.db
scripts/slack_to_db.py <export dir> dev.db
build/sumeval run dev.db <model> <channel> <start_ms> <end_ms> [threshold] [gap_minutes] [tree.md]
```

`<model>` is a GGUF file, run by the local engine, or the URL of a
chat-completions API, asked by the hosted engine (§6) for the model in
`OPENCHIME_SUMMARY_MODEL` and the key in `OPENCHIME_SUMMARY_API_KEY`; nothing
runs on the machine for a URL.

`run` prints the summary as a client receives it, then one line of measurements:
nodes stored, model calls, CPU seconds, wall seconds and peak memory. With
`tree.md`, every model call — the prompt, the answer as written, and what was
stored — is written there in the order made. `batch` writes a line a span to
`measure.tsv`: id, class, channel, ok or FAILED, calls, CPU and wall seconds,
nodes stored, peak memory, the error, how many notes and sentences the checks
dropped, how many calls wrote something again shorter, how many requests were
sent again after a 429 or a 5xx, the seconds waited, and how many answers ran
away and were asked for once more. A span that fails
still stores the nodes it finished, so a later pass over the set starts from
them.

**The set.** `scripts/sumeval_set.py <db> <seed> <out.tsv>` draws spans at random,
with the seed recorded, classed only by size — never by channel or by who posts
— ten each of small (≤400 words), medium (fits one prompt) and large (more than
one prompt), and four of each held out, used only to check the result.
`scripts/sumeval_bakeoff.sh` runs every model over the set, each on its own copy
of the database, keeping each span's summary, its tree and its measurements.

**The rubric.** A person grades each summary, lengths held equal: every line
supported by the messages it cites (at least 98%; an invented decision, owner,
date or number fails the summary); every citation right, with the right person;
the span's must-know facts present (at least 80%, every decision and action);
within the caps, nothing redundant; the overview first, the titles telling the
topics apart.

**The budget.** The model, its context and its working memory together stay
within 2.5 GB on a box of 4 GB, beside the daemon: a model over what is free does
not fail, it thrashes. Models are compared within the budget, at the largest
quantization that fits (4-bit quantization measurably costs small models their
instruction following), by the graded summaries.

Teaching a smaller model the form, so it can run on a smaller box, is
docs/TUNING.md.

## 8. Settings

| Variable | Values |
|---|---|
| `OPENCHIME_SUMMARY` | `off` (default), `local` or `cloud` (§6). |
| `OPENCHIME_SUMMARY_URL` | For `cloud`, required: the API's base, e.g. `https://models.example.com/v1`; requests go to `<base>/chat/completions`. https, or http to a loopback address. |
| `OPENCHIME_SUMMARY_MODEL` | For `cloud`, required: the model's name there. It names the summaries' version, so changing it remakes them. |
| `OPENCHIME_SUMMARY_API_KEY` | For `cloud`: its key, sent as a bearer token; none when empty. |

Everything else — the chunk size, the gap, the standard's caps, the gate's
thresholds — is a constant in the code (`daemon/sum_core.h`, `daemon/sum_load.h`,
`daemon/sum_worker.h`).
