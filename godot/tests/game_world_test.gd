extends GutTest

const WORLD_TEST_ROOT := "game_world_test"


func after_each() -> void:
	_remove_dir_recursive(OS.get_cache_dir().path_join(WORLD_TEST_ROOT))


class TransportRuntimeStub:
	extends Node
	var ticks := 0
	var _playing := false
	func is_playing() -> bool:
		return _playing
	func play() -> void:
		_playing = true
	func pause() -> void:
		_playing = false
	func tick() -> bool:
		ticks += 1
		return true


class FxRuntimeStub:
	extends Node
	func entity_position_for_ssn(ssn: int) -> Variant:
		return Vector3(4, 5, 6) if ssn == 17 else null


class FxWorldStub:
	extends NovaEffectWorld
	var spawns: Array = []
	func spawn_effect_owned(owner_key: Variant, effect: String, position: Vector3,
			orientation: Vector3 = Vector3.ZERO) -> int:
		spawns.append({
			"owner": owner_key,
			"effect": effect,
			"position": position,
			"orientation": orientation,
		})
		return 1


func test_tick_gates_the_runtime_on_its_transport() -> void:
	# The game host's tick must respect MissionRuntime's play flag - the debug
	# overlay's Pause/Step work on a live mission BECAUSE this gate exists
	# (before it, play()/pause() were inert in the game).
	var world := _make_world()
	add_child_autofree(world)
	var runtime := TransportRuntimeStub.new()
	add_child_autofree(runtime)
	world._runtime = runtime
	world._loaded = true

	world.tick(Vector3.ZERO)
	assert_eq(runtime.ticks, 0, "a paused runtime never ticks")
	runtime.play()
	world.tick(Vector3.ZERO)
	assert_eq(runtime.ticks, 1, "a playing runtime ticks once per host frame")
	runtime.pause()
	world.tick(Vector3.ZERO)
	assert_eq(runtime.ticks, 1, "pausing stops it again")
	world._runtime = null
	world._loaded = false


func test_fx2ssn_routes_position_owner_and_up_orientation() -> void:
	var world := _make_world()
	add_child_autofree(world)
	var runtime := FxRuntimeStub.new()
	var effects := FxWorldStub.new()
	add_child_autofree(runtime)
	add_child_autofree(effects)
	world._runtime = runtime
	world._effect_world = effects
	world._route_mission_effects([{"kind": "fx2ssn", "b": 17, "str": "Dust"}])
	assert_eq(effects.spawns.size(), 1)
	assert_eq(effects.spawns[0].owner, 17)
	assert_eq(effects.spawns[0].effect, "Dust")
	assert_eq(effects.spawns[0].position, Vector3(4, 5, 6))
	assert_eq(effects.spawns[0].orientation, Vector3.UP,
			"the documented terrain-normal placeholder must actually reach the emitter")
	world._runtime = null
	world._effect_world = null


func test_load_world_requires_hardcoded_environment_in_global_root() -> void:
	var root := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join("missing_env_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root)
	_write_fixture_file(root.path_join("Dvxi5.trn"), "terrain_name \"Dvxi5\"\n")

	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame

	assert_eq(world.load_world(root), ERR_FILE_NOT_FOUND, "Runtime global root must contain full_00.env next to Dvxi5.trn.")


func test_packaged_scene_instantiates_with_intact_wiring() -> void:
	# game_world.tscn is the embeddable world (the game instances it in
	# main_game.tscn; play-in-editor instances it in a workspace viewport). Pin
	# the extraction: every engine node is present and the intra-scene NodePaths
	# survived the move out of main_game.tscn.
	var packed := load("res://engine/world/game_world.tscn") as PackedScene
	assert_not_null(packed, "the packaged world scene loads")
	var world := packed.instantiate()
	add_child_autofree(world)
	assert_true(world is GameWorld, "the root carries the GameWorld script")
	for child_name in ["NovaTerrain", "NovaEnvironment", "NovaSky", "NovaWeather", "NovaWater", "NovaCelestial"]:
		assert_not_null(world.get_node_or_null(child_name), "%s is in the packaged scene" % child_name)
	assert_not_null(world.get_node_or_null("NovaTerrain/FoliageDispatcher"))
	assert_not_null(world.get_node_or_null("NovaTerrain/TileOverlay"))
	var terrain: NovaTerrain = world.get_node("NovaTerrain")
	assert_eq(terrain.environment_path, NodePath("../NovaEnvironment"), "terrain env path survived extraction")
	assert_eq(terrain.weather_path, NodePath("../NovaWeather"), "terrain weather path survived extraction")


func test_clear_color_environment_renders_the_witnessed_frame_clear() -> void:
	# _update_frame_clear_color() writes the witnessed frame clear into the
	# ClearColor Environment's background_color every frame - but the scene
	# resource decides whether that color ever renders. The Wave-1 scene shipped
	# background_mode = 2 (BG_SKY) with no Sky resource, which renders BLACK and
	# silently swallows the env-#21 clear consumer: a 1px black dome-rim seam in
	# ground views, a black band in aerial views. Pin the mode so it can't drift.
	var packed := load("res://engine/world/game_world.tscn") as PackedScene
	assert_not_null(packed, "the packaged world scene loads")
	var world := packed.instantiate()
	add_child_autofree(world)
	var clear := world.get_node_or_null("ClearColor") as WorldEnvironment
	assert_not_null(clear, "the ClearColor WorldEnvironment is in the packaged scene")
	if clear == null:
		return
	assert_not_null(clear.environment, "ClearColor carries an Environment resource")
	if clear.environment == null:
		return
	assert_eq(clear.environment.background_mode, Environment.BG_COLOR,
		"BG_COLOR renders background_color; BG_SKY with a null sky renders BLACK and silently swallows the witnessed frame clear [orig: Render_ProcessMainSceneFrame @ 0x5ca776..0x5ca792]")
	assert_eq(clear.environment.ambient_light_source, Environment.AMBIENT_SOURCE_DISABLED,
		"Godot ambient must never inject into the witnessed lighting model - all OpenNova materials light themselves; AMBIENT_SOURCE_BG would derive ambient from the clear color")


func test_hidden_world_suppresses_retained_terrain_and_restores_idle_frame_clear() -> void:
	var packed := load("res://engine/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	var clear := world.get_node("ClearColor") as WorldEnvironment
	clear.environment = clear.environment.duplicate()
	var idle_clear := Color(0.01, 0.02, 0.03)
	clear.environment.background_color = idle_clear

	var camera := Camera3D.new()
	camera.position = Vector3(0, 71, 0)
	camera.current = true
	world.add_child(camera)
	add_child_autofree(world)
	world.set_playable(false)
	assert_eq(world.get_current_frame_clear_color(), idle_clear,
		"the scene-authored clear is the menu/loading baseline before a mission presents")

	var root_dir := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join(
		"presentation_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root_dir)
	for source_dir in [
		ProjectSettings.globalize_path("res://../fixtures/godot/dvxi5"),
		ProjectSettings.globalize_path("res://../fixtures/minimal/resources"),
	]:
		for file_name in DirAccess.get_files_at(source_dir):
			assert_eq(DirAccess.copy_absolute(
				source_dir.path_join(file_name), root_dir.path_join(file_name)), OK)

	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)
	world.set_resource_root(root)
	var mission := NovaMissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	assert_true(mission.set_header_string("terrain", "Dvxi5"))
	assert_true(mission.set_header_string("environment", "mnml"))
	assert_eq(world.load_mission_data(mission, "mnml.bms"), OK)
	await get_tree().process_frame
	await get_tree().process_frame

	var terrain := world.get_node("NovaTerrain") as NovaTerrain
	assert_gt(terrain.get_visible_patch_count(), 0,
		"the loaded fixture presents native RenderingServer terrain patches")
	var mission_clear := world.get_current_frame_clear_color()
	assert_ne(mission_clear, idle_clear,
		"the loaded mission replaces the scene-authored frame clear")

	world.visible = false
	await get_tree().process_frame
	assert_eq(terrain.get_visible_patch_count(), 0,
		"a hidden GameWorld must hide native terrain RIDs that bypass Node3D visibility")
	assert_eq(world.get_current_frame_clear_color(), idle_clear,
		"a hidden GameWorld must restore the menu/loading frame clear")

	world.visible = true
	await get_tree().process_frame
	assert_gt(terrain.get_visible_patch_count(), 0,
		"showing the retained world lets terrain traversal present patches again")
	assert_eq(world.get_current_frame_clear_color(), mission_clear,
		"showing the loaded world restores its mission frame clear")

	world.unload()
	await get_tree().process_frame
	assert_eq(world.get_current_frame_clear_color(), idle_clear,
		"an unloaded GameWorld restores the scene-authored frame clear")


func test_water_mirror_camera_sees_the_body_layer_but_never_the_viewmodel() -> void:
	# The reflection layer contract (env #30): the witnessed mirror is a
	# re-render of the WORLD scene - which contains the local player's body -
	# but never the water surface itself and never the first-person overlay,
	# which retail draws as its own near-Z viewport pass [orig:
	# Water_ReflectionPrerender @ 0x5c2780 -> render_main_scene @ 0x5c1240;
	# Player_RenderFirstPersonViewModel @ 0x4ded60]. Pin the packaged scene's
	# mirror cull_mask so first-person arms can never leak back into the
	# reflection (and the FP-mode body, parked on the reflection-only layer by
	# LocalPlayerHost, always renders in it).
	var packed := load("res://engine/world/game_world.tscn") as PackedScene
	assert_not_null(packed, "the packaged world scene loads")
	var world := packed.instantiate()
	add_child_autofree(world)
	var water: NovaWater = world.get_node_or_null("NovaWater")
	assert_not_null(water, "the packaged scene ships the water node")
	if water == null:
		return
	var mirror: Camera3D = water.reflection_camera
	assert_not_null(mirror, "the water builds its mirror camera on ready")
	if mirror == null:
		return
	assert_eq(mirror.cull_mask & NovaWater.VISUAL_LAYER_WATER, 0,
		"the mirrored scene never draws the water surface itself")
	assert_eq(mirror.cull_mask & NovaWater.VISUAL_LAYER_VIEWMODEL, 0,
		"the FP arms/weapon overlay never enters the mirrored scene")
	assert_ne(mirror.cull_mask & NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY, 0,
		"the FP-mode local body DOES render in the mirror")
	assert_ne(mirror.cull_mask & NovaWater.VISUAL_LAYER_WORLD, 0,
		"the mirrored scene renders the normal world")
	assert_eq(water.mesh_instance.layers, NovaWater.VISUAL_LAYER_WATER,
		"the water strip rides the water-only layer the mirror excludes")


func test_game_world_is_playable_by_default_without_env_flag() -> void:
	var world := _make_world()
	add_child_autofree(world)
	assert_true(world.is_playable(), "standalone and editor Play Mission should spawn a player by default")
	world.set_playable(false)
	assert_false(world.is_playable(), "diagnostic previews can explicitly opt out of local-player setup")


func test_load_mission_data_rejects_an_empty_document() -> void:
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	var failures: Array = []
	world.load_failed.connect(func(reason): failures.append(reason))
	assert_eq(world.load_mission_data(null, "x.bms"), ERR_INVALID_PARAMETER)
	assert_eq(world.load_mission_data(NovaMissionData.new(), "x.bms"), ERR_INVALID_PARAMETER,
		"an unloaded document is rejected before any root resolution")
	assert_eq(failures.size(), 2, "both rejections explain themselves via load_failed")


func test_loaded_mission_drives_the_shared_time_of_day_clock() -> void:
	var packed := load("res://engine/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)

	var root := NovaResourceRoot.new()
	var fixture_dir := ProjectSettings.globalize_path("res://../fixtures/minimal/resources")
	assert_eq(root.set_root_dir(fixture_dir), OK)
	world.set_resource_root(root)
	var mission := NovaMissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	mission.set_header_int("start_time", 0x0540)  # unsigned Q8.8 = 05:15
	mission.set_header_int("minutes_per_day", 60)

	assert_eq(world.load_mission_data(mission, "mnml.bms"), OK)
	var audio := world.get_mission_audio()
	assert_not_null(audio)
	if audio == null:
		return
	var env := world.get_node("NovaEnvironment") as NovaEnvironment
	assert_almost_eq(env.time_of_day, 515.0, 0.001,
		"the BMS start time, not the environment node's noon default, initializes the shared clock")

	# 0.128 seconds advances eight fixed 62.5 Hz ticks. The exact clock math is
	# pinned at NovaEnvironment's public seam; this integration assertion pins
	# GameWorld's runtime-tick routing and guards against a reset to stale noon.
	world.tick(Vector3.ZERO, Transform3D(), 0.128)
	var advanced := env.time_of_day
	assert_gt(advanced, 515.0, "runtime ticks advance the authored mission clock")
	assert_lt(advanced, 516.0, "a single frame cannot jump the clock to another hour")
	world.unload()


func test_injected_root_bypasses_settings_mount() -> void:
	# The editor injects its own mounted root; the load must resolve through it
	# (and report ITS directory in errors) instead of mounting from settings.
	var root_dir := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join("injected_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root_dir)
	var injected := NovaResourceRoot.new()
	assert_eq(injected.set_root_dir(root_dir), OK)

	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_resource_root(injected)

	var failures: Array = []
	world.load_failed.connect(func(reason): failures.append(String(reason)))
	assert_eq(world.load_mission("missing.bms"), ERR_FILE_NOT_FOUND,
		"the missing file resolves against the injected root")
	assert_eq(failures.size(), 1)
	assert_string_contains(failures[0], root_dir.get_file(),
		"the error names the injected root's directory, proving no settings mount ran")


func test_successful_mission_load_exposes_the_loaded_file_until_unload() -> void:
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame

	var root := NovaResourceRoot.new()
	var fixture_dir := ProjectSettings.globalize_path("res://../fixtures/minimal/resources")
	assert_eq(root.set_root_dir(fixture_dir), OK)
	world.set_resource_root(root)
	world.mission_file = "boot-option.bms"

	assert_eq(world.load_mission("mnml.bms"), OK)
	assert_eq(world.get_loaded_mission_file(), "mnml.bms",
		"the successful load argument, not the exported boot option, is the active mission")
	assert_eq(world.mission_file, "boot-option.bms",
		"loading does not repurpose the exported boot option as mutable runtime state")

	world.unload()
	assert_eq(world.get_loaded_mission_file(), "",
		"an unloaded world no longer reports a stale active mission")


func test_mission_til_is_shared_by_terrain_foliage_and_cleared_without_file() -> void:
	var root_dir := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join(
		"mission_til_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir), OK)
	var source_dir := ProjectSettings.globalize_path("res://../fixtures/minimal/resources")
	for file_name in DirAccess.get_files_at(source_dir):
		assert_eq(DirAccess.copy_absolute(
			source_dir.path_join(file_name), root_dir.path_join(file_name)), OK)

	var til_bytes := PackedByteArray()
	til_bytes.resize(28)
	til_bytes.encode_u32(0, 0x74696c30)
	til_bytes.encode_u32(4, 1)
	# One entry at world [0,16] x [0,16].
	til_bytes.encode_u32(16, 0)
	til_bytes.encode_u32(20, 0)
	til_bytes[24] = 1
	var til_file := FileAccess.open(root_dir.path_join("mnml.til"), FileAccess.WRITE)
	assert_not_null(til_file)
	if til_file == null:
		return
	til_file.store_buffer(til_bytes)
	til_file.close()

	var resource_root := NovaResourceRoot.new()
	assert_eq(resource_root.set_root_dir(root_dir), OK)
	var packed := load("res://engine/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	world.set_resource_root(resource_root)
	assert_eq(world.load_mission("mnml.bms"), OK)

	var terrain := world.get_node("NovaTerrain") as NovaTerrain
	var dispatcher := world.get_node("NovaTerrain/FoliageDispatcher") as NovaFoliageDispatcher
	var tile_info := terrain.tile_info_override as NovaTerrainTileInfo
	assert_not_null(tile_info)
	if tile_info != null:
		assert_eq(tile_info.get_entry_count(), 1)
		assert_true(tile_info.blocks_foliage(8.0, 8.0, 2.0))
		assert_same(dispatcher.tile_info, tile_info,
			"Terrain composition and foliage exclusion must share the parsed mission resource.")

	world.unload()
	await get_tree().process_frame
	assert_null(terrain.tile_info_override)
	assert_null(dispatcher.tile_info)

	assert_eq(DirAccess.remove_absolute(root_dir.path_join("mnml.til")), OK)
	var no_til_root := NovaResourceRoot.new()
	assert_eq(no_til_root.set_root_dir(root_dir), OK)
	world.set_resource_root(no_til_root)
	assert_eq(world.load_mission("mnml.bms"), OK)
	assert_null(terrain.tile_info_override,
		"A subsequent mission without a co-named TIL cannot inherit stale blockers.")
	assert_null(dispatcher.tile_info)
	world.unload()
	await get_tree().process_frame


func test_unload_drops_the_previous_entitys_armory_viewmodel_state() -> void:
	var world := _make_world()
	add_child_autofree(world)
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/minimal/resources")), OK)
	world.set_resource_root(root)
	assert_eq(world.load_mission("mnml.bms"), OK)

	world.clear_local_player_weapon()
	assert_null(world.local_player_viewmodel_def(), "the authored NONE row has no viewmodel")
	world.unload()

	var old_debug_weapon := OS.get_environment("NOVA_VM_WEAPON")
	OS.set_environment("NOVA_VM_WEAPON", "WPN_M4")
	var restored: PlayerViewmodelDef = world.local_player_viewmodel_def()
	OS.set_environment("NOVA_VM_WEAPON", old_debug_weapon)
	assert_not_null(restored, "a new mission is not stuck with the previous entity's NONE state")
	if restored != null:
		assert_eq(restored.weapon_name, "WPN_M4",
			"the next mission can resolve a weapon after the previous entity selected NONE")


func test_joiner_accepts_novaworld_advertised_mission_basename() -> void:
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	var root := NovaResourceRoot.new()
	var fixture_dir := ProjectSettings.globalize_path("res://../fixtures/minimal/resources")
	assert_eq(root.set_root_dir(fixture_dir), OK)
	world.set_resource_root(root)

	var err := world.load_mission_as_joiner({
		"mission": "mnml",
		"host_ip": "127.0.0.1",
		"port": 9,
	}, "Joiner")
	assert_eq(err, OK, "A NovaWorld host-row basename resolves to its .bms resource.")
	assert_eq(world.get_loaded_mission_file(), "mnml.bms",
		"The normalized filename reaches the active mission/text-table seam.")
	world.unload()


func test_skeleton_debug_builds_and_frees_the_view() -> void:
	# The F3 overlay's "Show skeletons" toggle routes here: enabling builds a child
	# SkeletonDebugView under the world, disabling frees it (mirrors set_pick_debug).
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	assert_false(world.is_skeleton_debug(), "off by default")
	assert_null(world.get_node_or_null("SkeletonDebug"), "...with no view node")

	world.set_skeleton_debug(true)
	assert_true(world.is_skeleton_debug())
	assert_not_null(world.get_node_or_null("SkeletonDebug"), "enabling builds the 3D view")

	world.set_skeleton_debug(false)
	assert_false(world.is_skeleton_debug())
	await get_tree().process_frame  # queue_free lands at frame end
	assert_null(world.get_node_or_null("SkeletonDebug"), "disabling frees it")


func test_hide_foliage_toggles_dispatcher_visibility() -> void:
	# The F3 overlay's "Hide foliage" toggle routes here: it hides/shows the foliage
	# dispatcher node (whose ArrayMesh batches render the scattered vegetation).
	var world := GameWorld.new()
	var terrain := NovaTerrain.new()
	terrain.name = "NovaTerrain"
	var disp := NovaFoliageDispatcher.new()
	disp.name = "FoliageDispatcher"
	terrain.add_child(disp)
	world.add_child(terrain)
	add_child_autofree(world)
	await get_tree().process_frame  # _ready wires _dispatcher from the named child

	assert_false(world.is_foliage_hidden(), "foliage is shown by default")
	assert_true(disp.visible, "...with the dispatcher visible")

	world.set_foliage_hidden(true)
	assert_true(world.is_foliage_hidden())
	assert_false(disp.visible, "hiding foliage hides the dispatcher (and its MultiMesh slots)")

	world.set_foliage_hidden(false)
	assert_false(world.is_foliage_hidden())
	assert_true(disp.visible, "showing foliage restores the dispatcher")


class AnchorSimStub:
	extends RefCounted
	var anchors := PackedVector3Array()
	func get_foliage_mask_anchor_positions() -> PackedVector3Array:
		return anchors


class AnchorRuntimeStub:
	extends Node
	var sim = null
	func is_playing() -> bool:
		return false
	func get_sim():
		return sim


class SimlessRuntimeStub:
	extends Node
	func is_playing() -> bool:
		return false


func test_tick_feeds_dispatcher_silhouette_anchors_from_the_sim() -> void:
	# The hide-in-grass anchor feed: every host tick routes the sim's
	# crouched/prone infantry positions into the foliage dispatcher's silhouette
	# tier [orig: Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded
	# (MoveOrder & 0x300), groundEntity gate @ 0x5c7dd5..0x5c7df7].
	var world := GameWorld.new()
	var terrain := NovaTerrain.new()
	terrain.name = "NovaTerrain"
	var disp := NovaFoliageDispatcher.new()
	disp.name = "FoliageDispatcher"
	terrain.add_child(disp)
	world.add_child(terrain)
	add_child_autofree(world)
	await get_tree().process_frame  # _ready wires _dispatcher from the named child

	var runtime := AnchorRuntimeStub.new()
	var sim := AnchorSimStub.new()
	runtime.sim = sim
	add_child_autofree(runtime)
	world._runtime = runtime
	world._loaded = true

	var expected := PackedVector3Array([Vector3(12.0, 3.0, -40.0), Vector3(-7.5, 0.25, 96.0)])
	sim.anchors = expected
	world.tick(Vector3.ZERO)
	assert_eq(disp.silhouette_anchors, expected,
		"tick feeds the sim's anchor positions into the dispatcher's silhouette tier")

	sim.anchors = PackedVector3Array()
	world.tick(Vector3.ZERO)
	assert_eq(disp.silhouette_anchors, PackedVector3Array(),
		"an emptied sim anchor list clears the previous frame's anchors")

	world._runtime = null
	world._loaded = false


func test_tick_clears_stale_silhouette_anchors_when_no_sim_is_reachable() -> void:
	# The feed assigns unconditionally: a runtime without get_sim() (or a null
	# sim) must wipe anchors left by an earlier mission, never leave grass
	# clumps orbiting a despawned player.
	var world := GameWorld.new()
	var terrain := NovaTerrain.new()
	terrain.name = "NovaTerrain"
	var disp := NovaFoliageDispatcher.new()
	disp.name = "FoliageDispatcher"
	terrain.add_child(disp)
	world.add_child(terrain)
	add_child_autofree(world)
	await get_tree().process_frame

	var runtime := SimlessRuntimeStub.new()
	add_child_autofree(runtime)
	world._runtime = runtime
	world._loaded = true

	disp.silhouette_anchors = PackedVector3Array([Vector3(1.0, 2.0, 3.0)])  # stale
	world.tick(Vector3.ZERO)
	assert_eq(disp.silhouette_anchors, PackedVector3Array(),
		"a runtime with no sim seam clears stale anchors on the next tick")

	var anchorless := AnchorRuntimeStub.new()  # get_sim() returns null
	add_child_autofree(anchorless)
	world._runtime = anchorless
	disp.silhouette_anchors = PackedVector3Array([Vector3(4.0, 5.0, 6.0)])  # stale
	world.tick(Vector3.ZERO)
	assert_eq(disp.silhouette_anchors, PackedVector3Array(),
		"a null sim clears stale anchors too")

	world._runtime = null
	world._loaded = false


func test_set_foliage_hidden_is_safe_without_a_dispatcher() -> void:
	# Before a world loads (or with no foliage), there is no dispatcher; the toggle must
	# just record intent and never crash.
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_foliage_hidden(true)
	assert_true(world.is_foliage_hidden(), "the flag holds even with no dispatcher to act on")


func _make_world() -> GameWorld:
	var world := GameWorld.new()
	var terrain := NovaTerrain.new()
	terrain.name = "NovaTerrain"
	world.add_child(terrain)
	return world


func _write_fixture_file(path: String, text: String) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "Fixture file should be writable: %s" % path)
	if file != null:
		file.store_string(text)
		file.close()


func _remove_dir_recursive(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	dir.list_dir_begin()
	var entry := dir.get_next()
	while entry != "":
		var child := path.path_join(entry)
		if dir.current_is_dir():
			_remove_dir_recursive(child)
		else:
			DirAccess.remove_absolute(child)
		entry = dir.get_next()
	dir.list_dir_end()
	DirAccess.remove_absolute(path)
