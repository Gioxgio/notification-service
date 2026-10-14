# syntax=docker/dockerfile:1

# ---------- build stage ----------
FROM debian:bookworm-slim AS builder

RUN apt-get update \
    && apt-get install -y --no-install-recommends \
        build-essential \
        libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# "make" also regenerates compile_commands.json, which references
# tests/*.c — keep the tests/ directory in the build context.
RUN make -j"$(nproc)"

# ---------- runtime stage ----------
FROM debian:bookworm-slim

# libcurl4 at runtime (plus its TLS deps) and ca-certificates so
# the service can verify api.telegram.org.
RUN apt-get update \
    && apt-get install -y --no-install-recommends libcurl4 ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --home-dir /nonexistent --shell /usr/sbin/nologin notification

COPY --from=builder /src/build/notification-service /usr/local/bin/notification-service

# Defaults; override with -e PORT=... (and map the same port).
ENV PORT=8080
EXPOSE 8080

# docker stop sends SIGTERM, which the service handles gracefully.
USER notification
STOPSIGNAL SIGTERM
ENTRYPOINT ["notification-service"]
