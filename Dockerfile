# syntax=docker/dockerfile:1
#
# The container image: the released daemon and its data on a glibc base. The
# build context is a release tarball laid out by packaging/image-context.sh --
# the same binary the .deb, the .rpm and the tarball ship, read-aloud and
# voice input included -- so nothing compiles here and no RUN step exists,
# which is what lets `buildah build --arch arm64` produce the arm64 image on
# an x86 machine with no emulation (.github/workflows/release.yml).
#
#   make full                                   # or: make RELEASE=1 ARCH=aarch64 ...
#   packaging/build-tarball.sh <version> <arch> openchimed dist
#   packaging/image-context.sh dist/openchimed-<version>-linux-<arch>.tar.gz image
#   buildah build --arch <arch> -f Dockerfile -t openchime:<version> image
#
# bookworm's glibc is 2.36, above the 2.34 the binary is linked against. No
# packages are added: the CA roots the daemon's outbound TLS verifies against
# are compiled in (ARCH-10), and it creates and migrates its own database.
FROM debian:bookworm-slim

COPY --chmod=755 openchimed /usr/local/bin/openchimed
# Where an installed daemon looks for its data (daemon/tts_data.h, webapp.h).
COPY tts /usr/share/openchime/tts
COPY stt /usr/share/openchime/stt
COPY web /usr/share/openchime/web
COPY --chmod=755 entrypoint.sh /entrypoint.sh

ENV OPENCHIME_DATA_DIR=/data
ENV OPENCHIME_HEALTH_PORT=8080
ENV OPENCHIME_PORT=8443

EXPOSE 8080 8443/tcp 8443/udp

ENTRYPOINT ["/entrypoint.sh"]
