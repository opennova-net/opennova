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
nw_replay .scratch/capture.pcapng --print-roles
nw_replay .scratch/capture.pcapng --validate --items /path/to/items.def
```

## Env-Gated Witnesses

These tests often skip cleanly when local captures or retail assets are absent.
A skip is not coverage.

- `NW_INGAME_HEXCAP`
- `NW_PROFILE_SPH_DIR`
- `NW_DVXI5_PCAP`, `NW_DVXI3_PCAP`, `NW_DVXC1_PCAP`
- `NW_PROBE3AGAIN_PCAP`, `NW_PROBE3AGAIN_HOST_SPH`
- `NW_WHITENOISE_PCAP`
- `OPENNOVA_JO_DIR`
- `OPENNOVA_MISSION_CORPUS`
- `OPENNOVA_JO_ASSETS`, `JO_ASSETS_DIR`, `NOVA_RESOURCE_DIR`

## Logs and Cleanup

- Docker dev: collect `docker compose ... logs --tail=200 novaworld web`,
  `/api/server-info`, `/api/hosts`, `/api/lobbies`, and `/api/unknowns`.
- Native server: collect stdout/stderr. It clears active hosts and active user
  sessions at boot and sweeps stale hosts.
- Godot: fully close the editor after GDExtension rebuilds. If GUT reports odd
  failures, rerun the single test file before trusting full-suite noise.
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

