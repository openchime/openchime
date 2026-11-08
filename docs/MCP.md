# MCP

How an AI agent, or a person's own AI client, reads and posts in a workspace
through the Model Context Protocol (REQ-320–325, ARCH-120). This document is the
daemon-side design; the decision and what was rejected are ARCH-120, and the
tools, their paging and the agent token lifecycle are specified in
[Plans/MCP.md](./Plans/MCP.md).

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
route's are (ARCH-32): a malformed head, a missing header, a body over the
limit. One that is well formed goes to the event loop, which checks the
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

An agent authenticates with a **bearer token the daemon minted**. An owner or
admin creates the agent and mints its tokens from the client; a token is 32
random bytes, shown once, stored as its SHA-256 and found by one indexed probe,
exactly as session and webhook tokens are (ARCH-58/71). An agent may hold
several, so one can be replaced without a gap. Revoking a token deletes its
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
  (ARCH-114). Being in a channel gives it what being in a channel gives a
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
- **Reads and searches** have a budget per principal, refilled continuously.
  Proposed for review: 60 tool calls a minute with a burst of 20, and searches
  costing three times a history read, since a search scans the index.
- **Failed authentication** is limited per source (REQ-191), so guessing tokens
  is as slow as guessing passwords.

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
