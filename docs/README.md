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
| [`current-state.md`](current-state.md) | Where the project is and where the next step is written down: the phase (maturity program closed, retail-fidelity slices current), the standing slice loop, which record names each domain's next step, and the research queue |
| [`engine-primer.md`](engine-primer.md) | Start here for engine work: the original engine in one read — binaries/IDBs, engine-wide conventions (fixed-point, coordinates, the 62 Hz tick), subsystem index, and the research toolbox |
| [`runtime-architecture.md`](runtime-architecture.md) | How a mission runs: the consolidated logic tick, present pass, and audio pass, mapped onto the original main loop |
| [`correspondence.md`](correspondence.md) | The cross-system parity matrix: which original function each reimplementation corresponds to, with verdicts |
| [`perf/mission-load-baseline.md`](perf/mission-load-baseline.md) | Recorded mission-load timings (PerfTimeline) and the verdict that gates the perf push-down slices |
| [`perf/reground-baseline.md`](perf/reground-baseline.md) | Recorded bulk re-ground timings (the activate-time "terrain changed under N objects" flow) behind the re-ground perf slices |
| [`env/env-honored-matrix.md`](env/env-honored-matrix.md) | Which `.env` fields each renderer consumer actually honors: live control vs parsed-but-deferred, per field |
| [`oned/editor-runtime-parity.md`](oned/editor-runtime-parity.md) | How ONED reuses runtime rendering/data seams for authoring previews while the standalone game remains the only live mission host |
| [`oned/editor-layer-program.md`](oned/editor-layer-program.md) | The editor-layer refactor program (complete 2026-07-04): EditorApp root, shell decomposition, undo everywhere, global shortcuts, shared widgets/theme/vocabulary, with per-slice gates |
| [`oned/workspace-maturity-program.md`](oned/workspace-maturity-program.md) | The maturity program's ONED track: the R/W/E/G capability bar, the twelve-workspace matrix (Avatars joined at AVA), foundation services, the music redesign, responsiveness, test-seam refits, per-workspace phases, and the RE ledger gating them |
| [`asset-gated-tests.md`](asset-gated-tests.md) | Every env-var-gated test: the var→data matrix, the skip-passes-as-green caveat, local setup, why captures are never committed, and the CI stance |
| [`maturity-program.md`](maturity-program.md) | The maturity program (the pre-reimplementation rearchitecture push): seven tracks, five waves, the boundary-conformance checklist, enforcement ratchets, and gates — the program dashboard. **Closed 2026-07-12** (freeze lifted); the enforcement instruments and standing ADRs survive it |
| [`divergence-ledger.md`](divergence-ledger.md) | The divergence burn-down: every tracked divergence in one place under one vocabulary, the per-domain OPEN tables, the count-to-zero scoreboard, the permanent register, and the UNAUDITED systems — the parity dashboard (ADR 0022) |
| [`render/README.md`](render/README.md) | The render domain's own index: which REN record covers what, and the three-tier parity instrument (state vectors, offline compare, retail scene attestation) |
| [`required-resources.md`](required-resources.md) | The witnessed boot-required, hardcoded-by-name resource set (R8/ENG-6): the fatal set, per-resource failure behavior, the ordered boot sequence, and the D-BOOT catalog — the source for the ENG-6 manifest and ONED's new-game scaffold |

## Decision records

| ADR | Decision |
|---|---|
| [0001](adr/0001-mnu-action-command-boundary.md) | MNU: Actions live in the file, Commands come from the shell by control name |
| [0002](adr/0002-mnu-round-trip-preserves-superset.md) | MNU: round-trip preserves the format superset |
| [0003](adr/0003-no-raw-passthrough-create-from-scratch.md) | MNU: no raw byte passthrough; documents are created from scratch |
| [0004](adr/0004-audio-selection-pushdown.md) | Audio: member selection pushed down into portable `libs/audio` |
| [0005](adr/0005-mnu-var-expansion-policy.md) | MNU: `%VAR%` expansion policy |
| [0006](adr/0006-unified-mission-runtime-present-pass.md) | Unified mission present pass and entity index; its former embedded editor preview is superseded by ADR 0025 |
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
| [0024](adr/0024-lib-family-topology.md) | Lib family topology: one-lib-per-format affirmed; terrain/audio families are CMake INTERFACE link groups (never merges, never edge laundering); the renderer-fold reversal recorded; the two consumption models named (Model A flat C ABI / Model B C++ static link) |
| [0025](adr/0025-standalone-game-is-the-only-live-mission-runtime.md) | ONED has no PIE or in-place mission simulation: F5/F6 run saved loose assets in one managed standalone game child; F8 stops it |

## RE records by domain

| Domain | Doc | Status |
|---|---|---|
| Audio | [`audio/lwf-dbf-sound-re.md`](audio/lwf-dbf-sound-re.md) | landed (+ §sound-profile: the SndProf.def system + infantry slot-sound consumers, witnessed + ported 2026-07-17, D-SND-10..15; + §driver cadence: the ambient tick/frame clock split witnessed 2026-07-28, D-SND-16 minted + PORTED same day into libs/audio AmbientMixer) |
| Credits (CBIN) | [`credits/cbin-re.md`](credits/cbin-re.md) | partial (PAR-R5: codec magic/header/ROL32-XOR cipher MATCHING vs retail `@0x75e348`; markup + read-path NEEDS-RE) |
| Audio | [`audio/mus-sbf-re.md`](audio/mus-sbf-re.md) | landed |
| Environment | [`env/env-tod-re.md`](env/env-tod-re.md) | landed |
| Fonts | [`fonts/fnt-re.md`](fonts/fnt-re.md) | landed (PAR-R4 audit: the `.fnt` format + load contract, D-FNT-1..4; cp1252 glyph mapping fixed 2026-07-19) |
| Foliage | [`foliage/foliage-re.md`](foliage/foliage-re.md) | landed (fresh 2026-07-13 re-grill and replacement: ported detail/MODEL tier semantics, separate flat-detail and sector-routed MODEL authored-map gates, all-surface LOD0 geometry, `:fd`, shaders, static `.til` RGB/tint at the pre-wind coordinate, persistent LRU/1000-entry cache cadence, identity/eviction order, and matching shadow-off for retail's dead flag; D-FOLIAGE-7/-9/-10 bound the general page/cache + ordered RT producer, visibility membership, and reimpl draw order/reflection) |
| Tiles | [`tiles/til-re.md`](tiles/til-re.md) | landed (PAR-R3 audit: overlay/atlas/flip-rotate MATCHING vs retail `@0x60df0d`/`@0x604700`, D-TIL-1 outline) |
| Importer | [`importer/importer-audit.md`](importer/importer-audit.md) | landed (PAR-R6: tracked-by-composition — no independent parity surface, composes RE'd `libs` via `pyopennova`) |
| Interface | [`interface/rtxt-strings-re.md`](interface/rtxt-strings-re.md) | landed |
| Terrain | [`terrain/terrain-re.md`](terrain/terrain-re.md) | partial (fresh 2026-07-13 rendering re-grill: exact eight-family LOD selector, authored coefficient/DBlend/paired mips, four-lock heightfield-normal atlas, bare cached-tile RGB/DOT3 alpha, direct light packing, top ps.1.4 arithmetic, overlay order, and fog; D-TERRAIN-7/-8/-9 now bound only remaining dynamic composition, local lights/shadows, and editor-preview inputs) |
| Interface | [`interface/loading-screen-re.md`](interface/loading-screen-re.md) | landed (2026-07-12: the sidecar `<missionbase>.pcx` rule, MP session text, progress bar + creep smoothing witnessed AND ported; D-LOADSCR-1..6; SP splash + joiner hold tracked) |
| Interface | [`interface/hud-re.md`](interface/hud-re.md) | landed (2026-07-09: weapon-coupled elements witnessed AND ported — ammo/name text, clip indicator, crosshair spread, stance cross-fade, triggered text; 2026-07-11 re-grill: every ported function fresh-decompiled, seven port fixes, D-HUD-1..10; 2026-07-17: attach labels witnessed AND ported — seat/armory floats + the attachtextid chain, D-HUD-11..14, bottom prompts deferred; 2026-07-18: waypoint HUD chain + weapon heat bar + the MISSION OBJECTIVES panel witnessed AND ported — the "radar @0x599700" misnomer resolved as the heat bar, JO:CA has no in-HUD radar, D-HUD-15..18; 2026-07-31: exact recoil/movement accumulators wired into projectile, camera, aim overlay, and HUD spread — D-HUD-7 CLOSED; map-overlay/MP-HUD follow-ups tracked) |
| Menus | [`mnu/menu-re.md`](mnu/menu-re.md) | landed (+ 2026-06-23c combo-dropdown grill, D-MNU-7/8 fixed; + 2026-07-11 in-game armory re-grill, D-MNU-9; + 2026-07-16 dropdown-input grill, D-MNU-11 fixed / D-MNU-12 reimpl mapping; + 2026-07-18 loadout grill — multi-slot ACCEPT + availability filter live, net-re §5.63) |
| Menus | [`mnu/menu-wiring.md`](mnu/menu-wiring.md) | landed (shell wiring, architectural) |
| Mission | [`mission/bms-event-runtime-re.md`](mission/bms-event-runtime-re.md) | landed |
| Mission | [`mission/mis-format-re.md`](mission/mis-format-re.md) | partial: writer-generated subset, full `dfx2med.exe` grill pending |
| Net | [`net/novaworld-net-re.md`](net/novaworld-net-re.md) | authoritative NovaWorld wire record — layering, matchmaking, and the in-match protocol; §5 carries the tag-level findings (including the 2026-07-31 exact recoil/spread re-grill in §5.60), §8 the D-NET divergence catalog with per-entry fix/live-verify state and the ranked open items |
| Particles | [`particles/ptl-format-re.md`](particles/ptl-format-re.md) | landed (#237 merged 2026-07-16) — the `.ptl` stack + effect world; re-grilled 2026-07-12/13/15/16 (weapon/vehicle effect chains, the rewrite grill, curve-table stricmp, name-resolution case fold) |
| Player info | [`playerinfo/avatars-re.md`](playerinfo/avatars-re.md) | landed (`libs/avatars` + ONED editor bridge + the runtime `player.mnu` host `PlayerInfoMenuCompanion` ported — cascade/team/voice/preview/weapon lists live, D-PLAYERINFO-7/-10 FIXED; open: persistence D-PLAYERINFO-9, ammo + weight D-PLAYERINFO-11, selection globals D-PLAYERINFO-12, spawned-player binding D-PLAYERINFO-1) |
| Render | [`render/render-material-re.md`](render/render-material-re.md) | landed (REN-2: the runtime material path — HLSLEffect registry, tag resolution, flag-byte state, blend/depth policy — D-RMAT-1..6) |
| Render | [`render/render-order-re.md`](render/render-order-re.md) | landed (REN-3: the batch queues, sort keys, technique-class selection, render-state stack, and the frame pass sequence — D-RORD-1..6; the ordering ladder ported to `libs/renderer/render_order`) |
| Render | [`render/render-lighting-re.md`](render/render-lighting-re.md) | landed (REN-5: the iris/modulator chain, the world lighting block + entity uniforms + hemisphere lights, dynamic point lights, terrain/foliage c0/c1, lighting textures + the cubemap sources — D-RLIT-1..6; ported to `libs/renderer/light_runtime` + `libs/env::ModulatorChain`; 2026-07-18: the marched iris sampling PORTED — D-RLIT-2) |
| Render | [`render/render-occlusion-re.md`](render/render-occlusion-re.md) | landed 2026-07-16 (blink-box visibility: section masks, portal traversal, occluder culling, indoor frame gates, GPM `OVRT`/`OPLN`/`OFAC`/`OOBJ` occlusion chunks, sound-occlusion witness — D-OCC-1..8 open witness details; sound occlusion (closes D-SND-7) + the indoor frame gates ported on the occlusion slice; the section-mask/portal engine is the follow-up slice) |
| 3DI | [`threedi/3di-gp-format-re.md`](threedi/3di-gp-format-re.md) | landed (`libs/threedi`) |
| 3DI | [`threedi/3di-lw-format-re.md`](threedi/3di-lw-format-re.md) | unlanded: Land Warrior import, PR #45 closed |
| VFS/PFF | [`vfs/vfs-pff-mount-re.md`](vfs/vfs-pff-mount-re.md) | landed (PAR-R7 audit: the mount stack, resolution order, /d gate, D-VFS-1..9) |
| World | [`world/itemdef-re.md`](world/itemdef-re.md) | landed |
| World | [`world/world-wac-ai-re.md`](world/world-wac-ai-re.md) | landed (+ §13 held-weapon mount-hide; §14 third-person body aim overlay / torso bend + camera modes, local player ported 2026-07-08; §14.8 upper-body weapon channel producer, local player FULLY ported 2026-07-09 incl. the special_hold/attack_anim kind ladder + arms-dip; §14.3 pitch kick resolved = the audio output power meter — remainder tracked D-INF-11; §15 world-object collision + blink boxes, engine-research 2026-07-09 + full re-grill 2026-07-11 — ported libs/world/collision, D-COL-1..9; §16 ground-AI combat chain, engine-research 2026-07-16 — SM rows 16/17/18 + targeting feed + fire convergence witnessed, port tracked D-AI-1..3; §17 infantry combat pass + state-17 tick digest, engine-research 2026-07-16 session 2 — perception/attack-anims/anim-event fire/aim model + the 0x472e00 full body, port tracked D-AI-4/5; §18 fire presentation + the LOS raycast internals, engine-research 2026-07-16 session 4 — ai_launch/ai_launcheffect/MF_Light/tracer_type legs + Physics_RaycastTerrainAndSectors, ported (fire_present_pass + CollisionWorld::raycast_clear), tracked D-AI-7/8; §19 death presentation, engine-research 2026-07-16 session 5 — ported, tracked D-AI-9; §20 round-outcome loop, engine-research 2026-07-16 session 6 — WAC win/lose + named-value table + Server_ProcessRoundEnd + SP end presentation, ported + 04TR probe PASS, tracked D-AI-10; §21 fire-origin/userpoint chain, engine-research 2026-07-16 session 7 — the posed gun-flash userpoint muzzle seam LANDED (D-AI-6 fire-origin clause closed), CP01 muzzle probe PASS; §22 org2 player-body physics grill, grill-ida 2026-07-16 session 8 — the player heading/leg model (legs chase the yaw, body = leg midpoint), per-tick −208 gravity, jump cooldown/momentum, the airborne/landing edges + the org1 leg corrections, ported — D-INF-10/-12 CLOSED, D-INF-20/21 opened; §17.4b the trigger-word sound legs (footsteps by surface + capsule-bottom dip, SSAudio foley, landing pair, death-scream night gate = EnableNVG, the org2 chute/freefall family), witnessed + ported 2026-07-17 with the audio record's D-SND-10..15; §23 the vehicle pass, engine-research 2026-07-16 session 9 — USE-ITEM mount chain + BMS mount triggers 38-41 + vehicle physics parked/AI-driver legs + AI boarding chain + player deploy group stamp, ported (vehicle_attach toggle/scan, vehicle_ai_drive, group 1), tracked D-AI-11); §24 item destruction — native collision/damage, destructible death, husk swap, death-piece motion/ring, and the main item-settle callback family ported and validated; the remaining ground-transition gaps are tracked in D-ITEM-14; retail residuals remain bounded, including piece meshes, effect banks/steam, triangle debris, glass, the specialized unitType-3 mover, bridge water shocks, and loaded-husk gating tracked in D-ITEM-1..20; §15.8a the hit-chain re-grill 2026-07-18 — format layer cleared, exclusion set + strict terrain tie-break ported, round ballistics/residuals ledgered within D-ITEM-1..13; §25 the tracer visual system, grill-ida 2026-07-18 — the trail emitter pool @ 0x2BF5270 + the 12 tracer_type style blocks + the camera-facing ribbon renderer witnessed and ported (world/tracer_trails + RoundSim + fire_present_pass ribbons), D-AI-8c closed into D-AI-12; §27 throwables, engine-research 2026-07-20 — the PowerThrow charge chain, the items.def class-tag motors (nade/schl/clym/vmne/lndm) + grenade bounce/fuse, satchel/claymore stick + placed-device conversion, the think/detonate chain incl. the detonator and the claymore cone/fan, ported (libs/world/throwables + RoundSim dispatches), tracked D-THROW-1..9; §28 the entity Flags dword bit table, consolidation 2026-07-29 — backs the kEntityFlag* constants in world/entity.h) |
| World (AI parity refresh) | [`world/world-wac-ai-re.md`](world/world-wac-ai-re.md) | landed (§26 allegiance/damage/mounted-weapon parity, grill-ida 2026-07-20: BMS combat flags + Berserk same-team exception, actual-hit alerting, retail no-op near misses, mounted aim/request/FSM fire, collision-force suppression, and death-detach animation witnessed + ported; residual automatic ADM-duration source tracked as D-WPN-26) |

The UNAUDITED set was emptied by the PAR-R1..R7 sweep (2026-07-05), then
reopened the same day with the three runtime-render systems the REN track
audits — materials/state, draw order, lighting (the records land under
`docs/render/` at REN-2/3/5; see the
[divergence ledger](divergence-ledger.md)'s audit track and
[ADR 0023](adr/0023-render-visual-parity.md)). Every other subsystem has a
dedicated RE record (full or partial) or a tracked-by-composition audit. The
project glossary lives at the repo root in [`CONTEXT.md`](../CONTEXT.md); the
project vision is [`GOALS.md`](../GOALS.md).
