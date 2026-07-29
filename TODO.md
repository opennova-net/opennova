# TODO

Everything here is work that is **not** a parity divergence: editor UX, code
hardening, and project health. Divergences from the original engine belong in
[docs/divergence-ledger.md](docs/divergence-ledger.md) instead, and
[docs/current-state.md](docs/current-state.md) explains which is which.

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

- [x] `npruntime_golden_gameplay` — RESOLVED. The 2026-07-04 entry recorded a real failure
      against the golden capture: the test pinned the pre-round-14 `0x2B idle default` at
      record off-14 while the server had moved to recomputing anim state from replicated
      input (D-NET-159). #276 re-pinned it against the wire witness — it now asserts
      `anim_state_id == 0x2C`, retail's idle2 spawn default
      [orig: `Entity_ResetToSpawnState @0x4B9714`]. Verified 2026-07-22 by running the
      binary directly against `retail-gameplay-session.pcapng` (C2S 0x0C = 2351,
      S2C 0x0A = 2361), so it is passing on real data, not skip-passing. The test still
      prints its own DEFERRED note: full 0x0A body byte-parity vs the capture is not
      asserted, because our World holds host+peer only and not the capture's ASH_G3D
      entity set.

- [ ] Terrain native `[orig]` citation pass: sweep `godot/engine/terrain/` + `libs/terrain` for the remaining uncited chains — `docs/terrain/terrain-re.md` now exists (partial, PAR-R1) and `libs/terrain` carries inline citations after the 2026-07 re-grills (#245); narrow or close this entry after the sweep
- [ ] Present-pass / entity-reconcile citation pass: narrowed 2026-07-22 — the fidelity slices gave `mission_present_pass.gd` and `wire_present_pass.gd` `[orig]` anchors (4 and 3 respectively), but `engine/world/mission_entity_registry.gd` still carries none. Remaining: engine-research the original entity-reconcile chain and cite it into `docs/runtime-architecture.md` + `docs/correspondence.md`
- [ ] Two-net-stack convergence: the NovaNetClient replay/spectate path vs the NovaWorldClient/NovaSimulation listen-server path (see godot/engine/CLAUDE.md); decide convergence once the net workstream stabilizes
- [ ] `engine/mcp/` relocation (optional): all 12 files are class_name-referenced with zero `res://engine/mcp` literals, so it can move (e.g. next to `modtools/mcp/`) without path edits if engine/ layering ever needs it
- [ ] `opennova::io` adoption continuation: migrate remaining per-lib byte readers on-touch (policy in libs/CLAUDE.md); excluded: mus/wac VM cursors (faithful-port surface)
- [ ] `mission_controller.gd` full decomposition (beyond what the inspector split needed): extract selection/gizmo/placement concerns
- [ ] Mission workspace rail conversion: with the inspector decomposed into section components, moving Mission onto `_build_inspector_defs()` workflow rows is a small step, but it swaps the in-panel mode tabs for the shell's workflow rail (visible layout change) - needs a deliberate UX pass
- [ ] Failed replay-boot rollback (owner: `godot/game/main_game.gd`): route an immediate `load_net_session()` error through the same idempotent menu teardown as emitted load failures. Acceptance (`godot/tests/game/main_game_lifecycle_test.gd`): a replay boot rejected before connecting restores `State.MENU`, menu visibility, hidden world/HUD, and no kill feed.
- [ ] NovaWorld disconnect-state reset (owner: `godot/game/novaworld_panel.gd`): clear all connection-derived rows, login/join state, pending mission/player data, and disable Host/Join/Login on disconnect or error. Acceptance (`godot/tests/novaworld_panel_test.gd`): a populated, logged-in, pending-join panel returns to a clean disconnected state and cannot submit a stale row.
- [ ] Flavored-export initiator preservation (owner: `godot/modtools/editor/shell/save_export_flow.gd`): retain the workspace that opened the flavor dialog instead of resolving whichever workspace is active at confirmation. Acceptance (`godot/tests/terrain_editor_workstation_test.gd`): switching tabs while the dialog is open exports the initiating workspace exactly once and never the newly active one.
- [ ] Idempotent browser-pane wiring (owner: `godot/modtools/editor/shell/layout_persistence.gd`): guard the browser toggle connection just like the split signal. Acceptance (`godot/tests/terrain_editor_workstation_test.gd`): calling `wire_browser_pane()` twice raises no signal-connect error and one toggle produces one visibility/persistence update.
- [x] Defined bitstream dword access (owners: `libs/io/include/io/bit_stream.h`, `libs/cpt/src/cpt_io.cpp`): replace the unaligned `reinterpret_cast<uint32_t *>` stores with byte-safe little-endian loads/stores and converge CPT on the shared primitive. Acceptance (`tests/io/io_test.cpp`, `tests/cpt/cpt_roundtrip_test.cpp`): fields crossing 1-, 2-, and 3-byte offsets retain golden bytes and run clean under UBSan alignment checks. (Done 2026-07-28, quality campaign W2-7: both writers now read-modify-write through `io/le.h`. `io_test`'s `test_bit_stream_unaligned_golden` pins the golden bytes at 1-, 2- and 3-byte offsets, and the byte-identical CPT fixture parity run (`parametric_parity_test`, ~23 MB of CDEP+POLY bitstream across 4 fixtures) confirms the encoder is bit-for-bit unchanged. The CONVERGENCE half is deliberately NOT done — see the next entry.)
- [ ] Converge `libs/cpt`'s bit codec on `io/bit_stream.h` (owner: `libs/cpt/src/cpt_io.cpp`): the two have diverged (cpt's writer carries a normalizing `set_position` and a `write_to_file`; its reader now carries `remaining_bits`), so this is a real migration, not a swap — the reason it is tracked separately in `libs/CLAUDE.md`. Acceptance: `parametric_parity_test` still reports byte-identical CPT output for all four fixtures after cpt drops its private copy.
- [x] Bound malformed CPT declarations before allocation (owner: `libs/cpt/src/cpt_io.cpp`): validate CDEP block-count/width products and remaining bit budget before resizing decoded buffers. Acceptance (`tests/cpt/cpt_roundtrip_test.cpp`): a tiny CDEP declaring `0xffff * 0xffff` pixels and a truncated large-count POLY both return `false` with stable errors, without large allocation or ASan findings. (Done 2026-07-28, W2-7: the CDEP product is computed in 64 bits and bounded by the format's fixed 1024x1024 depth image, plus a minimum-20-bits-per-block budget against the section's remaining bits; POLY bounds its vertex count the same way and rejects `bit_width == 0`, which used to shift by -1. Pixels deliberately have no per-bit bound: `bits_per_delta` may legitimately be 0.)
- [x] Reject backward RTXT text ranges (owner: `libs/rtxt/src/rtxt.cpp`): require `text_data_end` to be at or after the entry table before treating it as section metadata. Acceptance (`tests/rtxt/roundtrip_test.cpp`): a one-entry file whose `text_data_end` points inside the entry table is rejected with a stable parse error. (Done 2026-07-28, W2-7.)
- [x] Harden replay numeric input and playback I/O (owner: `apps/nw_replay/nw_replay.cpp`): reject non-finite or partially parsed numeric arguments and propagate capture-read / UDP-send failures instead of reporting playback complete. Acceptance (new `tests/novaworld/nw_replay_cli_test.cpp`): `--speed nan`, `inf`, junk, and overflow fail parsing, while injected stream/read and send failures produce a non-zero playback result. (Done 2026-07-28, W2-7: the CLI and the paced playback core moved into `apps/nw_replay/nw_replay_cli.{h,cpp}` — `parse_args` reports through an out-param instead of printing, and `run_playback` takes its capture reader, its send and its clock as parameters, so the test injects the failures directly.)

## Quality campaign — remaining slices

The 2026-07 quality campaign (approved 2026-07-28) runs as small independent PRs
straight to master. Waves 1–3 are DONE (#310–#350): Phase 0 relanded the stranded
#303 audit; W1 hygiene (dead code, print-zero + ratchets, the io/log.h sink,
canonical constants); W2 duplication collapse (strutil, framers + PeerAddr, io
primitives, gd helpers, tooling, pcapio, W2-7 hardening; W2-3 retired as refuted);
W3 split the six god files into one-concern TUs (mission, collision, 3DI3, GP,
ai, def) and fixed the citation ratchet (#349). Remaining — each a different kind
of job, not the TU-split recipe:

- [ ] W3-4 `http_listener::start` — a ~2,200-line FUNCTION decomposition in a net
      lib (the wire byte-gate applies)
- [ ] W3-5 `apps/importer/scene_builder.py` split (gate = `scripts/test_python.sh`)
- [ ] W3-6 `nova_object_data.cpp` split — first collapse its 9
      `std::clamp(..., -32768, 32767)` copies onto the file's int16 helper, or the
      promoted-literal lint fires on all nine
- [ ] W3-7 header-width pass
- [ ] W4 GDScript: `game_world.gd` forwarder-delete + extractions, typed fields to
      retire the ~90 `has_method` guards, F3 overlay per-tab, modtools splits
- [ ] W5 consolidations + push-downs: perf-span unify, sim debug-snapshot
      narrowing (debug half only), ShellServices/DocumentKind/undo consolidation,
      push-downs (ItemSeatSpecs, camera, armory/catalog) — opportunistic, under
      the standing "new engine logic starts in libs/" rule

## Project health follow-ups

- [ ] Release-gate parity: make tag releases run the same required quality gates as PR/master CI, or reject release tags whose commit is not on `master`. Acceptance: an off-master tag cannot publish, and a valid release commit passes the shared maturity, native, Python, and Godot gates.
- [ ] Full Linux core tests: add an Ubuntu leg for the complete native/Python suite after triaging any platform-only failures. Acceptance: the full CTest and Python suites run on Linux for every PR without relying on the net-only or packaging jobs.
- [ ] Incremental conventional linting: establish project-owned editor/format settings, then add per-language lint checks in advisory or changed-file mode before enforcing them. Acceptance: CI checks new changes without requiring a repository-wide reformat, with documented local commands for each enabled linter.
- [x] Missing `.gd.uid` sidecars: 582 are tracked but 11 scripts have none (`godot/engine/world/panm_clock.gd`, `godot/engine/world/present_emplaced_weapon.gd`, and 9 files under `godot/tests/`). Godot regenerates them on the next import, which dirties a clean worktree for anyone who opens the editor. Acceptance: run an import once and commit the generated sidecars, then keep them in the same commit as any new script. (Done: the generated sidecars landed on master via #300's squash, and later renames/new scripts carry theirs in the same commits.)
- [ ] Two ctests are `DISABLED TRUE` in `tests/CMakeLists.txt` with reasons recorded but no owner: `parametric_parity` (long byte-identical CPT fixture run, disabled pending CI stability/perf cost) and `particle_smoke_all_fixtures` (waiting on the full 77-file corpus being mirrored into `fixtures/particle/`). Acceptance: each is either re-enabled or converted into an env-gated test alongside the rest of the asset-gated set (`docs/asset-gated-tests.md`).

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
