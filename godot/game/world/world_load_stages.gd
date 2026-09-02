class_name WorldLoadStages
extends RefCounted

## The mission load plan's stage bodies, extracted from GameWorld (the engine's
## mission_load_plan.h names the order): mount, place, environment, terrain,
## the tile info and join-wire assets, the runtime start, audio, the minimap
## mask, the effect world, and unload. Shared world state (the engine nodes,
## the runtime, the join-wire latches, every counter a staying GameWorld method
## also reads) stays on GameWorld and is reached through `_world`, the
## WorldDeviceFrame pattern. This is not a second load coordinator: the shell's
## WorldLoadCoordinator owns the operation and its cancel/settle edges.

const MissionPresentation := preload("res://game/world/mission_presentation.gd")
const ResourceDirSettings := preload("res://game/resource_index/resource_dir_settings.gd")
const VegAssets := preload("res://game/terrain/veg_assets.gd")

var _world: GameWorld


func setup(world: GameWorld) -> void:
	_world = world


# The missing-terrain/env reason, join-aware: a wire-header join has no local
# .bms — the host streamed the mission identity — so the reason names the
# stream and what is mounted/installed, making a live punt read as "your
# install lacks X" rather than a bad local file.
func missing_mission_asset_reason(asset: String, bms_name: String,
		resource_root: ResourceRoot, wire_header_join: bool) -> String:
	if not wire_header_join:
		return "%s (from %s) not found in %s" % [asset, bms_name, resource_root.get_root_dir()]
	var mounted := String(resource_root.get_expansion())
	var mounted_text := ("'%s'" % mounted) if not mounted.is_empty() else "base game"
	return "%s (named by the host's streamed mission %s) not found in %s (mounted: %s, installed: %s)" % [
		asset, bms_name, resource_root.get_root_dir(), mounted_text,
		NetSessionPolicy.describe_installed(
			resource_root.list_expansions(resource_root.get_root_dir()))]


# The ONE mission path — the file entry (load_mission) and the in-memory
# entry (load_mission_data) converge here: resolve the header's terrain +
# environment from `resource_root`, apply the mission's env overrides, build the
# world, place objects, start the runtime + audio.
func _load_mission_internal(mission: MissionData, bms_name: String,
		resource_root: ResourceRoot) -> int:
	var wire_header_join := mission.is_wire_header_only()
	_world._join_wire_assets_pending = wire_header_join
	_world._join_wire_til_applied = false
	_world._join_wire_assets_failed = false
	_world._join_wire_asset_failure_emitted = false
	# The local-asset gate holds for wire-header joins too — there is no world
	# without terrain/env, and retail joiners also resolve both from the local
	# install by the names the wire supplies (net-re section 5.28: custom
	# missions reference stock assets). Only the REPORT is join-aware: a wire
	# join names the host's stream and the mounted expansion so a live punt
	# reads as "your install lacks X", not as a bad local file.
	# The document's references are basenames from either header source (the
	# wire 0x0B header's extension is dropped by MissionDocument).
	var trn := mission.get_terrain_ref() + ".trn"
	if not resource_root.has_file(trn):
		_world.load_failed.emit(missing_mission_asset_reason(
				trn, bms_name, resource_root, wire_header_join))
		return ERR_FILE_NOT_FOUND
	var env_name := mission.get_environment_ref() + ".env"
	if not resource_root.has_file(env_name):
		_world.load_failed.emit(missing_mission_asset_reason(
				env_name, bms_name, resource_root, wire_header_join))
		return ERR_FILE_NOT_FOUND

	_set_weather_world_tick_driven(true)
	_set_water_world_rendering_enabled(false)
	# Keep stage attribution stable so load timelines remain comparable.
	var timeline := PerfTimeline.begin("Mission load %s" % bms_name)
	_world._last_load_timeline = timeline
	_world._resource_root = resource_root
	# The shared .3DI definition cache resets before the environment and
	# terrain stages (the load plan's first order witness): Celestial resolves
	# its models from _load_environment, and foliage loaded by terrain must
	# remain present-but-excluded in the same generation.
	ObjectData.reset_network_challenge_model_registry()
	_load_mission_tile_info(
			bms_name, resource_root, PackedByteArray(),
			mission.is_wire_header_only())
	# Each stage below presents the plan's anchor when it starts.
	_world.load_progress.emit(MissionData.load_progress_percent(MissionData.LOAD_STAGE_ENVIRONMENT))
	timeline.span("environment")
	if not _load_environment(env_name):
		_world.load_failed.emit("failed to load %s" % env_name)
		timeline.finish()
		return ERR_CANT_OPEN
	_apply_mission_environment_overrides(mission)
	# Initialize the exact mission clock and the reset weather owner before the
	# runtime is constructed. The authority publishes this T0 sample after setup
	# but before play, so its first network tick cannot observe stale/default data.
	var mission_info: Dictionary = mission.get_info()
	_world._mission_clock_start_q8_8 = int(mission_info.get("start_time", 0))
	_world._mission_clock_minutes_per_day = int(mission_info.get(
			"minutes_per_day", MissionEnvironment.DEFAULT_MINUTES_PER_DAY))
	if _world._env != null:
		_world._env.configure_mission_clock(_world._mission_clock_start_q8_8, _world._mission_clock_minutes_per_day)
	_prepare_world_driven_weather()
	timeline.end_span()
	_world.load_progress.emit(MissionData.load_progress_percent(MissionData.LOAD_STAGE_TERRAIN))
	timeline.span("terrain")
	if not _load_terrain(trn):
		_world.load_failed.emit("failed to load %s" % trn)
		timeline.finish()
		return ERR_CANT_OPEN
	timeline.end_span()
	_world.load_progress.emit(MissionData.load_progress_percent(MissionData.LOAD_STAGE_OBJECTS))

	_world._loaded_mission = mission
	# The mission attribute that forces the indoors accum bit every frame.
	# [orig: Bms_AttribFlags & 0x10 @ 0x5ca1c8]
	_world._mission_forces_indoors = (int(mission.get_info().get("attrib_flags", 0)) & MissionData.ATTRIB_FORCE_INDOORS) != 0
	timeline.span("objects")
	_place_mission_objects(mission, timeline)
	timeline.end_span()
	_world.load_progress.emit(MissionData.load_progress_percent(MissionData.LOAD_STAGE_RUNTIME))
	timeline.span("runtime")
	var runtime_error := _start_runtime(mission, bms_name)
	timeline.end_span()
	if runtime_error != OK:
		timeline.finish()
		unload()
		return runtime_error
	# The non-foliage loaded-.3DI page freezes once here (the load plan's
	# freeze witness): MissionPresentation.setup has now resolved the placed/wire
	# mission models (including collision/husk definitions); late network spawns
	# must not change this page.
	var challenge_sim: Simulation = _world._runtime.get_sim()
	if challenge_sim != null and not wire_header_join:
		if challenge_sim.is_joiner():
			_world._prewarm_loaded_model_challenge_definitions()
		challenge_sim.finalize_loaded_model_challenge_snapshot()
	_world.load_progress.emit(MissionData.load_progress_percent(MissionData.LOAD_STAGE_AUDIO))
	timeline.span("audio")
	_start_mission_audio(mission, bms_name)
	timeline.end_span()
	_world.load_progress.emit(MissionData.load_progress_percent(MissionData.LOAD_STAGE_EFFECTS))
	timeline.span("effects")
	_start_effect_world()
	timeline.end_span()
	# Warm the effect catalog while the loading screen still covers the frame:
	# the first live spawn otherwise pays the deferred texture resolves + the
	# renderer's first-draw pipeline compiles as a ~90 ms hitch on the player's
	# first shot (measured: first-fire tap 92.9 ms -> repeat 12.5 ms). Retail
	# pays this at load (the load plan's effect-system witness).
	timeline.span("effects_warm")
	warm_effect_world_catalog()
	timeline.end_span()
	_world.load_progress.emit(MissionData.load_progress_percent(MissionData.LOAD_STAGE_FINISH))
	timeline.finish()
	_world._loaded_mission_file = bms_name
	_world._world_ready = true
	_set_water_world_rendering_enabled(true)
	build_minimap_water_mask()
	_world.load_progress.emit(MissionData.LOAD_PROGRESS_COMPLETE)
	_world._debug_views.on_loaded()
	_world.world_loaded.emit()
	return OK


# Mount `dir` as the runtime resource root: PFF archives are the packed game data,
# the `/exp <name>` flag (or persisted setting) layers an expansion over the base,
# loose files override the archives only under the `/d` dev flag, and the `/game <code>`
# flag (or persisted setting, default "jo") selects the SCR decode key so demo data
# decodes correctly. Emits load_failed and returns null on a bad root.
# The expansion here is the LOCAL choice, which is only authoritative for single-player and
# for hosting. A joiner's is the HOST's, learned after this mount and reconciled by
# NetSessionDrive._reconcile_join_expansion before any host data is read (D-NET-178).
func _mount_runtime_root(dir: String) -> ResourceRoot:
	var resource_root := ResourceRoot.new()
	var expansion := LaunchFlags.expansion(ResourceDirSettings.get_expansion())
	var game := LaunchFlags.game(ResourceDirSettings.get_game())
	if resource_root.mount_runtime(dir, expansion, LaunchFlags.loose_override_enabled(), game) != OK:
		_world.load_failed.emit(resource_root.get_last_error())
		return null
	return resource_root


# Populate the world with the mission's placed objects under a MissionObjects node.
# Uses the same shell-agnostic placer as every other mission load path.
func _place_mission_objects(mission: MissionData, timeline: PerfTimeline = null) -> void:
	if _world._resource_root == null or mission == null:
		return
	_world._placer = MissionObjectPlacer.create(_world._resource_root, null)
	# The placer's Avatars.def is the one registry every player visual resolves
	# against; the same table projects the local profile the sim stamps.
	var avatar_db: AvatarDatabase = _world._placer.get_avatar_db()
	var sim := _world.get_sim()
	if sim != null:
		sim.set_character_avatar_database(avatar_db)
	if avatar_db != null:
		_world._local_character_profile = NetSessionDrive.character_join_profile_from_database(
				avatar_db, _world._local_player_spawn_loadout)
	else:
		push_warning("GameWorld: Avatars.def unavailable; players draw their item model")
		_world._local_character_profile = {}
	_world._panm_clock.sample_frame()
	_world._placer.set_panm_clock(_world._panm_clock)
	var options := {}
	# A wire-header join deliberately has no authored body records. The load stream
	# creates native pools 2/1/3 from S2C 0x10/0x0D/0x20 at exact handles; remote
	# pool-0 organics arrive in 0x0C and every live pose advances through 0x0A.
	# MissionPresentation presents those decoded rows directly instead of deferring them
	# onto nonexistent local BMS placements (D-NET-194). Explicit-mission/debug
	# joins still use their complete document.
	if timeline != null:
		options["timeline"] = timeline
	# Pulse the load-progress screen from inside the model-load loop at the
	# object stage's constant value (the load plan's per-model pulse witness).
	options["progress"] = func() -> void: _world.load_progress.emit(
			MissionData.load_progress_percent(MissionData.LOAD_STAGE_OBJECTS))
	_world._mission_stats = _world._placer.place(mission, _world, options)
	_world._apply_occlusion_culling_policy()
	# Static tile shadows are composed from the placer's resolved ObjectData and
	# exact entity transforms. Attach only after place() has finished building
	# that immutable mission snapshot; Terrain invalidates any pre-placement
	# cache pages when the producer becomes live.
	_world._terrain.set_static_shadow_placer(_world._placer)
	print_verbose("GameWorld: placed %d mission objects (%d batched / %d animated, %d unresolved, %d markers)" % [
		int(_world._mission_stats.placed),
		int(_world._mission_stats.batched),
		int(_world._mission_stats.animated),
		int(_world._mission_stats.unresolved),
		int(_world._mission_stats.markers),
	])


## Tear down a loaded world so the shell can return to the menu (or load a
## different mission) without the previous world lingering. Frees the dynamically
## placed MissionObjects subtree and resets the load state; the terrain /
## environment scene nodes are kept in place and rebuilt by the next load_*().
## Safe to call when nothing is loaded.
func unload() -> void:
	_world._world_ready = false
	_world._minimap_water_mask = null
	_world.minimap_water_changed.emit(null)
	_world._join_wire_assets_pending = false
	_world._join_wire_til_applied = false
	_world._join_wire_assets_failed = false
	_world._join_wire_asset_failure_emitted = false
	_world._device_frame._stop_water_render_stats()
	# Net-session teardown: the preload sim/root, the notification latches, the
	# typed request staging, and the NovaWorld gate registration.
	_world._net_drive.reset()
	# The environment's weather view points into the departing sim's World:
	# detach before the runtime (and its off-tree sim) is freed.
	if _world._weather != null:
		_world._weather.bind_simulation(null)
	_set_weather_world_tick_driven(true)
	_set_water_world_rendering_enabled(false)
	if _world._env != null:
		_world._env.set_underwater_view(false)
		_world._env.set_underwater_overlay_view(false)
	_world._local_player_spawn_loadout = {}
	_world._local_character_profile = {}
	_clear_mission_tile_info()
	_world._device_frame._restore_idle_frame_clear_color()
	# Point-light output is a RenderingServer global, so retire it before the
	# placed nodes begin their deferred queue_free teardown. The director also
	# disconnects its wire-node exit hooks here; those hooks must not race the
	# whole-world reset or leak a prior mission's pool into the menu frame.
	if _world._light_director != null:
		_world._light_director.reset()
	# Release Terrain's Ref before the MissionObjects nodes and owning placer.
	# This also invalidates pages composed with the departing caster snapshot.
	if _world._terrain != null:
		_world._terrain.set_static_shadow_placer(null)
	_world._apply_occlusion_culling_policy()
	var container := _world.get_node_or_null(NodePath("MissionObjects"))
	if container != null:
		container.queue_free()
	# Per-item attached-effect owner keys reference nodes in that container —
	# never let a reload's provider resolve against freed instances.
	_world._item_fx.reset()
	# Debug-view teardown: the retain/free split (user-point re-arm vs freed
	# overlays vs the deliberately surviving particle/pick stack) lives in the set.
	_world._debug_views.on_unload()
	if _world._mission_audio != null:
		_world._mission_audio.teardown()
	# Tear down the game music context [orig: AudioVM_StopMusicContext @ 0x671e00].
	# The game shell re-opens menu music on its return to the front end.
	MusicService.stop_context()
	# Blink frame gates and every occlusion override reset with the mission
	# [orig: the letter-bit clear @ 0x525c45 at mission start] — an unload while
	# indoors must not leave the next mission's terrain/sky/water hidden. The
	# pass clears the shared present-visibility intent FIRST (the pre-extraction
	# unload cleared it up top), so its release walk falls back to
	# sim.entity_present_visible — see OcclusionFramePass.reset.
	_world._occlusion.reset()
	_world._mission_forces_indoors = false
	_world.set_local_player_nvg_view(false, 0)
	if _world._env != null and _world._env.environment_data != null:
		_world._env.environment_data.clear_mission_overrides()
	_set_mission_water_height_override(NAN)
	_world._loaded_mission = null
	_world._loaded_mission_file = ""
	if _world._runtime != null:
		_world._runtime.queue_free()  # frees its off-tree sim too (MissionPresentation._exit_tree)
	_world._runtime = null
	if _world._effect_world != null:
		_world._effect_world.release_runtime_renderer_resources()
		_world._effect_world.queue_free()
		_world._effect_world = null
	_world._mission_audio = null
	_world._placer = null
	_world._weapon_db = null  # re-resolves against the next load's mounted root
	_world._local_weapon_dict = {}
	# The decoded view record is keyed on the resolved name; the next mission
	# re-decodes from ITS weapon.def even when the name repeats, or the memo
	# would short-circuit with the dict above left empty.
	_world._viewmodel_def_name = ""
	_world._viewmodel_def = null
	_world._local_weapon_preserve_slot_state = false
	# Armory selections belong to the entity from the mission being torn down.
	# A new spawn must resolve from its own equipped AdmDef instead of inheriting
	# either the previous mission's override or its authored NONE state.
	_world._viewmodel_weapon_override = ""
	_world._viewmodel_weapon_cleared = false
	_world._mission_stats = {}


func _load_environment(env_path: String) -> bool:
	if _world._env == null:
		return true
	var env := EnvFile.new()
	if env.load_from_resource_root(_world._resource_root, env_path) != OK:
		push_warning("GameWorld: failed to load environment '%s'" % env_path)
		return false
	# MissionEnvironment's setter reloads + pushes shader globals on assignment.
	_world._env.environment_data = env
	# The overcast table the overcast blend cross-fades against: overcast.def
	# appended after the .trn pass (stock .trn files carry no TOD blocks)
	# (retail Environment_LoadTimeOfDayConfig @ 0x57db30).
	var overcast := EnvFile.new()
	if overcast.load_from_resource_root(_world._resource_root, "overcast.def") == OK:
		_world._env.overcast_data = overcast
	else:
		_world._env.overcast_data = null
	# GameWorld retains one Weather node across loads. A replacement ENV is
	# a discrete state change: retail snaps every color block to the new mission
	# targets instead of easing over from the previous mission's currents.
	var weather: Weather = _world._weather
	if weather != null:
		weather.resync_colors()
	if _world._celestial != null:
		_world._celestial.set_resource_root(_world._resource_root)
	if _world._precipitation != null:
		_world._precipitation.set_resource_root(_world._resource_root)
	if _world._environment_cube != null:
		_world._environment_cube.force_capture()
	return true


## Apply the mission's attrib-gated water/fog overrides onto the loaded env via
## EnvFile's non-persistent override layer [orig: Game_LoadTerrainDuringConnect
## @ 0x520710]. The base .env is never mutated.
func _apply_mission_environment_overrides(mission: MissionData) -> void:
	if mission == null:
		return
	var overrides: Dictionary = mission.get_environment_overrides()
	# EnvFile owns the other live-view overrides, while water keeps the BMS
	# rung distinct so a flagged zero still beats a nonzero TRN height.
	if _world._env != null:
		var env_data: EnvFile = _world._env.environment_data
		if env_data != null:
			if overrides.is_empty():
				env_data.clear_mission_overrides()
			else:
				env_data.apply_mission_overrides(overrides)
	var mission_water := NAN
	if overrides.has("water_height_world"):
		mission_water = float(overrides["water_height_world"])
	_set_mission_water_height_override(mission_water)


func _set_mission_water_height_override(world_height: float) -> void:
	if _world._water != null:
		_world._water.set_mission_water_height_override(world_height)


func _set_water_world_rendering_enabled(enabled: bool) -> void:
	if _world._water != null:
		_world._water.set_world_rendering_enabled(enabled)


func _set_weather_world_tick_driven(enabled: bool) -> void:
	var weather: Weather = _world._weather
	if weather != null:
		weather.set_world_tick_driven(enabled)


func _prepare_world_driven_weather() -> void:
	var weather: Weather = _world._weather
	if weather != null:
		weather.prepare_world_driven()
	else:
		_set_weather_world_tick_driven(true)


func _prepare_autonomous_weather() -> void:
	var weather: Weather = _world._weather
	if weather != null:
		# A world without a mission runs the environment's standalone weather
		# home: drop any bound Simulation first.
		weather.bind_simulation(null)
		weather.prepare_autonomous()
	else:
		_set_weather_world_tick_driven(false)


# The witnessed mission-start environment boundary runs natively on the
# weather device (Weather.run_mission_start_boundary): the World's weather
# seed from the loaded .env + the BMS clock, the authority's WAC direct
# execution, the initializer + 255-tick settle, the baseline seal.
func _run_mission_start_environment_boundary() -> void:
	var weather: Weather = _world._weather
	if weather != null:
		weather.run_mission_start_boundary(_world.get_sim(),
				_world._mission_clock_start_q8_8, _world._mission_clock_minutes_per_day)


# Retail loads <mission>.til into one shared g_TerrainTileArray used by
# terrain overlays/surface overrides, network initial state, and both foliage
# generators' radius-2 blocker.
# Its file probe/read force loose-first around this one load.
# [orig: Terrain_LoadTileInfoFile @ 0x60a740, policy force @ 0x60a74e;
# Terrain_GetSurfaceTypeAtPosition @ 0x606510;
# Foliage_PathBlockedByPlacedTile @ 0x606490]
func _load_mission_tile_info(bms_name: String, resource_root: ResourceRoot,
		wire_til_bytes: PackedByteArray = PackedByteArray(),
		wire_is_authoritative := false) -> void:
	_clear_mission_tile_info()
	if resource_root == null:
		return
	# A joining retail client consumes the host's paged S2C 0x45 bytes. An empty
	# payload means the host emitted no terrain overlay; it must not fall back to
	# a same-named local .til and accidentally render a different custom map.
	if wire_is_authoritative:
		if wire_til_bytes.is_empty():
			return
		var wire_tile_info := TerrainTileInfo.new()
		if wire_tile_info.load_from_bytes(wire_til_bytes) != OK:
			push_warning("GameWorld: failed to parse host S2C 0x45 terrain tile stream.")
			return
		_world._mission_tile_info = wire_tile_info
		_world._mission_til_bytes = wire_til_bytes
		return
	var mission_name := bms_name.get_file()
	if mission_name.is_empty():
		mission_name = bms_name
	var til_name := mission_name.get_basename() + ".til"
	if not resource_root.has_file(
			til_name, ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST):
		return
	var til_bytes := resource_root.read_file(
			til_name, ResourceRoot.LOOKUP_FORCE_LOOSE_FIRST)
	if til_bytes.is_empty():
		return
	var tile_info := TerrainTileInfo.new()
	if tile_info.load_from_bytes(til_bytes) != OK:
		push_warning("GameWorld: failed to parse mission tile file '%s'." % til_name)
		return
	_world._mission_tile_info = tile_info
	_world._mission_til_bytes = til_bytes


func apply_join_wire_til_if_ready() -> bool:
	if not _world._join_wire_assets_pending:
		return not _world._join_wire_assets_failed
	var sim: Simulation = _world._runtime.get_sim() if _world._runtime != null else null
	if sim == null or not sim.is_joiner():
		return true
	var til_state := sim.get_join_terrain_til_state()
	if til_state == Simulation.JOIN_TERRAIN_TIL_INVALID:
		_world._join_wire_assets_failed = true
		return false
	if til_state not in [
			Simulation.JOIN_TERRAIN_TIL_ABSENT,
			Simulation.JOIN_TERRAIN_TIL_RECEIVING,
			Simulation.JOIN_TERRAIN_TIL_COMPLETE]:
		_world._join_wire_assets_failed = true
		return false
	# Check Invalid before this latch: an extra semantic 0x45 after a completed
	# stream must not be hidden by an already-applied terrain override.
	if _world._join_wire_til_applied:
		return true
	if til_state in [Simulation.JOIN_TERRAIN_TIL_ABSENT,
			Simulation.JOIN_TERRAIN_TIL_RECEIVING]:
		return true
	var til_bytes := sim.get_join_terrain_til()
	if til_bytes.is_empty():
		_world._join_wire_assets_failed = true
		return false
	var tile_info := TerrainTileInfo.new()
	if tile_info.load_from_bytes(til_bytes) != OK:
		_world._join_wire_assets_failed = true
		return false
	_world._mission_tile_info = tile_info
	_world._mission_til_bytes = til_bytes
	_world._join_wire_til_applied = true
	# S2C 0x45 arrives only after the client releases the world-ready gate, so
	# terrain already exists. Both setters invalidate/rebuild their derived data;
	# the loading screen remains raised until settle_join_wire_assets below.
	if _world._terrain != null:
		_world._terrain.tile_info_override = tile_info
	if _world._dispatcher != null:
		_world._dispatcher.tile_info = tile_info
	build_minimap_water_mask()
	return true


## Complete the wire-only part of a retail join once the protocol reaches its
## deployment/admission boundary. NetSessionDrive calls this before revealing
## the world (or deploy map), guaranteeing the optional 0x45 overlay and the
## renderer-backed C2S 0x3D snapshot reflect the completed initial stream.
func settle_join_wire_assets() -> bool:
	if _world._join_wire_assets_failed:
		return false
	if not _world._join_wire_assets_pending:
		return true
	if not apply_join_wire_til_if_ready():
		return false
	var sim: Simulation = _world._runtime.get_sim() if _world._runtime != null else null
	if sim == null or not sim.is_joiner():
		_world._join_wire_assets_failed = true
		return false
	var til_state := sim.get_join_terrain_til_state()
	if til_state == Simulation.JOIN_TERRAIN_TIL_RECEIVING \
			or til_state == Simulation.JOIN_TERRAIN_TIL_INVALID \
			or (til_state == Simulation.JOIN_TERRAIN_TIL_COMPLETE \
					and not _world._join_wire_til_applied) \
			or til_state not in [
				Simulation.JOIN_TERRAIN_TIL_ABSENT,
				Simulation.JOIN_TERRAIN_TIL_COMPLETE]:
		_world._join_wire_assets_failed = true
		return false
	_place_streamed_mission_objects(sim)
	_world._prewarm_loaded_model_challenge_definitions()
	sim.finalize_loaded_model_challenge_snapshot()
	_world._join_wire_assets_pending = false
	return true


## A header-only joiner owns no authored body records, so its pools 1-3 arrive
## as the host's S2C 0x10/0x0D/0x20 world stream and are materialized into the
## native World at their exact wire handles. Once that stream's static pools
## are complete the sim stamps each row with a placed identity; this places
## them through the SAME MissionObjectPlacer path single player and the host
## use (batched static populations, terrain static shadows, occlusion keying,
## the per-entity sun query) and re-keys the presenters' index so the wire
## pass stops drawing them as individual animated nodes. Retail's client draws
## its streamed pools through the same sector renderer as the host; there is
## no per-role render path.
func _place_streamed_mission_objects(sim: Simulation) -> void:
	if _world._placer == null or sim == null:
		return
	var records: Array = sim.get_streamed_placement_records()
	if records.is_empty():
		return
	var options := {"skip_kinds": [MissionData.KIND_ORGANIC]}
	_world._mission_stats = _world._placer.place_entities(records, _world, options)
	if _world._runtime != null:
		_world._runtime.rebind_placed_entities(_world._placer)
	if _world._occlusion != null:
		_world._occlusion.rebind_placed_nodes()
	# The authored .def item effects and effect lights attached at load against
	# an empty placer; re-attach against the placed sources (the same pair the
	# effect-catalog warm-up re-runs).
	if _world._item_fx != null:
		_world._item_fx.reattach()
	if _world._light_director != null:
		_world._light_director.reattach()
	print_verbose("GameWorld: placed %d streamed mission objects (%d batched / %d animated, %d unresolved, %d markers)" % [
		int(_world._mission_stats.placed),
		int(_world._mission_stats.batched),
		int(_world._mission_stats.animated),
		int(_world._mission_stats.unresolved),
		int(_world._mission_stats.markers),
	])


## The revealed world must never race the budgeted cold wire materialization:
## NetSessionDrive holds the join-admission edge until the wire presenter's
## deferred-spawn queue drains behind the loading/DEATH hold. Trivially true
## with no runtime, a harness stub runtime, or no wire presenter.
func is_join_wire_present_drained() -> bool:
	var runtime := _world._runtime as MissionPresentation
	return runtime == null or runtime.join_wire_present_pending() == 0


## Fail the streamed-asset leg once per join. Both the per-frame runtime driver
## and the frame-polled admission observer can observe the same protocol edge;
## routing them through one latch prevents duplicate load_failed emissions.
func report_join_wire_asset_failure(reason: String) -> void:
	_world._join_wire_assets_failed = true
	_world._join_wire_assets_pending = false
	if _world._join_wire_asset_failure_emitted:
		return
	_world._join_wire_asset_failure_emitted = true
	_world.load_failed.emit(reason)


func _clear_mission_tile_info() -> void:
	_world._mission_tile_info = null
	_world._mission_til_bytes = PackedByteArray()
	if _world._terrain != null:
		_world._terrain.tile_info_override = null
	if _world._dispatcher != null:
		_world._dispatcher.tile_info = null


func _load_terrain(trn_path: String) -> bool:
	var data := TerrainData.new()
	if data.load_from_resource_root(_world._resource_root, trn_path) != OK:
		return false
	_world._terrain.tile_info_override = _world._mission_tile_info
	_world._terrain_data = data
	_world._terrain.terrain_data = data
	if _world._slot_shadow != null:
		# The shadow anchor march probes this terrain through the engine's
		# Terrain_GetHeightAtPosition port (the cite lives with the native
		# SlotShadow planner, godot/src/env/slot_shadow.cpp).
		_world._slot_shadow.set_terrain_data(data)
	_world._terrain.build()
	if _world._water != null:
		_world._water.terrain_data = data
	if _world._environment_cube != null:
		_world._environment_cube.terrain_data = data
	if _world._celestial != null:
		# The glare occlusion rays march this terrain (env #14).
		_world._celestial.terrain_data = data
	_configure_foliage()
	return true


# Runtime foliage: Terrain supplies the retail 16-unit detail-cell set;
# the sim's crouched/prone infantry supply the distant silhouette anchors
# (see tick()). Sampling and deterministic candidate generation stay in the
# fresh native runtime.
func _configure_foliage() -> void:
	if _world._dispatcher == null or _world._terrain_data == null:
		return
	# The runtime source already supplies height, detail/model foliage indices,
	# colormap, and change invalidation. Binding the same TerrainData again as
	# the fallback colormap source attempts a duplicate terrain_changed connection
	# in Godot and makes mission reloads report ERR_INVALID_PARAMETER.
	_world._dispatcher.terrain_data = _world._terrain_data
	_world._dispatcher.tile_info = _world._terrain.tile_info_override
	var defs: Array = _world._terrain_data.get_foliage_defs()
	_world._dispatcher.configure_slots(
		defs,
		VegAssets.resolve_slot_meshes(_world._resource_root, defs),
		VegAssets.resolve_slot_fd_textures(_world._resource_root, defs)
	)
	for diagnostic_value in _world._dispatcher.get_slot_diagnostics():
		var diagnostic := diagnostic_value as Dictionary
		var status := String(diagnostic.get('status', ''))
		if status == 'missing_mesh' or status == 'invalid_mesh':
			push_warning(
				"GameWorld: foliage slot %d graphic '%s' disabled (%s)." % [
					int(diagnostic.get('slot', -1)),
					String(diagnostic.get('graphic', '')),
					status,
				]
			)
		elif status == 'enabled' and not bool(diagnostic.get('fd_texture_loaded', false)):
			push_warning(
				"GameWorld: foliage slot %d graphic '%s' has no :fd texture; appearance is degraded." % [
					int(diagnostic.get('slot', -1)),
					String(diagnostic.get('graphic', '')),
				]
			)


# Start the shared mission runtime driver: it promotes the mission, builds the present index over the
# placed MissionObjects, and each tick applies every entity's transform + part animations (PLAYPARTANIM,
# applied in-engine) + visibility onto its model. The game runs it at the faithful 62-frame cadence and
# drives it explicitly from tick(); its drained side effects route through
# _on_runtime_effects. A reload reuses this GameWorld, so any prior runtime is freed in unload() first.
func _start_runtime(mission: MissionData, bms_name: String) -> int:
	var container := _world.get_node_or_null(NodePath("MissionObjects"))
	_world._runtime = MissionPresentation.new()
	_world._runtime.name = "MissionPresentation"
	_world.add_child(_world._runtime)
	if _world._frame_stats != null:
		_world._runtime.set_frame_stats(_world._frame_stats)
	var mission_file := bms_name.get_file()
	if mission_file.is_empty():
		mission_file = bms_name
	var mission_label := mission.get_mission_name().strip_edges()
	if mission_label.is_empty():
		mission_label = mission_file.get_basename()
	# A mission with no AI still ticks (BMS events / WAC); only a promote failure leaves a null sim.
	# Hand the loaded terrain to the runtime so promoted AI grounds on it (entities hug the terrain),
	# and the resource root so soldiers resolve their .adm/.bad root-motion clips.
	var opts := MissionSetupOptions.new()
	opts.terrain = _world._terrain_data
	opts.resource_root = _world._resource_root
	opts.wac_basename = bms_name.get_basename()
	opts.mission_file = mission_file
	opts.mission_name = mission_label
	opts.spawn_names = PackedStringArray([mission_label])
	# The placer's item database (item_id -> anim_def), so each soldier grounds off its own
	# model's .adm clip set (per-entity capsule_bottom), not the shared default. [D-INF-6]
	opts.item_db = _world._placer.get_item_db() if _world._placer != null else null
	if not _world._local_character_profile.is_empty():
		opts.local_character_profile = _world._local_character_profile.duplicate(true)
	# Serve-and-play hosts run the listen server AND spawn their own player (ADR 0011/0012, net-re
	# §5.2b/§5.38). A DEDICATED host (config "dedicated") serves WITHOUT a local player — same listen
	# server, just no own-player spawn; main_game skips the HUD when there is no local player. Diagnostic
	# previews opt out via _playable.
	# Terrain-tile (.til) bytes for the S2C 0x45 terrain-tile load a listen host streams to joiners so
	# their g_loading_progress climbs 5 -> 6 and terrain finishes loading (net-re §5.37). The tile-overlay
	# .til is named after the MISSION (localres.pff: ASH_I5A.til), not the terrain tileinfo
	# [orig: Terrain_LoadTileInfoFile @ 0x60a740;
	# serialize_terrain_tiles @ 0x6080f0]. Reuse the payload parsed before terrain build.
	if not _world._mission_til_bytes.is_empty():
		opts.terrain_til = _world._mission_til_bytes
	opts.playable = _world._playable and not _world._net_drive.pending_dedicated()
	# Stamp the staged net-session request (typed record + derived staging +
	# the surrendered preload sim, consumed once per load) onto the runtime's
	# options — MissionPresentation alone adopts opts.simulation (ADR 0011/0012).
	_world._net_drive.stage_runtime_options(opts)
	# The placer + environment node let the wire present pass resolve + light its
	# remote-entity avatars (build_player_animated_model): every remote row on a
	# joiner, and the admitted players' synthetic-origin rows on the host.
	opts.placer = _world._placer
	# The occlusion-claim set the present pass consults (two-bit visibility
	# ownership; see OcclusionFramePass._set_occlusion_hidden). Shared by
	# reference: the pass created these dictionaries once and mutates them in
	# place across the mission's occlusion frames — hand the SAME instances.
	opts.occlusion_hidden_ids = _world._occlusion.occlusion_hidden_ids()
	opts.present_visibility = _world._occlusion.present_visibility()
	# The fire present pass's providers (AI/remote fire sound + muzzle + tracers): audio
	# and effect world resolve lazily (mission audio is set up after the runtime), the
	# listener is the same camera position the audio render pass ticks with.
	opts.fire_audio = _world.get_mission_audio
	opts.fire_fx = _world.get_effect_world
	opts.fire_listener = _world._fire_listener_position
	# The destruction/throwable present passes anchor their wreck/piece/move
	# effect groups through the ItemEffectDirector's owner-anchor registry
	# (the typed seam; GameWorld's register_effect_anchor delegates to the
	# same instance).
	opts.effect_anchors = _world._item_fx
	# The scar present pass reads the fog distance + the combined terrain light
	# off the live environment node each present frame (world-wac-ai-re §24.9).
	opts.environment_node = _world.get_environment_node
	# The dynamic light-pool routes (renderer/light_scene.h witness map): the
	# MF_Light muzzle glow per presented fire, the death flash per husk death.
	if _world._light_director != null:
		opts.muzzle_light = _world._light_director.on_muzzle_fire
		opts.death_light = _world._light_director.on_death_light
	_world._runtime.setup(mission, container, opts)
	if _world._runtime.get_sim() == null:
		var setup_error := int(_world._runtime.get_setup_error())
		var lan_bind_failure := opts.net_transport == "lan"
		var bind_port := opts.bind_port
		# Free before emitting: a load_failed handler may synchronously tear
		# the world down (the game shell returns to the menu via unload()),
		# and unload() frees _runtime — emitting first turned this leg into a
		# null-instance free on reentry.
		_world._runtime.free()
		_world._runtime = null
		if lan_bind_failure:
			_world.load_failed.emit("host start: could not bind LAN UDP port %d" % bind_port)
		else:
			_world.load_failed.emit("failed to start mission runtime")
		return setup_error if setup_error != OK else ERR_CANT_CREATE
	_run_mission_start_environment_boundary()
	_world._sync_runtime_profiling()
	# The player profile's saved weapon kits, loaded before ANY kit is applied or
	# submitted: in a net session the original's spawn kit is a page of this file,
	# selected by the very class byte it also puts on the wire
	# [orig: Game_StartMission @ 0x525767-0x525836].
	_load_player_weapon_profile()
	_world._player_visuals._apply_local_player_spawn_loadout()
	_world._runtime.set_presentation_time_ms(_world._panm_clock.time_ms)
	if _world._water != null:
		# Water may have been built before the runtime existed — re-push the
		# sim-side plane the footstep/landing legs compare feet against.
		_world._runtime.get_sim().set_water_z(float(_world._water.water_height))
	_world._runtime.effects_drained.connect(_world._on_runtime_effects)
	_world._runtime.fixed_tick_completed.connect(_world._on_runtime_fixed_tick)
	_world._runtime.simulation_restarted.connect(_world._on_runtime_simulation_restarted)
	# A browsable listen host: register it with the NovaWorld gate (F1), if one was
	# configured. No-op for single-player, joiners, and pure-LAN play.
	_world._net_drive.on_runtime_started(opts, bms_name)
	# The game starts running (tick() gates on is_playing, so the overlay's
	# transport can pause/step a live mission).
	_world._runtime.play()
	return OK


# The on-disk path of the player profile's weapon file. Retail builds it from the
# ACTIVE expansion name — with an expansion loaded it looks ONLY under that
# expansion's directory (there is no base-game fallback leg), otherwise it reads the
# game root's copy [orig: PlayerProfile_LoadAllFromDisk @ 0x54f4d0, path build
# @ 0x54f68c-@ 0x54f6b7: g_ExpansionName[0] ? "expansion\<name>\weapon.sav" :
# "weapon.sav"]. The mount is the authority on both halves — for a joiner it has
# already been reconciled to the HOST's expansion (D-NET-178), which is what makes
# the profile's ADM index space agree with the host's.
# Load weapon.sav onto the sim: five profile-slot records, each carrying a per-side
# class byte and the five 2048-byte class kit pages the MP loadout submit indexes BY
# that class byte [orig: PlayerProfile_LoadAllFromDisk @ 0x54f4d0 — header check
# @ 0x54f586 ("FPBC"/"0211"), the 5 x 0x1080C record reads]. This is a plain disk
# file, not archive content, so it is read through the mount's directory rather than
# the VFS. A file that is absent or not a profile is NOT a load failure: retail's
# own miss leaves PlayerProfile_InitDefaults' shipped defaults in place (BLUE/RED
# class 8, one weapon name per class page) [orig: @ 0x54bb40].
func _load_player_weapon_profile() -> void:
	var sim := _world.get_sim()
	if sim == null:
		return
	var path := PlayerProfile.weapon_profile_path(_world._resource_root)
	if path.is_empty():
		return
	if not FileAccess.file_exists(path):
		print_verbose("GameWorld: no weapon.sav at %s — keeping the shipped profile defaults" % path)
		return
	var err := int(sim.load_weapon_profile(path))
	if err != OK:
		push_warning("GameWorld: weapon.sav at %s not accepted (error %d) — keeping the shipped profile defaults"
				% [path, err])


# Place real ambient sounds at the mission's sound markers: load the co-named .LWF
# + gamelocl.LWF, resolve each marker to a sound set by name, and spawn looping 3D
# voices. Reuses the placer's item database for the item_id -> soundloop_1..4 lookup.
func _start_mission_audio(mission: MissionData, bms_name: String) -> void:
	var item_db: ItemDatabase = _world._placer.get_item_db() if _world._placer != null else null
	_world._mission_audio = MissionAudio.new(_world._resource_root, item_db)
	# Sound occlusion runs LOS through the sim's collision world + terrain
	# [orig: Sound_ApplyOcclusionDistance @ 0x529970]; hosts without a sim mix
	# unoccluded.
	_world._mission_audio.set_simulation(_world.get_sim())
	var stats := _world._mission_audio.setup(mission, bms_name, _world)
	if _world._env != null:
		_world._mission_audio.set_time_of_day_hhmm(_world._env.time_of_day)
	print_verbose("GameWorld: mission audio — %d/%d sound markers resolved, %d bank(s), %d ambient candidate(s), %d/%d physical channel(s) allocated" % [
		int(stats.markers_resolved),
		int(stats.markers_total),
		int(stats.banks_loaded),
		int(stats.ambient_candidates),
		int(stats.physical_channels),
		int(stats.channel_budget),
	])
	# Open the GAME music context + seed the witnessed vars [orig: Game_StartMission
	# @ 0x525581-0x52561b]. Retail gates the open on is_mp_session_peer and STOPS
	# music in single-player; ours opens in ALL sessions — D-MUS-SPGATE
	# (docs/audio/mus-sbf-re.md §Game music driving; SP-as-listen-server, ADR
	# 0009/0011/0012). gamemus's discriminator Var1 stays 0 (never written in
	# retail), so the Multiplayerstart P0 loop plays.
	MusicService.open_game_context(_world._resource_root)


# Build retail's depthspin shore mask directly from the raw CPT height atlas.
# Streamed .til art does not participate in either the sharp colormap base or
# this independent water pass.
func build_minimap_water_mask() -> void:
	_world._minimap_water_mask = null
	var terrain_data := _world.get_terrain_data()
	if terrain_data != null and not Engine.is_editor_hint():
		var live_water := float(_world._water.water_height) if _world._water != null else NAN
		_world._minimap_water_mask = terrain_data.build_minimap_water_mask(live_water)
	_world.minimap_water_changed.emit(_world._minimap_water_mask)


func warm_effect_world_catalog() -> int:
	if _world._effect_world == null:
		return 0
	var warm_pos := Vector3.ZERO
	var cam := _world.get_viewport().get_camera_3d() if _world.is_inside_tree() else null
	if cam != null:
		warm_pos = cam.global_position - cam.global_transform.basis.z * 8.0
	# The persistent master switch is a gameplay preference, not a reason to
	# leave the catalog cold forever. Lift it only across the loading-screen
	# draws; keep the director's persisted switch unchanged and restore the
	# EffectWorld before persistent item effects are reattached.
	var restore_particles_hidden := _world._effect_world.are_particles_hidden()
	if restore_particles_hidden:
		_world._effect_world.set_particles_hidden(false)
	var spawned := int(_world._effect_world.warm_all_effects(warm_pos))
	if spawned <= 0:
		_world._effect_world.reset_runtime_state()
		if restore_particles_hidden:
			_world._effect_world.set_particles_hidden(true)
		return 0
	# The tracer ribbon pipelines compile in the same forced frames.
	if _world._runtime != null:
		_world._runtime.warm_present_pipelines(warm_pos)
	_world._effect_world.advance_fixed_tick(Simulation.tick_dt())
	_world._effect_world.render_now()
	# Pipeline compiles need real draws. Skip the forced frames inside the
	# editor embedder (re-entrant editor drawing); the texture warm above still
	# runs there, and the shipped game is what the full warm protects.
	if _world.is_inside_tree() and not Engine.is_editor_hint():
		# MainGame keeps World hidden behind the opaque loading CanvasLayer.
		# Temporarily expose it so the particle domains, tracer MeshInstance,
		# and deterministic helper quads are actually submitted to force_draw.
		var was_visible := _world.visible
		_world.visible = true
		RenderingServer.force_draw(true)
		_world._effect_world.advance_fixed_tick(Simulation.tick_dt())
		_world._effect_world.render_now()
		RenderingServer.force_draw(true)
		# The reset below cancels any unserviced compositor warm request. Drain
		# the forced draws first so threaded renderers cannot race that cancel.
		RenderingServer.force_sync()
		_world.visible = was_visible
	_world._effect_world.reset_runtime_state()
	if restore_particles_hidden:
		_world._effect_world.set_particles_hidden(true)
	_world._item_fx.reattach()
	if _world._light_director != null:
		_world._light_director.reattach()
	var unresolved := PackedStringArray(
			_world._effect_world.get_unresolved_texture_names()).size()
	print_verbose("GameWorld: effect warm pass — %d effect(s) precompiled, %d unresolved texture(s)" % [
			spawned, unresolved])
	return spawned


# Mission-start load of EVERY mounted .ptl into the runtime effect world
# [orig: CEffectSystem_Init @ 0x5f6070 <- Game_StartMission @ 0x524980 — no fixed
# file list: the loose ptl\*.ptl set and every PFF .ptl entry both parse].
func _start_effect_world() -> void:
	_world._effect_world = EffectWorld.new()
	_world._effect_world.name = "EffectWorld"
	_world.add_child(_world._effect_world)
	_world._effect_world.set_environment_source(_world._env)
	if _world._item_fx.particles_hidden():
		_world._effect_world.set_particles_hidden(true)
	var count := _world._effect_world.load_from_resource_root(_world._resource_root)
	if _world._water != null:
		_world._effect_world.set_water_plane(float(_world._water.water_height),
				_world._water.get_reflection_camera())
		# The sim-side water plane (env.water_z): the footstep water pick, the
		# landing legs, AND the destruction paths (submerged wrecks skip pieces,
		# the wreck fire steams out) all gate on it [orig: Env_WaterHeightFixed
		# @ 0x26C6454; world-wac-ai-re §24]. Idempotent; re-pushed after runtime
		# start too (either side may come up first).
		var water_sim := _world.get_sim()
		if water_sim != null:
			water_sim.set_water_z(float(_world._water.water_height))
	print_verbose("GameWorld: effect world — %d effect(s) across %d .ptl file(s)" % [
		count, _world._effect_world.file_count()])
	# Item-effect wiring — the owner-pose provider, the persistent per-item
	# attaches, and the wire-spawn callback — lives in the director.
	_world._item_fx.on_effect_world_started()
	if _world._light_director != null:
		_world._light_director.reattach()
	# One wire-spawn router for both directors: the runtime callback is
	# single-subscriber, so the world owns the fan-out.
	var wire_runtime: MissionPresentation = _world.get_runtime()
	if wire_runtime != null and _world._light_director != null:
		wire_runtime.set_wire_node_spawned_callback(
				func(node: ObjectModel, kind: int, item_id: int) -> void:
					_world._item_fx.on_wire_node_spawned(node, kind, item_id)
					_world._light_director.on_wire_node_spawned(node, kind, item_id))
