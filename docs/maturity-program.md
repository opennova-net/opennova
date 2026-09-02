# OpenNova maturity program

> **Historical program.** The program closed on 2026-07-12. Any remaining
> ONED workspace roadmap or editor-backlog language below was retired by
> [ADR 0037](adr/0037-oned-runs-game-data.md); ONED is now run-only. The
> landed-slices and C-ABI logs remain append-only records. The flat C ABI, its
> `abi_export_identity` guard (the since-deleted `abi_exports_check.py` and its
> baseline) and the release-deliverables validator were retired by
> [ADR 0038](adr/0038-native-runtime-assets-glb-editor.md) (2026-08-26); the
> `scripts/lint/` ratchet, lint and ledger instruments remain live CI gates
> (Enforcement table below).

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
(ADR 0016) *(historical: retired by ADR 0037; the standing boundary rule is
ADR 0042's)*. Godot products: exactly two exported exes; the server is a serve
MODE of the game (ADR 0015). OpenNova Launcher is outside that taxonomy.

## Status

| Field | Value |
|---|---|
| Living appendices | Two sections stay LIVE and append-only after the close: the **Landed slices** log (ratchet-bank/gates accounting — ordinary slices append rows) and the **C-ABI baseline-bump log** (`abi_export_identity` bumps, per engine/CLAUDE.md; closed 2026-08-26 with the FFI, ADR 0038, kept as record). Everything else in this file is a historical record of the closed program |
| State | **CLOSED 2026-07-12** at the post-trunk-2 boundary, under the stop-anywhere property (maintainer decision, the close-out session). Waves 0–1 complete (Wave-1 trunk #204 = `50fce423`); Wave 2 landed the boundary flip (#205), ENG-2 (#206), ENG-3 (#209), the full REN track (#207 — REN-3..7 pulled forward from Wave 3), Wave-2 trunk 2 (#227 = `66076f66`: LIBS-2/3, ENG-4, the ENG-6 manifest), and the close-out tail (PR #236: STD-2 conversions, ONED-W1 TER-1/ENV-1/CRE-1/FNT-1/STR-1/STR-2, the MUS-D design at the maintainer gate, ENG-5 sweep #2, D-FNT-4, this close-out). Everything not executed is dispositioned in the Close-out section below |
| Final wave reached | 2 — portability + standards adoption. At close: ENG-2/3/4 done (#206, #209, trunk 2), ENG-5 sweeps #1 (boundary) + #2 (close-out) run, ENG-6 done (R8 + the trunk-2 manifest); LIBS-2/3 done (ADR 0024); STD-3 flip done (boundary), STD-2 seeds converted 7/8 (the tail; the object-material-defs contract deferred with cause); ONED-W1 six slices landed (the tail), ONED-MUS-D design landed (acceptance = the open maintainer gate); the full REN track closed. Not executed: ONED-RSP, ONED-TST refits, the ONED-W1 remainder — dispositioned below |
| Boundary gates (Wave 1→2) | full ctest: 246/248 green; the two reds are pre-existing, tracked, and not Wave-1 regressions — `npruntime_golden_gameplay` (D-NET-159 wire adjudication, asset-gated so CI never sees it) and `opennova_python_pytest` (diagnosed at this boundary: the two DCC-parity tests build a zero-write request the jobs validation rejects, then their in-parent `import bpy` poisons the 13 spawn-context worker tests — all 13 pass in isolation). *(Both since closed — golden_gameplay green since #417, pytest green; their TODO.md rows were pruned on completion per the completed-entries convention)*. FULL GUT attested in the boundary PR |
| Freeze | **LIFTED at close (2026-07-12)** — new reimplementation work no longer waits on foundation phases. What survives is the standing rule set, not the gate: ADRs 0015–0018 (products/serve mode, engine–editor boundary, typed records, public-API testability), 0019–0021, [0022](adr/0022-divergence-burn-down.md) (the PAR zero-OPEN target — burn-down continues as standing policy), [0023](adr/0023-render-visual-parity.md) (the REN rules), [0024](adr/0024-lib-family-topology.md) (family topology), and every instrument in the enforcement table (all hard-fail-forever by design) |
| Detail docs | Historical ONED track: [docs/oned/workspace-maturity-program.md](oned/workspace-maturity-program.md), retired by ADR 0037 |
| Decision ADRs | [0015](adr/0015-two-products-serve-mode.md) products/serve-mode, [0016](adr/0016-engine-editor-boundary.md) engine/editor boundary, [0017](adr/0017-typed-records-named-constants.md) records/constants, [0018](adr/0018-public-api-testability.md) testability, [0022](adr/0022-divergence-burn-down.md) divergence burn-down, [0024](adr/0024-lib-family-topology.md) lib family topology + consumption models; boundary ADRs (npwire, world seam, responsive shell) minted in their tracks at decision time |

## Close-out (2026-07-12)

**2026-08-22 renderer amendment.** Rows below are historical landing snapshots,
so their then-current `D-RMAT-8 PERMANENT` and `D-RORD-5 tracked` language is
preserved as chronology, not current disposition. Both are now **FIXED** by the
gamma-domain scene-target/terminal-display cutover and the isolated native
Q3/FrameFX renderer; see the live divergence ledger and render RE records.

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
  each logged in the slice table). ADR 0037 later retired ONED-TST with the
  authoring suite.
- **responsiveness landed + one re-baseline** — not executed; dispositioned
  below.
- **music design accepted** — the design landed (track doc §The MUS-D
  design); acceptance is the open maintainer gate.
- **REN instrument landed + materials converged** — exceeded: the whole REN
  track closed at REN-7.

Dispositions — every item not executed, and where it lives now:

| Item | Disposition |
|---|---|
| ONED-RSP, MUS-I, ONED-TST, ONED-W1/W2/W3, REF, REQ | Retired by ADR 0037's run-only hard cut; the track document remains historical evidence |
| STD-2 remainder | the object-material-defs contract deferred with cause (native-owned in both directions; the 980-line materials inspector has no dedicated test bar — a conversion needs its own slice with tests first); everything else converts adopt-on-touch under the standing diff-scoped dict-contract lint |
| ENG-5 sweep-#2 tracked rows | ordinary port slices (libs/hud, libs/audio curves, libs/env TOD clock, the player_view fold, the part-anim integrator, the armory helpers); the sweep itself remains a standing instrument, run ad hoc |
| PROD-1 serve mode, PROD-2 taxonomy, PROD-3 release dry run | tracked future work; ADR 0015 remains the decision of record — PROD-1 now rides TODO.md § Project health follow-ups (serve mode); PROD-2/3 partially overlap the release-gate-parity and macOS rows there |
| PAR ledger (at close: 46 OPEN / 13 NEEDS-RE / 31 witnessed-ready-deferred per the generated scoreboard; D-FNT-4 the newest mint) | unchanged: ADR 0022's zero-OPEN target is standing policy; `ledger_check` enforces sync forever; the burn-down continues in fidelity work |
| GOV-4 | this close-out |

What the program leaves behind, permanent: the enforcement table below
(goldens, ratchets, lints, link-graph, ledger check, ABI guard — all
hard-fail forever); ADRs 0015–0024; `libs/npwire` + the terrain_query seam +
the family groups; the witnessed render/lighting/env/terrain-query cores;
the historical twelve-workspace R/W/E/G record; the divergence-ledger discipline;
and this dashboard as the program's record.

Landed slices (hash per slice, newest first):

| Slice | Commit | Note |
|---|---|---|
| Simplification campaign, cite dedup (2026-09-01) | (this PR) | **`adapter_cpp_orig_cites` 570 -> 441 by DEDUPLICATION, not push-down** (ADR 0042 d7's ratified exception): 129 doc-comment markers on `simulation.h`, `weapon_database.h`, `env_file.h`, `item_database.h` and `simulation_members.h` binding declarations named addresses the engine already carries; they are `(engine: <group/lib/file>)` pointers now. 10 markers name an address the engine lacks (the ladder's relocation candidates) and 7 carry no address; both kinds stay. |
| Simplification campaign, GDScript (2026-09-01) | (this PR) | **`oversize_gd_files` 1 -> 0**: `game_world.gd` 2310 -> 1181 lines. The load-plan stage bodies moved verbatim to `world_load_stages.gd` (a WorldDeviceFrame-shaped lane; state stays on GameWorld), GameFramePipeline drives WorldDeviceFrame through `device_frame()` (22 leg delegators gone), the F3 debug views are reached through `debug_views()` (24 delegators, 100 caller sites), and the perf-counter composition sits in the device frame. Same slice: `Strings.TABLE_*` / `SECTION_*` + `lookup_or` replace 72 bare table/section literals and the HUD's has/get pairs, the attach-label match reads `Simulation.SEAT_*` (all six bound), MenuDriver hands an absent widget a throwaway state row, and the last 16 untyped `var x =` declarations / 2 unannotated functions in godot/game are typed. |
| Simplification campaign, round 2 (2026-09-02) | (this PR) | **Two new ratchet counters** in `scripts/lint/ratchet_counts.py`: `gd_dict_key_sites` (seed 1265) counts Dictionary-keyed reads (`x["key"]`, `.get("key"`) on code lines of godot/game + godot/modtools, godot/game/mcp excluded as the sanctioned JSON edge — the untyped-record surface ADR 0017 burns down; `godot_src_dictionary_returns` (seed 171) counts binding methods declared to return `Dictionary`/`TypedArray<Dictionary>` in godot/src headers (`to_json_value` converters excluded) — the seams still owed a RefCounted row (ADR 0042 d5). Same round: **`adapter_cpp_orig_cites` 441 -> 421 by code moves** — the kill-feed fold (`hud::feed_event_rows` + `hud::FeedRow`, ADR 0040 B3) with the 0x1E classification and STRCND key table moved from npwire into runtime/hud; the server-browser columns/filters/sort/details into `engine/net/novaworld/server_browser`; the weapon.def row half of `WeaponInstallData` into `world::weapon_install_data_from_def`, shared by the mission kernel and the `Simulation` binding. **Typed records replace Dictionary transports** (ADR 0042 d5): `NovaWorldServerRow`, `FeedRow`, `WeaponDef` + `WeaponSightRow` + `WeaponActionRow` + `ArmoryClassRow` (`WeaponDatabase.get_weapon` returns the record, the install seams take it, GUT synthetic defs are authored through it — no Dictionary factory survives); `gd_dict_key_sites` 1265 -> 1106, `godot_src_dictionary_returns` 171 -> 168. Later in the same round: `CharacterJoinProfile` (npruntime::character_join_vars owns the wire narrowing), `PlayerVisualSpec`, `FpViewmodelSpec`, the ItemDatabase leaves (`ItemParticleFx`, `ItemEmplacementAttachment`, `EnvsMarkerRow`, `ItemSeatCard` over `EntityCardSeat`), `HostSessionOptions` (the configure decoder deleted), `PlayerLocalView` / `PlayerAimOverlay` / `PlayerWeaponView` / `PlayerWeaponEvent` moved from GDScript decoders to native records, `ScarDrawList` (sim -> pass -> presenter); dead binding surface deleted (the avatar model bridge, the item palette enumeration, two redundant `to_dictionary`, `part_anim_step`, the precipitation Dictionary hop); `world/occlusion_feed.h` owns the building verdict word. The round's second half: `WeaponKitEntry` + `PlayerInventory`, `MissionInfo` + `MissionEnvironmentOverrides` (`env::bms_env_overrides_from_header` owns the BMS override fold), `ModelUserPoint` + `ModelLight`, `FoliageFrameStats` (one X-macro lays out the dispatcher's counter struct and the record), the eight per-frame HUD view records, `VehicleHudBlock` (HudPos -> HudOverlay without the Dictionary re-parse) + `MusicPairNames`, `DestructionDrain` with its four event rows, the ObjectData / ObjectModel read-back records (`MaterialInfo`, `PartAnimInfo` + `PartAnimTrack`, `RenderLodInfo`, `BodyBlendState`, `WeaponChannelState`, `PartAnimChannelState`; `get_summary` -> `get_lod_count`), `MissionPlacementStats`, `EffectLoadReport`, `UdpDatagram`. Then the F3 debug cards (`HitboxDebugReport`, `AiDebugReport`, `RoundDebugReport`, `RayDebugReport`, `OcclusionPortalReport`, `CollisionDebugReport`, `DebugPickCard` with their row records; the views key their rebuild signatures on record values), the native `EffectLightReport` (the GDScript decoder deleted) and `HudDrawListStats` + `MenuDrawListStats`, then `EffectSpawnRequest` + `EffectSpawnReceipt` (both directions of the particle spawn seam) and the test-facing `WacState` / `NativePoseStats` / `DestructionDebugCard`. Counters at close: `adapter_cpp_orig_cites` 418, `gd_dict_key_sites` 648, `godot_src_dictionary_returns` 95; five `dict_contract_allowlist` rows retired (the two `mission_setup_options.gd` profiles, `host_session_config.gd\|func to_session_options()`), plus `debug_arg_spec.gd\|func to_json_value()` added (the MCP transport edge of the typed action-argument schema). |
| Simplification campaign, round 4 (2026-09-02) | (this PR) | **The GDScript F3 debug views are gone; the ImGui dev tools (ADR 0039) are the whole F3 surface.** The nine world-space overlays (`ai/collision/hitbox/occlusion/particle/ray/round/skeleton/user_point_debug_view.gd`), `sim_debug_view.gd`, the pick highlight view, their two draw helpers (`mission_overlay_util.gd`, `object_user_point_overlay.gd`), the `DebugViewSet` owner with its status row, `AiDebugSession` and the two DevTools bridges (`ray_view_bridge.gd`, `physics_view_bridge.gd`) went with their eleven GUT files; `DebugControls` lost the thirteen `show_*` overlay rows (`hide_foliage` / `hide_particles` stay), `DebugPickSession` installs the surviving `PickClickCatcher` under the world itself, and the `debug_view_set.gd|func get_ai_view_state()` dict-contract row retired. Caveat recorded in ADR 0039: the ImGui windows have no world-space overlay yet, so those drawings return only as engine windows (§6). `gd_orig_cites` 699 -> 695 (cites that died with the views), `gd_dict_key_sites` 324 -> 310. Before it, the dead-code sweep: the census's 98 "unreferenced header accessors" were X-macro-bound record properties read as `.field` from GDScript and STAY (rule: a record's field set is its contract); what actually died was one zero-caller GDScript pair (`get_destruction_present_stats`), 21 bound-but-never-called binding methods plus 13 bind lines whose C++ method is driven natively, eleven signals nobody connects (MusicDirector's `vm_error` became a `push_warning`), and the triple `GDREGISTER_CLASS(GlowSpawn/ModelLightSpawn)` that logged "already registered" in every GUT run. `get_meta_list` joined the `godot_node_meta_sites` regex; the stale `effect_light_report.gd` allowlist row went. With the views gone, the native residue followed: the DevTools view-toggle plumbing (the ray / physics toggles, the `ai_view_request` signal and its state provider, the `SetViewShown` request kinds and the `view_shown` / `boxes_drawn` snapshot fields, the AI window's overlay strip) and the four view-only record families (`CollisionDebug*`, `RayDebug*`, `OcclusionPortal*`, `AiDebug*` with their `Simulation` accessors); `get_hitbox_debug` / `get_round_debug` stay as GUT oracles and `native_ai_debug` / `native_rays_snapshot` / `native_physics_snapshot` keep feeding the windows. `MusicScript.compile_text` (a Dictionary-returning compile seam whose one caller was a GUT case the engine ctest `mus_names_roundtrip` already pins) died: `godot_src_dictionary_returns` 79 -> 78. **Consolidation:** `godot/src/util/color_convert.h` is the one packed-colour <-> `Color` home (eight converter copies and ten inline decodes collapsed; the CBIN writer's truncating form and the particle renderer's multiply form stay by design) and `env/env_axes.h` moved to `util/axes.h` carrying the one `bms_to_godot_basis` wrapper (`PresentApplier`'s twin died; the placer's bound face forwards). `ItemDatabase` indexes its retained `DefItemsFile` parse (`index_` + `sorted_ids_` + `row_()`) instead of copying every row into a private `Item` mirror; the `ReplicationDefinitionRecord` copy and the `item_replication_catalog_adapter` TU died for the engine's `ItemReplicationCatalog::from_items_def`: `adapter_cpp_orig_cites` 418 -> 415 (three comments that duplicated `formats/def/def.h`'s witness). **`MnuDocument`'s authoring half is gone** (ONED is run-only, ADR 0037; no shipping caller touched a mutator): the presence-aware authoring state and patch, the item / table / appearance / frame mutators and their Dictionary readers, `add_widget` / `delete_widget`, the snapshot / reparent state TU, `create_empty`, `get_menu_size` and `touch()` -- 35 of 68 bindings, mnu_document.cpp 2017 -> 696 lines, the driver's one authoring read replaced by a typed `is_widget_multiselect(id)`; `godot_src_dictionary_returns` 78 -> 70, `gd_dict_key_sites` 310 -> 308, `adapter_cpp_orig_cites` 415 -> 414 (the menu-size extent's cite died with it). The three surviving MnuDocument Dictionary readers became records: `get_widget_sounds` -> `TypedArray<MnuSoundRow>`, `get_widget_actions` -> `TypedArray<MnuActionRow>` (with a test-side `make()`), `get_item` -> `get_item_text` / `get_item_value`; the driver's action dispatch and sound relay read typed fields: `godot_src_dictionary_returns` 70 -> 67, `gd_dict_key_sites` 308 -> 297. The placer's two remaining Godot-free rules (`needs_individual_node`, the runtime-type -> visual-item resolution) moved into `runtime/mission/placement_traits.h` with ctest pins. **GDScript:** `Strings` is the one menu-text home (`menu_text(key, fallback)` = menutxt/Menu then gameui/Menu, the table and section names as constants); the four private copies in the armory and player-info companions, the DEATH-screen presenter and the loading screen collapsed onto `lookup_or`: `gd_orig_cites` 695 -> 694 (the DEATH-screen copy's `@0x5536a0` duplicated `runtime/world/deploy_screen_feed.h`). The loading screen's `info` Dictionary is a `LoadingScreenInfo` record (mission file, session flag, the session strings, the game type; `for_mission` / `make`) built by the shell's SP starts, both net-session starts, the joiner's post-auth `join_session_identified` payload and the stage probe, and `resolve_background` returns a `BackgroundPick`: `gd_dict_key_sites` 297 -> 281. |
| Simplification campaign, round 3 (2026-09-02) | (this PR) | **Node metadata is gone from `godot/` and a new absolute-zero floor keeps it out**: `godot_node_meta_sites` in `scripts/lint/ratchet_counts.py` counts `set_meta` / `get_meta` / `has_meta` / `remove_meta` calls over every `.gd`/`.cpp`/`.h` under godot/ (bindings, game, modtools, probes, tests) and, like `mcp_boundary_cites`, has no baseline key. The eight metadata families it retired: the ambient mixer's `ambient_candidate_id` (a duplicate of `Channel.candidate_id`; two public read seams replace it) and SoundBank's never-read `layer_params`; the `_opennova_auxiliary_draw` flag (the pair is a registered `PostMultiplyDraw` node class); the eleven `static_batch_*` / `static_shadow_*` metas on placer populations (a registered `StaticPopulationInstance : MultiMeshInstance3D` carries population kind, RLOD level, bin coordinates, the slot identity arrays and the live row -> slot map as typed properties; `static_shadow_batch_key` had no reader and died); the `avatar_part` / `character_id` / `avatar_graphic` / `avatar_camo` tags (a bound `ObjectModel.AvatarPart` enum plus `character_id`, `avatar_camo` and `graphic_name` properties); the `entity_ref` Dictionary (an `EntityRef` record on `ObjectModel.entity_ref`; the placer's `{model, ref}` `placed_entity_records` Array became `placed_models` and `EntityIndex` builds from the models' refs; the never-written `net_id` key and its dead branch went); the `_opennova_postmultiply_material` proxy on ShaderMaterial (a per-index cache entry returned by `create_material`). `gd_dict_key_sites` 648 -> 628 from the directors, probes and presenter reading `EntityRef` fields. **Then the round's record slices** (ADR 0042 d5, one commit each): the static-source rows (`StaticUserPointSource`, `StaticEffectSource`, `StaticLightDrawSource`, `StaticTerrainShadowSourceRow`), `EffectSpawnOptions` (the GDScript spawn-request options), `EffectOwnerPoseBatch` (owner poses cross the seam once per tick), the DEATH-screen rows (`DeployZoneRow` / `DeployOccupantRow` / `DeployListRow`), the end-round table (`EndRoundColumn` / `EndRoundRow`), `AttachLabelRow` + `FriendlyTagRow`, `CoronaRow` (`Simulation.predict_mount_seat` deleted: the engine ctest covers it), `RoundOutcome`, `WeaponProfileSummary` + `WeaponProfileSide`, `TracerRibbonFrame` + `TracerRibbonStrip`, `EnvDayPhase` / `EnvSunGlare` / `WeatherHomeState` / `NovaWorldServerTotals`, `MenuWidgetState` (MenuDriver's per-widget override state with `has_*` latches plus a `WidgetInfo` inner class), the nine per-tick present drains in `simulation/present_event_records.h` (`ThrowableVisualRow`, `FirePresentationEvent`, `FireSoundRow`, `SlotSoundRow`, `SoundEmitterRow`, `RoundImpactRow` with `has_light`, `TerrainScorchRow`, `WeatherSoundRow`, `ChatLineRow`, `MissionEffect` with `make()` and `wire_handle` -1 outside the vehicle_control_* kinds), `ObjectiveRow` / `DeathPieceRow` / `RoundGlowRow`, the LightScene spawn requests (`GlowSpawn` / `ModelLightSpawn` with `make` + `owned_by` / `fading` / `masking` / `attached` / `in_blink_box` chains replacing the config Dictionaries), `LanServerRow` (the LAN browser rows through `get_servers` / `servers_changed`, `JoinTarget.from_lan_row` and the spectator preflight), and PlayerViewEffects' four view facts as typed `update_view` arguments. **GDScript bookkeeping typed** as inner classes: ThrowablePresentPass (`ModelSlot`, `MoveEffect`), the HUD message queue (`PendingHudMessage`), the destruction pass's wreck anchors (`WreckFire`; the bms_id / spawn_origin / pos values were only existence-checked), the item-effect director's controller registries (`ControlNode`, `ControlInstance`, `PendingNode`), the user-point overlay's `Marker`, the effect-light director's `SpawnedNode`, PerfTimeline's `Span`, DeployScreenPresenter's `SpawnRow`. **Dead surface**: `ObjectData.get_materials()` (a 60-line Dictionary dump of every MTRL row rebuilt per model, plus five Dictionary helpers) and ObjectModel's `material_defs_` map / `build_material_defs` / `load_texture_for_slot` -- the material builder and `veg_assets` read rows through `find_material_array_index` + `load_material_slot_texture`, and a rebuild no longer resolves every texture path of every material. Named constants for the chute-flap / freefall SndProf slots and PerfTimeline's unit scales. Counters at close: `gd_dict_key_sites` 628 -> 324, `godot_src_dictionary_returns` 95 -> 79, `adapter_cpp_orig_cites` 418 (unchanged); judged not worth converting: the `MnuDocument` row Dictionaries (lossless ONED round trips), `LwfData`, the diagnostic JSON aggregates, the persisted player-profile / armory-loadout JSON shape, MissionData's 27 converters (the streamed placement rows feed the same `place_entities` reader). |
| Simplification campaign, push-down (2026-09-01) | (this PR) | **`adapter_cpp_orig_cites` 596 -> 570 by code moves**: the water strip's camera view build (`env::water_strip_view_from_camera`, 8 cites), the attach-label gates and the USEGUN text key (`MissionKernel::collect_attach_labels` / `AttachLabel::attach_text_key`), the weapon.sav defaults + header gate + class clamp (`playersav::profile_or_defaults`), the water-glint frame (`env::advance_water_glint`), the HUDDECLUT table build (`hud::declutter_from_hudpos` over the retained `DefHudPosFile`), the cull delta (`world::culled_changes_since`) and the weapon-slot predicate (`world::weapon_slot_indices`, with `WeaponDatabase` now retaining its `DefWeaponsFile` like `ItemDatabase`). Same campaign, no counter: `io/hash.h` (one FNV-1a for the page-cache stamps), `io/tick_rate.h` (62.5 Hz and the integer 62 named apart), `io/fixed.h`'s named scales and saturating binding converters, `strutil::to_upper`, `to_packed_bytes`, one `mission_to_godot`, the BMS cursors on `io::ByteReader/Writer`, and the named ChangeAI subs, input direction bits and joiner sync phases. |
| Simplification campaign, dead code (2026-09-01) | (this PR) | **`adapter_cpp_orig_cites` 600 -> 596** by verified dead code (the docstring's "died" clause): `Simulation::get_end_round_lines` (`draw_endround_stats_overlay @0x5b7cd0`), `MissionData::get_item_availability` (`build_item_restriction_table @0x54DDB0`) and `sync_local_player_damage_classes` (a twin of the kernel's `local_loadout_sync_damage_classes` call) had no caller in any `.gd`, `.tscn`, probe, test or C++. Same sweep: ~20 zero-caller engine helpers, 15 unbound godot/src members, 2 dangling declarations, 11 never-called `D_METHOD`s, 9 GDScript functions, 1 signal and 7 consts. Rule recorded for witnessed-but-unwired helpers: a cited zero-caller helper is deleted only when its address is in a tracked RE record (applied to `remove_devices_by_owner`, `emplaced_control_phase`, `is_player_spawn_marker_type`). The 323 engine / 104 GDScript test-only functions were bucketed, not deleted: staged ports carrying a cite or a `STAGED, NOT WIRED` header and ADR 0018 read seams (the devtools window accessors, the `*_for_test` family) stay; the uncited clusters (`tp_camera_mount.h`, `vehicle_part_anim.h`, `impact_scar.h`, `occlusion.h`, `collision.h`, `batch_chunker.h`) are the later audit. |
| Quick knock-outs (2026-08-29) | (this PR) | `citation_allowlist_engine` 16 -> 14: `io` deleted (header-only, no `.cpp` for the counter to see — the entry could never fire) and `scr` deleted (`engine/formats/scr/scr.cpp` carries its `[orig:` anchor). `engine_uncited_src_files` stays 0. Same slice: the twelve UI "register candidate" rows + D-THROW-2 + D-COL-7 ratified `PERMANENT` (ADR 0022 register), D-WPN-29 and D-NET-166 `FIXED`, D-AI-8 facet (e) closed; the three env-tod `VERIFY-pending` cites retired by decompile; the GSB/LAN server-name rows decode cp1252. |
| Hygiene round after #562-#582 (2026-08-28) | (this PR) | **`engine_uncited_src_files` 20 -> 0, `citation_allowlist_engine` accepts engine-relative path prefixes** beside bare lib names: `net/novaworld/service` (the NovaWorld service: witnessed by captures and retail logs, no `Jointops.exe` counterpart), `net/netsim/loopback_channel.cpp` + `net/netsim/udp_session_transport.cpp` (transport shims), `formats/lwf/wav_pcm.cpp` (public RIFF/WAVE + IMA-ADPCM, the pcapio precedent), `formats/threedi/threedi.cpp` (the chunk walker) and `formats/threedi/threedi_3di3_ground_anchor.cpp` (asset-derived, not an address), `formats/wac/command_table.cpp` (a name-lookup helper over the cited table), `net/npwire/replay/wire_capture.cpp` (the capture-side harness). Three files got the marker they were missing (the WAC command table, the /PROFILE serverlog decoder, the MUS decompiler); `session_protocol.cpp` cites the retail protocol-name check. `has_method_guards` and `test_private_pokes` now scan `godot/probes/` too: the `MainGame` private pokes there are gone (the perf probes read `MainGame.get_perf_probe_switches()`), and the 22 `has_method` guards of the render-fixture contract are BANKED as the new floor (0 -> 22, fail-on-increase) rather than burned: `render_fixture_capture_probe_test.gd` drives that contract through test doubles (`ComparisonWorld`, `ComparisonSim`, `CaptureTransactionGame`) that cannot be the native `Simulation`, so typing those seams needs an explicit interface script first (ADR 0018's answer for `GameFramePipeline`'s `FakeWorld` too). `engine/runtime/renderer` and the cbin/def/mns/mnu format libs joined the `opennova` namespace (engine/CLAUDE.md's rule is true of the whole tree). |
| Post-merge tidy of #576-#580 (2026-08-27) | (this PR) | **The IDB and the tree in agreement again, with the tool that keeps them so**: `scripts/ida/cite_sweep.py` joins every IDB `reimpl:` link against the markers inside the function it annotates (`reimpl-moved`, `reimpl-orphan` beside `reimpl-stale`) and repairs them (`--fix-reimpl --apply --save`; bulk replies through a temp file past the ~1 KB py_eval cap); 73 tool + 28 hand reverse-link rewrites, 43 retired `Nova*` names in comment prose renamed, eleven code cites brought to the IDB names, four ports re-marked; 22635 markers / 1832 links / 0 disagreements. `orphan_header_allowlist.json` burned to zero (threedi_compare to tests/threedi/, terrain/types.h deleted, serializing_sink.h STAGED with its owner); `engine_uncited_src_files` 21 -> **20**; the two dead `item_seat_specs.gd` dict-contract rows dropped. Three censuses (docs/agent files: 46 findings; claims vs the tree: 117 checks, 32 stale; IDB/code) drove the docs: skills, CLAUDE.md files and runbooks against ADR 0038-0040, correspondence.md and every RE record path, the ADR 0040 ladder as the push-down queue (13 of 19 slices landed), TODO.md premises deleted by ADR 0038, 19 ledger paths, plan/status.md last `[x]` rows. |
| One engine include root + the push-down campaign (ADR 0040, 2026-08-26/27) | PR #580 | [ADR 0040](adr/0040-the-engine-is-one-namespace.md): `engine/` becomes the ONE public include root with group-qualified includes (`<runtime/world/player_view.h>`), and 308 pure renames retire the `nova_` file prefix (`NovaWorld*`, `NovaLogic` and `opennova*` survive as proper nouns). The same PR ran the push-down campaign: A1 `local_player_view`, A2 the minimap marker rows + feed layout (`net/npruntime/minimap_markers`), A4 `LanDiscoveryBrowser` (`net/npruntime/lan_discovery`), A5a the boot/pack/options policies, A5b the seat mirror + volume law + weapon-category rows (`runtime/world/vehicle_attach`, `runtime/audio/volume_law.h`, `runtime/controls/controls`), B3a `occlusion_camera`, B3b `iris_march`, C1 `mission_load_plan`, C2 `hud_config_tokens`, C3+C4 `presentation_frame` + `fp_viewmodel_spec`, C5 the light-director spawner constants (`renderer/light_scene.h`), C6 the loading screen's names, sidecar and due rules, C7 `character_registry` + `join_character_profile`. **Banked `adapter_cpp_orig_cites_pushdown` 364 → 341** and **the new `gd_orig_cites` gauge 760 → 733** (minted at 760 in the same PR beside `oversize_cpp_headers` at 1, `simulation/simulation.h`) |
| In-engine dev tools, F3 overlay hard cut (ADR 0039) | PR #579 | `engine/runtime/devtools` joins `citation_allowlist_engine` (infrastructure, not a port — the same class as io/vfs/pcapio); the GDScript overlay's 190-slot board and Stats page moved into the engine, the overlay shell, page framework, snapshot writer and their tests were deleted, ONED's Control scene became the engine's `OnedUi` surface; **banked `test_private_pokes` 99 → 95** (the deleted overlay tests) |
| ADR 0038 fix-up: lints restored, FFI proofs retired | PR #578 | `scripts/lint/*` restored on a bare setup-python after the asset-pipeline cut; **banked `engine_uncited_src_files` 25 → 21** (ase_parser/ase_writer/mission_capi/oed.cpp deleted) and **`adapter_cpp_orig_cites_pushdown` 365 → 364**; orphan allowlist −4 (the FFI/ASE headers are gone); `abi_export_identity` retired with `opennova_shared` |
| Post-merge tidy of #553–#573 (2026-08-25) | (this PR) | **New enforcement instrument `scripts/lint/orphan_header_check.py` (CI, hard-fail)** — an `engine/**` header included by nothing outside `tests/` and its own unit fails unless it opens with the `STAGED, NOT WIRED` paragraph naming its live owner or carries a reasoned row in `scripts/lint/orphan_header_allowlist.json` (the flat C ABI headers the FFI consumes; the 2026-08-25 census debt: `bit_stream.h`, `ase_parser.h`/`ase_writer.h`, `serializing_sink.h`, `terrain/types.h` — burn down, never grow). The #554 round found eleven such headers by hand and #553–#573 shipped four more; the check would have caught every one. Same slice: `ratchet_counts._in_build_dir` narrowed to a top-level `build*` (or `godot/build*`) directory — the `any segment starting with "build"` form silently excluded any deeper source dir so named from five counters (counts unchanged); `test_private_pokes` 109 → **100** (the `oned_app_test.gd` pokes replaced by `OnedApp` seams); the 371 → 370 pushdown bank's owner named (#564) in the #565 row below. |
| ONED run-only hard cut (2026-08-24) | (this PR) | Retired the workspace/editor implementation and banked the resulting ratchet reductions: **`engine_uncited_src_files` 49 → 43**, **`has_method_guards` 1 → 0**, **`oversize_gd_files` 6 → 1**, **`test_private_pokes` 1260 → 109**, and **`adapter_cpp_orig_cites_pushdown` 370 → 369**. Removed the `dep`, `oned_edit`, and `refs` citation allowlists and every deleted editor dictionary exception, including `editor_game_packer.gd\|static func pack_for_retail(`. The retained packer exceptions moved to `game_packer.gd`; its transport-edge `static func stage_retail(` and the process-session snapshot `game_run_session.gd\|func get_state()` are explicitly allowlisted. |
| Play-in-Retail process seam (2026-08-24) | #565 | **`adapter_cpp_orig_cites_device` baseline 195 → 196** — one `(retail: PFF_OpenAllArchives @0x4a4310, see docs/vfs/vfs-pff-mount-re.md)` note in `godot/src/util/process.h`: the working-directory contract of the retail spawn seam exists because retail opens its boot archives CWD-relative through a raw `_lopen`; the note documents why the binding must set the child's CWD rather than inherit ONED's. A documented device fold over the VFS record's witness, banked per the counter's contract. Same slice: **`adapter_cpp_orig_cites_pushdown` baseline 371 → 370** — master already counted 370: the decrease was #564's (its body claims the bank, but the slice never ran `--write-baseline`); banked here per the counter's contract, this row standing in for #564's. There is ONE 371 → 370 decrement, owned by #564 and banked by this slice. Same slice: `dict_contract_allowlist` +`editor_game_packer.gd\|static func pack(`, +`static func pack_for_retail(`, +`static func stage_loose(`, +`static func export_game(` — the packer's results are a transport edge throughout: the `pack_game` MCP tool's JSON payload verbatim, the `--pack-game` CLI's printed summary, and the session's injected `pack_retail` Callable seam, which by design carries no class dependency on the packer (ADR 0025's one-managed-child session stays UI/packer-neutral). |
| Wire-up of the staged modules (2026-08-21) | (this PR) | **`adapter_cpp_orig_cites_device` baseline 173 → 187** — the device legs this PR adds carry their seam contracts as `(retail: Name @0xADDR, see docs/...)` notes in `godot/src`: the terrain light pool (`light_scene`/`terrain`: the rows texture, the clamp/bilinear/no-mip texture state `@0x5a98eb..0x5a98f4`, the pre-fog sum), the scar presenter (`scar_presenter`/`simulation_scars`: the view-space pull in place of a z-bias the retail drawer lacks, the section-mask owner gate, the axis swap), the mounted-camera carrier feed (`simulation_player_view`: the yaw-only `carrier_forward`), the round-effect liveness row, and the HUD device legs (`hud_overlay`/`simulation_hud_feeds`: the vehicle panel's whole-panel gate on the loaded interface texture `@0x5a5038`, the chat wrap width from the authored HUDCHATTEXT pair as retail's `g_hudChatBoxCoords` rows, the zone panel's blink on the 62 Hz HUD tick, the v4 minimap snapshot's medic column). Each is a documented device fold over a record-owned witness, which is what the non-zero floor exists for; banked per the counter's contract (181 after the first four legs, 186 once the HUD device half merged, 187 with the review's own-zone tile note — `lfp_dlf.tga` `@0x59e11a`). |
| Static terrain-shadow light feed + near clip (2026-08-22) | (this PR) | **`adapter_cpp_orig_cites_device` baseline 194 → 195** — one `(retail: ...)` seam note in `godot/src/terrain/terrain.cpp`: the page path consumes the RAW `Environment_GetLightDirectionFloat @0x57D870` tuple (the collector reads `@0x60D2F5/0x60D2FF`, the tile DOT3 pack `@0x60E231..0x60E331`), never the Godot-axes vector — the note documents why the 2026-08-20 getter change must not reach this consumer. |
| Entity ground-shadow extent bounds (2026-08-22) | #558 | **`adapter_cpp_orig_cites_device` baseline 187 → 194** — the seven `(retail: ...)` seam notes the drape's patch + depth-clip port adds in `godot/src/env/slot_shadow.{h,cpp}`: the terrain height probe the anchor march reads (`Terrain_GetHeightAtPosition @0x606720`), the generated `shadowztex` texture (`shadow_system_init_resources @0x5d6260..0x5d62d7`), the two-radius source — the model sphere for the capture extent and depth clip vs the entity bound for the lod and light query — with the render-bounds fallback (`Entity_InitFromModel @0x40dc30`, `RenderSlot_AllocSlot @0x5d5773`, `RenderSlot_RenderEntityAndChildren @0x5d7835`, twice: the candidate build and the `CasterInfo` field), the per-bound-slot dominant-light pick the blob leg now shares (`RenderSlot_UpdateEntityLight @0x5d6a30`), the stored slot direction the grazing rescale and the depth clip read (`@0x5d6d5c`, `RenderSlot_DrawSilhouetteDrape @0x5d5d66`), and the capture half-extent's gpm[5] source. Each is a documented device fold over render-lighting-re.md's witness. |
| Post-merge tidy of #519-#528 (2026-08-21) | (this PR) | **`adapter_cpp_orig_cites_device` now counts `(retail:` beside `[orig:`; baseline 122 → 173** — the adjudicated `(retail: ...)` form (godot/src/CLAUDE.md) decremented the device floor on every conversion (four `glare_occlusion.*` notes in #523 left the counter at 118 against a 122 baseline: four units of deletable slack), defeating the "must not shrink" contract; the counter now sees both markers and the true number is banked (174 at the re-count, 172 after this PR moved the slot refresh-mask and first-person drape notes into engine/, 173 once master's 2026-08-21 `HUD_ParseHudposToken` note in hud_overlay.cpp merged in — the first `(retail:` addition the counter has seen land concurrently). Same pass: `dict_contract_allowlist` +`effect_light_report.gd\|to_json_value` (the #519 typed-seam pass tightened a `-> Variant` dodge to an honest `-> Dictionary` JSON projection — log row added retroactively) and the two entries #519 added for `render_capture_variant.gd`/`shadow_caster_diagnostic.gd` DELETED: that commit moved both files to `godot/tests/support/`, outside the lint's scope, so the rows could never match. |
| Post-merge review bank (2026-08-19) | (this PR) | **`engine_uncited_src_files` baseline 50 → 49** — the #503–#518 train retired one uncited engine source without banking the decrease; banked by the post-merge review per the counter's contract. |
| Viewmodel-parity tails push-down (#510, log row added retroactively 2026-08-19) | `6d7699bdd` | **`adapter_cpp_orig_cites_pushdown` baseline 372 → 371** — #510 banked the decrease via `--write-baseline` but the log row this table requires was missed; recorded by the post-merge review. |
| EffectWorld authored-light push-down (2026-08-17) | (this PR) | **`adapter_cpp_orig_cites_pushdown` baseline 373 → 372** — the obsolete Godot object-adapter citation claiming gameplay left model-authored LGHT inactive was removed after the authored-light lifecycle moved into the portable `engine/runtime/renderer` EffectWorld pool; the adapter now only guards the editor-preview-only local-light route against double injection. The decrease is banked per the ratchet contract. |
| Render-reflection policy push-down (2026-08-16) | (this PR) | **`adapter_cpp_orig_cites_pushdown` baseline 374 → 373** — the pool-2-building/vehicle reflection decision and its retail witnesses moved from the Godot mission/object adapters into `engine/runtime/mission/placement_traits.h`; the adapters now consume the typed policy and only map it to Godot layers. The decrease is banked per the ratchet contract. |
| Hygiene pass: the cite-ratchet split (2026-08-11) | (this PR) | **`adapter_cpp_orig_cites` (497) split into `adapter_cpp_orig_cites_pushdown` (375: simulation/object/mission — the burn-down class, can reach zero) + `adapter_cpp_orig_cites_device` (122: the ADR 0035 device-leg seam contracts — must not grow, floor NON-ZERO by design)**. Total preserved exactly; ends the chase-to-zero misreading of the single number. Same pass: `maturity_lint` dict-contract scope restored to `godot/game/` (the #460 rename pointed it at the now-C++-only `godot/src/` and silently dropped the game layer), `cpp_binding_console_writes` widened to raw CRT writes, `host_lint` scan gains `tools/`/`deploy/`/`infra/`/`fixtures/`, `include_graph_check` gains `engine/runtime/world`, `abi_exports_check` no longer filters new export families out of the compare set |
| .wav decode push-down (adapter shape C4, #455) | (this PR) | **`engine_uncited_src_files` baseline 50 → 51** — the new `engine/formats/lwf/src/wav_pcm.cpp` parses PUBLIC formats (RIFF/WAVE + standard IMA-ADPCM; the pcapio precedent), so an `[orig:]` citation is inapplicable by construction; the per-lib allowlist would exempt the witnessed `lwf.cpp` too, hence the bump. MAINTAINER RATIFIES ON MERGE |
| Loadout 0x2F push-down (ADR 0031 PR E) | (this PR) | **`adapter_cpp_orig_cites` baseline 602 → 580** — the profile-seed / side-change / 0x2F-composition citations moved to `npruntime/loadout_submit`; the S7b note in `joiner_world_bridge.h` rewritten to record both ADR 0031 re-opens |
| Assets-sweep push-down (ADR 0031 PR D) | (this PR) | **`adapter_cpp_orig_cites` baseline 630 → 602** — the collision/seat resolution sweep and its citations moved to `engine/runtime/simassets` (with #446's projection move banked in the same step); the decrease is banked per the counter's contract |
| Adapter composition contract (ADR 0031) | (this PR) | **New ratchet counter `adapter_cpp_orig_cites`, baseline 630** — `[orig:` citations across `godot/src` C++ (generated `build/` excluded). Witnessed engine behavior belongs in `engine/`; an adapter citation is either a documented seam contract (the S7b-shape bridge notes) or a push-down candidate, so a new one needs a deliberate baseline bump logged here, and push-down slices bank the decrease via `--write-baseline`. Makes the "thin wrappers only" contract (adapter CLAUDE.md, ADR 0016) measurable for the first time |
| Engine group-target collapse (ADR 0029, "Shape A") | (this PR) | **New enforcement instrument `scripts/lint/include_graph_check.py` (CI, hard-fail)** — ADR 0020's terrain seam re-homed at include level (files under `engine/net`, `engine/runtime/wac`, `engine/runtime/mission` may include only terrain_query's four `terrain/` headers, never the cpt/til/trn/tpj/foliage stack; since widened to four trees — `engine/runtime/world` joined them — and five headers under the ADR 0040 prefix `runtime/terrain_query/`), because the collapsed group targets can no longer carry the edge; `link_graph_check.py` keeps the surviving target-level rule (sqlite links into `opennova_novaworld_service` alone). The ~53 STATIC + 3 INTERFACE per-domain targets and both LIBS-2 family groups (`engine/families.cmake`) collapse into five STATIC group targets + the `opennova_io`/`opennova_oned_edit` INTERFACE pair; every consumer relinked in the same change, no aliases. `abi_export_identity` baseline byte-identical (the whole-archive list names group targets now; only `OPENNOVA_API`-annotated symbols export); ratchet counters path-keyed, +0 |
| Push-down residue: the sub-weapon walk | (#437) | **`abi_export_identity` baseline 107 → 108:** `def_subclass_weapon_index` joins the PLAYER_INFO family (the round-type walk behind `*_AMMO2` `[orig: @ 0x55def0 / @ 0x55e8b0 / @ 0x55f1f0]`), exported under the def lib's flat-C convention |
| Loadout-menu policy push-down (S19) | (this PR) | **`abi_export_identity` baseline 106 → 107:** `def_extra_ammo_weight` joins the PLAYER_INFO/armory weight family beside `def_loadout_weight` (the category-3 extra-ammo term `[orig: @ 0x5655c9..0x56561c]`), exported under the def lib's flat-C convention |
| Local-player TU split (S6a, ADR 0028 trunk) | (trunk PR) | **Banked `oversize_cpp_files` 2 → 1**: `simulation_player.cpp` (3,150) split into the core + `_view`/`_weapon`/`_loadout` TUs (max 1,322 lines) — a pure move; the residual floor member is `http_listener.cpp` |
| engine/ directory move (ADR 0028) | (trunk PR) | **Ratchet keys renamed, values carried verbatim:** `libs_uncited_src_files` → `engine_uncited_src_files` (50), `libs_stdout_prints` → `engine_stdout_prints` (0), `citation_allowlist_libs` → `citation_allowlist_engine` in `maturity_baseline.json`; `ratchet_counts.py` walkers re-rooted from `libs/` to the grouped `engine/<group>/<lib>` layout. No counter values changed |
| 3DI3-only model pipeline (ADR 0027) | (this PR) | **`abi_export_identity` baseline reshaped:** the six `threedi_ir_*` exports and `tdp_from_ir` are removed with the `ThreediModelIR` layer; `threedi_3di3_read`/`threedi_3di3_free` are exported for the Python FFI mirrors and `tdp_from_3di` replaces `tdp_from_ir`. Net −5 exports; the model surface the DLL exposes is now the parsed `Threedi3di3` itself. **Also banked `libs_uncited_src_files` 55 → 50** — the five deleted uncited threedi TUs (the IR core/converters and the GP reader/writer) leave the library with only its format-record-backed sources |
| The joiner-side between-update movers | (#403) | **Banked `has_method_guards` 567 → 566 and `libs_uncited_src_files` 56 → 55** (the final review's typed-seam conversions + netsim citations). Recorded here after the fact — the slice banked the decrements in `maturity_baseline.json` without a dashboard row |
| MNU parity + menu editor deepening | (#387) | **Banked `has_method_guards` 568 → 567 and `libs_uncited_src_files` 57 → 56.** Recorded here after the fact — no dashboard row landed with the slice |
| MP wire present-pass hot walk native | (#389) | **Banked `has_method_guards` 579 → 568** (the GDScript walk's duck-type guards retired with the native port). Recorded here after the fact — no dashboard row landed with the slice |
| Standalone game debug runtime | (#376) | **Banked `test_private_pokes` 1353 → 1313 in `maturity_baseline.json`; recorded here after the fact** — the PIE→standalone replacement touched 63 paths under godot/tests (37 rewritten, 8 deleted, 18 added, incl. sidecars/scenes — probe scenes and debug-page tests moved onto the new session/MCP seams), and the slice landed the decrement without a dashboard row (the same gap the #377 row below records) |
| Retail control-register catalog completion | (#377) | **Banked three counters in one slice:** `has_method_guards` 585 → 579, `libs_uncited_src_files` 61 → 57, and `test_private_pokes` 1363 → 1353. Recorded here after the fact — the slice banked the decrements in `maturity_baseline.json` without a dashboard row |
| Infantry death-transition fidelity | (this PR) | **`has_method_guards` baseline 578 → 585, with each +1 remaining a real dynamic seam:** three preserve legacy/test skeletal providers while the new two-channel pose calls are optional, one preserves avatar fallback nodes without skeletal animation methods, one preserves revisionless/clockless presentation sources, and two classify heterogeneous wire-visual nodes for the new blend/tick methods. The three newly bound `Simulation` local-player getters are deliberately unguarded after W4-2; this adjustment records capabilities that cannot be proven from the receivers rather than restoring typed-receiver defensive guards. |
| Quality W4-6 modtools splits (the W4 closer) | (this PR) | **New ratchet counter `oversize_gd_files`, baseline 7** — the counter the campaign plan schedules to land with the last W4 slice: no `.gd` under `godot/engine`, `godot/game`, or `godot/modtools` may grow past 1,200 lines without splitting first (`godot/tests` is deliberately out of scope — eleven test files already exceed the limit, and the test refit is ONED-TST's concern, not this ratchet's). The baseline is the residual floor the W4 god-file splits leave behind: `game_world.gd` (2,295 — the load path stays whole by design after the W4-3 extractions), `editor_workstation.gd` (1,533), `terrain_editor.gd` (1,446 — the W4-6d split moved every movable method bundle; the residual is the workspace facade: the flat accessor band into document/brush-session, lifecycle, input dispatch, and 33 load-bearing delegates), `mission_object_placer.gd` (1,389), `music_editor_document.gd` (1,306), `mnu_canvas.gd` (1,243), and `editor_mcp_menu_tools.gd` (1,223). Shrinking any below the threshold banks a −1 via `--write-baseline` in the slice that earns it. (That enumeration is the mint-time snapshot; the live authority is `scripts/lint/maturity_baseline.json` — as of 2026-08-11 the baseline is 6: `game_world.gd`, `editor_mcp_menu_tools.gd`, `editor_workstation.gd`, `terrain_editor.gd`, `mnu_canvas.gd`, `music_editor_document.gd`, with `mission_object_placer.gd` gone native in the #460 rework) |
| Quality W4-2 guard floor | (this PR) | **New ratchet counter `has_method_guards`, baseline 578 — a floor, not zero:** the kept set is the documented duck-type seams (GameWorld/LocalPlayerPresenter harness contracts), the modtools workspace capability hooks, and dynamic-name dispatch. The ~157 deleted were typed-receiver guards on methods proven bound/defined (+7 `has_signal` guards on declared signals, outside the counter). Shrinking the floor banks a decrement via `--write-baseline` in the slice that earns it |
| Quality W3-7b sim headers (the W3 closer) | (this PR) | **New ratchet counter `oversize_cpp_files`, baseline 2** — the counter the campaign plan schedules to land with the last W3 slice: no `.cpp` under `libs/`, `apps/`, or `godot/engine` may grow past 2,500 lines without splitting first. The baseline is the residual floor the W3 god-file splits leave behind: `godot/engine/simulation/simulation_player.cpp` (2,877) and `apps/novaworld_server/http_listener.cpp` (2,646 — its 2,230-line `start()` was decomposed in W3-4, but the TU keeps 34 route handlers). Shrinking either below the threshold banks a −1 via `--write-baseline` in the slice that earns it |
| Weekly audit: citation detector exact-match + boilerplate sweep | (this PR) | **`libs_uncited_src_files` detector tightened from the substring `[orig` to the exact citation form `[orig:`, baseline 60 → 61.** The +1 is `libs/oed/src/rdta.cpp` finally counted honestly: its only `[orig` hits are the `remap[orig]` array indexes — CODE, not citations — so the substring test read an uncited file as cited. The six W3-2 collision TU headers also carried the same `[orig]` boilerplate sentence #349 reworded for mission/threedi; reworded here so no file can ever satisfy the gate on comment text again (all six carry real citations regardless). Also banked: `test_private_pokes` 1378 → 1366, earned by #342's public-seam probes and deliberately left unbanked by the W3-3b slice |
| Citation ratchet measures citations, not comments | (this PR) | **`libs_uncited_src_files` baseline 59 → 60, and the +1 is a gate finally SEEING a file it should always have seen.** The W3-1 mission-split file headers contained the literal token `[orig]` inside a sentence *about* citations ("the [orig] citations moved with the code they annotate"). `ratchet_counts.py` greps sources for `[orig`, and a grep cannot tell a citation from a sentence mentioning one — so all six new mission TUs read as cited. Four carried real citations anyway; `mission_capi.cpp` and `mission_names.cpp` did not, and passed the gate on comment text. Fixed: every header reworded so none contains a bracket token, and the claim now matches the file. `mission_names.cpp` EARNED a real citation rather than a baseline entry — its `ai_action_sub_type_name` is the port of the table `bms.h` already attributes to `[orig: dfx2med Med_ActionSubTypeName @0x445EE0]`. `mission_capi.cpp` is genuinely uncitable (our own flat C ABI over MissionDocument, not a port), so it becomes a visible uncited file — hence the +1. Found because W3-3 reported the counter IMPROVING 57 → 56 after splitting an uncited file in two, which cannot happen |
| Quality W3-3b GP read/write split | (this PR) | **`libs_uncited_src_files` baseline 58 → 59**, the same mechanical effect the W3-3 row below describes: `threedi_gp.cpp` carries no `[orig:]` citation, so splitting it into a read TU and a write TU turns one uncited file into two. Citation coverage is unchanged — `libs/threedi` is built from the format records in `docs/threedi/`, not from decompiled functions — but the counter counts FILES. With this, both of the library's format codecs are one-direction-per-file |
| Quality W3-3 threedi read/write split | (this PR) | **`libs_uncited_src_files` baseline 57 → 58, and the reason is worth reading.** Splitting `threedi_3di3.cpp` (which carries no `[orig:]` citation) into a read TU and a write TU turns ONE uncited file into TWO. Citation coverage does not change — the same uncited code is in the same library — but the counter counts FILES, so a motion-only split moves it. `libs/threedi` is built from the format records in `docs/threedi/`, not from decompiled functions: 11 of its 13 sources carry no citation, which is why it is not allowlisted like `pcapio` either (two files DO cite, so the library is not uniformly inapplicable). **Also recorded here as a correction:** the W3-1 mission split's file-header boilerplate contained the literal token `[orig]` in the sentence "the [orig] citations moved with the code they annotate". That made the counter read `mission_capi.cpp` and `mission_names.cpp` — which carry no real citation — as cited, so W3-1 passed this gate on comment text rather than substance. The boilerplate is fixed here for threedi and in a follow-up for mission (which will move the baseline again, honestly). A ratchet that can be satisfied by a comment is not a ratchet |
| Quality W2-8 pcapio | (this PR) | **Citation-allowlist addition, NOT a baseline bump:** `libs/pcapio` joins `citation_allowlist_libs` so `libs_uncited_src_files` stays 57. The pcap/pcapng reader parses a PUBLIC format (its header cites pcap-savefile(5) and the pcapng block spec), so it is infrastructure like io/vfs/resource_index, not a reimplementation of witnessed engine behavior — an `[orig:]` citation is inapplicable by construction. Also: `PcapWriter` deleted (229 lines, zero references repo-wide) |
| Quality W1-3 libs log sink | (this PR) | **New ratchet counter `libs_stdout_prints`, baseline 0.** `io/log.h` is the one diagnostic channel for `libs/`: libraries call `io::logf` and stay silent unless the host installs a sink (`novaworld_server` and `nw_server` install stderr/stdout sinks; `NW_LOG_DEBUG=1` opts into kDebug). All 57 console writes across 7 libs routed; `rdta.cpp`'s unconditional cwd `stripify_call_0.log` writer deleted (with its always-0 call counter and `OED_STRIPIFY_LOG_PATH`), `OED_STRIPIFY_SEED_DIAG` now defaults OFF. FILE*-parameter writers (tdp/mus/adm) are deliberately outside the counter |
| Quality W1-2 print-zero | (this PR) | **Two new ratchet counters, baseline 0**: `gd_prints_outside_debug` (raw print family in godot/{engine,game,modtools}; sanctioned channels = push_error/push_warning, print_verbose, the F3 system; the hidden `pack_game_cli.gd` release CLI is allowlisted because stdout/stderr is its interface) and `cpp_binding_console_writes` (UtilityFunctions::print/printerr, print_line, WARN/ERR_PRINT in godot/engine). The sweep that zeroed them: ~40 binding `printerr` → `push_warning` (these legs report via their return contract; GUT counts engine errors as failures, and the negative-path tests drive them), skeletal-anim `WARN_PRINT` → `push_warning`, the joiner freeze-tripwire `print_line` → `print_verbose` (its OPENNOVA_NET_DIAGNOSTICS gate retained), PerfTimeline + GameWorld warm-pass summaries → verbose channel, the NOVA_INF_DEBUG diagnosis leftover deleted, the overlay's duplicate pose print and the MCP banner print dropped. Channel policy recorded in godot/src/CLAUDE.md |
| Host-punt handling (PR #300) | (this PR) | `test_private_pokes` 1376 → **1378** (+2, maintainer-ratified 2026-07-26). `godot/tests/net/host_punt_surfacing_test.gd` installs a runtime double into a REAL `GameWorld` (`_runtime` / `_loaded`, two lines in one helper) to pin the D-NET-177 contract that a host's close surfaces exactly once and never re-opens the deploy screen — GameWorld exposes no public runtime-injection seam, and `game_world_test.gd` already drives the identical one. The round's other five pokes were removed instead: the shell case now reads `find_children(…, "DeployScreenPresenter")`, `get_node("World")` and `is_gameplay_input_active()` rather than `_deploy_presenter` / `_world` / `_state` |
| Gates repair after the #232 merge | (rides PR #236) | master merged #232 (the loading screen) with a red STD-1 ratchet: `test_private_pokes` rose 1384 → **1393** (+9, all `loading_screen_test.gd` `screen._in_session`/`_title`/`_mission_name`/`_custom_text`/`_game_type_text`/`_texture` state reads — logged here per the ratchet policy; baseline bumped via `--write-baseline`, MAINTAINER RATIFIES ON MERGE). Every open PR is red on this gate until this lands. The ledger scoreboard was already regenerated by #232 itself |
| Wave-2 tail + GOV-4 close-out (PR #236) | (this PR) | the closing train, ground in parallel agent slices and consolidated green: **STD-2** 7/8 seeded contracts → typed records (DocumentTabRow, TileGizmoState, ReferenceServices deduped, ReferenceEdge, FocusPayload + EditorNavLocation, MissionParamSpec/SlotSpec, McpToolDef; net −9 class-level Dictionary signatures, 3 transport edges precisely allowlisted); **ONED-W1 ×6** — TER-1 (at bar, the E/G exemplar; F3 staging note via the new `get_game_launch_note` hook + typed GameLaunchNote), ENV-1 (at bar; save-into-launch-dir is the staging; popup note routing), CRE-1 (scrub transport; cadence UNCITED → R5 re-opened), FNT-1 + STR-1 (EngineTextPreview adopted — the game's draw path replaces the Label truth-claims), STR-2 (encoding pinned; **D-FNT-4 minted** — cp1252 specials fall to the system-font fallback where retail draws `glyph = byte−32`); **MUS-D** design landed at the maintainer gate; **ENG-5 sweep #2** on the conformance checklist; this close-out. Gates: canary 719/720 (the documented intra-file flake, isolation-green ×2 + master-green), mission_inspector 125/500, every touched suite re-verified on the consolidated tree, ratchets +0, dict-contract 0, ledger synced, link-graph 0 forbidden |
| ENG-6 manifest leg (Wave-2 trunk 2) | (this trunk) | `libs/gameprofile/required_resources.{h,c}` instantiates the R8 record — ~75 rows phase-major in the witnessed load order, severity classes (FATAL/DIALOG/REQUIRED/SOFT/OPTIONAL), the boot-archive-table any-of flag, per-row witnessed failure text + `[orig]` citation; Model B only (no C-ABI change); `required_resources` ctest pins the eight-row fatal set + completeness. `ResourceRoot.list_missing_boot_resources()`/`boot_resource_failure_text()` probe the individually-fatal file rows (the archive trio stays `mount_runtime`'s gate), and `main_game.gd` raises honest missing-resource errors at mount (reported-not-enforced — the picker flow keeps a partial dir inspectable where retail MessageBox-exits). ONED conveniences over the same table stay ONED-REQ (Wave 3) |
| ENG-4 fonts + OED_UPDATE_* (Wave-2 trunk 2) | (this trunk) | **both conformance rows closed**: `fnt_pack_shelf` ports the rasterizer's deterministic shelf packer into `libs/fnt` beside the format facts (authoring policy, not witnessed engine behavior — noted in-header), bound as `FntResource.pack_shelf` + six format class constants; `fnt_rasterizer.gd` keeps only TextServer rasterization + blits (constants now aliases, `_pack()` deleted); `fnt_pack` ctest hand-walks the reference layout + the 16-page boundary, GUT `fnt_resource` gains the binding contract pin. `OED_UPDATE_*`: the C++ chain was already single-sourced (libs/oed → `ObjectData::UPDATE_*` → bound constants) — the three GDScript re-declarations became aliases (engine model + workspace) and deliberate literal pins (the test suite, + a new binding-drift test), the ENG-3 contract-pin pattern |
| LIBS-2 + LIBS-3 family topology (Wave-2 trunk 2) | (this trunk) | ADR 0024: one-lib-per-format affirmed; `opennova_terrain_family` + `opennova_audio_family` minted as pure INTERFACE link groups (`libs/families.cmake`, included by both CMake roots; the file was renamed `engine/families.cmake` by ADR 0028 and deleted by ADR 0029's group collapse) and adopted by the GDExtension in place of the twelve member lines — link conveniences, never merges, never an edge-laundering path (`link_graph_check` 330 targets / 0 forbidden). The ADR records the ADR-0023 renderer-fold reversal where the clause originated, and names the two consumption models; LIBS-3's operational text (Model A flat C ABI / Model B C++ static link, mixing rules, the annotate+baseline-bump protocol) lands in engine/CLAUDE.md |
| Gates repair after the #230 merge | (this slice) | master merged #230 with two red STD-1 gates (the #229 pattern): the merge's post-review particle tests raised `test_private_pokes` 1370 → **1384** (+5 `game_world_test.gd`, +6 `particle_authoring_test.gd`, +2 `effect_world_test.gd`, +1 `particle_editor_workstation_test.gd` — logged here per the ratchet policy; baseline bumped via `--write-baseline`, MAINTAINER RATIFIES ON MERGE), and the review round edited ledger tables without regenerating the count-to-zero scoreboard (`ledger_check.py --write` re-run). Every open PR is red on both gates until this lands. |
| Gates repair after the #226 merge | (this slice) | master merged #226 with two red STD-1 gates: the ledger's new D-ANIM-1 row had no scoreboard domain mapping (`ledger_check.py` DOMAIN_ORDER gains `ANIM` → World / AI + events; scoreboard regenerated) and `test_private_pokes` rose 1367 → **1370** (the three pokes are #226's own `weapon_round_probe.gd` `_host._vm_parts`/`_host._viewmodel` reads — probe diagnostics, logged here per the ratchet policy; baseline bumped via `--write-baseline`, MAINTAINER RATIFIES ON MERGE). Every open PR was red on both gates until this lands. |
| ENG-3 terrain query APIs (PR #209 train: A, C, B0, B1a, B1b) | 9a7c05b3 / 25a7cc01 / 09fd1ca2 / 54c3f269 / (B1b this slice) | one slice per boundary-conformance row: **A** scalar height sampler unified on the C++ live core (`TerrainData.sample_height_world_live`, GDScript bilinear deleted, parity test flipped to pin the single path); **C** sector/atlas constants single-sourced (`COORDS_ATLAS_SIZE`/`COORDS_SECTOR_ID_MAX` minted, four bound class constants + `world_to_sector_cell`, all GDScript re-declarations now aliases, contract pin); **B0** the raycast witnessed (terrain-re.md §Runtime terrain queries: the ~1-unit major-axis march + point-coarse/bilinear-confirm, the ÷4-step ≤9/≤9/8-bisection refine, the column-shortcut crossing rule; headline IDB repair — `null_stub @ 0x6067b0` was the canonical bilinear height sampler mistyped `void()`, retyped + renamed `Terrain_SampleHeightBilinear`, ~40 callers un-elided; 14 renames saved); **B1a** the core ported to `libs/terrain_query/terrain_raycast` (16.16 structural translation behind a point+bilinear sampler seam, ~60-check ctest, four witness claims sharpened at the port against live decompiles); **B1b** `TerrainData.raycast_terrain(from, to)` over live-image/baked-CPT substrates, mission picking + celestial glare adopted, the GDScript march/slab/bisection trio deleted, D-TERRAIN-4 minted (editor-guard residuals, class C candidate). Checklist rows 1/2/3 closed; ENG-4's fonts + `OED_UPDATE_*` row is the remainder |
| REN-7 close-out (REN TRAIN END) | (this slice) | the close-out gates, all green: **FULL GUT 2006/2007** (200 scripts, 27791 asserts, 0 failures — the single non-pass is the documented asset-gated avatar-preview PENDING; silent-drop greps clean via `scripts/test_godot.sh`); **full ctest 248/250** — the only reds are the two standing tracked ones (`opennova_python_pytest`, `npruntime_golden_gameplay`; both since closed, TODO.md rows pruned on completion), `parametric_parity` disabled as always; **T1** green on the fresh detail-stage golden; **T2** = swatch attested at the detail slice (exactly the 40 detail-keyed cells moved, 80/80 identical), **composite IDENTICAL vs post-gamma** (ordering untouched through REN-5/6/7), world = post-domeband2 (the skyfog fill live; the pure-land framing = the bitwise determinism sentinel); **T3** scene-by-scene table landed in the trunk PR — scenes 1/2/5 our-side captured at head, 3/4/6 enumerated on their tracked rows (D-RLIT-5, D-RMAT-8, D-RORD-4), banked RckS05 authentic-dark cited, the W_RCK1_O watch item CLOSED as D-RMAT-10; the by-eye retail pass is the maintainer's attestation. **Ledger**: the folded env rows ALL FIXED (#17/#19/#21/#27/#29/#30/#33/#34); D-RMAT-1..5/7/9/10 FIXED, -8 PERMANENT, -6 WRD riding D-RORD-4/-5; D-RORD-1 FIXED, -2/-6 PERMANENT, -3/-4/-5 tracked with named riders; D-RLIT/-FOLIAGE/-TERRAIN rows dispositioned with named riders; scoreboard regenerated. **IDB/dead-variant catalogs** current in the records (this train's rename logged in the env table; REN-6's appendix prior). **Correspondence** env+materials rows carry the REN-6/7 state; render README carries the T3 pointer; the conformance MATERIAL_FLAG_* row closed (single-sourced since REN-2, extended not re-duplicated at REN-7) |
| REN-7 detail stage (D-RMAT-10 minted-and-FIXED) | (this slice) | the T3 "W_RCK1_O watch item" resolved into a witnessed divergence: the `_MT` secondary (detail) stage ran HALF the witnessed combine — composer `base.rgb *= detail.rgb` (×1, alpha untouched) vs the witnessed stage 1 `TSSColor(1, Modulate2x, Texture, Current)` + `TSSAlpha(1, Modulate, Texture, Current)` (§FF technique tables) — so resolved MT surfaces (RckS05's gray `W_Rck1_o`, avg 93/255) modulated ×0.365 where retail runs ×0.73: MT objects TOO DARK in detail regions, the exact "retail reads brighter" direction banked at the RckS05 adjudication. Fixed composer-side (`× 2.0` + `base.a *= detail.a`, cited) + host-side stage-drop gating (`OSCAP_DETAIL` masked off the key when the secondary fails to resolve — retail's NULL-texture drop, exactly; the white ×1 fallback deleted; `classify()` pure). UV-set question settled with data: the .3di v8 vertex carries TWO authored UV sets (stride 40; RckS05 uv1 distinct 48/48, FOUNTAIN M4 455/455) — `v_uv2` was always the right sample. T1 re-dump: exactly the 224 `OSCAP_DETAIL` composed hashes moved (+27 bytes each), 0 classification rows, handoff pins unchanged (`FF_MT_OP/base → 0x00001004`); ctest renderer 5/5; GUT keystones green in isolation (handoff 3/3, object_editor 73/73, game_world 10/10, part_anim 10/10, submesh cache 8/8). The ×2 verified CORPUS-UNIFORM at the port (retail `localres.pff` .fx re-derived via `libs/pff`+`libs/scr`): `BDiffT2.fx` (`VS_DOT3DIFF2`) identical stage-1 pair; `SkBDiffO2.fx` (`VS_SKBUMPDIFFOBJ2`) same pair in its P3 post-multiply pass (DESTCOLOR/SRCCOLOR ×2 framebuffer form) — every family the key bit reaches is witnessed. Record gains the stage-1 `#UV` TextureTransform open question (identity for all non-`#UV` MT). T2 swatch A/B (instrument compare): EXACTLY the 40 detail-keyed cells moved (FF_MT_OP/AB/AD ± _LUM + VS_DOT3DIFF2 + VS_SKBUMPDIFFOBJ2, all variants), 80/80 non-detail cells bitwise identical. CP15 mission recapture attests the world-visible delta |
| REN-7 dome-band fill (D-TERRAIN-3 FIXED) | (this slice) | the below-horizon witness closed: retail fills the below-rim region with the **frame clear alone** — no skirt/ring geometry anywhere in the frame walk (`Terrain_RenderSkyboxPass @ 0x610ac0` → `Terrain_RenderSectorBatchLit @ 0x60c670`, ex `sub_60C670` = the plain fogged sector batch bracketed by white-ambient/lighting-on; the dome pass fogs toward the DOUBLED SKYFOG while drawing `[orig: sub_579CB0]` — the same block the clear paints, which is the whole seam-invisibility mechanism). The host defect was TWO stacked wiring bugs, not a missing witness: (1) the env #21 clear consumer wrote `background_color` into a `BG_SKY`(null-sky) Environment (Wave-1) that renders BLACK — the 1-px black dome-rim seam in every runtime view, the aerial black band, the water-horizon grazing band once #29's fade landed; fixed to `BG_COLOR` + `AMBIENT_SOURCE_DISABLED` (Godot ambient never injects), GUT-pinned — post-#29 the exposed defect was a PURE-BLACK BAND filling the whole strip-edge-to-rim region (near half the frame in dusk water views), not just the 1-px rim seam; (2) `get_frame_clear_color()` served the UNDOUBLED blend (the 07-05 "non-modulate2x device" reasoning inverted for this host — since D-RMAT-7 the host reproduces the MODULATE2X framebuffer, whose Clear takes the post-blend DOUBLED skyfog verbatim; the undoubled band measured exactly half the fogged rim); fixed to blend-then-double `[orig: @ 0x57f037..0x57f0a1 then @ 0x57f1b1]`, env vectors re-dumped (frame-clear token only), underwater branch verified already render-space, and the blend's distance input corrected to the smoothed current (`Env_FogDistCurrent @ 0x26c681c` — the #27 seam claim now holds for the clear). Ledger: D-TERRAIN-3 FIXED + D-TERRAIN-2 catalog row back-filled (terrain 0 open / 3 settled; total OPEN 35 → 34). World A/B (post-domeband2 = the rolling world baseline): the band renders the skyfog fill continuous with the dome (rim step ≤5 LSB at noon/dusk, EXACTLY 0 at night — the same fog-vs-skyfog two-block relation retail has, riding #20's cosmetic caveat); pure-land t1200_sun bitwise identical vs post-ren6-30 |
| REN-6 tail + fidelity riders (REN-6 CLOSED) | bc4e8e3f / 12f76202 / 671d6d17 / 442285b6 / 2c5b52cf / bef63329 | **D-RLIT-8 minted-and-FIXED** (object hemi_sky joined the smoothed writeback — the "buildings too dark off-noon" bug; night register verified on CP15); **D-FOLIAGE-2 minted-and-FIXED** (the foliage combine ported to the witnessed blend PS `[orig: @ 0x5ff7a0]`; D-FOLIAGE-1 narrowed, D-FOLIAGE-3 minted); the mission_visual_probe T3 ground-POV instrument (+ the public `set_resource_root_dir` knobs, private-pokes ratchet 1371 → 1370); **env #29 FIXED** (the strip-march water live end to end — `env::water_*` structural translation with 40 ctest pins, the packed-array bridge, the per-frame strip ArrayMesh, and the witnessed detail≥2 **embedded ps.1.1** found at the port's debug `[orig: Water_InitSurfaceShaders @ 0x5c19b0]`; witness re-grades landed: underwater-view arg, tier constant families, vertex-vs-prim counts, depth clamps); **env #30 FIXED** (host planar reflection — SubViewport mirror camera feeding the ps.1.1 t2 slot; the witnessed rows pin the mapping so the texm3x2 runs verbatim; clip plane approximation tracked in-row). REN-6 is CLOSED — all four env leftovers FIXED; Environment 3 open. Goldens re-pinned surgically (water/mesh, water/strip; water/snap retired). Remaining: the below-horizon skyfog band (the fade exposes the dome's black below-rim region — D-TERRAIN-3's substance, needs its own dome witness) rides REN-7 |
| REN-6 witness + first port leg | (this slice) | all four env-leftover unknowns closed in IDA (witness commit 2cf6ba77): the #33 generator FOUND (`Star_GenerateInstanceTable @ 0x5ac850`, ex `init_weather_particles` — the loop starts at table+16, why the base had no xref; per-star math fully decoded; dead PRNG stub `@ 0x5ac010`), the #27 mystery pair = the RAIN PERCENT spring (`Env_RainPct*` — "Rain: %i%%" debug label, net-synced target, >48 drives particle fall decay), the #29 strip decode with exact constants (2..9 = adaptive ROW stride `clamp(int(1/w×500),2,9)` — doc corrected; 3 verts/row; ≤5-row batches via static index tables; the murk chain + fog W clamps pinned — the detailed-tier chain, clamps, batch prim count, and the "isReflection"→underwater-view arg were RE-GRADED at the 2026-07-07 libs port leg's IDB re-read, env-tod-re.md §The strips), the #30 reflection internals (RTT begin/end on `Water_ReflectionTexture`, skyfog clear, clip plane wh−0.1, `Water_RenderReflectedWorldScene` = the ex three-flush decompile-fail — a stale NORET flag; builds the water CLIP-plane texture matrix `u = y−wh+0.5` + arms `g_WaterMirrorActive`; render-order-re.md's open question closed). Port leg: **env #27 FIXED** — `env::EnvScalarChannels` (witnessed spring/eighth steps in the witnessed in-tick position, ctest-pinned; targets-only snap semantics) ticked by `WeatherCore`, smoothed fog-distance/sky-height served through `MissionEnvironment.set_smoothed_scalars` to every consumer (dome, water UV, object/terrain fog, frame clear), SunDim live through celestial sun alpha + glare fold; **env #33 FIXED** — `env::generate_star_instances`/`star_twinkle_tick`/`star_visible_fixed` (PRNG pins from seed 1, 256-star invariants) hosted by `StarField` + the camera-anchored billboard MultiMesh in `celestial.gd` (per-star twinkle via instance color, 0.98 near-light cull, regenerate-per-load; the single-body stand-in + its dead-variant 0x2000 opacity deleted). Ledger: Environment 7 → 5 open. GUT: env vectors 204/204, env_file, sky, environment editor/preview/badges, celestial parse+field smoke (252/256 visible at zenith — the cone culls 4) |
| REN model-parity: witnessed preview defaults + static-batch relighting | ff13e643 (+ rider b93d29b0) | the follow-through on the color-pipeline slice, from a live world audit (the mission workspace's 943-object scene, per-material uniform diagnostics): **un-enved preview defaults re-derived to the retail noon register** — full_00.env tod 1200 block bytes (sun 170,170,167; sky 84,88,89; ground 49,55,46) across the composer uniform defaults, `object_model.gd` DEFAULT_*, the `opennova_*` shader globals, and the terrain include (one cited register; the old ad-hoc trio was tuned for the pre-gamma pipeline); **D-RLIT-7 minted-and-FIXED** — the placer's static MultiMesh batches froze the load-time env snapshot (no live owner; the pre-first-iris-tick modulator baked in) where retail relights every entity per frame `[orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0]`; the placer now registers harvested batch materials and re-stamps them per frame via the container's `mission_batch_env_stamper` (generation-gated; stamping single-sourced as `ObjectModel.environment_values_from`/`apply_environment_values`; verified batch == live-model uniforms after settle). Audit byproducts, all verified clean: every batch carries a bound ShaderMaterial (0 null overrides/446), every diffuse bound (0 unbound), no black texture decodes, no degenerate normals. T1 re-dumped (key set identical — the defaults are composer text); T2 swatch 68/120 lit cells moved (defaults-only delta; emissive/tracer cells untouched); placer/controller/collision/resolver/object/canary GUT green |
| REN model-parity: the gamma-space color pipeline | 024f7f54 | maintainer-priority interleave before REN-6 ("make the 3di/material/texture world look faithful"): the retail color pipeline witnessed **gamma-space end to end** — no `D3DSAMP_SRGBTEXTURE` at any device sampler-state site (full sweep: only ADDRESS/FILTER/LODBIAS/MAXMIP/ANISO states `[orig: CD3DDevice_InitializeDisplay @ 0x679c1b; CGfxDevice_ApplyRenderStates @ 0x67e3ec]`), no `D3DRS_SRGBWRITEENABLE` at any of the 170 render-state sites, zero sRGB states in the 44-file `.fx` corpus, identity display ramp at default gamma 1.0 `[orig: GLib_SetGammaRamp @ 0x677be0; @ 0x84f354]` — while the host decoded textures sRGB→linear and re-encoded at the blit around the witnessed math (compressed lighting contrast; the washed-out world). **D-RMAT-7 minted-and-FIXED**: raw sampling + gamma-space math + the exact-inverse `gamma_to_linear` output across the object composer and all 8 world shaders (`godot/shaders/color.gdshaderinc`); the swatch probe gained a **calibrate mode** proving displayed-byte == computed-byte 256/256 on the live build; **D-RMAT-9 minted-and-FIXED** (the composer's invented fog ramp → the witnessed device fog table `[orig: @ 0x58a950 → @ 0x677960]`); **D-RMAT-8 minted-PERMANENT** (blend-space residual; ADR 0022 register, which also gains the back-filled REN-3 D-RORD-2/-6 entries). T1: key set identical, all 630 hashes cited-re-dumped; T2: swatch 120/120 moved (the expected global response change), composite IDENTICAL (ordering untouched), world set re-captured (post-gamma = the new rolling baseline — the washed-out sky/terrain now render the saturated retail register); GUT keystones green (handoff, object model, object_editor 73/73, canary, env vectors, env_file, badges) |
| REN-5 lighting | e24c6a9f (+ rider bd0a5840) | the world lighting chain witnessed end to end and ported: **env #17 CLOSED** — the iris/modulator chain is LIVE (`libs/env::ModulatorChain`, the witnessed modulator2→modulator→blocks tick order `[orig: @ 0x57ef97..0x57f03c]`, the 62-tick exposure chase `[orig: @ 0x57e512; ColorBlock_SetStepDeltas @ 0x57d940]`, ÷64 gain to ColorSrcGlobalGain/ambient scale `[orig: @ 0x58db30; @ 0x5aaef0]`; env vectors re-dumped surgically — exactly 8 weather rows, hand-verified `0x31·61/64 = 0x2E`); **D-RMAT-5 CLOSED** — the composer emits the witnessed FF MODULATE2X model on the pinned uniform surface (slots 225-230 + 232; the ×1.5/×1.6/spec-0.8 prototypes deleted; T1: key set identical, all 630 hashes cited-re-dumped, +30 lighting vectors in NEW section 5; T2: composite IDENTICAL, swatch 116/120 cells moved with the 4 unlit VS_TRACER cells byte-identical); the entity chain decoded (writer `@ 0x5c8090`/store `@ 0x5d89e0`, reader `@ 0x5d98a0`, hemisphere delta lights `@ 0x5d8cb0`, the REN-4 "dual-LOD lerp" erratum corrected to the INTERIOR DAYLIGHT lerp, effectScale = 3-ray sun visibility `@ 0x5c6800`) and ported to `libs/renderer/light_runtime`; terrain/foliage **c0/c1 = sky/light** closed (`terrain_lighting.gdshaderinc` corrected from the gobj-era pairing); point-light math + group culling witnessed (= the OED attenuation); `Lighting_InitTextures` + the cubemap sources witnessed (CubeRotSpecular = the static sun-glint cube — D-RORD-5's substrate answered); [`render-lighting-re.md`](render/render-lighting-re.md) + D-RLIT-1..6 minted (audit track **1 → 0**); 15 fn + 49 data IDB renames; GUT keystones green (handoff 3/3, object model 10/10, object_editor 73/73, canary 89/89, env vectors 204/204) |
| REN-4 shaders/TSS | fb1c8e7c | the fixed-function shader/TSS layer decoded end to end: the engine render-mode word (blend/alpha/color combiner tables `[orig: decode_blend_mode_to_d3d_states @ 0x680f00; decode_mode_alpha_stage @ 0x680b00; decode_mode_color_stage @ 0x681080]`) + the state permutation cache (`[orig: @ 0x681d00; RenderState_CacheFindOrAdd @ 0x683420]`); the water surface material set (`Water_InitSurfaceShaders @ 0x5c19b0` — the "Terrain_InitShaders" misnomer resolved) ported into `water.gdshader` (witnessed ONE+dst·SRCALPHA blend via premul-alpha + ref-32 discard; env #34 minted-and-closed); the terrain surface shading witnessed into terrain-re.md §Runtime surface shading (8 embedded pixel shaders incl. the ps.1.4 splat, tier gates, foliage-model VS/PS set, alpha refs 180/8); D-RMAT-2 FIXED (`OSCAP_VIEW_FADE` tracer fade) + D-RMAT-4 FIXED (the IsParameterUsed probe replicated over the shipped corpus — 5 OED-dump drift rows corrected on the descriptor table, `MATERIAL_FLAG_GLOW` minted, `is_glow_capable` classification); `renderer::uv_anim` ported (T1 section 4); the FlushBatches pass loop witnessed (per-light passrules multiplication, submit-0x10/+841/MATCHTERRAIN questions closed); ptl §5.2 stage-stride + §5.5 VS/PS errata; 17+4 IDB renames; T2: composite IDENTICAL, swatch key-set delta fully T1-hash-attributed, world set re-captured (post-ren4 = the rolling baseline) |
| REN-3 draw order | 885d8702 | the batching/draw-order system witnessed end to end (four queues + the unsigned-ascending sort `[orig: RenderBatch_QuickSort @ 0x5d8b40]`; the opaque state-sort key + the transparent `~float_bits` back-to-front key; the water-plane queue split + frame bracket `[orig: Terrain_RenderSceneWithReflection @ 0x5c93a0]`; technique-class selection; the render-state stack; the viewmodel near-Z 0.05 + viewport-depth [0,0.1] pass; the Q3 glow/envmap copy flushed by the bloom pass) — [render/render-order-re.md](render/render-order-re.md), D-RORD-1..6. Ordering semantics ported to `libs/renderer/render_order` and applied as the generalized Godot priority ladder (celestial rungs re-derived, water rung, object-model water-side rungs via the new `ObjectShaderCache` seam); T1 extended with the draw-order vector section (cited re-dump, pure append); T2 gained the depth-adversarial composite ordering scenes (fresh baseline; swatch A/B bitwise-identical vs post-ren2); 11 function + 15 data renames landed in the IDB (EffectWorld particle pass ex-`CNapiSession_*`, the render-state stack push ex-`CNetPlayer_*`, the viewmodel viewport/near-Z pair ex-`Scar_*`); draw order leaves UNAUDITED (2→1); D-RORD-2/-6 ratified into the permanent register; env #30 gained its pass-structure rider; world §13's `0x10000000` identified as the repeat-draw marker |
| REN-2 materials grill + converge | 005efae0 | the runtime material path witnessed end to end (HLSLEffect registry built from `_FFP.fx` ×24 + the localres `.fx` set + `#UV` twins; probe-derived capability flags; six technique-class pass blocks; state application at FlushBatches) — [render/render-material-re.md](render/render-material-re.md), D-RMAT-1..6. Fixed in-slice with cited T1 re-dumps: the alpha-test compare shape (invert flips the COMPARE `[orig: @ 0x6770a0]`), case-insensitive tag lookup `[orig: @ 0x5ade70]`, the VS_TRACER registry row; flag-enum misnomers renamed everywhere (BLENDING/SKINNED/TANGENT/UVGEN); `MATERIAL_FLAG_*` single-sourced onto `ObjectShaderCache` (the ENG-4 leg); 3 kong misnomers renamed in the IDB; materials leave UNAUDITED (3→2); ratchet `libs_uncited_src_files` tightened 65 → **64** (the composer earned its citation) |
| REN-1 instrument (rode this trunk, d158e3e3) + REN-0 mint (ce2b0978) | d158e3e3 / ce2b0978 | the three-tier parity instrument + pre-change baselines; the track mint + [ADR 0023](adr/0023-render-visual-parity.md); ledger audit track reopened (UNAUDITED = 3); env #17 → REN-5 and #27/#29/#30/#33 → REN-6 transfers; `ledger_check.py` DOMAIN_ORDER + the LIBS-2 `libs/renderer`-fold reversal |
| ENG-2 env port (PR #206 train) | 28cc9474 … (7 slices) | the five environment GDScript files are scene plumbing over `libs/env`: weather core (slices 1-2), sky dome + cloud scroll (3-4), water surface (5-6, full witnessed render: per-frame noise textures + UV + lit-color pipeline), celestial placement + glare occlusion (7). Re-grills caught the shared RNG-seed transcription (env #25, fixed in libs/env + libs/wac), the scroll model (#26), the water look/precedence (#28/#31), celestial placement inventions (#32), and CLOSED env #14; minted #27/#29/#30/#33 as tracked deferrals. Honored-matrix re-attested; vectors 204/204 with per-slice witnessed re-dumps |
| Wave-2 boundary: STD-3 flip + GOV-4 sync | (this slice) | CI lint step now `--enforce` ×3 (ratchet, dict-contract, link-graph) + `fetch-depth: 0` so the diff lint stops self-skipping on PRs; dict-contract lint repaired to class-level declarations only (its Wave-1 soft run flagged 36 function-locals — a lint bug, not code debt; the 4 surviving range hits are pre-existing contracts moved by F5); ledger scoreboard now GENERATED (`scripts/lint/ledger_check.py`, hard-fail like the ratchet) — mechanical recount corrected the hand-kept total 64 → **66 open** (Net rows were undercounted 21→23, Credits 2→1, Tiles 1→0, Fonts 3→1); ratchet baseline tightened `libs_uncited_src_files` 87 → **65** (earned by the PAR-train citations); D-FOLIAGE-1's raw `\|` escaped (it broke GFM rendering + parsers) |
| GOV-4 Wave-1 close-out | 50fce423 (rode the trunk) | ratchets +0 all train (`test_private_pokes` 1371, `libs_uncited_src_files` 88 at close — see the boundary slice's recount); ledger synced continuously (the trunk-era hand count said 66 → 62; the mechanical recount at the boundary says 66 — the drift is why the scoreboard is now generated); stale-doc touches rode each slice (vfs record, menu-re, world-wac-ai-re, env records) |
| NET-4 C-ABI guard | ae221e7d + 729f01c7 + 17f06457 | `abi_export_identity` ctest (106-export baseline since 2026-07-05, forbidden net families); its macOS leg immediately caught + fixed the dylib leak (hidden visibility + 38 phantom MISSION_EXPORTs stripped) — the flat C ABI is now provably identical on every platform. (2026-07-29 audit note: #344 removed the macOS legs from regular CI and release.yml tests ubuntu+windows only, so the macOS enforcement this row credits is currently LAPSED - no workflow compiles or tests macOS between release tags) |
| PAR burn-down (trunk train) | 8778a0b4 … 2ed9a4fb | eight rows closed: D-EVT-2/-4 + cats 5/6 + D-EVT-5 mint, D-PLAYERINFO-2, D-NET-20, env #21, env #19 (tint consumers; dead-bake correction), D-VFS-2 (fixed boot table + the editor-index decision), D-CTRL-2 (witnessed visibility flags), D-INF-4 (witnessed direction-table generator); PAR-R7 VFS/PFF audit landed (D-VFS-1..9) |
| ENG-6 R8 boot-resource research | 1139190e | docs/required-resources.md (fatal set, ordered boot sequence, D-BOOT catalog); the Wave-2 manifest leg stays open |
| ONED F5 mission_controller decomposition | 9c4a1ea3 | 4,033 → 1,646-line composer + seven `_ops` sections; weakref `_c` leak fix (65e9e33d); the master-red `mission_data_test` literal repaired (7055d0d5) |
| ONED F2 EngineTextPreview + F3 See-in-game | ba467137 + a68e271f | the game-seam text preview widget; the `/d` loose-override runner with typed LaunchPlan |
| ADR-0016 packaging repair | 0836edc2 | AvatarPreview moved to the shared engine layer — the runtime package excluded `modtools/*`, master's boot smoke was red since #194 |
| ONED F4 writer-parity convention | 29b5ded8 | the four-part W gate recorded in the track doc; engine/CLAUDE.md points at it; binds hudpos (HUD-1) and avatars (AVT-1) forward |
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
    (`nw_golden_diff_test --ours <capture>` against
    `<OPENNOVA_CAPTURES>/golden/retail-gameplay-session.pcapng`) and the npruntime golden joins,
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
  ctest (`scripts/lint/abi_exports_check.py`, retired 2026-08-26 by ADR 0038;
  106-export committed baseline, never-bypassable forbidden-family check) ran
  wherever `BUILD_SHARED_LIB=ON` builds run ctest (scripts/build.sh + the CI
  build-and-test job).

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
  documented in engine/CLAUDE.md and named in ADR 0024: Model A = the flat C
  ABI (`opennova_shared`, importer/Python/DCC, `abi_export_identity`-pinned),
  Model B = C++ static link (engine, apps, tests, net). Naming them makes
  the npwire ADR's "net stays out of the C ABI" clause structural.

### ENG — engine portability, boundary APIs, required resources

Background finding (the seed list — ADR 0016 is the rule): a GDScript
scalar height sampler duplicating the C++ one; a hand-rolled terrain
ray-march under mission picking; duplicated sector/atlas constants; FNT
format facts + a shelf packer in `fnt_rasterizer.gd`; `MATERIAL_FLAG_*`/
`OED_UPDATE_*` duplicated in `object_model.gd`; and
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
  aliases over the bound `ObjectData.UPDATE_*` + test contract pins).
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
  boot validation consumes it (`ResourceRoot.list_missing_boot_resources`
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
| Scalar GDScript height sampler duplicating C++ | terrain editor mesh | **closed (ENG-3, 2026-07-07)** — `EditorTerrainMesh.sample_world_height` forwards to the new `TerrainData.sample_height_world_live` (the batch sampler's per-point core, shared so the two can never disagree); the GDScript bilinear deleted; `terrain_height_revision_test` flipped from pinning batch/scalar drift to pinning the single path |
| Hand-rolled terrain ray-march + slab test | terrain editor → mission picking | **closed (ENG-3, 2026-07-07)** — the witnessed raycast ported to `libs/terrain_query/terrain_raycast` (`[orig: @ 0x60cb80; @ 0x60e710]`, ENG-3 B0 grill → B1 port), bound as `TerrainData.raycast_terrain` over both substrates; the GDScript march/slab/bisection trio deleted; the celestial glare stand-in retired onto the same binding; editor-guard residuals = D-TERRAIN-4 |
| Sector/atlas constants + coord math duplicated | terrain editor mesh | **closed (ENG-3, 2026-07-07)** — `COORDS_ATLAS_SIZE`/`COORDS_SECTOR_ID_MAX` minted beside the existing pair in `terrain_query/coords.h`; all four bound as `TerrainData` class constants and consumed by the editor scripts (the `ATLAS_SIZE`/`SECTOR_SIZE`/`PATCH_VERTS`/`HM_SIZE` re-declarations are now aliases; `row*16+col` strides + 0..4 clamps on the constants); `world_to_sector_cell` bound and forwarded like the sibling transforms; contract pinned by `terrain_coords_test` |
| FNT format facts + shelf packer in editor | fonts rasterizer | **closed (ENG-4, Wave-2 trunk 2)** — the deterministic shelf packer ported to `libs/fnt` (`fnt_pack_shelf`, `FNT_PACK_PAD` minted; ctest hand-walks the reference layout); `FntResource` binds it plus the six format class constants; `fnt_rasterizer.gd` keeps only the TextServer rasterization + page blits, its constants now aliases |
| MATERIAL_FLAG_* / OED_UPDATE_* duplicated | engine object model GDScript | **closed (flags: REN-2, confirmed at REN-7; OED_UPDATE_*: ENG-4, Wave-2 trunk 2)** — MATERIAL_FLAG_* single-sourced onto ObjectShaderCache; OED_UPDATE_* GDScript re-declarations became aliases of the already-bound `ObjectData.UPDATE_*` (engine model + object workspace), with the test suite keeping deliberate literal pins + a binding-drift test (the ENG-3 contract-pin pattern) |
| Env/TOD/celestial/weather math in GDScript | godot/engine/environment | **closed (ENG-2, 2026-07-06)** — nodes are plumbing over libs/env; the tracked stand-ins are divergence rows (env #29 strip tessellation, #33 star instancing, #27 smoothed scalars) |
| Menu absolute-rect math in canvas | mnu canvas | **closed as reviewed exception (ENG-5 sweep #2, 2026-07-12)** — the canvas's absolute-rect summation is edit-model gesture math over the document tree (picking, ghost drags, snap tuning in board units); the witnessed scale model stays in the hosted live engine node (`[orig: CUIScene_SetScreenScale @ 0x639480]`); no engine constant or witnessed math is duplicated |
| HUD view-helper math cluster (exact-integer fade decay, 1024×768 design scale, 16.16 crosshair spread + TAPER strip, Q16 stance scaling, health thresholds, message tick policy, half-bright text, ammo format, stance→ERROR-row remap, capacity-1 reserve fold — all `[orig]`-cited) | `godot/engine/ui/hud_*.gd`, `game_hud.gd`, `game_hud_presenter.gd` | open (sweep #2, 2026-07-12) — **tracked: one `libs/hud` port slice** (ENG-4/FNT pattern: math + constants native, `draw_*`/Font blits stay host); no libs home exists today — `libs/def` deliberately keeps ALPHAFADE raw; **closed** — the ADR 0033 R2 HUD cutover landed exactly this port (`engine/runtime/hud` `hud_math` + `HudFrameCompiler`, 2026-08-09) |
| Sound distance/TOD-crossfade curves (`calc_distance_volume` Q16 chain `[orig: @ 0x75ca20]`, emitter falloff arms `[orig: @ 0x528667]`, oneshot curve `[orig: @ 0x75cf14]`, `time_of_day_region` cuts + blend `[orig: @ 0x408110]`, crossfade byte, marker stagger) | `libs/audio` `ambient_mixer.cpp` (curves + emitter arms + TOD region + crossfade byte; the GDScript seams delegate; oneshot curve composes from the shared `calc_distance_volume`) | **DONE 2026-07-28** (the D-SND-16 cadence port carried the curves slice; `ambient_mixer` ctest pins the integers; Godot voice/bus writes stay host) |
| Mission TOD clock (`Env_TodAdvancePerTick = 0x18000000/(3720·minutes)` `[orig: @ 0x57d108]`, Q8.8→8.24 widening, 60-min clamp) | `engine/runtime/environment/environment_state.h` (ex `environment.gd`, native since the #460 rework; the clock itself now lives in `engine/formats/env/tod_clock.h`) | **closed (#426, the ADR 0028 push-down trunk)** — the clock lives at `engine/formats/env/tod_clock.h`; the state at `engine/runtime/environment/environment_state.h` |
| Camera/view composition remainder (eye height 1.0 dual-declared with `simulation.cpp`, TP distance/orbit `[orig: @ 0x4391d0]`, eye re-aim `[orig: @ 0x437d10]`, weapon.def /256 view-offset + axis map `[orig: @ 0x4dd380]`; the ADS bias lerp shadows the production-callerless `player_view_bias_units`) | `player_viewmodel_rig.gd` | open (sweep #2) — **tracked (S): fold into `libs/world` player_view**; the ADS-lerp, the anim-key-substring stance probe (sim owns `net_stance_bits`), and the 4×-re-declared 62.5 Hz tick constant were folded by the #460 avatar slice (`world/player_view.h` engine statics); **closed (#426/#460)** |
| Editor-preview PLAYPARTANIM phase integrator coexisting with `AiSystem::advance_part_anim` | `object_model.gd` | open (sweep #2) — **tracked**: route the preview through the engine integrator (the height-sampler dual-implementation pattern); runtime already uses `set_part_phase` correctly; **closed (#460 — `object_model.gd` went native; one ObjectModel integrator)** |
| Armory derivation math (class resolve scan + masks `[orig: @ 0x5642f0]`, loadout weight Σ + encumbrance bands `[orig: @ 0x565490; @ 0x565640]`) | `armory_menu_companion.gd` | weight half CLOSED 2026-07-30: `def_loadout_weight`/`def_encumbrance_class` are bound on `WeaponDatabase` (`loadout_weight`/`encumbrance_class`) and consumed by BOTH `player_info_menu_companion.gd` and `armory_menu_companion.gd`; residual class-resolve scan half **closed (#426/#460 — `armory_menu_companion.gd:485,496` consumes the bound `WeaponDatabase` surface)** |
| Particle flag literals (`1<<18/27/28`) re-declared + flags→kill-plane dispatch | `effect_world.gd`, `particle_preview.gd` | open (sweep #2) — **close-now candidate**: alias off the already-bound `ParticleDef` flag table (the MATERIAL_FLAG_*/OED_UPDATE_* pattern exactly); **closed (#426 — the literals survive only as deliberate test pins in `effect_world_test.gd`)** |
| Avatar menu-portrait presentation math (BAM/frame idle, 2^28 sway, rand-yaw `[orig: @ 0x55dba0; @ 0x5600d0]`) | `avatar_preview.gd` | open (sweep #2) — minor, exception-leaning (witnessed menu-frontend presentation over Godot camera); disposition with the HUD slice's review; **closed (#426 — the witnessed constants moved engine-side in the #460 avatar slice, `avatars/preview_animation.h`)** |

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

### ONED — run only

[ADR 0037](adr/0037-oned-runs-game-data.md) supersedes the former editor
maturity track. ONED now owns settings and one managed game-run session; project,
workspace, import-database, reference-graph, and asset-authoring work is not on
the active roadmap. The old workspace program remains only as historical context.

### PROD — products and serve mode

- **PROD-1** (M) serve mode per ADR 0015: `opennova.exe --server`
  (+ `--headless`): windowed serve jumps the menu shell to the
  server-options `.mnu`; headless serve drives the existing host-session
  bring-up (ADR 0013 helper, 62 Hz pump). No new seam, no new protocol.
  The packaging boot smoke gains a `--headless --server` leg.
- **PROD-2** (S) taxonomy conformance: export-presets audit (exactly two
  Godot products), validate_release_deliverables expectations, the
  title-identity fragmentation note recorded as future work.
- **PROD-3** (S/M) program-end release dry run: package both Godot exes, all
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
- **PAR-UI** (M) D-MNU-5/6, D-CTRL-3, D-PLAYERINFO-11 (closed 2026-07-30, #388),
  D-SND-2, and D-HUD after its RE port lands.
- **PAR-R1..R7** (S/M each; **R7 landed 2026-07-05** — [vfs/vfs-pff-mount-re.md](vfs/vfs-pff-mount-re.md), D-VFS-1..9) the UNAUDITED-system audits (engine-research /
  grill-ida): terrain, foliage, tiles, fonts, credits, importer pipeline, VFS/PFF mount
  stack — each lands an RE record **with a D-catalog**.
- **Class-B research starters** (freeze-exempt engine-research): the full `.mis`
  grammar grill (`dfx2med.exe`, D-MIS-1/-3). Earlier starters are closed: the HUD
  radar/crosshair question (D-HUD-2), D-NET-49, and the in-world avatar binding
  (D-PLAYERINFO-1, packed-id/world/FP/`TEX_CAMO` path ported 2026-08-15).

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
the raw `gMaterialInfoTable` dump) and its historical chain — `libs/renderer`
`classify_object_material()` → generated GLSL (`object_shader_template.cpp`)
→ `ObjectShaderCache` → `object_model.gd` ShaderMaterials — carried
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
  `libs/env::ModulatorChain` + `WeatherCore`, **env #17 CLOSED**, env
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
  128 frames; **CubeRotSpecular = the static sun-glint cube** — the source
  consumed by the now-fixed Q3 path); the render-slot (character shadow) lighting witnessed as
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
  **#33 FIXED** (`env::generate_star_instances` + `StarField` — the
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
slots; scars/decals (the `Scar_` family — a future track candidate; the
render-slot ENTITY ground-shadow family left this exclusion on 2026-08-20
when it was witnessed end to end and ported —
`engine/runtime/renderer/render_slot_shadow`, render-lighting-re.md);
particle LOOK (PTL's domain — REN provides the state substrate);
2D/HUD/text/menu/loading draw; performance work beyond parity; runtime `.fx`
parsing (the descriptor table stays canonical).

## Out of scope (tracked here so nobody re-litigates silently)

DFX2 / any title split (cheap later; no new hardcoded title identity
meanwhile); title-identity consolidation (note-only); NovaWorld
service/backend/web/OpenNova Launcher feature work; the music document/VM layer;
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
| New Dictionary contracts | diff-scoped lint over `maturity_lint.py`'s `LINT_SCOPES` (`godot/modtools/`, `godot/game/`, `godot/src/`, `godot/probes/`) signatures, allowlist for transport edges | Wave 0 soft | hard-fail at Wave-2 boundary |
| [orig] citation coverage | ratchet `engine_uncited_src_files`: `engine/<group>/<lib>` source files with zero citations (infra libs allowlisted), fail-on-increase | Wave 1 | hard on increase; semantic coverage stays a review concern |
| Magic numbers | diff-scoped advisory in the CI summary | Wave 2 | advisory permanently |
| Link-graph edges | forbidden-edge script (npwire !→ sqlite; wac/mission/net !→ terrain-format libs post-seam) | Wave 1 | hard-fail forever |
| Ledger scoreboard sync | `scripts/lint/ledger_check.py` — the count-to-zero block is generated from the per-domain tables (`--write`), CI checks the equality | Wave 2 | hard-fail forever (ratchet-class: a mismatch is never a false positive, `--write` IS the fix) |
| GUT silent-drop greps | existing | exists | unchanged |
| C-ABI export identity + net-family ban | `abi_export_identity` ctest (`abi_exports_check.py` vs the committed baseline; a baseline bump is same-commit and logged here; the net-family check is never bypassable) | Wave 1 (NET-4) | retired 2026-08-26 (ADR 0038) with the FFI |

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
