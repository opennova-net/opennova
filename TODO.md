# TODO

Everything here is work that is **not** a parity divergence: editor UX, code
hardening, and project health. Divergences from the original engine belong in
[docs/divergence-ledger.md](docs/divergence-ledger.md) instead, and
[docs/current-state.md](docs/current-state.md) explains which is which.

## General Mission

- [ ] Waypoint type is still a raw number in the UI: the trigger/action "Waypoint type"
      params (`mission_schema.cpp`) need an enum param kind — the values are already
      witnessed (`world/ai.h` `kWpType`: 1 nav-node, 3 literal coord); the waypoint-path
      fields are dropdowns already
- [ ] Zones are awkward to create
- [ ] AI class and AI script: should be a selection, not a free input, if we can pull the options from a loadable resource (def, etc)
- [ ] Group record semantics: the field WIDTHS are grilled (`bms.h`,
      `[orig: Med_WriteBmsFile @ 0x44f920]` — @0 a 2-bit flags, @8 the only free int,
      @12 the literal 10) and the editor clamps to that shape, but the two flag bits and
      the free int still lack semantic MEANING — grill what the engine reads them for
      and label the UI accordingly
- [ ] Too much useless text noise in Mission tab
- [ ] Briefing needs to be a larger textbox. Also confirm whether it can be a string (rtxt) and integrate nicely if so
- [ ] "Music track" being a number is no good. Better integration (the music workspace
      and `NovaMusicService` exist now to feed a named picker)

## BMS Scripting

- [ ] "PlayWavList" needs deeper editor integration: the dialog/wav id param is a raw
      number, and the runtime resolution chain through the co-named `.DBF` is in
      (`nova_mission_audio.gd`), so a dialog-name picker with preview is feasible now
- [ ] If there is no "sub-type" for an action (ie: only Null), just disable the box
- [ ] MisvarChange/Set etc need better editor integration

## Cleanup & verification backlog

- [ ] Retail-LAN parity 24-cell verdict (`PARITY_STATUS`): blocked on a cross-repo
      harness defect — the opennova-int MCP's single-role `onhook_host_lan`/
      `onhook_join_lan` pass role config via child env and never render `onhook.cfg`,
      so `generate_parity_manifest.ps1` rejects every RO/OO server cfg snapshot
      ("lacks one exact LanHostCallsign"). Fix belongs in opennova-int (make
      `LaunchLanRole` render the cfg like the `onhook_run_lan_pair` half); when it
      lands, run a full 24-cell suite under a NEW SuitePrefix to produce the verdict.
      Until then acceptance rests on the wire-ready witnesses + `diff_vs_golden.ps1`
      gates (both GREEN on the 2026-08-05 suites, 24/24 cells captured cleanly).
      Details: `.agents/retail-lan-parity.md` §9
- [ ] Terrain native `[orig]` citation pass: sweep the remaining uncited chains —
      `engine/runtime/terrain` carries 23 anchors across 15 files and `godot/engine/terrain/` 18
      across 8 of its ~16 source files (the cpt/til/trn resource-format files and the
      remaining builder files carry none); `docs/terrain/terrain-re.md` is still partial
      (PAR-R1); narrow or close this entry after the sweep
- [ ] Present-pass / entity-reconcile citation pass: the present anchors live at
      the native walks (`nova_present_applier.{h,cpp}` 12, the wire walk
      `nova_present_applier_wire.cpp` 5, the `wire_present_pass.gd` facade's
      cold-path 3, `present_held_weapon.gd`'s reference math) —
      but `engine/world/mission_entity_registry.gd` still carries none. Remaining:
      engine-research the original entity-reconcile chain and cite it into
      `docs/runtime-architecture.md` + `docs/correspondence.md`
- [ ] Env/water/sky/weather singleton `_process` set: re-measure at the ASH_I5A
      vantage before slicing — the #403 present-side rework (parked idle models,
      `env_generation_changed`, the staggered per-model light restamp) invalidated
      the earlier ~1 ms reading. The next perf attribution round starts from the F3
      "Outside shell spans" row (the #403 live sessions observed the remaining frame
      cost concentrated outside the model system after the park/submission gates)
- [ ] Main-loop order grill: `docs/runtime-architecture.md` cites the exact main-loop /
      entity-render order from existing RE notes; a focused grill-ida pass to pin
      `WacScript_AdvanceTick`'s surroundings + the original entity-render function would
      witness it directly
- [ ] Incremental terrain-build tile load: `engine/runtime/terrain/src/build_quadtree.cpp` skips the
      witnessed cached-tile load leg (`process_quadtree_leaf` / `sub_402730`) and always
      regenerates from base meshes — correct on a clean build, but retail treats existing
      tile files as a cache. Decide: ledger it as a D-TERRAIN row (cache-trust is the
      witnessed semantic) or record it in `terrain-re.md` as a deliberate tool-side
      divergence
- [ ] `engine/mcp/` relocation (optional): all 16 files are class_name-referenced with zero `res://engine/mcp` literals (#376 added the `game_mcp_*` service trio and `mcp_peer_client.gd`), so it can move (e.g. next to `modtools/mcp/`) without path edits if engine/ layering ever needs it
- [ ] `opennova::io` adoption continuation: migrate remaining per-lib byte readers on-touch (policy in engine/CLAUDE.md); excluded: mus/wac VM cursors (faithful-port surface)
- [ ] Mission workspace rail conversion: with the inspector decomposed into section components, moving Mission onto `_build_inspector_defs()` workflow rows is a small step, but it swaps the in-panel mode tabs for the shell's workflow rail (visible layout change) - needs a deliberate UX pass
- [ ] NovaWorld disconnect-state reset (owner: `godot/game/novaworld_panel.gd`): clear all connection-derived rows, login/join state, pending mission/player data, and disable Host/Join/Login on disconnect or error. Acceptance (`godot/tests/novaworld_panel_test.gd`): a populated, logged-in, pending-join panel returns to a clean disconnected state and cannot submit a stale row. Coordinate with the unlanded novaworld_panel rework held in the `gsb` worktree (WIP commit 0fd58850b on `worktree-gsb`; the branch's earlier commits landed via #300) before landing.
- [ ] Export-progress overlay initiator awareness (owner: `godot/modtools/editor/shell/export_progress_overlay.gd`): the overlay polls the ACTIVE workspace, so an export running on a non-active workspace — reachable now that the flavor confirm exports the initiator — shows no progress UI. Route the overlay through the exporting workspace instead.
- [ ] Converge `engine/formats/cpt`'s bit codec on `io/bit_stream.h` (owner: `engine/formats/cpt/src/cpt_io.cpp`): the two have diverged (cpt's writer carries a normalizing `set_position` and a `write_to_file`; its reader now carries `remaining_bits`), so this is a real migration, not a swap — the reason it is tracked separately in `engine/CLAUDE.md`. Acceptance: `parametric_parity_test` still reports byte-identical CPT output for all four fixtures after cpt drops its private copy.
- [ ] Typed shutdown-coordinator seams (owner: `godot/game/runtime_shutdown_coordinator.gd`): the coordinator still pokes the shell stringly (`_shell.call("_cleanup_picker")`, `_shell.call("_dismiss_loading_screen")`); replace with a typed shell contract (#403 review follow-up).
- [ ] Vehicle-drive slice start (retail-join-0a): the `game-server` worktree holds WIP commit a6bf98a30 on `worktree-game-server` — VehicleTraits `ground_family`/`is_eweap` groundwork (6 files; based pre-#403, snapshot-committed 2026-08-04). Reconcile onto current master when the local vehicle-drive slice runs (#403's `VehicleTraits` since gained the items.def-derived family tag + air/water params, so this is a rebase-and-rethink, not an apply).
- [ ] W5 consolidations + push-downs (the 2026-07 quality campaign's last wave;
      waves 1-4 landed as #310-#375): perf-span unify, sim debug-snapshot
      narrowing (debug half only), ShellServices/DocumentKind/undo consolidation,
      the avatar/object preview de-fork + shared MCP arg helper, push-downs
      (ItemSeatSpecs first — that PR also lands the standing "new engine logic
      starts in engine/" CLAUDE.md rule; the camera and armory push-downs are their
      own rows below) — opportunistic
- [ ] OED rattrib/pattrib no-magic (witness-first): named constants for the OED
      rattrib/pattrib magic values — #365 landed the net-message-id and
      witnessed-flag-bit halves; blocked on a ModSuperOed IDB witness
- [ ] HUD view-helper math cluster -> one `engine/hud` port slice (the ENG-4/FNT
      pattern: math + constants native, draw/Font blits stay host): exact-integer
      fade decay, 1024x768 design scale, 16.16 crosshair spread + TAPER strip,
      Q16 stance scaling, health thresholds, message tick policy, half-bright
      text, ammo format, stance->ERROR-row remap, capacity-1 reserve fold — all
      `[orig]`-cited in `godot/engine/ui/hud_*.gd` / `game_hud.gd` /
      `game_hud_presenter.gd`; no libs home exists today (ENG-5 sweep-#2 row,
      moved from the closed maturity program 2026-08-06)
- [ ] Mission TOD clock -> `engine/formats/env` clock slice: `Env_TodAdvancePerTick =
      0x18000000/(3720*minutes)` `[orig: @ 0x57d108]`, Q8.8->8.24 widening,
      60-min clamp — currently in `nova_environment.gd`, zero presence in
      engine/formats/env (ENG-5 row)
- [ ] Camera/view composition remainder -> fold into `engine/runtime/world` player_view:
      eye height 1.0 dual-declared with `nova_simulation.cpp`, TP distance/orbit
      `[orig: @ 0x4391d0]`, eye re-aim `[orig: @ 0x437d10]`, weapon.def /256
      view-offset + axis map `[orig: @ 0x4dd380]`; the ADS lerp, the
      anim-key-substring stance probe, and the 4x-re-declared 62.5 Hz tick
      constant are close-now candidates riding this (`player_viewmodel_rig.gd`;
      ENG-5 row; the W5 "camera" push-down is this row)
- [ ] Editor-preview PLAYPARTANIM phase integrator: route `nova_object_model.gd`'s
      preview through `AiSystem::advance_part_anim` (the height-sampler
      dual-implementation pattern); the runtime already uses `set_part_phase`
      correctly (ENG-5 row)
- [ ] Particle flag literals (`1<<18/27/28`) re-declared + flags->kill-plane
      dispatch in `effect_world.gd` / `particle_preview.gd` — close-now: alias
      off the already-bound `NovaParticleDef` flag table (the MATERIAL_FLAG_*
      pattern exactly) (ENG-5 row)
- [ ] Armory derivation math, class-resolve half: the scan + masks
      `[orig: @ 0x5642f0]` in `armory_menu_companion.gd` — the weight half
      closed 2026-07-30 via `NovaWeaponDatabase` (ENG-5 row; the W5
      "armory/catalog" push-down is this row)
- [ ] Avatar menu-portrait presentation math (BAM/frame idle, 2^28 sway,
      rand-yaw `[orig: @ 0x55dba0; @ 0x5600d0]`) in `avatar_preview.gd` —
      minor, exception-leaning; disposition with the `engine/hud` slice's review
      (ENG-5 row)
- [ ] Editor/runtime version-skew guard: the two CI zips (`opennova-modtools-windows`,
      `opennova-runtime-windows`) can pair a fixed editor with a stale `opennova.exe`
      and fail as an unexplained resource picker. The child already writes a `version`
      field into the run descriptor (`game_mcp_service.gd`) but the editor never
      validates it — and the skew failure happens pre-descriptor anyway. Verify a
      shared build id (descriptor check plus a boot-time check), or ship one combined
      zip.
- [ ] Cross-mission debug-intent replay: `NovaDebugSession._explicit_values` is never
      cleared, so `sync()` replays explicit debug writes into a NEW mission's fresh
      sim/terrain targets. Decide the cross-mission scope of debug intent: clear on
      mission unload, or document the replay as intended.
- [ ] F3 UX deltas vs the #342/#307 design: transport (pause/step) requires Edit
      unlock even in local SP where the player has host authority; Stop maps to
      return-to-menu rather than a pause-style stop; expensive views are physically
      reset when F3 closes. Review and either ratify the new behavior in
      `godot/engine/debug/pages/README.md` or change it.
- [ ] Managed-game shutdown: editor close/F8 always ends in `OS.kill`
      (TerminateProcess) because the graceful-quit path requires the debug identity
      that non-debug runs never get; and `user://oned-run-*.log` files accumulate
      unbounded. Give the managed child a graceful-quit path (or a bounded grace
      window) and prune or rotate the logs.

## Project health follow-ups

- [ ] NovaWorld production deploy + cutover (operator-executed, one-time): the
      stack (gate + NovaWorld server + legacy HTTP services + web portal) has
      never been deployed to production — steps in [DEPLOY.md](DEPLOY.md), the
      operator sequence in `plan/pr21-cutover-runbook.md` (its "#136 open"
      premise is historical; commands remain current)
- [ ] Release-gate parity: make tag releases run the same required quality gates as PR/master CI, or reject release tags whose commit is not on `master`. Acceptance: an off-master tag cannot publish, and a valid release commit passes the shared maturity, native, Python, and Godot gates.
- [ ] Full Linux core tests: add an Ubuntu leg for the complete native/Python suite after triaging any platform-only failures. Acceptance: the full CTest and Python suites run on Linux for every PR without relying on the net-only or packaging jobs.
- [ ] macOS CI coverage: #344 removed every macOS job from regular CI (the test-matrix leg, `build-gdextension-macos`, the godot-tests macOS leg, and the macOS package jobs); `macos-latest` now appears only in `release.yml` tag-time packaging, so macOS builds are first exercised at release time. Decide: restore a macOS leg (full or smoke) to PR/master CI, or record the lapse as accepted and note the release-time-only risk.
- [ ] Incremental conventional linting: establish project-owned editor/format settings, then add per-language lint checks in advisory or changed-file mode before enforcing them. Acceptance: CI checks new changes without requiring a repository-wide reformat, with documented local commands for each enabled linter.
- [ ] Two ctests are `DISABLED TRUE` in `tests/CMakeLists.txt` with reasons recorded but no owner: `parametric_parity` (long byte-identical CPT fixture run, disabled pending CI stability/perf cost) and `particle_smoke_all_fixtures` (waiting on the full 77-file corpus being mirrored into `fixtures/particle/`). Acceptance: each is either re-enabled or converted into an env-gated test alongside the rest of the asset-gated set (`docs/asset-gated-tests.md`).
- [ ] Product/engine boundary lint: PR #252's `scripts/lint/product_boundary_check.py` (mechanical teeth for the ADR 0015/0016 boundaries, 106 lines) closed unmerged 2026-07-17 and never landed in any form; the `heigene` worktree (branch `boundary-lint-pack`, head 149b88144) still holds it. Decide: reland a current-tree version, or record the drop as covered by `link_graph_check` (ADR 0020) + `host_lint`.

## Player info (player.mnu / PLAYER_INFO)

player.mnu is wired end to end (`player_info_menu_companion.gd`; grilled in
docs/playerinfo/avatars-re.md, D-PLAYERINFO-7..12). Remaining:

- [ ] Persist the avatar/class/loadout selections to the on-disk profile
      (D-PLAYERINFO-9 — the NAME half is done: `NovaPlayerProfile.save_callsign` writes
      `user://player_profile.cfg`; the ammo/type picks now ride `snapshot()` as
      `*_clips`/`*_ammo_type`) + render the chosen combo on the spawned soldier
      (D-PLAYERINFO-1, still NEEDS-RE).
