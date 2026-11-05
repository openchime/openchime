# MCP — tools, paging, agent tokens and audit

The specification the MCP surface is built to: the tools an agent or a
person's AI client calls, how they page through long channels, the life of an
agent's token, and the audit row each tool writes. The surface itself — the
endpoint, the protocol revision, the two principals, what each may read and
the limits — is [MCP.md](../MCP.md); the decision is ARCH-120, and the
requirements REQ-320–325.

Where a number is given it is a proposal for review, marked so; everything
else is decided.

## 1. Tools

Three tools: **`read_history`**, **`search`** and **`post`**. They are the same
for both principals; what differs is which conversations they reach (MCP.md
§4).

`tools/list` returns them in that order, every time. Each tool's `channel`
parameter is an `enum` of the conversations the caller can read, by name —
`#general` for a channel, `@alice` for a DM, `@alice,@bob` for a group DM — so a
client learns where it can read and post from the tool list itself and needs no
fourth tool to find out. Because that list is the caller's own, the result is
`cacheScope: "private"`, with `ttlMs` proposed at 60 000, so a channel an agent
was just added to appears within a minute. A conversation is always also
accepted by its numeric id, which does not change when a channel is renamed.

A call to a conversation the caller cannot read is a tool error ("no such
conversation"), the same whether it exists or not.

### 1.1 `read_history`

Reads a conversation's messages, or one thread's.

| Argument | | |
|---|---|---|
| `channel` | required | name or id |
| `thread` | optional | a message id: read that thread's replies instead of the conversation |
| `before` | optional | a cursor from an earlier page, or a message id: messages older than it |
| `after` | optional | a cursor or message id: messages newer than it |
| `since` | optional | a time (RFC 3339): messages at or after it |
| `limit` | optional | messages to return, 1–200, default 50 (proposed) |

With none of `before`, `after` or `since`, it returns the newest messages. At
most one of `before` and `after` may be given.

Each message is returned as:

| Field | |
|---|---|
| `id` | the message id |
| `author` | display name and handle |
| `author_kind` | `person`, `agent` or `webhook` |
| `via` | for a post through a person's connection, the client's name |
| `at` | time, RFC 3339 in UTC |
| `text` | the body, as written (Markdown, MARKDOWN.md) |
| `edited` | true if it has been edited |
| `replies` | for a message that starts a thread, how many replies it has |
| `files` | for each file attached: name, type and size, not content |
| `truncated` | present when `text` was cut (§2) |

Messages within a page are oldest first, so a page reads as the conversation
does. The result's `structuredContent` holds the messages and the cursors; its
`content` holds the same as text, one message to a paragraph, for a client
that passes text straight to a model.

### 1.2 `search`

Searches the conversations the caller can read, with the same full-text
search and query syntax the clients use (ARCH-15).

| Argument | | |
|---|---|---|
| `query` | required | the search |
| `channel` | optional | name or id: search only there |
| `from` | optional | a handle: only messages by that author |
| `since`, `until` | optional | times (RFC 3339) bounding the search |
| `cursor` | optional | from an earlier page |
| `limit` | optional | results to return, 1–50, default 20 (proposed) |

Results are best match first, each a message as in §1.1 with its conversation
added and `text` replaced by an excerpt around the match (`excerpt`, proposed
at most 300 characters). `read_history` with `before` and `after` around a
result's id reads its context.

### 1.3 `post`

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

## 2. Paging and truncation

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
- **A page has a size limit as well as a count.** Proposed: 64 KiB of text in
  all. When the next message would pass it, the page ends there, short of
  `limit`, and the cursor continues from it; a page always holds at least one
  message.
- **A long message is cut, and says so.** Proposed: a message's `text` is cut
  at 8 000 characters, at the last whole line before it, and the message
  carries `truncated: {"length": <full length in characters>}`. A page of one
  message is cut only at the page limit, so `read_history` with `before` one
  more than its id and `limit` 1 returns it whole, for a client that needs all
  of it.
- **A thread is read on its own.** A conversation's history lists a thread's
  first message with its reply count, not the replies, so a busy thread does
  not crowd out the channel; `thread` reads the replies, paged the same way.

## 3. Agent tokens

### 3.1 Minting

An owner or admin mints a token for an agent from the agent's page in member
administration, naming it ("CI runner", "laptop"). The daemon makes 32 random
bytes and shows the token once, as a fixed prefix and the bytes in base64url,
so a secret scanner can recognise a leaked one. It stores only:

- the token's SHA-256, under a unique index, so a request's lookup is one
  probe;
- the agent it belongs to, its name, who minted it and when;
- when and from which address it was last used.

An agent may hold several tokens (proposed: at most 10), so a token is replaced
by minting the new one, moving the agent to it, then revoking the old. Minting
is recorded in the administrative audit family, with the agent and the token's
name, never the token.

Tokens do not expire on their own: an agent is a service, and a token that
lapsed would stop it without anyone choosing to. Revoking is the way a token
ends.

### 3.2 Listing

The agent's page lists its tokens: name, who minted it, when, and when and
from where it was last used. The last use is written at most once a minute per
token, so a busy agent does not turn every read into a write. The token itself
is never shown again.

### 3.3 Revoking

An owner or admin revokes a token from that list. Revoking deletes the row in
the writer, so the **next request with the token is `401`**: every request
looks its token up afresh, and nothing about a token is cached. A request
already being answered when the row goes finishes. Revoking is recorded in the
administrative family.

Removing an agent deletes all its tokens and its channel memberships in one
transaction; its messages stay, under its name. A token whose agent is gone is
therefore refused the same way.

## 4. Audit

Every tool call writes one row to the **MCP access** family of the audit log
(REQ-251, REQ-325), in the same transaction as a post and from the writer for a
read, as every other entry is written. The family ages out on its own, after
365 days (REQ-251a/b), so heavy reading cannot push older entries of other
families out.

| Field | Written as |
|---|---|
| `action` | `mcp.read_history`, `mcp.search` or `mcp.post` |
| actor | the agent, or the person whose connection it was |
| `target` | the conversation; for a search over every conversation, none |
| `outcome` | 1 served, 0 refused (no such conversation, not allowed to post, over a limit) |
| `detail` | the principal — the token's name, or the connection's client — then, by action, below |

| Action | `detail` adds |
|---|---|
| `mcp.read_history` | the thread if one was read, the ids of the first and last message returned, and how many |
| `mcp.search` | how many results, and in how many conversations; **not the query**, which is the caller's words about the content and is kept out of the log as message content is |
| `mcp.post` | the message id, and the thread if it was a reply |

No row holds a message's text, an excerpt, a query or a token. `tools/list`
and `server/discover` read nothing of anyone's and write no row. A request
refused for its credential is not in this family: it is a failed
authentication, in the security family (REQ-251).

An owner or admin reads the family in the audit view beside the others, and
can narrow it to one agent or one person, which answers "what has this agent
read, and when".
