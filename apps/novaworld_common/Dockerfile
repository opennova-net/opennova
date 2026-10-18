# The NovaWorld service's six programs (README.md "Programs"), one image each,
# from one shared build stage. Build context is the repo root, one target per
# program:
#   docker build -f apps/novaworld_common/Dockerfile --target gate -t opennova-novaworld-gate .
# and alike for lobby, web, fleet, worker and data (.github/workflows/novaworld-images.yml
# publishes them as ghcr.io/<owner>/opennova-novaworld-<program>, all six in one bake:
# docker-bake.hcl beside this file). One more target, smoke, is the local stack's
# smoke test (scripts/dev_stack.sh), never published.
#
# Each runtime image holds its program's binary alone over one shared base: libpq,
# the migrations and the seed (every program may migrate at boot),
# the RDS CA bundle (DATABASE_URL against RDS carries
# sslmode=verify-full&sslrootcert=/app/rds-global-bundle.pem), curl (the ECS
# container health check, and the image's HEALTHCHECK, ask GET /livez) and the
# shell tools ECS Exec sessions use (sh, base64, tr, sed, head, mktemp). Beyond
# that base: web hiredis (the one program that links it) and the retail pages
# (templates, static); the worker the machine script (its machine driver alone
# renders it); the lobby (its own public address, from
# EC2), web (the game data's store: the admin page's uploads, the update
# service's packages), the worker (the fleet's machines, the outbound mail) and
# data (the store: the trees it builds, the packages it reads) the AWS CLI v2. No image keeps state: the database is
# PostgreSQL (DATABASE_URL, DATABASE_PASSWORD), web's state Redis
# (ONNET_REDIS_URL, ONNET_REDIS_PASSWORD), the game data the store (S3 on a
# deployment; the compose stack's directory volume mounts under
# /var/lib/opennova/store). The web portal is a separate nginx image
# (web/Dockerfile), a second container in web's task.

# ---- build stage ----------------------------------------------------------
FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates libpq-dev \
        libhiredis-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Configure the whole tree (cheap) but compile only the six programs and
# their lib deps. OPENNOVA_NOVAWORLD_SERVICE=ON defines the service and its
# programs; BUILD_NOVAWORLD_HTTP=ON pulls Crow + standalone Asio via
# FetchContent (web, the fleet, the worker and data build with it).
# GDExtension is never touched (godot/src is its own project). Mirrors the
# flags of ci.yml's `test-linux` job and of .github/workflows/net-linux.yml.
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DOPENNOVA_NOVAWORLD_SERVICE=ON \
        -DBUILD_NOVAWORLD_HTTP=ON \
    && cmake --build build --target opennova_novaworld_gate opennova_novaworld_lobby \
        opennova_novaworld_web opennova_novaworld_fleet opennova_novaworld_worker \
        opennova_novaworld_data

# ---- AWS CLI stage ---------------------------------------------------------
# The hosted servers' ec2 machine driver (hosted/cloud/ec2_machine_driver.h), the
# game data's S3 store (core/s3_object_store.h: s3 cp, sync and rm, s3api's objects
# and multipart uploads) and the outbound mail (accounts/mailer.h, sesv2 send-email) run
# the AWS CLI v2, their credentials the ECS task role's. A pinned release,
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

# The programs call ec2 (the machines, and the static addresses' Elastic IPs),
# route53 (the static addresses' records, hosted/cloud/address_zone.h), s3 (the
# s3 and s3api commands share its model) and sesv2 alone (sts for a
# credentials check by hand); web reads CloudWatch, its logs and ECS through
# curl's own SigV4 (infra/aws_api.h), never the CLI. The help examples, the
# completer and its index, and every other service's model go (about 160 MB
# of the CLI's 270). The models' root files (endpoints.json, partitions.json)
# stay. Then the check: the commands the programs run still build, and a real
# describe-instances and a real s3 ls run to their HTTP request (fake keys, a
# closed local port, so they can reach nothing).
RUN dist=/usr/local/aws-cli/v2/current/dist \
    && data="$dist/awscli/botocore/data" \
    && test -d "$data/ec2" && test -d "$data/route53" && test -d "$data/s3" && test -d "$data/sts" \
    && test -d "$data/sesv2" && test -f "$data/endpoints.json" \
    && rm -rf "$dist/awscli/examples" "$dist/awscli/data/ac.index" "$dist/aws_completer" \
        /usr/local/aws-cli/v2/current/bin/aws_completer /usr/local/bin/aws_completer \
    && find "$data" -mindepth 1 -maxdepth 1 -type d ! -name ec2 ! -name route53 ! -name s3 ! -name sts ! -name sesv2 \
        -exec rm -rf {} + \
    && aws ec2 describe-instances --generate-cli-skeleton >/dev/null \
    && aws ec2 run-instances --generate-cli-skeleton >/dev/null \
    && aws ec2 terminate-instances --generate-cli-skeleton >/dev/null \
    && aws ec2 describe-addresses --generate-cli-skeleton >/dev/null \
    && aws ec2 allocate-address --generate-cli-skeleton >/dev/null \
    && aws ec2 associate-address --generate-cli-skeleton >/dev/null \
    && aws ec2 disassociate-address --generate-cli-skeleton >/dev/null \
    && aws ec2 release-address --generate-cli-skeleton >/dev/null \
    && aws route53 change-resource-record-sets --generate-cli-skeleton >/dev/null \
    && aws route53 get-change --generate-cli-skeleton >/dev/null \
    && aws s3api list-objects-v2 --generate-cli-skeleton >/dev/null \
    && aws s3api head-object --generate-cli-skeleton >/dev/null \
    && aws s3api upload-part --generate-cli-skeleton >/dev/null \
    && aws s3api copy-object --generate-cli-skeleton >/dev/null \
    && aws s3api create-multipart-upload --generate-cli-skeleton >/dev/null \
    && aws s3api complete-multipart-upload --generate-cli-skeleton >/dev/null \
    && aws s3api abort-multipart-upload --generate-cli-skeleton >/dev/null \
    && aws s3api list-multipart-uploads --generate-cli-skeleton >/dev/null \
    && aws sts get-caller-identity --generate-cli-skeleton >/dev/null \
    && aws sesv2 send-email --generate-cli-skeleton >/dev/null \
    && out="$(AWS_ACCESS_KEY_ID=AKIAEXAMPLEEXAMPLE00 AWS_SECRET_ACCESS_KEY=example AWS_MAX_ATTEMPTS=1 \
        aws ec2 describe-instances --region us-east-1 --endpoint-url http://127.0.0.1:9 \
        --cli-connect-timeout 2 --output json --no-cli-pager 2>&1 || true)" \
    && echo "$out" | grep -q 'Could not connect to the endpoint URL' \
    && out="$(AWS_ACCESS_KEY_ID=AKIAEXAMPLEEXAMPLE00 AWS_SECRET_ACCESS_KEY=example AWS_MAX_ATTEMPTS=1 \
        aws s3 ls s3://example-bucket/data/ --region us-east-1 --endpoint-url http://127.0.0.1:9 \
        --cli-connect-timeout 2 --no-cli-pager 2>&1 || true)" \
    && echo "$out" | grep -q 'Could not connect to the endpoint URL' \
    && du -sh /usr/local/aws-cli

# ---- RDS CA bundle stage ---------------------------------------------------
# The CA bundle that signs every RDS endpoint (all regions), for the programs'
# sslmode=verify-full connection. Pinned by its SHA-256 like the AWS CLI above:
# AWS adds a CA to it before rotating an endpoint's certificate, so a bump
# fetches the new bundle, checks it lists the old CAs and the new one, and
# records the new sum.
FROM ubuntu:24.04 AS rdsca

ARG RDS_CA_BUNDLE_SHA256=90a6f1a7978a1b1b096dca6c57511ca48d3b8cf052f8e704baf358f34ea2f21b

RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && curl -fsSL https://truststore.pki.rds.amazonaws.com/global/global-bundle.pem \
        -o /tmp/rds-global-bundle.pem \
    && echo "${RDS_CA_BUNDLE_SHA256}  /tmp/rds-global-bundle.pem" | sha256sum -c -

# ---- the runtime base, every program's --------------------------------------
# Same distro/glibc as the build stage so the binaries' GLIBC/GLIBCXX
# requirements resolve (a debian:12 runtime is too old for ubuntu:24.04's
# libstdc++).
FROM ubuntu:24.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl libpq5 \
    && rm -rf /var/lib/apt/lists/* \
    && for tool in sh curl base64 tr sed head mktemp; do \
        command -v "$tool" >/dev/null || { echo "the runtime image lacks $tool"; exit 1; }; \
    done

WORKDIR /app

COPY --from=build /src/backend/migrations /app/backend/migrations
COPY --from=build /src/backend/seed /app/backend/seed
COPY --from=rdsca /tmp/rds-global-bundle.pem /app/rds-global-bundle.pem

# The programs read these from the environment (program/server_config.cpp); the
# database's DATABASE_URL and DATABASE_PASSWORD come from the deployment.
# ONNET_HTTP_PORT is web's port and the one the gate and the lobby announce
# beside ONNET_WEB_HOST; ONNET_HEALTH_PORT the GET /livez port of every program
# but web, whose own is ONNET_HTTP_PORT.
ENV ONNET_GATE_UDP_PORT=7597 \
    ONNET_NW_UDP_PORT=64206 \
    ONNET_HTTP_PORT=8080 \
    ONNET_HEALTH_PORT=8081 \
    MIGRATIONS_DIR=/app/backend/migrations \
    SEED_DIR=/app/backend/seed

# Every program runs as UID 10002, an account of its own with no home and no
# shell: it keeps no state in the image (the database is PostgreSQL), writes
# only its temporary files (/tmp) and binds only ports above 1024.
RUN useradd --uid 10002 --user-group --no-create-home --home-dir /nonexistent \
        --shell /usr/sbin/nologin novaworld

# The local log store's mount point (the compose stack's `logs` volume,
# ONNET_LOG_STORE=file:///var/log/opennova): world-writable with the sticky bit,
# as /tmp, so a named volume first mounted here takes that mode and each
# program (UID 10002) and the local stack's machines (hostd, UID 10001) make
# their own directories in it. A deployment's logs are CloudWatch's: nothing
# mounts it there.
RUN install -d -m 1777 /var/log/opennova

# Run each program under an init as PID 1 (the ECS task definitions'
# initProcessEnabled, the compose files' `init: true`, or `docker run --init`):
# a second SIGTERM ends a stuck shutdown only when the program is not PID 1,
# which the kernel shields from default-action signals (program/program.cpp).

# The AWS CLI, at the absolute path the programs run it by: web's, the
# worker's and data's. The local store's mount point beside it (the compose stack's volume:
# a named volume takes the image directory's owner and mode on its first mount,
# so the program, UID 10002, can write it, and the local stack's machines, whose
# hostd runs as the serve image's UID 10001, can read the trees through it; a
# deployment's store is S3 and nothing mounts it), and the local stack's
# static-address zone file's (the worker writes it, the same way).
FROM runtime AS runtime-aws
COPY --from=awscli /usr/local/aws-cli /usr/local/aws-cli
RUN ln -s /usr/local/aws-cli/v2/current/bin/aws /usr/local/bin/aws && aws --version \
    && install -d -o 10002 -g 10002 -m 0755 /var/lib/opennova/store /var/lib/opennova/zone

# ---- gate ------------------------------------------------------------------
FROM runtime AS gate
COPY --from=build /src/build/apps/novaworld_gate/opennova-novaworld-gate /app/
RUN ! ldd /app/opennova-novaworld-gate | grep -q 'not found'
USER 10002
EXPOSE 7597/udp 8081/tcp
HEALTHCHECK --interval=10s --timeout=3s --start-period=60s --retries=3 \
    CMD curl -fsS --max-time 2 "http://127.0.0.1:${ONNET_HEALTH_PORT}/livez" || exit 1
ENTRYPOINT ["/app/opennova-novaworld-gate"]

# ---- lobby -----------------------------------------------------------------
FROM runtime AS lobby
COPY --from=build /src/build/apps/novaworld_lobby/opennova-novaworld-lobby /app/
RUN ! ldd /app/opennova-novaworld-lobby | grep -q 'not found'
USER 10002
EXPOSE 64206/udp 8081/tcp
HEALTHCHECK --interval=10s --timeout=3s --start-period=60s --retries=3 \
    CMD curl -fsS --max-time 2 "http://127.0.0.1:${ONNET_HEALTH_PORT}/livez" || exit 1
ENTRYPOINT ["/app/opennova-novaworld-lobby"]

# ---- web -------------------------------------------------------------------
# Web alone links hiredis (its Redis pool, the shared library's one hiredis
# user: scripts/lint/link_graph_check.py), so its image alone carries it. It
# runs the AWS CLI for the game data's store (the admin page's uploads, which
# it puts there part by part, and the update service's packages) and serves
# retail UPDATE.EXE on the updater's ports (ONNET_UPDATE_PORTS, with
# ONNET_UPDATE_HOST set).
FROM runtime-aws AS web
RUN apt-get update && apt-get install -y --no-install-recommends libhiredis1.1.0 \
    && rm -rf /var/lib/apt/lists/*
COPY --from=build /src/build/apps/novaworld_web/opennova-novaworld-web /app/
COPY --from=build /src/apps/novaworld_web/templates /app/templates
COPY --from=build /src/apps/novaworld_web/static /app/static
RUN ! ldd /app/opennova-novaworld-web | grep -q 'not found'
ENV TEMPLATES_DIR=/app/templates \
    STATIC_DIR=/app/static \
    ONNET_HOSTED_AWS_CLI=/usr/local/bin/aws
USER 10002
EXPOSE 8080/tcp 20044/tcp 20556/tcp 21068/tcp 21580/tcp
# GET /livez on its own HTTP port: the process's liveness (GET /healthz, the
# load balancer's, also asks Redis).
HEALTHCHECK --interval=10s --timeout=3s --start-period=60s --retries=3 \
    CMD curl -fsS --max-time 2 "http://127.0.0.1:${ONNET_HTTP_PORT}/livez" || exit 1
ENTRYPOINT ["/app/opennova-novaworld-web"]

# ---- fleet -----------------------------------------------------------------
FROM runtime AS fleet
COPY --from=build /src/build/apps/novaworld_fleet/opennova-novaworld-fleet /app/
RUN ! ldd /app/opennova-novaworld-fleet | grep -q 'not found'
USER 10002
EXPOSE 8070/tcp 8081/tcp
HEALTHCHECK --interval=10s --timeout=3s --start-period=60s --retries=3 \
    CMD curl -fsS --max-time 2 "http://127.0.0.1:${ONNET_HEALTH_PORT}/livez" || exit 1
ENTRYPOINT ["/app/opennova-novaworld-fleet"]

# ---- worker ----------------------------------------------------------------
FROM runtime-aws AS worker
COPY --from=build /src/build/apps/novaworld_worker/opennova-novaworld-worker /app/
COPY --from=build /src/apps/novaworld_common/hosted/cloud/machine/machine_user_data.sh /app/hosted/machine_user_data.sh
RUN ! ldd /app/opennova-novaworld-worker | grep -q 'not found'
ENV ONNET_HOSTED_MACHINE_SCRIPT=/app/hosted/machine_user_data.sh \
    ONNET_HOSTED_AWS_CLI=/usr/local/bin/aws
USER 10002
EXPOSE 8081/tcp
HEALTHCHECK --interval=10s --timeout=3s --start-period=60s --retries=3 \
    CMD curl -fsS --max-time 2 "http://127.0.0.1:${ONNET_HEALTH_PORT}/livez" || exit 1
ENTRYPOINT ["/app/opennova-novaworld-worker"]

# ---- data ------------------------------------------------------------------
# The data sets' jobs over the game data's store (S3 through the AWS CLI in a
# deployment; the compose stack's directory volume at /var/lib/opennova/store):
# a tree's files and a release's package pass through its disk.
FROM runtime-aws AS data
COPY --from=build /src/build/apps/novaworld_data/opennova-novaworld-data /app/
RUN ! ldd /app/opennova-novaworld-data | grep -q 'not found'
ENV ONNET_HOSTED_AWS_CLI=/usr/local/bin/aws
USER 10002
EXPOSE 8081/tcp
HEALTHCHECK --interval=10s --timeout=3s --start-period=60s --retries=3 \
    CMD curl -fsS --max-time 2 "http://127.0.0.1:${ONNET_HEALTH_PORT}/livez" || exit 1
ENTRYPOINT ["/app/opennova-novaworld-data"]

# ---- the local stack's smoke test ------------------------------------------
# No program and never published: the smoke test scripts/dev_stack.sh runs
# against a running local stack from the stack's own network
# (tests/novaworld/stack_smoke.cpp), and the synthetic data set (a loose
# deathmatch map and its game text) the stack's machines serve when no licensed
# data is given, written here at build time.
FROM build AS smoke-build
RUN cmake --build build --target opennova_novaworld_stack_smoke

FROM runtime AS smoke
COPY --from=smoke-build /src/build/tests/opennova-novaworld-stack-smoke /app/
RUN ! ldd /app/opennova-novaworld-stack-smoke | grep -q 'not found' \
    && /app/opennova-novaworld-stack-smoke data-set /app/synthetic-data
USER 10002
ENTRYPOINT ["/app/opennova-novaworld-stack-smoke"]
