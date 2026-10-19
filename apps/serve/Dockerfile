# Multi-stage build of opennova-serve, the headless game server (ADR 0051), as
# the hosted game servers run it (ADR 0053), and of opennova-hostd, the
# machine agent that runs them (apps/hostd). Build context is the repo root.
# Two images, two targets:
#
#   serve   the server alone (the default target: the last stage)
#             docker build -f apps/serve/Dockerfile -t opennova-serve .
#   hostd   the agent, with the server beside it (the local stack's machines
#           run their servers as its children), the docker CLI it starts the
#           servers' containers with, the AWS CLI it fetches the trees with and
#           nft for the slots' fences and counters: each cloud machine's ECS
#           daemon, root on the machine's network (apps/hostd/README.md)
#             docker build -f apps/serve/Dockerfile --target hostd -t opennova-hostd .
#
# Neither holds anything of the game: no retail byte, ever (ADR 0053 d3). The
# game data, a data set (a licensed Joint Operations or Delta Force Xtreme 2
# install, or OpenNova's own data), is mounted read-only at /opt/game, and
# `/game <code>` in the arguments names its title. The server keeps its files
# (game.cfg, admin.cfg, admin_log.txt, the ban lists, the logs) in its working
# directory (ADR 0051 d2), /srv/serve, a writable volume. A run:
#   docker run --rm --network host \
#     -v <game folder>:/opt/game:ro -v <state dir>:/srv/serve \
#     ghcr.io/<owner>/opennova-serve /HOST /srv/serve/server.host [serve args]
# apps/serve/README.md ("Container image") has the contract.

# ---- build stage ----------------------------------------------------------
FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# The build opennova-hostd reports to the service (serve-image.yml passes the
# commit).
ARG OPENNOVA_VERSION=dev

# Configure the whole tree (cheap) but compile only the server and the agent
# targets and their lib deps: the engine groups and apps/common, with no HTTP
# layer and no Godot. The configure is .github/workflows/net-linux.yml's, which
# builds and tests both on Linux, minus its NovaWorld service flags (neither links
# the service; the agent links the service's core and agents' protocol, which build
# without it on Linux and need no libpq).
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        "-DOPENNOVA_HOSTD_VERSION=${OPENNOVA_VERSION}" \
    && cmake --build build --target opennova_serve opennova_hostd

# ---- the docker CLI stage ---------------------------------------------------
# The static docker CLI hostd runs (the client alone, no daemon: it talks to the
# machine's docker over the socket the task mounts; it negotiates the daemon's
# API version down). A pinned release, checked against its SHA-256 before it is
# unpacked; a bump moves both ARGs (fetch the new tgz, record its sum).
FROM ubuntu:24.04 AS dockercli

ARG DOCKER_VERSION=28.5.2
ARG DOCKER_SHA256=ea90cfd12e1eeb12aa1c971741adb8bd4ed88e2a574eaac13f5029a1dbc6300d

RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates curl \
    && rm -rf /var/lib/apt/lists/* \
    && curl -fsSL "https://download.docker.com/linux/static/stable/x86_64/docker-${DOCKER_VERSION}.tgz" \
        -o /tmp/docker.tgz \
    && echo "${DOCKER_SHA256}  /tmp/docker.tgz" | sha256sum -c - \
    && tar -xzf /tmp/docker.tgz -C /tmp docker/docker \
    && install -m 0755 /tmp/docker/docker /usr/local/bin/docker \
    && /usr/local/bin/docker --version

# ---- the AWS CLI stage ------------------------------------------------------
# The AWS CLI v2 hostd fetches each tree with (`aws s3 sync`, tree_store.h),
# its credentials the hostd task role's: the release the service's images
# carry, checked against the same SHA-256 (apps/novaworld_common/Dockerfile
# says where the sum comes from; a bump moves both files' ARGs together).
# Its s3 model alone stays (and sts, for a credentials check by hand).
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
    && dist=/usr/local/aws-cli/v2/current/dist \
    && data="$dist/awscli/botocore/data" \
    && test -d "$data/s3" && test -d "$data/sts" && test -f "$data/endpoints.json" \
    && rm -rf "$dist/awscli/examples" "$dist/awscli/data/ac.index" "$dist/aws_completer" \
        /usr/local/aws-cli/v2/current/bin/aws_completer /usr/local/bin/aws_completer \
    && find "$data" -mindepth 1 -maxdepth 1 -type d ! -name s3 ! -name sts -exec rm -rf {} + \
    && out="$(AWS_ACCESS_KEY_ID=AKIAEXAMPLEEXAMPLE00 AWS_SECRET_ACCESS_KEY=example AWS_MAX_ATTEMPTS=1 \
        /usr/local/bin/aws s3 sync s3://example-bucket/data/trees/1/ /tmp/tree --region us-east-1 \
        --endpoint-url http://127.0.0.1:9 --cli-connect-timeout 2 --no-progress 2>&1 || true)" \
    && echo "$out" | grep -q 'Could not connect to the endpoint URL'

# ---- the server's runtime ---------------------------------------------------
# Same distro/glibc as the build stage so the binary's GLIBC/GLIBCXX
# requirements resolve. The binary needs nothing past the base image's C and
# C++ runtimes: no TLS and no HTTP client library (curl); its NovaWorld login
# is plain HTTP over its own sockets (apps/common/net_http).
FROM ubuntu:24.04 AS runtime

# A fixed non-root user, so a host directory mounted at /srv/serve can be
# handed to it by number (chown 10001:10001). A fresh named volume copies the
# image's ownership of /srv/serve and needs nothing. /opt/game ships empty and
# owned by that user: a deployment's machine binds a tree over it read-only,
# while the local stack's machine (the hostd image, running hostd itself as
# this user) fetches its trees into it (--trees-root /opt/game,
# apps/hostd/README.md).
RUN groupadd --system --gid 10001 serve \
    && useradd --system --uid 10001 --gid serve --home-dir /srv/serve \
        --no-create-home --shell /usr/sbin/nologin serve \
    && mkdir -p /opt/game /srv/serve \
    && chown serve:serve /opt/game /srv/serve

COPY --from=build /src/build/apps/serve/opennova-serve /usr/local/bin/opennova-serve

# ---- hostd: the machine agent's image -------------------------------------
# Root (the fleet cluster's task definition runs it as user 0 with NET_ADMIN
# added: the slots' users' fences, the containers through the machine's docker
# socket, the slots' directories and the trees under the paths the task binds
# at the machine's own). The local stack's machines start it as the serve user
# (docker_machine_driver.h), its servers its own children.
FROM runtime AS hostd

RUN apt-get update && apt-get install -y --no-install-recommends ca-certificates nftables \
    && rm -rf /var/lib/apt/lists/* \
    && command -v nft >/dev/null

COPY --from=build /src/build/apps/hostd/opennova-hostd /usr/local/bin/opennova-hostd
COPY --from=dockercli /usr/local/bin/docker /usr/bin/docker
COPY --from=awscli /usr/local/aws-cli /usr/local/aws-cli
RUN ln -s /usr/local/aws-cli/v2/current/bin/aws /usr/local/bin/aws && aws --version \
    && /usr/local/bin/opennova-hostd --version

USER root
WORKDIR /
ENTRYPOINT ["/usr/local/bin/opennova-hostd"]

# ---- serve: the server alone (the default target) ----------------------------
FROM runtime AS serve

USER 10001:10001
WORKDIR /srv/serve
VOLUME ["/srv/serve"]

# Documentation only; the machine's security group is the restriction. UDP:
# game.cfg's stock LAN server window (mplanserverportmin..max), whose head is
# also the NovaWorld window's (a hosted server pins mpnovaworldportmin and
# mpnovaworldportmax both to 32768, so one port). TCP 32800: the hosted
# servers' remote_admin_port (game.cfg's stock 0 opens no admin port), reachable
# only from the NovaWorld service.
EXPOSE 32768-32787/udp 32800/tcp

# The game folder is a fixed mount, so the entrypoint names it; a later
# --resource-dir in the arguments wins, and --loose-root mounts a directory of
# loose files there. The default arguments serve the host file the state
# volume holds; any arguments given replace them, so they name /HOST too.
# serve installs its own SIGINT/SIGTERM handlers (a clean stop deregisters
# from NovaWorld), so it stops cleanly as PID 1 too; `docker run --init` (the
# compose files' `init: true`) works as for the NovaWorld programs' images.
ENTRYPOINT ["/usr/local/bin/opennova-serve", "--resource-dir", "/opt/game"]
CMD ["/HOST", "/srv/serve/server.host"]
