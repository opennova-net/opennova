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
| [`perf/reground-baseline.md`](perf/reground-baseline.md) | Recorded bulk re-ground timings (the activate-time "terrain changed under N objects" flow) behind the re-ground perf slices |
| [`env/env-honored-matrix.md`](env/env-honored-matrix.md) | Which `.env` fields each renderer consumer actually honors: live control vs parsed-but-deferred, per field |
| [`oned/editor-runtime-parity.md`](oned/editor-runtime-parity.md) | How ONED and the game runtime share rendering and simulation (the `edit_mode` runtime-node pattern, sampler seams, the one mission runtime) |
| [`oned/editor-layer-program.md`](oned/editor-layer-program.md) | The editor-layer refactor program (complete 2026-07-04): EditorApp root, shell decomposition, undo everywhere, global shortcuts, shared widgets/theme/vocabulary, with per-slice gates |
| [`oned/workspace-maturity-program.md`](oned/workspace-maturity-program.md) | The maturity program's ONED track: the R/W/E/G capability bar, the twelve-workspace matrix (Avatars joined at AVA), foundation services, the music redesign, responsiveness, test-seam refits, per-workspace phases, and the RE ledger gating them |
| [`asset-gated-tests.md`](asset-gated-tests.md) | Every env-var-gated test: the var→data matrix, the skip-passes-as-green caveat, local setup, why captures are never committed, and the CI stance |
| [`maturity-program.md`](maturity-program.md) | The maturity program (the pre-reimplementation rearchitecture push): seven tracks, five waves, the boundary-conformance checklist, enforcement ratchets, and gates — the program dashboard |
| [`divergence-ledger.md`](divergence-ledger.md) | The divergence burn-down: every tracked divergence in one place under one vocabulary, the per-domain OPEN tables, the count-to-zero scoreboard, the permanent register, and the UNAUDITED systems — the parity dashboard (ADR 0022) |
| [`required-resources.md`](required-resources.md) | The witnessed boot-required, hardcoded-by-name resource set (R8/ENG-6): the fatal set, per-resource failure behavior, the ordered boot sequence, and the D-BOOT catalog — the source for the ENG-6 manifest and ONED's new-game scaffold |

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
| [0014](adr/0014-mns-lossless-document-model.md) | MNS: stylesheets parse into a lossless document; the flat table is its flatten() view |
| [0015](adr/0015-two-products-serve-mode.md) | Two shipped products; the server is a serve MODE of the game exe, not a product |
| [0016](adr/0016-engine-editor-boundary.md) | The editor is a detachable layer over public engine APIs; engine behavior does not live in GDScript |
| [0017](adr/0017-typed-records-named-constants.md) | Contracts are typed records, not dictionaries; constants are named, not magic |
| [0018](adr/0018-public-api-testability.md) | Tests exercise public seams; a test that needs a private is an API bug report |
| [0019](adr/0019-npwire-game-wire-lib.md) | libs/npwire is the game wire protocol lib; matchmaking (novaworld) sits on it, direction npwire → napi/novacrypto |
| [0020](adr/0020-world-terrain-query-seam.md) | libs/terrain_query is the world→terrain seam: world links the height-query leaf, never the terrain-format stack; the forbidden-edge check is permanent |
| [0021](adr/0021-avatars-writer-policy.md) | Avatars.def writer: from-scratch canonical output; lossless + idempotent round-trip, not byte-exact vs the hand-authored file |
| [0022](adr/0022-divergence-burn-down.md) | Divergence burn-down: zero-OPEN target, the canonical disposition vocabulary, the PAR freeze exemption, and the permanent register of ratified deliberate divergences |
| [0023](adr/0023-render-visual-parity.md) | Render visual parity (REN): the fixed-function look is the target (no PBR), the D3D device layer is witness-source only, REN runs freeze-exempt on the PAR model, and the three-tier parity instrument's tolerances never widen |

## RE records by domain

| Domain | Doc | Status |
|---|---|---|
| Audio | [`audio/lwf-dbf-sound-re.md`](audio/lwf-dbf-sound-re.md) | landed |
| Credits (CBIN) | [`credits/cbin-re.md`](credits/cbin-re.md) | partial (PAR-R5: codec magic/header/ROL32-XOR cipher MATCHING vs retail `@0x75e348`; markup + read-path NEEDS-RE) |
| Audio | [`audio/mus-sbf-re.md`](audio/mus-sbf-re.md) | landed |
| Environment | [`env/env-tod-re.md`](env/env-tod-re.md) | landed |
| Fonts | [`fonts/fnt-re.md`](fonts/fnt-re.md) | landed (PAR-R4 audit: the `.fnt` format + load contract, D-FNT-1..3) |
| Foliage | [`foliage/foliage-re.md`](foliage/foliage-re.md) | landed (PAR-R2 audit: placement MATCHING vs retail `@0x600197`, D-FOLIAGE-1 color; + 2026-07-08 the MODEL tier fully witnessed — sector-entity clusters, 0.75-XZ/0.5-height scale, the biquadratic ground fit, the `:fd` bake — D-FOLIAGE-4/-5 OPEN, port in flight) |
| Tiles | [`tiles/til-re.md`](tiles/til-re.md) | landed (PAR-R3 audit: overlay/atlas/flip-rotate MATCHING vs retail `@0x60df0d`/`@0x604700`, D-TIL-1 outline) |
| Importer | [`importer/importer-audit.md`](importer/importer-audit.md) | landed (PAR-R6: tracked-by-composition — no independent parity surface, composes RE'd `libs` via `pyopennova`) |
| Interface | [`interface/rtxt-strings-re.md`](interface/rtxt-strings-re.md) | landed |
| Terrain | [`terrain/terrain-re.md`](terrain/terrain-re.md) | partial (PAR-R1: module surface + witness basis + D-TERRAIN-1 shader split; REN-4 runtime surface shading; ENG-3 B0 runtime terrain queries — height samplers + segment raycast, port pending B1; mesh_simp/CDEP deep grill pending) |
| Interface | [`interface/hud-re.md`](interface/hud-re.md) | landed (engine-research; HUD port in flight) |
| Menus | [`mnu/menu-re.md`](mnu/menu-re.md) | landed (+ 2026-06-23c combo-dropdown geometry/row-height grill, D-MNU-7/8 fixed) |
| Menus | [`mnu/menu-wiring.md`](mnu/menu-wiring.md) | landed (host wiring, architectural) |
| Mission | [`mission/bms-event-runtime-re.md`](mission/bms-event-runtime-re.md) | landed |
| Mission | [`mission/mis-format-re.md`](mission/mis-format-re.md) | partial: writer-generated subset, full `dfx2med.exe` grill pending |
| Net | [`net/novaworld-net-re.md`](net/novaworld-net-re.md) | authoritative NovaWorld wire record — layering, matchmaking, and the in-match protocol — current through §5.61 and D-NET-162; per-entry fix/live-verify state and the ranked open items live in the doc's own D-NET divergence catalog |
| Particles | [`particles/ptl-format-re.md`](particles/ptl-format-re.md) | redesign in flight in a worktree |
| Player info | [`playerinfo/avatars-re.md`](playerinfo/avatars-re.md) | landed (`libs/avatars` + ONED editor bridge + full `PLAYER_INFO` screen orchestration grilled, D-PLAYERINFO-1..12; runtime `player.mnu` host wiring + spawned-player binding open) |
| Render | [`render/render-material-re.md`](render/render-material-re.md) | landed (REN-2: the runtime material path — HLSLEffect registry, tag resolution, flag-byte state, blend/depth policy — D-RMAT-1..6) |
| Render | [`render/render-order-re.md`](render/render-order-re.md) | landed (REN-3: the batch queues, sort keys, technique-class selection, render-state stack, and the frame pass sequence — D-RORD-1..6; the ordering ladder ported to `libs/renderer/render_order`) |
| Render | [`render/render-lighting-re.md`](render/render-lighting-re.md) | landed (REN-5: the iris/modulator chain, the world lighting block + entity uniforms + hemisphere lights, dynamic point lights, terrain/foliage c0/c1, lighting textures + the cubemap sources — D-RLIT-1..6; ported to `libs/renderer/light_runtime` + `libs/env::ModulatorChain`) |
| 3DI | [`threedi/3di-gp-format-re.md`](threedi/3di-gp-format-re.md) | landed (`libs/threedi`) |
| 3DI | [`threedi/3di-lw-format-re.md`](threedi/3di-lw-format-re.md) | unlanded: Land Warrior import, PR #45 closed |
| VFS/PFF | [`vfs/vfs-pff-mount-re.md`](vfs/vfs-pff-mount-re.md) | landed (PAR-R7 audit: the mount stack, resolution order, /d gate, D-VFS-1..9) |
| World | [`world/itemdef-re.md`](world/itemdef-re.md) | landed |
| World | [`world/world-wac-ai-re.md`](world/world-wac-ai-re.md) | landed (+ §13 held-weapon mount-hide, engine-research) |

The UNAUDITED set was emptied by the PAR-R1..R7 sweep (2026-07-05), then
reopened the same day with the three runtime-render systems the REN track
audits — materials/state, draw order, lighting (the records land under
`docs/render/` at REN-2/3/5; see the
[divergence ledger](divergence-ledger.md)'s audit track and
[ADR 0023](adr/0023-render-visual-parity.md)). Every other subsystem has a
dedicated RE record (full or partial) or a tracked-by-composition audit. The
project glossary lives at the repo root in [`CONTEXT.md`](../CONTEXT.md); the
project vision is [`GOALS.md`](../GOALS.md).
