# OpenNova documentation

This index routes readers to the current architecture, work queues, decisions,
and reverse-engineering records. The records own their evidence and dated
findings; this page does not repeat their progress logs.

Original-engine claims use `[orig: Name @ 0xADDR]` citations, normally against
retail `Jointops.exe`. Use the curated IDB name at that address; qualify another
binary in the marker. RE records own stable `D-<DOMAIN>-n` entries and witness
details. The [divergence ledger](divergence-ledger.md) lists open parity gaps
and points to their owning records. Do not commit raw decompiled code.

## Architecture and current work

- [Current state](current-state.md): phase and routes to each domain's work.
- [Runtime architecture](runtime-architecture.md): the current mission frame
  and engine/Godot boundary under [ADR 0043](adr/0043-canonical-cpp-and-godot-hard-cut.md).
- [Engine primer](engine-primer.md): original binaries, shared conventions,
  subsystem map, and research tools.
- [Correspondence](correspondence.md): function-level retail/reimplementation
  verdicts.
- [Divergence ledger](divergence-ledger.md): live parity backlog and scoreboard.
- [Required resources](required-resources.md): boot-required game data.
- [Environment and launch options](dev-env-vars.md), [asset-gated
  tests](asset-gated-tests.md), and [game MCP](mcp.md): operational references.
- [Render index](render/README.md): render records and comparison procedure.

## Decision records

[ADR 0043](adr/0043-canonical-cpp-and-godot-hard-cut.md) is the current
architecture contract, amended by [ADR 0044](adr/0044-shared-native-assets.md).
[ADR 0045](adr/0045-cli-game-data-runtime-only.md) defines the runtime-only
distribution. Earlier ADRs record the decisions that led here; read each
record's status or supersession note before applying its implementation advice.

- [0001: MNU Actions stay authored; Commands stay shell-bound by control name](adr/0001-mnu-action-command-boundary.md)
- [0002: Round-trip preserves every authored attribute, even ones the runtime ignores](adr/0002-mnu-round-trip-preserves-superset.md)
- [0003: The editor models every construct; no raw import-to-export passthrough](adr/0003-no-raw-passthrough-create-from-scratch.md)
- [0004: Sound-set member selection pushed down to libs/audio](adr/0004-audio-selection-pushdown.md)
- [0005: `%VAR%` expansion: keep raw tokens in the document, expand per field at build](adr/0005-mnu-var-expansion-policy.md)
- [0006: Unified mission runtime present pass](adr/0006-unified-mission-runtime-present-pass.md)
- [0007: Runtime skeletal animation (`.bad`/`.adm`) + the `NovaEntityVisual` contract](adr/0007-skeletal-runtime-and-entity-visual.md)
- [0008: PFF writer policy](adr/0008-pff-writer-policy.md)
- [0009: In-match networking enters the world tick through one seam](adr/0009-in-match-net-seam.md)
- [0010: Completing the NovaWorld client (switchable OpenNova / real NovaWorld)](adr/0010-novaworld-client-completion.md)
- [0011: Single-player is the in-process listen server (network-shaped from day one)](adr/0011-single-player-in-process-listen-server.md)
- [0012: The player is a host-side server entity driven by a wire-shaped intent](adr/0012-player-is-host-side-server-entity.md)
- [0013: Consolidated in-match net core: one message registry, one server-state model](adr/0013-consolidated-net-core.md)
- [0014: MNS stylesheets parse into a lossless document; runtime uses its evaluated view](adr/0014-mns-lossless-document-model.md)
- [0015: Two Godot products; the server is a mode of the game, not a product](adr/0015-two-products-serve-mode.md)
- [0016: The editor is a detachable layer over public engine APIs; engine behavior does not live in GDScript](adr/0016-engine-editor-boundary.md)
- [0017: Contracts are typed records, not dictionaries; constants are named, not magic](adr/0017-typed-records-named-constants.md)
- [0018: Tests exercise public seams; a test that needs a private is an API bug report](adr/0018-public-api-testability.md)
- [0019: libs/npwire — the game wire protocol library](adr/0019-npwire-game-wire-lib.md)
- [0020: the world→terrain seam — libs/terrain_query](adr/0020-world-terrain-query-seam.md)
- [0021: Avatars.def writer policy](adr/0021-avatars-writer-policy.md)
- [0022: Divergence burn-down and the permanent register](adr/0022-divergence-burn-down.md)
- [0023: Render visual parity (the REN track)](adr/0023-render-visual-parity.md)
- [0024: lib family topology — one lib per format, families as link groups](adr/0024-lib-family-topology.md)
- [0025: the standalone game is ONED's only live mission runtime](adr/0025-standalone-game-is-the-only-live-mission-runtime.md)
- [0026: one client replica pipeline and one wire presenter](adr/0026-one-client-replica-pipeline.md)
- [0027: DI3 is the only model format, consumed directly — no model IR](adr/0027-3di3-only-no-model-ir.md)
- [0028: engine/ is the engine; godot/src/ is the shell adapter](adr/0028-engine-directory-and-shell-adapter.md)
- [0029: engine target topology — five group archives, per-lib targets retired](adr/0029-engine-group-targets.md)
- [0030: the formats placement criterion — what earns an engine/formats/ lib](adr/0030-formats-placement-criterion.md)
- [0031: the adapter composition contract — what earns C++ in godot/src/](adr/0031-adapter-composition-contract.md)
- [0032: direct document I/O — godot/ adds nothing but Godot](adr/0032-direct-document-io.md)
- [0033: the engine owns the loops; shells are devices](adr/0033-engine-owned-loops-device-shells.md)
- [0034: Godot is the first-class shell](adr/0034-first-class-godot.md)
- [0035: MissionSession with a first-class Godot frame pipeline](adr/0035-mission-session-game-frame-pipeline.md)
- [0036: one in-match session, wire-first compatibility](adr/0036-one-inmatch-session-wire-first.md)
- [0037: ONED runs game data; it does not edit it](adr/0037-oned-runs-game-data.md)
- [0038: Native runtime assets now; GLB at the future editor seam](adr/0038-native-runtime-assets-glb-editor.md)
- [0039: The tool UI lives in the engine, drawn with Dear ImGui](adr/0039-in-engine-dev-tools.md)
- [0040: the engine is one namespace — no Nova prefix, files follow classes, group-qualified includes](adr/0040-the-engine-is-one-namespace.md)
- [0041: Probes are MCP tools; launch behaviour is a launch flag](adr/0041-probes-are-mcp-tools-launch-flags.md)
- [0042: Godot is the permanent shell; one mission kernel; engine facts through engine functions](adr/0042-godot-permanent-shell-one-mission-kernel.md)
- [0043: canonical C++ and canonical Godot — one session with roles, the world is C++ Nodes, tests drive real fixtures](adr/0043-canonical-cpp-and-godot-hard-cut.md)
- [0044: shared native assets](adr/0044-shared-native-assets.md)
- [0045: CLI game data and a runtime-only distribution](adr/0045-cli-game-data-runtime-only.md)

## Reverse-engineering records by domain

- **Animation and models:** [ADM/BAD](anim/adm-bad-format-re.md);
  [3DI GP](threedi/3di-gp-format-re.md), [3DI LW](threedi/3di-lw-format-re.md),
  and the [scene naming contract](threedi/scene-naming-contract.md).
- **Audio:** [LWF/DBF sound](audio/lwf-dbf-sound-re.md) and
  [MUS/SBF music](audio/mus-sbf-re.md).
- **World and mission:** [world, WAC, and AI](world/world-wac-ai-re.md),
  [vehicle movers](world/vehicle-client-movers-re.md),
  [item definitions](world/itemdef-re.md),
  [NPC script coverage](world/npc-script-coverage.md),
  [NPC mission completion](world/npc-mission-completion.md),
  [special weapons](world/special-weapons-parity.md),
  [BMS events](mission/bms-event-runtime-re.md),
  [MIS format](mission/mis-format-re.md), and
  [savegames](mission/savegame-re.md).
- **Network:** [NovaWorld and in-match protocol](net/novaworld-net-re.md),
  [retail dispatch audit](net/retail-message-dispatch-audit.md), and
  [vehicle deployment regression](net/retail-vehicle-deployment-regression.md).
- **Rendering and environment:** [render records](render/README.md),
  [terrain](terrain/terrain-re.md), [tiles](tiles/til-re.md),
  [foliage](foliage/foliage-re.md), [environment and time of day](env/env-tod-re.md),
  [honored environment fields](env/env-honored-matrix.md), and
  [particles](particles/ptl-format-re.md).
- **Interface and data:** [HUD](interface/hud-re.md),
  [weapon/vehicle HUD validation](interface/weapon-vehicle-hud-validation.md),
  [loading screen](interface/loading-screen-re.md),
  [RTXT strings](interface/rtxt-strings-re.md),
  [MNU menus](mnu/menu-re.md), [menu wiring](mnu/menu-wiring.md),
  [fonts](fonts/fnt-re.md), [avatars](playerinfo/avatars-re.md),
  [credits](credits/cbin-re.md), and [VFS/PFF](vfs/vfs-pff-mount-re.md).

## Dated and retired-program records

The [2026-09-13 JO-C audit](jo-c-parity-audit-2026-09-13.md) and
[2026-09-18 validation](jo-c-validation-2026-09-18.md) are dated evidence,
not live status. The [maturity program](maturity-program.md) and
[ONED editor](oned/editor-layer-program.md),
[workspace](oned/workspace-maturity-program.md), and
[runtime-parity](oned/editor-runtime-parity.md) programs are historical.
[Performance records](perf/03tr-frame-costs.md) include the
[mission-load](perf/mission-load-baseline.md) and
[re-ground](perf/reground-baseline.md) baselines. For current work, return
to the ledger and current-state router.
