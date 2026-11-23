# OpenChime — The Documentation

OpenChime is a business chat system: one C daemon per workspace, a shared C
client core under a terminal client and one portable graphical client
(Windows, and the browser), and a small set of optional services the
project operates. This document is the map of its documentation: what each
document holds, how they relate, and where to look first.

## How the documents relate

**Two documents are the references.**

- **[REQUIREMENTS.md](./REQUIREMENTS.md)** says *what* the finished system
  does, as numbered requirements (`REQ-N`). It describes the end state, built
  or not, and records the product decisions behind it. It carries no build
  status: which requirements are built is the issue tracker's to say.
- **[ARCHITECTURE.md](./ARCHITECTURE.md)** says *how*, as numbered decisions
  (`ARCH-N`): each the decision, the constraints it holds the system to, and
  what was rejected.

Every other document **fills those two in**: the byte-level protocol, the
schema, a feature's full design, a library's internals, how to build, test
and operate. A detail document never overrides them; where one disagrees, it
is the one to correct. Ids are stable: a requirement or decision is never
renumbered, and a withdrawn one keeps its id so references still resolve.

**[details/](./details/)** holds those documents. **[plans/](./plans/)** holds
proposals for work not yet built, and research notes behind decisions; once a
plan is built, what it decided is in the references and the detail documents.

The repository's [README](../README.md) is the short introduction; this is
the map behind it.

## Where to start

- **What OpenChime is and does:** REQUIREMENTS.md, §1–§16.
- **How it is built:** ARCHITECTURE.md, then
  [CLIENT.md](./details/CLIENT.md) and [PROTOCOL.md](./details/PROTOCOL.md).
- **Running a daemon:** [INSTALLING.md](./details/INSTALLING.md),
  [CONFIG.md](./details/CONFIG.md), [UPGRADING.md](./details/UPGRADING.md),
  [TLS.md](./details/TLS.md) and [SSO.md](./details/SSO.md).
- **Changing the code:** [CONTRIBUTING.md](./details/CONTRIBUTING.md),
  [DEVELOPMENT.md](./details/DEVELOPMENT.md) and
  [TESTING.md](./details/TESTING.md).

## The references

| Document | Holds |
|---|---|
| [REQUIREMENTS.md](./REQUIREMENTS.md) | The finished system's requirements, `REQ-N`: tenancy and identity, messaging, delivery, presence, notifications, attachments and media, integrations, security, platforms, resources, rich text, retrieval, profiles, compliance, the client shell, and what is deliberately excluded. |
| [ARCHITECTURE.md](./ARCHITECTURE.md) | The architecture decisions, `ARCH-N`: the two deployment models, the daemon, the wire protocol, the clients, discovery, storage, HTTP and webhooks, apps, authentication, packaging, and the build. |

## details/ — the design filled in

**The system as a whole**

| Document | Holds | Fills in |
|---|---|---|
| [PROTOCOL.md](./details/PROTOCOL.md) | The binary wire protocol, byte by byte: framing, the handshake, every message type and error code. | ARCH-6–10, 30, 41–47, 54; REQ-090–111 |
| [SCHEMA.md](./details/SCHEMA.md) | The SQLite schema, migration by migration, and how migrations run. | ARCH-2, 27, 49, 50 |
| [CONFIG.md](./details/CONFIG.md) | Every setting the daemon reads, its command line and build switches. Its settings table is generated from the daemon's registry. | ARCH-26, 76 |
| [CLIENT.md](./details/CLIENT.md) | The client: the shared core, the terminal client, the portable graphical client and the rules its surfaces keep. | ARCH-11, 61–65, 74–83, 88, 92–109; REQ-012–015, 260–269, 290 |
| [WEB.md](./details/WEB.md) | The graphical client compiled for the browser and served by the daemon. | ARCH-74, 80; REQ-200 |
| [TLS.md](./details/TLS.md) | TLS: the library, how clients trust a daemon, where its certificate comes from, load balancers. | ARCH-10, 34, 51; REQ-180, 183 |
| [VENDORS.md](./details/VENDORS.md) | Every third-party component, how it is pinned, and its licence. | ARCH-20, 51, 75, 87 |

**Features**

| Document | Holds | Fills in |
|---|---|---|
| [AUTH.md](./details/AUTH.md) | Signing in: local accounts, the relay, direct connections, sessions, second steps, the demo workspace, and the MCP endpoint's authorization server. | ARCH-19, 55–60, 84; REQ-020–029, 181–184, 191 |
| [SSO.md](./details/SSO.md) | Registering the daemon with an organization's own sign-in provider. | ARCH-55; REQ-027 |
| [CALLS.md](./details/CALLS.md) | Calls: the model, Calls home, signaling, end-to-end encryption, screen sharing, cameras, recording. | ARCH-73, 86, 87, 113, 136; REQ-150–152, 160–161, 301–306 |
| [AUDIO.md](./details/AUDIO.md) | A call's audio: the relay, the client's audio engine, transport and echo cancellation. | ARCH-18, 31, 73; REQ-151–152 |
| [VIDEO-MESSAGES.md](./details/VIDEO-MESSAGES.md) | Recording, posting and playing a video message, and its export. | ARCH-110, 135; REQ-162–166, 382–384 |
| [IMAGE-PREVIEWS.md](./details/IMAGE-PREVIEWS.md) | Image previews: the two rungs, when an image gets none, each format, the four decode bounds, fuzzing, storage. | ARCH-137; REQ-142, 390–393 |
| [READ-ALOUD.md](./details/READ-ALOUD.md) | Reading a conversation aloud, rendered in the daemon. | ARCH-111; REQ-291–295 |
| [VOICE-INPUT.md](./details/VOICE-INPUT.md) | Speaking instead of typing, recognized in the daemon. | ARCH-112; REQ-296–300 |
| [SUMMARIES.md](./details/SUMMARIES.md) | Summaries of a conversation: the standard, the pipeline, the model. | ARCH-116; REQ-310–313 |
| [PRIVATE-DM.md](./details/PRIVATE-DM.md) | End-to-end encrypted DMs. | ARCH-118; REQ-314 |
| [MARKDOWN.md](./details/MARKDOWN.md) | The message formatting dialect and where it is parsed. | ARCH-100; REQ-220 |
| [MCP.md](./details/MCP.md) | The Model Context Protocol surface for agents and people's AI clients: the endpoint, its tools, paging, agent tokens and audit. | ARCH-120; REQ-320–325 |
| [EMAIL.md](./details/EMAIL.md) | Notification email and invitations: the three levels, providers, the outbox. | ARCH-125, 126; REQ-280, 365–370 |
| [EXPORT.md](./details/EXPORT.md) | The export format, version 1. | ARCH-124; REQ-360 |

**Libraries in the tree**

| Document | Holds | Fills in |
|---|---|---|
| [TTSKIT.md](./details/TTSKIT.md) | Pronunciation and the voice model behind read-aloud. | ARCH-111 |
| [STTKIT.md](./details/STTKIT.md) | The recognizer behind voice input. | ARCH-112 |
| [SUMKIT.md](./details/SUMKIT.md) | The inference engine behind summaries. | ARCH-116 |
| [TUIKIT.md](./details/TUIKIT.md) | The terminal client's widget toolbox. | ARCH-83 |

**Operating and contributing**

| Document | Holds |
|---|---|
| [INSTALLING.md](./details/INSTALLING.md) | Installing the daemon and the clients, and running the container image. |
| [UPGRADING.md](./details/UPGRADING.md) | How a version is named, how to upgrade, backing up. |
| [DEVELOPMENT.md](./details/DEVELOPMENT.md) | Building the daemon and clients from source, and running them locally. |
| [CONTRIBUTING.md](./details/CONTRIBUTING.md) | Branches, commits, CI and releases. |
| [TESTING.md](./details/TESTING.md) | The two test suites, their conventions, and the capacity benchmark. |

## plans/ — proposals and research

| Document | Holds | Fills in |
|---|---|---|
| [APPS.md](./plans/APPS.md) | The app platform: manifests, scopes, events, Block Kit UI, the in-workspace runtime. | ARCH-130–133; REQ-172–175, 376–381 |
| [BOLT.md](./plans/BOLT.md) | Research: what apps written with Bolt assume, and the decision that they run unchanged. | ARCH-131 |
| [QUICKJS.md](./plans/QUICKJS.md) | Research: QuickJS's costs and limits as the in-workspace runtime. | ARCH-133 |
| [HTTP-API.md](./plans/HTTP-API.md) | The documented HTTP API for integrations. | ARCH-123; REQ-176, 350–354 |
| [ASSISTANT.md](./plans/ASSISTANT.md) | The assistant that answers from what a person can read. | ARCH-121; REQ-330–334 |
| [COMPLIANCE-CAPTURE.md](./plans/COMPLIANCE-CAPTURE.md) | Compliance capture: the log, the extract schema, the file drop, the pull API. | ARCH-122; REQ-276, 340–345 |
| [MODERATION.md](./plans/MODERATION.md) | The moderator role, reports, and the three permissions. | ARCH-128; REQ-371–374 |

Outside `docs/`: [LICENSING.md](../LICENSING.md) holds the project's licence
terms, and [tools/gui/README.md](../tools/gui/README.md) how the graphical
client is driven and checked by hand.
