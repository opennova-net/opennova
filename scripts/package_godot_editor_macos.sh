#!/usr/bin/env bash
# Build and export the OpenNova Mod Tools Godot package for macOS.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
exec "$SCRIPT_DIR"/package_godot_macos.sh editor
