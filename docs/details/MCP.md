# MCP

How an AI agent, or a person's own AI client, reads and posts in a workspace
through the Model Context Protocol (REQ-320–325, ARCH-120). This document is the
daemon-side design, with the tools, their paging, the agent token lifecycle and
the audit (§8–§11); the decision and what was rejected are ARCH-120.

Built: the endpoint for agents (`daemon/mcp.c` reads and writes the JSON,
`daemon/netloop.c` serves `/mcp`, `daemon/dbwriter.c` reads, posts and audits).
A person's own AI client (§3.2) is not built yet: until it is, every token the
endpoint accepts is an agent's.

Nothing here is a setting's name, a table or a frame. Those are written into
CONFIG.md, SCHEMA.md and PROTOCOL.md with the code that has them.

## 1. Endpoint and transport

The daemon speaks MCP itself, on one more route of its own HTTP listener
(ARCH-32): **`POST /mcp`**, on port 443 beside the webhook, reached by any
connection that does not negotiate `oc/1` (ARCH-54). There is no adapter
process and no second listener.

The route exists only when the operator turns MCP on. While it is off, `/mcp`
is a path like any other the listener does not know, and is answered `404`.

The transport is MCP's Streamable HTTP as the 2026-07-28 revision defines it,
which is stateless, and the daemon uses only the part of it that a
one-request-per-connection listener can serve:

- **One JSON-RPC request per POST, one JSON object back** (`Content-Type:
  application/json`). The daemon never answers with an event stream, so it
  holds nothing open after the answer; the connection closes, as every HTTP
  connection to the daemon does.
- **No session.** No `Mcp-Session-Id` is minted or read, and nothing about a
  client is kept between its requests. Each request carries its credential,
  its protocol version and its capabilities, and is judged on those alone.
- **No `GET`, `DELETE` or `subscriptions/listen`.** `GET` and `DELETE` on `/mcp`
  are `405`, as the revision asks of a server that does not serve the earlier
  transports; `subscriptions/listen` is a method the daemon does not have
  (`404` with JSON-RPC `-32601`). A client learns of new messages by reading
  again.
- **A body limit for the route**, refused with `413` before the body is read,
  as for every route (ARCH-32). A post's text is held to the message size
  limit; the route's limit is that plus room for the envelope.
- **Headers checked against the body.** `MCP-Protocol-Version`, `Mcp-Method`
  and, for `tools/call`, `Mcp-Name` must be present and match the body, or the
  request is `400` with `HeaderMismatch` (`-32020`).
- **`Origin` checked.** A request that carries an `Origin` other than the
  workspace's own `https` origin is `403`. An agent or a desktop AI client
  sends none; the check stops a web page in someone's browser from driving the
  endpoint.

A request is parsed and refused on the I/O thread where it can be, as every
route's are (ARCH-32): a malformed head, a method other than `POST`, a body
over the limit. One that is well formed goes to the event loop, which checks
its headers against its body before anything else, which checks the
credential, reads through the readers and posts through the writer, exactly
as it does for a client's frames (ARCH-52).

The methods served are `server/discover`, `tools/list` and `tools/call`. The
daemon offers tools only: no resources, prompts, completions or tasks, and it
never returns an input-required result, since nothing it does needs to ask the
client a question part-way.

## 2. Protocol version

The daemon speaks **one revision, 2026-07-28**. Every request names its
revision in `_meta` (`io.modelcontextprotocol/protocolVersion`) and in the
`MCP-Protocol-Version` header, and they must agree.

- A request on 2026-07-28 is served.
- A request on any other revision is `400` with
  `UnsupportedProtocolVersionError`, listing `["2026-07-28"]` as supported. A
  client that speaks both eras reads that and retries on the revision the
  daemon speaks.
- A request with no version, or an `initialize`, is the earlier era's
  handshake and is refused the same way. The daemon does not implement
  `initialize`: the earlier revisions need a session, and a session is the
  state this design keeps out of the daemon.
- `server/discover` answers without a credential, with the supported revision,
  the daemon's name and version, and its one capability, `tools`. It names
  nothing about the workspace.

When a later revision comes, the daemon adds it to the supported list and
serves each request on the revision that request names; a revision is dropped
only in a release that says so.

Every result carries `resultType: "complete"` and the daemon's
`io.modelcontextprotocol/serverInfo` in its `_meta`.

## 3. Principals

Every request but `server/discover` carries `Authorization: Bearer <token>`. A
request without one, or with one the daemon does not hold, is `401` with a
`WWW-Authenticate: Bearer` challenge naming the workspace's protected-resource
metadata, so a client that can sign a person in knows where to start (§3.2). A
refused token is a failed authentication: it is counted against the source's
failed-authentication limit (REQ-191) and recorded in the security family of
the audit log (REQ-251), never with the token.

A token is one of two kinds, and the daemon knows which from the token
itself.

### 3.1 Agents

An agent is an account of its own kind: a `users` row whose kind is `agent`
(the `users.kind` column that visitor accounts bring; this design adds no
second column). It has a name and a handle like a person, so its posts and
mentions read naturally, and no credential of a person's: no password, no
identity-provider subject, no sessions, no devices. It cannot sign in to a
client, has no presence, and is never in a DM or group DM.

An agent authenticates with a **bearer token the daemon minted**, carrying the
scopes chosen for it when it was minted. An owner or admin creates the agent and
mints its tokens from the client; a token is 32
random bytes, shown once, stored as its SHA-256 and found by one indexed probe,
exactly as session and webhook tokens are (ARCH-58/71). An agent may hold
up to 10, so one can be replaced without a gap; an eleventh is refused.
Revoking a token deletes its
row, and the next request with it is `401`; removing the agent deletes its
tokens and its memberships. Minting needs no identity source and no
OpenChime-operated service, so agents work on a workspace with nothing else
configured, and on one that is air-gapped.

Agents do not count against the registered-user cap (AUTH.md §2): they are not
people, and a workspace at its cap can still add one.

**Central-brokered agent identity** is not built here. When it is, an agent of a
connected self-hosted or managed workspace will present an ES256 token central minted for
the workspace's ratified `aud`, verified as a relay sign-in is (ARCH-56/57),
short-lived and refreshed by the agent from central, so the daemon still
contacts no one. Daemon-minted tokens are complete without it.

### 3.2 A person's own AI client

A person may connect an AI client of their own — a desktop assistant, an
editor — and it then acts **as them, with exactly their access**: the channels
and DMs they can read and post in, no more. Nothing it does is possible that
the person could not do in a client.

It signs in with **MCP's OAuth authorization flow, with the daemon as the
authorization server**. In outline: the `401` challenge names the daemon's
protected-resource metadata; the client finds the daemon's authorization
server metadata from it, identifies itself, and opens the person's browser at
the daemon's authorize page with PKCE; the person signs in there as they
always do (ARCH-59, AUTH.md §8.10, or through the workspace's identity source)
and approves the client by name; the client redeems the code for tokens bound
to this workspace's `/mcp`. The daemon stores the tokens hashed, as it does
sessions, and a person sees their connections and ends any of them; removing a
person ends theirs. An ended connection's next request is `401`.

The authorization server — its metadata, client identification, the approval
page, token lifetimes and refresh, and the connection list — is its own change
and its own document. This one fixes only what it must: the daemon is the
authorization server, the principal is the person, and their access is the
whole of the client's.

Encrypted DMs (REQ-314) are not readable through a connection: the daemon
cannot read them, and they are left out of everything §4 returns.

## 4. What a principal can read

Every read is checked on the daemon at the moment it is made, against the same
membership the binary protocol checks; nothing is cached from an earlier
request.

- **An agent reads the channels it is a member of**, and only those — through
  `channel_members`, the effective membership every query already reads
  (ARCH-114) — and only as far as its token's scopes allow: the scopes an owner
  or admin chose for it from the app platform's catalog (plans/APPS.md §4),
  checked as a bot's are (ARCH-120). Being in a channel gives it what being in a channel gives a
  member: the channel's history and its threads. It reads no DM, no group DM
  and no channel it has not been added to, public or not; a public channel is
  open for a *person* to join, and an agent joins nothing by itself.
- **A connection reads what its person can read**: their channels, DMs and
  group DMs, but not encrypted DMs.
- **Deleted messages are not returned**, and a message deleted between two
  pages simply is not on the second. Retention (REQ-250) has already removed
  what it removes; the MCP surface reads what is stored, like any client.
- **A channel the principal cannot read is answered as one that does not
  exist**, so a refusal does not tell an agent which channels there are.

Search reads the same full-text index the clients search (ARCH-15), restricted
to these channels before ranking, so a result never comes from a channel the
principal cannot read.

## 5. Rate limits

Each principal — an agent, or a person's connection — is limited on its own,
so one busy agent does not slow another or the person whose client is
connected.

- **Posts** are limited exactly as a person's sends are (REQ-190): an agent's
  posts count against its own account's limit, and a connection's against its
  person's, shared with their other clients.
- **Tool calls** have a budget per token, a bucket refilled continuously: 60
  calls a minute with a burst of 20, a search costing three since it scans the
  index. An operator may move both. `tools/list` and `server/discover` cost
  nothing.
- **Failed authentication** is limited per source (REQ-191): 20 refused tokens
  a minute, after which the source is answered `429` without its token being
  looked up, so guessing tokens is as slow as guessing passwords.

A request over a limit is `429` with `Retry-After` in seconds, before any read
is made. The limits are the daemon's protection, not a quota anyone buys.

## 6. Adding an agent to a channel

An agent is created by an owner or admin, in the workspace's member
administration, where it is listed with the people but marked as an agent;
creating one mints its first token and shows it once.

**An owner or admin adds an agent to a channel** from the channel's member
list, as a member is invited, and removes it the same way. In a private
channel, the owner or admin must be a member themselves: adding an agent shows
the channel's history to whoever holds its token, and only someone who can
already read that history may decide it. Members cannot add an agent; an agent
cannot add itself, join a channel, leave one, or be put in a channel through a
user group.

Adding or removing an agent is told to the channel's members as a member
joining or leaving is, and recorded in the administrative family of the audit
log.

## 7. How members see an agent

Everywhere a person's name appears, an agent's carries an **Agent** tag beside
it: the transcript, the member list, mention suggestions, a profile, search
results and a summary's list of who posted (REQ-321). An agent shows no
presence, no status and no local time. A channel's member list shows its
agents in a group of their own, below the people, so who can read the channel
through a token is plain at a glance.

**A post through a person's connection** is theirs, under their name, marked
as sent through their AI client by the client's name (REQ-324), in the
transcript and in every excerpt cut from it.

An agent can be mentioned; the mention is recorded as any is, and the agent
finds it by reading or searching. Nothing is pushed or notified to an agent,
which has no device.

## 8. Tools

Three tools: **`read_history`**, **`search`** and **`post`**. They are the same
for both principals; what differs is which conversations they reach (§4).

`tools/list` returns them in that order, every time. Each tool's `channel`
parameter is an `enum` of the conversations the caller can read, by name —
`#general` for a channel, `@alice` for a DM, `@alice,@bob` for a group DM — so a
client learns where it can read and post from the tool list itself and needs no
fourth tool to find out. Because that list is the caller's own, the result is
`cacheScope: "private"`, with `ttlMs` 60 000, so a channel an agent was just
added to appears within a minute. A conversation is always also accepted by its
numeric id, which does not change when a channel is renamed, and by its name
without the `#`.

A call to a conversation the caller cannot read is a tool error ("no such
conversation"), the same whether it exists or not.

### 8.1 `read_history`

Reads a conversation's messages, or one thread's.

| Argument | | |
|---|---|---|
| `channel` | required | name or id |
| `thread` | optional | a message id: read that thread's replies instead of the conversation |
| `before` | optional | a cursor from an earlier page, or a message id: messages older than it |
| `after` | optional | a cursor or message id: messages newer than it |
| `since` | optional | a time (RFC 3339): messages at or after it |
| `limit` | optional | messages to return, 1–200, default 50 |

With none of `before`, `after` or `since`, it returns the newest messages. At
most one of `before` and `after` may be given. `since` alone reads forwards
from that time, oldest first; with `before` it bounds a backward read.

Each message is returned as:

| Field | |
|---|---|
| `id` | the message id |
| `author` | `name` (the display name) and `handle` (empty for an agent) |
| `author_kind` | `person`, `agent` or `webhook` |
| `via` | for a post through a person's connection, the client's name |
| `at` | time, RFC 3339 in UTC |
| `text` | the body, as written (Markdown, MARKDOWN.md) |
| `edited` | `true` if it has been edited; absent otherwise |
| `replies` | for a message that starts a thread, how many replies it has; absent when none |
| `files` | for each file attached: name, type and size, not content |
| `truncated` | present when `text` was cut (§9) |

`via` is written only for a post through a person's connection (§3.2), so an
agent's reads carry none until that is built.

Messages within a page are oldest first, so a page reads as the conversation
does. The result's `structuredContent` holds the messages and the cursors; its
`content` holds the same JSON as one text block, for a client that passes text
straight to a model.

### 8.2 `search`

Searches the conversations the caller can read, with the same full-text
search and query syntax the clients use (ARCH-15).

| Argument | | |
|---|---|---|
| `query` | required | the search |
| `channel` | optional | name or id: search only there |
| `from` | optional | a handle: only messages by that author |
| `since`, `until` | optional | times (RFC 3339) bounding the search |
| `cursor` | optional | from an earlier page |
| `limit` | optional | results to return, 1–50, default 20 |

Each word of the query must match; the words are taken as words, not as
operators. Results are best match first, each a message as in §8.1 with its
conversation added (`channel`, `channel_id`) and `text` replaced by an excerpt
around the first word of the query (`excerpt`, at most 300 characters). When
there are more, the result carries `cursor`. `read_history` with `before` and
`after` around a result's id reads its context. Without `channel`, a search
covers the public channels the agent is in when its token has
`channels:history`, and its private ones when it has `groups:history`.

### 8.3 `post`

Posts a message as the caller.

| Argument | | |
|---|---|---|
| `channel` | required | name or id |
| `text` | required | the body, at most the message size limit (64 KiB, REQ-054) |
| `thread` | optional | a message id: post as a reply in its thread |
| `key` | optional | an idempotency key, up to 64 characters |

The message is an ordinary one: delivered, notified, searchable and retained
as any other, under the caller's name — an agent's as an agent's, a person's
marked as sent through their client (REQ-324). It is posted through the same
send path and permission check as a client's message, so a channel the caller
may read but not post in, such as an archived one, refuses it as it would
refuse the person.

**`key` makes a retry safe.** The 2026-07-28 revision drops a request whose
connection breaks, and a client re-sends it as a new request; with the same
`key`, the daemon returns the first post's id instead of posting twice. It is
the send path's idempotency mapping (ARCH-44), the key hashed to the token that
mapping takes, scoped to the caller and the conversation.

The result is the message's `id` and `at`.

Editing, deleting, reacting, opening a DM and uploading a file are not tools.
They can be added later, each a tool with its own audit action; none is
needed to read a channel and answer in it.

## 9. Paging and truncation

A channel may hold years of messages and single messages up to 64 KiB, and a
model's context is small. The tools are built so that no call returns more
than a client can use, and so that paging through a long channel is exact.

- **Cursors are message ids.** Message ids increase with time and are never
  reused (ARCH-43), so "older than message N" means the same thing on every
  call: a page does not shift when someone posts, nothing is skipped or
  repeated between pages, and a message deleted meanwhile is simply absent.
  A result carries `older` and `newer` cursors when there is more in that
  direction, and neither past the end. A search cursor encodes the last
  result's rank and id, so it pages through a snapshot of the ranking.
- **A page has a size limit as well as a count**: 64 KiB of text in all. When the next message would pass it, the page ends there, short of
  `limit`, and the cursor continues from it; a page always holds at least one
  message.
- **A long message is cut, and says so.** A message's `text` is cut at 8 000
  characters, at the last whole line before it (at 8 000 when it has no line
  break), and the message
  carries `truncated: {"length": <full length in characters>}`. A page of one
  message is cut only at the page limit, so `read_history` with `before` one
  more than its id and `limit` 1 returns it whole, for a client that needs all
  of it.
- **A thread is read on its own.** A conversation's history lists a thread's
  first message with its reply count, not the replies, so a busy thread does
  not crowd out the channel; `thread` reads the replies, paged the same way.

## 10. Agent tokens

### 10.1 Minting

An owner or admin mints a token for an agent from the agent's page in member
administration, naming it ("CI runner", "laptop"). The daemon makes 32 random
bytes and shows the token once, as a fixed prefix and the bytes in base64url,
so a secret scanner can recognise a leaked one. The minter chooses the token's scopes from the app platform's catalog
(plans/APPS.md §4); the tools need `channels:history` to read public channels,
`groups:history` to read private ones, and `chat:write` to post. A call without
the scope it needs is a tool error naming the scope. It stores only:

- the token's SHA-256, under a unique index, so a request's lookup is one
  probe;
- the agent it belongs to, its scopes, its name, who minted it and when;
- when and from which address it was last used.

An agent may hold several tokens (at most 10), so a token is replaced
by minting the new one, moving the agent to it, then revoking the old. Minting
is recorded in the administrative audit family, with the agent and the token's
name, never the token.

Tokens do not expire on their own: an agent is a service, and a token that
lapsed would stop it without anyone choosing to. Revoking is the way a token
ends.

### 10.2 Listing

The agent's page lists its tokens: name, who minted it, when, and when and
from where it was last used. The last use is written at most once a minute per
token, so a busy agent does not turn every read into a write. The token itself
is never shown again.

### 10.3 Revoking

An owner or admin revokes a token from that list. Revoking deletes the row in
the writer, so the **next request with the token is `401`**: every request
looks its token up afresh, and nothing about a token is cached. A request
already being answered when the row goes finishes. Revoking is recorded in the
administrative family.

Removing an agent deletes all its tokens and its channel memberships in one
transaction; its messages stay, under its name. A token whose agent is gone is
therefore refused the same way.

## 11. Audit

Every tool call writes one row to the **MCP access** family of the audit log
(REQ-251, REQ-325), written by the writer once the call is answered, as every
other entry is written. The family ages out on its own, after 365 days, in the
storage pass, whatever age the other families keep (REQ-251a/b), so heavy
reading cannot push older entries of other families out.

| Field | Written as |
|---|---|
| `action` | `mcp.read_history`, `mcp.search` or `mcp.post` |
| actor | the agent, or the person whose connection it was |
| `target` | the conversation; for a search over every conversation, none |
| `outcome` | 1 served, 0 refused (wrong arguments, no such conversation, a missing scope, not allowed to post, over a limit) |
| `detail` | the principal — `token=<the token's name>`, or the connection's client — then, by action, below; `refused` for a refusal, `over the limit` for a call refused `429` (whose row has no target, since the call is refused before its conversation is looked up) |

| Action | `detail` adds |
|---|---|
| `mcp.read_history` | `first=`, `last=` and `count=`: the ids of the first and last message returned, and how many |
| `mcp.search` | `results=` and `conversations=`: how many results, and in how many conversations; **not the query**, which is the caller's words about the content and is kept out of the log as message content is |
| `mcp.post` | `message=`, the message id |

No row holds a message's text, an excerpt, a query or a token. `tools/list`
and `server/discover` read nothing of anyone's and write no row. A request
refused by the per-source limit on failed authentication is refused before its
token is looked up, so it is no tool call and writes none. A request refused for its
credential is not in this family: it is a failed authentication
(`mcp.auth_failed`), in the security family (REQ-251).

An owner or admin reads the family in the audit view beside the others, and
can narrow it to one agent or one person, which answers "what has this agent
read, and when".
