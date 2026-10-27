#!/usr/bin/env bash
# The whole NovaWorld service on this machine, in Docker (deploy/compose): the
# six programs (gate, lobby, web, fleet, worker, data), each its own container
# as a deployment runs each as its own ECS service, PostgreSQL, Valkey, the
# portal (prod's nginx image in web's network, as in web's ECS task) and, with
# a game install, hosted game servers on machines the worker launches as
# containers of the hostd image (the real opennova-hostd and opennova-serve).
# The project is opennova-dev; its state (the compose env file, the synthetic
# install, the project made from an install) is build/dev-stack/.
#
#   scripts/dev_stack.sh up [options]   build the images and start the stack
#     --data <dir>          host game servers on this game install (a licensed
#                           one of the game --game names): once the stack is up,
#                           the game's project is made from it (opennova-project's
#                           new, import --all and create-missing, in the data
#                           program's image), pushed through the editor's routes
#                           as the dev admin, and a release built from that
#                           revision, validated and published, out of which
#                           release.apply makes the tree the servers run; the
#                           rotation is the data's multiplayer missions
#     --game <key>          that game, by its project key (default JO; DFX2,
#                           ACIDGAMBIT)
#     --mission <name.bms>  the smoke test's server runs this mission of the
#                           data (default: the data's whole rotation)
#     --synthetic-data      host on the synthetic install instead (one
#                           deathmatch map packed into the three archives, no
#                           licensed byte: what CI runs), as JO's project
#     --no-hosting          no hosted servers, whatever --data or
#                           --synthetic-data say
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
#   scripts/dev_stack.sh reset          down, the database, the store, the logs and the
#                                       state too
#
# Every program and machine also writes its log into the stack's log store (the
# `logs` volume, ONNET_LOG_STORE=file:///var/log/opennova), which the site's
# admin pages read (GET /api/admin/logs) as a deployment's read CloudWatch.
#
# Seeding a game needs curl and jq on this machine (the push is the editor's:
# the files the store lacks, each PUT through the site, then the manifest as a
# revision; the release's build and publish are the admin API's). Nothing is
# uploaded by hand: a hosted server's data is the game's published release.
# DEVELOPING.md "Run the NovaWorld servers locally" has the rest: the ports, the
# dev accounts, pointing a retail client at it with a hosts-file line.
set -euo pipefail

repo="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
compose_dir="$repo/deploy/compose"
state="$repo/build/dev-stack"
env_file="$state/stack.env"
project="opennova-dev"
deployment="dev"
admin_token="dev-admin-token"
site_port=8000
# The data program's image, which carries opennova-project (its release
# builds' export): seeding makes a game's project in a container of it.
data_image="opennova-novaworld-data:local"
usage="usage: scripts/dev_stack.sh up [--data <dir>] [--game <key>] [--mission <name.bms>]
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
    for program in gate lobby web fleet worker data; do
        mkdir -p "$ctx/src/build/apps/novaworld_$program"
        cp "$tree/apps/novaworld_$program/opennova-novaworld-$program" "$ctx/src/build/apps/novaworld_$program/"
    done
    mkdir -p "$ctx/src/build/apps/serve" "$ctx/src/build/apps/hostd" "$ctx/src/build/tests" \
        "$ctx/src/build/apps/project" "$ctx/src/build/apps/update_tool" \
        "$ctx/src/backend" "$ctx/src/apps/novaworld_web" "$ctx/src/apps/novaworld_common/hosted/cloud/machine"
    # The data program's release build runs these two (products/release_build.h);
    # seeding makes a game's project with the first in the data image (make_project).
    cp "$tree/apps/project/opennova-project" "$ctx/src/build/apps/project/"
    cp "$tree/apps/update_tool/opennova-update-tool" "$ctx/src/build/apps/update_tool/"
    cp "$tree/apps/serve/opennova-serve" "$ctx/src/build/apps/serve/"
    cp "$tree/apps/hostd/opennova-hostd" "$ctx/src/build/apps/hostd/"
    cp "$tree/tests/opennova-novaworld-stack-smoke" "$ctx/src/build/tests/"
    cp -r "$repo/backend/migrations" "$repo/backend/seed" "$ctx/src/backend/"
    cp -r "$repo/apps/novaworld_web/templates" "$repo/apps/novaworld_web/static" "$ctx/src/apps/novaworld_web/"
    cp "$repo/apps/novaworld_common/hosted/cloud/machine/machine_user_data.sh" \
        "$ctx/src/apps/novaworld_common/hosted/cloud/machine/"
    printf '%s\n' "$ctx"
}

# The programs' images, the portal's and the smoke test's (compiled in Docker,
# or from --prebuilt binaries, which builds the machines' hostd image too).
build_images() {
    local prebuilt="$1"
    if [[ -z "$prebuilt" ]]; then
        say "building the images (the six programs, the portal, the smoke test)"
        stack_compose --profile smoke build
        return
    fi
    local ctx target
    ctx="$(host_path "$(stage_prebuilt "$prebuilt")")"
    say "building the images from $prebuilt's binaries"
    for target in gate lobby web fleet worker data smoke; do
        local tag="opennova-novaworld-$target:local"
        MSYS_NO_PATHCONV=1 docker build -q -f "$(host_path "$repo/apps/novaworld_common/Dockerfile")" \
            --target "$target" --build-context "build=$ctx" --build-context "smoke-build=$ctx" \
            -t "$tag" "$(host_path "$repo")" > /dev/null
        say "  $tag"
    done
    MSYS_NO_PATHCONV=1 docker build -q -f "$(host_path "$repo/apps/serve/Dockerfile")" --target hostd \
        --build-context "build=$ctx" -t opennova-hostd:local "$(host_path "$repo")" > /dev/null
    say "  opennova-hostd:local"
    MSYS_NO_PATHCONV=1 docker build -q -t opennova-portal:local "$(host_path "$repo/web")" > /dev/null
    say "  opennova-portal:local"
}

# The synthetic install, out of the smoke image (packed there at its build: the
# three base archives and a version.txt, as an install's root holds them).
synthetic_data() {
    local dir="$state/synthetic-data" cid
    rm -rf "$dir"
    mkdir -p "$dir"
    cid="$(docker create opennova-novaworld-smoke:local)"
    MSYS_NO_PATHCONV=1 docker cp "$cid:/app/synthetic-data/." "$(host_path "$dir")" > /dev/null
    docker rm "$cid" > /dev/null
    printf '%s\n' "$dir"
}

# ---- the game, seeded as a published release of its project -------------------

# The site's API through the portal: AUTH METHOD PATH [BODY [TYPE]]. AUTH is
# admin (the Bearer token the stack's web holds) or session (the dev admin's
# own session, its cookie kept in the state: the editor's routes take no
# token, so every revision names its author); BODY a JSON text or @<file>
# (curl's --data-binary, the file as host_path spells it), sent as TYPE
# (default application/json). The reply's body on stdout, the HTTP status in
# $state/reply.code, which reply_code reads: callers run this in a command
# substitution, where a variable set here would be lost.
call_api() {
    local auth="$1" method="$2" path="$3" body="${4:-}" type="${5:-application/json}"
    local args=(-sS -o "$state/reply.json" -w '%{http_code}' -X "$method")
    case "$auth" in
        admin) args+=(-H "Authorization: Bearer $admin_token") ;;
        session) args+=(-b "$state/cookies.txt" -c "$state/cookies.txt" -H "X-OpenNova-Request: 1") ;;
    esac
    [[ -z "$body" ]] || args+=(-H "Content-Type: $type" --data-binary "$body")
    curl "${args[@]}" "http://localhost:$site_port$path" > "$state/reply.code" \
        || die "curl $method $path failed (is the stack up?)"
    cat "$state/reply.json"
    rm -f "$state/reply.json"
}

admin_api() { call_api admin "$@"; }
session_api() { call_api session "$@"; }

# The HTTP status of the last call_api.
reply_code() {
    cat "$state/reply.code"
}

# The field $4 of tree $3 of the game $2 in the GET /api/admin/products reply
# $1 ("" for none).
tree_value() {
    jq -r --argjson d "$2" --argjson t "$3" --arg f "$4" \
        '[.products[] | select(.id == $d) | .trees[] | select(.id == $t) | .[$f]] | first // ""' <<< "$1"
}

# The project of the game install $1 for the game whose title (opennova-project's
# --game) is $2, named $3, made in $state/project/project as the editor makes a
# retail game's stock revision (new on the install, then import --all), with
# create-missing after, which makes from scratch each file a build needs that
# the install lacks (the synthetic install holds one map and its text).
# opennova-project runs in a container of the data program's image, which
# carries it for its release builds. Each of the project's files but the
# machine's own (.opennova/: the editor's push leaves out every dot-folder) is
# listed in $state/project/files.tsv, one "<sha256> <size> <path>" line each,
# tab-separated.
make_project() {
    local install="$1" title="$2" name="$3" work="$state/project"
    rm -rf "$work"
    mkdir -p "$work"
    say "making $name's project from $install (opennova-project in $data_image)"
    # shellcheck disable=SC2016 # the container's shell expands them
    MSYS_NO_PATHCONV=1 docker run --rm --user "$(id -u):$(id -g)" --entrypoint sh \
        -v "$(host_path "$install"):/install:ro" -v "$(host_path "$work"):/work" "$data_image" -c '
            set -e
            /app/opennova-project new /work/project --title "$2" --game "$1" --install /install
            /app/opennova-project import /work/project --all --install /install
            /app/opennova-project create-missing /work/project
            cd /work/project
            find . -mindepth 1 -type d -name ".*" -prune -o -type f -print | sed "s|^\./||" |
                while IFS= read -r f; do
                    printf "%s\t%s\t%s\n" "$(sha256sum < "$f" | cut -c 1-64)" "$(stat -c %s "$f")" "$f"
                done > /work/files.tsv' \
        sh "$title" "$name" > "$work/make.log" 2>&1 \
        || { tail -n 30 "$work/make.log" >&2; die "making the project failed (its whole log: $work/make.log)"; }
    say "  $(wc -l < "$work/files.tsv" | tr -d ' ') files"
}

# The game whose project key is $2 seeded from the game install $1 as a
# deployment's game gets its data, nothing uploaded by hand: its project made
# from the install (make_project) and pushed as the OpenNova Editor pushes, as
# the dev admin `test` (the files the store lacks named, each PUT where its
# upload says, the manifest pushed as the revision after the head); a release
# built from that revision (the data program's release.build: the export,
# packed whole), validated and published, which queues the release.apply that
# makes the game's tree from it; then the data program's verdict on the tree
# (ready, or failed with why). $3 names a mission the data must hold. Writes
# the game's key and id, the release's version and the rotation to the state.
seed_game() {
    local install="$1" key="$2" mission="$3" work="$state/project"
    local name reply row id title head root sha url path upload version release status tree state_now
    local missions waited
    for name in curl jq; do
        command -v "$name" > /dev/null || die "$name is needed to seed the game (install it, or scripts/dev_stack.sh up --no-hosting)"
    done

    reply="$(admin_api GET /api/admin/products)"
    [[ "$(reply_code)" == 200 ]] || die "GET /api/admin/products: HTTP $(reply_code) $reply"
    [[ "$(jq -r '.store.kind // ""' <<< "$reply")" != "" ]] \
        || die "web names no store (ONNET_OBJECT_STORE_URL): hosting cannot take a game"
    row="$(jq -c --arg k "$key" '[.products[] | select(.key == $k)] | first // empty' <<< "$reply")"
    [[ -n "$row" ]] || die "no game has the project key $key (the database seeds JO, DFX2 and ACIDGAMBIT)"
    [[ "$(jq -r '.kind' <<< "$row")" == game ]] || die "$key is an expansion: a hosted server runs a game"
    id="$(jq -r '.id' <<< "$row")"
    title="$(jq -r '.title' <<< "$row")"
    head="$(jq -r '.head // 0' <<< "$row")"
    make_project "$install" "$title" "$(jq -r '.display_name' <<< "$row")"

    # The push, through the editor's routes as the dev admin's own session. The
    # manifest is its canonical text (base/project/project_manifest.h: io::json_write's,
    # its keys sorted, two spaces an indent, the files sorted by their paths' bytes),
    # which jq's sorted output is.
    : > "$state/cookies.txt"
    reply="$(session_api POST /api/login '{"username":"test","password":"test"}')"
    [[ "$(reply_code)" == 200 ]] || die "signing in as test failed: HTTP $(reply_code) $reply"
    jq -R -s -S '{schema_version: 1, files: ([split("\n")[] | select(length > 0) | split("\t")
            | {path: .[2], sha256: .[0], size: (.[1] | tonumber)}] | sort_by(.path))}' \
        < "$work/files.tsv" | tr -d '\r' > "$work/manifest.json"
    jq -c '{blobs: ([.files[] | {sha256, size}] | unique_by(.sha256))}' "$work/manifest.json" \
        > "$work/missing.json"
    root="$(host_path "$work")"
    reply="$(session_api POST "/api/products/$key/missing" "@$root/missing.json")"
    [[ "$(reply_code)" == 200 ]] || die "POST /api/products/$key/missing: HTTP $(reply_code) $reply"
    say "pushing $key r$((head + 1)): $(jq '.missing | length' <<< "$reply") file(s) the store lacks"
    while IFS=$'\t' read -r sha url; do
        # The site's own blob route: the local stack's store is a directory, which
        # signs no URL of its own.
        [[ "$url" == /api/* ]] || die "the store signs its own uploads ($url): the local stack's is a directory"
        path="$(grep -m 1 "^$sha"$'\t' "$work/files.tsv" | cut -f 3)"
        upload="$(session_api PUT "$url" "@$root/project/$path" application/octet-stream)"
        [[ "$(reply_code)" == 200 ]] || die "PUT $path: HTTP $(reply_code) $upload"
    done < <(jq -r '.missing[] | [.sha256, .upload.url] | @tsv' <<< "$reply")
    jq -cn --argjson parent "$head" --arg message "scripts/dev_stack.sh up: imported from $install" \
        --rawfile manifest "$work/manifest.json" '{parent: $parent, message: $message, manifest: $manifest}' \
        > "$work/push.json"
    reply="$(session_api POST "/api/products/$key/revisions" "@$root/push.json")"
    [[ "$(reply_code)" == 201 ]] || die "the push failed: HTTP $(reply_code) $reply"
    head="$(jq -r '.head' <<< "$reply")"

    # A release built from that revision, validated, then published.
    version="1.0.$head"
    reply="$(admin_api POST "/api/admin/products/$key/releases" \
        "$(jq -cn --arg v "$version" --argjson r "$head" '{version: $v, revision: $r}')")"
    [[ "$(reply_code)" == 201 ]] || die "building a release of $key r$head failed: HTTP $(reply_code) $reply"
    release="$(jq -r '.release.id' <<< "$reply")"
    say "release $release ($key $version) building from r$head (the data program exports and packs it)..."
    waited=0
    while :; do
        reply="$(admin_api GET /api/admin/releases)"
        status="$(jq -r --argjson r "$release" '[.releases[] | select(.id == $r) | .status] | first // ""' <<< "$reply")"
        case "$status" in
            draft) break ;;
            invalid)
                jq -r --argjson r "$release" '.releases[] | select(.id == $r) | .build_log' <<< "$reply" \
                    | tail -n 30 >&2
                die "release $release is invalid: $(jq -r --argjson r "$release" \
                    '.releases[] | select(.id == $r) | .error' <<< "$reply")" ;;
            "") die "release $release is gone" ;;
        esac
        [[ "$waited" -lt 900 ]] || die "release $release is still $status after 900 s (scripts/dev_stack.sh logs novaworld-data)"
        sleep 2
        waited=$((waited + 2))
    done
    reply="$(admin_api POST "/api/admin/releases/$release/publish")"
    [[ "$(reply_code)" == 202 ]] || die "publishing release $release failed: HTTP $(reply_code) $reply"
    say "release $release published: release.apply makes $key's tree from it..."
    # The tree: its id from the release's section once the job made it, its
    # state, missions and error from the game's row.
    tree=""
    state_now=""
    waited=0
    while :; do
        if [[ -z "$tree" ]]; then
            reply="$(admin_api GET /api/admin/releases)"
            tree="$(jq -r --argjson r "$release" --argjson d "$id" \
                '[.releases[] | select(.id == $r) | .sections[] | select(.product_id == $d) | .tree.id // empty] | first // ""' \
                <<< "$reply")"
        fi
        if [[ -n "$tree" ]]; then
            reply="$(admin_api GET /api/admin/products)"
            state_now="$(tree_value "$reply" "$id" "$tree" state)"
            case "$state_now" in
                ready) break ;;
                failed) die "tree $tree failed: $(tree_value "$reply" "$id" "$tree" error)" ;;
            esac
        fi
        [[ "$waited" -lt 600 ]] || die "release $release's tree is still ${state_now:-unmade} after 600 s (scripts/dev_stack.sh logs novaworld-data)"
        sleep 2
        waited=$((waited + 2))
    done
    # How many multiplayer missions the data holds (the tree's row counts them;
    # the owner's offer lists them): what a start may rotate.
    missions="$(tree_value "$reply" "$id" "$tree" missions)"
    say "tree $tree is ready: $missions multiplayer mission(s); new servers start on it"
    [[ "$missions" != 0 ]] || die "the data holds no multiplayer mission: no server can start on it"
    {
        echo "STACK_GAME=$key"
        echo "STACK_GAME_ID=$id"
        echo "STACK_RELEASE=$version"
        echo "STACK_MISSION=$mission"
    } >> "$env_file"
}

cmd_up() {
    local data="" game="JO" mission="" synthetic=0 hosting_off=0 address=""
    local prebuilt="" build=1 vite=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --data) [[ $# -ge 2 ]] || die "$usage"; data="$2"; shift 2 ;;
            --game) [[ $# -ge 2 ]] || die "$usage"; game="$2"; shift 2 ;;
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

    [[ "$hosting_off" == 0 ]] || { data=""; synthetic=0; }
    local hosting=0
    if [[ "$synthetic" == 1 || -n "$data" ]]; then hosting=1; fi
    if [[ -n "$data" && ! -d "$data" ]]; then die "--data $data is no folder"; fi
    # Absolute: docker mounts it (make_project), and takes a relative source as
    # a volume's name.
    [[ -z "$data" ]] || data="$(cd "$data" && pwd)"
    [[ "$game" =~ ^[A-Z0-9_]{1,16}$ ]] || die "--game takes a game's project key (JO)"
    [[ "$synthetic" == 0 ]] || game="JO"
    [[ -z "$mission" || "$mission" =~ ^[A-Za-z0-9_-]+\.[Bb][Mm][Ss]$ ]] || die "--mission takes a .bms name"

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

    local dir=""
    if [[ "$hosting" == 1 ]]; then
        if [[ "$synthetic" == 1 ]]; then
            # Out of the smoke image, built above.
            dir="$(synthetic_data)"
        else
            dir="$data"
        fi
        sed -i.bak 's/^STACK_HOSTING=0$/STACK_HOSTING=1/' "$env_file" && rm -f "$env_file.bak"
        # The hostd image the machines run (--prebuilt built it above).
        if [[ "$build" == 1 && -z "$prebuilt" ]]; then
            say "building the machines' hostd image"
            stack_compose --profile machines build machine-image
        fi
    fi

    say "starting the stack (project $project)"
    local profiles=()
    [[ "$vite" == 1 ]] && profiles=(--profile vite)
    stack_compose "${profiles[@]}" up -d --no-build --remove-orphans --wait --wait-timeout 300 \
        || { stack_compose ps; die "the stack did not come up healthy (scripts/dev_stack.sh logs)"; }

    # The game's data reaches the stack as it reaches a deployment: its project
    # pushed, a release built from it and published, its tree applied by the
    # data program.
    [[ "$hosting" == 0 ]] || seed_game "$dir" "$game" "$mission"

    cat << EOF

The local NovaWorld stack runs (project $project).
  site            http://localhost:$site_port  (the portal; its API at /api)
  site admin      log in as test / test (the seeded dev admin)
  admin API       Authorization: Bearer $admin_token
  retail client   the hosts line: $address gs.novaworld.net; allow UDP 7597 and 64206
  opennova.exe    its NovaWorld screen's OpenNova target: a source run dials 127.0.0.1;
                  a packaged one takes --novaworld-host $address
  dev accounts    test/test, foo/foo, test1..3/test
  announced at    $address (gate udp 7597, lobby udp 64206, web http 8080)
EOF
    if [[ "$hosting" == 1 ]]; then
        echo "  hosting         on: $game (game $(state_value STACK_GAME_ID)) release $(state_value STACK_RELEASE), its project from $( [[ "$synthetic" == 1 ]] && echo "the synthetic install" || echo "$data") (the site's Products page lists it)"
        [[ -z "$mission" ]] || echo "  mission         $mission (the smoke test's server runs it)"
    else
        echo "  hosting         off: no game install (up --data <install> or --synthetic-data)"
    fi
    [[ "$vite" == 1 ]] && echo "  vite            http://localhost:5173 (hot reload of web/)"
    echo "  smoke test      scripts/dev_stack.sh smoke"
}

cmd_smoke() {
    [[ -f "$env_file" ]] || die "no stack (scripts/dev_stack.sh up first)"
    local args=(run --web novaworld-web:8082 --site novaworld-web:80 --gate novaworld-gate:7597
        --livez novaworld-gate:8081 --livez novaworld-lobby:8081
        --livez novaworld-fleet:8081 --livez novaworld-worker:8081 --livez novaworld-data:8081
        --admin-token "$(state_value ADMIN_API_TOKEN)" --timeout 300)
    if [[ "$(state_value STACK_HOSTING)" == "1" ]]; then
        local id mission
        id="$(state_value STACK_GAME_ID)"
        [[ -n "$id" ]] || die "the stack hosts, but no game was seeded (scripts/dev_stack.sh up again)"
        # The static addresses' zone the worker writes (deploy/compose/docker-compose.hosting.yml).
        args+=(--hosting "$id" --zone-file /var/lib/opennova/zone/play.localhost.zone)
        mission="$(state_value STACK_MISSION)"
        [[ -z "$mission" ]] || args+=(--mission "$mission")
    fi
    local rc=0
    stack_compose --profile smoke run --rm --no-deps smoke "${args[@]}" || rc=$?
    if [[ "$rc" != 0 ]]; then
        echo
        say "the smoke test failed; the last lines of each program (and machine):"
        stack_compose logs --no-color --tail 40 novaworld-gate novaworld-lobby novaworld-web \
            novaworld-fleet novaworld-worker novaworld-data || true
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
        say "the stack's database, store, logs and state are gone"
        ;;
    -h|--help) echo "$usage" ;;
    *) die "$usage" ;;
esac
