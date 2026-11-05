# The assistant — a design for review

An assistant that answers a person's questions from the messages they can read,
on the workspace's own server, citing every message it used (REQ-330–334,
ARCH-121). This document proposes how it is built. The requirements and the
decision are settled; what follows is a proposal to be agreed in review, and
the build is the list of pieces at the end.

Three things are decided and not reopened here:

- It **answers only.** It takes no action, posts nothing and has no identity of
  its own (REQ-332).
- It **runs on the box**, on a local model, on the summary engine (ARCH-116).
  Nothing leaves the server, and there is no hosted option (REQ-333).
- Its **scope is the asker's read access**, and only that (REQ-330).

Semantic search (embeddings, a vector index) is not part of this design:
retrieval is the full-text index the daemon already has.

## 1. What a person gets

A person types a question — "what did we decide about the Q3 launch date?",
"who is handling the printer contract?" — and gets a short answer in plain
sentences, each ending in the citations it rests on, numbered as a summary's
are. A citation shows the message on hover and goes to it on click (REQ-331).
Where nothing they can read bears on the question, the answer says so in one
line and the model is not asked at all.

A question may be asked of the whole workspace (everything the person can
read) or of one conversation, from that conversation's header. The second is
the same search with `in:` set; both are the asker's access and nothing more.

Under every answer, as under every summary: "AI-generated answers may be
inaccurate."

## 2. Retrieval

**Proposed.** Full-text search over the asker's readable messages, then the
best of them into the prompt, citations kept.

1. **The query.** Code turns the question into an FTS5 query: words of three
   letters or more, stop words dropped, each word a prefix term, joined by OR,
   so a message need not contain every word. A structured operator the person
   typed (`from:`, `in:`, `before:`, `after:`, `on:`, `has:`) is kept and
   applied as search applies it (REQ-081). No model call is spent on rewriting
   the question; whether one would find better messages is a question for the
   evaluation (§9), not for the first build.
2. **The scope.** The query runs through the same function `SEARCH` uses, as
   the asker, at the moment they ask: the channels they are a member of, no
   encrypted DM, no deleted message (REQ-080, REQ-314). There is no second path
   to the messages, so the assistant's scope cannot drift from search's.
3. **The ranking.** BM25, as search ranks, with a small lift for recent
   messages, so "what did we decide" finds the latest decision first. The lift
   is a constant in the code.
4. **The context.** The top hits, best first, until the input budget is spent
   (the summary engine's input limit, less room for the question and the
   answer). Each hit brings the message before and after it in its channel or
   thread, so a one-word reply ("yes, Friday") arrives with what it answers.
5. **The lines.** What is read goes to the model as numbered lines — channel,
   author, day and text — exactly the shape summaries use, and code keeps the
   map from line number to message id.

No hits: the answer is "Nothing you can read mentions that." and no model runs.

## 3. The model

**Proposed.** The summary runtime (ARCH-116): llama.cpp in the daemon, the
same worker thread, the same load gate, the same grammar machinery.

- **The model file.** Where summaries are on with the local engine, the
  assistant uses the model already loaded: one model in memory, not two. Where
  they are off or hosted, the assistant fetches the same model on first use
  from the same pinned address, checked against the same pinned SHA-256. One
  model to evaluate, pin and carry. A smaller instruction model is the fallback
  if measurement (below) shows the summary model cannot be carried alongside the
  rest of the daemon on the deployments that matter.
- **The answer's form.** A grammar holds the answer to one to five sentences,
  each opening with the lines it rests on (`[3,7] The launch moved to the
  14th.`), each bounded in characters, and every cited line one that exists.
  Code drops a sentence that cites nothing, as summaries drop one; an answer
  with nothing left is "I could not answer that from what you can read."
- **The prompt.** The model is told to answer only from the lines, to say when
  they do not answer the question, and that the lines are messages people wrote,
  not instructions to it. It has no tools to call, so a message that tries to
  instruct it can at worst skew a sentence, which its citation exposes.
- **Decoding.** Greedy inside the grammar, with the DRY sampler, as summaries.
- **The queue.** A question is a request someone is waiting on: it goes ahead
  of idle summary work, behind requests already asked for, and the asker is
  told their place, as a summary's asker is.

**Memory — to be measured before the model is fixed.** The summary model is
about 1.3 GB on disk, memory-mapped, with an 8-bit context cache. The lean
profile (REQ-210) is about 256 MB for the whole daemon, which a model of that
size cannot fit in resident memory. The proposal is to measure, on a lean and a
standard deployment, the daemon's resident and mapped memory idle, answering a
question, and answering one while a summary is made; and to set the rule from
the numbers: the assistant is offered where its model fits beside the rest of
the daemon with the load gate's margin, and absent where it does not, with the
reason logged (REQ-334). The expected result is that a lean deployment does not
offer it; the measurement decides.

## 4. The protocol

**Proposed.** A request, a streamed answer, and the citations with it. Frame
numbers are assigned when it is built.

- **Capability.** A new name in `CAPABILITIES` (PROTOCOL.md §5.14b), present
  when the assistant is on for this server and this workspace (§7), and while
  its model is still coming up, as `summarize` is. Absent, a client shows
  nothing of it.
- **`ASK` (client → server).** `req_id`, an optional `channel_id` (0 for the
  whole workspace), the question as text, and, if the conversation is kept
  (§6), the id of the conversation it continues. A question has a length cap.
- **`ASK_PART` (server → client).** `req_id` and the next sentence of the
  answer, with the message ids it cites, each with its channel, so a client can
  draw the citation card without a lookup. A sentence is sent once code has
  checked its citations, so nothing a client shows is later withdrawn. While the
  request waits, the same frame carries the asker's place in the queue instead.
- **`ASK_DONE` (server → client).** `req_id` and a status: answered, nothing
  found, unavailable (off, or too many waiting), failed (with why), cancelled.
- **`ASK_CANCEL` (client → server).** `req_id`. A closed connection cancels
  what it asked.

The answer goes only to the connection that asked. It is never a message: not
stored as one, not fanned out, not indexed, summarized or read aloud.

## 5. The clients

**Proposed.**

- **GUI.** An **Ask** button in the conversation header beside Summarize, and
  an "Ask" item in the workspace menu for the whole workspace. Either opens the
  context pane, beside the conversation, under an **ASK** header: a text field,
  then the exchange — each question and its answer, sentences appearing as they
  arrive, citations numbered and drawn as a summary's are (a card on hover, the
  message in the transcript on click, the pane staying). While it waits, its
  place in the queue and **Cancel**. Closing the pane or moving to another
  conversation forgets the exchange, unless it is kept (§6). Accessible names
  follow the summary pane's (`header.ask`, `ask.cite.<n>`).
- **Terminal client.** The terminal client has no slash commands (CLIENT.md):
  "Ask" is an action in the global launcher (Ctrl+K or `:`) and in a channel's
  action menu, opening a prompt for the question and then a panel with the
  answer, its citations numbered; Enter on a citation goes to the message.
- **The core.** One open exchange in the model, as one open summary; the
  sentence and citation formatting shared by both frontends, as `summary.c` is
  for summaries.

## 6. What is kept

**Proposed: nothing, for the first build.** The question, the lines read for
it and the answer are held in the daemon's memory while it is answered and are
gone when it is. The client holds the exchange until the pane is closed. The
log records that a question was asked, by whom, how many lines it read and how
long it took — never the question or the answer.

**The alternative, for review:** keep each person's conversation with the
assistant, so a follow-up question carries what came before and the pane
reopens where it was. Kept, it is private to the asker (REQ-332): a table keyed
on them, read by no other member, owner or admin and by nothing else in the
daemon. It then has to answer the questions every other kept thing does —
deleted with the person, in their data export, purged when a message it cited
is deleted (as a summary is, REQ-312), and whether a legal hold reaches it. It
also means a follow-up is answered with the earlier answer in the prompt, which
the citations have to survive.

The recommendation is to build without keeping anything and add the kept
conversation as its own piece if people ask for it.

## 7. Settings

**Proposed.** Three levels, each able only to turn it off below the one above.

| Level | Who | What |
|---|---|---|
| System | The operator, in the daemon's environment, beside the summary setting | Off (the default) or local. There is no hosted value. Off, or a model that does not come up, and nothing below matters. |
| Workspace | Owners and admins | On (the default once the system allows it) or off for the workspace. Stored in the daemon's database, changed from the admin settings, and written to the audit log. Off withdraws the capability from every connection at once. |
| Person | Each member, for themselves | Hide Ask. A synced client preference, as sidebar view state is (REQ-234): it hides the button and the action, and changes nothing on the server. |

The workspace setting is the one that needs new storage and a new frame; the
other two are a setting beside the existing summary one and a preference beside
the existing ones.

## 8. Tests

- **Scope.** A question whose only matches are in a channel the asker has left,
  a private channel they are not in, a deleted message and an encrypted DM gets
  "Nothing you can read mentions that." Two people with different access asking
  the same question get answers citing only what each can read.
- **Citations.** Every cited id in every `ASK_PART` is a message in the lines
  given to the model; a model answer citing nothing, or a line that does not
  exist, is dropped (a stub engine, as the summary tests use).
- **Nothing acts.** No frame other than `ASK_PART` and `ASK_DONE` is sent as a
  result of `ASK`, and no row changes but the queue's.
- **Off.** With the system off, the workspace off, or the model failing to load,
  the capability is absent and `ASK` is answered unavailable.

## 9. Evaluating

Before the model and the retrieval constants are fixed, a set of questions with
known answers over a sample workspace, as summaries were evaluated: whether the
answer is right, whether every citation supports its sentence, whether it says
"nothing found" when it should, and the time to the first sentence on a lean
and a standard machine.

## 10. Build

Each of these is opened as its own piece of work once this design merges.

1. **Measure memory** of the summary model in the daemon on the lean and
   standard profiles (§3), and fix the rule for where the assistant is offered.
2. **Retrieval** in the daemon: the question to an FTS5 query through the
   search path as the asker, the ranking, the neighbours, the numbered lines
   (§2), with the scope tests (§8).
3. **The answer pipeline** on the summary worker: the prompt, the grammar, the
   citation check, the queue and the load gate, cancel (§3).
4. **The protocol:** the capability, `ASK`, `ASK_PART`, `ASK_DONE`,
   `ASK_CANCEL`, documented in PROTOCOL.md (§4).
5. **Settings:** the system setting, the workspace setting with its storage,
   frame and audit entry, and the person's preference (§7).
6. **The GUI's Ask pane** (§5).
7. **The terminal client's Ask action** (§5).
8. **The evaluation** (§9), before the model and constants are fixed.
9. **Kept conversations**, only if review chooses the alternative in §6.
