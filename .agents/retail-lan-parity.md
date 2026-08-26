# Retail LAN parity probes

Use this runbook to probe the four Joint Operations LAN paths:

1. retail host to retail joiner (`RR`);
2. retail host to OpenNova joiner (`RO`);
3. OpenNova host to retail joiner (`OR`);
4. OpenNova host to OpenNova joiner (`OO`).

The onHook MCP bridge is the primary retail probe. The former Python corpus
exporter, generated matrix, manifest, and verifier are retired. A run is
evidence only for the exact mission, game type, expansion, binaries, retail
trees, hook build, port, and callsigns recorded with it.

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
  state.

## Single-topology runner

`scripts/net/run_parity_topology.ps1` is the repository adapter around the
onHook MCP executable and the native OpenNova launchers. It runs one explicit
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
    -GodotLauncher C:\Godot\Godot_v4.6.1-stable_win64_console.exe
```

Run the command once for each of `RR`, `RO`, `OR`, and `OO`, changing `RunId`
for every cell while holding all other case inputs fixed. Ports supported by
the current retail automation are 32786 through 32789. Numeric game type zero
is valid Deathmatch; `auto` is discovery-only and is not a verdict-bearing
input.

For a cold retail-to-retail session, the lower-level MCP call is
`onhook_run_lan_pair`. Preserve both returned run and instance IDs and stop
them with `onhook_stop_run`. Prefer the repository topology runner when
comparing all four paths because it applies the same capture, readiness, and
artifact layout to each cell.

## Acceptance

- The same case inputs were used for all four topologies.
- Every retail endpoint reports the expected role, exact run ID, capture state,
  port, responder/peer readiness, and mission state through MCP.
- Captures decode with the native `nw_pp` application.
- Mismatches are recorded in `docs/net/novaworld-net-re.md`; absence of a
  decoder warning is not itself a parity verdict.
- Only sanitized conclusions and structural fixtures enter Git. Raw local
  evidence remains under `.scratch/`.
