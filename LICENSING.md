# Licensing

Two licences, split by what the code is:

| Path | Licence | SPDX |
|---|---|---|
| `daemon/` | GNU Affero General Public License v3.0 or later | `AGPL-3.0-or-later` |
| `client/` | MIT | `MIT` |
| `shared/` | MIT | `MIT` |
| `tuikit/` | MIT | `MIT` |
| everything else (`scripts/`, `packaging/`, `docs/`, `tests/`, `Scripts/`) | AGPL-3.0-or-later | `AGPL-3.0-or-later` |

`LICENSE` is the AGPL text and is the repository default. `LICENSE-MIT` is the
MIT text; the three MIT directories each carry a copy so the boundary is
unambiguous in a source tarball.

## Why the split is drawn here and not somewhere tidier

The daemon is a network server, and AGPL's §13 is the point: someone who runs a
modified openchimed as a service for other people has to offer those users the
modified source. That is the licence doing the one job no other OSI-approved
licence does.

`shared/` is MIT rather than AGPL **because the client links it.** The Makefile
calls it "the wire contract (linked by both the daemon and the client)". Had it
been AGPL, every client binary would be a work based on AGPL code and would have
to be conveyed under the AGPL regardless of what `client/LICENSE` said -- the MIT
label would have been decorative. Making the wire contract permissive is also the
right outcome on its own terms: a protocol implementation that other people
cannot freely reimplement against is not much of a protocol.

The daemon combining MIT `shared/` into an AGPL binary is fine and needs no
special handling: AGPL-3.0 can absorb MIT, not the other way round.

## Third-party code

`third_party/` is other people's work under their own licences, unchanged:

| Component | Licence | Used by |
|---|---|---|
| Mbed TLS 3.6.2 | Apache-2.0 | TLS, SHA-256/PBKDF2, ES256 |
| jsmn | MIT | JSON tokenizer |
| termbox2 | MIT | TUI terminal layer |
| utf8proc | MIT-like | Unicode handling |
| lucide | ISC | icons |
| SQLite | public domain | linked dynamically (`-lsqlite3`) |

All are permissive, so nothing in `third_party/` constrains the choices above.
Note Apache-2.0 is compatible with AGPL-3.0 but **not** with GPL-2.0 -- if the
daemon's licence is ever revisited, GPL-2.0 is not available while mbedTLS is
linked.

## Contributing

Contributions are accepted under the Developer Certificate of Origin 1.1
(`DCO.txt`). Sign off each commit:

    git commit -s

There is no CLA, and copyright is not assigned: contributors keep their own. The
consequence is deliberate and worth stating -- the project cannot be relicensed
without the agreement of everyone who has contributed, so nobody has to trust a
promise about future terms.
