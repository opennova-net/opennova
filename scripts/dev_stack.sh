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
#                           when it names a folder): once the stack is up, its
#                           base files (resource.pff, localres.pff,
#                           language.pff; version.txt, score.ini, _NSTMOUT.TXT
#                           and gt.ssc when present) are uploaded into the
#                           stack's store as a data set through the site's
#                           admin API, the way the Game data page does, and
#                           its rotation is the data's multiplayer missions
#     --data-set <slug>     the lobby (games slug) that data set lists in
#                           (default jop_2_consumer)
#     --mission <name.bms>  the smoke test's server runs this mission of the
#                           data (default: the data's whole rotation)
#     --synthetic-data      host on the synthetic data set instead (one
#                           deathmatch map packed into the three archives, no
#                           licensed byte: what CI runs)
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
#   scripts/dev_stack.sh reset          down, the database, the store and the state too
#
# Seeding a data set needs curl and jq on this machine (the upload flow is the
# admin API's, as the Game data page's: a tree, each file's parts PUT through
# the site, a completion, a finalize).
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

# The synthetic data set, out of the smoke image (packed there at its build:
# the three base archives and a version.txt, as an install's root holds them).
synthetic_data() {
    local dir="$state/synthetic-data" cid
    rm -rf "$dir"
    mkdir -p "$dir"
    cid="$(docker create opennova-novaworld-smoke:local)"
    MSYS_NO_PATHCONV=1 docker cp "$cid:/app/synthetic-data/." "$(host_path "$dir")" > /dev/null
    docker rm "$cid" > /dev/null
    printf '%s\n' "$dir"
}

# ---- the data set, seeded through the admin API --------------------------------

# The title a lobby's data set speaks (what opennova-serve's /game takes), by
# the games slug; "" for a lobby the stack knows no title for.
title_of() {
    case "$1" in
        jop_2_consumer | opennova_0_consumer) echo jo ;;
        dfx2_consumer) echo dfx2 ;;
        *) echo "" ;;
    esac
}

# A file of the folder $1 named $2 (spelt in any case, as a retail install
# spells them), or nothing.
find_file() {
    find "$1" -maxdepth 1 -type f -iname "$2" 2> /dev/null | head -n 1
}

file_size() {
    wc -c < "$1" | tr -d ' '
}

# The site's API as the admin, through the portal (the Bearer token the stack's
# web holds): METHOD PATH [BODY], BODY a JSON text or @<file> (a raw part, sent
# as application/octet-stream). The reply's body on stdout, the HTTP status in
# $state/reply.code, which reply_code reads: callers run this in a command
# substitution, where a variable set here would be lost.
admin_api() {
    local method="$1" path="$2" body="${3:-}"
    local args=(-sS -o "$state/reply.json" -w '%{http_code}' -X "$method" \
        -H "Authorization: Bearer $admin_token" "http://localhost:$site_port$path")
    case "$body" in
        "") ;;
        @*) args+=(-H "Content-Type: application/octet-stream" --data-binary "$body") ;;
        *) args+=(-H "Content-Type: application/json" --data-binary "$body") ;;
    esac
    curl "${args[@]}" > "$state/reply.code" || die "curl $method $path failed (is the stack up?)"
    cat "$state/reply.json"
    rm -f "$state/reply.json"
}

# The HTTP status of the last admin_api call.
reply_code() {
    cat "$state/reply.code"
}

# One file of a tree, uploaded in parts of PART_SIZE bytes: each part PUT to
# the admin API's part route through the portal (web hands it to the store,
# as the Game data page's uploads go), the etag it answers collected, then
# the file's completion.
upload_file() {
    local data_set="$1" tree="$2" name="$3" upload_id="$4" part_size="$5" path="$6"
    local size parts n part="$state/part.bin" query etag reply
    size="$(file_size "$path")"
    parts=$(( (size + part_size - 1) / part_size ))
    [[ "$parts" -ge 1 ]] || parts=1
    query="upload_id=$(jq -rn --arg u "$upload_id" '$u | @uri')"
    local list="[]"
    for (( n = 1; n <= parts; n++ )); do
        dd if="$path" of="$part" bs="$part_size" skip=$((n - 1)) count=1 status=none
        reply="$(admin_api PUT "/api/admin/data-sets/$data_set/trees/$tree/files/$name/parts/$n?$query" "@$part")"
        [[ "$(reply_code)" == 200 ]] || die "the PUT of part $n of $name failed: HTTP $(reply_code) $reply"
        etag="$(jq -r '.etag // ""' <<< "$reply")"
        [[ -n "$etag" ]] || die "the PUT of part $n of $name answered no etag: $reply"
        list="$(jq -c --argjson n "$n" --arg e "$etag" '. + [{part_number: $n, etag: $e}]' <<< "$list")"
    done
    rm -f "$part"
    reply="$(admin_api POST "/api/admin/data-sets/$data_set/trees/$tree/files/$name/complete" \
        "$(jq -cn --arg u "$upload_id" --argjson parts "$list" '{upload_id: $u, parts: $parts}')")"
    [[ "$(reply_code)" == 200 ]] || die "completing $name failed: HTTP $(reply_code) $reply"
    say "  $name ($size bytes, $parts part$([[ "$parts" == 1 ]] || echo s))"
}

# The data set of lobby $2 (its games slug) from the folder $1, through the
# admin API as the Game data page does: the row (made once; an `up` run again
# finds it), a tree of the folder's base files, each uploaded, the finalize,
# then the worker's verdict (ready, or failed with why). $3 names a mission
# the data must hold. Writes the data set's id and the rotation to the state.
seed_data_set() {
    local dir="$1" game="$2" mission="$3"
    local title display reply id tree part_size name path upload_id state_now error missions waited
    local required=(resource.pff localres.pff language.pff) optional=(version.txt score.ini _NSTMOUT.TXT gt.ssc)
    local -a files=()
    for name in curl jq; do
        command -v "$name" > /dev/null || die "$name is needed to seed the data set (install it, or scripts/dev_stack.sh up --no-hosting)"
    done
    title="$(title_of "$game")"
    for name in "${required[@]}"; do
        path="$(find_file "$dir" "$name")"
        [[ -n "$path" ]] || die "$dir holds no $name: a data set is a game install's root (its three base archives)"
        files+=("$path")
    done
    for name in "${optional[@]}"; do
        path="$(find_file "$dir" "$name")"
        [[ -z "$path" ]] || files+=("$path")
    done
    case "$game" in
        jop_2_consumer) display="Joint Operations (dev)" ;;
        dfx2_consumer) display="Delta Force Xtreme 2 (dev)" ;;
        opennova_0_consumer) display="OpenNova (dev)" ;;
        *) display="$game (dev)" ;;
    esac

    reply="$(admin_api GET /api/admin/data-sets)"
    [[ "$(reply_code)" == 200 ]] || die "GET /api/admin/data-sets: HTTP $(reply_code) $reply"
    [[ "$(jq -r '.store.kind // ""' <<< "$reply")" != "" ]] \
        || die "web names no store (ONNET_OBJECT_STORE_URL): hosting cannot take a data set"
    id="$(jq -r --arg g "$game" '[.data_sets[] | select(.kind == "base" and .game == $g) | .id] | first // ""' <<< "$reply")"
    if [[ -z "$id" ]]; then
        reply="$(admin_api POST /api/admin/data-sets \
            "$(jq -cn --arg d "$display" --arg t "$title" --arg g "$game" '{display_name: $d, kind: "base", title: $t, game: $g}')")"
        [[ "$(reply_code)" == 201 ]] || die "making the data set failed: HTTP $(reply_code) $reply"
        id="$(jq -r '.data_set.id' <<< "$reply")"
        say "data set $id ($display, lobby $game, title $title) made"
    else
        say "data set $id ($game) is there: a new tree of $dir goes on it"
    fi

    local list="[]"
    for path in "${files[@]}"; do
        list="$(jq -c --arg n "$(basename "$path")" --argjson s "$(file_size "$path")" '. + [{name: $n, size: $s}]' <<< "$list")"
    done
    reply="$(admin_api POST "/api/admin/data-sets/$id/trees" "$(jq -cn --argjson f "$list" '{files: $f}')")"
    [[ "$(reply_code)" == 201 ]] || die "starting the tree failed: HTTP $(reply_code) $reply"
    tree="$(jq -r '.tree.id' <<< "$reply")"
    part_size="$(jq -r '.uploads[0].part_size' <<< "$reply")"
    say "uploading tree $tree of data set $id from $dir (parts of $part_size bytes):"
    for path in "${files[@]}"; do
        # The canonical spelling the route gave the file, and its upload.
        name="$(jq -r --arg n "$(basename "$path")" '[.uploads[] | select((.name | ascii_downcase) == ($n | ascii_downcase)) | .name] | first // ""' <<< "$reply")"
        upload_id="$(jq -r --arg n "$name" '[.uploads[] | select(.name == $n) | .upload_id] | first // ""' <<< "$reply")"
        [[ -n "$name" && -n "$upload_id" ]] || die "the tree names no upload for $(basename "$path")"
        upload_file "$id" "$tree" "$name" "$upload_id" "$part_size" "$path"
    done
    reply="$(admin_api POST "/api/admin/data-sets/$id/trees/$tree/finalize")"
    [[ "$(reply_code)" == 202 ]] || die "finalizing the tree failed: HTTP $(reply_code) $reply"
    say "finalizing (the worker checks the files and lists the missions)..."
    waited=0
    while :; do
        reply="$(admin_api GET /api/admin/data-sets)"
        state_now="$(jq -r --argjson d "$id" --argjson t "$tree" \
            '[.data_sets[] | select(.id == $d) | .trees[] | select(.id == $t) | .state] | first // ""' <<< "$reply")"
        case "$state_now" in
            ready) break ;;
            failed)
                error="$(jq -r --argjson d "$id" --argjson t "$tree" \
                    '[.data_sets[] | select(.id == $d) | .trees[] | select(.id == $t) | .error] | first // ""' <<< "$reply")"
                die "the tree failed: $error" ;;
            "") die "tree $tree of data set $id is gone" ;;
        esac
        [[ "$waited" -lt 300 ]] || die "the tree is still $state_now after 300 s (scripts/dev_stack.sh logs novaworld-worker)"
        sleep 2
        waited=$((waited + 2))
    done
    # How many multiplayer missions the data holds (the tree's row counts
    # them; the owner's offer lists them): what a start may rotate.
    missions="$(jq -r --argjson d "$id" --argjson t "$tree" \
        '[.data_sets[] | select(.id == $d) | .trees[] | select(.id == $t) | .missions] | first // 0' <<< "$reply")"
    say "tree $tree is ready: $missions multiplayer mission(s); new servers start on it"
    [[ "$missions" != 0 ]] || die "the data holds no multiplayer mission: no server can start on it"
    {
        echo "STACK_DATA_SET=$game"
        echo "STACK_DATA_SET_ID=$id"
        echo "STACK_MISSION=$mission"
    } >> "$env_file"
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
    [[ "$synthetic" == 0 ]] || data_set="jop_2_consumer"
    [[ "$hosting" == 0 || -n "$(title_of "$data_set")" ]] \
        || die "--data-set $data_set: no title known for that lobby (jop_2_consumer, dfx2_consumer, opennova_0_consumer)"
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

    # The data set reaches the stack as it reaches a deployment: uploaded into
    # the store through the admin API, finalized by the worker.
    [[ "$hosting" == 0 ]] || seed_data_set "$dir" "$data_set" "$mission"

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
        echo "  hosting         on: data set $(state_value STACK_DATA_SET_ID) ($data_set) from $( [[ "$synthetic" == 1 ]] && echo "the synthetic data set" || echo "$data"), in the stack's store (the site's Game data page lists it)"
        [[ -z "$mission" ]] || echo "  mission         $mission (the smoke test's server runs it)"
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
        local id mission
        id="$(state_value STACK_DATA_SET_ID)"
        [[ -n "$id" ]] || die "the stack hosts, but no data set was seeded (scripts/dev_stack.sh up again)"
        args+=(--hosting "$id")
        mission="$(state_value STACK_MISSION)"
        [[ -z "$mission" ]] || args+=(--mission "$mission")
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
        say "the stack's database, store and state are gone"
        ;;
    -h|--help) echo "$usage" ;;
    *) die "$usage" ;;
esac
