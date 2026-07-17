# TODO

## General Mission

- [ ] Waypoint types needs to be an enum and then not a number in the UI
- [ ] Zones are awkward to create
- [ ] AI class and AI script: should be a selection, not a free input, if we can pull the options from a loadable resource (def, etc)
- [ ] "Weapon loadout" field semantics need an IDA grill (the editor panel itself is in)
- [ ] "Raw" group fields need an IDA grill and better editor integration
- [ ] Too much useless text noise in Mission tab
- [ ] Briefing needs to be a larger textbox. Also confirm whether it can be a string (rtxt) and integrate nicely if so
- [ ] "Music track" being a number is no good. Better integration
- [ ] Still no sound?

## BMS Scripting

- [ ] "PlayWavList" needs deeper editor integration
- [ ] Validate "reset after" / "pre mission" / "post mission" is implemented properly
- [ ] "ShowWaypoints" needs to be validated
- [ ] If there is no "sub-type" for an action (ie: only Null), just disable the box
- [ ] MisvarChange/Set etc need better editor integration

## Editor-wide

- [ ] The editor needs "depth": these files reference each other, and those references should be links

## Cleanup & verification backlog

- [ ] `npruntime_golden_gameplay` FAILS when actually run against the golden capture
      (found 2026-07-04 by re-arming the asset-gated tests; it skip-passed before):
      the test pins the pre-round-14 `0x2B idle default` at record off-14
      (`tests/npruntime/golden_gameplay_test.cpp:230`) while the server now recomputes
      anim state from replicated input (D-NET-159). Decide test-stale vs regression
      against the wire witness and re-pin. All other gated tests pass with data
      (117/117 corpus, JO sweeps, LAN-join goldens, golden client).

- [ ] `opennova_python_pytest` FAILS 15 tests on any machine with a working `bpy`
      (diagnosed 2026-07-05 at the Wave-2 boundary; CI stays green because
      `importorskip("bpy")` skips there — same skip-as-green trap as the gated tests).
      Two roots, both in the DCC parity tests (`tests/test_anim_dcc_parity.py`,
      `test_ase_dcc_parity.py`, landed #59): (1) they build an animations-only request
      (`write_blend/ase/3dp=False`) that `validate_import_request` rejects
      ("Select at least one file to write." — `writes_any_output_file()` predates
      animation export and does not count it; `_request_for_blender` in
      `opennova_blender/backend.py` likewise returns None for it); (2) their in-process
      `import bpy` poisons the pytest parent, and the spawn-context worker pool inherits
      the corrupted `sys.path`, so the 13 `test_importer_integration` tests die with
      `No module named '_bpy'` — all 13 pass in isolation. `bpy_session.py`'s docstring
      forbids exactly this parent-process import. Fix shape: decide whether
      animations-only is a valid import request (count animation exports in
      `writes_any_output_file()` + give the blender leg the work) or fix the tests to
      request a scene output — and either way run the parity exports in a subprocess.

- [ ] Terrain native `[orig]` citation pass: sweep `godot/engine/terrain/` + `libs/terrain` for the remaining uncited chains — `docs/terrain/terrain-re.md` now exists (partial, PAR-R1) and `libs/terrain` carries inline citations after the 2026-07 re-grills (#245); narrow or close this entry after the sweep
- [ ] Present-pass / entity-reconcile citation pass: `engine/world/mission_present_pass.gd`, `mission_entity_registry.gd`, `wire_present_pass.gd` document design but carry no `[orig]` anchors; engine-research the original present/tick chain and cite into `docs/runtime-architecture.md` + `docs/correspondence.md`
- [ ] Two-net-stack convergence: the NovaNetClient replay/spectate path vs the NovaWorldClient/NovaSimulation listen-server path (see godot/engine/CLAUDE.md); decide convergence once the net workstream stabilizes
- [ ] `engine/mcp/` relocation (optional): all 11 files are class_name-referenced with zero `res://engine/mcp` literals, so it can move (e.g. next to `modtools/mcp/`) without path edits if engine/ layering ever needs it
- [ ] `opennova::io` adoption continuation: migrate remaining per-lib byte readers on-touch (policy in libs/CLAUDE.md); excluded: mus/wac VM cursors (faithful-port surface)
- [ ] `mission_controller.gd` full decomposition (beyond what the inspector split needed): extract selection/gizmo/placement concerns
- [ ] Mission workspace rail conversion: with the inspector decomposed into section components, moving Mission onto `_build_inspector_defs()` workflow rows is a small step, but it swaps the in-panel mode tabs for the shell's workflow rail (visible layout change) - needs a deliberate UX pass

## Player info (player.mnu / PLAYER_INFO)

The avatar lists, cascade, team filter, name, 3D preview, and ACCEPT seam are wired
(godot/game/player_info_menu_host.gd; grilled in docs/playerinfo/avatars-re.md
D-PLAYERINFO-7..12). Remaining:

- [ ] Loadout combos (PRIMARY/SECONDARY/ACCESSORY + *_AMMO* + STATIC_TOTAL_WEIGHT).
      A separate subsystem (D-PLAYERINFO-11): the runtime weapon table is built by
      `WeaponDef_LoadAll @ 0x54dd10` (weapon.def -> `g_weaponDefTable` dword_2540CE0,
      0xBF40 B) and consumed by `populate_weapon_slot_lists @ 0x560430` filtered by the
      class mask (`g_playerInfoClassMask`) + team mask (`g_playerInfoTeamMask`),
      slot-routed by entry+68; ammo via `populate_weapon_accessory_ammo_ui @ 0x55e8b0`,
      weight via `update_player_info_weight_and_weapon_icons @ 0x55f480`. `libs/def`
      `DefWeaponDef` does NOT yet capture slot/class/weight/team-mask, so this needs:
      (1) grill `WeaponDef_ParseProperty` for the weapon.def tokens that feed those
      fields, (2) extend `libs/def` (weapon.def + ammo.def) to parse them, (3) a
      `NovaWeaponDatabase`/`NovaAmmoDatabase` binding, (4) wire the 13 loadout combos +
      the weight budget label in the companion.
- [ ] Persist the chosen avatar/name to a player profile + render the chosen combo on
      the spawned soldier (D-PLAYERINFO-1, combo -> spawned-player model still open).
