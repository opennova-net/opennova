# Multi-stage build of the standalone NovaWorld server (gate UDP, NW UDP,
# HTTP via Crow). Build context is the repo root:
#   docker build -f apps/novaworld_server/Dockerfile -t opennova-novaworld .
#
# The runtime image needs only the server binary plus its data dirs
# (migrations, seed, retail templates/static). SQLite lives in a mounted
# volume; the web portal is a separate nginx container.

# ---- build stage ----------------------------------------------------------
FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates \
        libcurl4-openssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Configure the whole tree (cheap) but compile only the server target and
# its lib deps. BUILD_NOVAWORLD_HTTP=ON pulls Crow + standalone Asio via
# FetchContent. GDExtension is never touched (godot/engine is its own
# project). Mirrors the ci.yml `novaworld-server` job's flags.
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_NOVAWORLD_HTTP=ON \
    && cmake --build build --target opennova_novaworld_server

# ---- runtime stage --------------------------------------------------------
# Same distro/glibc as the build stage so the binary's GLIBC/GLIBCXX
# requirements resolve (a debian:12 runtime is too old for ubuntu:24.04's
# libstdc++).
FROM ubuntu:24.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl libcurl4 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=build /src/build/apps/novaworld_server/opennova-novaworld /app/opennova-novaworld
COPY --from=build /src/backend/migrations /app/backend/migrations
COPY --from=build /src/backend/seed /app/backend/seed
COPY --from=build /src/apps/novaworld_server/templates /app/templates
COPY --from=build /src/apps/novaworld_server/static /app/static

# The server reads these paths from the environment (server_config.cpp).
ENV ONNET_GATE_UDP_PORT=7597 \
    ONNET_NW_UDP_PORT=64206 \
    ONNET_HTTP_PORT=8080 \
    DATABASE_PATH=/data/novaworld.db \
    MIGRATIONS_DIR=/app/backend/migrations \
    SEED_DIR=/app/backend/seed \
    TEMPLATES_DIR=/app/templates \
    STATIC_DIR=/app/static

VOLUME ["/data"]
EXPOSE 7597/udp 64206/udp 8080/tcp

HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8080/api/health || exit 1

ENTRYPOINT ["/app/opennova-novaworld"]
