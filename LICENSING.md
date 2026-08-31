# Licensing

One licence, for everything this repository's contributors wrote:

| Path | Licence | SPDX |
|---|---|---|
| the whole repository — `daemon/`, `client/`, `shared/`, `tuikit/`, `scripts/`, `packaging/`, `docs/`, `tests/`, `Scripts/` | GNU Affero General Public License v3.0 or later | `AGPL-3.0-or-later` |
| `third_party/` | other people's work under their own terms — see below | — |

`LICENSE` is the AGPL text and covers the repository. There are no
per-directory licence files, because there is no boundary for one to mark.

## Why AGPL, and why it reaches the client too

The daemon is a network server, and AGPL's §13 is the point: someone who runs a
modified openchimed as a service for other people has to offer those users the
modified source. That is the licence doing the one job no other OSI-approved
licence does, and it is the reason a competitor cannot take this code, build a
closed hosted product on top of it, and keep the additions.

The clients are covered by the same licence rather than a permissive one. A
permissive `shared/` would have been the wire contract that anyone could
reimplement against, which has real value — but it also leaves the strategy
half-applied, since the parts a hosted competitor most wants to differentiate on
are as much client as daemon. Uniformity also removes the linking question
entirely: `shared/` is linked by both the daemon and the client, and when every
side of that link carries the same terms there is nothing to reason about.

For the clients specifically, §13 rarely fires — a desktop binary is not
interacted with over a network — so in practice the client terms behave like
GPL-3.0: modify and distribute a client, and the source travels with it.

## Third-party code

`third_party/` is other people's work under their own licences, unchanged:

| Component | Licence | Used by |
|---|---|---|
| Mbed TLS 3.6.2 | Apache-2.0 | TLS, SHA-256/PBKDF2, ES256 |
| SDL3 | zlib | GUI windowing, input, 2D renderer |
| jsmn | MIT | JSON tokenizer |
| termbox2 | MIT | TUI terminal layer |
| utf8proc | MIT-like | Unicode handling |
| lucide | ISC | icons |
| SQLite | public domain | linked dynamically (`-lsqlite3`) |

All are permissive and all are compatible with AGPL-3.0-or-later, so nothing in
`third_party/` constrains the choice above. Two consequences worth stating:

- Apache-2.0 is compatible with AGPL-3.0 but **not** with GPL-2.0 — while
  mbedTLS is linked, GPL-2.0 is not an available licence for this project.
- The optional Linux keyring path links **libsecret** and **glib**, which are
  LGPL-2.1-**or-later**. The "or later" is what makes them compatible here;
  LGPL-2.1-only would not be. glibc reaches the same result by its own system
  library exception.

## Contributing

Contributions are accepted under the Developer Certificate of Origin 1.1
(`DCO.txt`). Sign off each commit:

    git commit -s

There is no CLA, and copyright is not assigned: contributors keep their own. The
consequence is deliberate and worth stating -- the project cannot be relicensed
without the agreement of everyone who has contributed, so nobody has to trust a
promise about future terms.
