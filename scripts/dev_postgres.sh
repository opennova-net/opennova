#!/usr/bin/env bash
# A local PostgreSQL 17 for the NovaWorld service and its tests, in Docker:
# the container opennova-dev-postgres on 127.0.0.1 (port 5432 unless --port
# names another), its data in the named volume opennova-dev-pgdata. The
# database is made the way the service requires (backend/README.md):
# SQL_ASCII with the C locale.
#
#   scripts/dev_postgres.sh up [--port N]   start it (once created, the same
#                                          data comes back) and print the
#                                          settings that reach it
#   scripts/dev_postgres.sh down            stop and remove the container;
#                                          the volume stays
#   scripts/dev_postgres.sh reset           down, and delete the volume too
#
# The role novaworld (password dev) owns the database novaworld and is the
# server's superuser, so the tests may create and drop their own databases
# beside it.
set -euo pipefail

container="opennova-dev-postgres"
volume="opennova-dev-pgdata"
image="postgres:17"
port=5432
usage="usage: scripts/dev_postgres.sh up [--port N] | down | reset"

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
            docker run -d --name "$container" \
                -p "127.0.0.1:${port}:5432" \
                -e POSTGRES_USER=novaworld \
                -e POSTGRES_DB=novaworld \
                -e POSTGRES_PASSWORD=dev \
                -e POSTGRES_INITDB_ARGS="--locale=C --encoding=SQL_ASCII" \
                -v "${volume}:/var/lib/postgresql/data" \
                "$image" > /dev/null
        else
            docker start "$container" > /dev/null
        fi
        # The first start runs initdb before the server takes connections.
        for _ in $(seq 1 60); do
            if docker exec "$container" pg_isready -q -U novaworld -d novaworld; then
                break
            fi
            sleep 1
        done
        docker exec "$container" pg_isready -q -U novaworld -d novaworld || {
            echo "$container did not come up; see: docker logs $container" >&2
            exit 1
        }
        cat <<EOF
$container is up on 127.0.0.1:${port}.

The server (apps/novaworld_server/README.md "Database"):
  DATABASE_URL=postgresql://novaworld@127.0.0.1:${port}/novaworld DATABASE_PASSWORD=dev

The service's database tests (each makes and drops its own databases):
  cmake ... -DOPENNOVA_TEST_PG=postgresql://novaworld:dev@127.0.0.1:${port}/postgres
EOF
        ;;
    down)
        down ;;
    reset)
        down
        if docker volume inspect "$volume" > /dev/null 2>&1; then
            docker volume rm "$volume" > /dev/null
            echo "removed volume $volume"
        fi
        ;;
    *)
        echo "$usage" >&2; exit 2 ;;
esac
