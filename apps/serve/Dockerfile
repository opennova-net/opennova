# Multi-stage build of opennova-serve, the headless game server (ADR 0051), as
# the hosted game servers run it (ADR 0053). Build context is the repo root:
#   docker build -f apps/serve/Dockerfile -t opennova-serve .
#
# The image holds the server binary and nothing of the game: no retail byte,
# ever (ADR 0053 d3). The game data, a data set (a licensed Joint Operations
# or Delta Force Xtreme 2 install, or OpenNova's own data), is mounted
# read-only at /opt/game, and `/game <code>` in the arguments names its title.
# The server keeps its files (game.cfg, admin.cfg, admin_log.txt, the ban
# lists, the logs) in its working directory (ADR 0051 d2), /srv/serve, a
# writable volume. A run:
#   docker run --rm --network host \
#     -v <game folder>:/opt/game:ro -v <state dir>:/srv/serve \
#     ghcr.io/<owner>/opennova-serve /HOST /srv/serve/server.host [serve args]
# apps/serve/README.md ("Container image") has the contract.
#
# Beside the server it carries opennova-hostd (apps/hostd), the machine agent,
# at /usr/local/bin/opennova-hostd. That one never runs in a container: a
# machine copies it out of the image it pulled at its bootstrap and runs it as
# root under systemd, so each machine's agent is the build of the serve image
# it was launched with (apps/hostd/README.md).

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
# the service; the agent compiles the service's agent protocol itself).
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
        "-DOPENNOVA_HOSTD_VERSION=${OPENNOVA_VERSION}" \
    && cmake --build build --target opennova_serve opennova_hostd

# ---- runtime stage --------------------------------------------------------
# Same distro/glibc as the build stage so the binary's GLIBC/GLIBCXX
# requirements resolve. The binary needs nothing past the base image's C and
# C++ runtimes: no TLS and no HTTP client library (curl); its NovaWorld login
# is plain HTTP over its own sockets (apps/common/net_http).
FROM ubuntu:24.04 AS runtime

# A fixed non-root user, so a host directory mounted at /srv/serve can be
# handed to it by number (chown 10001:10001). A fresh named volume copies the
# image's ownership of /srv/serve and needs nothing. /opt/game ships empty and
# owned by that user: a deployment's machine binds a tree over it read-only,
# while the local stack's machine (this image, running hostd itself) fetches
# its trees into it (--trees-root /opt/game, apps/hostd/README.md).
RUN groupadd --system --gid 10001 serve \
    && useradd --system --uid 10001 --gid serve --home-dir /srv/serve \
        --no-create-home --shell /usr/sbin/nologin serve \
    && mkdir -p /opt/game /srv/serve \
    && chown serve:serve /opt/game /srv/serve

COPY --from=build /src/build/apps/serve/opennova-serve /usr/local/bin/opennova-serve
# The machine agent, for the machine to copy out (never run here).
COPY --from=build /src/build/apps/hostd/opennova-hostd /usr/local/bin/opennova-hostd

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
