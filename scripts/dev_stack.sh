#!/usr/bin/env bash
# The whole NovaWorld service on this machine, in Docker (deploy/compose): the
# five programs (gate, lobby, web, controller, worker), each its own container
# as a deployment runs each as its own ECS service, PostgreSQL, Valkey, the
# portal (prod's nginx image in web's network, as in web's ECS task) and, with
# a data set, hosted game servers on machines the worker launches as containers
# of the serve image (the real opennova-hostd and opennova-serve). The project
# is opennova-dev; its state (the compose env file, the synthetic data set) is
# build/dev-stack/.
#
#   scripts/dev_stack.sh up [options]   build the images and start the stack
#     --data <dir>          host game servers on this game folder (a licensed
#                           Joint Operations install; default: OPENNOVA_JO_DIR
#                           when it names a folder), its rotation --mission
#                           (default ASH_G1A.bms)
#     --data-set <slug>     the data set that folder is (default jop_2_consumer)
#     --synthetic-data      host on the synthetic data set instead (a loose
#                           deathmatch map, no licensed byte: what CI runs)
#     --no-hosting          no hosted servers, whatever OPENNOVA_JO_DIR says
#     --address <ipv4>      the address everything announces (default: this
#                           machine's LAN address); never loopback
#     --prebuilt <dir>      build the images from a Linux build tree's binaries
#                           (a configure of the service as net-linux.yml's)
#                           instead of compiling in Docker
#     --no-build            start the images already built
#     --vite                also run the site's Vite dev server on :5173
#   scripts/dev_stack.sh smoke          the smoke test against the running stack
#   scripts/dev_stack.sh status         the containers, the machines among them
#   scripts/dev_stack.sh logs [args]    docker compose logs (e.g. -f novaworld-web)
#   scripts/dev_stack.sh down           stop and remove it (the database stays)
#   scripts/dev_stack.sh reset          down, the database and the state too
#
# DEVELOPING.md "Run the NovaWorld servers locally" has the rest: the ports, the
# dev accounts, pointing a retail client at it with the OpenNova Switcher.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
compose_dir="$repo/deploy/compose"
state="$repo/build/dev-stack"
env_file="$state/stack.env"
project="opennova-dev"
deployment="dev"
admin_token="dev-admin-token"
site_port=8000
usage="usage: scripts/dev_stack.sh up [--data <dir>] [--data-set <slug>] [--mission <name.bms>]
           [--synthetic-data] [--no-hosting] [--address <ipv4>] [--prebuilt <build dir>]
           [--no-build] [--vite]
       scripts/dev_stack.sh smoke | status | logs [args] | down | reset"

die() { echo "dev_stack: $*" >&2; exit 1; }
say() { echo "dev_stack: $*"; }

# A host path as docker and compose take it: Windows' form under Git Bash.
host_path() {
    if command -v cygpath > /dev/null 2>&1; then cygpath -m "$1"; else printf '%s\n' "$1"; fi
}

# This machine's LAN IPv4: the source address of its default route.
lan_address() {
    local addr=""
    if command -v powershell.exe > /dev/null 2>&1; then
        addr="$(powershell.exe -NoProfile -Command \
            "(Get-NetIPConfiguration | Where-Object { \$_.IPv4DefaultGateway -ne \$null -and \$_.NetAdapter.Status -eq 'Up' } | Select-Object -First 1).IPv4Address.IPAddress" \
            2> /dev/null | tr -d '\r' | head -n 1)"
    elif command -v ip > /dev/null 2>&1; then
        addr="$(ip -4 route get 1.1.1.1 2> /dev/null | sed -n 's/.* src \([0-9.]*\).*/\1/p' | head -n 1)"
    elif command -v ipconfig > /dev/null 2>&1; then
        addr="$(ipconfig getifaddr en0 2> /dev/null || true)"
    fi
    printf '%s\n' "$addr"
}

# The value of NAME in the state's env file ("" when unset).
state_value() {
    [[ -f "$env_file" ]] || return 0
    sed -n "s/^$1=//p" "$env_file" | head -n 1 | sed "s/^'\(.*\)'\$/\1/"
}

# docker compose over the stack's files (the hosting overlay when the state says
# hosting is on), its env file, its project.
stack_compose() {
    local dir
    dir="$(host_path "$compose_dir")"
    local files=(-f "$dir/docker-compose.yml" -f "$dir/docker-compose.dev.yml")
    [[ "$(state_value STACK_HOSTING)" == "1" ]] && files+=(-f "$dir/docker-compose.hosting.yml")
    MSYS_NO_PATHCONV=1 docker compose --project-name "$project" --project-directory "$dir" \
        --env-file "$(host_path "$env_file")" "${files[@]}" "$@"
}

# The hosted machines the worker launched: no compose service of their own.
machine_ids() {
    docker ps -aq --filter "label=opennova.hosted=1" --filter "label=opennova.deployment=$deployment"
}

remove_machines() {
    local ids
    ids="$(machine_ids)"
    if [[ -n "$ids" ]]; then
        # shellcheck disable=SC2086 # one id per word
        docker rm -f -v $ids > /dev/null
        say "removed the hosted machines"
    fi
}

# Refuses ports another container holds (another stack, an old one).
check_ports() {
    local port holders
    for port in 7597 64206 8080 "$site_port"; do
        holders="$(docker ps --format '{{.Names}} {{.Ports}}' | grep -E "[:.]${port}->" \
            | grep -v "^${project}-" | cut -d' ' -f1 || true)"
        if [[ -n "$holders" ]]; then
            die "port $port is published by $(echo "$holders" | tr '\n' ' ')- stop that (docker stop <name>) and try again"
        fi
    done
}

# The image build context of a Linux build tree's binaries (--prebuilt): the
# files the Dockerfiles' `build` and `smoke-build` stages hold, laid out as
# there (/src/...), so the runtime stages are built from them unchanged.
stage_prebuilt() {
    local tree="$1" ctx="$state/prebuilt"
    local program
    rm -rf "$ctx"
    for program in gate lobby web controller worker; do
        mkdir -p "$ctx/src/build/apps/novaworld_$program"
        cp "$tree/apps/novaworld_$program/opennova-novaworld-$program" "$ctx/src/build/apps/novaworld_$program/"
    done
    mkdir -p "$ctx/src/build/apps/serve" "$ctx/src/build/apps/hostd" "$ctx/src/build/tests" \
        "$ctx/src/backend" "$ctx/src/apps/novaworld_web" "$ctx/src/apps/novaworld_common/hosted/machine"
    cp "$tree/apps/serve/opennova-serve" "$ctx/src/build/apps/serve/"
    cp "$tree/apps/hostd/opennova-hostd" "$ctx/src/build/apps/hostd/"
    cp "$tree/tests/opennova-novaworld-stack-smoke" "$ctx/src/build/tests/"
    cp -r "$repo/backend/migrations" "$repo/backend/seed" "$ctx/src/backend/"
    cp -r "$repo/apps/novaworld_web/templates" "$repo/apps/novaworld_web/static" "$ctx/src/apps/novaworld_web/"
    cp "$repo/apps/novaworld_common/hosted/machine/machine_user_data.sh" \
        "$ctx/src/apps/novaworld_common/hosted/machine/"
    printf '%s\n' "$ctx"
}

# The programs' images, the portal's and the smoke test's (compiled in Docker,
# or from --prebuilt binaries, which builds the machines' serve image too).
build_images() {
    local prebuilt="$1"
    if [[ -z "$prebuilt" ]]; then
        say "building the images (the five programs, the portal, the smoke test)"
        stack_compose --profile smoke build
        return
    fi
    local ctx target
    ctx="$(host_path "$(stage_prebuilt "$prebuilt")")"
    say "building the images from $prebuilt's binaries"
    for target in gate lobby web controller worker smoke; do
        local tag="opennova-novaworld-$target:local"
        MSYS_NO_PATHCONV=1 docker build -q -f "$(host_path "$repo/apps/novaworld_common/Dockerfile")" \
            --target "$target" --build-context "build=$ctx" --build-context "smoke-build=$ctx" \
            -t "$tag" "$(host_path "$repo")" > /dev/null
        say "  $tag"
    done
    MSYS_NO_PATHCONV=1 docker build -q -f "$(host_path "$repo/apps/serve/Dockerfile")" \
        --build-context "build=$ctx" -t opennova-serve:local "$(host_path "$repo")" > /dev/null
    say "  opennova-serve:local"
    MSYS_NO_PATHCONV=1 docker build -q -t opennova-portal:local "$(host_path "$repo/web")" > /dev/null
    say "  opennova-portal:local"
}

# The synthetic data set, out of the smoke image (written there at its build).
synthetic_data() {
    local dir="$state/synthetic-data" cid
    rm -rf "$dir"
    mkdir -p "$dir"
    cid="$(docker create opennova-novaworld-smoke:local)"
    MSYS_NO_PATHCONV=1 docker cp "$cid:/app/synthetic-data/." "$(host_path "$dir")" > /dev/null
    docker rm "$cid" > /dev/null
    printf '%s\n' "$dir"
}

cmd_up() {
    local data="" data_set="jop_2_consumer" mission="" synthetic=0 hosting_off=0 address=""
    local prebuilt="" build=1 vite=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --data) [[ $# -ge 2 ]] || die "$usage"; data="$2"; shift 2 ;;
            --data-set) [[ $# -ge 2 ]] || die "$usage"; data_set="$2"; shift 2 ;;
            --mission) [[ $# -ge 2 ]] || die "$usage"; mission="$2"; shift 2 ;;
            --synthetic-data) synthetic=1; shift ;;
            --no-hosting) hosting_off=1; shift ;;
            --address) [[ $# -ge 2 ]] || die "$usage"; address="$2"; shift 2 ;;
            --prebuilt) [[ $# -ge 2 ]] || die "$usage"; prebuilt="$2"; shift 2 ;;
            --no-build) build=0; shift ;;
            --vite) vite=1; shift ;;
            *) die "$usage" ;;
        esac
    done
    command -v docker > /dev/null || die "docker is not on PATH (Docker Desktop on Windows and macOS)"
    docker info > /dev/null 2>&1 || die "the docker daemon does not answer (is Docker Desktop running?)"

    [[ -n "$address" ]] || address="$(lan_address)"
    [[ "$address" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] \
        || die "no LAN address found; name the one to announce with --address <ipv4>"
    [[ "$address" != 127.* ]] || die "--address cannot be loopback: the stack's containers reach it too"

    # The data set: --data, else OPENNOVA_JO_DIR (a retail install), else none.
    if [[ "$hosting_off" == 0 && "$synthetic" == 0 && -z "$data" && -n "${OPENNOVA_JO_DIR:-}" \
            && -d "${OPENNOVA_JO_DIR:-}" ]]; then
        data="$OPENNOVA_JO_DIR"
    fi
    [[ "$hosting_off" == 0 ]] || { data=""; synthetic=0; }
    local hosting=0
    if [[ "$synthetic" == 1 || -n "$data" ]]; then hosting=1; fi
    if [[ -n "$data" && ! -d "$data" ]]; then die "--data $data is no folder"; fi
    [[ "$data_set" =~ ^[a-z0-9_]+$ ]] || die "--data-set takes a data set slug (jop_2_consumer)"

    mkdir -p "$state"
    # The env file compose reads (its values, never this shell's): the stack
    # without hosting first, which builds the images.
    {
        echo "ONNET_PUBLIC_HOST=$address"
        echo "ADMIN_API_TOKEN=$admin_token"
        echo "STACK_SITE_PORT=$site_port"
        echo "STACK_HOSTING=0"
    } > "$env_file"
    check_ports
    [[ "$build" == 0 ]] || build_images "$prebuilt"

    if [[ "$hosting" == 1 ]]; then
        local dir sets
        if [[ "$synthetic" == 1 ]]; then
            # Out of the smoke image, built above.
            dir="$(synthetic_data)"
            data_set="jop_2_consumer"
            sets="{\"jop_2_consumer\":{\"loose_root\":true,\"missions\":[\"${mission:-servetst.bms}\"]}}"
        else
            dir="$data"
            sets="{\"$data_set\":{\"missions\":[\"${mission:-ASH_G1A.bms}\"]}}"
        fi
        sed -i.bak 's/^STACK_HOSTING=0$/STACK_HOSTING=1/' "$env_file" && rm -f "$env_file.bak"
        {
            echo "STACK_DATA_DIR='$(host_path "$dir")'"
            echo "STACK_DATA_SET=$data_set"
            echo "STACK_DATA_SETS='$sets'"
        } >> "$env_file"
        # The serve image the machines run (--prebuilt built it above).
        if [[ "$build" == 1 && -z "$prebuilt" ]]; then
            say "building the machines' serve image"
            stack_compose --profile machines build machine-image
        fi
    fi

    say "starting the stack (project $project)"
    local profiles=()
    [[ "$vite" == 1 ]] && profiles=(--profile vite)
    stack_compose "${profiles[@]}" up -d --no-build --remove-orphans --wait --wait-timeout 300 \
        || { stack_compose ps; die "the stack did not come up healthy (scripts/dev_stack.sh logs)"; }

    cat << EOF

The local NovaWorld stack runs (project $project).
  site            http://localhost:$site_port  (the portal; its API at /api)
  site admin      log in as test / test (the seeded dev admin); /admin pages' basic auth admin / dev
  admin API       Authorization: Bearer $admin_token
  retail client   the OpenNova Switcher: Custom address... $address
                  (or the hosts line: $address gs.novaworld.net); allow UDP 7597 and 64206
  dev accounts    test/test, foo/foo, test1..3/test
  announced at    $address (gate udp 7597, lobby udp 64206, web http 8080)
EOF
    if [[ "$hosting" == 1 ]]; then
        echo "  hosting         on: data set $data_set from $( [[ "$synthetic" == 1 ]] && echo "the synthetic data set" || echo "$data")"
    else
        echo "  hosting         off: no data set (up --data <install> or --synthetic-data)"
    fi
    [[ "$vite" == 1 ]] && echo "  vite            http://localhost:5173 (hot reload of web/)"
    echo "  smoke test      scripts/dev_stack.sh smoke"
}

cmd_smoke() {
    [[ -f "$env_file" ]] || die "no stack (scripts/dev_stack.sh up first)"
    local args=(run --web novaworld-web:8080 --site novaworld-web:80 --gate novaworld-gate:7597
        --livez novaworld-gate:8081 --livez novaworld-lobby:8081
        --livez novaworld-controller:8081 --livez novaworld-worker:8081
        --admin-token "$(state_value ADMIN_API_TOKEN)" --timeout 300)
    if [[ "$(state_value STACK_HOSTING)" == "1" ]]; then
        args+=(--hosting "$(state_value STACK_DATA_SET)")
    fi
    local rc=0
    stack_compose --profile smoke run --rm --no-deps smoke "${args[@]}" || rc=$?
    if [[ "$rc" != 0 ]]; then
        echo
        say "the smoke test failed; the last lines of each program (and machine):"
        stack_compose logs --no-color --tail 40 novaworld-gate novaworld-lobby novaworld-web \
            novaworld-controller novaworld-worker || true
        local id
        for id in $(machine_ids); do
            echo "---- machine $(docker inspect --format '{{.Name}}' "$id")"
            docker logs --tail 60 "$id" 2>&1 || true
        done
    fi
    return "$rc"
}

cmd_status() {
    [[ -f "$env_file" ]] || die "no stack (scripts/dev_stack.sh up first)"
    stack_compose ps
    local ids
    ids="$(machine_ids)"
    if [[ -n "$ids" ]]; then
        echo "hosted machines:"
        # shellcheck disable=SC2086 # one id per word
        docker ps -a --filter "label=opennova.deployment=$deployment" --filter "label=opennova.hosted=1" \
            --format '  {{.Names}}  {{.Status}}'
    fi
}

cmd_down() {
    local volumes=("$@")
    # The worker first: a launch it has under way could make a machine after
    # the removal below, which would hold the network compose removes.
    # A stop that fails (an env file an interrupted up left incomplete) must
    # not keep the machines from going.
    [[ ! -f "$env_file" ]] || stack_compose stop novaworld-worker || true
    remove_machines
    if [[ -f "$env_file" ]]; then
        stack_compose --profile smoke --profile vite down --remove-orphans "${volumes[@]}"
    fi
}

[[ $# -ge 1 ]] || die "$usage"
action="$1"; shift
case "$action" in
    up) cmd_up "$@" ;;
    smoke) cmd_smoke ;;
    status) cmd_status ;;
    logs) stack_compose logs "$@" ;;
    down) cmd_down ;;
    reset)
        cmd_down -v
        rm -rf "$state"
        say "the stack's database and state are gone"
        ;;
    -h|--help) echo "$usage" ;;
    *) die "$usage" ;;
esac
