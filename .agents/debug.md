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
  the native `nw_pp` reader.

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

Launcher work:

```powershell
dotnet test launcher/OpenNovaLauncher.sln -c Release
```

Packet tools:

```bash
nw_pp .scratch/capture.pcapng --stream --items /path/to/items.def
```

## Env-Gated Witnesses

These tests often skip cleanly when local captures or retail assets are absent.
A skip is not coverage.

- `OPENNOVA_JO_DIR` (a packed retail install)
- `OPENNOVA_JO_ASSETS` (an extracted retail tree, its `.bms` missions loose at the root)

The two roots are the whole registry ([docs/asset-gated-tests.md](../docs/asset-gated-tests.md));
a gated ctest reports Skipped (exit 77) without its root, never a silent pass.

## Logs and Cleanup

- Docker dev: collect `docker compose ... logs --tail=200 novaworld web`,
  `/api/server-info`, `/api/hosts`, `/api/lobbies`, and `/api/unknowns`.
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
- Launcher: use the launcher to remove managed hosts entries. Inspect only the
  OpenNova marker block in the hosts file, and do not commit launcher settings.

## When To Stop

Stop and report instead of guessing when:

- A capture lacks handshake packets needed for SCRK/session recovery.
- A packet field is not witnessed in `docs/net/novaworld-net-re.md`.
- IDA is needed but unavailable.
- A retail run needs admin hosts edits, firewall changes, credentials, or a
  visible desktop operator.
- A refactor would change packet order, byte layout, DCB/handle semantics, or
  load timing without a retail witness.
