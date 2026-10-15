# Multi-stage build of the standalone NovaWorld server (gate UDP, NW UDP,
# HTTP via Crow). Build context is the repo root:
#   docker build -f apps/novaworld_server/Dockerfile -t opennova-novaworld-server .
#
# The runtime image needs only the server binary, libpq and its data dirs
# (migrations, seed, retail templates/static), and for the hosted servers' ec2
# machine driver the AWS CLI v2 and the machine script. It keeps no state: the
# database is PostgreSQL (DATABASE_URL, a libpq URI, and DATABASE_PASSWORD
# beside it; apps/novaworld_server/README.md). Against RDS the URI carries
# sslmode=verify-full&sslrootcert=/app/rds-global-bundle.pem, the RDS CA bundle
# this image pins. The web portal is a separate nginx container.

# ---- build stage ----------------------------------------------------------
FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates libpq-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Configure the whole tree (cheap) but compile only the server target and
# its lib deps. OPENNOVA_NOVAWORLD_SERVICE=ON defines the service and the
# server app; BUILD_NOVAWORLD_HTTP=ON pulls Crow + standalone Asio via
# FetchContent. GDExtension is never touched (godot/src is its own
# project). Mirrors the flags of ci.yml's `test-linux` job and of
# .github/workflows/net-linux.yml.
RUN cmake -S . -B build -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DOPENNOVA_NOVAWORLD_SERVICE=ON \
        -DBUILD_NOVAWORLD_HTTP=ON \
    && cmake --build build --target opennova_novaworld_server

# ---- AWS CLI stage ---------------------------------------------------------
# The hosted servers' ec2 machine driver (hosted/ec2_machine_driver.h) runs the
# AWS CLI v2, its credentials the service instance's role (IMDSv2). A pinned release,
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

# The driver calls ec2 alone (sts for a credentials check by hand): the help
# examples, the completer and its index, and every other service's model go
# (about 160 MB of the CLI's 270). The models' root files (endpoints.json,
# partitions.json) stay. Then the check: both services' commands still build,
# and a real describe-instances runs to its HTTP request (fake keys, a closed
# local port, so it can reach nothing).
RUN dist=/usr/local/aws-cli/v2/current/dist \
    && data="$dist/awscli/botocore/data" \
    && test -d "$data/ec2" && test -d "$data/sts" && test -f "$data/endpoints.json" \
    && rm -rf "$dist/awscli/examples" "$dist/awscli/data/ac.index" "$dist/aws_completer" \
        /usr/local/aws-cli/v2/current/bin/aws_completer /usr/local/bin/aws_completer \
    && find "$data" -mindepth 1 -maxdepth 1 -type d ! -name ec2 ! -name sts -exec rm -rf {} + \
    && aws ec2 describe-instances --generate-cli-skeleton >/dev/null \
    && aws ec2 run-instances --generate-cli-skeleton >/dev/null \
    && aws ec2 terminate-instances --generate-cli-skeleton >/dev/null \
    && aws sts get-caller-identity --generate-cli-skeleton >/dev/null \
    && out="$(AWS_ACCESS_KEY_ID=AKIAEXAMPLEEXAMPLE00 AWS_SECRET_ACCESS_KEY=example AWS_MAX_ATTEMPTS=1 \
        aws ec2 describe-instances --region us-east-1 --endpoint-url http://127.0.0.1:9 \
        --cli-connect-timeout 2 --output json --no-cli-pager 2>&1 || true)" \
    && echo "$out" | grep -q 'Could not connect to the endpoint URL' \
    && du -sh /usr/local/aws-cli

# ---- RDS CA bundle stage ---------------------------------------------------
# The CA bundle that signs every RDS endpoint (all regions), for the service's
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

# ---- runtime stage --------------------------------------------------------
# Same distro/glibc as the build stage so the binary's GLIBC/GLIBCXX
# requirements resolve (a debian:12 runtime is too old for ubuntu:24.04's
# libstdc++).
FROM ubuntu:24.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates curl libpq5 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

COPY --from=build /src/build/apps/novaworld_server/opennova-novaworld-server /app/opennova-novaworld-server
COPY --from=build /src/backend/migrations /app/backend/migrations
COPY --from=build /src/backend/seed /app/backend/seed
COPY --from=build /src/apps/novaworld_server/templates /app/templates
COPY --from=build /src/apps/novaworld_server/static /app/static
# The ec2 machine driver's machine script (the cloud machines' user-data
# template, which installs opennova-hostd) and the AWS CLI it runs, at the
# absolute path it runs it by.
COPY --from=build /src/apps/novaworld_server/hosted/machine/machine_user_data.sh /app/hosted/machine_user_data.sh
COPY --from=awscli /usr/local/aws-cli /usr/local/aws-cli
RUN ln -s /usr/local/aws-cli/v2/current/bin/aws /usr/local/bin/aws && aws --version
COPY --from=rdsca /tmp/rds-global-bundle.pem /app/rds-global-bundle.pem

# The server reads these paths from the environment (server_config.cpp); the
# database's DATABASE_URL and DATABASE_PASSWORD come from the deployment.
ENV ONNET_GATE_UDP_PORT=7597 \
    ONNET_NW_UDP_PORT=64206 \
    ONNET_HTTP_PORT=8080 \
    MIGRATIONS_DIR=/app/backend/migrations \
    SEED_DIR=/app/backend/seed \
    TEMPLATES_DIR=/app/templates \
    STATIC_DIR=/app/static \
    ONNET_HOSTED_MACHINE_SCRIPT=/app/hosted/machine_user_data.sh \
    ONNET_HOSTED_AWS_CLI=/usr/local/bin/aws

EXPOSE 7597/udp 64206/udp 8080/tcp

# The server runs as UID 10002, an account of its own with no home and no
# shell: it keeps no state in the image (the database is PostgreSQL), writes
# only its temporary files (/tmp, a tmpfs in the compose files) and binds only
# ports above 1024. The number is fixed so a host's firewall rules can name it.
RUN useradd --uid 10002 --user-group --no-create-home --home-dir /nonexistent \
        --shell /usr/sbin/nologin novaworld
USER 10002

# GET /healthz: "ok", or 503 once the stall watchdog finds a loop stuck (the
# process then exits 70 and the runtime starts it again). Outside /api, so
# ONNET_API_PROXY_ONLY never refuses it.
HEALTHCHECK --interval=30s --timeout=3s --start-period=10s --retries=3 \
    CMD curl -fsS http://127.0.0.1:8080/healthz || exit 1

# Run it under an init as PID 1 (the compose files' `init: true`, or `docker
# run --init`): a second SIGTERM ends a stuck shutdown only when the server is
# not PID 1, which the kernel shields from default-action signals (main.cpp).
ENTRYPOINT ["/app/opennova-novaworld-server"]
