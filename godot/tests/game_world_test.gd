extends GutTest

const WORLD_TEST_ROOT := "game_world_test"
const ArmoryHost := preload("res://engine/world/armory_host.gd")


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


class ItemPoseRuntimeStub:
	extends Node
	var has_snapshot := true
	var pose: Variant = Transform3D(Basis.IDENTITY, Vector3(7, 8, 9))
	var refs: Array = []
	func has_current_present_effect_snapshot() -> bool:
		return has_snapshot
	func presented_entity_effect_transform(entity_ref: Dictionary) -> Variant:
		refs.append(entity_ref.duplicate())
		return pose


class ViewmodelPlacerStub:
	extends RefCounted
	var graphics: Array[String] = []
	func build_model_from_graphic(graphic: String, _adm_name: String,
			_parent: Node3D, _clip_key: String, _env_node, _rig_graphic: String):
		graphics.append(graphic)
		return null


class ViewmodelWorldHarness:
	extends GameWorld
	var requested_def: PlayerViewmodelDef
	var model_availability: Array[bool] = []
	func install_viewmodel_fixture(def: PlayerViewmodelDef, placer) -> void:
		requested_def = def
		_placer = placer
	func local_player_viewmodel_def() -> PlayerViewmodelDef:
		return requested_def
	func _set_local_player_first_person_model_available(available: bool) -> void:
		model_availability.append(available)


class ImpactSimStub:
	extends RefCounted
	var drain_count := 0
	var weapon_rows: Array = []
	var rows: Array = [{
		'position': Vector3(3, 2, 1),
		'direction': Vector3.FORWARD,
		'effect': 'Effect_AmHitDirt',
		'sound': 'IMP_BULLET_DIRT',
		'age_ticks': 2,
		'source_tick': 41,
		'source_order': 7,
	}]
	func drain_round_impacts() -> Array:
		drain_count += 1
		var out := rows
		rows = []
		return out
	func drain_local_player_weapon_events() -> Array:
		var out := weapon_rows
		weapon_rows = []
		return out


class ImpactRuntimeStub:
	extends Node
	var sim := ImpactSimStub.new()
	func get_sim() -> ImpactSimStub:
		return sim


class FirstOpenArmorySimProxy:
	extends RefCounted
	var inner: NovaSimulation

	func _init(p_inner: NovaSimulation) -> void:
		inner = p_inner

	func local_player_in_armory_zone() -> bool:
		return true

	func is_host_listening() -> bool:
		return false

	func is_joiner() -> bool:
		return false

	func get_local_player_class() -> int:
		return inner.get_local_player_class()

	func get_local_player_weapon_name() -> String:
		return inner.get_local_player_weapon_name()

	func get_local_player_loadout() -> Array:
		return inner.get_local_player_loadout()

	func get_local_player_inventory() -> Dictionary:
		return inner.get_local_player_inventory()

	func get_weapon_availability(weapon_name: String) -> int:
		return inner.get_weapon_availability(weapon_name)

	func apply_local_player_loadout(kit: Array, selected_class: int) -> bool:
		return inner.apply_local_player_loadout(kit, selected_class)


class FirstOpenArmoryWorldProxy:
	extends Node
	var inner: GameWorld
	var sim: FirstOpenArmorySimProxy

	func _init(p_inner: GameWorld) -> void:
		inner = p_inner
		sim = FirstOpenArmorySimProxy.new(inner.get_sim())

	func get_sim():
		return sim

	func get_resource_root() -> NovaResourceRoot:
		return inner.get_resource_root()

	func get_weapon_database() -> NovaWeaponDatabase:
		return inner.get_weapon_database()

	func local_player_team() -> int:
		return inner.local_player_team()

	func local_player_viewmodel_def():
		return inner.local_player_viewmodel_def()

	func set_local_player_weapon_by_name(weapon_name: String) -> bool:
		return inner.set_local_player_weapon_by_name(weapon_name)

	func clear_local_player_weapon() -> void:
		inner.clear_local_player_weapon()


class ImpactAudioStub:
	extends NovaMissionAudio
	var fires: Array = []
	func fire_soundset(set_name: String, world_pos: Vector3, _source_bms_id: int = 0) -> bool:
		fires.append({'name': set_name, 'position': world_pos})
		return true


class FxWorldStub:
	extends NovaEffectWorld
	var spawns: Array = []
	var attached_spawns: Array = []
	var request_spawns: Array = []
	var stopped_groups: Array[int] = []
	var timeline: Array[String] = []
	func spawn_effect_request(effect: String, transform: Transform3D,
			options: Dictionary = {}) -> Dictionary:
		if are_particles_hidden():
			return {"spawned": false, "accepted": false, "effect_handle": 0}
		request_spawns.append({
			"effect": effect,
			"transform": transform,
			"options": options.duplicate(),
		})
		return {
			"spawned": true,
			"accepted": true,
			"effect_handle": 1,
			"group_id": request_spawns.size(),
		}
	func spawn_effect(effect: String, position: Vector3,
			orientation: Vector3 = Vector3.ZERO) -> int:
		spawns.append({
			'effect': effect,
			'position': position,
			'orientation': orientation,
		})
		return 1
	func spawn_effect_transient(effect: String, position: Vector3,
			orientation: Vector3 = Vector3.ZERO, initial_age_ticks: int = 0,
			render_domain: int = RENDER_DOMAIN_WORLD, source_tick: int = 0,
			source_order: int = 0) -> int:
		timeline.append("impact")
		spawns.append({
			'effect': effect,
			'position': position,
			'orientation': orientation,
			'initial_age_ticks': initial_age_ticks,
			'render_domain': render_domain,
			'source_tick': source_tick,
			'source_order': source_order,
		})
		return 1
	func advance_fixed_tick(_delta: float) -> void:
		timeline.append("advance")
	func spawn_effect_owned(owner_key: Variant, effect: String, position: Vector3,
			orientation: Vector3 = Vector3.ZERO) -> int:
		spawns.append({
			"owner": owner_key,
			"effect": effect,
			"position": position,
			"orientation": orientation,
		})
		return 1
	func spawn_effect_attached(owner_key: Variant, effect: String,
			initial_transform: Transform3D, local_pos: Vector3, local_dir: Vector3) -> int:
		var receipt := spawn_effect_attached_request(
				owner_key, effect, initial_transform, local_pos, local_dir)
		return int(receipt.get("effect_handle", 0)) if bool(
				receipt.get("spawned", false)) else 0
	func spawn_effect_attached_request(owner_key: Variant, effect: String,
			initial_transform: Transform3D, local_pos: Vector3,
			local_dir: Vector3) -> Dictionary:
		if are_particles_hidden():
			return {
				"spawned": false,
				"accepted": false,
				"effect_handle": 0,
				"group_id": 0,
			}
		attached_spawns.append({
			"owner": owner_key,
			"effect": effect,
			"transform": initial_transform,
			"local_pos": local_pos,
			"local_dir": local_dir,
		})
		return {
			"spawned": true,
			"accepted": true,
			"effect_handle": 1,
			"group_id": attached_spawns.size(),
		}
	func stop_group(group_id: int) -> void:
		stopped_groups.append(group_id)


class ItemFxDbStub:
	extends RefCounted
	var attribs: Dictionary = {}
	var effects: Dictionary = {}
	func get_attrib(item_id: int) -> int:
		return int(attribs.get(item_id, 0))
	func get_particle_effects(item_id: int) -> Dictionary:
		return effects.get(item_id,
				{"particlefx": {"effect": "Effect_Test", "userpoint": "FX00"}})


class ItemFxPlacerStub:
	extends RefCounted
	var item_db: ItemFxDbStub
	var static_sources: Array = []
	func get_item_db() -> ItemFxDbStub:
		return item_db
	func get_static_item_effect_sources() -> Array:
		return static_sources.duplicate(true)


class ItemFxObjectDataStub:
	extends RefCounted
	var points: Array = [{
		"name": "fx00",
		"position": Vector3(1, 2, 3),
		"rotation": Vector3(0, 0, -1),
	}]
	func _init(initial_points: Array = []) -> void:
		if not initial_points.is_empty():
			points = initial_points.duplicate(true)
	func get_user_point_count() -> int:
		return points.size()
	func get_user_point_info(index: int) -> Dictionary:
		return points[index]


class ItemFxModelStub:
	extends Node3D
	var data := ItemFxObjectDataStub.new()
	func get_object_data() -> ItemFxObjectDataStub:
		return data


class ItemFxGameWorldHarness:
	extends GameWorld
	func configure_item_fx(effects: NovaEffectWorld, placer: RefCounted) -> void:
		_effect_world = effects
		_placer = placer
	func present_item_fx(node: Node3D, kind: int, item_id: int) -> int:
		return _attach_item_effect_to_node(node, kind, item_id)
	func attach_all_item_fx() -> void:
		_attach_item_effects()
	func present_static_item_fx(source: Dictionary, source_index: int) -> int:
		return _attach_item_effect_to_static(source, source_index)
	func pending_item_fx_count() -> int:
		return _item_fx_pending_nodes.size()
	func pending_static_item_fx_count() -> int:
		return _item_fx_pending_static.size()
	func deferred_control_item_fx_count() -> int:
		return _item_fx_control_nodes.size()
	func active_control_identity_count() -> int:
		return _item_fx_control_active.size()
	func consume_runtime_effects(effects: Array) -> void:
		_on_runtime_effects(effects)
	func configure_item_owner(runtime: Node, key: String, node: Node3D,
			entity_ref: Dictionary) -> void:
		_runtime = runtime
		_item_fx_nodes[key] = node
		_item_fx_owner_refs[key] = entity_ref.duplicate()
	func resolve_item_owner(key: String) -> Variant:
		return _effect_owner_transform(key)


class ImpactGameWorldHarness:
	extends GameWorld
	func configure(runtime: Node, effects: NovaEffectWorld, audio: NovaMissionAudio) -> void:
		_runtime = runtime
		_effect_world = effects
		_mission_audio = audio
	func route_round_impacts() -> void:
		_route_round_impacts()
	func fixed_tick() -> void:
		_on_runtime_fixed_tick(1)



# Single stub-injection seam for this file: GameWorld builds its runtime
# internally in _start_runtime, so duck-typed transport stubs go in through
# these two helpers only (keeps the private pokes to one site).
func _install_runtime(world, runtime, effects = null) -> void:
	world._runtime = runtime
	world._loaded = runtime != null
	world._effect_world = effects


func _detach_runtime(world) -> void:
	_install_runtime(world, null)


func test_tick_gates_the_runtime_on_its_transport() -> void:
	# The game host's tick must respect MissionRuntime's play flag - the debug
	# overlay's Pause/Step work on a live mission BECAUSE this gate exists
	# (before it, play()/pause() were inert in the game).
	var world := _make_world()
	add_child_autofree(world)
	var runtime := TransportRuntimeStub.new()
	add_child_autofree(runtime)
	_install_runtime(world, runtime)

	world.tick(Vector3.ZERO)
	assert_eq(runtime.ticks, 0, "a paused runtime never ticks")
	runtime.play()
	world.tick(Vector3.ZERO)
	assert_eq(runtime.ticks, 1, "a playing runtime ticks once per host frame")
	runtime.pause()
	world.tick(Vector3.ZERO)
	assert_eq(runtime.ticks, 1, "pausing stops it again")
	_detach_runtime(world)


func test_round_impacts_route_generic_transient_and_audio_legs() -> void:
	var world := ImpactGameWorldHarness.new()
	var terrain := NovaTerrain.new()
	terrain.name = 'NovaTerrain'
	world.add_child(terrain)
	add_child_autofree(world)
	var runtime := ImpactRuntimeStub.new()
	var effects := FxWorldStub.new()
	var audio := ImpactAudioStub.new(null, null)
	add_child_autofree(runtime)
	add_child_autofree(effects)
	world.configure(runtime, effects, audio)

	world.route_round_impacts()

	assert_eq(runtime.sim.drain_count, 1,
			'resolved impact rows cannot accumulate between presentation frames')
	assert_eq(effects.spawns, [{
		'effect': 'Effect_AmHitDirt',
		'position': Vector3(3, 2, 1),
		'orientation': Vector3.FORWARD,
		'initial_age_ticks': 2,
		'render_domain': NovaEffectScene.RENDER_DOMAIN_WORLD,
		'source_tick': 41,
		'source_order': 7,
	}], 'impact particles retain their direction, age, and stable source order')
	assert_eq(audio.fires, [{
		'name': 'IMP_BULLET_DIRT',
		'position': Vector3(3, 2, 1),
	}], 'impact audio shares collision presentation with the visual transient')


func test_round_impacts_route_sound_only_without_a_particle() -> void:
	var world := ImpactGameWorldHarness.new()
	var terrain := NovaTerrain.new()
	terrain.name = 'NovaTerrain'
	world.add_child(terrain)
	add_child_autofree(world)
	var runtime := ImpactRuntimeStub.new()
	var effects := FxWorldStub.new()
	var audio := ImpactAudioStub.new(null, null)
	add_child_autofree(runtime)
	add_child_autofree(effects)
	world.configure(runtime, effects, audio)
	runtime.sim.rows = [{
		'position': Vector3(3, 2, 1),
		'direction': Vector3.UP,
		'effect': '',
		'sound': 'IMP_GREN_DIRT',
		'age_ticks': 0,
		'source_tick': 43,
		'source_order': 8,
	}]

	world.route_round_impacts()

	assert_true(effects.spawns.is_empty(),
			'a sound-only grenade bounce must not spawn its explosion particle')
	assert_eq(audio.fires, [{
		'name': 'IMP_GREN_DIRT',
		'position': Vector3(3, 2, 1),
	}], 'the first five grenade bounces retain their authored surface sound')


func test_fixed_tick_orders_weapon_and_impact_before_particle_advance() -> void:
	var world := ImpactGameWorldHarness.new()
	var terrain := NovaTerrain.new()
	terrain.name = "NovaTerrain"
	world.add_child(terrain)
	add_child_autofree(world)
	var runtime := ImpactRuntimeStub.new()
	var effects := FxWorldStub.new()
	var audio := ImpactAudioStub.new(null, null)
	add_child_autofree(runtime)
	add_child_autofree(effects)
	world.configure(runtime, effects, audio)
	runtime.sim.weapon_rows = [{"action_effect": 3}]
	runtime.sim.rows[0]["age_ticks"] = 0
	world.set_local_player_weapon_tick_consumer(func(events: Array[PlayerWeaponEvent]) -> void:
		assert_eq(events.size(), 1, "the source tick drains exactly one weapon batch")
		effects.timeline.append("weapon")
	)

	world.fixed_tick()

	assert_eq(effects.timeline, ["weapon", "impact", "advance"],
			"the source tick is presented chronologically before its particle pass")
	assert_eq(int(effects.spawns[0].initial_age_ticks), 0,
			"a physical collision is visible in its production tick without age compensation")


func test_fx2ssn_routes_position_owner_and_up_orientation() -> void:
	var world := _make_world()
	add_child_autofree(world)
	var runtime := FxRuntimeStub.new()
	var effects := FxWorldStub.new()
	add_child_autofree(runtime)
	add_child_autofree(effects)
	_install_runtime(world, runtime, effects)
	world._route_mission_effects([{"kind": "fx2ssn", "b": 17, "str": "Dust"}])
	assert_eq(effects.spawns.size(), 1)
	assert_eq(effects.spawns[0].owner, 17)
	assert_eq(effects.spawns[0].effect, "Dust")
	assert_eq(effects.spawns[0].position, Vector3(4, 5, 6))
	assert_eq(effects.spawns[0].orientation, Vector3.UP,
			"the documented terrain-normal placeholder must actually reach the emitter")
	_detach_runtime(world)


func test_round_outcome_effects_pass_through_to_hud_consumers() -> void:
	# The round-outcome trio (win/lose/round_end) is produced sim-side [orig: the WAC
	# win/lose handlers @0x4ed4a0/@0x4ed3f0 + Server_ProcessRoundEnd @0x5164f0] and is
	# host presentation on this side: the router must pass every row through to
	# mission_effects (the HUD banner + the shell's end-of-mission flow consume there).
	var world := _make_item_fx_world()
	add_child_autofree(world)
	watch_signals(world)
	var rows := [
		{"kind": "lose", "a": 0, "str": "STRMISC_KILLEDGREEN"},
		{"kind": "win", "a": 1},
		{"kind": "round_end", "a": 2},
	]
	world.consume_runtime_effects(rows)
	assert_signal_emitted_with_parameters(world, "mission_effects", [rows])


func test_load_world_requires_hardcoded_environment_in_global_root() -> void:
	var root := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join("missing_env_%d" % Time.get_ticks_usec())
	DirAccess.make_dir_recursive_absolute(root)
	# Pass the runtime archive gate so this fixture reaches the missing-environment contract.
	_write_pff(root.path_join("resource.pff"), [])
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


func test_armory_can_reuse_game_world_weapon_database_on_first_open() -> void:
	NovaStrings.clear()
	var root_dir := _stage_minimal_fixture("first_armory_open")
	for files in [
		["res://../fixtures/mnu/jo_weapon.mnu", "weapon.mnu"],
		["res://../fixtures/def/weapon.def", "weapon.def"],
		["res://../fixtures/rtxt/menutxt.bin", "menutxt.BIN"],
		["res://../fixtures/rtxt/gametext.bin", "gametext.bin"],
	]:
		var target := root_dir.path_join(files[1])
		if FileAccess.file_exists(target):
			assert_eq(DirAccess.remove_absolute(target), OK)
		assert_eq(DirAccess.copy_absolute(
				ProjectSettings.globalize_path(files[0]), target), OK)

	var world := _make_world()
	add_child_autofree(world)
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)
	world.set_resource_root(root)
	world.set_local_player_spawn_loadout({
		"primary": "WPN_M4AUTO",
		"accessory": "WPN_SATCHEL_CHARGE",
		"player_class": 8,
	})
	assert_eq(world.load_mission("mnml.bms"), OK)

	assert_true(world.has_method("get_weapon_database"),
		"the production world exposes the same weapon database seam ArmoryHost consumes")
	if not world.has_method("get_weapon_database"):
		return
	var weapons := world.call("get_weapon_database") as NovaWeaponDatabase
	assert_not_null(weapons, "first armory open lazily resolves weapon.def")
	if weapons == null:
		return
	assert_true(weapons.is_loaded())
	assert_gte(weapons.find_weapon("WPN_M4AUTO"), 0)
	assert_gte(weapons.find_weapon("WPN_SATCHEL_CHARGE"), 0)
	var sim := world.get_sim()
	var expected_names := ["WPN_M4AUTO", "WPN_SATCHEL_CHARGE"]
	var before_names: Array[String] = []
	for value in sim.get_local_player_loadout():
		before_names.append(String((value as Dictionary).get("name", "")))
	assert_eq(before_names, expected_names,
		"the production world promoted the PLAYER_INFO-style canonical profile")

	var armory_world := FirstOpenArmoryWorldProxy.new(world)
	add_child_autofree(armory_world)
	var overlay := Control.new()
	add_child_autofree(overlay)
	overlay.size = Vector2(800, 600)
	var host := ArmoryHost.new()
	add_child_autofree(host)
	host.setup(armory_world, null, overlay)
	assert_true(host.try_open(), "the production world catalog reaches first armory open")
	var menu := overlay.get_node("ArmoryMenu") as NovaMnuMenu
	var primary := menu.find_child("PRIMARY", true, false) as NovaMnuCombo
	var accessory := menu.find_child("ACCESSORY", true, false) as NovaMnuCombo
	assert_gt(primary.get_selected(), 0, "the current primary is not NONE on first visit")
	assert_gt(accessory.get_selected(), 0, "the current satchel is not NONE on first visit")

	menu.find_child("ACCEPT", true, false).emit_signal("pressed")
	var after_names: Array[String] = []
	for value in sim.get_local_player_loadout():
		after_names.append(String((value as Dictionary).get("name", "")))
	assert_eq(after_names, expected_names,
		"accepting the untouched first-open rows preserves the exact canonical kit")
	world.unload()
	NovaStrings.clear()


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


func test_failed_host_load_does_not_arm_the_next_mission_as_a_lan_host() -> void:
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/minimal/resources")), OK)
	world.set_resource_root(root)

	assert_eq(world.load_mission_as_host({
		"mission": "missing.bms",
		"net_transport": "lan",
		"bind_port": 0,
	}), ERR_FILE_NOT_FOUND)
	assert_eq(world.load_mission("mnml.bms"), OK)
	var sim: NovaSimulation = world.get_sim()
	assert_not_null(sim)
	if sim != null:
		assert_false(sim.is_host_listening(),
			"a rejected host request cannot turn a later ordinary mission into a LAN host")
	world.unload()


func test_failed_join_load_does_not_make_the_next_mission_wire_only() -> void:
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/minimal/resources")), OK)
	world.set_resource_root(root)

	assert_eq(world.load_mission_as_joiner({
		"mission": "missing.bms",
		"host_ip": "127.0.0.1",
		"port": 9,
	}, "Joiner"), ERR_FILE_NOT_FOUND)
	assert_eq(world.load_mission("mnml.bms"), OK)
	assert_gt(int(world.get_mission_stats().get("markers", 0)), 0,
		"a rejected join request cannot make a later ordinary mission wire-only")
	world.unload()


func test_environment_load_failure_finishes_its_perf_timeline() -> void:
	var root_dir := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join(
		"timeline_env_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir), OK)
	var bms_name := "timeline_env_fail_%d.bms" % Time.get_ticks_usec()
	assert_eq(DirAccess.copy_absolute(
		ProjectSettings.globalize_path("res://../fixtures/minimal/resources/mnml.bms"),
		root_dir.path_join(bms_name)), OK)
	_write_fixture_file(root_dir.path_join("mnml.env"), "")
	_write_fixture_file(root_dir.path_join("mnml.trn"), "terrain_name \"mnml\"\n")
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)

	var packed := load("res://engine/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	world.set_resource_root(root)
	assert_eq(world.load_mission(bms_name), ERR_CANT_OPEN)

	var timeline: PerfTimeline = PerfTimeline.latest()
	assert_not_null(timeline, "a failed environment stage still retains its timeline")
	if timeline == null:
		return
	assert_eq(timeline.label, "Mission load %s" % bms_name)
	var spans := timeline.spans()
	assert_eq(spans.size(), 1)
	if spans.size() == 1:
		assert_eq(String(spans[0].get("name", "")), "environment")
		assert_gt(int(spans[0].get("end_us", 0)), 0,
			"finish closes the environment span left open by the early return")


func test_terrain_load_failure_finishes_its_perf_timeline() -> void:
	var root_dir := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join(
		"timeline_terrain_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir), OK)
	var bms_name := "timeline_terrain_fail_%d.bms" % Time.get_ticks_usec()
	assert_eq(DirAccess.copy_absolute(
		ProjectSettings.globalize_path("res://../fixtures/minimal/resources/mnml.bms"),
		root_dir.path_join(bms_name)), OK)
	_write_fixture_file(root_dir.path_join("mnml.env"), "")
	_write_fixture_file(root_dir.path_join("mnml.trn"), "")
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)

	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	world.set_resource_root(root)
	assert_eq(world.load_mission(bms_name), ERR_CANT_OPEN)

	var timeline: PerfTimeline = PerfTimeline.latest()
	assert_not_null(timeline, "a failed terrain stage still retains its timeline")
	if timeline == null:
		return
	assert_eq(timeline.label, "Mission load %s" % bms_name)
	var spans := timeline.spans()
	assert_eq(spans.size(), 2)
	if spans.size() == 2:
		assert_eq(String(spans[0].get("name", "")), "environment")
		assert_eq(String(spans[1].get("name", "")), "terrain")
		assert_gt(int(spans[1].get("end_us", 0)), 0,
			"finish closes the terrain span left open by the early return")


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


func test_runtime_dev_mount_still_loads_bms_from_archive() -> void:
	var root_dir := _stage_minimal_fixture("archive_only_bms")
	var archived_bms := FileAccess.get_file_as_bytes(root_dir.path_join("mnml.bms"))
	_write_pff(root_dir.path_join("resource.pff"), [{
		"name": "mnml.bms",
		"bytes": archived_bms,
	}])
	_write_bytes(root_dir.path_join("mnml.bms"), "not a mission".to_utf8_buffer())

	var resource_root := NovaResourceRoot.new()
	assert_eq(resource_root.mount_runtime(root_dir, "", true), OK,
		"the runtime fixture mounts with /d loose overrides enabled")
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	world.set_resource_root(resource_root)

	assert_eq(world.load_mission("mnml.bms"), OK,
		"the witnessed BMS caller bypasses the corrupt loose override")
	assert_eq(world.get_loaded_mission_file(), "mnml.bms")
	world.unload()


func test_runtime_mission_til_forces_loose_first_in_packed_mode() -> void:
	var root_dir := _make_fixture_root("loose_first_til")
	var source_dir := ProjectSettings.globalize_path("res://../fixtures/minimal/resources")
	var archive_entries: Array = []
	for file_name in DirAccess.get_files_at(source_dir):
		archive_entries.append({
			"name": file_name,
			"bytes": FileAccess.get_file_as_bytes(source_dir.path_join(file_name)),
		})
	archive_entries.append({
		"name": "mnml.til",
		"bytes": _til_bytes_for_cell(0),
	})
	_write_pff(root_dir.path_join("resource.pff"), archive_entries)
	_write_bytes(root_dir.path_join("mnml.til"), _til_bytes_for_cell(4))

	var resource_root := NovaResourceRoot.new()
	assert_eq(resource_root.mount_runtime(root_dir), OK,
		"packed-default mode makes the archive win unless the caller forces loose-first")
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	world.set_resource_root(resource_root)
	assert_eq(world.load_mission("mnml.bms"), OK)

	var terrain := world.get_node("NovaTerrain") as NovaTerrain
	var tile_info := terrain.tile_info_override as NovaTerrainTileInfo
	assert_not_null(tile_info)
	if tile_info != null:
		assert_true(tile_info.blocks_foliage(72.0, 8.0, 2.0),
			"the loose TIL blocker at cell 4 reaches terrain composition")
		assert_false(tile_info.blocks_foliage(8.0, 8.0, 2.0),
			"the conflicting archived TIL at cell 0 does not win")
	world.unload()


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


func test_valid_emplaced_def_without_gfx1_builds_no_fallback_gun() -> void:
	# AVENGER has a valid retail weapon definition but no fpModel. That means an
	# intentionally empty FP pass, not the bring-up AK fallback used when no
	# definition resolves at all.
	var world: ViewmodelWorldHarness = autofree(ViewmodelWorldHarness.new())
	var placer := ViewmodelPlacerStub.new()
	world.install_viewmodel_fixture(PlayerViewmodelDef.from_weapon_dict({
		"name": "WPN_AVENGER",
		"gfx1": "",
		"flags": 0x80,
	}), placer)
	var viewmodel := world.build_local_player_viewmodel()
	assert_not_null(viewmodel,
			"a valid no-model definition is a stable empty FP presentation epoch")
	assert_true(placer.graphics.is_empty(),
			"a missing authored gfx1 must not substitute the AK first-person gun")
	assert_eq(world.model_availability, [false],
			"the render gate observes that no first-person gun model resolved")


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


func test_user_point_debug_builds_frees_and_cleans_up_on_unload() -> void:
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	assert_false(world.is_user_point_debug(), "off by default")
	assert_null(world.get_node_or_null("UserPointDebug"), "...with no view node")

	world.set_user_point_debug(true)
	assert_true(world.is_user_point_debug())
	assert_not_null(world.get_node_or_null("UserPointDebug"),
			"enabling builds the world-wide user-point view")

	world.unload()
	assert_true(world.is_user_point_debug(),
			"the checked toggle survives teardown so the next load can re-arm it")
	assert_null(world.get_node_or_null("UserPointDebug"),
			"teardown detaches the view immediately, before deferred destruction")

	world.set_user_point_debug(false)
	assert_false(world.is_user_point_debug())


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
	_install_runtime(world, runtime)

	var expected := PackedVector3Array([Vector3(12.0, 3.0, -40.0), Vector3(-7.5, 0.25, 96.0)])
	sim.anchors = expected
	world.tick(Vector3.ZERO)
	assert_eq(disp.silhouette_anchors, expected,
		"tick feeds the sim's anchor positions into the dispatcher's silhouette tier")

	sim.anchors = PackedVector3Array()
	world.tick(Vector3.ZERO)
	assert_eq(disp.silhouette_anchors, PackedVector3Array(),
		"an emptied sim anchor list clears the previous frame's anchors")

	_detach_runtime(world)


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
	_install_runtime(world, runtime)

	disp.silhouette_anchors = PackedVector3Array([Vector3(1.0, 2.0, 3.0)])  # stale
	world.tick(Vector3.ZERO)
	assert_eq(disp.silhouette_anchors, PackedVector3Array(),
		"a runtime with no sim seam clears stale anchors on the next tick")

	var anchorless := AnchorRuntimeStub.new()  # get_sim() returns null
	add_child_autofree(anchorless)
	_install_runtime(world, anchorless)
	disp.silhouette_anchors = PackedVector3Array([Vector3(4.0, 5.0, 6.0)])  # stale
	world.tick(Vector3.ZERO)
	assert_eq(disp.silhouette_anchors, PackedVector3Array(),
		"a null sim clears stale anchors too")

	_detach_runtime(world)


func test_set_foliage_hidden_is_safe_without_a_dispatcher() -> void:
	# Before a world loads (or with no foliage), there is no dispatcher; the toggle must
	# just record intent and never crash.
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_foliage_hidden(true)
	assert_true(world.is_foliage_hidden(), "the flag holds even with no dispatcher to act on")


func test_item_effect_attach_uses_the_original_pool_specific_gates() -> void:
	# Mission kinds preserve the original pool mapping. Pool 0 is not walked;
	# pool 1 skips attrib 0x42; pools 2/3 skip only powerup bit 0x2.
	# [orig: resolve_item_materials_and_spawn_bone_trails @ 0x522ee0,
	#  gates @ 0x523233 / @ 0x523272 / @ 0x5232af]
	var world := _make_item_fx_world()
	add_child_autofree(world)
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var db := ItemFxDbStub.new()
	db.attribs = {
		1: 0,
		2: 0,
		3: 0x40,
		4: 0x2,
		5: 0x40,
		6: 0,
		7: 0,
		8: 0,
	}
	var placer := ItemFxPlacerStub.new()
	placer.item_db = db
	world.configure_item_fx(effects, placer)

	var cases := [
		[NovaMissionData.KIND_ORGANIC, 1, 0],
		[NovaMissionData.KIND_ITEM, 2, 1],
		[NovaMissionData.KIND_ITEM, 3, 0],
		[NovaMissionData.KIND_BUILDING, 4, 0],
		[NovaMissionData.KIND_BUILDING, 5, 1],
		[NovaMissionData.KIND_MARKER, 6, 1],
	]
	var nodes: Array[Node3D] = []
	for case_v in cases:
		var case: Array = case_v
		var model := ItemFxModelStub.new()
		world.add_child(model)
		nodes.append(model)
		assert_eq(world.present_item_fx(model, int(case[0]), int(case[1])),
				int(case[2]), "kind %d attrib 0x%x" % [int(case[0]), db.get_attrib(int(case[1]))])

	assert_eq(effects.attached_spawns.size(), 3,
			"only normal pool-1 plus allowed pool-2/3 entities attach")
	assert_eq(effects.attached_spawns[0].local_pos, Vector3(1, 2, 3))
	assert_eq(effects.attached_spawns[0].local_dir, Vector3(0, 0, -1))
	assert_eq(world.present_item_fx(nodes[1], NovaMissionData.KIND_ITEM, 2), 0,
			"a replayed wire-node callback cannot duplicate an existing attach")
	assert_eq(effects.attached_spawns.size(), 3)

	# A persistent item first materialized while the retail master switch is off
	# must attach once when particles are re-enabled; a mission loaded hidden
	# otherwise loses that effect permanently.
	world.set_particles_hidden(true)
	var hidden_model := ItemFxModelStub.new()
	world.add_child(hidden_model)
	assert_eq(world.present_item_fx(
			hidden_model, NovaMissionData.KIND_ITEM, 7), 0)
	assert_eq(effects.attached_spawns.size(), 3)
	world.set_particles_hidden(false)
	assert_eq(effects.attached_spawns.size(), 4,
			"re-enable retries the hidden-at-load persistent attachment once")
	world.set_particles_hidden(false)
	assert_eq(effects.attached_spawns.size(), 4, "steady enabled state cannot duplicate it")

	world.set_particles_hidden(true)
	var despawned_model := ItemFxModelStub.new()
	world.add_child(despawned_model)
	assert_eq(world.present_item_fx(
			despawned_model, NovaMissionData.KIND_ITEM, 8), 0)
	despawned_model.free()
	world.set_particles_hidden(false)
	assert_eq(effects.attached_spawns.size(), 4,
			"a wire node freed while hidden is pruned instead of retried")
	assert_eq(world.pending_item_fx_count(), 0)


func test_dbuggy_fx00_follows_controller_lifecycle_with_pre_node_race() -> void:
	# Shipped DBuggy1.3di: ITEMS.DEF item 101291 is PlayerControl (0x40) and
	# authors Effect_whiteExhaust at model userpoint FX00. It stays dormant in
	# the mission-start pool walk, then follows controller occupancy events.
	const DBUGGY_ITEM := 101291
	var world := _make_item_fx_world()
	add_child_autofree(world)
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var db := ItemFxDbStub.new()
	db.attribs[DBUGGY_ITEM] = 0x40
	db.effects[DBUGGY_ITEM] = {
		"particlefx": {
			"effect": "Effect_whiteExhaust",
			"userpoint": "FX00",
		},
	}
	var placer := ItemFxPlacerStub.new()
	placer.item_db = db
	world.configure_item_fx(effects, placer)
	watch_signals(world)

	var spawn_origin := (NovaMissionData.KIND_ITEM << 24) | 3
	var started := {
		"kind": "vehicle_control_started",
		"a": 71,
		"b": 9001,
		"c": spawn_origin,
	}
	var stopped := {
		"kind": "vehicle_control_stopped",
		"a": 71,
		"b": 9001,
		"c": spawn_origin,
	}

	# The simulation event can precede the present pass's wire/model callback.
	world.consume_runtime_effects([started])
	assert_eq(world.active_control_identity_count(), 3)
	assert_signal_not_emitted(world, "mission_effects",
			"render-internal lifecycle events never leak to HUD consumers")
	var model := ItemFxModelStub.new()
	model.set_meta("entity_ref", {
		"kind": NovaMissionData.KIND_ITEM,
		"index": 3,
		"bms_id": 9001,
		"item_id": DBUGGY_ITEM,
	})
	world.add_child(model)
	assert_eq(world.present_item_fx(model, NovaMissionData.KIND_ITEM, DBUGGY_ITEM), 1)
	assert_eq(world.deferred_control_item_fx_count(), 1)
	assert_eq(effects.attached_spawns.size(), 1)
	assert_eq(String(effects.attached_spawns[0].effect), "Effect_whiteExhaust")
	assert_eq(Vector3(effects.attached_spawns[0].local_pos), Vector3(1, 2, 3))
	assert_eq(Vector3(effects.attached_spawns[0].local_dir), Vector3(0, 0, -1))

	# Replayed starts are idempotent; a single transition stop detaches the
	# exact native group id returned by the receipt-bearing facade.
	world.consume_runtime_effects([started])
	assert_eq(effects.attached_spawns.size(), 1)
	world.consume_runtime_effects([stopped])
	assert_eq(effects.stopped_groups, [1])
	assert_eq(world.active_control_identity_count(), 0)

	# A later control transition can create a fresh group, and a mixed drain
	# exposes only the public effect after consuming the lifecycle row.
	world.consume_runtime_effects([started])
	assert_eq(effects.attached_spawns.size(), 2)
	var public_effect := {"kind": "text", "str": "still public"}
	world.consume_runtime_effects([stopped, public_effect])
	assert_eq(effects.stopped_groups, [1, 2])
	assert_signal_emitted_with_parameters(
			world, "mission_effects", [[public_effect]])


func test_dbuggy_hidden_pending_is_cancelled_when_control_stops() -> void:
	const DBUGGY_ITEM := 101291
	var world := _make_item_fx_world()
	add_child_autofree(world)
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var db := ItemFxDbStub.new()
	db.attribs[DBUGGY_ITEM] = 0x40
	db.effects[DBUGGY_ITEM] = {
		"particlefx": {
			"effect": "Effect_whiteExhaust",
			"userpoint": "FX00",
		},
	}
	var placer := ItemFxPlacerStub.new()
	placer.item_db = db
	world.configure_item_fx(effects, placer)
	world.set_particles_hidden(true)

	var model := ItemFxModelStub.new()
	model.set_meta("entity_ref", {
		"kind": NovaMissionData.KIND_ITEM,
		"index": 8,
		"bms_id": 9010,
		"item_id": DBUGGY_ITEM,
	})
	world.add_child(model)
	assert_eq(world.present_item_fx(model, NovaMissionData.KIND_ITEM, DBUGGY_ITEM), 0,
			"the unchanged mission-start 0x42 gate keeps PlayerControl dormant")
	var spawn_origin := (NovaMissionData.KIND_ITEM << 24) | 8
	world.consume_runtime_effects([{
		"kind": "vehicle_control_started",
		"a": 81,
		"b": 9010,
		"c": spawn_origin,
	}])
	assert_eq(world.pending_item_fx_count(), 1)
	world.consume_runtime_effects([{
		"kind": "vehicle_control_stopped",
		"a": 81,
		"b": 9010,
		"c": spawn_origin,
	}])
	assert_eq(world.pending_item_fx_count(), 0,
			"stop cancels a hidden activation before particles are re-enabled")
	world.set_particles_hidden(false)
	assert_eq(effects.attached_spawns.size(), 0,
			"re-enable cannot resurrect a stopped controller attachment")


func test_controller_net_id_does_not_alias_a_wire_handle() -> void:
	const DBUGGY_ITEM := 101291
	var world := _make_item_fx_world()
	add_child_autofree(world)
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var db := ItemFxDbStub.new()
	db.attribs[DBUGGY_ITEM] = 0x40
	var placer := ItemFxPlacerStub.new()
	placer.item_db = db
	world.configure_item_fx(effects, placer)

	world.consume_runtime_effects([{
		"kind": "vehicle_control_started",
		"a": 77,
		"b": 0,
		"c": 0,
	}])
	var model := ItemFxModelStub.new()
	model.set_meta("entity_ref", {
		"kind": NovaMissionData.KIND_ITEM,
		"index": 12,
		"bms_id": 0,
		"wire_handle": 77,
		"item_id": DBUGGY_ITEM,
	})
	world.add_child(model)
	assert_eq(world.present_item_fx(model, NovaMissionData.KIND_ITEM, DBUGGY_ITEM), 0)
	assert_eq(effects.attached_spawns.size(), 0,
			"event a is a simulation net id, not the presentation wire handle")


func test_static_item_effects_spawn_world_bound_from_value_descriptors() -> void:
	var world := _make_item_fx_world()
	add_child_autofree(world)
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var db := ItemFxDbStub.new()
	db.attribs = {2: 0, 3: 0x40, 9: 0}
	db.effects[9] = {
		"particlefx": {"effect": "Effect_Fallback", "userpoint": "FX00"},
	}
	var placer := ItemFxPlacerStub.new()
	placer.item_db = db

	var points: Array = []
	for i in range(18):
		points.append({
			"name": "other",
			"position": Vector3(i, 0, 0),
			"rotation": Vector3.FORWARD,
		})
	points[0] = {
		"name": "fx00",
		"position": Vector3(1, 2, 3),
		"rotation": Vector3(0, 0, -1),
	}
	points[15] = {
		"name": "FX00",
		"position": Vector3(-2, 0.5, 4),
		"rotation": Vector3.RIGHT,
	}
	# This authored duplicate is beyond the original 16-bit userpoint mask.
	var beyond_mask: Dictionary = points[16]
	beyond_mask["name"] = "FX00"
	points[16] = beyond_mask
	var matched_data := ItemFxObjectDataStub.new(points)
	var fallback_data := ItemFxObjectDataStub.new([{
		"name": "OTHER",
		"position": Vector3(99, 99, 99),
		"rotation": Vector3.UP,
	}])
	var entity_transform := Transform3D(
			Basis(Vector3.UP, PI * 0.5), Vector3(10, 20, 30))
	var fallback_transform := Transform3D(
			Basis(Vector3.RIGHT, PI * 0.25), Vector3(-5, 6, 7))
	placer.static_sources = [{
		"kind": NovaMissionData.KIND_ITEM,
		"item_id": 2,
		"graphic": "StaticVehicle1",
		"world_transform": entity_transform,
		"object_data": matched_data,
	}, {
		"kind": NovaMissionData.KIND_BUILDING,
		"item_id": 9,
		"graphic": "StaticBuilding1",
		"world_transform": fallback_transform,
		"object_data": fallback_data,
	}, {
		# Pool-1 attrib 0x40 is excluded before any effect request.
		"kind": NovaMissionData.KIND_ITEM,
		"item_id": 3,
		"graphic": "BlockedStatic",
		"world_transform": Transform3D.IDENTITY,
		"object_data": matched_data,
	}]
	world.configure_item_fx(effects, placer)

	world.attach_all_item_fx()

	assert_eq(effects.request_spawns.size(), 3,
			"two first-16 matches plus one origin fallback; the gated row is excluded")
	var first: Dictionary = effects.request_spawns[0]
	var first_options: Dictionary = first.get("options", {})
	assert_eq(int(first_options.get("admission", -1)), NovaEffectScene.ADMISSION_ALWAYS)
	assert_eq(int(first_options.get("binding", -1)), NovaEffectScene.BINDING_WORLD)
	assert_eq(int(first_options.get("render_domain", -1)),
			NovaEffectScene.RENDER_DOMAIN_WORLD)
	assert_false(first_options.has("owner_key"), "static batches never invent follow owners")
	assert_false(first_options.has("slot_key"), "Always spawns need no synthetic slot identity")
	var first_transform: Transform3D = first.get("transform", Transform3D.IDENTITY)
	assert_true(first_transform.origin.is_equal_approx(entity_transform * Vector3(1, 2, 3)))
	assert_true(first_transform.basis.z.normalized().is_equal_approx(
			(entity_transform.basis * Vector3(0, 0, -1)).normalized()),
			"the authored direction composes through the entity basis")
	var second_transform: Transform3D = effects.request_spawns[1].get(
			"transform", Transform3D.IDENTITY)
	assert_true(second_transform.origin.is_equal_approx(
			entity_transform * Vector3(-2, 0.5, 4)),
			"the duplicate name at userpoint 15 also spawns")
	var fallback_request: Dictionary = effects.request_spawns[2]
	assert_eq(String(fallback_request.get("effect", "")), "Effect_Fallback")
	var fallback_actual: Transform3D = fallback_request.get(
			"transform", Transform3D.IDENTITY)
	assert_true(fallback_actual.is_equal_approx(fallback_transform),
			"an unmatched userpoint falls back to the entity origin and basis")
	assert_eq(world.present_static_item_fx(placer.static_sources[0], 0), 0,
			"revisiting the same descriptor index cannot duplicate its persistent effect")
	assert_eq(effects.request_spawns.size(), 3)


func test_live_item_effect_owner_uses_each_fixed_ticks_value_pose() -> void:
	var world := _make_item_fx_world()
	add_child_autofree(world)
	var node := ItemFxModelStub.new()
	node.transform = Transform3D(Basis.IDENTITY, Vector3(99, 99, 99))
	world.add_child(node)
	var runtime := ItemPoseRuntimeStub.new()
	add_child_autofree(runtime)
	var key := "itemfx:42:0"
	var entity_ref := {"kind": NovaMissionData.KIND_ITEM, "index": 4, "bms_id": 42}
	world.configure_item_owner(runtime, key, node, entity_ref)

	var resolved: Variant = world.resolve_item_owner(key)
	assert_true(resolved is Transform3D)
	assert_true((resolved as Transform3D).origin.is_equal_approx(Vector3(7, 8, 9)),
			"a fixed tick follows the current client-view value, not the stale presented Node")
	assert_eq(runtime.refs, [entity_ref], "the copied stable identity crosses the provider seam")

	runtime.has_snapshot = false
	resolved = world.resolve_item_owner(key)
	assert_true((resolved as Transform3D).origin.is_equal_approx(Vector3(99, 99, 99)),
			"before the first sim snapshot, the authored Node remains the safe spawn seed")

	runtime.has_snapshot = true
	runtime.pose = null
	assert_null(world.resolve_item_owner(key),
			"an owner absent from this tick detaches instead of emitting once from stale presentation")


func test_static_item_effect_hidden_at_load_retries_once_when_enabled() -> void:
	var world := _make_item_fx_world()
	add_child_autofree(world)
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var db := ItemFxDbStub.new()
	var placer := ItemFxPlacerStub.new()
	placer.item_db = db
	placer.static_sources = [{
		"kind": NovaMissionData.KIND_ITEM,
		"item_id": 2,
		"graphic": "StaticVehicle1",
		"world_transform": Transform3D(Basis.IDENTITY, Vector3(3, 4, 5)),
		"object_data": ItemFxObjectDataStub.new(),
	}]
	world.configure_item_fx(effects, placer)

	world.set_particles_hidden(true)
	world.attach_all_item_fx()
	assert_eq(effects.request_spawns.size(), 0)
	assert_eq(world.pending_static_item_fx_count(), 1,
			"a world-bound persistent source survives a hidden mission load as values")
	world.set_particles_hidden(false)
	assert_eq(effects.request_spawns.size(), 1,
			"re-enabling submits the deferred static source")
	assert_eq(world.pending_static_item_fx_count(), 0)
	world.set_particles_hidden(false)
	assert_eq(effects.request_spawns.size(), 1, "steady enabled state cannot duplicate it")


func _make_item_fx_world() -> ItemFxGameWorldHarness:
	var world := ItemFxGameWorldHarness.new()
	var terrain := NovaTerrain.new()
	terrain.name = "NovaTerrain"
	world.add_child(terrain)
	return world


class BlinkSimStub:
	var blink_flags := 0
	func local_player_blink_flags() -> int:
		return blink_flags


class BlinkRuntimeStub:
	extends Node
	var sim := BlinkSimStub.new()
	func is_playing() -> bool:
		return true
	func tick() -> bool:
		return true
	func get_sim():
		return sim


func test_blink_frame_gates_toggle_render_passes() -> void:
	# The blink letter gates (docs/render/render-occlusion-re.md §4): indoors
	# (accum bit 0x2) hides the terrain render — near detail + far foliage ride
	# the terrain node — and the sky dome + celestials; the water letter (bit
	# 0x8) hides the water passes; leaving the boxes restores everything
	# [orig: render_main_scene @ 0x5c1353 (PolyTrn skip), the skybox skip
	# @ 0x5ca84f, the water skips @ 0x5c93cb].
	var world := _make_world()
	var sky := Node3D.new()
	sky.name = "NovaSky"
	world.add_child(sky)
	var water := Node3D.new()
	water.name = "NovaWater"
	world.add_child(water)
	add_child_autofree(world)
	var runtime := BlinkRuntimeStub.new()
	add_child_autofree(runtime)
	_install_runtime(world, runtime)

	runtime.sim.blink_flags = 0x2
	world.tick(Vector3.ZERO)
	assert_false(world.get_node("NovaTerrain").visible, "indoors hides the terrain render")
	assert_false(sky.visible, "indoors skips the skybox pass")
	assert_true(water.visible, "the indoors bit alone leaves water on")

	runtime.sim.blink_flags = 0x2 | 0x8
	world.tick(Vector3.ZERO)
	assert_false(water.visible, "the water letter suppresses the water passes")

	runtime.sim.blink_flags = 0
	world.tick(Vector3.ZERO)
	assert_true(world.get_node("NovaTerrain").visible, "outdoors restores the terrain")
	assert_true(sky.visible, "outdoors restores the sky")
	assert_true(water.visible, "outdoors restores the water")

	# An unload while indoors must not leach into the next mission.
	runtime.sim.blink_flags = 0x2
	world.tick(Vector3.ZERO)
	assert_false(sky.visible, "back indoors before the unload")
	world.unload()
	assert_true(world.get_node("NovaTerrain").visible, "unload restores the terrain gate")
	assert_true(sky.visible, "unload restores the sky gate")


class OcclusionSimStub:
	var blink_flags := 0
	var building_vis := PackedInt64Array()
	var culled := PackedInt32Array()
	var water_visible := true
	var frame_calls := 0
	func local_player_blink_flags() -> int:
		return blink_flags
	func run_occlusion_frame(_camera: Transform3D, _fov_y: float, _aspect: float,
			_near: float, _fog: float, _water_z: float, _force_indoors: bool) -> void:
		frame_calls += 1
	func get_building_visibility() -> PackedInt64Array:
		return building_vis
	func get_render_culled_bms_ids() -> PackedInt32Array:
		return culled
	func occlusion_water_visible() -> bool:
		return water_visible


class OcclusionRegistryStub:
	var nodes := {}
	func resolve_single(bms_id: int) -> Node:
		return nodes.get(bms_id)


class OcclusionRuntimeStub:
	extends Node
	var sim := OcclusionSimStub.new()
	var registry := OcclusionRegistryStub.new()
	# Present-pass stand-in: a node the "sim" hides during the runtime tick,
	# AFTER GameWorld's pre-tick occlusion restore — the real present ordering.
	var hide_on_tick: Node3D = null
	func is_playing() -> bool:
		return true
	func tick() -> bool:
		if hide_on_tick != null:
			hide_on_tick.visible = false
		return true
	func get_sim():
		return sim
	func get_registry():
		return registry


class MaskedBuildingStub:
	extends Node3D
	var applied_mask := -1
	func set_section_visibility_mask(mask: int) -> void:
		applied_mask = mask


func test_occlusion_frame_drives_masks_gates_and_water_override() -> void:
	# The section-mask/portal frame (docs/render/render-occlusion-re.md §3/§5):
	# building batch visibility + per-section masks land on the de-batched
	# building nodes, the entity render gates hide/restore collected entities,
	# and the g_BlinkWaterVisible override keeps water on while the authored
	# letter suppresses it [orig: Terrain_RenderSectorModels @ 0x5c5d30, the
	# collector gates @ 0x5c7022-0x5c708a, the water override @ 0x5c93cb].
	var world := _make_world()
	var water := Node3D.new()
	water.name = "NovaWater"
	world.add_child(water)
	add_child_autofree(world)
	var runtime := OcclusionRuntimeStub.new()
	add_child_autofree(runtime)
	var building := MaskedBuildingStub.new()
	world.add_child(building)
	var npc := Node3D.new()
	world.add_child(npc)
	runtime.registry.nodes[42] = building
	runtime.registry.nodes[7] = npc
	runtime.sim.building_vis = PackedInt64Array([42, (1 << 32) | 0x5])
	runtime.sim.culled = PackedInt32Array([7])
	_install_runtime(world, runtime)

	world.tick(Vector3.ZERO)
	assert_gt(runtime.sim.frame_calls, 0, "the occlusion frame runs each tick")
	assert_true(building.visible, "a batched building stays visible")
	assert_eq(building.applied_mask, 0x5, "the section mask lands on the node")
	assert_false(npc.visible, "the render gate hides the culled entity")

	runtime.sim.culled = PackedInt32Array()
	world.tick(Vector3.ZERO)
	assert_true(npc.visible, "an un-culled entity is restored next frame")

	runtime.sim.building_vis = PackedInt64Array([42, 0x0])
	world.tick(Vector3.ZERO)
	assert_false(building.visible, "a TOC-culled building is hidden whole")
	assert_eq(building.applied_mask, 0, "its mask carries the zeroed sections")

	# Water: the authored letter suppresses (bit 0x8), the frame override
	# restores while a straddle/window latch keeps g_BlinkWaterVisible set.
	runtime.sim.blink_flags = 0x8
	runtime.sim.water_visible = false
	world.tick(Vector3.ZERO)
	assert_false(water.visible, "letter suppression with no override hides water")
	runtime.sim.water_visible = true
	world.tick(Vector3.ZERO)
	assert_true(water.visible, "the g_BlinkWaterVisible override keeps water on")

	# Unload restores the driven nodes for the next mission.
	runtime.sim.building_vis = PackedInt64Array([42, 0x0])
	runtime.sim.culled = PackedInt32Array([7])
	world.tick(Vector3.ZERO)
	assert_false(building.visible)
	assert_false(npc.visible)
	world.unload()
	assert_true(building.visible, "unload restores building visibility")
	assert_eq(building.applied_mask, -1, "unload resets the section mask")
	assert_true(npc.visible, "unload restores gated entities")


func test_occlusion_never_resurrects_sim_hidden_nodes() -> void:
	# The visibility-write ordering contract: occlusion's restore runs BEFORE
	# the runtime tick (the present pass), so a node the sim hides during the
	# tick stays hidden even if occlusion culled it earlier and now releases
	# it — occlusion only ever HIDES on top of the present pass's base state.
	var world := _make_world()
	add_child_autofree(world)
	var runtime := OcclusionRuntimeStub.new()
	add_child_autofree(runtime)
	var npc := Node3D.new()
	world.add_child(npc)
	runtime.registry.nodes[7] = npc
	runtime.sim.culled = PackedInt32Array([7])
	_install_runtime(world, runtime)

	world.tick(Vector3.ZERO)
	assert_false(npc.visible, "occlusion culls the visible npc")

	# The sim now hides the npc (corpse despawn / WAC hide) while occlusion
	# releases it: the present-analog hide happens after the restore.
	runtime.hide_on_tick = npc
	runtime.sim.culled = PackedInt32Array()
	world.tick(Vector3.ZERO)
	assert_false(npc.visible, "the sim's hide is not overridden by the occlusion restore")

	# The sim shows it again (stops hiding): the release becomes visible.
	runtime.hide_on_tick = null
	npc.visible = true
	world.tick(Vector3.ZERO)
	assert_true(npc.visible, "an un-culled, un-hidden npc stays visible")


func test_occlusion_debug_view_builds_and_frees() -> void:
	# The F3 overlay's "Show portal faces" toggle: GameWorld builds/frees the
	# OcclusionDebugView child (the collision-view contract). Without a sim the
	# built view clears instead of erroring.
	var world := _make_world()
	add_child_autofree(world)
	world.set_occlusion_debug(true)
	var view := world.get_node_or_null(NodePath("OcclusionDebug"))
	assert_not_null(view, "enabling builds the occlusion debug child")
	assert_true(world.is_occlusion_debug())
	view.refresh_now()  # no sim resolved: clears, no error
	world.set_occlusion_debug(false)
	assert_false(world.is_occlusion_debug())
	assert_true(view.is_queued_for_deletion(), "disabling frees the view")


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


func _make_fixture_root(name: String) -> String:
	var root_dir := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join(
		"%s_%d" % [name, Time.get_ticks_usec()])
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir), OK)
	return root_dir


func _stage_minimal_fixture(name: String) -> String:
	var root_dir := _make_fixture_root(name)
	var source_dir := ProjectSettings.globalize_path("res://../fixtures/minimal/resources")
	for file_name in DirAccess.get_files_at(source_dir):
		assert_eq(DirAccess.copy_absolute(
			source_dir.path_join(file_name), root_dir.path_join(file_name)), OK)
	return root_dir


func _write_bytes(path: String, bytes: PackedByteArray) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "Fixture file should be writable: %s" % path)
	if file != null:
		file.store_buffer(bytes)
		file.close()


func _til_bytes_for_cell(cell_x: int) -> PackedByteArray:
	var bytes := PackedByteArray()
	bytes.resize(28)
	bytes.encode_u32(0, 0x74696c30)
	bytes.encode_u32(4, 1)
	bytes.encode_u32(16, cell_x * (16 << 16))
	bytes.encode_u32(20, 0)
	bytes[24] = 1
	return bytes


# PFF3: 20-byte header, 36-byte entries with 16-byte names, then payloads.
func _write_pff(path: String, entries: Array) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, "PFF fixture should be writable: %s" % path)
	if file == null:
		return
	var header_size := 20
	var entry_size := 36
	var next_offset := header_size + entries.size() * entry_size
	file.store_32(header_size)
	file.store_32(0x33464650)
	file.store_32(entries.size())
	file.store_32(entry_size)
	file.store_32(header_size)
	for entry in entries:
		var bytes: PackedByteArray = entry.bytes
		var name_bytes := String(entry.name).to_utf8_buffer()
		assert_true(name_bytes.size() <= 16, "%s fits the PFF name field" % entry.name)
		file.store_32(0)
		file.store_32(next_offset)
		file.store_32(bytes.size())
		file.store_32(0)
		for index in range(16):
			file.store_8(name_bytes[index] if index < name_bytes.size() else 0)
		file.store_32(0)
		next_offset += bytes.size()
	for entry in entries:
		file.store_buffer(entry.bytes)
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
