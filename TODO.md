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
- [ ] Apply the grilled loadout-tuple semantics to the "Weapon loadout" panel: the IDA
      grill is DONE (net-re §5.63, D-MIS-2 closed — tuples are
      `{name, ammoPri, ammoSec, flags}` sanitized by `AIProfile_SanitizeConfigData @ 0x40cfe0`),
      but `loadout_groups_inspector.gd` still shows "Value 1"/"Value 2" with "usually -1"
      tooltips and `bms.h` still names the fields `value1/value2/value3`
- [ ] Group record semantics: the field WIDTHS are grilled (`bms.h`,
      `[orig: Med_WriteBmsFile @ 0x44f920]` — @0 a 2-bit flags, @8 the only free int,
      @12 the literal 10) and the editor clamps to that shape, but the two flag bits and
      the free int still lack semantic MEANING — grill what the engine reads them for
      and label the UI accordingly
- [ ] Too much useless text noise in Mission tab
- [ ] Briefing needs to be a larger textbox. Also confirm whether it can be a string (rtxt) and integrate nicely if so
- [ ] "Music track" being a number is no good. Better integration (the music workspace
      and `NovaMusicService` exist now to feed a named picker)
- [x] Still no sound? (RESOLVED: audio is end-to-end now — `nova_mission_audio.gd`
      carries the PlayWavList dialog queue + `snd:` emitters, plus `nova_music_service.gd`
      and the sound banks; the editor previews audio in the Sound and Music workspaces.
      If auditioning audio inside the mission workspace itself is wanted, that is a new
      entry.)

## BMS Scripting

- [ ] "PlayWavList" needs deeper editor integration: the dialog/wav id param is a raw
      number, and the runtime resolution chain through the co-named `.DBF` is in
      (`nova_mission_audio.gd`), so a dialog-name picker with preview is feasible now
- [x] Validate "reset after" / "pre mission" / "post mission" is implemented properly
      (validated 2026-06-10: `docs/mission/bms-event-runtime-re.md` verdict MATCHING —
      the flags dword is pinned bit0 RESET_AFTER / bit1 pre / bit2 post,
      `event_runtime.cpp` carries the `[orig]` cites for both one-shot passes, and
      `event_runtime_test.cpp` pins delay, signed wrap, cooldown window,
      `reset_after=0` refire, and pre-pass exclusivity)
- [x] "ShowWaypoints" needs to be validated (validated: action 40 dispatch
      `[orig: Game_SetShowWaypoints @ 0x58fb50]` in `event_runtime.cpp`, both the on and
      off legs pinned in `event_runtime_test.cpp`; the HUD consumer chain is recorded in
      `bms-event-runtime-re.md` and `hud-re.md`)
- [ ] If there is no "sub-type" for an action (ie: only Null), just disable the box
- [ ] MisvarChange/Set etc need better editor integration

## Editor-wide

- [x] The editor needs "depth": these files reference each other, and those references
      should be links (DONE via the editor-depth waves: `godot/modtools/framework/links/`
      ships ResourceRef/StringRef/TextureRef widgets with resolve badges and
      jump-to-workspace, the "Used by" ReferenceStrip, and ReferenceServices, plus shell
      quick-open — adopted by the mission, environment, mnu, avatar, and credits
      workspaces. The specific still-unlinked fields — music track, PlayWavList dialog
      id, AI class/script — are tracked as their own entries above.)

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

- [ ] Terrain native `[orig]` citation pass: sweep the remaining uncited chains —
      `libs/terrain` carries 22 anchors across 15 files and `godot/engine/terrain/` 18
      across 8 of its ~16 source files (the cpt/til/trn resource-format files and the
      remaining builder files carry none); `docs/terrain/terrain-re.md` is still partial
      (PAR-R1); narrow or close this entry after the sweep
- [ ] Present-pass / entity-reconcile citation pass: `wire_present_pass.gd` now carries
      7 `[orig]` anchors, and `mission_present_pass.gd`'s anchors moved to the native row
      walk with #321 (`godot/engine/simulation/nova_present_applier.cpp`, 7 anchors) —
      but `engine/world/mission_entity_registry.gd` still carries none. Remaining:
      engine-research the original entity-reconcile chain and cite it into
      `docs/runtime-architecture.md` + `docs/correspondence.md`
- [ ] Two-net-stack convergence: the NovaNetClient replay/spectate path vs the NovaWorldClient/NovaSimulation listen-server path (see godot/engine/CLAUDE.md); decide convergence once the net workstream stabilizes
- [ ] `engine/mcp/` relocation (optional): all 16 files are class_name-referenced with zero `res://engine/mcp` literals (#376 added the `game_mcp_*` service trio and `mcp_peer_client.gd`), so it can move (e.g. next to `modtools/mcp/`) without path edits if engine/ layering ever needs it
- [ ] `opennova::io` adoption continuation: migrate remaining per-lib byte readers on-touch (policy in libs/CLAUDE.md); excluded: mus/wac VM cursors (faithful-port surface)
- [x] `mission_controller.gd` full decomposition (beyond what the inspector split needed): extract selection/gizmo/placement concerns (DONE across #204 + #373: selection → `controller/selection_ops.gd`, gizmo → `controller/viewport_ops.gd`, placement → `controller/placement_ops.gd`; the ~1,040-line residual is the composing facade the section framework prescribes — document/selection/overlay state stays on the controller by design)
- [ ] Mission workspace rail conversion: with the inspector decomposed into section components, moving Mission onto `_build_inspector_defs()` workflow rows is a small step, but it swaps the in-panel mode tabs for the shell's workflow rail (visible layout change) - needs a deliberate UX pass
- [ ] Failed replay-boot rollback (owner: `godot/game/net_session_controller.gd` — the `load_net_session()` call moved there in #310): `_enter_net_session()` enters the world FIRST and on an immediate error only warns and returns, stranding the shell in `State.WORLD` with no session; route the error through `main_game.gd`'s idempotent teardown (`_abort_to_menu`/`_teardown_world_to_menu`, which post-#376 also handles the rootless leg) and clean up the kill-feed child added on success. Acceptance (`godot/tests/game/main_game_lifecycle_test.gd`): a replay boot rejected before connecting restores `State.MENU`, menu visibility, hidden world/HUD, and no kill feed.
- [ ] NovaWorld disconnect-state reset (owner: `godot/game/novaworld_panel.gd`): clear all connection-derived rows, login/join state, pending mission/player data, and disable Host/Join/Login on disconnect or error. Acceptance (`godot/tests/novaworld_panel_test.gd`): a populated, logged-in, pending-join panel returns to a clean disconnected state and cannot submit a stale row. Coordinate with the unlanded novaworld_panel rework on the `worktree-gsb` branch before landing.
- [ ] Flavored-export initiator preservation (owner: `godot/modtools/editor/shell/save_export_flow.gd`): retain the workspace that opened the flavor dialog instead of resolving whichever workspace is active at confirmation. Acceptance (`godot/tests/terrain_editor_workstation_test.gd`): switching tabs while the dialog is open exports the initiating workspace exactly once and never the newly active one.
- [ ] Idempotent browser-pane wiring (owner: `godot/modtools/editor/shell/layout_persistence.gd`): guard the browser toggle connection just like the split signal. Acceptance (`godot/tests/terrain_editor_workstation_test.gd`): calling `wire_browser_pane()` twice raises no signal-connect error and one toggle produces one visibility/persistence update.
- [x] Defined bitstream dword access (owners: `libs/io/include/io/bit_stream.h`, `libs/cpt/src/cpt_io.cpp`): replace the unaligned `reinterpret_cast<uint32_t *>` stores with byte-safe little-endian loads/stores and converge CPT on the shared primitive. Acceptance (`tests/io/io_test.cpp`, `tests/cpt/cpt_roundtrip_test.cpp`): fields crossing 1-, 2-, and 3-byte offsets retain golden bytes and run clean under UBSan alignment checks. (Done 2026-07-28, quality campaign W2-7: both writers now read-modify-write through `io/le.h`. `io_test`'s `test_bit_stream_unaligned_golden` pins the golden bytes at 1-, 2- and 3-byte offsets, and the byte-identical CPT fixture parity run (`parametric_parity_test`, ~23 MB of CDEP+POLY bitstream across 4 fixtures) confirms the encoder is bit-for-bit unchanged. The CONVERGENCE half is deliberately NOT done — see the next entry.)
- [ ] Converge `libs/cpt`'s bit codec on `io/bit_stream.h` (owner: `libs/cpt/src/cpt_io.cpp`): the two have diverged (cpt's writer carries a normalizing `set_position` and a `write_to_file`; its reader now carries `remaining_bits`), so this is a real migration, not a swap — the reason it is tracked separately in `libs/CLAUDE.md`. Acceptance: `parametric_parity_test` still reports byte-identical CPT output for all four fixtures after cpt drops its private copy.
- [x] Bound malformed CPT declarations before allocation (owner: `libs/cpt/src/cpt_io.cpp`): validate CDEP block-count/width products and remaining bit budget before resizing decoded buffers. Acceptance (`tests/cpt/cpt_roundtrip_test.cpp`): a tiny CDEP declaring `0xffff * 0xffff` pixels and a truncated large-count POLY both return `false` with stable errors, without large allocation or ASan findings. (Done 2026-07-28, W2-7: the CDEP product is computed in 64 bits and bounded by the format's fixed 1024x1024 depth image, plus a minimum-20-bits-per-block budget against the section's remaining bits; POLY bounds its vertex count the same way and rejects `bit_width == 0`, which used to shift by -1. Pixels deliberately have no per-bit bound: `bits_per_delta` may legitimately be 0.)
- [x] Reject backward RTXT text ranges (owner: `libs/rtxt/src/rtxt.cpp`): require `text_data_end` to be at or after the entry table before treating it as section metadata. Acceptance (`tests/rtxt/roundtrip_test.cpp`): a one-entry file whose `text_data_end` points inside the entry table is rejected with a stable parse error. (Done 2026-07-28, W2-7.)
- [x] Harden replay numeric input and playback I/O (owner: `apps/nw_replay/nw_replay.cpp`): reject non-finite or partially parsed numeric arguments and propagate capture-read / UDP-send failures instead of reporting playback complete. Acceptance (new `tests/novaworld/nw_replay_cli_test.cpp`): `--speed nan`, `inf`, junk, and overflow fail parsing, while injected stream/read and send failures produce a non-zero playback result. (Done 2026-07-28, W2-7: the CLI and the paced playback core moved into `apps/nw_replay/nw_replay_cli.{h,cpp}` — `parse_args` reports through an out-param instead of printing, and `run_playback` takes its capture reader, its send and its clock as parameters, so the test injects the failures directly.)

## Quality campaign — remaining slices

The 2026-07 quality campaign (approved 2026-07-28) runs as small independent PRs
straight to master. Waves 1–4 are DONE (#310–#375): Phase 0 relanded the stranded
#303 audit; W1 hygiene (dead code, print-zero + ratchets, the io/log.h sink,
canonical constants); W2 duplication collapse (strutil, framers + PeerAddr, io
primitives, gd helpers, tooling, pcapio, W2-7 hardening; W2-3 retired as refuted);
W3 split the six god files into one-concern TUs (mission, collision, 3DI3, GP,
ai, def), fixed the citation ratchet (#349), and closed with the W3-4..W3-7
slices below (#352–#356); W4 was the GDScript wave (#357–#375). Remaining: W5
only, plus the no-magic remainder:

- [x] W3-4 `http_listener::start` (DONE: the 2,230-line function became six
      per-family registrar methods — admin API, publish callbacks, public API,
      legacy login chain, legacy host/join, static/catch-all — handler bodies
      verbatim; gate = the Linux HTTP-ON compile + boot smoke, since no ctest
      links this TU and the HTTP surface has no goldens)
- [x] W3-5 `apps/importer/scene_builder.py` split (DONE: 2,209 lines → the
      `apps/importer/scene_builder/` package — five concern mixins + core facade +
      helpers, public import surface unchanged; gate held at `scripts/test_python.sh`)
- [x] W3-6 `nova_object_data.cpp` split (DONE: the clamp copies collapsed onto
      `clamp_to_i16`, then the 3,991-line binding cut into seven one-concern TUs +
      `nova_object_data_internal.h`, following the `nova_simulation_*` family shape)
- [x] W3-7 header-width pass (DONE across two PRs: the net half extracted
      `npwire/entity_class.h`, dropped two dead includes, and gave session_hello's
      identity defaults one home; the sim half split `env_render.h` into
      `env_weather.h`/`env_celestial.h`/`env_water_render.h` (+ BMS overrides into
      `env.h`), trimmed the `world.h` umbrella by extracting `system.h`,
      `net_command_sink.h`, `vehicle_mount.h`, `entity_commands.h`, and landed the
      `oversize_cpp_files` ratchet at its residual floor of 2)
- [x] W4 GDScript (DONE: W4-1 forwarder-delete; W4-2 guard floor — the
      `has_method_guards` ratchet landed at 578 (floor now 579: #367 raised it
      to 585 with documented seams, #377 banked it back down); W4-3
      `game_world.gd` extractions
      3,876 → 2,295 (#362/#364/#366); W4-4 `local_player_host.gd` split
      1,440 → 672 (#369/#370); W4-5 retired as already-done — #342's F3
      rework shipped the per-tab overlay; W4-6 modtools splits — `live_mode`
      1,951 → 779 (#372), `mission_controller` 1,695 → 1,093 (#373),
      `nova_object_model` 1,877 → 1,171 (#374), `terrain_editor`
      2,540 → 1,446 — and the `oversize_gd_files` ratchet landed at its
      residual floor of 7)
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
- [ ] game_debug MCP state rows report `writable:false` / "Unlock edits" even when
      `confirm_authority` made the write succeed: `read_control_state` hardcodes
      `allow_authority=false` (`nova_debug_session.gd`). Thread the caller's granted
      authority into the returned row.
- [ ] MCP hardening: `game_screenshot` acquires the presentation lease before
      validating its arg types (`game_mcp_tools.gd`) — a wrong-typed arg errors between
      acquire and release and leaks the lease; and the editor's descriptor wait loops
      (`editor_mcp_game_tools.gd`, 15 s) never check child liveness, so a child that
      dies pre-descriptor stalls the full window.
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
- [ ] Consolidation leftovers: retire the `debug_option_changed` compatibility signal
      (its assertion sites in `nova_debug_overlay_test.gd` need porting onto the
      session's `control_changed`/`control_invoked` signals — the repo bans back-compat
      shims); single-source `PUBLIC_GAME_CONTROL_ACTIONS` (a hand-copy of
      `game_debug_host.gd`'s action match arms); delete the dead
      `NovaLaunchFlags.oned_run_id`/`oned_run_descriptor` accessors or converge
      `GameMcpService`'s private arg parser onto them.

## Project health follow-ups

- [ ] Release-gate parity: make tag releases run the same required quality gates as PR/master CI, or reject release tags whose commit is not on `master`. Acceptance: an off-master tag cannot publish, and a valid release commit passes the shared maturity, native, Python, and Godot gates.
- [ ] Full Linux core tests: add an Ubuntu leg for the complete native/Python suite after triaging any platform-only failures. Acceptance: the full CTest and Python suites run on Linux for every PR without relying on the net-only or packaging jobs.
- [ ] macOS CI coverage: #344 removed every macOS job from regular CI (the test-matrix leg, `build-gdextension-macos`, the godot-tests macOS leg, and the macOS package jobs); `macos-latest` now appears only in `release.yml` tag-time packaging, so macOS builds are first exercised at release time. Decide: restore a macOS leg (full or smoke) to PR/master CI, or record the lapse as accepted and note the release-time-only risk.
- [ ] Incremental conventional linting: establish project-owned editor/format settings, then add per-language lint checks in advisory or changed-file mode before enforcing them. Acceptance: CI checks new changes without requiring a repository-wide reformat, with documented local commands for each enabled linter.
- [x] Missing `.gd.uid` sidecars: 582 are tracked but 11 scripts have none (`godot/engine/world/panm_clock.gd`, `godot/engine/world/present_emplaced_weapon.gd`, and 9 files under `godot/tests/`). Godot regenerates them on the next import, which dirties a clean worktree for anyone who opens the editor. Acceptance: run an import once and commit the generated sidecars, then keep them in the same commit as any new script. (Done: the generated sidecars landed on master via #300's squash, and later renames/new scripts carry theirs in the same commits.)
- [ ] Two ctests are `DISABLED TRUE` in `tests/CMakeLists.txt` with reasons recorded but no owner: `parametric_parity` (long byte-identical CPT fixture run, disabled pending CI stability/perf cost) and `particle_smoke_all_fixtures` (waiting on the full 77-file corpus being mirrored into `fixtures/particle/`). Acceptance: each is either re-enabled or converted into an env-gated test alongside the rest of the asset-gated set (`docs/asset-gated-tests.md`).

## Player info (player.mnu / PLAYER_INFO)

The avatar lists, cascade, team filter, name, 3D preview, ACCEPT seam, and the three
loadout weapon lists (PRIMARY/SECONDARY/ACCESSORY) are wired
(godot/game/player_info_menu_host.gd; grilled in docs/playerinfo/avatars-re.md
D-PLAYERINFO-7..12). Remaining:

- [ ] Loadout ammo combos + weight readout (the D-PLAYERINFO-11 residual). The weapon
      side is DONE: `libs/def` parses the loadout fields (slot/class/team-mask/weight,
      `def.h` `[orig: WeaponDef_ParseProperty @ 0x54d730]`), the weight math is ported
      and unit-tested (`def_loadout_weight`/`def_encumbrance_class`),
      `NovaWeaponDatabase` exists, and the host populates the three weapon combos with
      the class + team masks. Remaining: the `*_AMMO*` combos
      (`populate_weapon_accessory_ammo_ui @ 0x55e8b0`) need an ammo-side binding
      (a `NovaAmmoDatabase`, or extend `NovaWeaponDatabase`), and the
      STATIC_TOTAL_WEIGHT budget label + weapon icons
      (`update_player_info_weight_and_weapon_icons @ 0x55f480`) need host wiring.
- [ ] Persist the avatar/class/loadout selections to the on-disk profile
      (D-PLAYERINFO-9 — the NAME half is done: `NovaPlayerProfile.save_callsign` writes
      `user://player_profile.cfg`) + render the chosen combo on the spawned soldier
      (D-PLAYERINFO-1, still NEEDS-RE).
