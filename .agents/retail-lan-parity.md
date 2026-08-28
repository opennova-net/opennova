# Retail LAN parity probes

Use this runbook to probe the four Joint Operations LAN paths:

1. retail host to retail joiner (`RR`);
2. retail host to OpenNova joiner (`RO`);
3. OpenNova host to retail joiner (`OR`);
4. OpenNova host to OpenNova joiner (`OO`).

Two MCP servers, one client layer. The onHook MCP bridge is the only retail
driver: it is NOT a registered MCP server in this environment and never
appears in an agent's tool list; it is an external `opennova-int` executable
(`onhook-mcp.exe`) that the repository adapter shells out to, located by the
mandatory `-OnHookMcpPath` argument. An agent that does not have that
executable on disk cannot run any retail leg of this runbook and must report
the parity leg as BLOCKED rather than hunt for an onHook tool or substitute a
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

- Use two distinct retail copies for SERVER and CLIENT.
- Keep retail files, captures, hook logs, screenshots, account data, machine
  paths, adapter addresses, and DLLs beneath the ignored `.scratch/` tree.
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
    -HostSessionName 'Untitled ' `
    -ReadinessMode in_match `
    -RunId 01tr-rr-001 `
    -RetailServerRoot C:\retail\SERVER `
    -RetailClientRoot C:\retail\CLIENT `
    -OnHookMcpPath C:\opennova-int\onhook\onhook-mcp.exe `
    -GodotLauncher C:\Godot\Godot_v4.6.1-stable_win64_console.exe `
    -HostMcpPort 8975 -JoinerMcpPort 8976
```

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
`onhook_join_lan` tools and stop with a named `UPSTREAM BLOCKER` error until
those tools return `pid`/`instance_id`/`run_id`/`capture_path` like
`onhook_run_lan_pair` and render `onhook.cfg` from their arguments (the
opennova-int items in `TODO.md`); the `RR` in-match cell rides
`onhook_run_lan_pair` today.

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
- Captures decode with the native `nw_pp` application.
- Mismatches are recorded in `docs/net/novaworld-net-re.md`; absence of a
  decoder warning is not itself a parity verdict.
- Only sanitized conclusions and structural fixtures enter Git. Raw local
  evidence remains under `.scratch/`.
