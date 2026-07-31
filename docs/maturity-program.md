# OpenNova maturity program

Before further reimplementation: rearchitect where it pays, refactor the
rest, and institutionalize the codebase design. Started 2026-07-04, after
the editor-layer program (docs/oned/editor-layer-program.md, complete) and
grounded in a three-way exploration of the tree (net/libs topology, the
ONED/engine boundary, products/standards) whose findings are folded into the
track descriptions below.

Two things hold at every commit of this program:

- **The wire-compat invariant** — our client joins retail servers, retail
  clients join ours (ADR 0010's standing amendment). Every net-touching
  slice proves it (see the NET-0 gate tiers).
- **The stop-anywhere property** — one slice = one independently-green,
  revertable commit; the program can halt at any boundary having paid down
  real debt.

The architecture constraint the whole program serves: **ONED is a detachable
layer over public engine APIs, and the engine never depends on the editor**
(ADR 0016). Products: exactly two shipped exes; the server is a serve MODE
of the game (ADR 0015).

## Status

| Field | Value |
|---|---|
| State | **CLOSED 2026-07-12** at the post-trunk-2 boundary, under the stop-anywhere property (maintainer decision, the close-out session). Waves 0–1 complete (Wave-1 trunk #204 = `50fce423`); Wave 2 landed the boundary flip (#205), ENG-2 (#206), ENG-3 (#209), the full REN track (#207 — REN-3..7 pulled forward from Wave 3), Wave-2 trunk 2 (#227 = `66076f66`: LIBS-2/3, ENG-4, the ENG-6 manifest), and the close-out tail (PR #236: STD-2 conversions, ONED-W1 TER-1/ENV-1/CRE-1/FNT-1/STR-1/STR-2, the MUS-D design at the maintainer gate, ENG-5 sweep #2, D-FNT-4, this close-out). Everything not executed is dispositioned in the Close-out section below |
| Final wave reached | 2 — portability + standards adoption. At close: ENG-2/3/4 done (#206, #209, trunk 2), ENG-5 sweeps #1 (boundary) + #2 (close-out) run, ENG-6 done (R8 + the trunk-2 manifest); LIBS-2/3 done (ADR 0024); STD-3 flip done (boundary), STD-2 seeds converted 7/8 (the tail; the object-material-defs contract deferred with cause); ONED-W1 six slices landed (the tail), ONED-MUS-D design landed (acceptance = the open maintainer gate); the full REN track closed. Not executed: ONED-RSP, ONED-TST refits, the ONED-W1 remainder — dispositioned below |
| Boundary gates (Wave 1→2) | full ctest: 246/248 green; the two reds are pre-existing, tracked, and not Wave-1 regressions — `npruntime_golden_gameplay` (TODO.md, D-NET-159 wire adjudication, asset-gated so CI never sees it) and `opennova_python_pytest` (diagnosed at this boundary, TODO.md: the two DCC-parity tests build a zero-write request the jobs validation rejects, then their in-parent `import bpy` poisons the 13 spawn-context worker tests — all 13 pass in isolation). FULL GUT attested in the boundary PR |
| Freeze | **LIFTED at close (2026-07-12)** — new reimplementation work no longer waits on foundation phases. What survives is the standing rule set, not the gate: ADRs 0015–0018 (products/serve mode, engine–editor boundary, typed records, public-API testability), 0019–0021, [0022](adr/0022-divergence-burn-down.md) (the PAR zero-OPEN target — burn-down continues as standing policy), [0023](adr/0023-render-visual-parity.md) (the REN rules), [0024](adr/0024-lib-family-topology.md) (family topology), and every instrument in the enforcement table (all hard-fail-forever by design) |
| Detail docs | ONED track: [docs/oned/workspace-maturity-program.md](oned/workspace-maturity-program.md) — survives the program as the editor's standing roadmap; its phases execute as ordinary slices under the standing ADRs, RE gates unchanged |
| Decision ADRs | [0015](adr/0015-two-products-serve-mode.md) products/serve-mode, [0016](adr/0016-engine-editor-boundary.md) engine/editor boundary, [0017](adr/0017-typed-records-named-constants.md) records/constants, [0018](adr/0018-public-api-testability.md) testability, [0022](adr/0022-divergence-burn-down.md) divergence burn-down, [0024](adr/0024-lib-family-topology.md) lib family topology + consumption models; boundary ADRs (npwire, world seam, responsive shell) minted in their tracks at decision time |

## Close-out (2026-07-12)

The program closed at the post-trunk-2 boundary under the stop-anywhere
property. Wave-2 exit-gate accounting, honestly:

- **env GDScript deleted, vectors green** — met (ENG-2; re-verified by sweep
  #2 modulo the tracked TOD-clock arrival, a post-ENG-2 fidelity-train
  addition with its own checklist row).
- **conformance checklist closed-or-tracked** — met: every row is closed or
  carries a tracked slice; sweep #2 added the fidelity-train rows.
- **ratchets hard and trending down** — met with an honest caveat:
  `libs_uncited_src_files` fell 88 → 60 across the program;
  `test_private_pokes` rose 1319 → 1384 via maintainer-ratified bumps (AVA's
  two pre-ratchet white-box files, probe diagnostics, the particle tests —
  each logged in the slice table). The claw-back remains ONED-TST's charter.
- **responsiveness landed + one re-baseline** — not executed; dispositioned
  below.
- **music design accepted** — the design landed (track doc §The MUS-D
  design); acceptance is the open maintainer gate.
- **REN instrument landed + materials converged** — exceeded: the whole REN
  track closed at REN-7.

Dispositions — every item not executed, and where it lives now:

| Item | Disposition |
|---|---|
| ONED-RSP (responsive shell) | ONED track doc backlog. RSP-1 policy ADR first whenever picked up; the two-floor conflict and raw-px persistence stay recorded there |
| ONED-MUS-D → MUS-I | the design landed (track doc §The MUS-D design), **at the maintainer gate**; MUS-I per its 8-slice plan after acceptance |
| ONED-TST | rides the hard private-pokes ratchet + the track doc's refit list; needs no wave gate |
| ONED-W1 remainder (OBJ-1/3/4, MNU-1, SND-1; F3 wirings CRE-2/FNT-3/STR-3/SND-3/OBJ-5/MNU-3/MUS-G) | ONED track doc backlog — ordinary slices under the standing ADRs |
| ONED-W2/W3, REF-1..4, REQ | ONED track doc backlog; the RE gates (R1–R7) unchanged — UI never ahead of the witness. R5 re-opened at CRE-1: the credits cadence constants (60 fps rate convert, 50 px fade zone) are uncited pending a grill |
| STD-2 remainder | the object-material-defs contract deferred with cause (native-owned in both directions; the 980-line materials inspector has no dedicated test bar — a conversion needs its own slice with tests first); everything else converts adopt-on-touch under the standing diff-scoped dict-contract lint |
| ENG-5 sweep-#2 tracked rows | ordinary port slices (libs/hud, libs/audio curves, libs/env TOD clock, the player_view fold, the part-anim integrator, the armory helpers); the sweep itself remains a standing instrument, run ad hoc |
| PROD-1 serve mode, PROD-2 taxonomy, PROD-3 release dry run | tracked future work; ADR 0015 remains the decision of record — implementation unscheduled |
| PAR ledger (at close: 46 OPEN / 13 NEEDS-RE / 31 witnessed-ready-deferred per the generated scoreboard; D-FNT-4 the newest mint) | unchanged: ADR 0022's zero-OPEN target is standing policy; `ledger_check` enforces sync forever; the burn-down continues in fidelity work |
| GOV-4 | this close-out |

What the program leaves behind, permanent: the enforcement table below
(goldens, ratchets, lints, link-graph, ledger check, ABI guard — all
hard-fail forever); ADRs 0015–0024; `libs/npwire` + the terrain_query seam +
the family groups; the witnessed render/lighting/env/terrain-query cores;
twelve workspaces on the R/W/E/G bar with two recorded exemplars (terrain =
E/G, environment = runtime-node reuse); the divergence-ledger discipline;
and this dashboard as the program's record.

Landed slices (hash per slice, newest first):

| Slice | Commit | Note |
|---|---|---|
| Standalone game debug runtime | (#376) | **Banked `test_private_pokes` 1353 → 1313 in `maturity_baseline.json`; recorded here after the fact** — the PIE→standalone replacement touched 63 paths under godot/tests (37 rewritten, 8 deleted, 18 added, incl. sidecars/scenes — probe scenes and debug-page tests moved onto the new session/MCP seams), and the slice landed the decrement without a dashboard row (the same gap the #377 row below records) |
| Retail control-register catalog completion | (#377) | **Banked three counters in one slice:** `has_method_guards` 585 → 579, `libs_uncited_src_files` 61 → 57, and `test_private_pokes` 1363 → 1353. Recorded here after the fact — the slice banked the decrements in `maturity_baseline.json` without a dashboard row |
| Infantry death-transition fidelity | (this PR) | **`has_method_guards` baseline 578 → 585, with each +1 remaining a real dynamic seam:** three preserve legacy/test skeletal providers while the new two-channel pose calls are optional, one preserves avatar fallback nodes without skeletal animation methods, one preserves revisionless/clockless presentation sources, and two classify heterogeneous wire-visual nodes for the new blend/tick methods. The three newly bound `NovaSimulation` local-player getters are deliberately unguarded after W4-2; this adjustment records capabilities that cannot be proven from the receivers rather than restoring typed-receiver defensive guards. |
| Quality W4-6 modtools splits (the W4 closer) | (this PR) | **New ratchet counter `oversize_gd_files`, baseline 7** — the counter the campaign plan schedules to land with the last W4 slice: no `.gd` under `godot/engine`, `godot/game`, or `godot/modtools` may grow past 1,200 lines without splitting first (`godot/tests` is deliberately out of scope — eleven test files already exceed the limit, and the test refit is ONED-TST's concern, not this ratchet's). The baseline is the residual floor the W4 god-file splits leave behind: `game_world.gd` (2,295 — the load path stays whole by design after the W4-3 extractions), `editor_workstation.gd` (1,533), `terrain_editor.gd` (1,446 — the W4-6d split moved every movable method bundle; the residual is the workspace facade: the flat accessor band into document/brush-session, lifecycle, input dispatch, and 33 load-bearing delegates), `mission_object_placer.gd` (1,389), `music_editor_document.gd` (1,306), `mnu_canvas.gd` (1,243), and `editor_mcp_menu_tools.gd` (1,223). Shrinking any below the threshold banks a −1 via `--write-baseline` in the slice that earns it |
| Quality W4-2 guard floor | (this PR) | **New ratchet counter `has_method_guards`, baseline 578 — a floor, not zero:** the kept set is the documented duck-type seams (GameWorld/LocalPlayerPresenter harness contracts), the modtools workspace capability hooks, and dynamic-name dispatch. The ~157 deleted were typed-receiver guards on methods proven bound/defined (+7 `has_signal` guards on declared signals, outside the counter). Shrinking the floor banks a decrement via `--write-baseline` in the slice that earns it |
| Quality W3-7b sim headers (the W3 closer) | (this PR) | **New ratchet counter `oversize_cpp_files`, baseline 2** — the counter the campaign plan schedules to land with the last W3 slice: no `.cpp` under `libs/`, `apps/`, or `godot/engine` may grow past 2,500 lines without splitting first. The baseline is the residual floor the W3 god-file splits leave behind: `godot/engine/simulation/nova_simulation_player.cpp` (2,877) and `apps/novaworld_server/http_listener.cpp` (2,646 — its 2,230-line `start()` was decomposed in W3-4, but the TU keeps 34 route handlers). Shrinking either below the threshold banks a −1 via `--write-baseline` in the slice that earns it |
| Weekly audit: citation detector exact-match + boilerplate sweep | (this PR) | **`libs_uncited_src_files` detector tightened from the substring `[orig` to the exact citation form `[orig:`, baseline 60 → 61.** The +1 is `libs/oed/src/rdta.cpp` finally counted honestly: its only `[orig` hits are the `remap[orig]` array indexes — CODE, not citations — so the substring test read an uncited file as cited. The six W3-2 collision TU headers also carried the same `[orig]` boilerplate sentence #349 reworded for mission/threedi; reworded here so no file can ever satisfy the gate on comment text again (all six carry real citations regardless). Also banked: `test_private_pokes` 1378 → 1366, earned by #342's public-seam probes and deliberately left unbanked by the W3-3b slice |
| Citation ratchet measures citations, not comments | (this PR) | **`libs_uncited_src_files` baseline 59 → 60, and the +1 is a gate finally SEEING a file it should always have seen.** The W3-1 mission-split file headers contained the literal token `[orig]` inside a sentence *about* citations ("the [orig] citations moved with the code they annotate"). `ratchet_counts.py` greps sources for `[orig`, and a grep cannot tell a citation from a sentence mentioning one — so all six new mission TUs read as cited. Four carried real citations anyway; `mission_capi.cpp` and `mission_names.cpp` did not, and passed the gate on comment text. Fixed: every header reworded so none contains a bracket token, and the claim now matches the file. `mission_names.cpp` EARNED a real citation rather than a baseline entry — its `ai_action_sub_type_name` is the port of the table `bms.h` already attributes to `[orig: dfx2med Med_ActionSubTypeName @0x445EE0]`. `mission_capi.cpp` is genuinely uncitable (our own flat C ABI over MissionDocument, not a port), so it becomes a visible uncited file — hence the +1. Found because W3-3 reported the counter IMPROVING 57 → 56 after splitting an uncited file in two, which cannot happen |
| Quality W3-3b GP read/write split | (this PR) | **`libs_uncited_src_files` baseline 58 → 59**, the same mechanical effect the W3-3 row below describes: `threedi_gp.cpp` carries no `[orig:]` citation, so splitting it into a read TU and a write TU turns one uncited file into two. Citation coverage is unchanged — `libs/threedi` is built from the format records in `docs/threedi/`, not from decompiled functions — but the counter counts FILES. With this, both of the library's format codecs are one-direction-per-file |
| Quality W3-3 threedi read/write split | (this PR) | **`libs_uncited_src_files` baseline 57 → 58, and the reason is worth reading.** Splitting `threedi_3di3.cpp` (which carries no `[orig:]` citation) into a read TU and a write TU turns ONE uncited file into TWO. Citation coverage does not change — the same uncited code is in the same library — but the counter counts FILES, so a motion-only split moves it. `libs/threedi` is built from the format records in `docs/threedi/`, not from decompiled functions: 11 of its 13 sources carry no citation, which is why it is not allowlisted like `pcapio` either (two files DO cite, so the library is not uniformly inapplicable). **Also recorded here as a correction:** the W3-1 mission split's file-header boilerplate contained the literal token `[orig]` in the sentence "the [orig] citations moved with the code they annotate". That made the counter read `mission_capi.cpp` and `mission_names.cpp` — which carry no real citation — as cited, so W3-1 passed this gate on comment text rather than substance. The boilerplate is fixed here for threedi and in a follow-up for mission (which will move the baseline again, honestly). A ratchet that can be satisfied by a comment is not a ratchet |
| Quality W2-8 pcapio | (this PR) | **Citation-allowlist addition, NOT a baseline bump:** `libs/pcapio` joins `citation_allowlist_libs` so `libs_uncited_src_files` stays 57. The pcap/pcapng reader parses a PUBLIC format (its header cites pcap-savefile(5) and the pcapng block spec), so it is infrastructure like io/vfs/resource_index, not a reimplementation of witnessed engine behavior — an `[orig:]` citation is inapplicable by construction. Also: `PcapWriter` deleted (229 lines, zero references repo-wide) |
| Quality W1-3 libs log sink | (this PR) | **New ratchet counter `libs_stdout_prints`, baseline 0.** `io/log.h` is the one diagnostic channel for `libs/`: libraries call `io::logf` and stay silent unless the host installs a sink (`novaworld_server` and `nw_server` install stderr/stdout sinks; `NW_LOG_DEBUG=1` opts into kDebug). All 57 console writes across 7 libs routed; `rdta.cpp`'s unconditional cwd `stripify_call_0.log` writer deleted (with its always-0 call counter and `OED_STRIPIFY_LOG_PATH`), `OED_STRIPIFY_SEED_DIAG` now defaults OFF. FILE*-parameter writers (tdp/mus/adm) are deliberately outside the counter |
| Quality W1-2 print-zero | (this PR) | **Two new ratchet counters, baseline 0**: `gd_prints_outside_debug` (raw print family in godot/{engine,game,modtools}; sanctioned channels = push_error/push_warning, print_verbose, the F3 system; screenshot_capture allowlisted as a CLI driver) and `cpp_binding_console_writes` (UtilityFunctions::print/printerr, print_line, WARN/ERR_PRINT in godot/engine). The sweep that zeroed them: ~40 binding `printerr` → `push_warning` (these legs report via their return contract; GUT counts engine errors as failures, and the negative-path tests drive them), skeletal-anim `WARN_PRINT` → `push_warning`, the joiner freeze-tripwire `print_line` → `print_verbose` (its OPENNOVA_NET_DIAGNOSTICS gate retained), PerfTimeline + GameWorld warm-pass summaries → verbose channel, the NOVA_INF_DEBUG diagnosis leftover deleted, the overlay's duplicate pose print and the MCP banner print dropped. Channel policy recorded in godot/engine/CLAUDE.md |
| Host-punt handling (PR #300) | (this PR) | `test_private_pokes` 1376 → **1378** (+2, maintainer-ratified 2026-07-26). `godot/tests/net/host_punt_surfacing_test.gd` installs a runtime double into a REAL `GameWorld` (`_runtime` / `_loaded`, two lines in one helper) to pin the D-NET-177 contract that a host's close surfaces exactly once and never re-opens the deploy screen — GameWorld exposes no public runtime-injection seam, and `game_world_test.gd` already drives the identical one. The round's other five pokes were removed instead: the shell case now reads `find_children(…, "NovaDeployScreenPresenter")`, `get_node("World")` and `is_gameplay_input_active()` rather than `_deploy_presenter` / `_world` / `_state` |
| Gates repair after the #232 merge | (rides PR #236) | master merged #232 (the loading screen) with a red STD-1 ratchet: `test_private_pokes` rose 1384 → **1393** (+9, all `loading_screen_test.gd` `screen._in_session`/`_title`/`_mission_name`/`_custom_text`/`_game_type_text`/`_texture` state reads — logged here per the ratchet policy; baseline bumped via `--write-baseline`, MAINTAINER RATIFIES ON MERGE). Every open PR is red on this gate until this lands. The ledger scoreboard was already regenerated by #232 itself |
| Wave-2 tail + GOV-4 close-out (PR #236) | (this PR) | the closing train, ground in parallel agent slices and consolidated green: **STD-2** 7/8 seeded contracts → typed records (DocumentTabRow, TileGizmoState, ReferenceServices deduped, ReferenceEdge, FocusPayload + EditorNavLocation, MissionParamSpec/SlotSpec, McpToolDef; net −9 class-level Dictionary signatures, 3 transport edges precisely allowlisted); **ONED-W1 ×6** — TER-1 (at bar, the E/G exemplar; F3 staging note via the new `get_game_launch_note` hook + typed GameLaunchNote), ENV-1 (at bar; save-into-launch-dir is the staging; popup note routing), CRE-1 (scrub transport; cadence UNCITED → R5 re-opened), FNT-1 + STR-1 (EngineTextPreview adopted — the game's draw path replaces the Label truth-claims), STR-2 (encoding pinned; **D-FNT-4 minted** — cp1252 specials fall to the system-font fallback where retail draws `glyph = byte−32`); **MUS-D** design landed at the maintainer gate; **ENG-5 sweep #2** on the conformance checklist; this close-out. Gates: canary 719/720 (the documented intra-file flake, isolation-green ×2 + master-green), mission_inspector 125/500, every touched suite re-verified on the consolidated tree, ratchets +0, dict-contract 0, ledger synced, link-graph 0 forbidden |
| ENG-6 manifest leg (Wave-2 trunk 2) | (this trunk) | `libs/gameprofile/required_resources.{h,c}` instantiates the R8 record — ~75 rows phase-major in the witnessed load order, severity classes (FATAL/DIALOG/REQUIRED/SOFT/OPTIONAL), the boot-archive-table any-of flag, per-row witnessed failure text + `[orig]` citation; Model B only (no C-ABI change); `required_resources` ctest pins the eight-row fatal set + completeness. `NovaResourceRoot.list_missing_boot_resources()`/`boot_resource_failure_text()` probe the individually-fatal file rows (the archive trio stays `mount_runtime`'s gate), and `main_game.gd` raises honest missing-resource errors at mount (reported-not-enforced — the picker flow keeps a partial dir inspectable where retail MessageBox-exits). ONED conveniences over the same table stay ONED-REQ (Wave 3) |
| ENG-4 fonts + OED_UPDATE_* (Wave-2 trunk 2) | (this trunk) | **both conformance rows closed**: `fnt_pack_shelf` ports the rasterizer's deterministic shelf packer into `libs/fnt` beside the format facts (authoring policy, not witnessed engine behavior — noted in-header), bound as `NovaFntResource.pack_shelf` + six format class constants; `fnt_rasterizer.gd` keeps only TextServer rasterization + blits (constants now aliases, `_pack()` deleted); `fnt_pack` ctest hand-walks the reference layout + the 16-page boundary, GUT `fnt_resource` gains the binding contract pin. `OED_UPDATE_*`: the C++ chain was already single-sourced (libs/oed → `NovaObjectData::UPDATE_*` → bound constants) — the three GDScript re-declarations became aliases (engine model + workspace) and deliberate literal pins (the test suite, + a new binding-drift test), the ENG-3 contract-pin pattern |
| LIBS-2 + LIBS-3 family topology (Wave-2 trunk 2) | (this trunk) | ADR 0024: one-lib-per-format affirmed; `opennova_terrain_family` + `opennova_audio_family` minted as pure INTERFACE link groups (`libs/families.cmake`, included by both CMake roots) and adopted by the GDExtension in place of the twelve member lines — link conveniences, never merges, never an edge-laundering path (`link_graph_check` 330 targets / 0 forbidden). The ADR records the ADR-0023 renderer-fold reversal where the clause originated, and names the two consumption models; LIBS-3's operational text (Model A flat C ABI / Model B C++ static link, mixing rules, the annotate+baseline-bump protocol) lands in libs/CLAUDE.md |
| Gates repair after the #230 merge | (this slice) | master merged #230 with two red STD-1 gates (the #229 pattern): the merge's post-review particle tests raised `test_private_pokes` 1370 → **1384** (+5 `game_world_test.gd`, +6 `particle_authoring_test.gd`, +2 `effect_world_test.gd`, +1 `particle_editor_workstation_test.gd` — logged here per the ratchet policy; baseline bumped via `--write-baseline`, MAINTAINER RATIFIES ON MERGE), and the review round edited ledger tables without regenerating the count-to-zero scoreboard (`ledger_check.py --write` re-run). Every open PR is red on both gates until this lands. |
| Gates repair after the #226 merge | (this slice) | master merged #226 with two red STD-1 gates: the ledger's new D-ANIM-1 row had no scoreboard domain mapping (`ledger_check.py` DOMAIN_ORDER gains `ANIM` → World / AI + events; scoreboard regenerated) and `test_private_pokes` rose 1367 → **1370** (the three pokes are #226's own `weapon_round_probe.gd` `_host._vm_parts`/`_host._viewmodel` reads — probe diagnostics, logged here per the ratchet policy; baseline bumped via `--write-baseline`, MAINTAINER RATIFIES ON MERGE). Every open PR was red on both gates until this lands. |
| ENG-3 terrain query APIs (PR #209 train: A, C, B0, B1a, B1b) | 9a7c05b3 / 25a7cc01 / 09fd1ca2 / 54c3f269 / (B1b this slice) | one slice per boundary-conformance row: **A** scalar height sampler unified on the C++ live core (`NovaTerrainData.sample_height_world_live`, GDScript bilinear deleted, parity test flipped to pin the single path); **C** sector/atlas constants single-sourced (`COORDS_ATLAS_SIZE`/`COORDS_SECTOR_ID_MAX` minted, four bound class constants + `world_to_sector_cell`, all GDScript re-declarations now aliases, contract pin); **B0** the raycast witnessed (terrain-re.md §Runtime terrain queries: the ~1-unit major-axis march + point-coarse/bilinear-confirm, the ÷4-step ≤9/≤9/8-bisection refine, the column-shortcut crossing rule; headline IDB repair — `null_stub @ 0x6067b0` was the canonical bilinear height sampler mistyped `void()`, retyped + renamed `Terrain_SampleHeightBilinear`, ~40 callers un-elided; 14 renames saved); **B1a** the core ported to `libs/terrain_query/terrain_raycast` (16.16 structural translation behind a point+bilinear sampler seam, ~60-check ctest, four witness claims sharpened at the port against live decompiles); **B1b** `NovaTerrainData.raycast_terrain(from, to)` over live-image/baked-CPT substrates, mission picking + celestial glare adopted, the GDScript march/slab/bisection trio deleted, D-TERRAIN-4 minted (editor-guard residuals, class C candidate). Checklist rows 1/2/3 closed; ENG-4's fonts + `OED_UPDATE_*` row is the remainder |
| REN-7 close-out (REN TRAIN END) | (this slice) | the close-out gates, all green: **FULL GUT 2006/2007** (200 scripts, 27791 asserts, 0 failures — the single non-pass is the documented asset-gated avatar-preview PENDING; silent-drop greps clean via `scripts/test_godot.sh`); **full ctest 248/250** — the only reds are the two standing tracked ones (`opennova_python_pytest`, `npruntime_golden_gameplay`; TODO.md), `parametric_parity` disabled as always; **T1** green on the fresh detail-stage golden; **T2** = swatch attested at the detail slice (exactly the 40 detail-keyed cells moved, 80/80 identical), **composite IDENTICAL vs post-gamma** (ordering untouched through REN-5/6/7), world = post-domeband2 (the skyfog fill live; the pure-land framing = the bitwise determinism sentinel); **T3** scene-by-scene table landed in the trunk PR — scenes 1/2/5 our-side captured at head, 3/4/6 enumerated on their tracked rows (D-RLIT-5, D-RMAT-8, D-RORD-4), banked RckS05 authentic-dark cited, the W_RCK1_O watch item CLOSED as D-RMAT-10; the by-eye retail pass is the maintainer's attestation. **Ledger**: the folded env rows ALL FIXED (#17/#19/#21/#27/#29/#30/#33/#34); D-RMAT-1..5/7/9/10 FIXED, -8 PERMANENT, -6 WRD riding D-RORD-4/-5; D-RORD-1 FIXED, -2/-6 PERMANENT, -3/-4/-5 tracked with named riders; D-RLIT/-FOLIAGE/-TERRAIN rows dispositioned with named riders; scoreboard regenerated. **IDB/dead-variant catalogs** current in the records (this train's rename logged in the env table; REN-6's appendix prior). **Correspondence** env+materials rows carry the REN-6/7 state; render README carries the T3 pointer; the conformance MATERIAL_FLAG_* row closed (single-sourced since REN-2, extended not re-duplicated at REN-7) |
| REN-7 detail stage (D-RMAT-10 minted-and-FIXED) | (this slice) | the T3 "W_RCK1_O watch item" resolved into a witnessed divergence: the `_MT` secondary (detail) stage ran HALF the witnessed combine — composer `base.rgb *= detail.rgb` (×1, alpha untouched) vs the witnessed stage 1 `TSSColor(1, Modulate2x, Texture, Current)` + `TSSAlpha(1, Modulate, Texture, Current)` (§FF technique tables) — so resolved MT surfaces (RckS05's gray `W_Rck1_o`, avg 93/255) modulated ×0.365 where retail runs ×0.73: MT objects TOO DARK in detail regions, the exact "retail reads brighter" direction banked at the RckS05 adjudication. Fixed composer-side (`× 2.0` + `base.a *= detail.a`, cited) + host-side stage-drop gating (`OSCAP_DETAIL` masked off the key when the secondary fails to resolve — retail's NULL-texture drop, exactly; the white ×1 fallback deleted; `classify()` pure). UV-set question settled with data: the .3di v8 vertex carries TWO authored UV sets (stride 40; RckS05 uv1 distinct 48/48, FOUNTAIN M4 455/455) — `v_uv2` was always the right sample. T1 re-dump: exactly the 224 `OSCAP_DETAIL` composed hashes moved (+27 bytes each), 0 classification rows, handoff pins unchanged (`FF_MT_OP/base → 0x00001004`); ctest renderer 5/5; GUT keystones green in isolation (handoff 3/3, object_editor 73/73, game_world 10/10, part_anim 10/10, submesh cache 8/8). The ×2 verified CORPUS-UNIFORM at the port (retail `localres.pff` .fx re-derived via `libs/pff`+`libs/scr`): `BDiffT2.fx` (`VS_DOT3DIFF2`) identical stage-1 pair; `SkBDiffO2.fx` (`VS_SKBUMPDIFFOBJ2`) same pair in its P3 post-multiply pass (DESTCOLOR/SRCCOLOR ×2 framebuffer form) — every family the key bit reaches is witnessed. Record gains the stage-1 `#UV` TextureTransform open question (identity for all non-`#UV` MT). T2 swatch A/B (instrument compare): EXACTLY the 40 detail-keyed cells moved (FF_MT_OP/AB/AD ± _LUM + VS_DOT3DIFF2 + VS_SKBUMPDIFFOBJ2, all variants), 80/80 non-detail cells bitwise identical. CP15 mission recapture attests the world-visible delta |
| REN-7 dome-band fill (D-TERRAIN-3 FIXED) | (this slice) | the below-horizon witness closed: retail fills the below-rim region with the **frame clear alone** — no skirt/ring geometry anywhere in the frame walk (`Terrain_RenderSkyboxPass @ 0x610ac0` → `Terrain_RenderSectorBatchLit @ 0x60c670`, ex `sub_60C670` = the plain fogged sector batch bracketed by white-ambient/lighting-on; the dome pass fogs toward the DOUBLED SKYFOG while drawing `[orig: sub_579CB0]` — the same block the clear paints, which is the whole seam-invisibility mechanism). The host defect was TWO stacked wiring bugs, not a missing witness: (1) the env #21 clear consumer wrote `background_color` into a `BG_SKY`(null-sky) Environment (Wave-1) that renders BLACK — the 1-px black dome-rim seam in every runtime view, the aerial black band, the water-horizon grazing band once #29's fade landed; fixed to `BG_COLOR` + `AMBIENT_SOURCE_DISABLED` (Godot ambient never injects), GUT-pinned — post-#29 the exposed defect was a PURE-BLACK BAND filling the whole strip-edge-to-rim region (near half the frame in dusk water views), not just the 1-px rim seam; (2) `get_frame_clear_color()` served the UNDOUBLED blend (the 07-05 "non-modulate2x device" reasoning inverted for this host — since D-RMAT-7 the host reproduces the MODULATE2X framebuffer, whose Clear takes the post-blend DOUBLED skyfog verbatim; the undoubled band measured exactly half the fogged rim); fixed to blend-then-double `[orig: @ 0x57f037..0x57f0a1 then @ 0x57f1b1]`, env vectors re-dumped (frame-clear token only), underwater branch verified already render-space, and the blend's distance input corrected to the smoothed current (`Env_FogDistCurrent @ 0x26c681c` — the #27 seam claim now holds for the clear). Ledger: D-TERRAIN-3 FIXED + D-TERRAIN-2 catalog row back-filled (terrain 0 open / 3 settled; total OPEN 35 → 34). World A/B (post-domeband2 = the rolling world baseline): the band renders the skyfog fill continuous with the dome (rim step ≤5 LSB at noon/dusk, EXACTLY 0 at night — the same fog-vs-skyfog two-block relation retail has, riding #20's cosmetic caveat); pure-land t1200_sun bitwise identical vs post-ren6-30 |
| REN-6 tail + fidelity riders (REN-6 CLOSED) | bc4e8e3f / 12f76202 / 671d6d17 / 442285b6 / 2c5b52cf / bef63329 | **D-RLIT-8 minted-and-FIXED** (object hemi_sky joined the smoothed writeback — the "buildings too dark off-noon" bug; night register verified on CP15); **D-FOLIAGE-2 minted-and-FIXED** (the foliage combine ported to the witnessed blend PS `[orig: @ 0x5ff7a0]`; D-FOLIAGE-1 narrowed, D-FOLIAGE-3 minted); the mission_visual_probe T3 ground-POV instrument (+ the public `set_resource_root_dir` knobs, private-pokes ratchet 1371 → 1370); **env #29 FIXED** (the strip-march water live end to end — `env::water_*` structural translation with 40 ctest pins, the packed-array bridge, the per-frame strip ArrayMesh, and the witnessed detail≥2 **embedded ps.1.1** found at the port's debug `[orig: Water_InitSurfaceShaders @ 0x5c19b0]`; witness re-grades landed: underwater-view arg, tier constant families, vertex-vs-prim counts, depth clamps); **env #30 FIXED** (host planar reflection — SubViewport mirror camera feeding the ps.1.1 t2 slot; the witnessed rows pin the mapping so the texm3x2 runs verbatim; clip plane approximation tracked in-row). REN-6 is CLOSED — all four env leftovers FIXED; Environment 3 open. Goldens re-pinned surgically (water/mesh, water/strip; water/snap retired). Remaining: the below-horizon skyfog band (the fade exposes the dome's black below-rim region — D-TERRAIN-3's substance, needs its own dome witness) rides REN-7 |
| REN-6 witness + first port leg | (this slice) | all four env-leftover unknowns closed in IDA (witness commit 2cf6ba77): the #33 generator FOUND (`Star_GenerateInstanceTable @ 0x5ac850`, ex `init_weather_particles` — the loop starts at table+16, why the base had no xref; per-star math fully decoded; dead PRNG stub `@ 0x5ac010`), the #27 mystery pair = the RAIN PERCENT spring (`Env_RainPct*` — "Rain: %i%%" debug label, net-synced target, >48 drives particle fall decay), the #29 strip decode with exact constants (2..9 = adaptive ROW stride `clamp(int(1/w×500),2,9)` — doc corrected; 3 verts/row; ≤5-row batches via static index tables; the murk chain + fog W clamps pinned — the detailed-tier chain, clamps, batch prim count, and the "isReflection"→underwater-view arg were RE-GRADED at the 2026-07-07 libs port leg's IDB re-read, env-tod-re.md §The strips), the #30 reflection internals (RTT begin/end on `Water_ReflectionTexture`, skyfog clear, clip plane wh−0.1, `Water_RenderReflectedWorldScene` = the ex three-flush decompile-fail — a stale NORET flag; builds the water CLIP-plane texture matrix `u = y−wh+0.5` + arms `g_WaterMirrorActive`; render-order-re.md's open question closed). Port leg: **env #27 FIXED** — `env::EnvScalarChannels` (witnessed spring/eighth steps in the witnessed in-tick position, ctest-pinned; targets-only snap semantics) ticked by `NovaWeatherCore`, smoothed fog-distance/sky-height served through `NovaEnvironment.set_smoothed_scalars` to every consumer (dome, water UV, object/terrain fog, frame clear), SunDim live through celestial sun alpha + glare fold; **env #33 FIXED** — `env::generate_star_instances`/`star_twinkle_tick`/`star_visible_fixed` (PRNG pins from seed 1, 256-star invariants) hosted by `NovaStarField` + the camera-anchored billboard MultiMesh in `nova_celestial.gd` (per-star twinkle via instance color, 0.98 near-light cull, regenerate-per-load; the single-body stand-in + its dead-variant 0x2000 opacity deleted). Ledger: Environment 7 → 5 open. GUT: env vectors 204/204, env_file, sky, environment editor/preview/badges, celestial parse+field smoke (252/256 visible at zenith — the cone culls 4) |
| REN model-parity: witnessed preview defaults + static-batch relighting | ff13e643 (+ rider b93d29b0) | the follow-through on the color-pipeline slice, from a live world audit (the mission workspace's 943-object scene, per-material uniform diagnostics): **un-enved preview defaults re-derived to the retail noon register** — full_00.env tod 1200 block bytes (sun 170,170,167; sky 84,88,89; ground 49,55,46) across the composer uniform defaults, `nova_object_model.gd` DEFAULT_*, the `opennova_*` shader globals, and the terrain include (one cited register; the old ad-hoc trio was tuned for the pre-gamma pipeline); **D-RLIT-7 minted-and-FIXED** — the placer's static MultiMesh batches froze the load-time env snapshot (no live owner; the pre-first-iris-tick modulator baked in) where retail relights every entity per frame `[orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0]`; the placer now registers harvested batch materials and re-stamps them per frame via the container's `mission_batch_env_stamper` (generation-gated; stamping single-sourced as `NovaObjectModel.environment_values_from`/`apply_environment_values`; verified batch == live-model uniforms after settle). Audit byproducts, all verified clean: every batch carries a bound ShaderMaterial (0 null overrides/446), every diffuse bound (0 unbound), no black texture decodes, no degenerate normals. T1 re-dumped (key set identical — the defaults are composer text); T2 swatch 68/120 lit cells moved (defaults-only delta; emissive/tracer cells untouched); placer/controller/collision/resolver/object/canary GUT green |
| REN model-parity: the gamma-space color pipeline | 024f7f54 | maintainer-priority interleave before REN-6 ("make the 3di/material/texture world look faithful"): the retail color pipeline witnessed **gamma-space end to end** — no `D3DSAMP_SRGBTEXTURE` at any device sampler-state site (full sweep: only ADDRESS/FILTER/LODBIAS/MAXMIP/ANISO states `[orig: CD3DDevice_InitializeDisplay @ 0x679c1b; CGfxDevice_ApplyRenderStates @ 0x67e3ec]`), no `D3DRS_SRGBWRITEENABLE` at any of the 170 render-state sites, zero sRGB states in the 44-file `.fx` corpus, identity display ramp at default gamma 1.0 `[orig: GLib_SetGammaRamp @ 0x677be0; @ 0x84f354]` — while the host decoded textures sRGB→linear and re-encoded at the blit around the witnessed math (compressed lighting contrast; the washed-out world). **D-RMAT-7 minted-and-FIXED**: raw sampling + gamma-space math + the exact-inverse `nova_gamma_to_linear` output across the object composer and all 8 world shaders (`godot/shaders/nova_color.gdshaderinc`); the swatch probe gained a **calibrate mode** proving displayed-byte == computed-byte 256/256 on the live build; **D-RMAT-9 minted-and-FIXED** (the composer's invented fog ramp → the witnessed device fog table `[orig: @ 0x58a950 → @ 0x677960]`); **D-RMAT-8 minted-PERMANENT** (blend-space residual; ADR 0022 register, which also gains the back-filled REN-3 D-RORD-2/-6 entries). T1: key set identical, all 630 hashes cited-re-dumped; T2: swatch 120/120 moved (the expected global response change), composite IDENTICAL (ordering untouched), world set re-captured (post-gamma = the new rolling baseline — the washed-out sky/terrain now render the saturated retail register); GUT keystones green (handoff, object model, object_editor 73/73, canary, env vectors, env_file, badges) |
| REN-5 lighting | e24c6a9f (+ rider bd0a5840) | the world lighting chain witnessed end to end and ported: **env #17 CLOSED** — the iris/modulator chain is LIVE (`libs/env::ModulatorChain`, the witnessed modulator2→modulator→blocks tick order `[orig: @ 0x57ef97..0x57f03c]`, the 62-tick exposure chase `[orig: @ 0x57e512; ColorBlock_SetStepDeltas @ 0x57d940]`, ÷64 gain to ColorSrcGlobalGain/ambient scale `[orig: @ 0x58db30; @ 0x5aaef0]`; env vectors re-dumped surgically — exactly 8 weather rows, hand-verified `0x31·61/64 = 0x2E`); **D-RMAT-5 CLOSED** — the composer emits the witnessed FF MODULATE2X model on the pinned uniform surface (slots 225-230 + 232; the ×1.5/×1.6/spec-0.8 prototypes deleted; T1: key set identical, all 630 hashes cited-re-dumped, +30 lighting vectors in NEW section 5; T2: composite IDENTICAL, swatch 116/120 cells moved with the 4 unlit VS_TRACER cells byte-identical); the entity chain decoded (writer `@ 0x5c8090`/store `@ 0x5d89e0`, reader `@ 0x5d98a0`, hemisphere delta lights `@ 0x5d8cb0`, the REN-4 "dual-LOD lerp" erratum corrected to the INTERIOR DAYLIGHT lerp, effectScale = 3-ray sun visibility `@ 0x5c6800`) and ported to `libs/renderer/light_runtime`; terrain/foliage **c0/c1 = sky/light** closed (`terrain_lighting.gdshaderinc` corrected from the gobj-era pairing); point-light math + group culling witnessed (= the OED attenuation); `Lighting_InitTextures` + the cubemap sources witnessed (CubeRotSpecular = the static sun-glint cube — D-RORD-5's substrate answered); [`render-lighting-re.md`](render/render-lighting-re.md) + D-RLIT-1..6 minted (audit track **1 → 0**); 15 fn + 49 data IDB renames; GUT keystones green (handoff 3/3, object model 10/10, object_editor 73/73, canary 89/89, env vectors 204/204) |
| REN-4 shaders/TSS | fb1c8e7c | the fixed-function shader/TSS layer decoded end to end: the engine render-mode word (blend/alpha/color combiner tables `[orig: decode_blend_mode_to_d3d_states @ 0x680f00; decode_mode_alpha_stage @ 0x680b00; decode_mode_color_stage @ 0x681080]`) + the state permutation cache (`[orig: @ 0x681d00; RenderState_CacheFindOrAdd @ 0x683420]`); the water surface material set (`Water_InitSurfaceShaders @ 0x5c19b0` — the "Terrain_InitShaders" misnomer resolved) ported into `water.gdshader` (witnessed ONE+dst·SRCALPHA blend via premul-alpha + ref-32 discard; env #34 minted-and-closed); the terrain surface shading witnessed into terrain-re.md §Runtime surface shading (8 embedded pixel shaders incl. the ps.1.4 splat, tier gates, foliage-model VS/PS set, alpha refs 180/8); D-RMAT-2 FIXED (`OSCAP_VIEW_FADE` tracer fade) + D-RMAT-4 FIXED (the IsParameterUsed probe replicated over the shipped corpus — 5 OED-dump drift rows corrected on the descriptor table, `MATERIAL_FLAG_GLOW` minted, `is_glow_capable` classification); `renderer::uv_anim` ported (T1 section 4); the FlushBatches pass loop witnessed (per-light passrules multiplication, submit-0x10/+841/MATCHTERRAIN questions closed); ptl §5.2 stage-stride + §5.5 VS/PS errata; 17+4 IDB renames; T2: composite IDENTICAL, swatch key-set delta fully T1-hash-attributed, world set re-captured (post-ren4 = the rolling baseline) |
| REN-3 draw order | 885d8702 | the batching/draw-order system witnessed end to end (four queues + the unsigned-ascending sort `[orig: RenderBatch_QuickSort @ 0x5d8b40]`; the opaque state-sort key + the transparent `~float_bits` back-to-front key; the water-plane queue split + frame bracket `[orig: Terrain_RenderSceneWithReflection @ 0x5c93a0]`; technique-class selection; the render-state stack; the viewmodel near-Z 0.05 + viewport-depth [0,0.1] pass; the Q3 glow/envmap copy flushed by the bloom pass) — [render/render-order-re.md](render/render-order-re.md), D-RORD-1..6. Ordering semantics ported to `libs/renderer/render_order` and applied as the generalized Godot priority ladder (celestial rungs re-derived, water rung, object-model water-side rungs via the new `NovaObjectShaderCache` seam); T1 extended with the draw-order vector section (cited re-dump, pure append); T2 gained the depth-adversarial composite ordering scenes (fresh baseline; swatch A/B bitwise-identical vs post-ren2); 11 function + 15 data renames landed in the IDB (EffectWorld particle pass ex-`CNapiSession_*`, the render-state stack push ex-`CNetPlayer_*`, the viewmodel viewport/near-Z pair ex-`Scar_*`); draw order leaves UNAUDITED (2→1); D-RORD-2/-6 ratified into the permanent register; env #30 gained its pass-structure rider; world §13's `0x10000000` identified as the repeat-draw marker |
| REN-2 materials grill + converge | 005efae0 | the runtime material path witnessed end to end (HLSLEffect registry built from `_FFP.fx` ×24 + the localres `.fx` set + `#UV` twins; probe-derived capability flags; six technique-class pass blocks; state application at FlushBatches) — [render/render-material-re.md](render/render-material-re.md), D-RMAT-1..6. Fixed in-slice with cited T1 re-dumps: the alpha-test compare shape (invert flips the COMPARE `[orig: @ 0x6770a0]`), case-insensitive tag lookup `[orig: @ 0x5ade70]`, the VS_TRACER registry row; flag-enum misnomers renamed everywhere (BLENDING/SKINNED/TANGENT/UVGEN); `MATERIAL_FLAG_*` single-sourced onto `NovaObjectShaderCache` (the ENG-4 leg); 3 kong misnomers renamed in the IDB; materials leave UNAUDITED (3→2); ratchet `libs_uncited_src_files` tightened 65 → **64** (the composer earned its citation) |
| REN-1 instrument (rode this trunk, d158e3e3) + REN-0 mint (ce2b0978) | d158e3e3 / ce2b0978 | the three-tier parity instrument + pre-change baselines; the track mint + [ADR 0023](adr/0023-render-visual-parity.md); ledger audit track reopened (UNAUDITED = 3); env #17 → REN-5 and #27/#29/#30/#33 → REN-6 transfers; `ledger_check.py` DOMAIN_ORDER + the LIBS-2 `libs/renderer`-fold reversal |
| ENG-2 env port (PR #206 train) | 28cc9474 … (7 slices) | the five environment GDScript files are scene plumbing over `libs/env`: weather core (slices 1-2), sky dome + cloud scroll (3-4), water surface (5-6, full witnessed render: per-frame noise textures + UV + lit-color pipeline), celestial placement + glare occlusion (7). Re-grills caught the shared RNG-seed transcription (env #25, fixed in libs/env + libs/wac), the scroll model (#26), the water look/precedence (#28/#31), celestial placement inventions (#32), and CLOSED env #14; minted #27/#29/#30/#33 as tracked deferrals. Honored-matrix re-attested; vectors 204/204 with per-slice witnessed re-dumps |
| Wave-2 boundary: STD-3 flip + GOV-4 sync | (this slice) | CI lint step now `--enforce` ×3 (ratchet, dict-contract, link-graph) + `fetch-depth: 0` so the diff lint stops self-skipping on PRs; dict-contract lint repaired to class-level declarations only (its Wave-1 soft run flagged 36 function-locals — a lint bug, not code debt; the 4 surviving range hits are pre-existing contracts moved by F5); ledger scoreboard now GENERATED (`scripts/lint/ledger_check.py`, hard-fail like the ratchet) — mechanical recount corrected the hand-kept total 64 → **66 open** (Net rows were undercounted 21→23, Credits 2→1, Tiles 1→0, Fonts 3→1); ratchet baseline tightened `libs_uncited_src_files` 87 → **65** (earned by the PAR-train citations); D-FOLIAGE-1's raw `\|` escaped (it broke GFM rendering + parsers) |
| GOV-4 Wave-1 close-out | 50fce423 (rode the trunk) | ratchets +0 all train (`test_private_pokes` 1371, `libs_uncited_src_files` 88 at close — see the boundary slice's recount); ledger synced continuously (the trunk-era hand count said 66 → 62; the mechanical recount at the boundary says 66 — the drift is why the scoreboard is now generated); stale-doc touches rode each slice (vfs record, menu-re, world-wac-ai-re, env records) |
| NET-4 C-ABI guard | ae221e7d + 729f01c7 + 17f06457 | `abi_export_identity` ctest (104-export baseline, forbidden net families); its macOS leg immediately caught + fixed the dylib leak (hidden visibility + 38 phantom MISSION_EXPORTs stripped) — the flat C ABI is now provably identical on every platform. (2026-07-29 audit note: #344 removed the macOS legs from regular CI and release.yml tests ubuntu+windows only, so the macOS enforcement this row credits is currently LAPSED - no workflow compiles or tests macOS between release tags) |
| PAR burn-down (trunk train) | 8778a0b4 … 2ed9a4fb | eight rows closed: D-EVT-2/-4 + cats 5/6 + D-EVT-5 mint, D-PLAYERINFO-2, D-NET-20, env #21, env #19 (tint consumers; dead-bake correction), D-VFS-2 (fixed boot table + the editor-index decision), D-CTRL-2 (witnessed visibility flags), D-INF-4 (witnessed direction-table generator); PAR-R7 VFS/PFF audit landed (D-VFS-1..9) |
| ENG-6 R8 boot-resource research | 1139190e | docs/required-resources.md (fatal set, ordered boot sequence, D-BOOT catalog); the Wave-2 manifest leg stays open |
| ONED F5 mission_controller decomposition | 9c4a1ea3 | 4,033 → 1,646-line composer + seven `_ops` sections; weakref `_c` leak fix (65e9e33d); the master-red `nova_mission_data_test` literal repaired (7055d0d5) |
| ONED F2 EngineTextPreview + F3 See-in-game | ba467137 + a68e271f | the game-seam text preview widget; the `/d` loose-override launcher with typed LaunchPlan |
| ADR-0016 packaging repair | 0836edc2 | AvatarPreview moved to the shared engine layer — the runtime package excluded `modtools/*`, master's boot smoke was red since #194 |
| ONED F4 writer-parity convention | 29b5ded8 | the four-part W gate recorded in the track doc; libs/CLAUDE.md points at it; binds hudpos (HUD-1) and avatars (AVT-1) forward |
| PAR-0 ledger train | 4948c540 | divergence-ledger.md + ADR 0022 + dashboard wiring; stable D-catalogs minted in the four prose-only records (97f64a97) |
| PAR: D-ITEMDEF-1 closed | faec4b3e | `item_type_from_string` witnessed mapping [orig: ItemDef_ParseProperty @ 0x49eb00]; itemdef-re verdict → MATCHING; first ledger row to zero |
| NET-3 reclassifications + doc sweep | d47932dc | `.agents/network.md` → npruntime ROADMAP redirect, nw_server README (dev/golden-harness), repo-map one-liners; stale-doc sweep 40504652; no-internal-back-compat convention 98f924af |
| ENG-1 env parity vectors | 27cff422 | 137 vectors dumped once from the cited GDScript port (`env_parity_vectors_test.gd`, env-gated dump mode); the ENG-2 port's pre/post harness |
| ONED F1 WorldContextPreview | 9c666c7a | in-world context service extracted from TerrainEditor with seams intact; consumers OBJ-1/SND-2 |
| ONED-A avatars merge train | 18791098 | the twelfth workspace (Avatars) merged from `playerinfo-runtime`; avatars ADR renumbered 0013→0021; eleven→twelve sweep; ratchet `test_private_pokes` baseline 1319→1371 — maintainer-approved bump for the branch's two pre-ratchet white-box files (`player_info_menu_seam_test.gd` 26, `avatar_preview_test.gd` 26); ONED-TST claws it back |
| LIBS-1 world→terrain seam | bc8c920a | libs/terrain_query query leaf + the permanent forbidden-edge check (link_graph_check.py); ADR 0020 |
| NET-2 npwire extraction | 46cd0ac4 | wire+replay+framing legs → libs/npwire; ADR 0019; NET-0/STD-1 rode Wave 0 |
| GOV-1/2/3 bootstrap docs | 4274cfdf | umbrella + vocabulary + ADRs 0015–0018 |

## Tracks

Track codes prefix slice IDs and PR titles. Sizes are S/M/L feel, not time.

### GOV — governance, vocabulary, program docs

- **GOV-1** (M) this umbrella; docs/README.md index rows.
- **GOV-2** (S) CONTEXT.md vocabulary batch 1: Serve mode; In-match vs
  Matchmaking; wire codec / net runtime / net seam; Product (+ Title
  forward-note); Promote disambiguation; Required resources. (Avatars/
  twelve-workspace entries ride ONED-A.)
- **GOV-3** (M) the four settled-decision ADRs (0015–0018).
- **GOV-4** (S per wave) wave close-outs: ratchet-counter review, stale-doc
  sweep, status table update. (.agents/network.md rewrite rides NET-3.)

### NET — in-match net boundary (owns the wire-compat invariant)

Background finding: the in-match *runtime* already lives outside the
NovaWorld lib (`libs/netsim` seam glue, `libs/npruntime` 62 Hz runtime), but
the in-match *codec* (wire leg: `ingame_decode/encode`,
`ingame_message_catalog.h`, `replication_model.h`) and the replay leg sat
under `libs/novaworld` — a matchmaking name for game-protocol code — until
NET-2 moved them to `libs/npwire` (ADR 0019).
`.agents/network.md` is stale (names classes deleted by the npruntime
rebuild). `apps/nw_server` vs `apps/novaworld_server` is the deliberate
ADR-0013 matchmaking/in-match split, not duplication — the `nw_*` naming is
what confuses.

- **NET-0** (M) **golden-gate hardening, before anything moves.** Two tiers
  (retail captures are never committed — docs/asset-gated-tests.md):
  - *Tier 1, default CI, cannot skip:* codec identity vectors — a synthetic
    message corpus encoded/decoded through the catalog with committed
    byte/hash goldens — plus an opennova↔opennova loopback self-capture
    fixture driven by an `nw_golden_diff` self mode. Wired into ci.yml's
    Linux net job (whose explicit test list is a known sync hazard: update
    it in the same commit).
  - *Tier 2, local, mandatory protocol:* the retail pcap golden diff
    (`NW_GOLDEN_OURS` + `.scratch/golden/`) and the npruntime golden joins,
    run locally for every net-touching PR and attested in its description.
- **NET-1** (S) quiesce — **done 2026-07-04**, dispositions (maintainer calls):
  - `worktree-net-final` — **dropped after bundle**: tip `b28cbe8f` verified
    identical in the 2026-07-04 archive bundle and on the still-live
    `origin/worktree-net-final`; local branch deleted. Its five Apr-26/27
    commits (RE/nethook capture tooling, codec_validation.py, novaworld_shim
    spawn/movement) are superseded by wire_capture, nw_replay, and npruntime.
  - `worktree-game-server` — **kept live** (rebase-after): branch content fully
    merged; the worktree carries the active v34 vehicle-drive/EWeap WIP
    (uncommitted, IDA-cited); fast-forward onto master when that work lands.
  - shared nw-merge worktree + `web-nw-for-real-master` — **retired**: branch
    fully merged and deleted (remote already pruned); worktree removed. Its
    untracked `.scratch` (retail_join_v2–v15, `ov-*`, host logs) was destroyed
    with the removal; the three NW goldens survive in the main checkout's
    `.scratch/golden/` and the v16–v35 series in game-server's `.scratch`
    (see docs/asset-gated-tests.md), and the lost captures are re-derivable
    from the `start_v*_capture.ps1` recipes.
- **NET-2** (L) extract the wire + replay legs out of `libs/novaworld` into
  **`libs/npwire`** (final name settled by its ADR): ingame_decode/encode,
  ingame_message_catalog.h, replication_model.h, peer_addr.h,
  replay_timeline, serverlog_decode, wire_capture, and the shared session
  framing (protocol_message, nw_session_framing, session_hello,
  session_keys). Dependency direction becomes `novaworld → npwire →
  napi/novacrypto`: matchmaking sits ON the wire base, and sqlite/gate are
  structurally outside the game link path. Pure `git mv` + include/CMake/
  ci.yml rewrites, zero behavior edits, one revertable train, ADR accepted
  in the same PR.
- **NET-3** (S) reclassifications: `apps/nw_server` README (dev/golden-
  harness owner; optional rename is the maintainer's pick);
  `.agents/network.md` rewritten as a redirect to `libs/npruntime/ROADMAP.md`
  + ADR 0013; the promote disambiguation lands (renaming
  `mission::promote_mission` is optional — if picked, full propagation per
  the rename-everywhere rule).
- **NET-4** (S, rides NET-2's ADR) net libs are formally OUTSIDE the C ABI
  (C++-linked only) — **guard landed 2026-07-05**: the `abi_export_identity`
  ctest (`scripts/lint/abi_exports_check.py`, 104-export committed baseline,
  never-bypassable forbidden-family check) runs wherever `BUILD_SHARED_LIB=ON`
  builds run ctest (scripts/build.sh + the CI build-and-test job).

### LIBS — topology and seams

Background finding: 44 libs is fine — one-lib-per-format is the tracked
rule. The real issues are one heavy edge and two families.

- **LIBS-1** (M/L) the **world→terrain seam** — DONE (ADR 0020):
  `libs/terrain_query` (height_field + coords, zero deps) is the query
  leaf `libs/world` links; `libs/terrain` sits on it; `wac`/`mission`/net
  closures dropped the terrain-format stack (cpt/til/trn/tpj/foliage);
  `scripts/lint/link_graph_check.py` (transitive-closure forbidden-edge
  check, soft mode in CI) makes the cut permanent. Ran AFTER NET-2 so
  link topology churned once.
- **LIBS-2** (S+M) — **DONE (Wave-2 trunk 2, [ADR 0024](adr/0024-lib-family-topology.md))** —
  family topology ADR + execution: one-lib-per-format affirmed; the terrain
  and audio families are CMake link-interface groups
  (`opennova_terrain_family`, `opennova_audio_family`; `libs/families.cmake`
  included by both roots, adopted by the GDExtension) rather than physical
  merges. The original "fold `libs/renderer`" clause (4 files that existed
  to dodge one oed header) is **reversed** (maintainer 2026-07-05,
  [ADR 0023](adr/0023-render-visual-parity.md)): REN grew `libs/renderer`
  into the witnessed render library instead — recorded in ADR 0024.
- **LIBS-3** (S) — **DONE (Wave-2 trunk 2)** — the two consumption models
  documented in libs/CLAUDE.md and named in ADR 0024: Model A = the flat C
  ABI (`opennova_shared`, importer/Python/DCC, `abi_export_identity`-pinned),
  Model B = C++ static link (engine, apps, tests, net). Naming them makes
  the npwire ADR's "net stays out of the C ABI" clause structural.

### ENG — engine portability, boundary APIs, required resources

Background finding (the seed list — ADR 0016 is the rule): a GDScript
scalar height sampler duplicating the C++ one; a hand-rolled terrain
ray-march under mission picking; duplicated sector/atlas constants; FNT
format facts + a shelf packer in `fnt_rasterizer.gd`; `MATERIAL_FLAG_*`/
`OED_UPDATE_*` duplicated in `nova_object_model.gd`; and
`godot/engine/environment/*.gd` — ~1,167 lines of `[orig]`-cited
TOD/celestial/fog/weather math that belongs in `libs/env`.

- **ENG-1** (M) env parity harness: fixture `.env` files × a time/state
  grid; expected vectors dumped ONCE from the current GDScript (itself the
  cited port) and committed with an explicit tolerance policy. A vector
  divergence during the port triggers a re-grill against the binary — never
  tolerance widening.
- **ENG-2** (L) — **DONE 2026-07-06 (PR #206)** — the env port: the five environment GDScript files →
  `libs/env`; Godot nodes become thin hosts; citations move and are
  re-verified; docs/env/env-honored-matrix.md updated; vectors green
  pre/post; the GDScript math is deleted.
- **ENG-3** (M) terrain query APIs + editor adoption, one slice per bypass:
  unify scalar/batch height on the C++ sampler (the parity test flips from
  pinning drift to pinning the single path); engine-side terrain raycast
  (the GDScript ray-march + slab test deleted; mission picking gated by the
  mission suites + a manual picking pass); sector/atlas constants and
  transforms exposed once and consumed everywhere.
- **ENG-4** (S/M) — **DONE (Wave-2 trunk 2)** — FNT packing port (shelf
  packer + format facts into `libs/fnt`; the Godot TextServer rasterization
  stays shell-side) and object-flag single-sourcing — the `MATERIAL_FLAG_*`
  leg was **subsumed by REN-2**; `OED_UPDATE_*` closed here (GDScript
  aliases over the bound `NovaObjectData.UPDATE_*` + test contract pins).
  The DCC-side `THREEDI_IR_MATERIAL_FLAG_*` ctypes mirror is FFI-inherent
  (every FFI struct mirrors its C header; the roundtrip pytest suite is its
  guard), not an eliminable duplication — recorded here so ENG-5 sweeps
  don't re-flag it.
- **ENG-5** (S per wave) **the generalized bypass sweep** — a standing
  audit instrument, run at every wave boundary: per-domain review of
  modtools/engine GDScript for math and constants that exist in `libs/`,
  plus grep heuristics (known engine constants, GDScript math adjacent to
  `Nova*` calls). Findings land on the conformance checklist below; each
  closes or carries a tracked exception. The exploration's list is the
  seed, not the boundary.
- **ENG-6** (M) **the required-resources manifest**: an engine-research
  session (R8) enumerates the boot-required, hardcoded-by-name resource set
  from the binary (menumus/gamemus banks, the game strings table, the
  main.mnu set, hudpos.def, default world files, items/weapon defs,
  controls, ...). **R8 landed 2026-07-05**:
  [docs/required-resources.md](required-resources.md) (+ engine-primer
  cross-ref; D-BOOT catalog minted, D-BOOT-1 ledgered). **The Wave-2 leg is
  DONE (Wave-2 trunk 2)**: the engine-side manifest table lives in
  `libs/gameprofile/required_resources.{h,c}` (Model B only), the game's
  boot validation consumes it (`NovaResourceRoot.list_missing_boot_resources`
  + honest errors in `main_game.gd`), and ONED's diagnostics/new-game
  scaffold conveniences over the same table are ONED-REQ (Wave 3). This
  defines "what a person starts with to make a new game"; the Game
  workspace itself stays out of scope.

#### Boundary conformance checklist (ENG-5 instrument; seeded 2026-07-04)

Sweep #2 ran 2026-07-12 over the post-#210..#230 fidelity-train GDScript
(HUD/weapon, collision/armory, sound, particles) plus regression checks on the
closed domains. Verdict: the boundary holds at the sim/FSM/def-parse level
(weapon FSM, view state, collision, def tables, particle sim are libs-side
with typed-record decodes at the edges); three new shell-side math clusters
landed with those trains (HUD helpers, sound curves, the mission TOD clock)
and are tracked below with their recommended slicing.

| Item | Where | Status |
|---|---|---|
| Scalar GDScript height sampler duplicating C++ | terrain editor mesh | **closed (ENG-3, 2026-07-07)** — `EditorTerrainMesh.sample_world_height` forwards to the new `NovaTerrainData.sample_height_world_live` (the batch sampler's per-point core, shared so the two can never disagree); the GDScript bilinear deleted; `terrain_height_revision_test` flipped from pinning batch/scalar drift to pinning the single path |
| Hand-rolled terrain ray-march + slab test | terrain editor → mission picking | **closed (ENG-3, 2026-07-07)** — the witnessed raycast ported to `libs/terrain_query/terrain_raycast` (`[orig: @ 0x60cb80; @ 0x60e710]`, ENG-3 B0 grill → B1 port), bound as `NovaTerrainData.raycast_terrain` over both substrates; the GDScript march/slab/bisection trio deleted; the celestial glare stand-in retired onto the same binding; editor-guard residuals = D-TERRAIN-4 |
| Sector/atlas constants + coord math duplicated | terrain editor mesh | **closed (ENG-3, 2026-07-07)** — `COORDS_ATLAS_SIZE`/`COORDS_SECTOR_ID_MAX` minted beside the existing pair in `terrain/coords.h`; all four bound as `NovaTerrainData` class constants and consumed by the editor scripts (the `ATLAS_SIZE`/`SECTOR_SIZE`/`PATCH_VERTS`/`HM_SIZE` re-declarations are now aliases; `row*16+col` strides + 0..4 clamps on the constants); `world_to_sector_cell` bound and forwarded like the sibling transforms; contract pinned by `terrain_coords_test` |
| FNT format facts + shelf packer in editor | fonts rasterizer | **closed (ENG-4, Wave-2 trunk 2)** — the deterministic shelf packer ported to `libs/fnt` (`fnt_pack_shelf`, `FNT_PACK_PAD` minted; ctest hand-walks the reference layout); `NovaFntResource` binds it plus the six format class constants; `fnt_rasterizer.gd` keeps only the TextServer rasterization + page blits, its constants now aliases |
| MATERIAL_FLAG_* / OED_UPDATE_* duplicated | engine object model GDScript | **closed (flags: REN-2, confirmed at REN-7; OED_UPDATE_*: ENG-4, Wave-2 trunk 2)** — MATERIAL_FLAG_* single-sourced onto NovaObjectShaderCache; OED_UPDATE_* GDScript re-declarations became aliases of the already-bound `NovaObjectData.UPDATE_*` (engine model + object workspace), with the test suite keeping deliberate literal pins + a binding-drift test (the ENG-3 contract-pin pattern) |
| Env/TOD/celestial/weather math in GDScript | godot/engine/environment | **closed (ENG-2, 2026-07-06)** — nodes are plumbing over libs/env; the tracked stand-ins are divergence rows (env #29 strip tessellation, #33 star instancing, #27 smoothed scalars) |
| Menu absolute-rect math in canvas | mnu canvas | **closed as reviewed exception (ENG-5 sweep #2, 2026-07-12)** — the canvas's absolute-rect summation is edit-model gesture math over the document tree (picking, ghost drags, snap tuning in board units); the witnessed scale model stays in the hosted live engine node (`[orig: CUIScene_SetScreenScale @ 0x639480]`); no engine constant or witnessed math is duplicated |
| HUD view-helper math cluster (exact-integer fade decay, 1024×768 design scale, 16.16 crosshair spread + TAPER strip, Q16 stance scaling, health thresholds, message tick policy, half-bright text, ammo format, stance→ERROR-row remap, capacity-1 reserve fold — all `[orig]`-cited) | `godot/engine/ui/hud_*.gd`, `game_hud.gd`, `game_hud_presenter.gd` | open (sweep #2, 2026-07-12) — **tracked: one `libs/hud` port slice** (ENG-4/FNT pattern: math + constants native, `draw_*`/Font blits stay host); no libs home exists today — `libs/def` deliberately keeps ALPHAFADE raw |
| Sound distance/TOD-crossfade curves (`calc_distance_volume` Q16 chain `[orig: @ 0x75ca20]`, emitter falloff arms `[orig: @ 0x528667]`, oneshot curve `[orig: @ 0x75cf14]`, `time_of_day_region` cuts + blend `[orig: @ 0x408110]`, crossfade byte, marker stagger) | `libs/audio` `ambient_mixer.cpp` (curves + emitter arms + TOD region + crossfade byte; the GDScript seams delegate; oneshot curve composes from the shared `calc_distance_volume`) | **DONE 2026-07-28** (the D-SND-16 cadence port carried the curves slice; `ambient_mixer` ctest pins the integers; Godot voice/bus writes stay host) |
| Mission TOD clock (`Env_TodAdvancePerTick = 0x18000000/(3720·minutes)` `[orig: @ 0x57d108]`, Q8.8→8.24 widening, 60-min clamp) | `nova_environment.gd` | open (sweep #2) — **tracked: `libs/env` clock slice**; a post-ENG-2 arrival with the game-runtime train, zero presence in libs/env |
| Camera/view composition remainder (eye height 1.0 dual-declared with `nova_simulation.cpp`, TP distance/orbit `[orig: @ 0x4391d0]`, eye re-aim `[orig: @ 0x437d10]`, weapon.def /256 view-offset + axis map `[orig: @ 0x4dd380]`; the ADS bias lerp shadows the production-callerless `player_view_bias_units`) | `player_viewmodel_rig.gd` | open (sweep #2) — **tracked (S): fold into `libs/world` player_view**; the ADS-lerp, the anim-key-substring stance probe (sim owns `net_stance_bits`), and the 4×-re-declared 62.5 Hz tick constant are close-now candidates riding this or any adjacent slice |
| Editor-preview PLAYPARTANIM phase integrator coexisting with `AiSystem::advance_part_anim` | `nova_object_model.gd` | open (sweep #2) — **tracked**: route the preview through the engine integrator (the height-sampler dual-implementation pattern); runtime already uses `set_part_phase` correctly |
| Armory derivation math (class resolve scan + masks `[orig: @ 0x5642f0]`, loadout weight Σ + encumbrance bands `[orig: @ 0x565490; @ 0x565640]`) | `armory_menu_companion.gd` | weight half CLOSED 2026-07-30: `def_loadout_weight`/`def_encumbrance_class` are bound on `NovaWeaponDatabase` (`loadout_weight`/`encumbrance_class`) and consumed by BOTH `player_info_menu_companion.gd` and `armory_menu_companion.gd`; residual = the class-resolve scan half |
| Particle flag literals (`1<<18/27/28`) re-declared + flags→kill-plane dispatch | `effect_world.gd`, `particle_preview.gd` | open (sweep #2) — **close-now candidate**: alias off the already-bound `NovaParticleDef` flag table (the MATERIAL_FLAG_*/OED_UPDATE_* pattern exactly) |
| Avatar menu-portrait presentation math (BAM/frame idle, 2^28 sway, rand-yaw `[orig: @ 0x55dba0; @ 0x5600d0]`) | `avatar_preview.gd` | open (sweep #2) — minor, exception-leaning (witnessed menu-frontend presentation over Godot camera); disposition with the HUD slice's review |

### STD — records, constants, testability standards + enforcement

Rules are ADRs 0017/0018; this track is the tooling and the seeded
conversions.

- **STD-1** (M) enforcement tooling, soft mode from Wave 0:
  `scripts/lint/maturity_lint.py` (diff-scoped pattern checks) +
  `scripts/lint/ratchet_counts.py` + a committed baseline JSON, wired as one
  small step into existing CI jobs. See the enforcement table below.
- **STD-2** (M aggregate) seeded record conversions (Wave 2, after the
  pattern survives a wave of real use): document-tab rows, tile gizmo
  state, reference services (dedups its two copies), reference-index edge
  dicts, focus payloads, mission param schema rows, MCP tool defs/args,
  object material defs. Local dicts convert adopt-on-touch only. Templates:
  LinkPayload, WorkspaceDef/InspectorDef.
- **STD-3** (S) hard-fail flip at the Wave-2 boundary for lints that ran a
  wave without false positives. Ratchets are hard from day one (they are
  noise-free by construction). **Executed at the boundary (2026-07-05)**:
  all three checks now run `--enforce` in CI, with two repairs the flip
  surfaced — the dict-contract lint matched indented function-locals
  (fixed: class-level column-0 declarations only, per its own spec), and
  the CI shallow clone made the diff lint self-skip on every PR (fixed:
  `fetch-depth: 0`). The ledger scoreboard check joined the gate set
  (generated block, ratchet-class noise-free).

### ONED — editor maturity

The detail doc is [docs/oned/workspace-maturity-program.md](oned/workspace-maturity-program.md)
(PR #184, rewritten in place as this track). Its R/W/E/G bar, foundation
phases F1–F5, per-workspace phases, and RE ledger stand; this program adds:

- **ONED-A** (M) the avatars merge train — DONE (18791098): the twelfth
  workspace (Avatars) merged from `playerinfo-runtime` across the #180 shell
  decomposition and Wave 0 — editor + engine `nova_avatar_database` +
  fixtures + five test files + RE doc; the branch's ADR renumbered
  0013 → 0021 (it collided with master's consolidated-net-core 0013) with a
  repo-wide reference sweep; the eleven→twelve sweep done (CONTEXT.md,
  modtools README table, screenshot driver, the track doc's matrix row).
- **ONED-F** = the track doc's F1–F5 (F5, the 4,033-line mission_controller
  decomposition, before any MIS phase).
- **ONED-W1/W2/W3** = the track doc's waves (W2's RE-gated bring-ups are
  the first phases the freeze releases).
- **ONED-TST** (M/L) public-seam test refits for the private-poking top
  offenders (ADR 0018's categories; the canary handled with care; identical
  assert counts prove refactor-only; the long tail rides the ratchet).
- **ONED-RSP** (M/L) responsiveness: policy ADR (scale model, ONE window
  floor — today there are two conflicting ones, ratio-based split
  persistence with a versioned migration, `custom_minimum_size` audit) →
  implementation → exactly one visual re-baseline slice.
- **ONED-MUS-D → ONED-MUS-I** (M then L) the music redesign: a bounded
  design spike with signed constraints (document/VM layer untouched; its 46
  tests stay green unmodified; shell left-lane conformance; no modal
  map/section canvas swap; honest navigation over the flat state machine;
  Live/Bank resolution; responsive-first), a maintainer gate, then
  implementation in shippable slices.
- **ONED-REF** (M) reference-extractor gaps: weapon.def, Avatars.def
  (post-A), .wac; .trn/.sbf/.lwf/strings as sources — via the documented
  refs.cpp plug-in point.
- **ONED-REQ** (S/M) required-resources conveniences over the ENG-6
  manifest: missing-required diagnostics; the "new game" scaffold seed.

### PROD — products and serve mode

- **PROD-1** (M) serve mode per ADR 0015: `opennova.exe --server`
  (+ `--headless`): windowed serve jumps the menu shell to the
  server-options `.mnu`; headless serve drives the existing host-session
  bring-up (ADR 0013 helper, 62 Hz pump). No new seam, no new protocol.
  The packaging boot smoke gains a `--headless --server` leg.
- **PROD-2** (S) taxonomy conformance: export-presets audit (exactly two
  products), validate_release_deliverables expectations, the
  title-identity fragmentation note recorded as future work.
- **PROD-3** (S/M) program-end release dry run: package both exes, all
  smokes, tier-2 retail-join attestation on the runtime build.

### PAR — parity burn-down (divergence ledger)

Target: **zero OPEN divergences** — every tracked divergence ported-and-closed or
ratified permanent. The living dashboard is
[docs/divergence-ledger.md](divergence-ledger.md); the policy (zero-OPEN target,
canonical vocabulary, the freeze exemption, and the permanent register) is
[ADR 0022](adr/0022-divergence-burn-down.md). Freeze-exempt (maintainer 2026-07-05);
env closures land **libs/env-first** so ENG-2 does not pay twice.

- **PAR-0** (M) this train: the ledger + ADR 0022 + this wiring; the prose-only records
  (3di-gp, 3di-lw, ptl, mis) gain stable `D-` catalogs.
- **PAR-NET** (L) the open D-NET set (the ledger's Net table) + populate
  `nw_golden_diff` `kDeferredGaps` with the D-NET refs from the attested 21-gap baseline,
  so the golden diff names each deferral by ID.
- **PAR-ENV** (M) env #15/#16/#18 implemented **libs/env-first** (#14 closed
  at ENG-2's celestial leg; #19/#21 closed 2026-07-05; **#17 transferred to
  REN-5** — the modulator chain is a render-lighting consumer — and the
  ENG-2-minted render rows #27/#29/#30/#33 fold into **REN-6**).
- **PAR-WORLD** (M) the D-INF opens and the D-EVT set (D-ITEMDEF-1 closed
  faec4b3e — the first ledger row to zero; the 2026-07-05 D-EVT grill + slice
  closed D-EVT-2/-4, cats 5/6 of D-EVT-3, and minted-closed D-EVT-5, leaving
  D-EVT-1 and the cat-1/2 matrix family witnessed-ready-deferred on the
  TriggerRelations / deploy-POI ports).
- **PAR-UI** (M) D-MNU-5/6, D-CTRL-3, D-PLAYERINFO-11, D-SND-2, and D-HUD after its RE
  port lands.
- **PAR-R1..R7** (S/M each; **R7 landed 2026-07-05** — [vfs/vfs-pff-mount-re.md](vfs/vfs-pff-mount-re.md), D-VFS-1..9) the UNAUDITED-system audits (engine-research /
  grill-ida): terrain, foliage, tiles, fonts, credits, importer pipeline, VFS/PFF mount
  stack — each lands an RE record **with a D-catalog**.
- **Class-B research starters** (freeze-exempt engine-research): the HUD radar/crosshair,
  D-NET-49 (in-match PG), the in-world avatar binding (D-PLAYERINFO-1), and the full `.mis`
  grammar grill (`dfx2med.exe`, D-MIS-1/-3).

### REN — render visual parity (materials, draw order, shaders, lighting)

Grill and reimplement the original renderer so our output looks identical to
retail. Standing rules are [ADR 0023](adr/0023-render-visual-parity.md): the
original fixed-function look is the target (no PBR reinterpretation); the D3D
device layer is the WITNESS SOURCE for state semantics and never a port target;
REN runs on the PAR model (grills exempt as research, ports are ledger
closures landed libs-first); instrument tolerances never widen — a divergence
triggers a re-grill.

Background finding (the 2026-07-05 planning grill): the runtime render path is
the one **reimplemented-but-unwitnessed** subsystem. The 45-entry shader-tag
table (`libs/oed/include/oed/material_descriptor.h`, static_assert-locked to
the raw `gMaterialInfoTable` dump) and its chain — `libs/renderer`
`classify_object_material()` → generated GLSL (`object_shader_template.cpp`)
→ `NovaObjectShaderCache` → `nova_object_model.gd` ShaderMaterials — carry
only ModSuperOed-side citations; no Jointops runtime render address is cited
in `libs/` outside `libs/env`, and no record covers the runtime material
path, batching/draw order, the runtime TSS tables, or lighting application.
Draw order is greenfield (the celestial priority ladder is the only ordering
machinery; world objects all render at priority 0). Three witnessed islands
are reused, never re-grilled: env's sky TSS tables + vs_1_1 sources +
dome→bodies→world order; ptl §5.2's `RenderState_ApplyToDevice @ 0x681920`
struct decode; world §13's org-callback 6-draw order + `0x10000000` pass
flag. Anchors were verified against the kong IDB at planning; two
brief-stage corrections already caught (`Terrain_InitShaders @ 0x5c19b0`,
`render_terrain_lightmaps @ 0x609de0` — mid-function addresses circulate)
plus three dead render variants — expect ENG-2-grade misnomer density and
verify every IDB name before citing it.

RE records land under `docs/render/` (re-doc format): `render-material-re.md`
(D-RMAT), `render-order-re.md` (D-RORD), `render-lighting-re.md` (D-RLIT);
terrain TSS findings grow `terrain/terrain-re.md` (its pending "runtime
render pass" item) and sky/water gaps grow `env/env-tod-re.md` in place.
One trunk PR, slice-per-commit, per-slice attestations in the description.

- **REN-0** (S) this mint: the track + ADR 0023 + the ledger audit-track
  reopen + `ledger_check.py` domain map + the LIBS-2 reversal + the env
  slice transfers. Behavior-neutral.
- **REN-1** (M) **the parity instrument + baselines, before any behavior
  change** (three tiers, NET-0's shape): *T1* render-state vectors — a ctest
  walks the material input matrix (shader tags × 3DI flag bytes ×
  emissive/glass × alpha refs) through the classification/composition chain
  (later: sort-key/pass class + lighting scalars) against committed goldens,
  dumped once from the current implementation, dump mode fails loudly, with
  a forced one-bit sensitivity proof; *T2* the swatch A/B probe
  (generalizing `env_visual_baseline_probe.gd`): a deterministic synthetic
  swatch grid + asset-gated world composites, baselines captured pre-change
  to `.scratch/golden/render/` (never committed), re-captured and attested
  per slice; *T3* the named retail side-by-side scene list (water horizon ×
  TOD grid, alpha-test foliage, glass/env-map, transparents composite, night
  lightmap terrain, first-person viewmodel) — the headline gate at REN-7.
- **REN-2** (L) **materials** (grill-ida — reimpl exists): decode the
  runtime flag/tag→state semantics (the `0x5d6a30` planning anchor resolved
  at REN-5 to the render-slot light updater, renamed
  `RenderSlot_UpdateEntityLight`;
  `apply_shader_parameters @ 0x58db80`, `HLSLEffect_InitFixedFunctionShaders
  @ 0x5af790`, `build_shader_pass_name @ 0x5bf5d0`) at the device boundary
  (`@ 0x681920` / `@ 0x681d00` / `@ 0x6817d0` — witness keys only); confirm
  Jointops' own material table against the OED-derived `kMaterialInfoTable`;
  converge classifier + composer; T1 pinned→witnessed with cited re-dumps;
  engine-side `MATERIAL_FLAG_*` single-sourcing (the ENG-4 leg). Lands
  `render-material-re.md` + D-RMAT; likely settles D-PTL-3/-4.
- **REN-3** (M/L) **draw order** — **DONE 2026-07-06** (engine-research —
  was greenfield): sort keys + pass structure witnessed from the batch family
  (`collect_render_batches_for_entity @ 0x5d94b0`,
  `collect_render_objects_for_batch @ 0x5d8f20`, `RenderBatch_QuickSort
  @ 0x5d8b40`, `CRenderBatchQueue_SortAndFlush @ 0x5dae40`,
  `Render_SubmitEntity @ 0x5dad80`) and the LIVE frame orchestrators
  (`Render_ProcessMainSceneFrame @ 0x5ca0f0`,
  `Terrain_RenderSceneWithReflection @ 0x5c93a0` — the 10-flush frame,
  `Player_RenderFirstPersonViewModel @ 0x4ded60`; `render_main_scene
  @ 0x5c1240` identified as the reflection/cinematic offscreen variant, and
  `Render_SubmitAlpha16 @ 0x83fde8` as a 16.16 submit-alpha global, not a
  queue); the ORDERING SEMANTICS ported as the cited pass/priority map in
  `libs/renderer/render_order`, applied as the global Godot priority ladder
  generalizing the celestial one — the queue itself is not reproduced.
  Landed [`render-order-re.md`](render/render-order-re.md) + D-RORD-1..6.
- **REN-4** (M/L) **shaders/TSS** — **DONE 2026-07-06** (engine-research +
  grill): the two named anchors resolved to their true subsystems —
  `Terrain_InitShaders @ 0x5c19b0` is the WATER surface shader/material set
  (renamed `Water_InitSurfaceShaders`; blend ONE+dst·SRCALPHA, alpha-test 32,
  NV variants, FF fallback tables — ported into `water.gdshader`, env #34
  minted-and-closed) and `create_water_shaders @ 0x5dfc30` is the EffectWorld
  PARTICLE water/distort trio; the ACTUAL terrain surface shading witnessed
  (`PolyTrn_InitTextures @ 0x60aaa0`, `compile_terrain_pixel_shaders
  @ 0x605260` — 8 embedded PS incl. the ps.1.4 3-way splat; tiers; the
  foliage-model shader set `@ 0x5ff630/0x5ff7a0/0x6007c0/0x601260`) growing
  `terrain-re.md` §Runtime surface shading; the engine render-mode word +
  state permutation cache fully decoded (`@ 0x680f00/0x680b00/0x681080/
  0x681d00` — blend/alpha/color mode tables + pass-flag bits); the sky
  pass-1 gradient TSS closed (mode 0x200 = flat diffuse — the C7 port was
  already faithful); the bogus `foliage.gdshader` anchors corrected
  (0x5BF064 = a death-screen overlay; the real set is the foliage VS/PS
  family); D-RMAT-2 (tracer `|dot(eye,normal)|²` fade → `OSCAP_VIEW_FADE`)
  and D-RMAT-4 (the capability probe replicated — 5 OED-dump drift rows
  corrected; `MATERIAL_FLAG_GLOW` minted) FIXED; the UV-anim path ported
  (`renderer::uv_anim` over the shared PANM wave table, T1 section 4); the
  FlushBatches pass-execution model witnessed (per-light pass
  multiplication, submit 0x10 = z-read-off, MATCHTERRAIN = terrain-tile
  texture bind, ctx+841 = the mirror-clip constants gate); ptl §5.2/§5.5
  errata corrected. The embedded-shader census is complete.
- **REN-5** (M/L) **lighting** — **DONE 2026-07-06**: the modulator chain
  witnessed end to end and PORTED LIVE (the ÷64 unpacks
  `@ 0x58db30/0x5aaef0`, the witnessed modulator2 → modulator → color-block
  tick order `@ 0x57ef97..`, the 62-tick iris chase `@ 0x57e512/0x57d940` —
  `libs/env::ModulatorChain` + `NovaWeatherCore`, **env #17 CLOSED**, env
  vectors re-dumped surgically); the world lighting block + per-entity
  uniforms decoded (writer `CTerrainRenderer_BuildLightingShaderConstants
  @ 0x5c8090` → store `@ 0x5d89e0`; reader `@ 0x5d98a0` — slots 225-230
  pinned, the hemisphere DELTA lights `@ 0x5d8cb0`, the "dual-LOD lerp"
  erratum corrected to the INTERIOR DAYLIGHT lerp, effectScale = the 3-ray
  sun-visibility factor `@ 0x5c6800`) and ported (`light_runtime`,
  **D-RMAT-5 CLOSED** — the composer emits the witnessed MODULATE2X model);
  the terrain/foliage **c0/c1 = sky/light blocks** closed
  (`@ 0x604420/0x604ee0/0x610c80`; `terrain_lighting.gdshaderinc` corrected;
  the planning anchors resolved: `sample_terrain_lightmap` = the tinted
  colormap sampler feeding D-FOLIAGE-1, `Terrain_LoadScorchTextures` = scorch textures,
  `render_terrain_lightmaps` = the sector-model lightmap-tile pass);
  point lights (modulator-scaled, {1,0,15/r²,1} = the OED math,
  owner/interior group culling, ≤4 D3D lights) witnessed;
  `Lighting_InitTextures` (the last embedded shader — the DOT3 dynamic
  light) and the cubemap sources witnessed (CubeEnvironment = live scene per
  128 frames; **CubeRotSpecular = the static sun-glint cube** — D-RORD-5's
  answer); the render-slot (character shadow) lighting witnessed as
  out-of-scope. Landed [`render-lighting-re.md`](render/render-lighting-re.md)
  + D-RLIT-1..6; T1 section 5; audit track **1 → 0**.
- **REN-6** (M) **the ENG-2 render leftovers**, libs/env-first. The witness
  leg (2026-07-06) closed all four unknowns in IDA (the #33 generator found —
  `Star_GenerateInstanceTable @ 0x5ac850`; the #27 mystery pair = the rain
  percent spring; the #29 strip decode corrected the "2..9 columns" reading
  to the adaptive row stride; the #30 offscreen pipeline internals incl. the
  ex-decompile-fail `Water_RenderReflectedWorldScene @ 0x5c8510`). The first
  port leg (same day) closed **#27 FIXED** (`env::EnvScalarChannels` — the
  witnessed scalar springs ticked by the weather core, smoothed values served
  through the env seam to every consumer; SunDim live end-to-end) and
  **#33 FIXED** (`env::generate_star_instances` + `NovaStarField` — the
  256-star camera-anchored twinkling billboard field). The tail: **#29** (the
  strip tessellation port over the pinned constants) and **#30** (the reimpl
  planar-reflection port over the witnessed RTT pipeline; the strip
  mirroring rides #29's builder). Sequenced after REN-3/REN-4.
- **REN-7** (S/M) close-out: T3 attested scene-by-scene; T1/T2 full re-run;
  ledger sync; the IDB-edits + dead-variant catalogs complete in the
  records; FULL GUT + full ctest; correspondence.md + README rows.

Out of REN's scope (mint rows, never port): device/driver plumbing
(swapchain, caps, device-reset, buffer management, the state/texture-format
permutation caches); FrameFX post-processing beyond acknowledging its frame
slots; shadows/scars/decals (the `Shadow_`/`Scar_` families — a future track
candidate); particle LOOK (PTL's domain — REN provides the state substrate);
2D/HUD/text/menu/loading draw; performance work beyond parity; runtime `.fx`
parsing (the descriptor table stays canonical).

## Out of scope (tracked here so nobody re-litigates silently)

DFX2 / any title split (cheap later; no new hardcoded title identity
meanwhile); title-identity consolidation (note-only); NovaWorld
service/backend/web/launcher feature work; the music document/VM layer;
a netsim/npruntime merger (ADR 0013 already consolidated); physical family
merges unless LIBS-2's ADR chooses one; GOALS.md's Game workspace and
export-a-game (this program builds their base); expanding the C ABI to net
libs; the D3D device layer as a port target (REN witnesses it, never ports
it — ADR 0023); new reimplementation outside ONED-W2's gated items and the
PAR/REN ledger-closure exemptions (the freeze — IDA research continues
freely).

## Waves

| Wave | Contents | Exit gate |
|---|---|---|
| **0 — bootstrap** (M) | GOV-1/2/3, STD-1 soft, NET-0 | umbrella merged; tier-1 goldens green on all CI legs; lints reporting (not failing) |
| **1 — boundary moves + ONED foundations** (L) | NET-1 → NET-2 → NET-3; LIBS-1 (after NET-2); ENG-1; ENG-6 research start; ONED-A then ONED-F | npwire landed goldens-green + tier-2 attested; seam landed with the link-graph check permanent; env vectors committed; twelve workspaces merged; F1–F5 done (FULL GUT at F5) |
| **2 — portability + standards adoption** (L, widest; WIP cap: two trains) | ENG-2/3/4, ENG-5 sweep #1, ENG-6 manifest; LIBS-2/3; STD-2, STD-3 flip; ONED-W1, ONED-TST, ONED-RSP, ONED-MUS-D (gate at end); REN-0..2 (the REN trunk opens) | env GDScript deleted, vectors green; conformance checklist closed-or-tracked; ratchets hard and trending down; responsiveness landed + one re-baseline; music design accepted; REN instrument landed + materials converged (or the trunk merged at its last green slice — stop-anywhere) — **the program closed here 2026-07-12**: gate accounting + the RSP/MUS-D-acceptance dispositions in the Close-out section |
| **3 — feature-bearing maturity** (L) | ONED-W2 (freeze releases here), ONED-MUS-I, ONED-REF, ONED-REQ; PROD-1/2; ENG-5 sweep #2; REN-3..7 | **not executed as a wave** — REN-3..7 and ENG-5 sweep #2 completed early (in Wave 2); the rest is dispositioned at the close-out (the freeze lift releases ONED-W2 outright) |
| **4 — equalize + close** (M) | ONED-W3; PROD-3; GOV-4 close-out; ENG-5 final sweep | **not executed as a wave** — GOV-4 ran at the 2026-07-12 close-out; the freeze lifted there; ONED-W3/PROD-3 dispositioned |

Cross-track ordering: NET-0 before NET-2 (protection precedes the move);
NET-1 before NET-2 (quiesce precedes the move); NET-2 before LIBS-1 (one
link-topology churn) and before PROD-1 (stable homes); ENG-1 before ENG-2;
GOV-3 + STD-1 before all Wave-1+ slices (adopt-on-touch needs minted
rules); ONED-A first in the ONED train (drift control); F5 before MIS
phases; ONED-RSP and ONED-MUS-D before ONED-MUS-I; R-items before their
ONED-W2 phases (UI never ahead of the witness); REN-1 before any REN port
slice (baselines precede the first behavior change); REN-3/REN-4 before
REN-6 (the leftovers consume the frame/shader decodes); REN-2 before
ENG-4's remainder (the flag leg is subsumed).

## Gates

Standing, every wave: one slice = one green commit; FULL GUT + full ctest at
wave boundaries; the GUT silent-drop greps on every class_name/path move;
dumpbin export-identity on any C-ABI touch; packaging boot smokes; the
canary test (`terrain_editor_workstation_test.gd`).

| Phase | Gate |
|---|---|
| NET-0 | tier-1 vectors/self-capture in default ctest + Linux CI job; a forced one-byte codec change flips them red (sensitivity proof) |
| NET-2 | tier-1 green; `nw_message_coverage` green; full ctest; tier-2 retail diff + npruntime goldens attested; ci.yml list synced in-commit |
| LIBS-1/2 | full ctest; net/wac/mission ctests; link-graph assertion permanent |
| ENG-2 | env vectors pre/post; env-honored-matrix review; visual pass; FULL GUT |
| ENG-3 | flipped parity test; mission suites; canary; FULL GUT |
| STD-2 | per-contract GUT suites; FULL GUT keystones on class_name moves |
| ONED-A | the branch's five avatar test files; FULL GUT; 12-shot screenshot driver; boot probe |
| ONED-TST | refitted suites green at identical assert counts; ratchet decreases |
| ONED-RSP | persistence-migration test; FULL GUT; one visual re-baseline |
| ONED-MUS-I | the 46 music doc/VM tests unmodified-green; new screen tests; FULL GUT; visual pass |
| PROD-1 | boot smokes incl. `--headless --server`; tier-1 green; tier-2 retail-join attestation |
| PROD-3 | validate-deliverables; all smokes |
| REN (per slice) | T1 state vectors green (changed rows carry cited re-dumps); T2 swatch A/B attested in the PR; focused ctest (`-R "renderer\|material\|env\|terrain"`) + GUT keystones; ledger synced in-commit |
| REN-7 (train end) | FULL GUT + full ctest; T3 retail scene list attested scene-by-scene; REN + folded env rows closed-or-ratified; the conformance checklist's flag row closed |

## Enforcement (STD-1 design)

Principles: diff-scoped for new-code bans (zero noise on untouched code);
repo-wide counts only as ratchets against a committed baseline; hard-fail
only for cheap unambiguous checks; every hard-fail has a maintainer escape
hatch (a baseline bump, logged in this doc).

| Check | Mechanism | Start | End state |
|---|---|---|---|
| Codec identity + self-capture goldens | default ctest | Wave 0 | hard-fail forever |
| Message-catalog coverage | existing CI gate | exists | unchanged |
| Tests poking privates | ratchet (non-self `._name` count in godot/tests), fail-on-increase | Wave 1 | hard from day one; top offenders driven down by ONED-TST |
| New Dictionary contracts | diff-scoped lint over modtools/engine signatures, allowlist for transport edges | Wave 0 soft | hard-fail at Wave-2 boundary |
| [orig] citation coverage | ratchet: libs/*/src files with zero citations (infra libs allowlisted), fail-on-increase | Wave 1 | hard on increase; semantic coverage stays a review concern |
| Magic numbers | diff-scoped advisory in the CI summary | Wave 2 | advisory permanently |
| Link-graph edges | forbidden-edge script (npwire !→ sqlite; wac/mission/net !→ terrain-format libs post-seam) | Wave 1 | hard-fail forever |
| Ledger scoreboard sync | `scripts/lint/ledger_check.py` — the count-to-zero block is generated from the per-domain tables (`--write`), CI checks the equality | Wave 2 | hard-fail forever (ratchet-class: a mismatch is never a false positive, `--write` IS the fix) |
| GUT silent-drop greps | existing | exists | unchanged |
| C-ABI export identity + net-family ban | `abi_export_identity` ctest (`abi_exports_check.py` vs the committed baseline; a baseline bump is same-commit and logged here; the net-family check is never bypassable) | Wave 1 (NET-4) | hard-fail forever |

Home: `scripts/lint/` + baseline JSON; one small step in existing CI jobs
(no new workflow).

**Logged C-ABI baseline bumps** (the same-commit escape hatch):

- 2026-07-05, 104 → 106: added `def_loadout_weight` + `def_encumbrance_class`
  (the PLAYER_INFO loadout-weight math ported to `libs/def`, D-PLAYERINFO-11
  weight readout). Additive, `[orig: calculate_loadout_weight @ 0x55f1f0;
  @ 0x55f480]`; no existing export changed semantics.

## Risks

1. **Wire-leg move breaks retail parity invisibly** (the retail gate is
   env-gated and skip-passes-as-green) → NET-0 lands first; NET-2 is a
   pure move; tier-2 attested; one revertable train.
2. **Freeze friction with in-flight net worktrees** → NET-1 quiesce with
   recorded dispositions; old→new path map published in the NET-2 PR body.
3. **Env port fidelity drift** (float math, GDScript doubles vs C++
   floats) → vectors before port; divergence triggers a re-grill, never a
   tolerance bump; vectors stay as permanent regression tests.
4. **Avatars ADR collision + branch drift** → merge early; renumber with a
   repo-wide reference sweep; the twelve-workspace sweep is a checklist.
5. **Music redesign scope creep** → bounded spike, signed constraints,
   maintainer gate before implementation, shippable slices throughout.
6. **Responsiveness churn** (raw-px persistence, 21 fixed-size sites, the
   canary) → policy ADR first; versioned persistence migration; floors
   unified before scale work; exactly one visual re-baseline slice.
7. **Enforcement noise poisons legitimacy** → diff-scoped bans, committed
   baselines, a soft wave before any hard-fail, logged escape hatch.
8. **Program sprawl vs review bandwidth** → hard wave boundaries; WIP cap
   of two concurrent PR trains; this doc is the only dashboard; ONED is
   the only track with a detail doc.
9. **Renderer expressibility** (TSS combiner corner cases, fog
   interactions, reverse-Z — env #20 is the precedent) → the T1 vector
   layer separates semantic parity from reimpl mapping; inexpressible states
   become tracked class-C rows via ADR 0022's register, never silent
   tolerance bumps; CI hard-gates on T1 only (visual tiers are local
   attestations, immune to GPU nondeterminism).
