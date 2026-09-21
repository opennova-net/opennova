# ADR 0040: the engine is one namespace — no Nova prefix, files follow classes, group-qualified includes

- **Status**: accepted (2026-08-26; maintainer directive)
- **Updated**: [ADR 0042](0042-godot-permanent-shell-one-mission-kernel.md)
  (2026-08-28) fixed decision 6's consumer list (the "any future non-Godot
  front-end" clause is gone) and reframed ladder rows A3 and B (remainder) as
  the mission-kernel / listen-host slices.
- **Owners**: engine layout, build topology, the Godot binding layer
- **Supersedes/updates**: ADR 0034 decision 5's deferral ("file names keep
  their `nova_` prefix for now"); ADR 0029's and ADR 0024's include-root
  clause ("each GROUP directory is the one public include dir, so
  `#include <domain/file.h>` resolves"); ADR 0034 §2's list of the engine's
  non-Godot consumers. ADR 0020's terrain seam, ADR 0029's five group
  targets and PUBLIC chain, and ADR 0033/0034's boundary rule stand.

## Context

This is a port, but everything we build is a feature of *our* engine. There is
no separate "Nova layer" wrapping something else, so nothing in the tree earns
a `Nova`/`nova_` prefix: not the registered classes (retired in ADR 0034 d5),
not the files (deferred there), not the GDScript aliases and autoloads, not the
`NOVA_*` macros and guards, not the shader helper functions, and not the
C-linkage `gameprofile` records. The survivors are the words that are proper
nouns in their own right — `NovaWorld*` (the matchmaking service we speak the
wire protocol of), `NovaLogic` (the vendor), `opennova`/`opennova_*` (the
project).

Finishing the file rename exposed a real design flaw rather than a naming
nit. The engine's public include namespace was FLAT: each group target
exported its own directory as an include root, so `<mission/x.h>` was
resolved by CMake link order across four roots. The engine already aliased
itself — `formats/mission` vs `runtime/mission`, `formats/particle` vs
`runtime/particle`, `formats/wac` vs `runtime/wac` — surviving only because
the file names inside the twins happened not to collide. `godot/src`, whose
subdirectories mirror engine lib names and whose root is searched before the
engine roots, would have gained five exact path twins the moment `nova_` was
stripped (`audio/ambient_mixer.h`, `audio/sound_selector.h`,
`devtools/oned_ui.h`, `mission/mission_catalog.h`, `particle/effect_scene.h`
— each a binding whose class name equals the engine type it wraps, which is
the thin-seam ideal, not a mistake). The `nova_` prefix had been silently
carrying the uniqueness load; nothing enforced it.

## Decision

1. **No Nova prefix anywhere.** Files, identifiers, macros, header guards,
   GDScript `class_name`s, preload aliases, autoloads (`MusicService`),
   shader helper functions (`scene_output`, `terrain_fog_factor`, ...), and the
   C-linkage `gameprofile` set (`GameProfile`, `GameId`, `BootPhase`,
   `RequiredResource`, `ResourceSeverity`; enumerators `GAME_*`,
   `BOOT_PHASE_*`, `RES_*`). Survivors: `NovaWorld*` classes and the
   `novaworld_*` files that carry them (one proper noun, spelled like
   `engine/net/novaworld/` and `apps/novaworld_server/`), `NovaLogic`,
   `opennova`. "`nova_` never starts a file name" is now a lintable
   property.
2. **A file is named after the primary type it declares**, in snake_case;
   translation-unit splits keep `<class>_<facet>` (`simulation_present.cpp`).
   The class name, not the old file name, wins where they differed
   (`PffDocument` -> `pff_document.*`, `FrameFx` -> `frame_fx.*`,
   `WindowState` -> `window_state.gd`).
3. **`engine/` is the one public include root, and includes are
   group-qualified.** Every engine header is included as
   `#include <group/lib/file.h>` with group in {`base`, `formats`, `runtime`,
   `net`}: `<runtime/world/player_view.h>`, `<formats/mission/mission.h>`,
   `<base/vfs/vfs.h>`, `<net/npwire/peer_addr.h>`. Same-directory siblings
   may keep a bare `"file.h"`; every cross-lib include is the angle form.
   Each group target exports `engine/` PUBLIC. The group in the path is what
   makes the layer visible at the include line.
4. **The layering is a lint, read off the include path.**
   `scripts/lint/include_graph_check.py` (a PR gate) enforces: the ADR 0029
   d3 group order (`base/io` and `base/crt` include only each other;
   `formats` includes `base/io`, `base/crt`, `formats`; `base` includes
   `base`, `formats`; `runtime` adds `runtime`; `net` includes all four); no
   unqualified engine include anywhere under `engine/`, `apps/`, `tests/`,
   `godot/src`; the ADR 0020 terrain seam under its qualified prefixes; no
   include naming `godot` under `engine/`, `apps/` or `tests/`; and no
   `godot/src` subdirectory named like an engine group.
5. **Bindings keep `godot/src` as their include root** with quoted
   root-relative includes (`"audio/ambient_mixer.h"` is the binding,
   `<runtime/audio/ambient_mixer.h>` the engine type it wraps). Because no
   `godot/src` subdirectory may be named `base`, `formats`, `runtime` or
   `net` (decision 4), a binding path can never alias an engine path, and a
   binding may share its class name and directory name with the engine
   concept it exposes — the thin typed seam ADR 0034 asks for.
6. **The engine's portability has an honest consumer list.** ADR 0034 §2
   named an importer FFI and DCC plugins; those left with ADR 0037/0038 (no
   `oned_edit`, no shared-library export, no ctypes/pybind11). What consumes
   `engine/` without Godot today: the six `apps/` (the NovaWorld service,
   the in-match dev host, the LAN probe, the packet pretty-printer, the
   mounted-install entry extractor `opennova-extract` added by ADR 0041, the
   shared socket helpers), the ctest suite, and the C-linkage headers.
   `engine/` stays Godot-free for them — now as
   a ratcheted property (decision 4), not a re-verified one.

## Consequences

- One rule for a reader: an angle include with a group prefix is the engine,
  a quoted root-relative include is the binding tree, a bare name is a
  sibling. The twin lib names (`mission`, `particle`, `wac`) stop aliasing.
- The `adapter_cpp_orig_cites_*` [since ADR 0042 d7 the one `adapter_cpp_orig_cites`],
  `gd_orig_cites`, and size ratchets key on
  paths and survive the rename by rewriting their baselines in the same
  change; the maturity instruments are otherwise untouched.
- Historical records keep their historical names where they describe a past
  state; paths that name a current file are updated.
- No compatibility aliases: every consumer is rewritten in the same change
  (pre-1.0 policy).

## Ladder: the push-down campaign

With the names and the include roots settled, the witnessed behavior still
living Godot-side moves to an engine home. The gauge is `godot_orig_cites`
(ONE `[orig:` count over `godot/src`, `godot/game` and `godot/probes`; ADR
0043 merged the former `adapter_cpp_orig_cites` / `gd_orig_cites` pair) in
`scripts/lint/maturity_baseline.json`: a slice moves code, banks the counter,
and keeps fidelity, because the cites travel with the code they cite. A cite
moving between GDScript and binding C++ leaves the count unchanged; only code
reaching its `engine/` home banks a decrease.

The campaign runs one commit per slice, and **this table is its queue.** The
ranked list originally written into `TODO.md` was lifted back out at
`77c025a2d` (the maintainer's call for this campaign: run it in the PR, not as
a TODO queue), so the remainder is tracked here and nowhere else. The ladder is
finished when both counters hold only documented seam contracts, not when they
reach any particular number.

| Slice | What moves | Engine home | Status |
|---|---|---|---|
| A1 | the local player's view cluster | `engine/runtime/world/local_player_view.{h,cpp}` | LANDED `4a7195299` |
| A2 | the minimap marker rows and their feed layout (duplicated at `simulation.h` and `hud/hud_overlay.cpp`) | `engine/runtime/inmatch/minimap_markers.{h,cpp}` + `engine/runtime/hud/hud_minimap_feed.{h,cpp}` | LANDED `61bfa5efa` |
| A3 | `simulation.cpp`'s `advance_world_tick` / `boot_mission` / `restore_world_baseline` | `engine/runtime/mission/mission_kernel` + `engine/runtime/inmatch/listen_host` (ADR 0042) | LANDED `972c774de` + `6510891bb` |
| A4 | the LAN browse/announce cadence and its constants | `engine/runtime/inmatch/lan_discovery.{h,cpp}` | LANDED `a5ca3f3c9` |
| A5a | the boot, pack and options policies leave the GDScript | `engine/base/resource_index/boot_policy.{h,cpp}`, `engine/base/vfs/pack_policy.h`, `engine/runtime/menu/options_policy.h` | LANDED `c33b15f46` |
| A5b | the seat mirror, the volume law and the weapon-category rows | `engine/runtime/world/vehicle_attach.{h,cpp}`, `engine/runtime/audio/volume_law.h`, `engine/runtime/controls/controls.{h,cpp}` | LANDED `6e34d2e2a` |
| B3a | the occlusion frame camera build | `engine/runtime/world/occlusion_camera.h` | LANDED `c2e41b97c` |
| B3b | the iris exposure march | `engine/runtime/world/iris_march.{h,cpp}` | LANDED `97fb7a83b` |
| B (remainder) | `simulation_present.cpp`'s data model, `simulation_net.cpp`'s GameConfig policy + pumps, `simulation_player_loadout.cpp` / `simulation_player_weapon.cpp` / `simulation_player.cpp`, `simulation_assets.cpp`, the feed marshallers, then the `simulation.h` state-model split, after which `Simulation` is a `TickTarget` adapter — reframed by ADR 0042: the rig-twinned half moves into the kernel; the Godot-only witnessed blocks stay binding-side until grilled | the rig-twinned half: `mission_kernel` + `listen_host` | PART-LANDED `6510891bb` (the boot/tick/player/table bodies); the present drains, the weapon view, the waypoint view, the objectives feed, the music-var pump and the fire-effect admission are E0 (2026-09-20); the joiner-side blocks (the sun feed, the deploy client, the end-round view, the HUD client feeds, the effect pose index) are E8; the scar gate stays binding-side ON-TOUCH after a grill |
| B1 | `Simulation::bringup_host_runtime` re-implements `listen_host::bringup` (the same `SinglePlayer_StartMission @0x561af0` body twice): parameterize `listen_host::bringup(kernel, state, ListenBringupOptions{host_cfg, socket_mode, serve_and_play, local_character_vars, mission text/til installs})`, adopt `state.client_runtime` as the HostClient runtime, and make the binding a converter | `engine/runtime/inmatch/listen_host` | CLOSED-STALE (2026-09-20): the parameterization it asked for exists — `inmatch::HostBringup` (`engine/runtime/inmatch/host_role.h`) staged by `Simulation::host_bringup` and consumed by `HostRole::bring_up`; the residue still in `simulation_net.cpp` (the RTXT mission-text harvest, the `"SINGLEPLAYERGAME"` / `0x3A06` literals duplicating `HostRole::bring_up_singleplayer`, the role-switch latch) is E8 |
| B2 | the deploy-screen zone rows and status composed in `simulation_net.cpp` (`get_deploy_spawn_zones`, `UI_UpdateDeathScreenContent @0x5536a0`) and re-parsed from their own Dictionaries | `engine/runtime/world/deploy_screen_feed` (`build_deploy_zone_rows` + a typed `DeployZoneRow` record) | PART-LANDED: the typed `DeployZoneRow` / `DeployScreenStatus` records and their engine builders exist; the binding-side gather (`Simulation::deploy_zone_rows`, `get_deploy_status`, which read `ClientState`) is E8 |
| B3 | the kill-feed policy composed in `drain_feed_events` (camp detection, the own/verbose gate, the STRCND48 bonus recompose) | `engine/runtime/hud/feed_format` (`feed_event_rows` + a typed `FeedRow` record) | LANDED `fbd04a8e6` |
| B4 | the local-player view/aim frames and the character profile still crossing as Dictionaries (`get_local_player_view`, `get_local_player_aim_overlay`, `MissionSetupOptions.local_character_profile`) | typed `LocalPlayerViewFrame` / `AimOverlay` / `CharacterProfile` records assigned from the engine structs | QUEUED (census C-15, C-22) |
| C1 | the mission load plan (`Game_StartMission`'s sequence + progress schedule) | `engine/runtime/mission/mission_load_plan.h` | LANDED `d081d2908` |
| C2 | the HUD presenter's config tokens and feed units | `engine/runtime/hud/hud_config_tokens.h` | LANDED `d8fa149b2` |
| C3+C4 | the presenter's swizzles/rangefinder and the viewmodel rig's frame math | `engine/runtime/world/presentation_frame.h` + `engine/runtime/renderer/fp_viewmodel_spec.h` | LANDED `0b07c5f9f` |
| C5 | the light director's spawner constants | `engine/runtime/renderer/light_scene.h` | LANDED `89440caf7` |
| C6 | the loading screen's names, sidecar and due rules | `engine/runtime/hud/loading_screen.h` | LANDED `4c8a38c6a` |
| C7 | the character registry and the joiner profile | `engine/runtime/inmatch/character_registry.{h,cpp}` + `engine/runtime/inmatch/join_character_profile.h` | LANDED `eb9fa8c69` |
| C8 | `godot/src/audio/mission_audio.cpp`'s reverb, which is an invented approximation and so cannot stand as written: port `Audio_LoadReverbDefs @0x766d80` or ledger the divergence | `engine/runtime/audio` | CLOSED (ledger D-SND-18): the invented reverb is gone — `MissionAudio::_apply_reverb` only strips any bus reverb, region selection lives in `World::reverb`, and the original mixer's preset copies have no observed sample consumer |
| C9 | `godot/src/world/session_drive.cpp` (ex net_session_drive.gd, ADR 0043 slice G10), plus the `destruction_presenter.cpp` / `fire_presenter.cpp` passes (the item-effect director's law landed in `engine/runtime/world/item_effects.{h,cpp}`, ADR 0043 slice G6) | to be chosen per slice | QUEUED |
| C10 | the player-info and armory companions (`godot/game/player_info_menu_companion.gd`, `godot/game/world/armory_menu_companion.gd`) — real behavior, not wired-doc cites: the nationality/division/combo cascade, the class and team weapon masks, the ammo/grenade fills, the weight and encumbrance line, the `weapon.sav` kit-page serialization (`serialize_weapon_loadout @0x55e4b0`) and the 11-row voice table (`0x83C7A8`) | split: the model is E4, the screen controllers ride E5 | QUEUED (superseded by E4 + E5) |
| D | `godot/src/object/object_model_anim.cpp` + the `skeletal_anim` slot machine to `engine/runtime/anim`; the `item_database` / `weapon_database` doc blocks to their engine homes; the CTRL store + PANM cache; `godot/src/env/weather.cpp`'s mission-start boundary; `godot/src/particle/particle_renderer.cpp`'s `lit_primary_color`; `godot/src/env/water_core.cpp`'s view builder; `godot/src/terrain/terrain.cpp`'s normal + quadrant policy | to be chosen per slice | QUEUED; the `object_model_anim.cpp` remote-body machine is E6 — it is the seconds-domain twin of `engine/runtime/replication/client_replica_body_arbitration.cpp` and the two DISAGREE on a same-state arrival (the binding clears an armed pending, the engine keeps it, cited `[orig: @0x4c115f / @0x4c0606]`): grill before unifying |
| E0 | slice 1 of the 2026-09-20 census (a Claude three-agent census reconciled with a Codex read-only review): the `Simulation` builders that already produce engine structs and touch Godot only on their last line — the present drains (`fill_throwable_visual_rows`, `fill_vehicle_trail_visual_rows`, `drain_round_impact_rows`, `drain_fire_presentation_rows`, `fill_death_pieces`, `fill_round_glows`), the waypoint HUD view, the objectives feed with one neutral text-lookup seam, the local-player weapon view, the game-music var pump, the fire-effect admission (the rendered muzzle anchor stays device) | `engine/runtime/world/present_drains.cpp`, `waypoint_track`, `objectives_feed`, `hud/game_text_lookup.h`, `player_weapon_view.cpp`, `music_vars`, `player_present` | LANDED (this PR, six commits; `godot_orig_cites` 996 -> 956, seven of them unbanked master decreases) |
| E1 | the record bindings that exist only so GUT can read an engine struct: `pff/pff_document` (GUT-only, 836 lines) goes outright; of the 49 record TU pairs, 26 have no production-GDScript consumer (`mission/mission_records`, `particle/particle_def`, `audio/mission_audio_records`, `simulation/hitbox_debug_report`, `lights/effect_light_report`, ...) — a candidate ceiling, not a deletion budget, because probes and C++ consume some: migrate each consumer to a headless ctest, then delete the wrapper, its `_bind_methods` and its `register_types` row (ADR 0043 d10) | `tests/<domain>/` | PART-LANDED (this PR): `pff/pff_document` is gone with its GUT file (its engine facts were already pinned: `pff_unit`, `scr_unit`'s SCR0 guard, `resource_index_mus`; the raw-fallback extraction was ONED authoring policy, ADR 0037). The re-verified census of the rest (25 ClassDB classes, 14/1/4/3/3, plus the already-unbound `ItemParticleFx` struct; 135 explicit GUT references across 26 files; consumers counted by ACCESSOR, not by class name, because GDScript reaches them through typed getters): `lights/effect_light_report` has a production and MCP reader (`game_render_diagnostics.gd` calls `to_json_value()` on the report; `game_debug_adapter.gd` relays it), `CoronaRow` is test inspection only; `mission/mission_records`: `MissionAreaTrigger` reaches `MissionRoot` / `EntityIndex`, `MissionEntityRecord` reaches a probe (`retail_parity_visual_probe.gd` through `MissionData.add_entity()`), the rest are authoring/inspection surfaces; `object/item_records`: `EnvsMarkerRow` reaches `MissionAudio` and `ItemParticleFx` the item-effect director, the seat/emplacement wrappers are test inspection; `mission/static_source_records`: the effect and light-draw rows reach the provider and the directors, `StaticTerrainShadowSourceRow` is test diagnostics; `particle/particle_def`: `ParticleFile::to_native()` feeds `EffectScene`, the renderer consumes the native def. Each family is its own slice: the test-only rows go first (assertions to ctests, then the wrapper dies), the live carriers move onto the engine structs, and the light-report slice migrates the diagnostics JSON contract. `effect_light_report` (this PR): `CoronaRow` and the `collect_corona_rows` inspection seam died — the corona walk's semantics are the `renderer_light_scene` ctest's and the GUT buffer test pins the MultiMesh packing against those literals through `get_last_corona_buffer`; the report keeps its production JSON contract. Queue order: `static_source_records` (`StaticTerrainShadowSourceRow` is read by three placer/present GUT files as the shadow-source diagnostics, so its cut needs a headless placer oracle first), `item_records`, `particle_def`, `mission_records` |
| E2 | the loading screen's text layout (`godot/game/ui/loading_screen.gd`: the verbatim `render_draw_wrapped_text_block_ex @0x580eb0` port — the line breaker and the line placer) | `engine/runtime/hud/loading_screen.{h,cpp}` (`wrap_text_lines`, `layout_text_block` over a `TextExtent` measure); the shell paints the placed block through `HudPos.draw_wrapped_text` | LANDED (this PR): the rules and their eight GUT pins moved to the `loading_screen` ctest; the measure stays the FontFile view, so D-LOADSCR-2 (CGameFont glyph metrics) stays open until the loading screen draws `GameFont` quads; the splash machine and the band policy are device legs and stayed |
| E3 | the HUD key latches and toggle machines (`godot/game/world/game_hud_presenter.gd` and the sub-lane presenters: the gated down-edge latch, the huddetail/hudcolor/showhud/dotsize/goals/view-action rows incl. the D-CTRL-4 first-match shadowing, the playerlist/OldMessages/ShowScore window toggles with the SP-only gate and sibling close, the respawn clears, the death-screen force, the friendly-tags cycle with its toast keys) | `engine/runtime/hud/hud_toggles.{h,cpp}` (`HudToggleState`, `hud_toggles_poll`) behind the `HudToggles` binding the presenter holds across mission rebuilds | LANDED (this PR): the presenter samples the keys and applies the device side effects the poll's events name; the three lanes take the engine's open flag; new `hud_toggles` ctest. E3b part-landed (this PR): the `DefHudPosFile` -> Godot types -> `HudLayout` round trip in `hud_overlay.cpp` is `hud::hud_layout_from_hudpos` in `engine/runtime/hud/hud_layout_from_hudpos.cpp` (the corner rects against the one x,y,w,h rect, the 4-field positioned records, the packed colours, the spinmap extent gate, the stance slots by id, the last-authored static frame, the hi-first font) returning the texture NAMES the overlay resolves; the `HudPos` binding lost its GUT-only record getters (`get_health_rect`, `get_colors`, `to_dictionary`, ...; ADR 0043 d10) and keeps the load contract, `native_file()` and the `VehicleHudBlock` handoff; new `hud_layout` ctest with a retail leg. The presenter's text compositions followed: the waypoint name's `STRWPNAME%03d` / "null" -> `STRWPNAMEDEFAULT` fallback, the `WinConditions`/`LoseConditions` subgoal announcement, the `Triggered Text`/`ID%03d` line and the WepDes weapon name are `engine/runtime/hud/hud_game_text.cpp` behind `HudPos` statics over the string tables, and the feed row's four gametext lookups (template, camp WPNames, `STRCND48` bonus, `STRCLI01` unknown actor) are `hud::feed_row_line` behind `FeedRow.resolve_line`; the `game_text_lookup` factory moved beside `RtxtStringFile`; new `hud_game_text` ctest. Still queued under E3b: the binding-token -> action table in `player_input_router.cpp` (poll, map and overlay gating interleave: high risk); the WAC Lose banner keeps its override-first `Misc` lookup in the presenter |
| E4 | the kit model behind the player-info and armory screens (C10's behavior half): class/team masks, ammo and grenade fills, the weight line, the `weapon.sav` kit serialization, the voice table; the GDScript companions become appliers of a typed screen model (ADR 0043 d9 keeps them as shell classes) | `engine/runtime/menu/player_info_kit` + `engine/runtime/world/player_loadout` | PART-LANDED (this PR, E4a): the PLAYERVOICE list (the 11-row voice table `@0x83C7A8`, DEFAULT_VOICE first, the disabled row skipped, the persisted-override reset `@0x55de21`) and the `weapon.sav` kit page order (`serialize_weapon_loadout @0x55e4b0`: the per-side knife, the medic's medpack, the three categories with their ammo-type flags, the fixed three grenade slots with the entry-0 quirk) are `menu::player_info_voice_values` / `player_info_voice_selection` / `player_info_kit_entries` in `engine/runtime/menu/player_info_kit.cpp`, reached through `WeaponDatabase` statics and `player_info_kit_entries`; the companion supplies its picks and applies the rows; new `player_info_kit` ctest. E4b part-landed (this PR): the two screens' shared text compositions — the WepDes weapon label with its raw-id fallback (`populate_weapon_slot_lists @0x560430`), the `"<rounds> - <round label>"` ammo row (`@0x564c7d..0x564ce4`), the WEAPON screen's case-insensitive row order (`ListWidget_SortRows cmp @0x6448a0`, NONE at row 0 `@0x566f15`) and the weight readout with its encumbrance band tokens (`update_weapon_weight_display @0x565640`, `update_player_info_weight_and_weapon_icons @0x55f480`) — are `engine/runtime/menu/loadout_labels.cpp` behind `WeaponDatabase` (`weapon_label`, `ammo_row_label`, `armory_slot_order`, `loadout_weight_line`); the GDScript `LoadoutLabels` helper keeps only the weapon.def load; new `loadout_labels` ctest. The GRENADE_AMMO* combos' zero-row default-select both companions duplicated is `world::player_info_default_grenade_row` beside the clip-row rule (`WeaponDatabase.default_grenade_row`). Still queued under E4b: the ammo/grenade fill order, the per-slot weight sums' UI gates (a term rides the control existing / shown) — the masks and clip rows are already `world/player_loadout` |
| E5 | the compiled-menu interaction runtime written in GDScript (`menu_driver.gd`, `menu_input_dispatch.gd`, the `menu_shell.gd` flow half, the `mnu_document.cpp` ID tree): navigation stack, ACTION dispatch (`CUIWidget_HandleScriptedAction @0x6497f0`), radio groups, spin cycle, the combo-popup exclusive pump (`@0x63ab00`), the 400 ms double-click rule, the hotkey scan, edit focus, the SP mission list + ACCEPT gate, the expansion remount two-phase machine; its 14 Godot signals become drained typed requests; THEN the screen rules on top (the host-settings dialog D-MNU-17, the options remap machine with OPT_ACCEPT/OPT_CANCEL revert, the deploy-screen statics, the STAT transition latch `@0x5b8600`) | `engine/runtime/menu/menu_runtime`, `options_screen`; `engine/runtime/inmatch/host_dialog` | PART-LANDED (this PR, E5a): the GDScript `MenuDriver` + `MenuInputDispatch` pair and their five helper scripts (state store, replay, scroll range, table state, credits overlays; ~1,500 lines) are gone. `menu::MenuRuntime` (`engine/runtime/menu/menu_runtime.{h,cpp}`) owns the positional id tree (`MenuDocIndex`), the first-match name seam, navigation and the back stack (`CUIScene_SelectNodeByName @0x63b6b0` closes the dropdown; the MUSICVAR push on every screen event `@0x54e6a0`), the per-widget state store with its authored fallbacks and its replay, ACTION dispatch (`CUIWidget_HandleScriptedAction @0x6497f0`, skipped when an observer swaps the document under the activation), radio groups, the spin wrap, the exclusive combo popup (`@0x63ab00`, `@0x65c190`), the hover sound edges (`@0x647a00`), the 400 ms double-click latch, CTRL multi-select, edit focus (`@0x661510`) and the key routing; it drives the frame through `MenuFrameSeam` and reports through a SYNCHRONOUS event sink (observers re-enter it). The C++ `MenuDriver` binding (`godot/src/mnu/menu_driver.*`) keeps the GDScript class's API and nine signals, implements the seam over the `MenuFrame` node and does the device work (text tables and marquee data through the resource root, the credits scrollers, the sound edges, the MUSICVAR push, the OS cursor); `show_screen` now adopts the AUTHORED screen spelling, which fixes WINDOW actions after a differently-cased jump. New `menu_runtime` ctest over a fake seam and a synthetic document. E5b (this PR): `MnuDocument` dropped its parallel `IdWindow` tree and the O(n) `locate` walk every getter paid; it reads through the engine's `MenuDocIndex`, so the document binding and the runtime share ONE numbering, and the item-container rule is the engine's `menu_items_container`. Still queued: the `menu_shell.gd` flow half (ADR 0043 d9 keeps the shell class; its SP-list ACCEPT gate and the two-phase expansion reload are the rules left in it), then the host-settings dialog, the options remap machine. Earlier in this PR, the deploy-screen statics' texts landed early (this PR): the PSPRESPAWN / MEDICTIMER `"%s  <cFF4040>%d"` lines and the CALLMEDIC key-slot substitution (`@0x553e10..0x553f60`) are `world::deploy_statics_text` on the `DeployScreenStatus` the sim hands over (`get_deploy_status` takes the MedicReq display string); the presenter keeps the widget shows and the zone-name lookup. The STAT transition latch landed too (this PR): `hud::EndRoundTransition` (`end_round_overlay.h`) steps the once-only bytes and the board + 6000 ms gate of `UI_ProcessEndRoundScreenTransition @0x5b8600` behind the `EndRoundTransition` binding; the presenter performs the device work its STEP_* bits name, and `Simulation.end_round_stat_screen_delay_msec` died with its only caller |
| E6 | object: reconcile the remote-body machine with its replication twin (row D's note) by a grill, then unify; the CPU mesh preparation in `object_data_geometry.cpp` (strip decode, bone-table remap, weight renormalization, tangent handedness); the PANM eval cache; the native entity index | `engine/runtime/anim`, `engine/runtime/renderer`, `engine/runtime/world` | QUEUED |
| E7 | the terrain tile-cache `AsyncState` (`terrain_tile_cache_device.cpp`: CPU scheduling, cancellation and composition over `Rgba8Image` snapshots) and the `MissionAudio` emitter lifecycle / channel-pool policy in `mission_audio.cpp::tick` | `engine/runtime/terrain`, `engine/runtime/audio` | QUEUED |
| E8 | the join and load decisions: the join-role predicate written twice in `net_session_controller.gd`; the host bring-up residue (B1's remainder); the `ClientState`-reading client facts still gathered in the binding (B2's remainder: deploy zone rows and status; the end-round view and RESULTLIST rows; the LFP zone and friendly-tag feeds; the sun-visibility quality diff; the minimap grid origin and footprints; the present-effect pose index; the hitbox oracle and entity pick); a typed joiner readiness replacing the prose-string match in `godot/probes/net/parity_joiner_witness.gd` | `engine/runtime/inmatch` | PART-LANDED (this PR, E8a): the client-side feeds that read a role's replica state left the `Simulation` binding for `engine/runtime/inmatch/role_feeds.{h,cpp}` over a `RoleView` (kernel, the role's replica runtime, the authority's session context, the joiner bit, the staged host option word): the end-of-round session state and overlay input (`NapiNPClientMsg_0x01D @0x430840`, the round clock's two homes `@0x24C1958`), the stat rows joined to the roster with the local row through the board index (`populate_stat_results_list @0x562240`), the friendly-tags gather with the authority's slot lookup and the joiner's roster walk, the DEATH screen status (`UI_UpdateDeathScreenContent @0x5536a0`), the death-screen and local-dead reads; `Simulation` keeps one `role_view()` and the record marshalling. New `role_feeds` ctest. E8b (this PR): the AAS zone-panel rows (`collect_lfp_zones`: the role's zone-timer image and minimap slot flags) and the per-drawn-entity sun-visibility diff (`SunQualityFeed`: both identity domains, the per-identity caches, the culled and hidden holds; `setup_terrain_effect_for_entity @0x5c74a0` -> `Entity_ComputeSunVisibility @0x5c6800`) followed into `role_feeds`; the binding maps the light direction, hands over the culled sets and packs the triples. E8c (this PR): the present-effect pose index (`inmatch/effect_pose_index.{h,cpp}`: the pose an attached effect follows by wire handle, SSN, authored id or spawn origin, cached per decoded-client epoch with its misses, the joiner self-filter and the wire-only identity on a joiner) and the authored-id handle index (`world/bms_handle_index.h`, rebuilt on the registry's spawn serial) left `SimulationPresentState`; the binding holds one object of each and maps the engine pose to Godot space. E8d (this PR): the host bring-up residue (B1's remainder) and the deploy zone rows: the `<mission>.bin` RTXT harvest (the briefing pages with the witnessed briefing2 -> briefing fallback `@0x506649..0x506660`, the `[Locations]` / `[PeopleNames]` numeric-key sections) is `mission::MissionText` / `parse_mission_text` in `engine/runtime/mission/mission_text.{h,cpp}`, carried whole by `HostBringup` and read by the kernel's people-name resolver; the SP listen server's config literals (SINGLEPLAYERGAME, the 0x3A06 attribute word, one player) have one home, `inmatch::singleplayer_game_config`, which `HostRole::bring_up_singleplayer` and the shell's bring-up both take; the DEATH screen's zone rows (`UI_UpdateDeathScreenContent @0x5536a0`, the 0x6E wave occupants `@0x553cd0..0x553d8b`) are `inmatch::deploy_zone_rows` over the `RoleView`. The role latch in `enable_host_listen` (the pump bind and the pending-role hand-off) is device work and stays. Still queued under E8: the join-role predicate (`net_session_controller.gd`), the joiner readiness for the parity probe, the minimap grid origin, the hitbox oracle + entity pick |
| E9 | the presentation clocks: `Time::get_ticks_msec` feeds the light director, the PANM clock, the slot-shadow and terrain-light contexts, foliage, the particle compositor, and in GDScript the HUD logic clock, the STAT delay and the menu blink — sample once per frame in `GameWorld` and pass the value; wall-clock semantics kept, input-event timestamps stay separate | `godot/src/world/game_world_frame.cpp` (one sample) + the engine consumers | QUEUED |

Decisions, not slices (the same census): an engine DDS/TGA decoder (the three engine
passes that consume texels — `renderer/material_texture.h`, `renderer/particle_atlas.h`,
`terrain/terrain_tile_composer.h` — are pinned by synthetic pixel oracles, retail decoded
through D3DX, and platform primitives are exempt from the port rule, so decoding stays a
Godot `Image` device leg unless retail-byte fixtures are wanted); native UDP/HTTP in the game
(`engine/` carries no socket code by rule and ADR 0043 keeps the thin pumps; the
`IDatagramSocket` seam already has the `apps/common` native implementation if that rule is
ever revisited); the five `user://` config stores (our own schemas, consolidated behind one
owner now, ported only after the retail `player.sav` format slice, ledger D-CTRL-3);
`MainGame`'s state machine stays GDScript (ADR 0043 d9). None of E0-E9 amends an ADR: the
shell classes ADR 0043 d9 names stay GDScript as thin appliers while the witnessed rules
inside them move.
