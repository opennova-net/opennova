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

- [ ] `env_render_unit` STILL fails "color pixels landmarks (t=0)" on the
      `macos-26-arm64` runner image (2026-07-10, PR #219 round 2) — with the water
      sine LUT already a committed constant (env #35) and the field checksum
      passing, i.e. every input byte identical and the downstream chain
      all-integer. `macos-latest` currently serves a MIXED fleet (round 1 drew
      macos-15-arm64 and passed the same code), so red/green is a per-run coin
      flip. Deprioritized by the maintainer 2026-07-10 ("Mac tests are not
      important to me right now"). Next lead when picked up: reproduce on a
      macos-26 runner with the intermediate buffers dumped (field -> animated ->
      kernel -> pixel) to see which integer stage diverges; suspect a new-Xcode
      codegen issue or an unnoticed UB in the kernel fold.

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

- [ ] Terrain native `[orig]` citation pass: `godot/engine/terrain/` + `libs/terrain` carry no inline citations; grill-ida the mesh build / sampler / lighting chain and land a `docs/terrain/*-re.md` record (docs/README.md lists terrain as record-less)
- [ ] Present-pass / entity-reconcile citation pass: `engine/world/mission_present_pass.gd`, `mission_entity_registry.gd`, `wire_present_pass.gd` document design but carry no `[orig]` anchors; engine-research the original present/tick chain and cite into `docs/runtime-architecture.md` + `docs/correspondence.md`
- [ ] Two-net-stack convergence: the NovaNetClient replay/spectate path vs the NovaWorldClient/NovaSimulation listen-server path (see godot/engine/CLAUDE.md); decide convergence once the net workstream stabilizes
- [ ] `engine/mcp/` relocation (optional): all 11 files are class_name-referenced with zero `res://engine/mcp` literals, so it can move (e.g. next to `modtools/mcp/`) without path edits if engine/ layering ever needs it
- [ ] `opennova::io` adoption continuation: migrate remaining per-lib byte readers on-touch (policy in libs/CLAUDE.md); excluded: mus/wac VM cursors (faithful-port surface)
- [ ] `mission_controller.gd` full decomposition (beyond what the inspector split needed): extract selection/gizmo/placement concerns
- [ ] Mission workspace rail conversion: with the inspector decomposed into section components, moving Mission onto `_build_inspector_defs()` workflow rows is a small step, but it swaps the in-panel mode tabs for the shell's workflow rail (visible layout change) - needs a deliberate UX pass

## Particles (runtime effect world)

The .ptl stack (libs/particle + engine wrappers + the ONED Particles workspace) and the
runtime effect world (NovaEffectWorld: mission-start load of every mounted .ptl, name/handle
intern, WAC fx2ssn spawns) landed on the 2026-07-10 particles train. Witnessed follow-ups
(addresses + detail in docs/particles/ptl-format-re.md §8):

- [ ] Weapon-action effects (muzzle flash): `ActionSlot_SpawnEffect @ 0x401f20` spawns the
      ACTION block's `particle` handle at the action-bone transform
      (`Entity_ComputeActionTransform @ 0x401310`) — the natural tail of the weapon FSM
      train. The wider spawn-site sweep (projectiles/explosions, vehicle dust, bone trails,
      death effects, `Weapon_RaycastAndSpawnImpact @ 0x4e8460` impacts, weather) rides after.
- [ ] fx2tgt routing: pin which `.bms` type-6088 record field carries the 1..99 target
      number (`WacScript_SpawnEffectAtTargetMarker @ 0x4f7fd0`), then route it like fx2ssn.
- [ ] D-PTL-7: scripted spawns should orient to the terrain surface normal at the entity's
      grid cell (both WAC handlers); host passes up-vector today.
- [ ] D-PTL-8: `stockeffect` clone fallback for unknown interned names
      (`CEffectWorld_InternEffectHandle @ 0x5f7310`).

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

## Foliage rebuild (fix-219, started 2026-07-11)

Maintainer verdict: delete the current foliage implementation, re-study the
original from scratch, reimplement. Perf triage (tests/perf_probe.gd, REVVY
ASB_G11A): the MODEL tier burns 15.8 ms/frame of a 17.8 ms world tick (89%),
171k cumulative tile regenerations — churn instead of the witnessed bake-once
architecture. The rebuild is both the fidelity and the perf fix.

Phases:
- A. Fresh IDA verification sweep of the load-bearing claims (docs/foliage/
  foliage-re.md is the prior record; verify, don't trust): the FAR feed
  (traverse-collect 42.0 <=128/frame @0x60905c/@0x603e60), per-def slot pools
  bake-once + LRU (@0x601b30, generate @0x5ffdd0 seed 0xA55B1EED), the draw
  walk + fades (@0x60a171..0x60a694, 20..42, high pass <33, refs 180/8),
  tile-RT t0 = COLORMAP-ONLY + N.L alpha (@0x60dce5/@0x60e38a), the
  foliagemap match remap (@0x605AD0/@0x5FF4E0/@0x6066d0), the MODEL tier
  (@0x601f50 quadrant walk + 8-frame stagger + 1000-entry cache, >=38.0;
  dispatch @0x5c7d50; FOGENABLE-OFF black; dedup-to-last D-FOLIAGE-10).
- B. Delete + reimplement the HOST side to the witnessed retained model:
  godot/engine/terrain/nova_foliage_dispatcher.{h,cpp} rebuilt around
  bake-once slot pools (NO per-frame regeneration; LRU eviction by frame
  stamp; frustum-gated MODEL dispatch). libs/foliage placement.cpp (byte-exact
  0xA55B1EED placement) KEEPS — it is parity-tested; the dispatcher/emitter
  layers rebuild against it.
- C. Gates: perf_probe ASB_G11A foliage_us < 3000 (from 15849); the fp/keys
  probes' scenes visually re-checked (near brightness, far tint, no flicker,
  black far models); GUT foliage files + ctest foliage suite green; the
  RE record rewritten as the single source.

### Phase A findings (2026-07-11, fresh decompiles — both pool managers verified)

- FAR pool `foliage_lod_update_texture_slots @ 0x601b30`: ONE static VB/IB per
  pool; slots are FIXED ranges (36-B verts x this[15] per slot; 2-B indices x
  this[16]); per frame: clear active bytes, mark+stamp hits, collect NEW keys
  (deduped, <=64/frame), LRU-adopt each new key and bake ONCE via
  generate_foliage_instances_0 STRAIGHT INTO the slot's locked VB/IB range.
  Steady state = zero regeneration, zero uploads.
- MODEL tiles `Foliage_UpdateModelTiles @ 0x601f50`: per visible sector entity
  per def slot; view-Z >= 38.0 gate; 4 quadrant 16u tiles; 1000-entry per-def
  cache (stride 1368 B; [0] key, [1] stamp, [341] count). HIT: restamp;
  regenerate ONLY on the 8-frame stagger ((frameParity + 2*defSlot) & 7) == 0 —
  and the regen (Foliage_GenerateModelTileInstances @ 0x600980) is a CPU
  instance-LIST rebuild, NOT geometry: draws submit per-model static buffers
  (Foliage_DrawModelTileSlot @ 0x601d90). MISS: unrolled LRU evict, adopt,
  generate once. => The rebuild keeps the witnessed stagger but must make tile
  regen a cheap list write; NO per-frame mesh building or MultiMesh re-upload
  (our model_uploads=76k is the divergence, not the regen count).
