# Debugging and Verification

Use this file to select checks and avoid mistaking skipped witnesses for tested
behavior.

## Baseline Checks

- `git status --short --branch`
- Confirm `GODOT_BIN --version` for Godot/GUT work.
- Confirm LFS/submodules are materialized before asset or fixture work.
- Confirm free ports before server work: `7597/udp`, `64206/udp`, `8080/tcp`,
  and `5173/tcp`.
- Confirm `ADMIN_API_TOKEN` before using admin endpoints.
- Confirm `tshark`/Npcap only when converting or acquiring captures outside
  the native `opennova-wire` reader.

## Performance Build Invariant

Always build the editor/F5 runtime used for FPS or frame-time measurements with:

```bash
bash scripts/build_godot.sh
```

The command must report `Dev -> CMake config RelWithDebInfo`. Do not benchmark a
DLL produced by `cmake --build godot/src/build --config Debug`: that is the
`DebugFull` `/Od` + runtime-check build, even though it has the same
`template_debug` artifact name, and it can make native simulation 5-10x slower.
`DebugFull` is only for native stepping and memory-corruption diagnosis. Rebuild
with the canonical Dev command before every live performance comparison.

## Common Commands

Library and net-focused CTest:

```bash
ctest --test-dir build --output-on-failure -C Release -R "novaworld|nw_|napi|session|gate|lobby|gsb|protocol|netsim"
```

Godot net work:

```bash
scripts/build_godot.sh
"$GODOT_BIN" --headless --path godot --import
scripts/test_godot.sh
```

Packet tools:

```bash
opennova-wire capture.pcapng --stream --items /path/to/items.def
```

## Retail Witnesses

No test reads retail data or a machine-local capture: the suite runs on minted
and authored fixtures (the wire fixtures in `fixtures/novaworld/`), with a
witnessed value pinned as a cited constant. A local retail run (a capture, an
install for `--resource-dir`) is a tool argument, never an environment variable.

## Logs and Cleanup

- Docker dev: collect `docker compose ... logs --tail=200 novaworld web`,
  `/api/health`, `/api/hosts`, `/api/lobbies`, and `/api/unknowns` (`/api/hosts` and
  `/api/unknowns` take the Bearer `ADMIN_API_TOKEN`).
- Native server: collect stdout/stderr. It clears active hosts and active user
  sessions at boot and sweeps stale hosts.
- Godot: fully close the editor after GDExtension rebuilds. If GUT reports odd
  failures, rerun the single test file before trusting full-suite noise.
- Visual Studio multi-config generators IGNORE `CMAKE_BUILD_TYPE`: judge a build's
  flavor by the selected `--config` and the timestamps of the corresponding
  `build/engine/*/opennova_*.dir/<Config>/*.obj`, never by cache variables or
  directory existence. For performance observations never use a Debug config;
  MSVC `/Od` and `/RTC1` invalidate frame-time readings. `scripts/build.sh`
  builds the main tree with `--config Release`.
- Hosts file: remove the `gs.novaworld.net` redirect line you added when the run is
  done (or switch the OpenNova Switcher back to **NovaLogic NovaWorld**), and touch no
  other hosts entry.

## When To Stop

Stop and report instead of guessing when:

- A capture lacks handshake packets needed for SCRK/session recovery.
- A packet field is not witnessed in `docs/net/novaworld-net-re.md`.
- IDA is needed but unavailable.
- A retail run needs admin hosts edits, firewall changes, credentials, or a
  visible desktop operator.
- A refactor would change packet order, byte layout, DCB/handle semantics, or
  load timing without a retail witness.
