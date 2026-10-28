# Summaries

A summary of a channel or DM, made on the workspace's own box by a small model
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

In the TUI and the GUI — the Windows client and the same client in the browser
(CLIENT.md §2–3) — a summary is asked for from
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

**Nobody need wait.** A request goes on when its asker stops watching it, and
every finished request leaves a notice (`summary_notices`) in the person's
Activity: the summary as it was sent, or why it was not made. A seen notice is
deleted a day later (`SUM_NOTICE_KEEP_MS`); an unseen one is kept. The frames
that detach, announce, open, dismiss and cancel a request are in PROTOCOL.md
§5.16m.

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

Which models have been run, and how each did, is §7.1.

**Engine.** llama.cpp, from pinned source, CPU only, one static archive
(`scripts/build/llamacpp.sh`; VENDORS.md). On x86-64 it targets x86-64-v3
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
from its Hugging Face address into `summary/` beside the database, checks it against
the pinned SHA-256 (the address names a branch, `resolve/main`, not a revision, so
the hash is what pins the file), and writes a marker; later starts check the marker and the
size, then load the model. Any failure leaves summaries off with a line in the
log saying why.

**While they come up.** Fetching, checking and loading the model (or the hosted
model's first answer) can take minutes, and the daemon serves meanwhile.
Summaries are offered from the start: a connection signed in then is sent the
`summarize` capability, and a SUMMARIZE waits in the queue (§5) — told its place,
nothing stored found yet — for the worker to take once they are on. If they do
not come up, every request that waited is answered UNAVAILABLE ("Summaries
could not be started on this server.") and they are offered no more.

**A hosted model** (`OPENCHIME_SUMMARY=cloud:<url>,<model>`, `daemon/sum_cloud.c`). The same
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

The evaluation was made with development tools that have since been removed
from the tree; this section stays as the record of the method. `sumeval.c` ran the same code
outside the daemon, and `slack_to_db.py` loaded a Slack export into a database
at the daemon's schema. Exported data never enters the repository.

For each span `sumeval` kept the summary as a client receives it, the nodes stored,
model calls, CPU and wall seconds, peak memory and every model call's prompt
and answer. A span that fails still stores the nodes it finished, so a later
pass over the set starts from them.

**The set.** Spans were drawn at random, with the seed recorded, classed only by
size — never by channel or by who posts — ten each of small (≤400 words), medium
(fits one prompt) and large (more than one prompt), and four of each held out,
used only to check the result; every model ran over the set on its own copy of
the database.

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

Teaching a smaller model the form, so it can run on a smaller box, is §7.2.

### 7.1 Models tried

Which models the summarizer has been run with, how each did, and what to know
before choosing one. The prompt is the same for every model; nothing in it is
written for one (§6). A model is **verified** when it completed every span of
the evaluation set with the current prompt version and its summaries read as
the standard asks (§1), graded by hand.

**How a run is made.** The evaluation set is 30 spans of a real Slack export, never committed
(10 small, 10 medium, 10 large), run cold through the real pipeline against the
hosted engine, a chat-completions URL. Hosted runs here went through OpenRouter
(`https://openrouter.ai/api/v1`), which fronts the model's own vendor or
third-party providers under one API and key. Numbers are from those runs; the
wall time of a call is the provider's, not the pipeline's, and varies by the hour.
Prompt versions are `SUM_PROMPT_VERSION` in `daemon/sum_core.h`; results under
an older version are not comparable with the current one.

**Hosted, through OpenRouter.**

| Model (OpenRouter name) | Prompt | Spans done | Verdict | Notes |
|---|---|---|---|---|
| `meta-llama/llama-3.3-70b-instruct` | s15 | 30/30, twice | **Verified** | The reference. About 5 s a call, 10 s for a large span's final call (the provider's time; it has ranged to 60 s). Faithful and specific on human channels; mean 118 words. Two of 401 chunk calls in one run came back as token loops from the provider: the first run kept the pieces and a second pass finished both spans in 3 and 1 calls; the re-test, with the second ask on a runaway, had none. Writes bare citations and titles without `##` now and then, which the parser accepts. |
| `openai/gpt-4.1` | s15 | 30/30, twice | **Verified** | One pass each time. Some requests were answered 429 by the provider and sent again after the pause, none failing a span. Specific and well cited; mean 156 words. |
| `anthropic/claude-haiku-5.5` | s15 | 30/30, twice | **Verified** | Reasons before answering when it finds a prompt worth it; the engine learns that on the first such call and sends `reasoning_effort: "none"` from then on (§6). Content the richest tried, with amounts, names and reasons kept, mean 172 words. Writes past the total on half the spans: the shorten pass gains a few words, the trim does the rest. One runaway in 412 calls, asked again and answered. On s14 it failed 7 of 30 by a token cap sized to the total, which s15 removed. |
| `openai/gpt-4.1-mini` | s15 | 30/30, twice | **Verified** | Fastest tried, under 2 s a call. Writes past the total on half the spans and is shortened once, then trimmed; on s14 it failed 9 of 30 by the token cap. Specific and cited; mean 179 words. |

**Small models through the hosted path** (candidates for the local engine,
run the same way; without the grammar the local engine holds them to, so a
form failure here says less than it would locally):

| Model (OpenRouter name) | Prompt | Spans done | Notes |
|---|---|---|---|
| `ibm-granite/granite-4.0-h-micro` (3B) | s15 | 30/30 | Holds the form; the thinnest content tried, mean 75 words, 473 sentences cut by the checks for numbers or names the lines do not have. Overviews are sometimes a line's label ("integration: Attachment provided"). |

Not tried: Google Gemini, xAI Grok, the larger Anthropic and OpenAI models.

**Local, in the daemon (llama.cpp).** The real daemon built for the model, on a
2-vCPU, 4 GB machine, fetching and checking its GGUF at startup and asked for
the 30 spans over the wire by a client.
Prompt s15. Times are for the whole span cold, every chunk call included; a
request whose chunks were pre-mapped waits for the final call only.

| Model | Spans done | Set time | Peak resident | Verdict | Notes |
|---|---|---|---|---|---|
| Qwen3.5 2B, Q4_K_M (the pinned default) | 29/30 | 5 h 55 min | 2.18 GB | Near the standard; the only small model whose summaries read as summaries | Keeps names, dates, numbers and decisions on small and medium spans; large spans keep the specifics but rank them loosely. Misattributes now and then (a reported bug written as if the reporter caused it) and pads "Needs attention" with statements on about a third of spans. One span not in the form; one runaway, asked again and answered. Small spans 55 to 150 s, medium 80 to 240 s, large 10 to 85 min. |
| Qwen3.5 0.8B, Q8_0 | 28/30 | 3 h 6 min | 1.22 GB | Below the standard | Keeps numbers and ids better than the 1B class but copies labels and fragments into titles and overviews, invents on several spans, repeats one notification four times, and was slower than LFM2.5. The tuning candidate by size (§7.2). |
| LFM2.5 1.2B, Q8_0 | 29/30 | 1 h 50 min | 1.39 GB | Below the standard | The fastest by far, 2 to 4 times the others. Writes names in square brackets, citations into titles, placeholders ("[Name] confirm schedule"), and three-word lines with no facts on large spans. The tuning candidate by speed. |
| Gemma 3 1B, Q8_0 | 30/30 | 2 h 33 min | 1.48 GB | Below the standard | Overviews are a copied line with the author's name in front; repeats a topic title five times on one span; degenerates on the largest. Keeps some ids and amounts. |
| Llama 3.2 1B, Q8_0 | 29/30 | 2 h 53 min | 1.66 GB | Below the standard | Copies lines with field labels as sentences, lists compliments as actions, repeats one garbled question five times. |
| Qwen3 1.7B, Q4_K_M | 28/30 | 4 h 10 min | 2.44 GB | Below the standard | The older generation: more memory than Qwen3.5 2B and worse. Copies the note labels ("(event) deal: …") into topic text, writes "Name: did X" fragments as details, overviews on bot spans are the notification itself. Two spans failed on a notes runaway that the second ask did not cure. |
| Granite 4.0 H 1B, Q4_K_M | 30/30 | 4 h 35 min | 1.79 GB | Below the standard, the weakest | Holds the form only by the grammar: one overview is the prompt's own placeholder text, topics are note labels and copied lines, details carry the prompt's section names ("Needs attention: No. No question asked. More topics: ["), amounts drift (6,000 becomes 6,00). Slow reading on CPU (about 20 tokens a second), 4 h 35 min for the set. |

The local engine runs beside the daemon on a box of 4 GB, so a candidate is
one whose GGUF and context fit about 2.5 GB (§7). Below 2B, no model
off the shelf holds the form by itself: the grammar makes the answer parse,
and what is inside it is copied lines and invented questions. Teaching one
the form is §7.2.

**What to tell a user choosing a hosted model.**

- Any OpenAI-style chat-completions API works; the request is `model`,
  `messages`, `temperature` 0 and `max_tokens`, nothing else.
- The model is named by its provider's name, exactly (the `<model>` in `OPENCHIME_SUMMARY=cloud:<url>,<model>`).
  Changing it remakes every summary.
- A model that reasons before answering is told not to at startup, through
  whichever of the known fields its API honours (§6, one log line
  says which). Some cannot be told (OpenAI's GPT-5 family on OpenRouter
  answers "reasoning is mandatory"; Qwen3-14B's provider there ignores every
  field), and summaries stay off with that reason.
- A model that writes past the total it was asked for is not failed for it
  (since prompt s15): it is asked once to write the summary again shorter,
  and the result is trimmed to the total. Every model overshoots a word limit
  some of the time, the larger commercial ones most.
- A provider's rate limit (429) or outage (5xx) is waited out: the same
  request is sent again after the pause the server names or a doubling one,
  up to five times and under three minutes in all, one log line a wait. A span
  that still fails keeps the pieces it finished for the next try.
- A large span's final call takes 15 to 40 s on a 70B-class API model; the
  chunk calls before it are made ahead of time for the last 7 days (§4, §5) and on demand for older ranges.

### 7.2 Tuning a smaller model

The method recorded for teaching a model too small to infer the form from its
prompt (§2) to write it anyway, so that it can run in the daemon on a smaller
box (§6). Nothing in the daemon changes: the result is one GGUF, pinned in
`daemon/sum_fetch.h` like the current model, and the prompts, the grammar and
the checks stay as they are. The tools it used were removed with the
others above.

**Why.** On the 30-span set (§7.1), models of 1B or so hold the form only
because the grammar makes them, and what they write inside it is copied lines,
invented questions and bracketed names; the 2B is the smallest that reads as a
summary. A machine running the 2B needs 4 GB. A model of 0.8B to 1.2B that had
been shown the prompt and a correct answer a few thousand times would hold the
form on its own, keep the facts, and fit a box of 2.5 GB. Judgement does not
come from imitation at this size (which topic matters most in a 24,000-word
span, what belongs under "Needs attention"): expect a tuned small model to pass
on form, citations and facts for small and medium spans, and to stay weaker
than the 2B on large ones.

The method: synthetic workspaces summarized by a verified teacher model, each
call kept as a (prompt, answer, kept) pair; a LoRA adapter trained on them by
supervised fine-tuning at 8,192 tokens of context; the merged model quantized to
GGUF and pinned in `daemon/sum_fetch.h`; and the same 30 spans graded on a
2-vCPU, 4 GB machine.

**What to watch for.** The 30-span set must not be among the training
workspaces. Synthetic chat is tidier than real chat; mix in real messages where
allowed, and keep the integration-post channels, where the small models failed
most. A pair is valid only for the prompt version it was made with: a new
`SUM_PROMPT_VERSION` means new pairs. The base model's licence travels with the
tuned file (Qwen3.5 is Apache 2.0).

## 8. Settings

| Variable | Values |
|---|---|
| `OPENCHIME_SUMMARY` | `off` (default), `local`, or `cloud:<url>,<model>` (§6): the API's base, e.g. `https://models.example.com/v1` (requests go to `<base>/chat/completions`; https, or http to a loopback address), and the model's name there, which names the summaries' version, so changing it remakes them. |
| `OPENCHIME_SUMMARY_API_KEY` | For `cloud`: its key, sent as a bearer token; none when empty. |

Everything else — the chunk size, the gap, the standard's caps, the gate's
thresholds, the hosted engine's retries — is a constant in the code
(`daemon/sum_core.h`, `daemon/sum_load.h`, `daemon/sum_worker.h`,
`daemon/sum_llama.c`, `daemon/sum_cloud.c`).
