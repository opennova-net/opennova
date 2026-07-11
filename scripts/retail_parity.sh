#!/usr/bin/env bash
# One front door for collecting and comparing retail/OpenNova parity evidence.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
default_env_file="$root/.scratch/retail-parity.env"
env_file="${RETAIL_PARITY_ENV_FILE:-$default_env_file}"
expected_jo_sha256="9a1035440a53af2057ce0995ac42dced840d3b9fd53c04dc86041a962b84fe57"

usage() {
  cat <<'EOF'
Usage: scripts/retail_parity.sh [options] [command] [arguments]

Commands:
  doctor          Check the local Windows/Godot prerequisites
  setup           Create .scratch/retail-parity.env (never overwrites)
  build           Build the hook, inspector, parity tool, and Godot extension
  test            Run the Bash contract test and native validation tests
  retail          Launch and wait for the baseline retail-host + hooked-client leg
  opennova        Launch and wait for the candidate OpenNova-host + hooked-client leg
  compare         Compare [baseline.ontrace] [candidate.ontrace]
  dump            Dump [trace.ontrace] [output.txt]
  export-pcapng   Export [trace.ontrace] [output.pcapng]
  validate        Run the guided two-leg workflow and compare the evidence

Options:
  --dry-run             Print commands without launching or writing files
  --env <path>          Use a local environment file (default: .scratch/retail-parity.env)
  --scenario <name>     Scenario label (default: player-combat-loop)
  --mission <name>      Hosted mission (default: ASH_G3D.bms)
  --game-dir <path>     Persist JO_GAME_DIR during setup
  --godot-bin <path>    Persist GODOT_BIN during setup
  --force               Let setup replace an existing local environment file
  --allow-writes        Explicitly opt the retail hook into mutation access
  -h, --help            Show this help

Machine paths belong in the gitignored environment file or process environment.
EOF
}

die() {
  printf 'error: %s\n' "$*" >&2
  exit 64
}

# Pick --env before sourcing it. Command-line options are parsed fully below.
argv=("$@")
for ((i = 0; i < ${#argv[@]}; i++)); do
  case "${argv[$i]}" in
    --env)
      ((i + 1 < ${#argv[@]})) || die '--env requires a path'
      env_file="${argv[$((i + 1))]}"
      ((i += 1))
      ;;
    --env=*) env_file="${argv[$i]#--env=}" ;;
  esac
done

# The invoking environment wins over the local file. Keep path discovery and
# machine-specific configuration at this one boundary.
override_names=(
  JO_GAME_DIR GODOT_BIN OPENNOVA_RESOURCE_DIR RETAIL_PARITY_BUILD_DIR
  RETAIL_PARITY_TRACE_DIR RETAIL_PARITY_RUN_ID RETAIL_PARITY_SCENARIO
  RETAIL_PARITY_MISSION RETAIL_PARITY_INSPECTOR RETAIL_HOOK_INJECTOR
  OPENNOVA_PARITY_TOOL OPENNOVA_PARITY_BUILD_ID RETAIL_PARITY_SKIP_PREP
)
override_present=()
override_values=()
for name in "${override_names[@]}"; do
  if [[ -v "$name" ]]; then
    override_present+=("$name")
    override_values+=("${!name}")
  fi
done
if [[ -f "$env_file" ]]; then
  # shellcheck disable=SC1090 # machine-local path selected by the operator
  source "$env_file"
fi
for ((i = 0; i < ${#override_present[@]}; i++)); do
  printf -v "${override_present[$i]}" '%s' "${override_values[$i]}"
  export "${override_present[$i]}"
done

dry_run=0
allow_writes=0
scenario="${RETAIL_PARITY_SCENARIO:-player-combat-loop}"
mission="${RETAIL_PARITY_MISSION:-ASH_G3D.bms}"
command_name=""
command_args=()
setup_game_dir=""
setup_godot_bin=""
setup_force=0

while (($#)); do
  case "$1" in
    --dry-run) dry_run=1 ;;
    --allow-writes) allow_writes=1 ;;
    --env) shift; (($#)) || die '--env requires a path'; env_file="$1" ;;
    --env=*) env_file="${1#--env=}" ;;
    --scenario) shift; (($#)) || die '--scenario requires a name'; scenario="$1" ;;
    --scenario=*) scenario="${1#--scenario=}" ;;
    --mission) shift; (($#)) || die '--mission requires a name'; mission="$1" ;;
    --mission=*) mission="${1#--mission=}" ;;
    --game-dir) shift; (($#)) || die '--game-dir requires a path'; setup_game_dir="$1" ;;
    --game-dir=*) setup_game_dir="${1#--game-dir=}" ;;
    --godot-bin) shift; (($#)) || die '--godot-bin requires a path'; setup_godot_bin="$1" ;;
    --godot-bin=*) setup_godot_bin="${1#--godot-bin=}" ;;
    --force) setup_force=1 ;;
    -h|--help) usage; exit 0 ;;
    -*) die "unknown option: $1" ;;
    *)
      if [[ -z "$command_name" ]]; then command_name="$1"
      else command_args+=("$1")
      fi
      ;;
  esac
  shift
done

[[ -n "$command_name" ]] || command_name=validate
if [[ "$command_name" != setup ]] &&
   { [[ -n "$setup_game_dir" ]] || [[ -n "$setup_godot_bin" ]] || ((setup_force)); }; then
  die '--game-dir, --godot-bin, and --force are setup options'
fi
[[ "$scenario" =~ ^[A-Za-z0-9._-]+$ ]] || die "unsafe scenario name: $scenario"
[[ -n "$mission" ]] || die 'mission must not be empty'

run_id="${RETAIL_PARITY_RUN_ID:-$(date -u +%Y%m%d-%H%M%S)}"
[[ "$run_id" =~ ^[A-Za-z0-9._-]+$ ]] || die "unsafe run id: $run_id"
opennova_build_id="${OPENNOVA_PARITY_BUILD_ID:-}"
if [[ -z "$opennova_build_id" ]]; then
  opennova_revision="$(git -C "$root" rev-parse --short HEAD 2>/dev/null || true)"
  [[ -n "$opennova_revision" ]] || opennova_revision=worktree
  opennova_build_id="opennova-$opennova_revision"
fi
build_dir="${RETAIL_PARITY_BUILD_DIR:-$root/build-retail-parity}"
trace_root="${RETAIL_PARITY_TRACE_DIR:-$root/.scratch/retail-parity}"
run_dir="$trace_root/$scenario/$run_id"
baseline_trace="$run_dir/baseline.ontrace"
candidate_trace="$run_dir/candidate.ontrace"
baseline_pipe="opennova-retail-parity-$run_id-baseline"
candidate_pipe="opennova-retail-parity-$run_id-candidate"

to_windows_path() {
  local input="$1" drive rest
  if [[ "$input" =~ ^([A-Za-z]):[\\/](.*)$ ]]; then
    drive="${BASH_REMATCH[1]^^}"
    rest="${BASH_REMATCH[2]//\\//}"
  elif [[ "$input" =~ ^/([A-Za-z])/(.*)$ ]]; then
    drive="${BASH_REMATCH[1]^^}"
    rest="${BASH_REMATCH[2]}"
  elif [[ "$input" =~ ^/mnt/([A-Za-z])/(.*)$ ]]; then
    drive="${BASH_REMATCH[1]^^}"
    rest="${BASH_REMATCH[2]}"
  elif command -v cygpath >/dev/null 2>&1; then
    cygpath -w "$input"
    return
  else
    printf '%s\n' "$input"
    return
  fi
  rest="${rest//\//\\}"
  printf '%s:\\%s\n' "$drive" "$rest"
}

to_bash_path() {
  local input="$1" drive rest
  input="${input//\\//}"
  if [[ "$input" =~ ^([A-Za-z]):/(.*)$ ]]; then
    drive="${BASH_REMATCH[1],,}"
    rest="${BASH_REMATCH[2]}"
    printf '/%s/%s\n' "$drive" "$rest"
  else
    printf '%s\n' "$input"
  fi
}

print_command() {
  local prefix="$1"
  shift
  printf '%s' "$prefix"
  printf ' %s' "$@"
  printf '\n'
}

run_command() {
  if ((dry_run)); then print_command 'DRY_RUN' "$@"
  else "$@"
  fi
}

last_background_pid=""
owned_child_pids=()
owned_game_pids=()

track_owned_child() {
  owned_child_pids+=("$1")
}

untrack_owned_child() {
  local target="$1" pid
  local remaining=()
  for pid in "${owned_child_pids[@]}"; do
    [[ "$pid" == "$target" ]] || remaining+=("$pid")
  done
  owned_child_pids=("${remaining[@]}")
}

wait_owned_child() {
  local pid="$1" status=0
  [[ -n "$pid" && "$pid" != dry-run ]] || return 0
  if wait "$pid"; then status=0
  else status=$?
  fi
  untrack_owned_child "$pid"
  return "$status"
}

cleanup_owned_children() {
  local pid attempt running
  ((${#owned_child_pids[@]} || ${#owned_game_pids[@]})) || return 0

  for pid in "${owned_child_pids[@]}"; do
    [[ "$pid" =~ ^[0-9]+$ ]] || continue
    kill -0 "$pid" 2>/dev/null && kill -TERM "$pid" 2>/dev/null || true
  done
  for pid in "${owned_game_pids[@]}"; do
    [[ "$pid" =~ ^[0-9]+$ ]] || continue
    if kill -0 "$pid" 2>/dev/null; then
      kill -TERM "$pid" 2>/dev/null || true
    elif command -v taskkill.exe >/dev/null 2>&1; then
      taskkill.exe /PID "$pid" /T /F >/dev/null 2>&1 || true
    fi
  done

  for ((attempt = 0; attempt < 20; attempt++)); do
    running=0
    for pid in "${owned_child_pids[@]}"; do
      [[ "$pid" =~ ^[0-9]+$ ]] || continue
      if kill -0 "$pid" 2>/dev/null; then running=1; break; fi
    done
    if ((running == 0)); then
      for pid in "${owned_game_pids[@]}"; do
        [[ "$pid" =~ ^[0-9]+$ ]] || continue
        if kill -0 "$pid" 2>/dev/null; then running=1; break; fi
      done
    fi
    ((running)) || break
    sleep 0.05
  done

  for pid in "${owned_child_pids[@]}"; do
    [[ "$pid" =~ ^[0-9]+$ ]] || continue
    kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
    wait "$pid" 2>/dev/null || true
  done
  for pid in "${owned_game_pids[@]}"; do
    [[ "$pid" =~ ^[0-9]+$ ]] || continue
    kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
  done
  owned_child_pids=()
  owned_game_pids=()
}

launcher_exit() {
  local status="$1"
  trap - EXIT INT TERM HUP
  cleanup_owned_children
  exit "$status"
}

launcher_signal() {
  local signal="$1" status=130
  case "$signal" in
    HUP) status=129 ;;
    TERM) status=143 ;;
  esac
  printf 'error: interrupted by %s\n' "$signal" >&2
  exit "$status"
}

trap 'launcher_exit $?' EXIT
trap 'launcher_signal INT' INT
trap 'launcher_signal TERM' TERM
trap 'launcher_signal HUP' HUP

run_background() {
  if ((dry_run)); then
    print_command 'DRY_RUN_BACKGROUND' "$@"
    last_background_pid="dry-run"
  else
    "$@" &
    last_background_pid="$!"
    track_owned_child "$last_background_pid"
  fi
}

run_with_env_background() {
  local resource_dir="$1" pipe="$2" role="$3" hosted_mission="$4" trace="$5"
  shift 5
  if ((dry_run)); then
    print_command 'DRY_RUN_BACKGROUND' \
      "MSYS2_ARG_CONV_EXCL=*" \
      "OPENNOVA_RESOURCE_DIR=$resource_dir" \
      "OPENNOVA_PARITY_PIPE=$pipe" \
      "OPENNOVA_PARITY_ROLE=$role" \
      "OPENNOVA_PARITY_STREAM_ID=$role" \
      "OPENNOVA_PARITY_RUN_ID=$run_id" \
      "OPENNOVA_PARITY_BUILD_ID=$opennova_build_id" \
      "OPENNOVA_PARITY_SCENARIO=$scenario" \
      "OPENNOVA_PARITY_TITLE=joint-operations" \
      "OPENNOVA_PARITY_EXPANSION=jox01" \
      "OPENNOVA_PARITY_MISSION=$hosted_mission" \
      "OPENNOVA_PARITY_TRACE=$trace" \
      "NW_LAN_HOST=$hosted_mission" "$@"
    last_background_pid="dry-run"
  else
    env "MSYS2_ARG_CONV_EXCL=*" \
      "OPENNOVA_RESOURCE_DIR=$resource_dir" \
      "OPENNOVA_PARITY_PIPE=$pipe" \
      "OPENNOVA_PARITY_ROLE=$role" \
      "OPENNOVA_PARITY_STREAM_ID=$role" \
      "OPENNOVA_PARITY_RUN_ID=$run_id" \
      "OPENNOVA_PARITY_BUILD_ID=$opennova_build_id" \
      "OPENNOVA_PARITY_SCENARIO=$scenario" \
      "OPENNOVA_PARITY_TITLE=joint-operations" \
      "OPENNOVA_PARITY_EXPANSION=jox01" \
      "OPENNOVA_PARITY_MISSION=$hosted_mission" \
      "OPENNOVA_PARITY_TRACE=$trace" \
      "NW_LAN_HOST=$hosted_mission" "$@" &
    last_background_pid="$!"
    track_owned_child "$last_background_pid"
  fi
}

find_tool() {
  local filename="$1" override="${2:-}" candidate found
  if [[ -n "$override" ]]; then printf '%s\n' "$(to_bash_path "$override")"; return; fi
  for candidate in \
    "$build_dir/tools/retail_hook/windows/Release/$filename" \
    "$build_dir/tools/retail_hook/Release/$filename" \
    "$build_dir/tools/parity/Release/$filename" \
    "$build_dir/Release/$filename"; do
    [[ -f "$candidate" ]] && { printf '%s\n' "$candidate"; return; }
  done
  if [[ -d "$build_dir" ]]; then
    found="$(find "$build_dir" -type f -iname "$filename" -print -quit 2>/dev/null || true)"
    [[ -n "$found" ]] && { printf '%s\n' "$found"; return; }
  fi
  if ((dry_run)); then printf '%s\n' "$filename"
  else die "built tool not found: $filename (run setup, then build)"
  fi
}

inspector_path() { find_tool retail_parity_inspector.exe "${RETAIL_PARITY_INSPECTOR:-}"; }
injector_path() { find_tool retail_hook_injector.exe "${RETAIL_HOOK_INJECTOR:-}"; }
parity_tool_path() { find_tool opennova_parity_tool.exe "${OPENNOVA_PARITY_TOOL:-}"; }

game_dir_windows() {
  [[ -n "${JO_GAME_DIR:-}" ]] || { ((dry_run)) && { printf '<JO_GAME_DIR>\n'; return; }; die 'set JO_GAME_DIR'; }
  to_windows_path "$JO_GAME_DIR"
}

godot_windows() {
  [[ -n "${GODOT_BIN:-}" ]] || { ((dry_run)) && { printf '<GODOT_BIN>\n'; return; }; die 'set GODOT_BIN'; }
  to_windows_path "$GODOT_BIN"
}

resource_dir_windows() {
  local value="${OPENNOVA_RESOURCE_DIR:-${JO_GAME_DIR:-}}"
  [[ -n "$value" ]] || { ((dry_run)) && { printf '<OPENNOVA_RESOURCE_DIR>\n'; return; }; die 'set OPENNOVA_RESOURCE_DIR or JO_GAME_DIR'; }
  to_windows_path "$value"
}

command_setup() {
  local game_value="${setup_game_dir:-${JO_GAME_DIR:-}}"
  local godot_value="${setup_godot_bin:-${GODOT_BIN:-}}"
  printf 'ENV_FILE=%s\n' "$env_file"
  if ((dry_run)); then
    printf 'DRY_RUN create environment template (no machine paths written)\n'
    return
  fi
  if [[ -e "$env_file" ]] && ((setup_force == 0)); then
    printf 'SETUP=kept-existing\n'
    return
  fi
  mkdir -p "$(dirname "$env_file")"
  {
    printf '%s\n' '# Local-only retail parity configuration (this file is gitignored).'
    printf '%s\n' '# Values use Bash syntax; quote Windows paths containing spaces.'
    printf 'JO_GAME_DIR=%q\n' "$game_value"
    printf 'GODOT_BIN=%q\n' "$godot_value"
    printf 'OPENNOVA_RESOURCE_DIR=%q\n' "${OPENNOVA_RESOURCE_DIR:-}"
    printf 'RETAIL_PARITY_BUILD_DIR=%q\n' "${RETAIL_PARITY_BUILD_DIR:-}"
    printf 'RETAIL_PARITY_TRACE_DIR=%q\n' "${RETAIL_PARITY_TRACE_DIR:-}"
  } >"$env_file"
  if ((setup_force)); then printf 'SETUP=replaced\n'
  else printf 'SETUP=created\n'
  fi
}

check_command() {
  local name="$1" found
  if found="$(command -v "$name" 2>/dev/null)"; then
    printf 'CHECK=%s STATUS=ok PATH=%s\n' "$name" "$found"
  elif ((dry_run)); then
    printf 'CHECK=%s STATUS=dry-run\n' "$name"
  else
    printf 'CHECK=%s STATUS=missing\n' "$name" >&2
    return 1
  fi
}

command_doctor() {
  local failures=0 game_bash="" godot_bash
  for dependency in bash cmake ctest python; do check_command "$dependency" || ((failures += 1)); done
  if [[ -n "${JO_GAME_DIR:-}" ]]; then
    game_bash="$(to_bash_path "$JO_GAME_DIR")/Jointops.exe"
    if [[ -f "$game_bash" ]] || ((dry_run)); then printf 'CHECK=JO_GAME_DIR STATUS=ok PATH=%s\n' "$(game_dir_windows)"
    else printf 'CHECK=JO_GAME_DIR STATUS=missing-jointops PATH=%s\n' "$JO_GAME_DIR" >&2; ((failures += 1))
    fi
  else
    if ((dry_run)); then printf 'CHECK=JO_GAME_DIR STATUS=dry-run\n'
    else printf 'CHECK=JO_GAME_DIR STATUS=missing\n' >&2; ((failures += 1))
    fi
  fi
  if [[ -n "${GODOT_BIN:-}" ]]; then
    godot_bash="$(to_bash_path "$GODOT_BIN")"
    if [[ -f "$godot_bash" ]] || ((dry_run)); then printf 'CHECK=GODOT_BIN STATUS=ok PATH=%s\n' "$(godot_windows)"
    else printf 'CHECK=GODOT_BIN STATUS=missing PATH=%s\n' "$GODOT_BIN" >&2; ((failures += 1))
    fi
  else
    if ((dry_run)); then printf 'CHECK=GODOT_BIN STATUS=dry-run\n'
    else printf 'CHECK=GODOT_BIN STATUS=missing\n' >&2; ((failures += 1))
    fi
  fi

  if ((dry_run)); then
    printf 'CHECK=JO_EXECUTABLE_SHA256 STATUS=dry-run EXPECTED=%s\n' "$expected_jo_sha256"
    printf 'CHECK=EXPANSION STATUS=dry-run NAME=jox01\n'
    printf 'CHECK=MISSION STATUS=dry-run NAME=%s SOURCE=packed\n' "$(basename "$mission")"
  elif [[ -n "$game_bash" && -f "$game_bash" ]] && command -v python >/dev/null 2>&1; then
    if ! python "$root/scripts/retail_parity/doctor_resources.py" \
      --game-root "$(dirname "$game_bash")" \
      --expected-sha "$expected_jo_sha256" \
      --expansion jox01 \
      --mission "$mission"; then
      ((failures += 1))
    fi
  else
    printf 'CHECK=JO_EXECUTABLE_SHA256 STATUS=unavailable EXPECTED=%s\n' "$expected_jo_sha256" >&2
    printf 'CHECK=EXPANSION STATUS=unavailable NAME=jox01\n' >&2
    printf 'CHECK=MISSION STATUS=unavailable NAME=%s SOURCE=packed\n' "$(basename "$mission")" >&2
  fi
  ((failures == 0)) || return 64
}

command_build() {
  run_command cmake -S "$root" -B "$build_dir" -A Win32 \
    -DBUILD_SHARED_LIB=OFF \
    -DOPENNOVA_ENABLE_MODSUPEROED_TOOLS=OFF \
    -DOPENNOVA_ENABLE_RETAIL_HOOK_TOOLS=ON
  run_command cmake --build "$build_dir" --config Release --target \
    retail_hook_validation_test retail_hook_capture_agent_test \
    retail_hook_winsock_capture_test retail_hook_named_pipe_trace_test \
    retail_hook_cli_test parity_trace_test parity_compare_test \
    parity_capture_test parity_file_event_sink_test parity_tool_cli_test \
    parity_tool_network_test parity_tool_export_test parity_tool_compare_test \
    parity_opennova_capture_test opennova_retail_hook \
    retail_hook_injector retail_parity_inspector opennova_parity_tool
  run_command bash "$root/scripts/build_godot.sh" Debug
}

command_test() {
  run_command bash -n "$root/scripts/retail_parity.sh"
  run_command bash "$root/tests/scripts/retail_parity_cli_test.sh"
  run_command bash "$root/tests/scripts/retail_parity_doctor_test.sh"
  run_command bash "$root/tests/scripts/retail_parity_lifecycle_test.sh"
  run_command ctest --test-dir "$build_dir" -C Release --output-on-failure \
    -R '^(retail_hook_|parity_)'
}

remember_trace() {
  local role="$1" trace="$2" pointer
  pointer="$trace_root/latest-$role.path"
  ((dry_run)) && return
  mkdir -p "$trace_root"
  printf '%s\n' "$trace" >"$pointer"
}

latest_trace() {
  local role="$1" fallback="$2" pointer
  pointer="$trace_root/latest-$role.path"
  if [[ -f "$pointer" ]]; then
    local value
    IFS= read -r value <"$pointer"
    [[ -n "$value" ]] && { printf '%s\n' "$value"; return; }
  fi
  printf '%s\n' "$fallback"
}

start_inspector() {
  local role_set="$1" pipe="$2" trace="$3" inspector pid status
  inspector="$(inspector_path)"
  run_background "$inspector" --pipe "$pipe" --trace "$trace" \
    --scenario "$scenario" --role-set "$role_set"
  ((dry_run)) || sleep 1
  pid="$last_background_pid"
  if ((dry_run == 0)) && ! kill -0 "$pid" 2>/dev/null; then
    if wait_owned_child "$pid"; then
      printf 'error: inspector exited before capture startup completed\n' >&2
      return 64
    else
      status=$?
      printf 'error: inspector exited before capture startup completed (status=%d)\n' "$status" >&2
      return "$status"
    fi
  fi
}

start_retail_role() {
  local role="$1" pipe="$2" injector game_dir output line process_id="" id_count=0 status
  injector="$(injector_path)"
  game_dir="$(game_dir_windows)"
  args=("$injector" --game-dir "$game_dir" --role "$role" --pipe "$pipe"
    --run-id "$run_id" --stream-id "$role"
    --scenario "$scenario" --mission "$mission")
  ((allow_writes)) && args+=(--allow-writes)
  args+=(-- /w /exp jox01 /MANY)
  if ((dry_run)); then
    run_command "${args[@]}"
    return
  fi
  if output="$(MSYS2_ARG_CONV_EXCL='*' "${args[@]}")"; then status=0
  else status=$?
  fi
  [[ -z "$output" ]] || printf '%s\n' "$output"
  ((status == 0)) || return "$status"
  while IFS= read -r line; do
    line="${line%$'\r'}"
    if [[ "$line" =~ ^JO_PROCESS_ID=([0-9]+)$ ]]; then
      process_id="${BASH_REMATCH[1]}"
      ((id_count += 1))
    fi
  done <<<"$output"
  if ((id_count != 1)); then
    printf 'error: injector success for %s emitted %d JO_PROCESS_ID records; expected exactly one\n' \
      "$role" "$id_count" >&2
    return 64
  fi
  owned_game_pids+=("$process_id")
}

print_combat_guide() {
  printf 'GUIDE_ACTOR=retail-client\n'
  printf 'GUIDE_STEP=idle-at-spawn ACTION=wait several seconds with a ready weapon\n'
  printf 'GUIDE_STEP=move-stance-look ACTION=walk, run, turn, crouch, prone, and jump\n'
  printf 'GUIDE_STEP=aim-fire-reload ACTION=aim or scope, fire a short burst, then reload\n'
  printf 'GUIDE_STEP=damage-and-settle ACTION=take damage if practical, then return to idle\n'
}

command_retail() {
  printf 'SCENARIO=%s\nMISSION=%s\n' "$scenario" "$mission"
  printf 'ROLE_SET=baseline TOPOLOGY=retail-host+hooked-client\n'
  printf 'TRACE=%s\n' "$baseline_trace"
  ((dry_run)) || mkdir -p "$run_dir"
  start_inspector baseline "$baseline_pipe" "$baseline_trace"
  local inspector_pid="$last_background_pid"
  start_retail_role retail-host "$baseline_pipe"
  start_retail_role retail-client "$baseline_pipe"
  last_background_pid="$inspector_pid"
  remember_trace baseline "$baseline_trace"
  printf 'GUIDE=On the retail host select %s; perform %s, then close both game windows.\n' "$mission" "$scenario"
  print_combat_guide
}

command_opennova() {
  printf 'SCENARIO=%s\nMISSION=%s\n' "$scenario" "$mission"
  printf 'ROLE_SET=candidate TOPOLOGY=opennova-host+hooked-client\n'
  printf 'TRACE=%s\n' "$candidate_trace"
  ((dry_run)) || mkdir -p "$run_dir"
  start_inspector candidate "$candidate_pipe" "$candidate_trace"
  local inspector_pid="$last_background_pid"
  local resource_dir godot_win godot_exec godot_command godot_project_win trace_win
  resource_dir="$(resource_dir_windows)"
  godot_win="$(godot_windows)"
  godot_exec="$(to_bash_path "$godot_win")"
  godot_command="$godot_exec"
  ((dry_run)) && godot_command="$godot_win"
  godot_project_win="$(to_windows_path "$root/godot")"
  trace_win="$(to_windows_path "$candidate_trace")"
  run_with_env_background "$resource_dir" "$candidate_pipe" opennova-host "$mission" "$trace_win" \
    "$godot_command" --path "$godot_project_win" -- /w /exp jox01
  start_retail_role retail-client "$candidate_pipe"
  last_background_pid="$inspector_pid"
  remember_trace candidate "$candidate_trace"
  printf 'GUIDE=Join the OpenNova host from the hooked retail client; perform %s, then close both windows.\n' "$scenario"
  print_combat_guide
}

parity_validate() {
  local tool
  tool="$(parity_tool_path)"
  run_command "$tool" validate "$1"
}

command_compare() {
  local reference="${command_args[0]:-$(latest_trace baseline "$baseline_trace")}" \
        candidate="${command_args[1]:-$(latest_trace candidate "$candidate_trace")}" tool
  tool="$(parity_tool_path)"
  run_command "$tool" compare "$reference" "$candidate"
}

command_dump() {
  local trace="${command_args[0]:-$(latest_trace candidate "$candidate_trace")}" tool
  tool="$(parity_tool_path)"
  if ((${#command_args[@]} >= 2)); then run_command "$tool" dump "$trace" "${command_args[1]}"
  else run_command "$tool" dump "$trace"
  fi
}

command_export_pcapng() {
  local trace="${command_args[0]:-$(latest_trace candidate "$candidate_trace")}" \
        output="${command_args[1]:-}" tool
  [[ -n "$output" ]] || output="${trace%.ontrace}.pcapng"
  tool="$(parity_tool_path)"
  run_command "$tool" export-pcapng "$trace" "$output"
}

finish_guided_leg() {
  local role_set="$1" pid="$2" status
  ((dry_run)) && { printf 'GUIDE_WAIT=%s\n' "$role_set"; return; }
  printf 'Press Enter after the %s game windows are closed... ' "$role_set"
  IFS= read -r _
  if wait_owned_child "$pid"; then
    owned_game_pids=()
  else
    status=$?
    return "$status"
  fi
}

command_validate() {
  printf 'WORKFLOW=guided-retail-parity\n'
  if [[ "${RETAIL_PARITY_SKIP_PREP:-0}" != 1 ]]; then
    command_doctor
    command_build
    command_test
  fi
  command_retail
  local baseline_pid="$last_background_pid"
  finish_guided_leg baseline "$baseline_pid"
  command_opennova
  local candidate_pid="$last_background_pid"
  finish_guided_leg candidate "$candidate_pid"
  parity_validate "$baseline_trace"
  parity_validate "$candidate_trace"
  command_args=("$baseline_trace" "$candidate_trace")
  command_compare
}

case "$command_name" in
  doctor) command_doctor ;;
  setup) command_setup ;;
  build) command_build ;;
  test) command_test ;;
  retail)
    command_retail
    finish_guided_leg baseline "$last_background_pid"
    ;;
  opennova)
    command_opennova
    finish_guided_leg candidate "$last_background_pid"
    ;;
  compare) command_compare ;;
  dump) command_dump ;;
  export-pcapng) command_export_pcapng ;;
  validate) command_validate ;;
  *) die "unknown command: $command_name" ;;
esac
