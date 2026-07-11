#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cli="$root/scripts/retail_parity.sh"
expected_sha='9a1035440a53af2057ce0995ac42dced840d3b9fd53c04dc86041a962b84fe57'
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

fail() {
  printf 'retail_parity_doctor_test: %s\n' "$*" >&2
  exit 1
}

assert_contains() {
  local haystack="$1" needle="$2"
  [[ "$haystack" == *"$needle"* ]] || fail "expected output to contain: $needle"
}

dry="$(
  env -u JO_GAME_DIR -u GODOT_BIN -u OPENNOVA_RESOURCE_DIR \
    RETAIL_PARITY_ENV_FILE="$tmp/missing.env" \
    bash "$cli" --dry-run doctor
)"
assert_contains "$dry" "CHECK=JO_EXECUTABLE_SHA256 STATUS=dry-run EXPECTED=$expected_sha"
assert_contains "$dry" 'CHECK=EXPANSION STATUS=dry-run NAME=jox01'
assert_contains "$dry" 'CHECK=MISSION STATUS=dry-run NAME=ASH_G3D.bms'

game="$tmp/game-root"
expansion="$game/expansion/jox01"
mkdir -p "$expansion"
printf 'synthetic executable, intentionally not retail\n' >"$game/Jointops.exe"
printf 'synthetic Godot marker\n' >"$tmp/Godot.exe"

python - "$expansion/jox01.pff" <<'PY'
import struct
import sys

path = sys.argv[1]
name = b"ASH_G3D.bms"
payload = b"synthetic mission marker"
table_offset = 20 + len(payload)
header = struct.pack("<IIIII", 20, 0x33464650, 1, 36, table_offset)
entry = struct.pack(
    "<IIII16sI",
    0,
    20,
    len(payload),
    0,
    name + bytes(16 - len(name)),
    0,
)
with open(path, "wb") as stream:
    stream.write(header)
    stream.write(payload)
    stream.write(entry)
PY

set +e
fixture="$(
  RETAIL_PARITY_ENV_FILE="$tmp/missing.env" \
  JO_GAME_DIR="$game" \
  GODOT_BIN="$tmp/Godot.exe" \
    bash "$cli" doctor 2>&1
)"
fixture_status=$?
set -e
[[ "$fixture_status" -eq 64 ]] || fail "wrong-hash doctor exited $fixture_status, expected 64"
assert_contains "$fixture" "CHECK=JO_EXECUTABLE_SHA256 STATUS=mismatch EXPECTED=$expected_sha"
assert_contains "$fixture" 'CHECK=EXPANSION STATUS=ok NAME=jox01'
assert_contains "$fixture" 'CHECK=MISSION STATUS=ok NAME=ASH_G3D.bms SOURCE=packed'
assert_contains "$fixture" 'ARCHIVE=jox01.pff'

python - "$game/resource.pff" <<'PY'
import struct
import sys

path = sys.argv[1]
name = b"SELECTED.bms"
payload = b"synthetic selected mission marker"
table_offset = 20 + len(payload)
header = struct.pack("<IIIII", 20, 0x33464650, 1, 36, table_offset)
entry = struct.pack(
    "<IIII16sI",
    0,
    20,
    len(payload),
    0,
    name + bytes(16 - len(name)),
    0,
)
with open(path, "wb") as stream:
    stream.write(header)
    stream.write(payload)
    stream.write(entry)
PY

set +e
selected="$(
  RETAIL_PARITY_ENV_FILE="$tmp/missing.env" \
  JO_GAME_DIR="$game" \
  GODOT_BIN="$tmp/Godot.exe" \
    bash "$cli" doctor --mission SELECTED.bms 2>&1
)"
selected_status=$?
set -e
[[ "$selected_status" -eq 64 ]] || fail "selected-mission doctor exited $selected_status, expected 64"
assert_contains "$selected" 'CHECK=MISSION STATUS=ok NAME=SELECTED.bms SOURCE=packed ARCHIVE=resource.pff PATH='

printf 'synthetic loose-only mission marker\n' >"$expansion/LOOSE_ONLY.bms"
set +e
loose_only="$(
  RETAIL_PARITY_ENV_FILE="$tmp/missing.env" \
  JO_GAME_DIR="$game" \
  GODOT_BIN="$tmp/Godot.exe" \
    bash "$cli" doctor --mission LOOSE_ONLY.bms 2>&1
)"
loose_status=$?
set -e
[[ "$loose_status" -eq 64 ]] || fail "loose-only doctor exited $loose_status, expected 64"
assert_contains "$loose_only" 'CHECK=MISSION STATUS=missing NAME=LOOSE_ONLY.bms SOURCE=packed'
assert_contains "$loose_only" 'DETAIL=loose-only-requires-/d'

mv "$expansion/jox01.pff" "$expansion/jox01.pff.fixture-away"
set +e
missing_archive="$(
  RETAIL_PARITY_ENV_FILE="$tmp/missing.env" \
  JO_GAME_DIR="$game" \
  GODOT_BIN="$tmp/Godot.exe" \
    bash "$cli" doctor 2>&1
)"
missing_archive_status=$?
set -e
[[ "$missing_archive_status" -eq 64 ]] || fail "missing-archive doctor exited $missing_archive_status, expected 64"
assert_contains "$missing_archive" 'CHECK=EXPANSION STATUS=missing-archive NAME=jox01'

printf 'retail_parity_doctor_test: OK\n'
