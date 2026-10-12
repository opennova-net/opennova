# Multi-stage build of opennova-serve, the headless game server (ADR 0051), as
# the hosted game servers run it (ADR 0053). Build context is the repo root:
#   docker build -f apps/serve/Dockerfile -t opennova-serve .
#
# The image holds the server binary and nothing of the game: no retail byte,
# ever (ADR 0053 d3). The game data is a licensed Joint Operations install,
# mounted read-only at /opt/jo. The server keeps its files (game.cfg,
# admin.cfg, admin_log.txt, the ban lists, the logs) in its working directory
# (ADR 0051 d2), /srv/serve, a writable volume. A run:
#   docker run --rm --network host \
#     -v <JO install>:/opt/jo:ro -v <state dir>:/srv/serve \
#     ghcr.io/<owner>/opennova-serve /HOST /srv/serve/server.host [serve args]
# apps/serve/README.md ("Container image") has the contract.

# ---- build stage ----------------------------------------------------------
FROM ubuntu:24.04 AS build

RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY . .

# Configure the whole tree (cheap) but compile only the server target and its
# lib deps: the engine groups and apps/common, with no HTTP layer and no Godot.
# The configure is .github/workflows/net-linux.yml's, which builds and tests
# opennova_serve on Linux, minus its BUILD_NOVAWORLD_HTTP (serve never links
# the service).
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build --target opennova_serve

# ---- runtime stage --------------------------------------------------------
# Same distro/glibc as the build stage so the binary's GLIBC/GLIBCXX
# requirements resolve. The binary needs nothing past the base image's C and
# C++ runtimes: no TLS, no HTTP client (its NovaWorld login is plain HTTP).
FROM ubuntu:24.04 AS runtime

# A fixed non-root user, so a host directory mounted at /srv/serve can be
# handed to it by number (chown 10001:10001). A fresh named volume copies the
# image's ownership of /srv/serve and needs nothing. /opt/jo is only the mount
# point: the image ships it empty.
RUN groupadd --system --gid 10001 serve \
    && useradd --system --uid 10001 --gid serve --home-dir /srv/serve \
        --no-create-home --shell /usr/sbin/nologin serve \
    && mkdir -p /opt/jo /srv/serve \
    && chown serve:serve /srv/serve

COPY --from=build /src/build/apps/serve/opennova-serve /usr/local/bin/opennova-serve

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

# The install is a fixed mount, so the entrypoint names it; a later
# --resource-dir in the arguments wins, and --loose-root mounts a directory of
# loose files there. The default arguments serve the host file the state
# volume holds; any arguments given replace them, so they name /HOST too.
# serve installs its own SIGINT/SIGTERM handlers (a clean stop deregisters
# from NovaWorld), so it stops cleanly as PID 1 too; `docker run --init` (the
# compose files' `init: true`) works as for the NovaWorld server image.
ENTRYPOINT ["/usr/local/bin/opennova-serve", "--resource-dir", "/opt/jo"]
CMD ["/HOST", "/srv/serve/server.host"]
