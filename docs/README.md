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
| [`oned/workspace-maturity-program.md`](oned/workspace-maturity-program.md) | The maturity program's ONED track: the R/W/E/G capability bar, the twelve-workspace matrix (Avatars joins at AVA), foundation services, the music redesign, responsiveness, test-seam refits, per-workspace phases, and the RE ledger gating them |
| [`asset-gated-tests.md`](asset-gated-tests.md) | Every env-var-gated test: the var→data matrix, the skip-passes-as-green caveat, local setup, why captures are never committed, and the CI stance |
| [`maturity-program.md`](maturity-program.md) | The maturity program (the pre-reimplementation rearchitecture push): seven tracks, five waves, the boundary-conformance checklist, enforcement ratchets, and gates — the program dashboard |

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
| Net | [`net/novaworld-net-re.md`](net/novaworld-net-re.md) | authoritative NovaWorld wire record; current through §5.60 + D-NET-155 (retail-join fidelity: D-NET-141..155 fixed + live-verified — 152 = client fire: C 0x06 → clip authority → tag-2 round-event echo, wire-verified v28 2026-07-03; 151 = the grounded-on-entity teleport, wire re-verified v28 across 4 carriers incl. a pool-1 deck; §5.9.1 corrected — tag-2 is a round-FIRED event, not a hit; §5.60 round sim + damage + death family witnessed, ported, and v29-live-exercised (kills + 0x13/0x1E death broadcast on the wire; D-NET-153 fire-direction frame / 154 tag-2 starvation / 155 stale 0x16 roster found live and fixed); v30 verified kill symmetry both ways + the 25-event positive tag-2 witness; game type locked to ADVANCE AND SECURE (0x10010); §5.61 AS spawn selection + zone-capture loop witnessed 2026-07-03 (the 0x0E deploy pick, the ZoneSlotChain frontier, spawn waves 0x6E, the 1 Hz capture block 0x53/0x6F/0x6C + 0x1E zone events, the control formula) + slice-1 port landed; v31 live = 4 failed riders → the ROUND-14 ALIGNMENT PASS (2026-07-03) witnessed AND ported all four: D-NET-156 deploy-screen hold (0x0A flags1 bit1 = slot respawn-pending, join-set iff spawn zones exist, 0x0E dead-or-pending gate + clear; the picker rows are client-local), D-NET-157 vehicle attach/detach (0x26/0x27 dispatch → the @0x435AA0 validation chain, the 0x0A mounted-branch echo is the confirmation), D-NET-158 HUD count (the 0x04 capacity byte terminates the 0x46/0x22 roster walk + join 0x46 broadcast + live 0x16 trailer counts; §5.20 rewritten), D-NET-159 body anims (the server RECOMPUTES anims from replicated input — the newly witnessed C2S 0x1D stance-change feeds MoveOrder bits 8-9; the 0x0A tail state byte echoes the recipient's own stance — the crouch/prone bug); v32/v33 live closed the loop twice more: D-NET-156 tail = the deploy-RELEASE bundle 0x5A+0x61+0x1E (the 0x5A apply resets the client's 81474C wait-gate — the v32 rubber-band) and D-NET-160 = the victim death cycle (live 0x0A tail health + the record dead bit; the killee now sees its death and redeploys). v33 verified: movement, deploy picker, crouch/prone, emplacement mounts, the roster contract. round 15 (2026-07-04) witnessed + ported the vehicle DRIVE chain — the assumed "mounted form + ownerSession grant + vehicle uplink" model was REFUTED (no vehicle uplink exists; the §5.13 short form is the DEAD-pose form; vehicle +0x1CC is an effect emitter) and drive = the HOST vehicle motor off the driver's replicated input, ported as world::vehicle_motor (D-NET-161, ground family; helos deferred); the §5.61 slice-2 capture loop landed the same session (world::zone_capture_tick + the 1 Hz 0x6F/0x53/0x1E/0x40 wire block — map colors/LFP capture; D-NET-162). OPEN, ranked: v34/v35 live verifies (death cycle D-NET-160; buggy drive + capture loop), the air/helo motor family, the client 0x5A apply witness (HUD magazines)) |
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
