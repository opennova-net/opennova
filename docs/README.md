# OpenNova documentation

Tracked golden docs: architecture maps, decision records (ADRs), and
reverse-engineering records — kept pristine, representing the best current
understanding of the original engine. RE findings land here directly, and
git history is the only archive: everything an agent or contributor needs
for the full picture is tracked in this repo.

Conventions: original-engine functions are cited inline as
`[orig: Name @ 0xADDR]` (addresses are `Jointops.exe` retail unless a doc says
otherwise). RE records carry correspondence tables, divergence lists
(`D-XXX-n`), and per-claim verdicts. No raw decompiled code is ever committed;
behavior is summarized and cited.

## Architecture

| Doc | What it covers |
|---|---|
| [`engine-primer.md`](engine-primer.md) | Start here: the original engine in one read — binaries/IDBs, engine-wide conventions (fixed-point, coordinates, the 62 Hz tick), subsystem index, and the research toolbox |
| [`runtime-architecture.md`](runtime-architecture.md) | How a mission runs: the consolidated logic tick, present pass, and audio pass, mapped onto the original main loop |
| [`correspondence.md`](correspondence.md) | The cross-system parity matrix: which original function each reimplementation corresponds to, with verdicts |
| [`perf/mission-load-baseline.md`](perf/mission-load-baseline.md) | Recorded mission-load timings (PerfTimeline) and the verdict that gates the perf push-down slices |
| [`oned/editor-runtime-parity.md`](oned/editor-runtime-parity.md) | How ONED and the game runtime share rendering and simulation (the `edit_mode` runtime-node pattern, sampler seams, the one mission runtime) |
| [`oned/editor-layer-program.md`](oned/editor-layer-program.md) | The editor-layer refactor program: landed architecture phases (EditorApp root, shell decomposition) and the remaining undo/shortcuts/widgets/theme phases with per-slice gates |

## Decision records

| ADR | Decision |
|---|---|
| [0001](adr/0001-mnu-action-command-boundary.md) | MNU: Actions live in the file, Commands come from the host by control name |
| [0002](adr/0002-mnu-round-trip-preserves-superset.md) | MNU: round-trip preserves the format superset |
| [0003](adr/0003-no-raw-passthrough-create-from-scratch.md) | MNU: no raw byte passthrough; documents are created from scratch |
| [0004](adr/0004-audio-selection-pushdown.md) | Audio: member selection pushed down into portable `libs/audio` |
| [0005](adr/0005-mnu-var-expansion-policy.md) | MNU: `%VAR%` expansion policy |
| [0006](adr/0006-unified-mission-runtime-present-pass.md) | One mission runtime, one present pass, one entity index for game and editor |
| [0007](adr/0007-skeletal-runtime-and-entity-visual.md) | Skeletal `.bad`/`.adm` runtime and the `NovaEntityVisual` contract |
| [0008](adr/0008-pff-writer-policy.md) | PFF writer: zero timestamp/checksum for new entries, verbatim for retained |
| [0009](adr/0009-in-match-net-seam.md) | In-match networking enters the world tick through one seam (NetSystem + INetCommandSink) |
| [0010](adr/0010-novaworld-client-completion.md) | NovaWorld client completion: gate → hello → auth → verify → join, then the JointOperations proto-switch |
| [0011](adr/0011-single-player-in-process-listen-server.md) | Single-player is the in-process listen server (network-shaped); supersedes ADR 0009's "SP pays nothing" |
| [0012](adr/0012-player-is-host-side-server-entity.md) | The player is a host-side server entity driven by a wire-shaped (C2S 0x0C) intent |
| [0013](adr/0013-consolidated-net-core.md) | Consolidated in-match net core: one message registry + capture→golden-diff harness, EntityRegistry as the one server-state authority, one host bring-up helper |

## RE records by domain

| Domain | Doc | Status |
|---|---|---|
| Audio | [`audio/lwf-dbf-sound-re.md`](audio/lwf-dbf-sound-re.md) | landed |
| Audio | [`audio/mus-sbf-re.md`](audio/mus-sbf-re.md) | landed |
| Environment | [`env/env-tod-re.md`](env/env-tod-re.md) | landed |
| Interface | [`interface/rtxt-strings-re.md`](interface/rtxt-strings-re.md) | landed |
| Interface | [`interface/hud-re.md`](interface/hud-re.md) | landed (engine-research; HUD port in flight) |
| Menus | [`mnu/menu-re.md`](mnu/menu-re.md) | landed |
| Menus | [`mnu/menu-wiring.md`](mnu/menu-wiring.md) | landed (host wiring, architectural) |
| Mission | [`mission/bms-event-runtime-re.md`](mission/bms-event-runtime-re.md) | landed |
| Mission | [`mission/mis-format-re.md`](mission/mis-format-re.md) | partial: writer-generated subset, full `dfx2med.exe` grill pending |
| Net | [`net/novaworld-net-re.md`](net/novaworld-net-re.md) | authoritative NovaWorld wire record; current through §5.57/§5.58 + D-NET-144 (0x0A entity loop ported, 0x0F flood closed, weapon.def/loadout + reload round-trip witnessed; open: D-NET-141..144 — the retail-join v15 ammo/reload/anim defects, 2026-07-02) |
| Particles | [`particles/ptl-format-re.md`](particles/ptl-format-re.md) | redesign in flight in a worktree |
| 3DI | [`threedi/3di-gp-format-re.md`](threedi/3di-gp-format-re.md) | landed (`libs/threedi`) |
| 3DI | [`threedi/3di-lw-format-re.md`](threedi/3di-lw-format-re.md) | unlanded: Land Warrior import, PR #45 closed |
| World | [`world/itemdef-re.md`](world/itemdef-re.md) | landed |
| World | [`world/world-wac-ai-re.md`](world/world-wac-ai-re.md) | landed (+ §13 held-weapon mount-hide, engine-research) |

Systems documented mainly by code and tests so far (no dedicated RE record
yet): terrain, foliage, tiles, fonts, credits, the importer pipeline, and the
VFS/PFF mount stack (the PFF write side is ADR 0008). The project glossary
lives at the repo root in [`CONTEXT.md`](../CONTEXT.md); the project vision is
[`GOALS.md`](../GOALS.md).
