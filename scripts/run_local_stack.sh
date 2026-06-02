#!/usr/bin/env bash
#
# Boot the standalone novaworld server + Vite dev server for end-to-end
# local development. Both processes log to the foreground; Ctrl+C tears
# them down cleanly.
#
# Prereqs:
#   - Built the standalone server:
#       cmake --build build --config Release \
#         --target opennova_novaworld_server \
#         -DBUILD_NOVAWORLD_HTTP=ON
#   - Installed the web/ npm deps:
#       (cd web && npm install)
#
# Env overrides (optional):
#   ONNET_PUBLIC_HOST   default 127.0.0.1
#   ONNET_GATE_UDP_PORT default 7597
#   ONNET_NW_UDP_PORT   default 64206
#   ONNET_HTTP_PORT     default 8080
#   DATABASE_PATH       default backend/data/state.db

set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

server_bin="$root/build/apps/novaworld_server/Release/opennova-novaworld.exe"
if [[ ! -x "$server_bin" ]]; then
    server_bin="$root/build/apps/novaworld_server/opennova-novaworld"
fi
if [[ ! -x "$server_bin" ]]; then
    echo "error: standalone server binary not found at:" >&2
    echo "  build/apps/novaworld_server/Release/opennova-novaworld.exe" >&2
    echo "  build/apps/novaworld_server/opennova-novaworld" >&2
    echo "build it with -DBUILD_NOVAWORLD_HTTP=ON first" >&2
    exit 1
fi

if [[ ! -d "$root/web/node_modules" ]]; then
    echo "error: web/node_modules missing — run (cd web && npm install) first" >&2
    exit 1
fi

cleanup() {
    echo "[stack] stopping..."
    [[ -n "${SERVER_PID:-}" ]] && kill "$SERVER_PID" 2>/dev/null || true
    [[ -n "${VITE_PID:-}" ]] && kill "$VITE_PID" 2>/dev/null || true
    wait 2>/dev/null || true
}
trap cleanup EXIT INT TERM

echo "[stack] starting novaworld server..."
"$server_bin" &
SERVER_PID=$!

echo "[stack] starting Vite dev server..."
(cd "$root/web" && npm run dev) &
VITE_PID=$!

echo "[stack] running. Ctrl+C to stop."
echo "[stack]   web UI:        http://localhost:5173"
echo "[stack]   /api directly: http://localhost:8080/api/games"
echo "[stack]   /api/lobbies:  http://localhost:8080/api/lobbies"
wait
