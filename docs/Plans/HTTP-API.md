# The HTTP API

A proposal, for review, of the documented HTTP API through which outside tools
read history, search and post (REQ-176, REQ-350–354). The decision to have it,
beside the MCP surface, and what was rejected are ARCH-123. Nothing here is
built yet; once reviewed and built, the reference moves to a document of its
own beside PROTOCOL.md and this plan is retired.

What is decided is stated as decided; everything else is a proposal, and the
points left open for the reviewer are marked **Open**.

## 1. Who it is for

Conventional integrations: a script, a CI job, a back-office system, written
once against a fixed reference and run unattended. An agent — a model choosing
what to do at run time — uses the MCP surface instead. Both enter as the same
kind of principal, under the same checks, into the same audit family
(ARCH-123); they differ only in shape.

The incoming webhook (ARCH-71) stays as it is: one channel, post only, a token
in the URL. The API is for anything that has to read, or post in more than one
place.

## 2. Base path and versioning

Decided: the API is served under **`/api/v1/`**, on the daemon's existing HTTP
surface — port 443 beside the wire, by ALPN (ARCH-54), HTTP/1.1 (ARCH-33), with
the daemon's one certificate (ARCH-34). No new listener and no new port. The
paths below are relative to `/api/v1`.

Within `v1`, changes are **additions only**: a new endpoint, a new field in a
response, a new optional parameter, a new error code. A client ignores fields
it does not know. Anything else — removing or renaming a field, changing a
type or a meaning, making a parameter required — is `v2`, served beside `v1`
(REQ-350).

Proposed:

- A superseded version is served for at least **twelve months** after its
  successor ships, and every response from it carries a `Deprecation` header
  and a `Sunset` header with the date it stops.
- `GET /api/v1/` answers `{"version": 1}` without a token, so a client can
  tell whether a daemon serves the version it was written for.

## 3. Authentication

Decided: every request carries

```
Authorization: Bearer <agent token>
```

— a token the daemon minted for an **agent principal**: a non-human account an
owner/admin creates and revokes, stored only as its hash (as a session token,
ARCH-58), whose channel memberships are its only access (REQ-351). The token's
format, how it is minted, shown and revoked are the agent principal's own
design; the API only presents it.

- A token is accepted in the `Authorization` header and nowhere else — never in
  the query string, where proxies and logs keep it.
- A missing, unknown or revoked token is `401` with
  `WWW-Authenticate: Bearer`, the same answer for each, so the API does not say
  which tokens once existed.
- A person's session token, and the web client's sign-in, do not open the API
  (ARCH-123).
- Revoking a token, or removing the agent from a channel, takes effect on the
  next request; nothing is cached across requests.

## 4. Resources

Every endpoint runs the checks the wire handler for the same act runs, against
the agent's memberships at the time of the request (REQ-352): reads and search
on the reader threads (ARCH-66), posts through the writer's send path.

### 4.1 Channels

**`GET /channels`** — the channels and DMs the agent is a member of,
archived ones included and marked. An encrypted DM is never listed (REQ-314):
the daemon cannot read it.

```json
{"channels": [
  {"id": "41", "name": "deploys", "kind": "channel", "topic": "Release train",
   "archived": false, "public": true, "last_message_id": "90211"}
]}
```

`kind` is `channel` or `dm`. Proposed: no paging in `v1` — an agent is a member
of tens of channels, not thousands; if that changes, a `before` parameter is an
addition.

### 4.2 History

**`GET /channels/{id}/messages`** — the channel's top-level messages, **paged
by message id** (keyset), newest first:

| Parameter | Meaning |
|---|---|
| `before` | Messages with ids lower than this. Absent: from the newest. |
| `after` | Messages with ids higher than this, oldest first — for catching up from the last one seen. |
| `limit` | Up to 100; default 50. |

`before` and `after` together are a `400`. The response:

```json
{"messages": [ <message>, ... ], "next": "90150"}
```

`next` is the id to pass as `before` (or `after`) for the next page, or `null`
at the end. Because it is an id, not an offset, a message posted or deleted
between two pages cannot make one repeat or vanish (ARCH-123), as with the
wire's search and backfill.

**`GET /messages/{id}`** — one message.

**`GET /messages/{id}/replies`** — a thread's replies, oldest first, paged by
`after` and `limit` as above.

### 4.3 Search

**`GET /search?q=<query>`** — the workspace's search, with its **structured
operators** (REQ-081): `from:`, `in:`, `has:`, `before:`/`after:`/`on:`,
combined with free text, parsed by the same `shared/searchq.c` the clients
use, and bounded to the channels the agent may read (REQ-031).

| Parameter | Meaning |
|---|---|
| `q` | The query, operators included. |
| `sort` | `newest` (default), `oldest` or `relevance`. |
| `cursor` | The `next` of the previous page. |
| `limit` | Up to 50; default 20 (the wire's cap). |

```json
{"results": [
  {"message_id": "90190", "channel_id": "41", "author_id": "7",
   "created_at": "2026-10-30T14:02:11.482Z",
   "snippet": "…rollback of the gateway finished…"}
],
 "next": "eyJiIjoiOTAxOTAifQ"}
```

Here `next` is an opaque string, not an id: sorted by relevance, the keyset is
the rank and the id together (PROTOCOL.md §5.11). It is still a keyset, not an
offset. The snippet is plain text, cut around the match as the clients show it.

### 4.4 Posting

**`POST /channels/{id}/messages`** — posts as the agent:

```json
{"text": "Deploy 412 finished in 3m 12s.", "parent_id": "90190"}
```

- `text` is the message body in the workspace's formatting (ARCH-100), with
  mentions resolved as a person's are (ARCH-89); at most `MAX_BODY_SIZE` (64
  KiB, REQ-054).
- `parent_id`, if present, makes it a reply in that message's thread, as a
  person's reply is; the parent must be in the same channel.
- An `Idempotency-Key` header (any string up to 64 characters) is mapped to the
  wire's idempotency token for the agent (ARCH-44): a retry with the same key
  returns the first post's message and posts nothing; the same key with a
  different body is `409`.
- The answer is `201` with the message as posted. It fans out to everyone in
  the channel through the same path as any message; the author is the agent,
  under its own name.
- An archived channel refuses the post, as it does on the wire.

Proposed for later, as additions: editing and deleting the agent's own
messages, reactions, and attachments. `v1` posts text.

## 5. JSON shapes and errors

### 5.1 Conventions

- Bodies are `application/json; charset=utf-8`; a `POST` with any other
  `Content-Type` is `415`.
- **Ids are strings.** The daemon's ids are `u64` (ARCH-43), and a JSON number
  past 2⁵³ is silently rounded by JavaScript and by many JSON libraries.
- Times are RFC 3339 in UTC, to the millisecond.
- Field names are `snake_case`. A field with no value is `null`, not absent.

### 5.2 A message

```json
{"id": "90190", "channel_id": "41",
 "author": {"id": "7", "name": "dana", "kind": "person"},
 "author_name": null,
 "created_at": "2026-10-30T14:02:11.482Z", "edited_at": null,
 "text": "Rolling back the gateway.",
 "parent_id": null, "reply_count": 3,
 "deleted": false, "kind": "message",
 "attachments": [{"name": "trace.txt", "size": 18233, "type": "text/plain"}]}
```

- `author.kind` is `person` or `agent`. `author_name` is the display name a
  webhook post carries (ARCH-71), `null` otherwise.
- A deleted message is its tombstone (REQ-052): `deleted: true`, `text` and
  `attachments` `null`, the rest kept, so thread counts and replies still make
  sense.
- `kind` is `message`, `action` (REQ-058; `text` is the action's text), `poll`
  (`text` is the question) or `call` (a call event). Polls' options and tallies,
  and attachment downloads, are not in `v1`; attachments are named, not served.

### 5.3 Errors

Every error is the HTTP status and one body shape:

```json
{"error": {"code": "not_found", "message": "No such channel."}}
```

`code` is the contract; `message` is for a person reading a log and may change.

| Status | `code` | When |
|---|---|---|
| 400 | `bad_request` | A malformed body or parameter |
| 401 | `unauthenticated` | No token, or one not known or revoked |
| 403 | `archived` | A post to an archived channel |
| 404 | `not_found` | No such route, channel or message — **or one the agent may not read** |
| 409 | `idempotency_conflict` | An `Idempotency-Key` reused with a different body |
| 413 | `too_large` | A body over its limit |
| 415 | `unsupported_media_type` | A `POST` that is not JSON |
| 429 | `rate_limited` | Over a limit (§6) |
| 503 | `unavailable` | Storage full (ARCH-77) or the daemon shutting down; try again |

A channel or message the agent cannot read is `404`, the same as one that does
not exist, so the API does not confirm that an id is in use. The HTTP
listener's own refusals before a request reaches the API — a head too large, a
request not whole in time (ARCH-32) — keep their plain-text pages.

## 6. Rate limits

Decided: limits are **per token**, with reads, searches and posts counted
separately, and every response reports where the caller stands (REQ-354).

Proposed defaults, fixed windows from the shared `ratelimit` module as the
webhook's (ARCH-71):

| Bucket | Limit |
|---|---|
| Reads (`GET` other than search) | 600 a minute |
| Search | 60 a minute |
| Posts | 60 a minute — the webhook's |

Headers on every response, for the bucket the request counted against:

```
RateLimit-Limit: 60
RateLimit-Remaining: 12
RateLimit-Reset: 23
```

(`Reset` in seconds.) Over the limit is `429` with `Retry-After` in seconds.
A limit is never a dropped request.

**Open:** whether an operator may raise the limits per agent, or only for the
workspace as a whole, or not at all in `v1`.

## 7. Auditing

Decided: every request is one entry in the audit log (REQ-251, REQ-353), in a
new family shared with the MCP surface: when, which agent, the act (read
history, read a message, search, post), the channel or message, and the outcome
— refusals included. **Never** the token, a message's text, or the search
query, which can quote messages.

**Open:** retention. REQ-251b keeps every family 365 days. An integration that
reads once a minute writes half a million entries a year, and one polling
faster a great deal more. Either the new family keeps a shorter age, which
amends REQ-251b, or consecutive reads of one channel by one agent are recorded
as one entry with a count. The proposal is the second: it keeps the
one-retention rule and loses nothing anyone audits for.

## 8. Connections

The HTTP listener answers **one request per connection** (ARCH-32), so a client
paging through history pays a TLS handshake for each page. `v1` lives with
that: the pages are large and integrations are not chatty.

**Open:** keep-alive for `/api/` routes, if measurement shows the handshakes
matter. It changes the listener, not the API, so it can come later without a
version.

## 9. The SDK

**Open for review.** Whether an SDK comes first, and in which language.

The options:

1. **No SDK at first.** The reference, and examples with `curl`, are the API's
   documentation; an SDK follows once the endpoints have settled.
2. **Python first**, alongside the read endpoints.
3. **TypeScript first.**
4. **Several at once**, generated from an OpenAPI description.

**Recommendation: the API first, then a Python SDK as soon as posting ships —
option 1 followed by option 2.** The API is the contract (REQ-350); an SDK
written before it settles would be rewritten with it. Then Python, because it is
what the scripts, CI jobs and back-office glue this API is for are most often
written in, and it needs nothing but the standard library to make an HTTPS
request. The SDK stays thin: the token, the error type, iterators that follow
`next`, an `Idempotency-Key` on every post, and waiting out a `429` by
`Retry-After`. No caching, no model of the workspace, nothing the API does not
do. It lives in this repository, so a change to the API and to the SDK is one
change, with a test that runs the SDK against a test daemon; how it is
published (a package index, and the release step that pushes to it) is decided
when it is built.

A generated multi-language SDK (option 4) is rejected for now: the generated
code would be several codebases nobody here reads, for callers nobody has asked
for.

## 10. To build

Once this is reviewed, the work is opened as four issues. Each depends on the
agent principal and its tokens, which come first.

1. **The read endpoints** — `/api/v1/` under the HTTP listener; bearer
   authentication by agent token; `GET /channels`, `GET /channels/{id}/messages`,
   `GET /messages/{id}`, `GET /messages/{id}/replies`; the JSON shapes and
   errors (§5); the per-token limits and their headers (§6); the audit family
   (§7); and the reference document.
2. **Search** — `GET /search` over the shared operator parser, with the
   relevance cursor (§4.3).
3. **Posting** — `POST /channels/{id}/messages`, replies by `parent_id`, and
   `Idempotency-Key` (§4.4).
4. **The SDK** — as the review of §9 decides.
