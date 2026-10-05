# Retail LAN parity probes

Use this runbook to probe the four Joint Operations LAN paths:

1. retail host to retail joiner (`RR`);
2. retail host to OpenNova joiner (`RO`);
3. OpenNova host to retail joiner (`OR`);
4. OpenNova host to OpenNova joiner (`OO`).

Two MCP servers, one client layer. The onHook MCP bridge is the only retail
driver: an external `opennova-int` executable (`onhook-mcp.exe`) that the
repository adapter shells out to, located by the mandatory `-OnHookMcpPath`
argument. A session may also have it registered as an MCP server (the
`onhook_*` tools), but the runner never depends on that registration. An agent
that does not have that executable on disk cannot run any retail leg of this
runbook and must report the parity leg as BLOCKED rather than substitute a
different probe. OpenNova endpoints are driven through the game's own
`opennova-game` MCP (`docs/mcp.md`): the runner launches each OpenNova role on
launch flags with `--mcp-port` (host 8975, joiner 8976 by default;
`-HostMcpPort`/`-JoinerMcpPort`) and reads readiness, the motion exercise and
the cooperative stop through it. The former Python corpus exporter, generated
matrix, manifest, verifier, the manual retail launchers and the file-IPC
joiner driver are retired. A run is evidence only for the exact mission, game
type, expansion, binaries, retail trees, hook build, port, and callsigns
recorded with it.

## Safety and evidence

- The retail reference is the maintainer's JO:CA install (`jox01`,
  `Jointops.exe` SHA-256 `9a1035440a53af2057ce0995ac42dced840d3b9fd53c04dc86041a962b84fe57`,
  code-identical to the IDB image; ADR 0050 decision 2). Keep it pristine: the
  SERVER and CLIENT copies below are hash-verified copies of it, and onHook is
  deployed only into the copies.
- Use two distinct retail copies for SERVER and CLIENT; the runner refuses
  copies whose `Jointops.exe` hashes differ and records the hash
  (`retail_exe_sha256`).
- Deploy the onHook debug proxy (bridge protocol 1.8 or later) into both
  copies: its `binkw32.dll` with the original Bink DLL renamed to
  `binkw32_.dll`, the forwarding filename the proxy expects, and pass the
  matching `onhook-mcp.exe` as `-OnHookMcpPath`.
- Retail runs stock. Every retail role loads with onHook's `StockObserver`:
  every behavior-changing hook and patch off, only the instruments (bridge,
  LAN automation, passive capture, read-only witnesses) installed. The loaded
  hook must confirm it (`stock_observer`), and the runner snapshots each role's
  rendered `onhook.cfg` with its SHA-256 (`effective_retail_configs`).
  `-AllowModifiedRetail` drops StockObserver for a diagnostic run only. OpenNova
  never ports onHook patches; the hook only observes and drives retail. A
  modded expansion such as revx02 (2,455 item definitions, above retail's
  unpatched 2,048) needs onHook's capacity patches and is therefore never a
  stock reference.
- Keep retail files, captures, hook logs, screenshots, account data, machine
  paths, adapter addresses, and DLLs machine-local and gitignored.
- Record the repository commit, onHook commit, deployed proxy SHA-256, Godot
  binary, mission, numeric game type, and topology with every result.
- Use a unique `RunId`; probe output is create-new and must not overwrite an
  earlier run.
- Stop a retail joiner before its host. Use MCP-owned instance and run IDs,
  never process-name guesses.
- A launched process or bound socket is not readiness. Require the bridge's
  authenticated peer/responder and requested `in_match` or `deploy_hold`
  state; on the OpenNova side the `parity_joiner_ready` witness classified by
  `scripts/net/lib.ps1`.

## Single-topology runner

`scripts/net/run_parity_topology.ps1` is the repository adapter around the
onHook MCP executable and the OpenNova launchers. It runs one explicit
topology; there is intentionally no second matrix definition or generated
manifest.

```powershell
pwsh -File scripts/net/run_parity_topology.ps1 `
    -Topology RR `
    -Mission 01TR.bms `
    -Port 32786 `
    -GameType 65568 `
    -HostSessionName 'Untitled' `
    -ReadinessMode in_match `
    -RunId 01tr-rr-001 `
    -MissionArtifactPath C:\retail\corpus\missions\01TR.bms `
    -MissionSha256 ((Get-FileHash C:\retail\corpus\missions\01TR.bms -Algorithm SHA256).Hash.ToLowerInvariant()) `
    -RetailServerRoot C:\retail\jox01\SERVER `
    -RetailClientRoot C:\retail\jox01\CLIENT `
    -Expansion jox01 `
    -OnHookMcpPath C:\opennova-int\onhook\onhook-mcp.exe `
    -GodotLauncher C:\Godot\Godot_v4.6.1-stable_win64_console.exe `
    -HostMcpPort 8975 -JoinerMcpPort 8976
```

The mission artifact must be the exact BMS used by the installed expansion.
Place matching extracted `items.def` one directory above its `missions/`
directory; the packet readiness gate uses both files to validate mission
identity and decode entity types. Extract both with the expansion layered,
e.g. `opennova-extract --game <install> /exp jox01 --out <corpus>\missions 01TR.bms`
and `... --out <corpus> items.def` (from git-bash, set `MSYS_NO_PATHCONV=1` or
`/exp` is rewritten as a path and the base `items.def` comes out).
`HostSessionName` must equal the SERVER copy's `game.cfg` `game_name` exactly,
the name a retail host advertises (JO:CA ships `Untitled`).

Run the command once for each of `RR`, `RO`, `OR`, and `OO`, changing `RunId`
for every cell while holding all other case inputs fixed. Ports supported by
the current retail automation are 32786 through 32789. Numeric game type zero
is valid Deathmatch; `auto` is discovery-only and is not a verdict-bearing
input. Run the command from a real terminal: the Godot console wrapper stalls
from a shell without a console, and a verdict-bearing run needs the wrapper's
ownership proof (`-AllowDirectRuntime` relaxes it for diagnostics only and is
recorded in the summary).

The OpenNova joiner's readiness is `game_probe parity_joiner_ready` (blocking;
`-AutoDeploy` sends the default deployment pick), every later snapshot is
`parity_joiner_state`, an exercised run's motion gate is `parity_joiner_motion`
inside the steady window, and the cooperative stop is `game_probe cancel` ->
`game_control quit` -> the process-shutdown evidence; the summary carries the
probe run ids (`joiner_ready_run_id`, `joiner_motion_run_id`), the MCP ports and
the witnesses (`opennova.parity-motion-gate.v2`,
`opennova.parity-joiner-shutdown.v2`). The OpenNova host's `game_state` rides
in the acceptance record.

Retail roles are launched only through onhook-mcp. `RR`/`RO` in `deploy_hold`
readiness and the `OR` retail joiner call the single-role `onhook_host_lan` /
`onhook_join_lan` tools; the `RR` in-match cell rides `onhook_run_lan_pair`.
Each returns `pid`/`instance_id`/`run_id`/`capture_path`/`hook_config_path`
and `stock_observer`, and a launch missing any of them stops the cell. A
`deploy_hold` retail joiner is launched with `readiness: "peer"`: an A&S
joiner holds on the deploy screen with no local Person, which the in-match
readiness waits for.

The OpenNova roles answer and validate retail's anti-cheat CRC challenges
only from an explicit `-IntegrityProfile` reproduced from the same retail
corpus (D-NET-181). The revx02 corpus has `retail-revx02-024f56f2-2d087374`;
the jox01 reference has none yet, so its default is empty: our host stays
silent on CRC replies and our joiner does not answer a retail host's
challenges. Passing the revx02 profile against jox01 makes our host punt a
retail joiner on the first mismatched reply.

For a cold retail-to-retail session, the lower-level call is
`onhook_run_lan_pair`, a method of the external onHook executable and invoked
through it, not a tool the agent can call directly. Preserve both returned run
and instance IDs and stop them with `onhook_stop_run`. Prefer the repository topology runner when
comparing all four paths because it applies the same capture, readiness, and
artifact layout to each cell.

## Acceptance

- The same case inputs were used for all four topologies.
- Every retail endpoint reports the expected role, exact run ID, capture state,
  port, responder/peer readiness, and mission state through MCP; every OpenNova
  endpoint reports its role through `game_state` and its readiness witness
  through the parity probes.
- Captures decode with the native `opennova-wire` application.
- Mismatches are recorded in `docs/net/novaworld-net-re.md`; absence of a
  decoder warning is not itself a parity verdict.
- Only sanitized conclusions and structural fixtures enter Git. Raw local
  evidence stays machine-local.
