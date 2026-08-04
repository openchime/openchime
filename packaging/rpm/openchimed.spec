# RPM packaging for the OpenChime daemon (ARCH-20).
#
# This spec packages an already-built binary rather than building one. The
# release builds each architecture once against the oldest glibc it supports and
# feeds that same binary to the .deb, the .rpm and the tarball, so all three
# carry an identical artifact instead of three builds that merely share a source
# tree. %build is therefore empty on purpose.
#
# Built with: rpmbuild -bb --define "_version N" --define "_bindir_src <dir>"

# %%{_unitdir} is defined by systemd-rpm-macros. Rather than take that as a build
# dependency for one path, fall back to the value it would supply. The spec then
# builds in any minimal container, which is where the release runs it.
# (Escaped above: rpm expands macros inside comments and warns about it.)
%{!?_unitdir: %global _unitdir /usr/lib/systemd/system}

# Ship the binary exactly as handed over.
#
# rpm's default post-install pipeline rewrites it -- brp-strip and
# brp-strip-comment-note between them changed a pre-stripped binary by 120
# bytes in testing. That would mean the .rpm, the .deb and the tarball each
# carry a *different* file built from one source, and the useful property here
# is the stronger one: all three ship the identical artifact, verifiable with a
# single checksum. The release strips once, before packaging.
#
# Safe for this package specifically: it installs no shared libraries (no
# ldconfig), no man pages (no brp-compress) and no scripts (no shebangs to
# mangle), so the skipped steps have nothing to act on.
%global __os_install_post %{nil}
%global debug_package %{nil}

Name:           openchimed
Version:        %{_version}
Release:        1%{?dist}
Summary:        OpenChime chat daemon

# The daemon's own code is not open source; the components it links are. RPM
# expects one field, so both are named.
License:        Proprietary AND Apache-2.0 AND MIT
URL:            https://openchime.io
BuildArch:      %{_target_cpu}

# sqlite-libs is the RHEL-family name for what Debian calls libsqlite3-0.
Requires:       sqlite-libs
# Needed only for outbound TLS -- federated enrollment, push, and S3-backed
# attachments. A stand-alone deployment with local blob storage genuinely runs
# without it, so this is not a hard requirement.
Recommends:     ca-certificates

%description
The OpenChime server: a single-binary chat daemon holding one tenant's messages
in a local SQLite database, speaking a binary protocol over TLS with certificate
pinning.

Runs stand-alone with no dependency on any external service, or federated,
opting in per function to OIDC login, mobile push delivery and the app
directory. In both cases the daemon holds all message data locally.

Configuration is entirely by environment variable; the package ships an
EnvironmentFile at /etc/openchime/openchimed.env.

%prep
# Nothing to unpack: the inputs are files staged by build-rpm.sh.

%build
# Intentionally empty -- see the header.

%install
install -D -m 0755 %{_bindir_src}/openchimed          %{buildroot}%{_bindir}/openchimed
install -D -m 0644 %{_bindir_src}/openchimed.service  %{buildroot}%{_unitdir}/openchimed.service
install -D -m 0644 %{_bindir_src}/openchimed.env      %{buildroot}%{_sysconfdir}/openchime/openchimed.env
install -D -m 0644 %{_bindir_src}/copyright           %{buildroot}%{_datadir}/licenses/openchimed/copyright

%files
%{_bindir}/openchimed
%{_unitdir}/openchimed.service
# noreplace is the %config form that preserves an operator's edits across an
# upgrade, writing any new version alongside as .rpmnew. It is the RPM
# equivalent of Debian's conffiles.
%config(noreplace) %{_sysconfdir}/openchime/openchimed.env
%license %{_datadir}/licenses/openchimed/copyright
%dir %{_sysconfdir}/openchime

# The scriptlets are hand-written rather than using the %systemd_* macros, so the
# spec needs no systemd-rpm-macros at build time and behaves the same as the
# Debian maintainer scripts. $1 is the count of installed versions: 1 on a fresh
# install, 2+ on an upgrade.
%post
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || :
    if [ "$1" -eq 1 ]; then
        systemctl enable --now openchimed.service >/dev/null 2>&1 || :
    else
        systemctl try-restart openchimed.service >/dev/null 2>&1 || :
    fi
fi
if [ "$1" -eq 1 ]; then
cat <<'EOF'

OpenChime is installed and starting.

  Configuration:  /etc/openchime/openchimed.env   (edit, then: systemctl restart openchimed)
  State:          /var/lib/openchime/
  Logs:           journalctl -u openchimed

The first-run owner setup token is printed to the log once, at first start:

  journalctl -u openchimed | grep -i 'setup token'

The daemon listens on 8443 by default. For a real deployment set
OPENCHIME_PROTO_PORT=443 -- the unit already holds CAP_NET_BIND_SERVICE.

EOF
fi

%preun
# $1 is 0 on a real removal, 1 on the remove half of an upgrade.
if [ "$1" -eq 0 ] && [ -d /run/systemd/system ]; then
    systemctl --no-reload disable --now openchimed.service >/dev/null 2>&1 || :
fi

%postun
if [ -d /run/systemd/system ]; then
    systemctl daemon-reload >/dev/null 2>&1 || :
fi
# /var/lib/openchime is deliberately never removed. It holds the tenant's entire
# message history, and the daemon has no replication or backup component by
# design (ARCH-3) -- that file may be the only copy. Uninstalling is not consent
# to destroy it.
if [ "$1" -eq 0 ] && [ -d /var/lib/openchime ]; then
    echo "openchimed: /var/lib/openchime left in place (database, blobs, TLS identity)."
fi

%changelog
# Deliberately empty. Releases are numbered by CI (ARCH-20) and the history lives
# in git; a hand-maintained changelog here would be a second, worse copy.
