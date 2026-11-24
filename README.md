# OpenChime

OpenChime is a business chat system you can run yourself. Each workspace is one
C daemon with its own SQLite database, so no workspace shares a process or a
database with another. Clients speak a compact binary protocol to it over TLS.
They are one shared C client core under two frontends: a portable graphical
client, which runs on Windows and in the browser (served by the daemon itself),
and a terminal client. Beside messaging, channels, threads and search, it does
calls with screen sharing and cameras, encrypted end to end; recorded video
messages; and read-aloud, voice input and conversation summaries, all computed
on the workspace's own server.

It comes in two deployment models. **Self-hosted**, you run the daemon and own
all of its data, and it depends on nothing the project operates unless you turn
a service on: sign-in through the project's relay, mobile push, invitation
mail, or a name and certificate under the service's domain. **Managed**, the
project runs the same daemon and those services for you. In both, no service
the project operates is ever in the message path: the services carry identity,
notification, discovery and provisioning metadata, never what people say.

To run it, start with [installing](./docs/details/INSTALLING.md) the daemon and
the clients, then [configuring](./docs/details/CONFIG.md) it — the daemon is
configured entirely by environment variables — and, for later releases,
[upgrading](./docs/details/UPGRADING.md). To work on it,
[DEVELOPMENT.md](./docs/details/DEVELOPMENT.md) covers building and running it
locally, [CONTRIBUTING.md](./docs/details/CONTRIBUTING.md) the process for a
change and [TESTING.md](./docs/details/TESTING.md) the test suites. What is
built, missing or broken is tracked in the
[issue tracker](https://github.com/openchime/openchime/issues).

The design is written down in two references. [REQUIREMENTS.md](./docs/REQUIREMENTS.md)
says what the finished system does, as numbered requirements (`REQ-N`), and
[ARCHITECTURE.md](./docs/ARCHITECTURE.md) says how it is built, as numbered
decisions (`ARCH-N`), each with what it rejected. Every other document fills
those two in — the wire protocol, the schema, each feature's design, each
library — and [OVERVIEW.md](./docs/OVERVIEW.md) is the map of them all.

Everything in this repository the project wrote is licensed
**AGPL-3.0-or-later**: run a modified daemon as a service for other people and
they are entitled to your changes. `third_party/` holds other projects' work
under their own licences. Contributions are taken under the Developer
Certificate of Origin (`git commit -s`), with no CLA. [LICENSING.md](./LICENSING.md)
has the full map.
