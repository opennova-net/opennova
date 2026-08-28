# ADR 0041: Probes are MCP tools; launch behaviour is a launch flag

- **Status**: accepted (2026-08-27; hard cut)
- **Updated**: [ADR 0042](0042-godot-permanent-shell-one-mission-kernel.md)
  (2026-08-28) updates decision 5's rig reference: once the kernel slices
  land, the mission kernel boots (`tests/common` retains only retail path
  glue).
- **Owners**: the game shell, `godot/probes/`, `scripts/mcp/`, `scripts/net/`,
  the test suites
- **Supersedes/updates**: updates ADR 0037's "ONED has no MCP" (unchanged for
  ONED; the game carries the endpoint) and ADR 0039's verification bullet (the
  dev-tools smoke is the `frame_stats` probe); retires the `godot/tests/*_probe.gd`
  manual-probe contract of `godot/tests/CLAUDE.md` and the `NW_*`/`NOVA_*`/`JO_*`
  environment hooks of `docs/dev-env-vars.md`

## Context

Three sprawls had grown around the test and probe surface. About 215
environment variables with 380 reader sites configured the runtime, the tools
and the tests: seven names for "the resource root", nine for the mission, a
17-variable env console in shipped code, and compile-time `.scratch` defaults
for capture paths — while the asset-gated ctests skipped as PASS, so a green
run proved nothing. `fixtures/` held 223 files (109 MB of LFS), most of them
retail-extracted bytes that our own writers could mint. And 82 manual
`godot/tests/*_probe.gd` scripts each launched their own Godot process
configured by environment variables and were judged on stdout, while the
in-repo game MCP (`godot/game/mcp/`, nine tools, fully tested) had no launcher
since ADR 0037 cut ONED's; the retail side of the parity workflow already ran
through the external onHook MCP, but the OpenNova side was env launches plus
files-on-disk IPC.

## Decision

1. **A probe is a registered `GameProbe` run through the `game_probe` tool.**
   `godot/probes/<family>/<name>_probe.gd` implements `run(ctx: ProbeContext)
   -> ProbeVerdict`; `ProbeCatalog.definitions()` is the one list (name,
   description, script, JSON-schema input, `needs_window`, `needs_mission`,
   timeout). The `ProbeRunner` (a child of the game's MCP service) runs one
   probe at a time, validates arguments against the schema, refuses window
   probes headless and mission probes without a world, keeps a log ring and
   progress object per run, cancels on request and by watchdog, and undoes
   every guarded mutation through `ProbeContext.finish()` on every exit path.
   The logic of the converted probes is kept verbatim; only the configuration
   and the verdict channel changed.
2. **Probes read typed arguments, never the environment.** The probe-source
   contract test bans `print(`, `OS.get_environment(` / `OS.has_environment(`
   and `res://tests/` in probe sources; a probe logs through `ctx.log` and
   returns its verdict as data.
3. **Launch behaviour is a launch flag.** `LaunchFlags`
   (`engine/base/resource_index/boot_policy.*`) parses `--mission`,
   `--lan-host`, `--lan-join`, `--lan-port`, `--lan-gametype`, `--lan-mode`,
   `--lan-max-players`, `--callsign`, `--integrity-profile`, `--capture-pcap`
   and `--mcp-port`; every post-boot environment hook became a debug-catalog
   action (`deploy_pick`, `set_viewmodel_weapon`, `kill_group`, `crew_vehicle`,
   `crew_local_player`, `local_player_look`, `net_joiner_diagnostics`) or was
   deleted. Four documented roots remain for machine paths
   (`OPENNOVA_JO_DIR`, `OPENNOVA_JO_ASSETS`, `OPENNOVA_MISSION_CORPUS`,
   `OPENNOVA_CAPTURES`), read only by the three resolvers
   (`tests/common/retail_paths.h`, `godot/tests/support/retail_data.gd`,
   `scripts/net/lib.ps1`) and the `--resource-dir` default of
   `scripts/mcp/game_mcp.py launch`, plus `GODOT_BIN` for the scripts and the deployed
   service's `ONNET_*` family; `scripts/lint/env_lint.py` keeps it so.
4. **`godot/probes/` is source-only.** Both export presets exclude `probes/*`
   and `tests/*`; a shipped build lists every probe as `available: false`.
5. **An assertion over the portable engine is a ctest, not a probe.** A probe
   whose verdict is engine behaviour (world, sim, collision, anim, WAC, mission
   runtime, net codecs — no rendering, presentation, audio or menus) became a
   C++ test under `tests/<domain>/`, gated on the retail roots when it needs
   retail data and Skipped (exit 77, `opennova_add_gated_test`) without them;
   the retail-mission rig (`tests/common/retail_mission_rig.*`) boots a mission
   through the engine's own boot policy and listen server for them. Only what
   needs the live Godot runtime is a probe.
6. **Retail is driven only through onhook-mcp.** The parity runner keeps
   `onhook-mcp.exe` for `Jointops.exe` and drives OpenNova through its own
   endpoint; the manual retail launchers and their cfg files are gone.
7. **Fixtures are minted, authored or keep.** `fixtures/README.md` and
   `scripts/lint/fixture_lint.py` (ADR 0003 applies: a minted file comes from
   our writer, never from retail bytes).

## Consequences

- One launch vocabulary for people, scripts and probes: the flags. The
  `opennova-game` MCP is reachable from Claude Code (`.mcp.json`, port 8975)
  and from `scripts/mcp/game_mcp.py` / `game_mcp.ps1`.
- `godot/tests/` holds GUT tests only; the probe companions live under
  `godot/tests/probes/`. The retired probes' evidence is recorded in the RE
  records they served; their recipes are `docs/mcp.md`.
- The parity runner's OpenNova joiner readiness, motion exercise and
  cooperative stop are probe runs and an MCP quit, witnessed in the run
  summary (`opennova.parity-motion-gate.v2`,
  `opennova.parity-joiner-shutdown.v2`); the deploy-hold RR/RO cells and the
  OR retail joiner wait on the upstream onhook-mcp role tools (TODO.md).
- The Godot console wrapper stalls when its launching shell has no console;
  automation launches from a real terminal or use the plain runtime executable
  (the runner's ownership proofs need the wrapper).

## Verification

- `godot/tests/mcp/game_probe_test.gd`, `probe_schema_test.gd`,
  `probes/probe_catalog_test.gd`, `probes/parity_joiner_probe_contract_test.gd`,
  `export_presets_test.gd`, `launch_flags_test.gd`; `tests/resource_index/boot_policy_test.cpp`.
- The gated ctests report Skipped without the roots and pass with them
  (`docs/asset-gated-tests.md`); `python scripts/lint/env_lint.py --enforce`,
  `python scripts/lint/fixture_lint.py --enforce`.
- `python scripts/mcp/game_mcp.py launch --windowed --resource-dir
  "$OPENNOVA_JO_DIR"` then `probe run frame_stats '{}' --wait` returns a verdict.
