# syntax=docker/dockerfile:1

# The web client (docs/WEB.md), built with Emscripten first: the daemon's own
# build compiles its page and loader in (daemon/webapp.h) and serves the wasm
# from beside itself, so the daemon stage takes this stage's build/ as built.
FROM emscripten/emsdk:6.0.8 AS web
RUN apt-get update && apt-get install -y --no-install-recommends curl bzip2 cmake python3 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY Makefile .
COPY scripts ./scripts
COPY shared ./shared
COPY client ./client
COPY sdltext ./sdltext
COPY third_party/jsmn ./third_party/jsmn
COPY third_party/ca-roots ./third_party/ca-roots
RUN make web-gui build/gen/webapp_res.c

FROM alpine:3.20 AS build
# build-base for the toolchain; bash/curl/tar/bzip2 for scripts/build_mbedtls.sh,
# which `make` invokes to fetch + build the pinned mbedTLS static libs.
RUN apk add --no-cache build-base bash curl tar bzip2
WORKDIR /src
COPY Makefile .
COPY scripts ./scripts
COPY shared ./shared
COPY daemon ./daemon
# The vendored jsmn single-header (daemon/jwt.c) has no fetch script — unlike
# mbedTLS, which scripts/build_mbedtls.sh downloads during `make` — so it must
# be copied from the build context.
COPY third_party/jsmn ./third_party/jsmn
# picohttpparser (daemon/http.c) is vendored the same way.
COPY third_party/picohttpparser ./third_party/picohttpparser
# SQLite is compiled into the daemon from its vendored amalgamation, which is
# likewise only in the build context.
COPY third_party/sqlite-3.53.4 ./third_party/sqlite-3.53.4
# The Mozilla root certificates compiled into the daemon (ARCH-10), also only in
# the build context.
COPY third_party/ca-roots ./third_party/ca-roots
# The web client as the stage above built it: its products are newer than the
# sources, so make takes them as they are and needs no emcc here.
COPY --from=web /src/build ./build
# Stamped by the release so a running container reports the release it came from
# (`openchimed --version`). Unset for a local build, which reports "dev".
ARG OC_VERSION=
# Without read-aloud or voice input (TTS=0 STT=0): their engine, ONNX Runtime, is
# built for the glibc toolchain the release packages use, not for musl. The image
# gains them by carrying the release binary on a glibc base instead of compiling
# its own.
RUN make TTS=0 STT=0 OC_VERSION="$OC_VERSION"

FROM alpine:3.20

# No packages at all. No `sqlite` CLI: the daemon creates and migrates its own
# database, and the entrypoint no longer seeds one. No ca-certificates: the CA
# roots its outbound TLS verifies against are compiled in (ARCH-10).

COPY --from=build /src/openchimed /usr/local/bin/openchimed
# The web client's wasm, where the daemon looks when installed (daemon/webapp.h).
COPY --from=web /src/build/web/openchime.wasm /usr/share/openchime/web/openchime.wasm
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENV OPENCHIME_DB_PATH=/data/openchime.db
ENV OPENCHIME_HEALTH_PORT=8080
ENV OPENCHIME_PROTO_PORT=8443

EXPOSE 8080 8443

ENTRYPOINT ["/entrypoint.sh"]
