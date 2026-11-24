# Outgoing webhooks

An owner or admin points the workspace at an HTTPS address, names the events it
is sent and the channels they come from, and the daemon POSTs each event there,
signed. It is the simple form of the event delivery that installed apps will use
([plans/APPS.md](../plans/APPS.md) §5): the same catalog, the same envelope, the
same signature, with no install, no bot and no commands. It delivers REQ-173
under ARCH-131.

## 1. Managing webhooks

Owners and admins alone create, list, turn on and off, and delete webhooks, in
**Admin > Webhooks** or over the wire (PROTOCOL.md §5.15a). Each change is an
entry in the admin family of the audit log: `webhook.outgoing.create`,
`.enable`, `.disable` and `.delete`.

A webhook is:

- **an address**, any `https://` URL with a host. There is no address check
  beyond the scheme: where the events go is the owner's or admin's choice.
  Plain `http://`, or no host, is refused with 3085.
- **its events**, one or more of the catalog in §2. Any other name is refused
  with 3086.
- **its channels**, up to 32 named channels, public or private. A DM or group DM
  is refused with 1003 (`FORBIDDEN`); a channel that is none with the unknown
  channel code. A webhook naming no channel is sent `user_change` alone.

**The endpoint check.** Before a webhook is saved, the daemon sends its address
`{"type":"url_verification","challenge":"<random>"}`, signed as any event is
(§4). The address must answer 2xx with the challenge, as the body or as the
`challenge` of a JSON object, within three seconds; otherwise nothing is saved
and the creation is refused with 3087. The check runs on the delivery worker
(§5), never on the net loop.

**The signing secret** is 20 random bytes, shown and used as its 40 lowercase
hex digits. It is returned once, in `OUTGOING_WEBHOOK_SECRET` to the connection
that made the webhook, and never listed. The daemon keeps it **sealed under the
factor key** (AES-256-GCM, `oc_factor_seal` in `daemon/totp.h`, the webhook's
id its associated data, so a sealed secret moved to another row does not open):
a secret that keys an HMAC has to be opened to sign, so it cannot be kept as a
hash. Only the delivery worker opens it. A daemon with no factor key starts no
worker, and refuses creations.

## 2. What a webhook receives

| Event | When |
|---|---|
| `message` | A message is posted in one of its channels, or a reply. Subtypes: `message_changed` (edited), `message_deleted`, `bot_message` (an agent's or a bot's), `me_message` (an action, `/me`). |
| `reaction_added`, `reaction_removed` | A reaction on a message in one of its channels. |
| `member_joined_channel`, `member_left_channel` | Someone joins or leaves one of its channels. |
| `channel_rename`, `channel_archive`, `channel_unarchive` | One of its channels changes. |
| `user_change` | A person's profile changes, whatever the webhook's channels. |

Only named channels' events are sent, never a DM's or a group DM's. A message
that is a call's event, and the workspace's own notices, are not messages here.
A reaction already there, or already gone, is no event.

## 3. The envelope

The writer builds each event and queues it for each webhook that receives it,
**in the same transaction as the event**, so an event that is written is sent
and one rolled back is not. The body is the convention's envelope:

```json
{
  "type": "event_callback",
  "team_id": "T5f3a9c",
  "api_app_id": "W3",
  "event_id": "Ev8d2e61a07b4c9f13e2a0",
  "event_time": 1730295731,
  "authorizations": [],
  "event": {"type": "message", "channel": "C7", "channel_type": "channel",
            "user": "U42", "text": "ship it, <@U57>", "ts": "90211", "event_ts": "90211"}
}
```

- `team_id` is `T` and the workspace's audience id without its `ws_`; `T0` when
  the workspace is not enrolled.
- `api_app_id` is `W` and the webhook's id: a webhook is no app, and the letter
  says so.
- `event_id` is `Ev` and 20 random hex digits, unique per delivery and the same
  on every retry, so a receiver can drop a duplicate.
- `event_time` is Unix seconds when the event was written.
- `authorizations` is empty: no bot is installed.
- Ids are `U42` for a person, agent or bot, `C7` for a channel, and a message's
  `ts` is its id; a reply carries `thread_ts`, its thread's first message.
- `channel_type` is `channel` for a public channel and `group` for a private
  one (`C` and `G` in the membership events).
- Text gives every mention of a person as `<@U42>`, whichever form its author
  typed; `@here`, `@channel` and `@everyone` become `<!here>`, `<!channel>` and
  `<!everyone>`. A name that names no member of the channel is left as written.

The other events, in the order of §2:

```json
{"type":"message","subtype":"message_changed","hidden":true,"channel":"C7","channel_type":"channel",
 "message":{"type":"message","user":"U42","text":"ship it now","ts":"90211",
            "edited":{"user":"U42","ts":"90211"}},"ts":"90211","event_ts":"90211"}
{"type":"message","subtype":"message_deleted","hidden":true,"channel":"C7","channel_type":"channel",
 "deleted_ts":"90211","ts":"90211","event_ts":"90211","user":"U42"}
{"type":"reaction_added","user":"U57","reaction":"thumbsup","item_user":"U42",
 "item":{"type":"message","channel":"C7","ts":"90211"},"event_ts":"90211"}
{"type":"member_joined_channel","user":"U57","channel":"C7","channel_type":"C"}
{"type":"channel_rename","channel":{"id":"C7","name":"launch","created":1730000000}}
{"type":"channel_archive","channel":"C7","user":"U42"}
{"type":"user_change","user":{"id":"U57","name":"ada","real_name":"Ada Lovelace","tz":"Europe/London",
 "is_bot":false,"profile":{"display_name":"ada","real_name":"Ada Lovelace","title":"Analyst",
 "pronouns":"she/her"}},"event_ts":"1730295731000"}
```

## 4. Signing

Every request is a POST of the envelope as `application/json`, with:

- `X-Slack-Request-Timestamp`: Unix seconds when it was sent;
- `X-Slack-Signature`: `v0=` and the hex HMAC-SHA-256, keyed by the secret's
  40 hex digits, of `v0:{timestamp}:{raw body}`.

A receiver computes the same and compares in constant time, and rejects a
timestamp more than five minutes from its own clock. These are the names and
the scheme Bolt's request verification reads ([plans/BOLT.md](../plans/BOLT.md)
§1.3), so a Bolt app verifies a webhook's requests unchanged.

**Verifying one by hand.** With the secret
`8f742231b10e8888abcd99yyyzzz85a5`, a request timestamped `1531420618` whose
body is the convention's published example
(`token=xyzz0WbapA4vBCDEFasx0q6G&team_id=T1DC2JH3J&...`, quoted whole in
`tests/test_webhooks_out.c`) is signed
`v0=a2114d57b48eac39b9ad189dd8316235a7b4a8d21a10bd27519666489c69b503`. In a
shell, for a body saved in `body.json`:

```sh
printf 'v0:%s:%s' "$TS" "$(cat body.json)" |
  openssl dgst -sha256 -hmac "$SECRET" | sed 's/^.*= /v0=/'
```

## 5. Delivery

The queue is the `webhook_deliveries` table (SCHEMA.md §3ce), so a restart
loses nothing. The net loop asks the writer for the deliveries due once a second
while the worker has room (`OUTWH_TAKE`, at most 16 at once), each then held
for a minute so the next take does not hand it out again; one never answered,
the daemon having stopped, is taken again when its hold ends. It hands them to
the delivery worker (`daemon/webhooks_out.c`), a thread of its own, which opens
the secret, signs and POSTs, and gives back how it went; the net loop writes
that back (`OUTWH_DONE`). Neither the delivery nor the check ever runs on the
net loop.

- **Done** when the address answers 2xx within three seconds.
- **Retried** otherwise: after 1 s, then 2, 4, ... up to eight attempts in all,
  each retry carrying `X-Slack-Retry-Num` (the attempts before it) and
  `X-Slack-Retry-Reason`: `http_timeout` when no answer came in time,
  `http_error` for an answer other than 2xx, `connection_failed` when no
  connection was made. The eighth failure drops the delivery.
- **Turned off after a day of failing.** A webhook's first failure since its
  last success is noted (`failing_since_ms`); a failure a day or more after it
  turns the webhook off, with the reason kept and shown in Admin, tells every
  owner and admin in their workspace DM, and is audited as
  `webhook.outgoing.disable` with no actor. Turned on again, it starts afresh,
  and the deliveries still queued for it go out.
- A webhook turned off is sent nothing, and nothing is queued for it; one
  deleted takes its channels and its queued deliveries with it.

## 6. Where it lives

| Part | Where |
|---|---|
| Frames | `shared/protocol.h` (`OC_MSG_*_OUTGOING_WEBHOOK*`, 0x0154–0x0159), PROTOCOL.md §5.15a |
| Tables | migration 0092, SCHEMA.md §3ce |
| Management, the envelope, the queue | `daemon/dbwriter.c`, the `owh_*` functions |
| The delivery worker and the check | `daemon/webhooks_out.c` |
| Glue | `daemon/netloop.c` (`owh_tick`), `daemon/main.c` |
| Admin > Webhooks | `client/gui/app/app.c`; the client core's `oc_client_*_outgoing_webhook*` |
| Tests | `tests/test_dbwriter.c` (`test_outgoing_webhooks`), `tests/test_webhooks_out.c`, `tests/test_protocol.c`, `tests/test_migrate.c`, `tools/gui/features/webhooks_out.sh` |
