#!/usr/bin/env bash
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cli="$root/scripts/retail_parity.sh"
tmp="$(mktemp -d)"
unrelated_pid=""
fixture_game_pid=""
launcher_pid=""
inspector_pid_file=""
game_pid_file=""
interrupt_game_pid_file="$tmp/interrupt-game-pids"

stop_pid() {
  local pid="$1" attempt
  [[ "$pid" =~ ^[0-9]+$ ]] || return 0
  kill -TERM "$pid" 2>/dev/null || true
  for ((attempt = 0; attempt < 20; attempt++)); do
    kill -0 "$pid" 2>/dev/null || return 0
    sleep 0.05
  done
  kill -KILL "$pid" 2>/dev/null || true
}

cleanup() {
  local status=$? pid
  trap - EXIT INT TERM HUP
  stop_pid "$launcher_pid"
  stop_pid "$unrelated_pid"
  stop_pid "$fixture_game_pid"
  if [[ -n "$inspector_pid_file" && -f "$inspector_pid_file" ]]; then
    stop_pid "$(<"$inspector_pid_file")"
  fi
  if [[ -n "$game_pid_file" && -f "$game_pid_file" ]]; then
    stop_pid "$(<"$game_pid_file")"
  fi
  if [[ -f "$interrupt_game_pid_file" ]]; then
    while IFS= read -r pid; do
      stop_pid "$pid"
    done <"$interrupt_game_pid_file"
  fi
  [[ -z "$launcher_pid" ]] || wait "$launcher_pid" 2>/dev/null || true
  [[ -z "$unrelated_pid" ]] || wait "$unrelated_pid" 2>/dev/null || true
  rm -rf "$tmp"
  exit "$status"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP

fail() {
  printf 'retail_parity_lifecycle_test: %s\n' "$*" >&2
  exit 1
}

wait_for_file() {
  local path="$1" attempt
  for ((attempt = 0; attempt < 100; attempt++)); do
    [[ -e "$path" ]] && return 0
    sleep 0.05
  done
  fail "timed out waiting for $path"
}

wait_for_text() {
  local path="$1" expected="$2" attempt
  for ((attempt = 0; attempt < 100; attempt++)); do
    [[ -f "$path" ]] && grep -Fq "$expected" "$path" && return 0
    sleep 0.05
  done
  fail "timed out waiting for '$expected' in $path"
}

wait_for_process_exit() {
  local pid="$1" label="${2:-process}" attempt
  for ((attempt = 0; attempt < 100; attempt++)); do
    kill -0 "$pid" 2>/dev/null || return 0
    sleep 0.05
  done
  fail "$label $pid was not cleaned up"
}

write_exit_script() {
  local path="$1" status="$2"
  printf '%s\n' '#!/usr/bin/env bash' "exit $status" >"$path"
  chmod +x "$path"
}

write_msys_guard() {
  local path="$1" target="$2"
  printf '%s\n' '#!/usr/bin/env bash' '[[ "${MSYS2_ARG_CONV_EXCL:-}" == "*" ]] || exit 65' "exec \"$target\" \"\$@\"" >"$path"
  chmod +x "$path"
}

write_long_inspector() {
  local path="$1" marker="$2" pid_file="$3"
  printf '%s\n' \
    '#!/usr/bin/env bash' \
    "trap 'printf cleaned >\"$marker\"; exit 0' TERM INT HUP" \
    "printf '%s\\n' \"\$\$\" >\"$pid_file\"" \
    'while :; do sleep 0.1; done' >"$path"
  chmod +x "$path"
}

run_retail() {
  local inspector="$1" injector="$2"
  RETAIL_PARITY_ENV_FILE="$tmp/missing.env" \
  RETAIL_PARITY_RUN_ID=lifecycle-test \
  RETAIL_PARITY_TRACE_DIR="$tmp/traces" \
  RETAIL_PARITY_INSPECTOR="$inspector" \
  RETAIL_HOOK_INJECTOR="$injector" \
  JO_GAME_DIR='C:\fixture\Joint Operations' \
    bash "$cli" retail
}

# A collector that fails during the startup grace period must be reaped and its
# own status must be the launcher's status. No proprietary executable is used.
failed_inspector="$tmp/failed-inspector.sh"
successful_injector="$tmp/successful-injector.sh"
write_exit_script "$failed_inspector" 23
write_exit_script "$successful_injector" 0

set +e
printf '\n' | run_retail "$failed_inspector" "$successful_injector" \
  >"$tmp/failed-inspector.out" 2>&1
status=$?
set -e
[[ "$status" -eq 23 ]] ||
  fail "failed inspector status was $status, expected 23"

# A later foreground launch failure cleans up only the inspector owned by this
# launcher. An unrelated background process must remain alive.
inspector_cleanup_marker="$tmp/inspector-cleaned"
game_cleanup_marker="$tmp/game-cleaned"
inspector_pid_file="$tmp/inspector.pid"
game_pid_file="$tmp/game.pid"
long_inspector="$tmp/long-inspector.sh"
fixture_game="$tmp/fixture-game.sh"
stateful_injector="$tmp/stateful-injector.sh"
write_long_inspector "$long_inspector" "$inspector_cleanup_marker" "$inspector_pid_file"
write_long_inspector "$fixture_game" "$game_cleanup_marker" "$game_pid_file"
printf '%s\n' \
  '#!/usr/bin/env bash' \
  "if [[ ! -e \"$tmp/injector-called\" ]]; then" \
  "  : >\"$tmp/injector-called\"" \
  "  \"$fixture_game\" >/dev/null 2>&1 &" \
  '  game_pid=$!' \
  '  disown' \
  '  printf '\''JO_PROCESS_ID=%s\n'\'' "$game_pid"' \
  '  printf '\''Validated supported Jointops.exe and started process %s.\n'\'' "$game_pid"' \
  '  exit 0' \
  'fi' \
  'exit 31' >"$stateful_injector"
chmod +x "$stateful_injector"
raw_stateful_injector="$stateful_injector"
stateful_injector="$tmp/stateful-injector-guard.sh"
write_msys_guard "$stateful_injector" "$raw_stateful_injector"

sleep 30 &
unrelated_pid=$!
set +e
printf '\n' | run_retail "$long_inspector" "$stateful_injector" \
  >"$tmp/failed-injector.out" 2>&1
status=$?
set -e
[[ "$status" -eq 31 ]] ||
  fail "failed injector status was $status, expected 31"
wait_for_file "$inspector_cleanup_marker"
wait_for_file "$game_cleanup_marker"
inspector_pid="$(<"$inspector_pid_file")"
fixture_game_pid="$(<"$game_pid_file")"
wait_for_process_exit "$inspector_pid" inspector
wait_for_process_exit "$fixture_game_pid" game
fixture_game_pid=""
kill -0 "$unrelated_pid" 2>/dev/null ||
  fail 'launcher cleanup killed an unrelated process'
kill -TERM "$unrelated_pid" 2>/dev/null || true
wait "$unrelated_pid" 2>/dev/null || true
unrelated_pid=""

# Keep stdin open so the standalone launcher is waiting at its documented
# operator prompt, then model Ctrl-C with SIGINT. Its live inspector is owned
# and must be terminated before the launcher exits.
rm -f "$inspector_cleanup_marker" "$inspector_pid_file"
interrupt_injector="$tmp/interrupt-injector.sh"
printf '%s\n' \
  '#!/usr/bin/env bash' \
  "\"$fixture_game\" >/dev/null 2>&1 &" \
  'game_pid=$!' \
  'disown' \
  "printf '%s\\n' \"\$game_pid\" >>\"$interrupt_game_pid_file\"" \
  'printf '\''JO_PROCESS_ID=%s\n'\'' "$game_pid"' >"$interrupt_injector"
chmod +x "$interrupt_injector"
raw_interrupt_injector="$interrupt_injector"
interrupt_injector="$tmp/interrupt-injector-guard.sh"
write_msys_guard "$interrupt_injector" "$raw_interrupt_injector"
fifo="$tmp/operator-input"
mkfifo "$fifo"
exec 9<>"$fifo"
set -m
env \
  RETAIL_PARITY_ENV_FILE="$tmp/missing.env" \
  RETAIL_PARITY_RUN_ID=lifecycle-test \
  RETAIL_PARITY_TRACE_DIR="$tmp/traces" \
  RETAIL_PARITY_INSPECTOR="$long_inspector" \
  RETAIL_HOOK_INJECTOR="$interrupt_injector" \
  JO_GAME_DIR='C:\fixture\Joint Operations' \
  bash "$cli" retail <&9 >"$tmp/interrupted.out" 2>&1 &
launcher_pid=$!
wait_for_file "$inspector_pid_file"
wait_for_text "$tmp/interrupted.out" 'Press Enter after the baseline game windows are closed'
[[ "$(wc -l <"$interrupt_game_pid_file")" -eq 2 ]] ||
  fail 'both injector-reported game PIDs were not recorded before interrupt'
kill -INT "$launcher_pid"
for ((attempt = 0; attempt < 100; attempt++)); do
  kill -0 "$launcher_pid" 2>/dev/null || break
  sleep 0.05
done
if kill -0 "$launcher_pid" 2>/dev/null; then
  fail "interrupted launcher $launcher_pid did not exit"
fi
set +e
wait "$launcher_pid"
status=$?
set -e
launcher_pid=""
set +m
exec 9>&-
[[ "$status" -eq 130 ]] ||
  fail "interrupted launcher status was $status, expected 130"
wait_for_file "$inspector_cleanup_marker"
inspector_pid="$(<"$inspector_pid_file")"
wait_for_process_exit "$inspector_pid" inspector
while IFS= read -r pid; do
  wait_for_process_exit "$pid" interrupt-game
done <"$interrupt_game_pid_file"

printf 'retail_parity_lifecycle_test: OK\n'
