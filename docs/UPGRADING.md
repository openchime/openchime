# Upgrading

For operators: how a version is named, how to upgrade, and what an upgrade
changes that an operator has to know about. Installing is in the
[README](../README.md#install-the-daemon); every setting is in
[CONFIG.md](./CONFIG.md).

## Versions: a build id and a release number

A build is identified by its **build id**: the short commit it was built from
(`a30809b`). It is compiled into the daemon, the web client the daemon serves
and the Windows clients, and it is what `openchimed --version`, the startup
log, `openchime-tui --version`, the Windows file properties (ProductVersion
`build a30809b`) and the GUI's About OpenChime report. About shows the
daemon's too, which the daemon sends each client at sign-in (PROTOCOL.md
`SERVER_BUILD`). A daemon built from source without one reports `dev`.

A **release number** is a label given to one build id when it is released:
`release-1` is build `a30809b`. It appears only where a growing version is
required: the deb and rpm versions and filenames, the tarball and installer
filenames, the installer's version, the `release-N` tag and the GitHub
release, whose notes name the build id. Nothing about a release number is in
a binary; the tag on the commit maps one to the other. The number is a plain
integer and only grows.

## How to upgrade

- **apt / dnf:** `sudo apt update && sudo apt install openchimed`, or
  `sudo dnf upgrade openchimed`. The package keeps
  `/etc/openchime/openchimed.env`, checks it (below), and restarts the daemon
  if it was running. If the check printed anything, fix it and run
  `systemctl restart openchimed`.
- **Tarball:** run the new tarball's `install.sh`; it keeps an existing
  `/etc/openchime/openchimed.env` and writes the new default beside it as
  `openchimed.env.new`. It neither checks the settings nor restarts a running
  daemon: run the check below, then `systemctl restart openchimed`.
- **Container image:** build the new release's image from its tarball (README
  "Container") and start it on the same volume.
- **Clients:** the web client is the daemon's own and updates with it. On
  Windows, run the newer release's `-setup.exe` over the old one. The terminal
  client built from source is rebuilt from the new release's source.

The daemon applies its own database migrations at start (SCHEMA.md §1,
"The migration mechanism"). Take a backup first (below): a migrated database
cannot be opened by an older daemon.

## Backing up

Everything the daemon keeps is in its data directory (`/var/lib/openchime` for
the packages, `/data` in the image): `openchime.db`, `identity/identity.db`,
`blobs/` (unless attachments are kept in S3), and `cert.pem`/`key.pem`. Back
the two databases up together: `identity/identity.db` holds the box's identity
(ARCH-66b): its certificates and key, the enrollment with central, the local
issuer and the second-step key. A copy of `openchime.db` restored without it
brings back the messages, but the daemon begins a new identity, and accounts
with a second step cannot sign in until the identity is restored. For a
consistent copy, stop the daemon (`systemctl stop openchimed`), copy the whole
directory, and start it again;
or, live, copy each database with `sqlite3 <db> ".backup <copy>"` and the rest
of the directory beside them. Keep the copy taken before an upgrade until you
are sure you will not go back.

An image that replicates (CONFIG.md "Replication, for the image only") also
copies its databases to S3; it needs all six `replica-*` files, `replica-endpoint`
and `replica-identity-prefix` among them, and stops the boot as misconfigured
(`entrypoint: replica=misconfigured`) with only some.

## Before restarting: check the settings

The daemon is strict about its settings (CONFIG.md): an unknown `OPENCHIME_*`
name, one that was a setting once (CONFIG.md "No longer read"), or a
value that does not parse or is out of its range stops it, rather than letting
it run with a setting it misread. A box upgraded with an old environment file
can therefore fail to come up, by design. The packages run

    openchimed --check-config --env-file /etc/openchime/openchimed.env

on install and print what the new daemon would refuse; run the same yourself
after editing the file, before restarting.

## Nothing that ships runs in development mode

There is one build, and the tests run the binary that ships; what keeps a
deployment ordinary is `OPENCHIME_MODE`, which defaults to `normal`
(CONFIG.md "Mode"). A development input in a deployment's environment stops
the daemon.
