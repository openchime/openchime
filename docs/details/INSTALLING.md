# Installing OpenChime

How to install the daemon and the clients. Every setting is in
[CONFIG.md](./CONFIG.md); upgrading is [UPGRADING.md](./UPGRADING.md); building
from source is [DEVELOPMENT.md](./DEVELOPMENT.md).

## The daemon

Three published channels and a container image you build, one binary, one build id (ARCH-20): `openchimed --version` names the commit it was built from, and the release number is the label the packages carry. Packages are signed; the daemon
runs under systemd as a transient user with its state in `/var/lib/openchime`.

**Debian / Ubuntu** — bookworm or 22.04 and newer:

```sh
curl -fsSL https://api.openchime.io/v1/public/releases/dist/openchime-archive-keyring.gpg \
  | sudo tee /usr/share/keyrings/openchime-archive-keyring.gpg >/dev/null
echo "deb [signed-by=/usr/share/keyrings/openchime-archive-keyring.gpg] https://api.openchime.io/v1/public/releases/dist/apt stable main" \
  | sudo tee /etc/apt/sources.list.d/openchime.list
sudo apt update && sudo apt install openchimed
```

**RHEL / Rocky / Alma / Fedora** — RHEL 10 and newer (RHEL 9, with glibc
2.34, runs the container image below, with Podman or Docker):

```sh
sudo curl -fsSL https://api.openchime.io/v1/public/releases/dist/openchime.repo -o /etc/yum.repos.d/openchime.repo
sudo rpm --import https://api.openchime.io/v1/public/releases/dist/openchime-archive-keyring.asc
sudo dnf install openchimed
```

Both repositories are signed with the OpenChime archive signing key, Ed25519,
fingerprint `47A4 ECF0 F41F D8E0 8480  9285 FA07 1DC4 0E2F 491A`. Ed25519
package signatures need rpm 4.16 or newer, so RHEL/CentOS 8 (rpm 4.14) cannot
verify them and is not supported. The package server logs the downloads it
serves; to avoid that, mirror the repository or use the tarball or a source
build.

**Anything else, and the offline case.** The tarballs on each
[release](https://github.com/openchime/openchime/releases) carry the binary,
the unit and the config with an `install.sh`. Nothing is fetched at install
time — TLS is statically linked — so this is the channel for an air-gapped box.
Verify the `.sha256` beside it.

### Container

For any OCI runtime (Podman, Docker, containerd), built
from a release tarball with the `Dockerfile` in this repository:

```sh
packaging/image-context.sh openchimed-<N>-linux-amd64.tar.gz image
buildah build --arch amd64 -f Dockerfile -t openchime:<N> image
podman run openchime:<N> --version
```

To run it, give it a volume for its data and publish its ports:

```sh
podman run -d --name openchime \
  -p 8443:8443/tcp -p 8443:8443/udp -p 8080:8080 \
  -v openchime-data:/data \
  --env-file openchime.env \
  openchime:<N>
podman logs openchime 2>&1 | grep -i 'setup token'
```

`openchime.env` is your own settings file (CONFIG.md). Do not pass the
packages' `openchimed.env` unchanged: it sets
`OPENCHIME_DATA_DIR=/var/lib/openchime`, which moves the data off the `/data`
volume.

Everything the daemon keeps is under `/data` (`OPENCHIME_DATA_DIR`): keep it on
a volume, or a new container starts a new workspace. **8443** carries the
clients, the web client and the sign-in pages over TLS, and calls need its
**UDP** side too; **8080** is the plaintext port for `/healthz` and the
landing page, needed only by whatever checks the daemon's health. Settings
are the container's environment, the same variables as `openchimed.env`
(CONFIG.md). The first start prints the one-time owner setup token to the
log, as above; redeem it on the daemon's sign-up page.

`image-context.sh` also fetches the pinned Litestream the image carries; it
runs only when the deployment writes the `replica-*` files under
`/etc/openchime` that name an S3 endpoint, a bucket, the data's and the identity's places in it,
and a key
(CONFIG.md, ARCH-3), and without them the image starts the daemon alone.
Releases publish no public image: build one from the tarball as above.
Nothing is built in a container, and there is no Compose stack in this
repository.

After installing, the daemon starts and mints a one-time owner setup token:

```sh
journalctl -u openchimed | grep -i 'setup token'
```

Open `https://<host>:8443/signup` and redeem it to create the workspace's
owner. The certificate is self-signed until you configure one (TLS.md),
so the browser warns once.

Configuration is entirely environment variables in
`/etc/openchime/openchimed.env` (every one of them:
[CONFIG.md](./CONFIG.md)). It listens on **8443** out of the box so
installing cannot collide with a web server on 443; switch
`OPENCHIME_PORT=443` for a real deployment, which needs no root because
the unit already carries `CAP_NET_BIND_SERVICE`.

**Requirements:** glibc **2.35+**: Ubuntu 22.04 and newer, Debian bookworm and
newer, RHEL 10 and newer. Everything else, TLS and SQLite included, is
statically linked. Debian bullseye (glibc 2.31) and RHEL 9 (glibc 2.34) are too
old for the packages; RHEL 9 runs the container image (Podman or Docker).

## The clients

- **Web:** nothing to install. Open `https://<host>:8443/app/` (or just the
  host: `/` sends a browser there) and sign in. The daemon serves it, so it
  always matches the daemon (WEB.md).
- **Windows:** run `openchime-<N>-windows-x64-setup.exe` from the
  [release](https://github.com/openchime/openchime/releases). It installs the
  graphical client and the terminal client (`openchime-tui`), optionally on
  `PATH`. A release whose installer is not code-signed says so in its notes,
  and SmartScreen warns before running it.
- **Linux:** the graphical client, `openchime`, from the same apt and dnf
  repositories as the daemon, or from its Flatpak repository (below).
- **Terminal client on Linux:** not in the packages; build it from source
  (`make build/openchime-tui`) and run `build/openchime-tui <workspace>`, or
  `build/openchime-tui <host> <port>` to connect to an address directly.

### The Linux client

The graphical client is its own package, `openchime`, beside the daemon's
`openchimed` in the same signed repositories and the same release, for amd64
and arm64; installing one does not install the other. Each channel adds the
desktop entry, which also opens `openchime://` links, and the icons, and
`openchime --version` names the build, as the daemon's does.

**Debian / Ubuntu** — Ubuntu 24.04 or Debian 13 (trixie) and newer, with the
repository added as for the daemon (above):

```sh
sudo apt update && sudo apt install openchime
```

**Fedora / RHEL / Rocky / Alma** — Fedora 40 or RHEL 10 and newer, with the
repository added as for the daemon (above):

```sh
sudo dnf install openchime
```

The packages need glibc 2.39 and the distribution's own FreeType, HarfBuzz,
fontconfig, PipeWire, GLib and libsecret, which every desktop has. That is
the floor (ARCH-20): Ubuntu 24.04+, Debian 13+, Fedora 40+, RHEL 10+.
**Older systems** (Ubuntu 22.04, Debian 12, RHEL 9 and the like) **use the
Flatpak**, which brings its own runtime.

**Flatpak** — any Linux desktop with Flatpak:

```sh
flatpak install https://api.openchime.io/v1/public/releases/dist/flatpak/io.openchime.OpenChime.flatpakref
```

That adds the repository as the remote `openchime`, installs the client,
`io.openchime.OpenChime`, and the runtime it runs on,
`org.freedesktop.Platform` 26.08, from Flathub, and `flatpak update` takes
each later release. To add only the repository:

```sh
flatpak remote-add --if-not-exists openchime https://api.openchime.io/v1/public/releases/dist/flatpak/io.openchime.OpenChime.flatpakrepo
flatpak install openchime io.openchime.OpenChime
```

The second command needs Flathub as a remote too, for the runtime. The
repository is the project's own, not Flathub, and its summary is signed with
the same archive signing key as the apt and dnf repositories; the files that
add it carry the key. Each release also attaches the client as single-file
bundles, `io.openchime.OpenChime-<N>-x86_64.flatpak` and `-aarch64.flatpak`
(`flatpak install <file>`), which take their runtime from Flathub; a client
installed from one is not updated by `flatpak update`. In the sandbox the client reaches the network, the display
(Wayland, else X11), the GPU, PipeWire for calls, and on the session bus the
desktop portals, notifications, the tray and the Secret Service, where its
session is kept as the packages keep it.
