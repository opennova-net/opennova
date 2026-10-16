#!/usr/bin/env bash
# A local Valkey 8 (the Redis the NovaWorld server's HTTP layer keeps its shared
# web state in: apps/novaworld_common/redis_client.h) for the server and its
# tests, in Docker: the container opennova-dev-redis on 127.0.0.1 (port 6379
# unless --port names another). It keeps nothing worth keeping (every key has a
# TTL), so it runs with no persistence and no volume.
#
#   scripts/dev_redis.sh up [--port N]   start it and print the settings that
#                                        reach it
#   scripts/dev_redis.sh down            stop and remove the container
#   scripts/dev_redis.sh reset           down (there is no data to delete)
#
# No password: it listens on 127.0.0.1 alone.
set -euo pipefail

container="opennova-dev-redis"
image="valkey/valkey:8"
port=6379
usage="usage: scripts/dev_redis.sh up [--port N] | down | reset"

[[ $# -ge 1 ]] || { echo "$usage" >&2; exit 2; }
action="$1"; shift
while [[ $# -gt 0 ]]; do
    case "$1" in
        --port)
            [[ $# -ge 2 ]] || { echo "$usage" >&2; exit 2; }
            port="$2"; shift 2 ;;
        --port=*) port="${1#--port=}"; shift ;;
        *) echo "$usage" >&2; exit 2 ;;
    esac
done

exists() { [[ -n "$(docker ps -aq --filter "name=^${container}\$")" ]]; }

down() {
    if exists; then
        docker rm -f "$container" > /dev/null
        echo "removed $container"
    fi
}

case "$action" in
    up)
        if ! exists; then
            # As production's ElastiCache runs it: no snapshots, and a full
            # memory evicts the key touched longest ago among those with a TTL
            # (every key the server writes has one).
            docker run -d --name "$container" \
                -p "127.0.0.1:${port}:6379" \
                "$image" valkey-server --save "" --appendonly no \
                --maxmemory 256mb --maxmemory-policy volatile-lru > /dev/null
        else
            docker start "$container" > /dev/null
        fi
        for _ in $(seq 1 30); do
            if docker exec "$container" valkey-cli ping > /dev/null 2>&1; then
                break
            fi
            sleep 1
        done
        docker exec "$container" valkey-cli ping > /dev/null 2>&1 || {
            echo "$container did not come up; see: docker logs $container" >&2
            exit 1
        }
        cat <<EOF
$container is up on 127.0.0.1:${port}.

The server (apps/novaworld_common/README.md "Redis"):
  ONNET_REDIS_URL=redis://127.0.0.1:${port}

The server's HTTP-layer tests (each keeps its keys under a prefix of its own):
  cmake ... -DOPENNOVA_TEST_REDIS=redis://127.0.0.1:${port}
EOF
        ;;
    down|reset)
        down ;;
    *)
        echo "$usage" >&2; exit 2 ;;
esac
