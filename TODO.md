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

- [ ] Terrain native `[orig]` citation pass: sweep the remaining uncited chains —
      `libs/terrain` carries 22 anchors across 15 files and `godot/engine/terrain/` 18
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
- [ ] Wire-walk edge-gating follow-up: the native wire walk deliberately
      transliterates the GDScript dispatch pattern, so the aim-payload,
      ctrl-publish, and weapon-channel legs still dispatch every frame while
      active; adopting the mission walk's change-gated edge machine for them is
      a measured follow-up (F3 Stats wire leg before/after)
- [ ] Env/water/sky/weather singleton `_process` set (~1 ms at the ASH_I5A
      vantage): the July perf program's next present-side slice now that the
      MP wire walk is native
- [ ] Main-loop order grill: `docs/runtime-architecture.md` cites the exact main-loop /
      entity-render order from existing RE notes; a focused grill-ida pass to pin
      `WacScript_AdvanceTick`'s surroundings + the original entity-render function would
      witness it directly
- [ ] Incremental terrain-build tile load: `libs/terrain/src/build_quadtree.cpp` skips the
      witnessed cached-tile load leg (`process_quadtree_leaf` / `sub_402730`) and always
      regenerates from base meshes — correct on a clean build, but retail treats existing
      tile files as a cache. Decide: ledger it as a D-TERRAIN row (cache-trust is the
      witnessed semantic) or record it in `terrain-re.md` as a deliberate tool-side
      divergence
- [ ] Two-net-stack convergence: the NovaNetClient replay/spectate path vs the NovaWorldClient/NovaSimulation listen-server path (see godot/engine/CLAUDE.md); decide convergence once the net workstream stabilizes
- [ ] `engine/mcp/` relocation (optional): all 16 files are class_name-referenced with zero `res://engine/mcp` literals (#376 added the `game_mcp_*` service trio and `mcp_peer_client.gd`), so it can move (e.g. next to `modtools/mcp/`) without path edits if engine/ layering ever needs it
- [ ] `opennova::io` adoption continuation: migrate remaining per-lib byte readers on-touch (policy in libs/CLAUDE.md); excluded: mus/wac VM cursors (faithful-port surface)
- [ ] Mission workspace rail conversion: with the inspector decomposed into section components, moving Mission onto `_build_inspector_defs()` workflow rows is a small step, but it swaps the in-panel mode tabs for the shell's workflow rail (visible layout change) - needs a deliberate UX pass
- [ ] NovaWorld disconnect-state reset (owner: `godot/game/novaworld_panel.gd`): clear all connection-derived rows, login/join state, pending mission/player data, and disable Host/Join/Login on disconnect or error. Acceptance (`godot/tests/novaworld_panel_test.gd`): a populated, logged-in, pending-join panel returns to a clean disconnected state and cannot submit a stale row. Coordinate with the unlanded novaworld_panel rework on the `worktree-gsb` branch before landing.
- [ ] Export-progress overlay initiator awareness (owner: `godot/modtools/editor/shell/export_progress_overlay.gd`): the overlay polls the ACTIVE workspace, so an export running on a non-active workspace — reachable now that the flavor confirm exports the initiator — shows no progress UI. Route the overlay through the exporting workspace instead.
- [ ] Converge `libs/cpt`'s bit codec on `io/bit_stream.h` (owner: `libs/cpt/src/cpt_io.cpp`): the two have diverged (cpt's writer carries a normalizing `set_position` and a `write_to_file`; its reader now carries `remaining_bits`), so this is a real migration, not a swap — the reason it is tracked separately in `libs/CLAUDE.md`. Acceptance: `parametric_parity_test` still reports byte-identical CPT output for all four fixtures after cpt drops its private copy.

## Quality campaign — remaining slices

The 2026-07 quality campaign (approved 2026-07-28) runs as small independent PRs
straight to master. Waves 1–4 are DONE (#310–#375): Phase 0 relanded the stranded
#303 audit; W1 hygiene (dead code, print-zero + ratchets, the io/log.h sink,
canonical constants); W2 duplication collapse (strutil, framers + PeerAddr, io
primitives, gd helpers, tooling, pcapio, W2-7 hardening; W2-3 retired as refuted);
W3 split the six god files into one-concern TUs (mission, collision, 3DI3, GP,
ai, def), fixed the citation ratchet (#349), and closed with the W3-4..W3-7
slices (#352–#356); W4 was the GDScript wave (#357–#375). Remaining: W5 only,
plus the no-magic remainder:

- [ ] W5 consolidations + push-downs: perf-span unify, sim debug-snapshot
      narrowing (debug half only), ShellServices/DocumentKind/undo consolidation,
      the avatar/object preview de-fork + shared MCP arg helper, push-downs
      (ItemSeatSpecs first — that PR also lands the standing "new engine logic
      starts in libs/" CLAUDE.md rule — then camera and armory/catalog) —
      opportunistic
- [ ] No-magic remainder (C11): named constants for the OED rattrib/pattrib magic
      values — #365 landed the net-message-id and witnessed-flag-bit halves; the
      OED half is blocked on a ModSuperOed IDB witness

## Standalone runtime / debug stack follow-ups (post-#376)

ADR 0025 made the standalone game ONED's only live mission runtime. These are the
open review follow-ups from #376/#378 — none blocking:

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

- [ ] Release-gate parity: make tag releases run the same required quality gates as PR/master CI, or reject release tags whose commit is not on `master`. Acceptance: an off-master tag cannot publish, and a valid release commit passes the shared maturity, native, Python, and Godot gates.
- [ ] Full Linux core tests: add an Ubuntu leg for the complete native/Python suite after triaging any platform-only failures. Acceptance: the full CTest and Python suites run on Linux for every PR without relying on the net-only or packaging jobs.
- [ ] macOS CI coverage: #344 removed every macOS job from regular CI (the test-matrix leg, `build-gdextension-macos`, the godot-tests macOS leg, and the macOS package jobs); `macos-latest` now appears only in `release.yml` tag-time packaging, so macOS builds are first exercised at release time. Decide: restore a macOS leg (full or smoke) to PR/master CI, or record the lapse as accepted and note the release-time-only risk.
- [ ] Incremental conventional linting: establish project-owned editor/format settings, then add per-language lint checks in advisory or changed-file mode before enforcing them. Acceptance: CI checks new changes without requiring a repository-wide reformat, with documented local commands for each enabled linter.
- [ ] Two ctests are `DISABLED TRUE` in `tests/CMakeLists.txt` with reasons recorded but no owner: `parametric_parity` (long byte-identical CPT fixture run, disabled pending CI stability/perf cost) and `particle_smoke_all_fixtures` (waiting on the full 77-file corpus being mirrored into `fixtures/particle/`). Acceptance: each is either re-enabled or converted into an env-gated test alongside the rest of the asset-gated set (`docs/asset-gated-tests.md`).

## Player info (player.mnu / PLAYER_INFO)

The avatar lists, cascade, team filter, name, 3D preview, ACCEPT seam, and the three
loadout weapon lists (PRIMARY/SECONDARY/ACCESSORY) are wired
(godot/game/player_info_menu_companion.gd; grilled in docs/playerinfo/avatars-re.md
D-PLAYERINFO-7..12). Remaining:

- [ ] Persist the avatar/class/loadout selections to the on-disk profile
      (D-PLAYERINFO-9 — the NAME half is done: `NovaPlayerProfile.save_callsign` writes
      `user://player_profile.cfg`; the ammo/type picks now ride `snapshot()` as
      `*_clips`/`*_ammo_type`) + render the chosen combo on the spawned soldier
      (D-PLAYERINFO-1, still NEEDS-RE).
