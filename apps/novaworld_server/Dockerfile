# Multi-stage build of the standalone NovaWorld server (gate UDP, NW UDP,
# HTTP via Crow). Build context is the repo root:
#   docker build -f apps/novaworld_server/Dockerfile -t opennova-novaworld-server .
#
# The runtime image needs only the server binary plus its data dirs
# (migrations, seed, retail templates/static), and for the hosted servers' ec2
# provider the AWS CLI v2 and the machine script. SQLite lives in a mounted
# volume; the web portal is a separate nginx container.

# ---- build stage ----------------------------------------------------------
FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Configure the whole tree (cheap) but compile only the server target and
# its lib deps. BUILD_NOVAWORLD_HTTP=ON pulls Crow + standalone Asio via
# FetchContent. GDExtension is never touched (godot/src is its own
# project). Mirrors the flags of ci.yml's `test-linux` job and of
# .github/workflows/net-linux.yml.
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_NOVAWORLD_HTTP=ON \
    && cmake --build build --target opennova_novaworld_server

# ---- AWS CLI stage ---------------------------------------------------------
# The hosted servers' ec2 provider (hosted/ec2_provider.h) runs the AWS CLI v2,
# its credentials the service instance's role (IMDSv2). A pinned release,
# checked against its SHA-256 before it is unpacked. The sum was taken from the
# release AWS signed: its .sig verifies against the AWS CLI Team key in the
# CLI's install guide (fingerprint FB5D B77F D5C1 18B8 0511 ADA8 A631 0ACC 4672
# 475C). A bump moves both ARGs: fetch the new zip and its .sig, verify, and
# record the new sum.
FROM ubuntu:24.04 AS awscli

ARG AWSCLI_VERSION=2.36.50
ARG AWSCLI_SHA256=0f15318b3edc4133ad71ce550303babdf2b5dc66bee6b91b4ac8bf05407a78bf

RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl unzip \
    && rm -rf /var/lib/apt/lists/* \
    && curl -fsSL "https://awscli.amazonaws.com/awscli-exe-linux-x86_64-${AWSCLI_VERSION}.zip" \
        -o /tmp/awscliv2.zip \
    && echo "${AWSCLI_SHA256}  /tmp/awscliv2.zip" | sha256sum -c - \
    && unzip -q /tmp/awscliv2.zip -d /tmp \
    && /tmp/aws/install --install-dir /usr/local/aws-cli --bin-dir /usr/local/bin \
    && /usr/local/bin/aws --version

# ---- runtime stage --------------------------------------------------------
# Same distro/glibc as the build stage so the binary's GLIBC/GLIBCXX
# requirements resolve (a debian:12 runtime is too old for ubuntu:24.04's
# libstdc++).
FROM ubuntu:24.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=build /src/build/apps/novaworld_server/opennova-novaworld-server /app/opennova-novaworld-server
COPY --from=build /src/backend/migrations /app/backend/migrations
COPY --from=build /src/backend/seed /app/backend/seed
COPY --from=build /src/apps/novaworld_server/templates /app/templates
COPY --from=build /src/apps/novaworld_server/static /app/static
# The ec2 provider's machine script (the cloud machines' user-data template)
# and the AWS CLI it runs, at the absolute path it runs it by.
COPY --from=build /src/apps/novaworld_server/hosted/machine/user_data.sh /app/hosted/machine_user_data.sh
COPY --from=awscli /usr/local/aws-cli /usr/local/aws-cli
RUN ln -s /usr/local/aws-cli/v2/current/bin/aws /usr/local/bin/aws && aws --version

# The server reads these paths from the environment (server_config.cpp).
ENV ONNET_GATE_UDP_PORT=7597 \
    ONNET_NW_UDP_PORT=64206 \
    ONNET_HTTP_PORT=8080 \
    DATABASE_PATH=/data/novaworld.db \
    MIGRATIONS_DIR=/app/backend/migrations \
    SEED_DIR=/app/backend/seed \
    TEMPLATES_DIR=/app/templates \
    STATIC_DIR=/app/static \
    ONNET_HOSTED_MACHINE_SCRIPT=/app/hosted/machine_user_data.sh \
    ONNET_HOSTED_AWS_CLI=/usr/local/bin/aws

VOLUME ["/data"]
EXPOSE 7597/udp 64206/udp 8080/tcp

HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8080/api/health || exit 1

# Run it under an init as PID 1 (the compose files' `init: true`, or `docker
# run --init`): a second SIGTERM ends a stuck shutdown only when the server is
# not PID 1, which the kernel shields from default-action signals (main.cpp).
ENTRYPOINT ["/app/opennova-novaworld-server"]
