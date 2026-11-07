# Bolt — can apps written with it run on OpenChime?

A research note. Most chat-platform apps are written with **Bolt**, Slack's app
framework, rather than against the raw Web API, so how closely OpenChime's app
platform (REQ-172, REQ-173, REQ-176) has to follow Slack's shapes depends on
what Bolt assumes. This note records what Bolt assumes, traces a sample Bolt
app call by call, maps each call to OpenChime's planned surface, and ends in a
recommendation, now decided (§6). Nothing here is built, and nothing here
changes another document; changes follow from the decision.

Where this note states a fact about Bolt, it names the release and the file it
was read from. Releases read, all current on 6 November 2026:

| Package | Release |
|---|---|
| Bolt for JavaScript (`@slack/bolt`) | 5.1.0 |
| its Web API client (`@slack/web-api`) | 8.3.0 |
| its OAuth installer (`@slack/oauth`) | 4.0.0 |
| its Socket Mode client (`@slack/socket-mode`) | 3.1.1 |
| Bolt for Python (`slack_bolt`), with `slack_sdk` | 1.30.0, with 3.46.0 |
| Bolt for Java (`com.slack.api:bolt`, `slack-api-client`, `slack-app-backend`) | 1.54.0 |

Paths in the JavaScript packages are under `dist/` of the published package.

## 1. What Bolt assumes (JavaScript)

### 1.1 Where the API is, and how it is called

- **The base URL is an option, not a constant.** The Web API client takes
  `slackApiUrl`, defaulting to `https://slack.com/api/`, and builds every call
  as that prefix plus the method name (`web-api` `WebClient.js`, constructor and
  `deriveRequestUrl`). Bolt passes `clientOptions` through to it (`bolt`
  `App.js`, constructor). A Bolt app can therefore be pointed at another host
  with one option and no code change.
- **Calls are `POST <base>/<method>`**, body `application/x-www-form-urlencoded`,
  with nested objects (a modal's `view`, a message's `blocks`) as JSON strings
  in a form field, and the token in `Authorization: Bearer` (`WebClient.js`,
  `serializeBody`; confirmed by the trace, §2). The client adds `team_id`
  to every call when it knows the team.
- **Responses are JSON with `ok`.** The client reads `response_metadata.warnings`
  and `.messages` (logged), `response_metadata.next_cursor` (pagination, sent
  back as `cursor`), and the optional `x-oauth-scopes` and
  `x-accepted-oauth-scopes` headers (`WebClient.js`, `buildResult` and
  `paginate`).
- **Rate limits are HTTP 429 with `Retry-After`.** By default the client waits
  and retries, up to ten times in about thirty minutes (`WebClient.js`,
  `retryConfig` default `tenRetriesInAboutThirtyMinutes`).
- **Token prefixes are not checked.** `xoxb-`, `xoxp-` and `xapp-` appear only in
  comments; no code inspects a token's shape (`WebClient.js`, `App.js`,
  `oauth`, `socket-mode`).

### 1.2 Methods Bolt calls without the app asking

- **`auth.test`, at start.** With a single token, `App` calls `auth.test` at
  start-up unless `tokenVerificationEnabled` is false, and keeps `user_id` as
  `botUserId` and `bot_id` as `botId` (`App.js`, constructor and
  `singleAuthorization`). With an installation store it calls `auth.test` for an
  installation that has no stored ids (`App.js`, `runAuthTestForBotToken`).
  Those two ids drive `ignoreSelf` (on by default) and `directMention`
  (`middleware/builtin.js`). **An OpenChime that does not answer `auth.test`
  breaks every Bolt app at start.**
- **`apps.connections.open`**, with the app-level token, in Socket Mode only
  (`socket-mode` `SocketModeClient.js`, `retrieveWSSURL`).
- **`oauth.v2.access`**, in the OAuth install flow and for token rotation
  (`oauth` `install-provider.js`, `handleCallback` and `refreshExpiringTokens`).
- `say()` is `chat.postMessage` to the conversation the payload came from, and
  `respond()` is a JSON POST to the payload's `response_url`, with no token
  (`context/create-say.js`, `context/create-respond.js`; trace, §2).

### 1.3 HTTP delivery

- **One endpoint** receives everything, `/slack/events` by default
  (`receivers/HTTPReceiver.js`, `endpoints`).
- **Request verification:**
  - headers `x-slack-request-timestamp` and `x-slack-signature`;
  - the signature is `v0=` followed by the hex HMAC-SHA-256, keyed with the signing secret, over `v0:<timestamp>:<raw body>`;
  - compared in constant time (`tsscmp`);
  - refused when the timestamp is more than five minutes old
  (`receivers/verify-request.js`, `verifySlackRequest`). Only `v0` is accepted.
- **The body's form decides how it is read.** JSON for events. Form-encoded for slash commands, as plain fields: `command`, `text`, `trigger_id`, `response_url`, `channel_id`, `user_id`, `team_id`. Form-encoded with a single `payload` field holding JSON for interactions (`receivers/HTTPModuleFunctions.js`, `parseHTTPRequestBody`).
- **Endpoint check:** a body of `type: "url_verification"` is answered with its
  `challenge` (`HTTPModuleFunctions.js`, `buildUrlVerificationResponse`;
  `HTTPReceiver.js`). A form body `ssl_check=1` is answered with an empty
  200 and needs no signature; it is optional for the sender.
- **Retries:** `x-slack-retry-num` and `x-slack-retry-reason` are read and handed
  to the listener as `retryNum` and `retryReason` (`HTTPModuleFunctions.js`,
  `extractRetryNumFromHTTPRequest`, `extractRetryReasonFromHTTPRequest`).
- **Acknowledgement:** the HTTP response *is* the acknowledgement. Bolt
  answers 200 when the listener calls `ack()` (with a body when `ack()` is given
  one, such as a modal's `response_action`). If no `ack()` comes within
  **3001 ms**, it logs an error (`receivers/HTTPResponseAck.js`,
  `unhandledRequestTimeoutMillis`). Events are acknowledged automatically.
- **How a payload is routed** (`helpers.js`, `getTypeAndConversation`):
  - `event` present: an event;
  - `command`: a slash command;
  - `actions`: a block action;
  - `type` of `shortcut`, `message_action`, `view_submission`, `view_closed` or `block_suggestion`: the matching kind.

### 1.4 The event envelope

- An event arrives as `type: "event_callback"`, with `team_id`, `api_app_id`,
  `event_id`, `event_time`, `authorizations[]` (`team_id`, `user_id`, `is_bot`,
  `is_enterprise_install`, `enterprise_id`) and `event`.
- Bolt takes the team from `authorizations[0].team_id`, falling back to
  `team_id` (`App.js`, `buildSource`).
- The conversation comes from `event.channel` (a string, or an object with `id`),
  `event.channel_id`, or `event.item.channel` (`helpers.js`).
- Payloads other than events carry `team.id` (or `team_id`) and `user.id`.
  Modals carry `view.app_installed_team_id` where present (`App.js`,
  `buildSource`).
- **Ids are opaque strings.** Message identity is `ts` (and `thread_ts`); Bolt
  passes them through and never parses them (trace, §2).
- **Message text uses the link syntax.** `directMention` matches
  `<@USERID>` at the start of the text, against `botUserId`
  (`middleware/builtin.js`, `slackLink`, `directMention`). `app.message(...)`
  matches a string or pattern against `event.text`. `ignoreSelf` drops message
  events whose `user` is the bot user, or whose `subtype` is `bot_message` with
  the bot's own `bot_id`.

### 1.5 Socket Mode

- **Opening a connection:** the app calls `apps.connections.open` with its app-level token, then opens the returned `url` as a WebSocket. It uses `undici`'s client, so `ws://` and `wss://` both work (`socket-mode` `SocketModeClient.js`, `SlackWebSocket.js`).
- **The handshake:** the server sends `type: "hello"`, and the client counts itself connected only then. `type: "disconnect"` makes it reconnect, which it does on its own by default. The client sends pings and expects the server's within 30 s (`serverPingTimeout`).
- **Envelopes and acknowledgement:** each message is an envelope with `envelope_id`, `type` (`events_api`, `slash_commands` or `interactive`), `payload` (the same body HTTP delivery would carry), `accepts_response_payload`, `retry_attempt` and `retry_reason`. The client acknowledges by sending back `{"envelope_id": ..., "payload": {...}}`.

### 1.6 The OAuth install flow

- **The authorize URL** is an option (`authorizationUrl`, default
  `https://slack.com/oauth/v2/authorize`), passed through Bolt's
  `installerOptions.authorizationUrl` (`oauth` `install-provider.js`
  constructor; `bolt` `HTTPReceiver.js`).
- **State** is a signed value set as a cookie (`slack-app-oauth-state`, ten
  minutes) and checked on return. The installer is in `@slack/oauth`, and the
  state store is its own.
- **The code is exchanged with `oauth.v2.access`** through the Web API client, so it goes to the same configurable base URL (`installerOptions.clientOptions`). Bolt stores the following from the answer:
  - `access_token`, `token_type`, `scope`;
  - `bot_user_id`, `app_id`;
  - `team.id`, `enterprise`, `is_enterprise_install`;
  - `authed_user`;
  - optionally `refresh_token` and `expires_in`, for token rotation.
- **After the exchange** the installer calls `auth.test` to fill in the bot's
  ids.

## 2. A sample app, traced

A Bolt for JavaScript 5.1.0 app was written with:
- a message listener (`app.message('hello')`, answering with `say`);
- `app_mention` (answering in the thread);
- `reaction_added`;
- a slash command `/ticket` that opens a modal;
- a button `approve` answered with `respond`;
- the modal's submission, which posts a message.

It was run twice against a stub workspace that logs every request: once over
HTTP delivery, with signed requests sent to it as a workspace would send them,
and once in Socket Mode. Its only configuration beyond the token and secret was
`clientOptions.slackApiUrl` pointing at the stub. The prototype was thrown away.

| # | What happened | Request seen | OpenChime's planned surface |
|---|---|---|---|
| 1 | Start | `POST /api/auth.test`, form, bearer bot token | **Gap.** The Web API (ARCH-123, [HTTP-API.md](./HTTP-API.md)) has no `auth.test`. It is needed, returning `ok`, `team_id`, `user_id` (the bot's user) and `bot_id`. |
| 2 | Socket Mode start | `POST /api/apps.connections.open`, bearer app-level token, then a WebSocket | **Gap.** The planned delivery is direct calls in-workspace and signed POSTs outward, with no socket for apps. There is also no app-level token. |
| 3 | Endpoint check | `{"type":"url_verification","challenge":…}` → `{"challenge":…}` | Planned: the event endpoint is checked when it is registered (REQ-173). Use this exact form. |
| 4 | Message event | `event_callback` with `authorizations[]`; `say` → `POST /api/chat.postMessage` (`channel`, `text`, `team_id`) | Event names and signing follow the convention (REQ-173). **Gaps:** the envelope needs `authorizations[]`, `team_id` and `api_app_id`. Posting needs the method form of the API, not `POST /channels/{id}/messages`. |
| 5 | `app_mention` | text `<@UBOT> status`; reply with `thread_ts` | **Gap.** OpenChime keeps a mention as plain text in the body, with resolved ids beside it (ARCH-89). Event text needs the `<@ID>` link form, and posted text needs to accept it. Threads map to `thread_ts` = the parent's id as a string. |
| 6 | `reaction_added` | `event.item.channel`, `event.item.ts` | Planned. Only the field names need to match. |
| 7 | Slash command | form fields `command`, `text`, `trigger_id`, `response_url`, …; acknowledged in the HTTP response; then `POST /api/views.open` with `view` as a JSON form field | Commands are planned with namespacing (`/<app>/<command>`). **Gaps:** form-encoded delivery, `response_url`, `trigger_id`, `views.open`. A namespaced command reaches Bolt's `app.command('/ticket')` only if `command` carries the name the app registered. |
| 8 | Button | form `payload=` JSON `block_actions`; `respond` → JSON POST to `response_url`, no token | **Gap:** `response_url` endpoints (unauthenticated, short-lived, per interaction). |
| 9 | Modal submission | form `payload=` JSON `view_submission`; acknowledged; `chat.postMessage` | Planned with Block Kit. `ack()` may carry `response_action` (`errors`, `update`, `push`, `clear`). |
| 10 | Stale request | timestamp 400 s old → 401 | Matches the planned five-minute window. |
| 11 | Socket Mode envelopes | `events_api`, `slash_commands`, `interactive`; each acknowledged with `{"envelope_id","payload"}` | **Gap**, as row 2. |

Every listener ran in both modes, against a stub that implemented only the
shapes above.

## 3. Three approaches

### 3.1 Unchanged, with the base URL pointed at the workspace

A Bolt app runs with no code change, only configuration:
- `clientOptions.slackApiUrl` set to `https://<workspace>/api/`;
- for distributed apps, `installerOptions.authorizationUrl` and `installerOptions.clientOptions`.

OpenChime would need:
- the Web API at `/api/<method>`, taking form-encoded and JSON bodies with a bearer token, answering with `ok`, `response_metadata.next_cursor` and 429 with `Retry-After`;
- `auth.test`;
- the methods the app itself uses;
- the event envelope of §1.4;
- commands and interactions delivered form-encoded, as in §1.3;
- `response_url` and `trigger_id`;
- ids as strings, with a message's id as its `ts`;
- mentions as `<@ID>` in event text, and accepted in posted text;
- retry headers;
- the OAuth flow of §1.6;
- for Socket Mode, `apps.connections.open`, an app-level token and the envelope protocol of §1.5.

**Cost:** close to what the decided design already intends (methods named and shaped by the convention, its event names and signing). The additions are:
- `auth.test`;
- the form-encoded and `payload=` delivery;
- `response_url`;
- the mention translation;
- for Socket Mode, one more delivery path.

Namespaced commands work, provided the app registers the name it listens for.

### 3.2 A small adapter package

An `openchime-bolt` package per language (a receiver, an authorize function
and client defaults) would absorb some differences: a JSON-only body, another
envelope, other id names. **Cost:** three packages to maintain and to keep in
step with three Bolt release trains. Every Bolt example and tutorial needs a
"but on OpenChime…" note. It cannot absorb the missing methods
(`auth.test`, `views.open`) or `response_url`, which the server must provide
anyway. It saves little over 3.1 and costs more.

### 3.3 Not feasible

Only if the API keeps its REST shape (`/api/v1/...`, [HTTP-API.md](./HTTP-API.md))
or delivery departs from the convention. Then Bolt apps would need rewriting,
and following the convention would bring no benefit beyond familiarity.

## 4. In-workspace apps

Bolt itself cannot run on the in-workspace runtime (QuickJS): it depends on
Node's `http`, `crypto` and `querystring` modules, on `undici` and on Express
(`bolt` `package.json` dependencies; `receivers/`).

A **Bolt-like listener API** is cheap to provide instead, as a small JavaScript
layer over the runtime's direct calls:
- `app.message`, `app.event`, `app.command`, `app.action`, `app.view`, `app.shortcut`;
- `ack`, `say`, `respond`, `client.<method>`.

There is no receiver, no signature check and no `auth.test` round trip,
because the daemon calls the handler directly with the same payloads. A
listener's body moves between an external Bolt app and an in-workspace app
almost unchanged. **Cost:** one small layer, generated against the same
interface description as the runtime's bindings, and its documentation.

## 5. Bolt for Python and Bolt for Java

**Python** (`slack_bolt` 1.30.0, `slack_sdk` 3.46.0) assumes the same things as
JavaScript on every point in §1:
- the base URL is `WebClient(base_url=...)`, default `https://slack.com/api/` (`slack_sdk/web/base_client.py`, `BASE_URL`);
- `auth.test` at start unless `token_verification_enabled=False` (`slack_bolt/app/app.py`);
- `url_verification` and `ssl_check` middleware on by default (`app.py`);
- the five-minute signature window (`slack_sdk/signature/__init__.py`);
- an `authorization_url` option, default `https://slack.com/oauth/v2/authorize` (`slack_sdk/oauth/authorize_url_generator/__init__.py`);
- Socket Mode through `apps.connections.open`, with a choice of WebSocket libraries (`slack_sdk/socket_mode/`).

One difference matters: a Python app commonly constructs its own `WebClient`, so
pointing it elsewhere is a constructor argument rather than an `App` option.

**Java** (1.54.0) assumes the same envelope and signing:
- the five-minute window (`slack-app-backend` `SlackSignature.java`, `TIMESTAMP_EXPIRATION_TIME_IN_MILLIS`);
- `v0` signing and the `X-Slack-Signature` header (same file);
- `url_verification` and `ssl_check` handling (`bolt` `App.java`, `middleware/builtin/SSLCheck.java`);
- Socket Mode (`bolt-socket-mode`).

Its base URL is `SlackConfig.methodsEndpointUrlPrefix` (`slack-api-client` `SlackConfig.java`), default `MethodsClient.ENDPOINT_URL_PREFIX`, `https://slack.com/api/` (`MethodsClient.java`). It is configurable as in the other two.

So 3.1 serves all three languages with the same server work; 3.2 would need
three packages.

## 6. Recommendation and decision

**Take 3.1: Bolt apps run unchanged, pointed at the workspace by configuration.**
Provide the Bolt-like listener API of §4 for in-workspace apps. The server
work this implies, in addition to what is already decided:

1. **Web API.**
   - Methods served at `/api/<method>`, taking form-encoded and JSON bodies, with the bearer token.
   - `ok`, `response_metadata` (`next_cursor`, `warnings`), and 429 with `Retry-After`. The REST shape of [HTTP-API.md](./HTTP-API.md) is replaced, as already intended.
   - `auth.test` in the first release, alongside the methods a typical app uses: `chat.postMessage`, `chat.update`, `chat.postEphemeral`, `views.open`, `views.update`, `views.push`.
2. **The event envelope.**
   - `event_callback` with `team_id`, `api_app_id`, `event_id`, `event_time` and `authorizations[]`.
   - `url_verification` when an endpoint is registered.
   - `x-slack-retry-num` and `x-slack-retry-reason` on redelivery.
   - Ids as strings throughout, with a message's id given as `ts` and its parent's as `thread_ts`.
   - Message text with mentions in `<@ID>` form. Posted text accepts the same form, alongside OpenChime's own.
3. **Commands and interactions.**
   - Delivered form-encoded: plain fields for commands, a `payload` field for interactions.
   - `trigger_id` and a per-interaction `response_url`.
   - The acknowledgement and its optional body (including `response_action`) taken from the HTTP response within three seconds.
   - Namespaced commands carry, in `command`, the name the app registered.
4. **OAuth install.**
   - The authorize page.
   - `oauth.v2.access`, with the answer fields of §1.6, including token rotation.
   - It works with Bolt's own state cookie, which needs nothing from the server.
5. **Socket Mode.**
   - `apps.connections.open`, an app-level token per app, and the envelope protocol of §1.5 on the shared listener.
   - This reverses the earlier choice of no app socket. It is recommended because many Bolt apps and most tutorials use it. It is also the way a developer runs an app with no public endpoint, and it costs one delivery path over a WebSocket the daemon already serves.

Every item reuses the decided signing scheme and event names.

**Decided (7 November 2026):**
- 3.1 is taken: Bolt apps run unchanged, pointed at the workspace by
  configuration, and in-workspace apps get the Bolt-like listener API of §4.
- Socket Mode is in the first release, beside HTTP delivery.
- `<@ID>` mentions are accepted in all posted text, from people's clients as
  well as from apps.
