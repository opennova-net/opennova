#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

cd "$root"
if [[ -z "${UV_CACHE_DIR:-}" ]]; then
    export UV_CACHE_DIR="${TMPDIR:-$root/.scratch}/opennova-uv-cache"
fi
uv run --frozen pytest "$@"
