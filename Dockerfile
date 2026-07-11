# syntax=docker/dockerfile:1

FROM alpine:3.20 AS build
RUN apk add --no-cache build-base sqlite-dev
WORKDIR /src
COPY Makefile .
COPY src ./src
RUN make

FROM alpine:3.20 AS litestream
ARG LITESTREAM_VERSION=v0.3.13
RUN apk add --no-cache curl tar && \
    curl -fsSL -o /tmp/litestream.tar.gz \
      "https://github.com/benbjohnson/litestream/releases/download/${LITESTREAM_VERSION}/litestream-${LITESTREAM_VERSION}-linux-amd64.tar.gz" && \
    tar -xzf /tmp/litestream.tar.gz -C /usr/local/bin litestream && \
    rm /tmp/litestream.tar.gz

FROM alpine:3.20

# gcompat: Litestream's release binaries are glibc-linked; this Alpine
# compatibility shim lets them run under musl without a source build.
RUN apk add --no-cache sqlite-libs sqlite ca-certificates gcompat

COPY --from=build /src/openchimed /usr/local/bin/openchimed
COPY --from=litestream /usr/local/bin/litestream /usr/local/bin/litestream
COPY litestream.yml /etc/litestream.yml
COPY entrypoint.sh /entrypoint.sh
RUN chmod +x /entrypoint.sh

ENV OPENCHIME_DB_PATH=/data/openchime.db
ENV OPENCHIME_HEALTH_PORT=8080

EXPOSE 8080

ENTRYPOINT ["/entrypoint.sh"]
