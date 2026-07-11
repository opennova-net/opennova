#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cli="$root/scripts/retail_parity.sh"
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT

fail() {
  printf 'retail_parity_cli_test: %s\n' "$*" >&2
  exit 1
}

assert_contains() {
  local haystack="$1"
  local needle="$2"
  [[ "$haystack" == *"$needle"* ]] ||
    fail "expected output to contain: $needle"
}

run_dry() {
  RETAIL_PARITY_ENV_FILE="$tmp/retail-parity.env" \
  RETAIL_PARITY_RUN_ID="test-run" \
  RETAIL_PARITY_TRACE_DIR='/z/fixture/traces' \
  JO_GAME_DIR='C:\Games\Joint Operations' \
  GODOT_BIN='C:\Tools\Godot\Godot.exe' \
    bash "$cli" --dry-run "$@"
}

help="$(bash "$cli" --help)"
for command in doctor setup build test retail opennova compare dump export-pcapng validate; do
  assert_contains "$help" "$command"
done
assert_contains "$help" 'Launch and wait for the baseline'
assert_contains "$help" 'Launch and wait for the candidate'

validate="$(run_dry validate)"
assert_contains "$validate" 'SCENARIO=player-combat-loop'
assert_contains "$validate" 'MISSION=ASH_G3D.bms'
assert_contains "$validate" 'retail_hook_capture_agent_test'
assert_contains "$validate" 'retail_hook_winsock_capture_test'
assert_contains "$validate" 'retail_hook_named_pipe_trace_test'
assert_contains "$validate" 'retail_hook_cli_test'
assert_contains "$validate" 'parity_trace_test'
assert_contains "$validate" 'parity_compare_test'
assert_contains "$validate" 'parity_capture_test'
assert_contains "$validate" 'parity_file_event_sink_test'
assert_contains "$validate" 'parity_tool_cli_test'
assert_contains "$validate" 'parity_tool_network_test'
assert_contains "$validate" 'parity_tool_export_test'
assert_contains "$validate" 'parity_tool_compare_test'
assert_contains "$validate" 'parity_opennova_capture_test'
assert_contains "$validate" "-R ^(retail_hook_|parity_)"
assert_contains "$validate" 'ROLE_SET=baseline TOPOLOGY=retail-host+hooked-client'
assert_contains "$validate" 'retail_parity_inspector.exe --pipe opennova-retail-parity-test-run-baseline'
assert_contains "$validate" '--scenario player-combat-loop --role-set baseline'
assert_contains "$validate" 'retail_hook_injector.exe --game-dir C:\Games\Joint Operations --role retail-host'
assert_contains "$validate" '--role retail-client'
assert_contains "$validate" '--run-id test-run --stream-id retail-host'
assert_contains "$validate" '--run-id test-run --stream-id retail-client'
assert_contains "$validate" '--scenario player-combat-loop --mission ASH_G3D.bms'
assert_contains "$validate" '-- /w /exp revx02'
assert_contains "$validate" 'ROLE_SET=candidate TOPOLOGY=opennova-host+hooked-client'
assert_contains "$validate" 'GUIDE_ACTOR=retail-client'
assert_contains "$validate" 'GUIDE_STEP=idle-at-spawn'
assert_contains "$validate" 'GUIDE_STEP=move-stance-look'
assert_contains "$validate" 'GUIDE_STEP=aim-fire-reload'
assert_contains "$validate" 'OPENNOVA_RESOURCE_DIR=C:\Games\Joint Operations'
assert_contains "$validate" 'OPENNOVA_PARITY_ROLE=opennova-host'
assert_contains "$validate" 'OPENNOVA_PARITY_STREAM_ID=opennova-host'
assert_contains "$validate" 'OPENNOVA_PARITY_RUN_ID=test-run'
assert_contains "$validate" 'OPENNOVA_PARITY_BUILD_ID=opennova-'
assert_contains "$validate" 'OPENNOVA_PARITY_SCENARIO=player-combat-loop'
assert_contains "$validate" 'OPENNOVA_PARITY_TITLE=joint-operations'
assert_contains "$validate" 'OPENNOVA_PARITY_EXPANSION=revx02'
assert_contains "$validate" 'OPENNOVA_PARITY_MISSION=ASH_G3D.bms'
assert_contains "$validate" 'OPENNOVA_PARITY_TRACE=Z:\fixture\traces\player-combat-loop\test-run\candidate.ontrace'
assert_contains "$validate" 'NW_LAN_HOST=ASH_G3D.bms'
assert_contains "$validate" 'C:\Tools\Godot\Godot.exe --path'
assert_contains "$validate" '-- /w /exp revx02'

msys="$(
  RETAIL_PARITY_ENV_FILE="$tmp/retail-parity.env" \
  RETAIL_PARITY_RUN_ID="test-run" \
  JO_GAME_DIR='/c/Games/Joint Operations' \
  GODOT_BIN='/c/Tools/Godot/Godot.exe' \
    bash "$cli" --dry-run retail
)"
assert_contains "$msys" '--game-dir C:\Games\Joint Operations'

setup_env="$tmp/setup.env"
setup="$(RETAIL_PARITY_ENV_FILE="$setup_env" bash "$cli" --dry-run setup)"
assert_contains "$setup" "ENV_FILE=$setup_env"
[[ ! -e "$setup_env" ]] || fail 'dry-run setup created the environment file'

setup="$(RETAIL_PARITY_ENV_FILE="$setup_env" bash "$cli" setup)"
assert_contains "$setup" 'SETUP=created'
assert_contains "$(<"$setup_env")" "JO_GAME_DIR=''"
assert_contains "$(<"$setup_env")" "GODOT_BIN=''"
printf '%s\n' '# operator-owned sentinel' >>"$setup_env"
setup="$(RETAIL_PARITY_ENV_FILE="$setup_env" bash "$cli" setup)"
assert_contains "$setup" 'SETUP=kept-existing'
assert_contains "$(<"$setup_env")" '# operator-owned sentinel'

unconfigured="$(
  env -u JO_GAME_DIR -u GODOT_BIN -u OPENNOVA_RESOURCE_DIR \
    RETAIL_PARITY_ENV_FILE="$tmp/missing.env" \
    RETAIL_PARITY_RUN_ID="test-run" \
    bash "$cli" --dry-run validate
)"
assert_contains "$unconfigured" 'CHECK=JO_GAME_DIR STATUS=dry-run'
assert_contains "$unconfigured" 'CHECK=GODOT_BIN STATUS=dry-run'
assert_contains "$unconfigured" '--game-dir <JO_GAME_DIR>'
assert_contains "$unconfigured" '<GODOT_BIN> --path'

implicit="$(
  env -u JO_GAME_DIR -u GODOT_BIN -u OPENNOVA_RESOURCE_DIR \
    RETAIL_PARITY_ENV_FILE="$tmp/missing.env" \
    RETAIL_PARITY_RUN_ID="test-run" \
    bash "$cli" --dry-run
)"
assert_contains "$implicit" 'WORKFLOW=guided-retail-parity'
assert_contains "$implicit" 'SCENARIO=player-combat-loop'

configured_env="$tmp/configured.env"
configured="$(
  RETAIL_PARITY_ENV_FILE="$configured_env" bash "$cli" setup \
    --game-dir '/c/fixture/Joint Operations' \
    --godot-bin 'C:\fixture\Godot.exe'
)"
assert_contains "$configured" 'SETUP=created'
configured_values="$(bash -c 'source "$1"; printf "JO=%s\nGODOT=%s\n" "$JO_GAME_DIR" "$GODOT_BIN"' _ "$configured_env")"
assert_contains "$configured_values" 'JO=/c/fixture/Joint Operations'
assert_contains "$configured_values" 'GODOT=C:\fixture\Godot.exe'

RETAIL_PARITY_ENV_FILE="$configured_env" bash "$cli" setup \
  --game-dir '/c/fixture/must-not-overwrite' >/dev/null
configured_values="$(bash -c 'source "$1"; printf "%s" "$JO_GAME_DIR"' _ "$configured_env")"
[[ "$configured_values" == '/c/fixture/Joint Operations' ]] || fail 'setup overwrote without --force'

RETAIL_PARITY_ENV_FILE="$configured_env" bash "$cli" setup --force \
  --game-dir '/c/fixture/reconfigured' >/dev/null
configured_values="$(bash -c 'source "$1"; printf "%s" "$JO_GAME_DIR"' _ "$configured_env")"
[[ "$configured_values" == '/c/fixture/reconfigured' ]] || fail 'setup --force did not rewrite the environment'

compare="$(run_dry compare reference.ontrace candidate.ontrace)"
assert_contains "$compare" 'opennova_parity_tool.exe compare reference.ontrace candidate.ontrace'

dump="$(run_dry dump candidate.ontrace report.txt)"
assert_contains "$dump" 'opennova_parity_tool.exe dump candidate.ontrace report.txt'

pcapng="$(run_dry export-pcapng candidate.ontrace candidate.pcapng)"
assert_contains "$pcapng" 'opennova_parity_tool.exe export-pcapng candidate.ontrace candidate.pcapng'

writable="$(run_dry --allow-writes retail)"
assert_contains "$writable" '--pipe opennova-retail-parity-test-run-baseline --run-id test-run --stream-id retail-host --scenario player-combat-loop --mission ASH_G3D.bms --allow-writes -- /w /exp revx02'
assert_contains "$writable" 'GUIDE_WAIT=baseline'

standalone_candidate="$(run_dry opennova)"
assert_contains "$standalone_candidate" 'GUIDE_WAIT=candidate'

printf 'retail_parity_cli_test: OK\n'
