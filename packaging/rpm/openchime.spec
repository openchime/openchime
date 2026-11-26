# RPM packaging for the OpenChime Linux client (ARCH-20).
#
# Packages an already-built binary, as openchimed.spec does and for its
# reasons: the release builds the client once per architecture, and the .rpm
# and the .deb carry that same file. %%build is therefore empty on purpose.
#
# Built by packaging/build-client-rpm.sh: rpmbuild -bb --target <arch>
#   --define "_version N" --define "_bindir_src <dir>"
# (Escaped above: rpm expands macros inside comments and warns about it.)

# Ship the binary exactly as handed over (openchimed.spec says why): the
# package installs no shared library, man page or script for the skipped steps
# to act on.
%global __os_install_post %{nil}
%global debug_package %{nil}

Name:           openchime
Version:        %{_version}
Release:        1%{?dist}
Summary:        OpenChime chat client

# The client's own code and the components it links: mbedTLS (Apache-2.0);
# libspng (BSD, two clauses); libvpx, Opus, speexdsp, libfvad and libwebp
# (BSD, three clauses); libjpeg-turbo (IJG, BSD three clauses and zlib);
# utf8proc, jsmn, QR Code generator, stb and miniz (MIT); miniaudio (MIT-0,
# or public domain); the Lucide icons (ISC); Mozilla's CA roots (MPL-2.0); and
# SDL3 (zlib).
License:        AGPL-3.0-or-later AND Apache-2.0 AND BSD-2-Clause AND BSD-3-Clause AND IJG AND MIT AND MIT-0 AND ISC AND MPL-2.0 AND Zlib
URL:            https://openchime.io
# No BuildArch: the architecture is the --target build-client-rpm.sh passes
# (openchimed.spec says why).

# The system libraries it links, under Fedora's and RHEL's names. rpm adds
# each shared object's own name beside them, from the binary.
Requires:       freetype
Requires:       harfbuzz
Requires:       fontconfig
Requires:       pipewire-libs
Requires:       glib2
Requires:       libsecret

%description
The desktop client for an OpenChime server: messaging, channels, threads and
search; calls with screen sharing and cameras, encrypted end to end; recorded
video messages; and read-aloud, voice input and conversation summaries, all
computed on the workspace's own server.

It runs on Wayland and X11, keeps its session in the desktop's keyring, and
opens openchime:// links.

%prep
# Nothing to unpack: the inputs are files staged by build-client-rpm.sh.

%build
# Intentionally empty -- see the header.

%install
install -D -m 0755 %{_bindir_src}/openchime                          %{buildroot}%{_bindir}/openchime
install -D -m 0644 %{_bindir_src}/io.openchime.OpenChime.desktop     %{buildroot}%{_datadir}/applications/io.openchime.OpenChime.desktop
install -D -m 0644 %{_bindir_src}/io.openchime.OpenChime.metainfo.xml %{buildroot}%{_datadir}/metainfo/io.openchime.OpenChime.metainfo.xml
install -D -m 0644 %{_bindir_src}/LICENSE                            %{buildroot}%{_datadir}/licenses/openchime/LICENSE
mkdir -p %{buildroot}%{_datadir}/icons
cp -r %{_bindir_src}/hicolor %{buildroot}%{_datadir}/icons/hicolor
find %{buildroot}%{_datadir}/icons -type d -exec chmod 0755 {} +
find %{buildroot}%{_datadir}/icons -type f -exec chmod 0644 {} +

%files
%{_bindir}/openchime
%{_datadir}/applications/io.openchime.OpenChime.desktop
%{_datadir}/metainfo/io.openchime.OpenChime.metainfo.xml
%{_datadir}/icons/hicolor/*/apps/io.openchime.OpenChime.png
%license %{_datadir}/licenses/openchime/LICENSE

%changelog
# Deliberately empty, as openchimed.spec's: releases are numbered by CI
# (ARCH-20) and the history lives in git.
