#!/usr/bin/env bash
# Regenerate the standalone importer screenshot used by README.md.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

(
  cd "$root"
  uv run python -m apps.importer.ui.screenshot_capture \
    --output "$root/screenshots/importer.png"
)

echo "Screenshot written to $root/screenshots/importer.png"
