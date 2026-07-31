extends GutTest

const WORLD_TEST_ROOT := "game_world_test"
const ArmoryPresenter := preload("res://engine/world/armory_presenter.gd")


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


class ProfilingRuntimeStub:
	extends Node
	var requests: Array[bool] = []
	func set_runtime_profiling_enabled(enabled: bool) -> void:
		requests.append(enabled)


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


class ChallengePrewarmPlacerStub:
	extends RefCounted
	var loaded: Array[String] = []
	func resolve_player_visual_item_id(_runtime_type_id: int) -> int:
		return 101001
	func graphic_for(_item_id: int) -> String:
		return "player_body"
	func object_data_for(graphic: String):
		loaded.append(graphic)
		return null


class ChallengePrewarmWorldHarness:
	extends GameWorld
	var requested_def: PlayerViewmodelDef
	func install_prewarm_fixture(def: PlayerViewmodelDef, placer) -> void:
		requested_def = def
		_placer = placer
	func local_player_viewmodel_def() -> PlayerViewmodelDef:
		return requested_def
	func prewarm_challenge_models() -> void:
		_prewarm_loaded_model_challenge_definitions()


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

	func get_local_player_team() -> int:
		return inner.get_local_player_team()

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


class ItemFxDirectorProbe:
	extends ItemEffectDirector
	# Probe wrappers over the REAL director's private internals: the pokes stay
	# implicit-self inside the subclass, keeping the private-poke ratchet flat.
	func attach_to_node(node: Node3D, kind: int, item_id: int) -> int:
		return _attach_item_effect_to_node(node, kind, item_id)
	func attach_to_static(source: Dictionary, source_index: int) -> int:
		return _attach_item_effect_to_static(source, source_index)
	func pending_node_count() -> int:
		return _item_fx_pending_nodes.size()
	func pending_static_count() -> int:
		return _item_fx_pending_static.size()
	func deferred_control_count() -> int:
		return _item_fx_control_nodes.size()
	func active_identity_count() -> int:
		return _item_fx_control_active.size()
	func seed_owner(key: String, node: Node3D, entity_ref: Dictionary) -> void:
		_item_fx_nodes[key] = node
		_item_fx_owner_refs[key] = entity_ref.duplicate()
	func resolve_owner(key: String) -> Variant:
		return _effect_owner_transform(key)


class ItemFxGameWorldHarness:
	extends GameWorld
	# _item_fx is the world's sanctioned director-injection seam (like the
	# _runtime seam below): swap in a probe subclass of the real director so
	# tests drive the extracted code through the world's own wiring.
	var fx: ItemFxDirectorProbe
	func _init() -> void:
		fx = ItemFxDirectorProbe.new()
		fx.setup(self,
				func() -> Array:
					return _placer.get_static_item_effect_sources() if _placer != null else [],
				func() -> Variant:
					return _placer.get_item_db() if _placer != null else null)
		_item_fx = fx
	func configure_item_fx(effects: NovaEffectWorld, placer: RefCounted) -> void:
		_effect_world = effects
		_placer = placer
	func present_item_fx(node: Node3D, kind: int, item_id: int) -> int:
		return fx.attach_to_node(node, kind, item_id)
	func attach_all_item_fx() -> void:
		fx.reattach()
	func present_static_item_fx(source: Dictionary, source_index: int) -> int:
		return fx.attach_to_static(source, source_index)
	func pending_item_fx_count() -> int:
		return fx.pending_node_count()
	func pending_static_item_fx_count() -> int:
		return fx.pending_static_count()
	func deferred_control_item_fx_count() -> int:
		return fx.deferred_control_count()
	func active_control_identity_count() -> int:
		return fx.active_identity_count()
	func consume_runtime_effects(effects: Array) -> void:
		_on_runtime_effects(effects)
	func configure_item_owner(runtime: Node, key: String, node: Node3D,
			entity_ref: Dictionary) -> void:
		_runtime = runtime
		fx.seed_owner(key, node, entity_ref)
	func resolve_item_owner(key: String) -> Variant:
		return fx.resolve_owner(key)


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


class WarmEffectWorldStub:
	extends NovaEffectWorld
	var hidden_during_warm := true
	var calls: Array[String] = []
	var visibility_changes: Array[bool] = []

	func set_particles_hidden(hidden: bool) -> void:
		visibility_changes.append(hidden)
		super.set_particles_hidden(hidden)

	func warm_all_effects(_position: Vector3) -> int:
		hidden_during_warm = are_particles_hidden()
		calls.append("warm")
		return 1

	func advance_fixed_tick(_delta: float) -> void:
		calls.append("advance")

	func render_now() -> int:
		calls.append("render")
		return 1

	func reset_runtime_state() -> void:
		calls.append("reset")


class WarmItemFxStub:
	extends ItemEffectDirector
	# Director double for the warm-pass ordering probe: reattach() records the
	# particle switch it ran under instead of attaching real item effects.
	var attached_while_hidden := false

	func reattach() -> void:
		var effect_world: NovaEffectWorld = _world.get_effect_world()
		attached_while_hidden = effect_world.are_particles_hidden()


class WarmGameWorldHarness:
	extends GameWorld
	# Injects a director double through the sanctioned _item_fx seam (the
	# overridable-hook role the world's own _attach_item_effects used to play).
	var fx_stub := WarmItemFxStub.new()

	var attached_while_hidden: bool:
		get:
			return fx_stub.attached_while_hidden

	func _init() -> void:
		fx_stub.setup(self, Callable(), Callable())
		_item_fx = fx_stub

	func configure_warm_effects(effects: NovaEffectWorld) -> void:
		_effect_world = effects

	func warm_effect_catalog() -> int:
		return _warm_effect_world_catalog()



# Single stub-injection seam for this file: GameWorld builds its runtime
# internally in _start_runtime, so duck-typed transport stubs go in through
# these two helpers only (keeps the private pokes to one site). The same
# sanction covers the _item_fx director seam above: harnesses swap in a
# director probe/double instead of poking the moved item-fx privates on the
# world (ItemFxGameWorldHarness / WarmGameWorldHarness).
func _install_runtime(world, runtime, effects = null) -> void:
	world._runtime = runtime
	world._loaded = runtime != null
	world._effect_world = effects


func _detach_runtime(world) -> void:
	_install_runtime(world, null)


func test_manual_perf_probe_routes_through_the_public_runtime_gate() -> void:
	var world := _make_world()
	add_child_autofree(world)
	var runtime := ProfilingRuntimeStub.new()
	add_child_autofree(runtime)
	_install_runtime(world, runtime)

	world.set_perf_probe_enabled(true)
	world.set_perf_probe_enabled(false)
	assert_eq(runtime.requests, [true, false],
			"GameWorld forwards only consumer intent through MissionRuntime's public seam")
	_detach_runtime(world)


func test_tick_gates_the_runtime_on_its_transport() -> void:
	# The game shell's tick must respect MissionRuntime's play flag - the debug
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
	assert_eq(runtime.ticks, 1, "a playing runtime ticks once per render frame")
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
	# game_world.tscn is the game shell's embeddable world. Pin the extraction:
	# every engine node is present and the intra-scene NodePaths survived the
	# move out of main_game.tscn.
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
	var water: NovaWater = world.get_node("NovaWater")
	assert_eq(water.water_height, 0.0,
		"the retained scene must not invent water before ENV/TRN/BMS author it")
	assert_false(water.is_water_active())
	assert_eq(water.reflection_viewport.render_target_update_mode,
		SubViewport.UPDATE_DISABLED)
	water.water_height = 10.0
	assert_true(water.is_water_active(), "the authored height remains retained")
	assert_false(water.is_water_render_active(),
		"the packaged shell keeps retained water out of rendering before a load")
	assert_false(world.is_water_render_active(),
		"frame clear and occlusion use the lifecycle-aware water predicate")
	water.set_world_rendering_enabled(true)
	assert_true(world.is_water_render_active())
	water.set_world_rendering_enabled(false)


func test_explicit_bms_zero_water_beats_nonzero_terrain() -> void:
	var root_dir := OS.get_cache_dir().path_join(WORLD_TEST_ROOT).path_join(
		"zero_water_%d" % Time.get_ticks_usec())
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
	var packed := load("res://engine/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	world.set_resource_root(root)
	var mission := NovaMissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	assert_true(mission.set_header_string("terrain", "Dvxi5"))
	assert_true(mission.set_header_string("environment", "mnml"))
	assert_true(mission.set_header_int("water_override", 0))
	assert_true(mission.set_header_flag(0x1, true))
	assert_true(mission.get_environment_overrides().has("water_height"))

	assert_eq(world.load_mission_data(mission, "mnml.bms"), OK)
	var water := world.get_node("NovaWater") as NovaWater
	assert_eq(world.get_terrain_data().get_water_height(), 21,
		"fixture proves the lower-priority TRN has nonzero water")
	assert_eq(water.water_height, 0.0,
		"flagged BMS zero disables water and still beats TRN")
	assert_false(water.is_water_active())
	assert_eq(water.reflection_viewport.render_target_update_mode,
		SubViewport.UPDATE_DISABLED)


func test_net_map_missing_environment_does_not_commit_partial_render_state() -> void:
	var root_dir := _stage_minimal_fixture("net_missing_env")
	var mission_path := root_dir.path_join("mnml.bms")
	var mission := NovaMissionData.new()
	assert_eq(mission.open_file(mission_path), OK)
	assert_true(mission.set_header_int("water_override", 24))
	assert_true(mission.set_header_flag(0x1, true))
	assert_eq(mission.save_file(), OK)
	_write_pff(root_dir.path_join("resource.pff"), [{
		"name": "mnml.bms",
		"bytes": FileAccess.get_file_as_bytes(mission_path),
	}])
	assert_eq(DirAccess.remove_absolute(root_dir.path_join("mnml.env")), OK)

	var root := NovaResourceRoot.new()
	assert_eq(root.mount_runtime(root_dir, "", true), OK)
	var packed := load("res://engine/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_resource_root(root)

	# A retained environment is exactly what the failed wire load must not
	# mutate or render against. It may belong to a preview or an earlier epoch.
	var stale_env := EnvFile.new()
	stale_env.set_source_path(ProjectSettings.globalize_path(
			"res://../fixtures/minimal/resources/mnml.env"))
	assert_eq(stale_env.load(), OK)
	var env_node := world.get_node("NovaEnvironment") as NovaEnvironment
	env_node.environment_data = stale_env
	var water := world.get_node("NovaWater") as NovaWater
	water.water_height = 3.0

	assert_eq(world.load_net_session({
		"replay_host": "127.0.0.1",
		"replay_port": 9,
	}), OK)
	var client = world.get_net_client()
	assert_not_null(client)
	if client == null:
		return
	client.emit_signal("mission_known", "mnml.bms")

	assert_false(stale_env.has_mission_overrides(),
			"a missing required ENV cannot apply BMS overrides to retained data")
	assert_null(world.get_loaded_mission(),
			"an incomplete wire map stays retryable instead of latching its BMS")
	assert_eq(water.water_height, 3.0,
			"the distinct BMS water rung commits with the map, not a partial load")
	assert_false(water.is_water_render_active(),
			"terrain success alone cannot render water against a stale ENV")
	world.unload()


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
		"the production world exposes the same weapon database seam ArmoryPresenter consumes")
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
	var presenter := ArmoryPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(armory_world, null, overlay)
	assert_true(presenter.try_open(), "the production world catalog reaches first armory open")
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

	# Camera offsets move the rendered eye independently of global_position.
	# Cross the waterline with v_offset alone and pin the clear-color branch to
	# the same adjusted eye used by NovaWater strip classification.
	var water := world.get_node("NovaWater") as NovaWater
	var env := world.get_node("NovaEnvironment") as NovaEnvironment
	water.set_height_override(10.0)
	camera.position.y = 10.25
	camera.v_offset = -1.0
	assert_lt(camera.get_camera_transform().origin.y, water.water_height)
	await get_tree().process_frame
	var combined := EnvFile.combine_terrain_light(
			Color(env.get_sun_light().x, env.get_sun_light().y, env.get_sun_light().z),
			Color(env.get_sky_ambient().x, env.get_sky_ambient().y, env.get_sky_ambient().z))
	var lit := EnvFile.lit_water_color(
			Color(env.get_water_color().x, env.get_water_color().y, env.get_water_color().z),
			combined)
	assert_eq(world.get_current_frame_clear_color(), Color(lit.r, lit.g, lit.b),
			"v_offset below water selects the underwater clear even when the node origin is above")
	camera.v_offset = 0.0
	camera.position.y = 71.0
	water.set_height_override(NAN)
	await get_tree().process_frame
	assert_eq(world.get_current_frame_clear_color(), mission_clear)

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
	# LocalPlayerPresenter, always renders in it).
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
	assert_eq(mirror.cull_mask & NovaWater.VISUAL_LAYER_SHADOW_CASTER_MASK, 0,
		"caster-only helper instances never enter the color reflection")
	assert_ne(mirror.cull_mask & NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY, 0,
		"the FP-mode local body DOES render in the mirror")
	assert_ne(mirror.cull_mask & NovaWater.VISUAL_LAYER_WORLD, 0,
		"the mirrored scene renders the normal world")
	assert_eq(water.mesh_instance.layers, NovaWater.VISUAL_LAYER_WATER,
		"the water strip rides the water-only layer the mirror excludes")


func test_game_world_is_playable_by_default_without_env_flag() -> void:
	var world := _make_world()
	add_child_autofree(world)
	assert_true(world.is_playable(),
			"normal and F6 standalone launches spawn a player by default")
	world.set_playable(false)
	assert_false(world.is_playable(),
			"diagnostic fixtures can explicitly opt out of local-player setup")


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

	# 0.128 seconds advances eight 62.5 Hz simulation ticks but only seven
	# recovered 62 Hz weather/TOD ticks. Those clocks must remain distinct.
	world.tick(Vector3.ZERO, Transform3D(), 0.128)
	var advanced := env.time_of_day
	var expected_clock := NovaEnvironment.new()
	expected_clock.configure_mission_clock(0x0540, 60)
	expected_clock.advance_mission_clock(7)
	assert_almost_eq(advanced, expected_clock.time_of_day, 0.000001,
			"the mission clock advances on the separate 62 Hz weather cadence")
	assert_eq(int(world.get_runtime().get_perf_counters().get("ticks", 0)), 8,
			"mission simulation retains its 62.5 Hz cadence")
	expected_clock.free()

	# F3/MCP scrubs are a host-level operation: pause guarantees no later
	# weather tick can hide a missing resync behind normal advancement.
	world.get_runtime().pause()
	assert_eq(world.debug_set_mission_minute_of_day(22.0 * 60.0 + 7.0), OK)
	assert_almost_eq(env.time_of_day, 2207.0, 0.001)
	var weather := world.get_weather_node() as NovaWeather
	assert_true(weather.get_smooth_fill().is_equal_approx(
			env.get_fill_light_target()))
	assert_true(weather.get_smooth_sun().is_equal_approx(
			env.get_sun_light_target()))
	assert_true(weather.get_smooth_fog().is_equal_approx(
			env.get_fog_color_target()))
	world.tick(Vector3.ZERO, Transform3D(), 0.25)
	assert_almost_eq(env.time_of_day, 2207.0, 0.001,
			"a paused host keeps the scrubbed clock and rendered weather")
	world.unload()


func _world_driven_weather_state_after(deltas: Array) -> Array:
	var packed := load("res://engine/world/game_world.tscn") as PackedScene
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)

	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/minimal/resources")), OK)
	world.set_resource_root(root)
	var mission := NovaMissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	mission.set_header_int("start_time", 0x0540)
	mission.set_header_int("minutes_per_day", 60)
	assert_eq(world.load_mission_data(mission, "mnml.bms"), OK)

	var sim_ticks := 0
	for delta in deltas:
		world.tick(Vector3.ZERO, Transform3D(), float(delta))
		sim_ticks += int(world.get_runtime().get_perf_counters().get("ticks", 0))
	var env := world.get_node("NovaEnvironment") as NovaEnvironment
	var weather := world.get_node("NovaWeather") as NovaWeather
	var state := [
		sim_ticks,
		env.time_of_day,
		weather.get_smooth_fill(),
		weather.get_smooth_sun(),
		weather.get_smooth_fog(),
		weather.get_smooth_sky(),
		weather.get_smooth_skyfog(),
		weather.get_smooth_sky_base(),
		weather.get_smooth_sky_bright(),
		weather.get_smooth_sky_highlight(),
		weather.get_smooth_cloud_base(),
		weather.get_smooth_cloud_highlight(),
		weather.get_smooth_cloud_edge(),
		weather.get_sway_amount(),
		weather.get_sway_phase(),
	]
	world.unload()
	return state


func test_world_driven_weather_is_invariant_to_render_batching() -> void:
	var slow: Array = await _world_driven_weather_state_after([0.128])
	var split: Array = await _world_driven_weather_state_after([
		0.016, 0.016, 0.016, 0.016, 0.016, 0.016, 0.016, 0.016,
	])
	assert_eq(slow, split,
			"each 62 Hz clock advance refreshes TOD targets before one weather tick")
	assert_eq(int(slow[0]), 8, "0.128 seconds still contains eight simulation ticks")

	var expected_seven := NovaEnvironment.new()
	expected_seven.configure_mission_clock(0x0540, 60)
	expected_seven.advance_mission_clock(7)
	assert_almost_eq(float(slow[1]), expected_seven.time_of_day, 0.000001,
			"0.128 seconds contains seven weather/TOD ticks")
	expected_seven.free()

	var eighth_delta := 8.0 / GameWorld.WEATHER_TICK_HZ - 0.128 + 0.000001
	var boundary: Array = await _world_driven_weather_state_after([0.128, eighth_delta])
	var expected_eight := NovaEnvironment.new()
	expected_eight.configure_mission_clock(0x0540, 60)
	expected_eight.advance_mission_clock(8)
	assert_eq(int(boundary[0]), 8,
			"the extra weather quantum is shorter than one simulation tick")
	assert_almost_eq(float(boundary[1]), expected_eight.time_of_day, 0.000001,
			"the residual weather credit consumes the eighth TOD tick")
	expected_eight.free()


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


# Typed net-session request builders (the records GameWorld's host/joiner entries
# consume; expansion/game_type ride the record defaults: "" + GAME_TYPE_COOP).
func _lan_host_config(mission: String, bind_port: int) -> HostSessionConfig:
	var config := HostSessionConfig.new()
	config.mission = mission
	config.bind_port = bind_port
	return config


func _join_target(host_ip: String, port: int, mission := "", player_name := "Joiner") -> JoinTarget:
	var target := JoinTarget.new()
	target.host_ip = host_ip
	target.port = port
	target.mission = mission
	target.player_name = player_name
	return target


func test_failed_host_load_does_not_arm_the_next_mission_as_a_lan_host() -> void:
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/minimal/resources")), OK)
	world.set_resource_root(root)

	assert_eq(world.load_mission_as_host(_lan_host_config("missing.bms", 0)),
		ERR_FILE_NOT_FOUND)
	assert_eq(world.load_mission("mnml.bms"), OK)
	var sim: NovaSimulation = world.get_sim()
	assert_not_null(sim)
	if sim != null:
		assert_false(sim.is_host_listening(),
			"a rejected host request cannot turn a later ordinary mission into a LAN host")
	world.unload()


func test_lan_host_threads_truthful_base_metadata_into_the_native_session() -> void:
	# GameConfig's old capture-shaped defaults include jox01; the production
	# GameWorld handoff must explicitly replace them with the menu/root values,
	# including the meaningful empty string for a base-game mount.
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/minimal/resources")), OK)
	world.set_resource_root(root)

	assert_eq(world.load_mission_as_host(_lan_host_config("mnml.bms", 0)), OK)
	var sim: NovaSimulation = world.get_sim()
	assert_not_null(sim)
	if sim != null:
		var config := sim.get_host_session_config()
		assert_eq(int(config.get("gametype", 0)), 0x30020)
		assert_eq(String(config.get("expansion", "missing")), "",
			"base JO stays empty instead of falling back to captured jox01")
	world.unload()


func test_lan_host_bind_failure_is_reported_instead_of_falling_back_socketless() -> void:
	# Reserve an OS-chosen endpoint, then request that exact port through the
	# production GameWorld host path. The old behavior silently started an SP
	# listen session and still emitted world_loaded, leaving joiners no socket.
	var blocker := NovaUdpPump.new()
	assert_eq(blocker.bind_listen(0), OK)
	var occupied_port := blocker.local_port()
	assert_gt(occupied_port, 0)

	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/minimal/resources")), OK)
	world.set_resource_root(root)
	watch_signals(world)
	var failures: Array[String] = []
	world.load_failed.connect(func(reason: String): failures.append(reason))

	var result := world.load_mission_as_host(_lan_host_config("mnml.bms", occupied_port))
	assert_eq(result, ERR_CANT_CREATE)
	assert_false(world.is_loaded())
	assert_null(world.get_sim(), "a failed UDP host bind creates no socketless fallback sim")
	assert_signal_not_emitted(world, "world_loaded")
	assert_eq(failures.size(), 1)
	if failures.size() == 1:
		assert_string_contains(failures[0], str(occupied_port),
			"the launch error identifies the exact requested port")
	blocker.close()


func test_lan_host_bind_failure_survives_synchronous_teardown_handler() -> void:
	# The game shell returns to the menu from INSIDE load_failed — its teardown
	# calls unload(), which frees the failed runtime. The bind-failure leg must
	# free/null its runtime before emitting; emitting first made the handler's
	# reentry turn the follow-up free into a null-instance error.
	var blocker := NovaUdpPump.new()
	assert_eq(blocker.bind_listen(0), OK)
	var occupied_port := blocker.local_port()
	assert_gt(occupied_port, 0)

	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/minimal/resources")), OK)
	world.set_resource_root(root)
	var failures: Array[String] = []
	world.load_failed.connect(func(reason: String):
		failures.append(reason)
		world.unload())

	var result := world.load_mission_as_host(_lan_host_config("mnml.bms", occupied_port))
	assert_eq(result, ERR_CANT_CREATE)
	assert_eq(failures.size(), 1)
	assert_false(world.is_loaded())
	assert_null(world.get_sim())
	blocker.close()


func test_escape_aborts_the_joiner_preload_wait() -> void:
	# ESC during a load: the joiner's pre-load connect/session wait is the one
	# interruptible leg — the reachable analog of the original per-asset abort
	# poll [orig: Client_CheckDisconnectOrEscDuringLoad @ 0x520270]
	# (docs/interface/loading-screen-re.md D-LOADSCR-7).
	var blocker := NovaUdpPump.new()  # a bound but silent "host": never replies
	assert_eq(blocker.bind_listen(0), OK)
	var silent_port := blocker.local_port()
	assert_gt(silent_port, 0)

	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/minimal/resources")), OK)
	world.set_resource_root(root)
	var failures: Array[String] = []
	world.load_failed.connect(func(reason: String): failures.append(reason))

	assert_false(world.cancel_join_preload(), "no preload in flight is a no-op")
	assert_eq(world.load_mission_as_joiner(
			_join_target("127.0.0.1", silent_port, "", "EscTester")), OK)
	await get_tree().process_frame  # the deferred preload driver starts
	assert_true(world.cancel_join_preload(), "an in-flight preload aborts")
	assert_eq(failures.size(), 1)
	if failures.size() == 1:
		assert_string_contains(failures[0], "aborted")
	assert_false(world.is_loaded())
	await get_tree().process_frame  # the canceled driver loop unwinds quietly
	assert_eq(failures.size(), 1, "the canceled driver does not double-report")
	blocker.close()


func test_escape_aborts_the_joiner_admission_wait() -> void:
	# ESC during the SECOND interruptible joiner wait: an explicit-mission joiner
	# loads its map locally and then parks in the post-load admission watchdog
	# until the (here: silent) host drives the join forward. cancel_join_admission
	# tells that armed wait to abort; the watchdog reports through the ordinary
	# load-failure leg exactly once, on its next process_frame resume.
	var blocker := NovaUdpPump.new()  # a bound but silent "host": never replies
	assert_eq(blocker.bind_listen(0), OK)
	var silent_port := blocker.local_port()
	assert_gt(silent_port, 0)

	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/minimal/resources")), OK)
	world.set_resource_root(root)
	var failures: Array[String] = []
	world.load_failed.connect(func(reason: String): failures.append(reason))

	assert_false(world.cancel_join_admission(), "no armed admission wait is a no-op")
	assert_eq(world.load_mission_as_joiner(
			_join_target("127.0.0.1", silent_port, "mnml")), OK)
	assert_true(world.cancel_join_admission(), "an armed admission wait accepts the abort")
	await get_tree().process_frame  # the watchdog resumes and observes the abort
	assert_eq(failures.size(), 1)
	if failures.size() == 1:
		assert_string_contains(failures[0], "aborted")
	await get_tree().process_frame
	assert_eq(failures.size(), 1, "the aborted watchdog does not double-report")
	assert_false(world.cancel_join_admission(), "the wait is disarmed after the abort")
	world.unload()
	blocker.close()


func test_failed_join_load_does_not_make_the_next_mission_wire_only() -> void:
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(
			ProjectSettings.globalize_path("res://../fixtures/minimal/resources")), OK)
	world.set_resource_root(root)

	assert_eq(world.load_mission_as_joiner(
			_join_target("127.0.0.1", 9, "missing.bms")), ERR_FILE_NOT_FOUND)
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


func test_editor_run_loads_the_exact_saved_loose_bms() -> void:
	var root_dir := _stage_minimal_fixture("exact_loose_bms")
	_write_pff(root_dir.path_join("resource.pff"), [{
		"name": "mnml.bms",
		"bytes": "not a mission".to_utf8_buffer(),
	}])

	var resource_root := NovaResourceRoot.new()
	assert_eq(resource_root.mount_runtime(root_dir, "", true), OK)
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	world.set_playable(false)
	world.set_resource_root(resource_root)

	assert_eq(world.load_loose_mission("mnml.bms"), OK,
		"F6 opens the valid disk BMS even when the archive row is corrupt")
	assert_eq(world.get_loaded_mission_file(), "mnml.bms")
	world.unload()


func test_editor_run_rejects_non_top_level_or_non_bms_paths() -> void:
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	assert_eq(world.load_loose_mission("nested/mnml.bms"), ERR_INVALID_PARAMETER)
	assert_eq(world.load_loose_mission("../mnml.bms"), ERR_INVALID_PARAMETER)
	assert_eq(world.load_loose_mission("mnml.mis"), ERR_INVALID_PARAMETER)


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


func test_joiner_challenge_prewarm_loads_player_and_current_viewmodels_before_freeze() -> void:
	var world: ChallengePrewarmWorldHarness = autofree(
			ChallengePrewarmWorldHarness.new())
	var placer := ChallengePrewarmPlacerStub.new()
	world.install_prewarm_fixture(PlayerViewmodelDef.from_weapon_dict({
		"name": "WPN_TEST",
		"gfx1": "test_gun",
		"gfx1a": "test_arms",
		"flags": 0,
	}), placer)

	world.prewarm_challenge_models()

	assert_eq(placer.loaded, ["player_body", "test_gun", "test_arms"],
		"the frozen 0x3D source includes every .3DI the first player frame would load")


func test_joiner_accepts_novaworld_advertised_mission_basename() -> void:
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	var root := NovaResourceRoot.new()
	var fixture_dir := ProjectSettings.globalize_path("res://../fixtures/minimal/resources")
	assert_eq(root.set_root_dir(fixture_dir), OK)
	world.set_resource_root(root)

	var err := world.load_mission_as_joiner(_join_target("127.0.0.1", 9, "mnml"))
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


func test_retained_debug_views_rearm_after_unload_and_reload() -> void:
	var root_dir := _stage_minimal_fixture("debug_view_reload")
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)
	world.set_resource_root(root)
	assert_eq(world.load_mission("mnml.bms"), OK)

	world.set_skeleton_debug(true)
	world.set_user_point_debug(true)
	world.set_collision_debug(true)
	world.set_occlusion_debug(true)
	world.set_round_debug(true)
	world.set_hitbox_debug(true)
	var debug_names := [
		"SkeletonDebug",
		"UserPointDebug",
		"CollisionDebug",
		"OcclusionDebug",
		"RoundDebug",
		"HitboxDebug",
	]
	for debug_name in debug_names:
		assert_not_null(world.get_node_or_null(NodePath(debug_name)),
				"%s exists before reload" % debug_name)

	world.unload()
	assert_true(world.is_skeleton_debug())
	assert_true(world.is_user_point_debug())
	assert_true(world.is_collision_debug())
	assert_true(world.is_occlusion_debug())
	assert_true(world.is_round_debug())
	assert_true(world.is_hitbox_debug())
	for debug_name in debug_names:
		assert_null(world.get_node_or_null(NodePath(debug_name)),
				"%s detaches immediately during unload" % debug_name)

	assert_eq(world.load_mission("mnml.bms"), OK)
	for debug_name in debug_names:
		assert_not_null(world.get_node_or_null(NodePath(debug_name)),
				"%s is rebuilt for the replacement mission" % debug_name)
	world.unload()


func test_pick_helpers_keep_stable_names_on_same_frame_replacement() -> void:
	var world := _make_world()
	add_child_autofree(world)
	await get_tree().process_frame
	var first_picks := NovaDebugPickList.new()
	var replacement_picks := NovaDebugPickList.new()

	world.set_pick_debug(first_picks)
	var first_view := world.get_node_or_null("PickDebug")
	assert_not_null(first_view)
	world.set_pick_debug(replacement_picks)
	var replacement_view := world.get_node_or_null("PickDebug")
	assert_not_null(replacement_view)
	assert_ne(replacement_view, first_view)
	assert_null(first_view.get_parent(),
			"the old view detaches before its deferred destruction")
	assert_eq(replacement_view.name, &"PickDebug")

	world.set_pick_click_enabled(true)
	var first_catcher := world.get_node_or_null("PickClickCatcher")
	assert_not_null(first_catcher)
	world.set_pick_click_enabled(true)
	var replacement_catcher := world.get_node_or_null("PickClickCatcher")
	assert_not_null(replacement_catcher)
	assert_ne(replacement_catcher, first_catcher)
	assert_null(first_catcher.get_parent(),
			"the old click catcher also detaches before replacement")
	assert_eq(replacement_catcher.name, &"PickClickCatcher")


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


# The joiner observer's seam: a PLAYING runtime whose sim reports the in-match
# joiner state game_world reads once per host tick.
class JoinerSignalSimStub:
	extends RefCounted
	var loss_reason := ""
	var in_match := true
	var deploy_pending := false
	func is_joiner() -> bool:
		return true
	func is_joined_in_match() -> bool:
		return in_match
	func is_join_deploy_pick_pending() -> bool:
		return deploy_pending
	func get_session_loss_reason() -> String:
		return loss_reason


class JoinerSignalRuntimeStub:
	extends Node
	var sim = null
	func is_playing() -> bool:
		return true
	func tick() -> bool:
		return true
	func get_sim():
		return sim


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


func test_effect_warm_temporarily_lifts_and_restores_the_particle_switch() -> void:
	var world := WarmGameWorldHarness.new()
	var effects := WarmEffectWorldStub.new()
	world.configure_warm_effects(effects)
	world.set_particles_hidden(true)

	assert_eq(world.warm_effect_catalog(), 1)

	assert_false(effects.hidden_during_warm,
			"the persistent gameplay preference cannot suppress load warming")
	assert_eq(effects.calls, ["warm", "advance", "render", "reset"],
			"the warm snapshot is submitted before its runtime values reset")
	assert_eq(effects.visibility_changes, [true, false, true],
			"warming lifts the switch only for the covered load pass")
	assert_true(effects.are_particles_hidden(),
			"the user's particle preference is restored before returning")
	assert_true(world.attached_while_hidden,
			"persistent item effects reattach under the restored preference")


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


func test_synthetic_controller_effects_are_scoped_to_their_wire_sibling() -> void:
	const PLAYER_CONTROL_ITEM := 101291
	var world := _make_item_fx_world()
	add_child_autofree(world)
	var effects := FxWorldStub.new()
	world.add_child(effects)
	var db := ItemFxDbStub.new()
	db.attribs[PLAYER_CONTROL_ITEM] = 0x40
	db.effects[PLAYER_CONTROL_ITEM] = {
		"particlefx": {
			"effect": "Effect_whiteExhaust",
			"userpoint": "FX00",
		},
	}
	var placer := ItemFxPlacerStub.new()
	placer.item_db = db
	world.configure_item_fx(effects, placer)

	var siblings: Array[ItemFxModelStub] = []
	for wire_handle in [0x1004, 0x1005]:
		var model := ItemFxModelStub.new()
		model.set_meta("entity_ref", {
			"kind": NovaMissionData.KIND_ITEM,
			"origin_kind": 0xff,
			"index": 0xffffff,
			"bms_id": 0,
			"wire_handle": wire_handle,
			"item_id": PLAYER_CONTROL_ITEM,
		})
		world.add_child(model)
		siblings.append(model)
		assert_eq(world.present_item_fx(
				model, NovaMissionData.KIND_ITEM, PLAYER_CONTROL_ITEM), 0)

	world.consume_runtime_effects([{
		"kind": "vehicle_control_started",
		"a": 0,
		"b": 0,
		"c": -1,
		"wire_handle": 0x1004,
	}])
	assert_eq(effects.attached_spawns.size(), 1,
			"mounting one synthetic emplacement starts only that sibling's effect")
	assert_eq(String(effects.attached_spawns[0].owner),
			"itemfx:%d:0" % siblings[0].get_instance_id())

	world.consume_runtime_effects([{
		"kind": "vehicle_control_stopped",
		"a": 0,
		"b": 0,
		"c": -1,
		"wire_handle": 0x1004,
	}, {
		"kind": "vehicle_control_started",
		"a": 0,
		"b": 0,
		"c": -1,
		"wire_handle": 0x1005,
	}])
	assert_eq(effects.stopped_groups, [1])
	assert_eq(effects.attached_spawns.size(), 2)
	assert_eq(String(effects.attached_spawns[1].owner),
			"itemfx:%d:0" % siblings[1].get_instance_id(),
			"the shared synthetic origin cannot activate a neighboring attachment")


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
	# building_vis / culled hold the CURRENT frame verdicts; the stub mirrors
	# the native delta contract by diffing against what it last emitted.
	var blink_flags := 0
	var building_vis := PackedInt64Array()
	var culled := PackedInt32Array()
	var water_visible := true
	var frame_calls := 0
	var iris_calls := 0
	# bms_id -> true when the sim's present intent for the entity is HIDDEN
	# (entity_present_visible returns false), the release-edge consult.
	var present_hidden := {}
	var _last_building := {}
	var _last_culled := PackedInt32Array()
	func local_player_blink_flags() -> int:
		return blink_flags
	func run_occlusion_frame(_camera: Transform3D, _fov_y: float, _aspect: float,
			_near: float, _fog: float, _water_z: float, _force_indoors: bool) -> void:
		frame_calls += 1
	func get_building_visibility_changes() -> PackedInt64Array:
		var out := PackedInt64Array()
		for i in range(0, building_vis.size(), 2):
			var bms_id := int(building_vis[i])
			var packed := int(building_vis[i + 1])
			if int(_last_building.get(bms_id, -1)) != packed:
				_last_building[bms_id] = packed
				out.append(bms_id)
				out.append(packed)
		return out
	func get_render_culled_changes() -> PackedInt32Array:
		var added := PackedInt32Array()
		var removed := PackedInt32Array()
		for id in culled:
			if not _last_culled.has(id):
				added.append(id)
		for id in _last_culled:
			if not culled.has(id):
				removed.append(id)
		_last_culled = culled.duplicate()
		var out := PackedInt32Array([added.size()])
		out.append_array(added)
		out.append(removed.size())
		out.append_array(removed)
		return out
	func entity_present_visible(bms_id: int) -> bool:
		return not present_hidden.has(bms_id)
	func reset_occlusion_apply_baseline() -> void:
		_last_building.clear()
		_last_culled = PackedInt32Array()
	func occlusion_water_visible() -> bool:
		return water_visible
	func compute_iris_samples(_origin: Vector3, _forward: Vector3,
			_light_dir: Vector3) -> PackedFloat32Array:
		iris_calls += 1
		return PackedFloat32Array([0.25, 0.5, 0.75])


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
	var mask_calls := 0
	func set_section_visibility_mask(mask: int) -> void:
		applied_mask = mask
		mask_calls += 1


class IrisWeatherStub:
	extends Node
	var iris_samples := PackedFloat32Array()


class WaterStatsStub:
	extends Node3D
	var reflection_viewport := SubViewport.new()
	func _init() -> void:
		name = "NovaWater"
		reflection_viewport.size = Vector2i(16, 16)
		add_child(reflection_viewport)


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
	# Two-bit visibility ownership: an occlusion RELEASE lands the node on the
	# sim's CURRENT present intent (entity_present_visible), so a node the sim
	# hid while occlusion owned it stays hidden — occlusion only ever HIDES on
	# top of the present pass's base state, never force-shows.
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
	# releases it: the release consults the sim's intent and leaves it hidden.
	runtime.hide_on_tick = npc
	runtime.sim.present_hidden[7] = true
	runtime.sim.culled = PackedInt32Array()
	world.tick(Vector3.ZERO)
	assert_false(npc.visible, "the occlusion release honors the sim's hide")

	# The sim shows it again (stops hiding): with no occlusion claim left, the
	# present drive owns visibility alone.
	runtime.hide_on_tick = null
	runtime.sim.present_hidden.clear()
	npc.visible = true
	world.tick(Vector3.ZERO)
	assert_true(npc.visible, "an un-culled, un-hidden npc stays visible")


func test_probe_occlusion_skip_restores_frame_state_and_keeps_iris_live() -> void:
	var world := _make_world()
	var water := Node3D.new()
	water.name = "NovaWater"
	world.add_child(water)
	var weather := IrisWeatherStub.new()
	weather.name = "NovaWeather"
	world.add_child(weather)
	add_child_autofree(world)
	var runtime := OcclusionRuntimeStub.new()
	add_child_autofree(runtime)
	var building := MaskedBuildingStub.new()
	world.add_child(building)
	var npc := Node3D.new()
	world.add_child(npc)
	runtime.registry.nodes[42] = building
	runtime.registry.nodes[7] = npc
	# A zero batch-visible bit hides the whole building while the low word still
	# drives its section mask.
	runtime.sim.building_vis = PackedInt64Array([42, 0x5])
	runtime.sim.culled = PackedInt32Array([7])
	runtime.sim.blink_flags = 0x8
	runtime.sim.water_visible = true
	_install_runtime(world, runtime)

	world.tick(Vector3.ZERO)
	assert_false(building.visible)
	assert_eq(building.applied_mask, 0x5)
	assert_false(npc.visible)
	assert_true(water.visible,
			"the prior occlusion frame may override the authored water suppression")
	assert_eq(runtime.sim.iris_calls, 1)
	assert_true((world.get("_perf_probe_spans") as Dictionary).is_empty(),
			"normal frames do not pay for or retain probe spans")

	world.set("_mission_forces_indoors", true)
	world.call("set_perf_probe_enabled", true)
	world.set("_perf_probe_skip_occl", true)
	var frame_calls := runtime.sim.frame_calls
	world.tick(Vector3.ZERO)
	assert_eq(runtime.sim.frame_calls, frame_calls,
			"the probe skips only the new occlusion frame")
	assert_true(building.visible, "the prior frame's whole-building hide is restored")
	assert_eq(building.applied_mask, -1,
			"entering the skip resets retained section masks exactly once")
	assert_true(npc.visible, "the prior frame's entity cull is restored")
	assert_false(water.visible,
			"entering the skip restores authored water visibility before bypassing occlusion")
	assert_true(bool(world.get("_mission_forces_indoors")),
			"probe cleanup cannot erase authored mission semantics")
	assert_eq(runtime.sim.iris_calls, 2, "iris exposure still samples while occlusion is skipped")
	assert_eq(weather.iris_samples, PackedFloat32Array([0.25, 0.5, 0.75]))
	var spans: Dictionary = world.get("_perf_probe_spans")
	assert_eq(int(spans.get("occl_frame", -1)), 0,
			"a skipped phase reports zero rather than a stale prior span")
	assert_true(spans.has("iris"), "enabled probe frames publish the live iris span")

	world.set("_perf_probe_skip_occl", false)
	world.tick(Vector3.ZERO)
	assert_eq(runtime.sim.frame_calls, frame_calls + 1,
			"leaving the skip resumes the render-occlusion frame")
	assert_false(building.visible,
			"leaving the skip reapplies whole-building visibility")
	assert_eq(building.applied_mask, 0x5,
			"leaving the skip reapplies retained section masks")
	assert_false(npc.visible, "leaving the skip reapplies entity culling")
	assert_true(water.visible,
			"the resumed occlusion frame may reapply its water override")


func test_occlusion_steady_frames_touch_no_nodes() -> void:
	# The diff-based apply: verdicts that did not change emit no work — no
	# section-mask dispatches and no visibility flips, frame after frame. This
	# is the churn contract the perf slice exists for (the old form restored
	# and re-hid the whole occluded set every frame).
	var world := _make_world()
	add_child_autofree(world)
	var runtime := OcclusionRuntimeStub.new()
	add_child_autofree(runtime)
	var building := MaskedBuildingStub.new()
	world.add_child(building)
	var npc := Node3D.new()
	world.add_child(npc)
	runtime.registry.nodes[42] = building
	runtime.registry.nodes[7] = npc
	runtime.sim.building_vis = PackedInt64Array([42, 0x5])  # batch-hidden, mask 0x5
	runtime.sim.culled = PackedInt32Array([7])
	_install_runtime(world, runtime)

	world.tick(Vector3.ZERO)
	assert_false(building.visible, "the batch-culled building hides on the change frame")
	assert_false(npc.visible, "the gated npc hides on the change frame")
	var calls := building.mask_calls
	world.tick(Vector3.ZERO)
	world.tick(Vector3.ZERO)
	assert_eq(building.mask_calls, calls,
			"steady verdicts dispatch no section-mask calls")
	assert_false(building.visible, "the hidden building stays hidden with no flips")
	assert_false(npc.visible, "the culled npc stays hidden with no flips")


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


# An ESTABLISHED in-match session that goes silent past the witnessed connection
# reap window must reach the shell exactly once. Retail reaps at
# cs_dir0.timeout_ms = 120000 and its disconnect event exits the mission with a
# mapped reason -- there is no in-world dialog -- so the host surfaces a reason and
# the shell owns the presentation.
# [orig: CNapiNetwork_Init @ 0x4ca4a0 -> CNapiNetwork_OnDisconnectedFromServer @ 0x4c63d0]
func test_tick_emits_session_lost_once_for_an_in_match_loss() -> void:
	var world := _make_world()
	add_child_autofree(world)
	var runtime := JoinerSignalRuntimeStub.new()
	var sim := JoinerSignalSimStub.new()
	runtime.sim = sim
	add_child_autofree(runtime)
	_install_runtime(world, runtime)
	var reasons: Array = []
	world.session_lost.connect(func(reason: String) -> void: reasons.append(reason))

	world.tick(Vector3.ZERO)
	assert_eq(reasons.size(), 0, "a healthy in-match session emits no loss")

	sim.loss_reason = "lost connection to the host (no traffic for 120 seconds)"
	world.tick(Vector3.ZERO)
	assert_eq(reasons, ["lost connection to the host (no traffic for 120 seconds)"],
		"the loss edge surfaces the host's reason verbatim")

	# The reason latches true inside the runtime; the observer must not re-notify.
	world.tick(Vector3.ZERO)
	world.tick(Vector3.ZERO)
	assert_eq(reasons.size(), 1, "a latched loss reason emits exactly once per session")

	_detach_runtime(world)


func test_tick_never_emits_session_lost_for_a_non_joiner_or_a_silent_seam() -> void:
	# The observer is duck-typed: a render-only runtime double with no session seam
	# must not be treated as a lost session.
	var world := _make_world()
	add_child_autofree(world)
	var runtime := TransportRuntimeStub.new()
	add_child_autofree(runtime)
	runtime.play()
	_install_runtime(world, runtime)
	var reasons: Array = []
	world.session_lost.connect(func(reason: String) -> void: reasons.append(reason))
	world.tick(Vector3.ZERO)
	world.tick(Vector3.ZERO)
	assert_eq(reasons.size(), 0, "a runtime without the session seam never reports a loss")
	_detach_runtime(world)


func test_stats_board_captures_world_tick_legs_only_while_enabled() -> void:
	# The F3 Stats feeds (FrameStatsBoard): a disabled board costs the tick
	# nothing and receives nothing; an enabled one gets every world leg — the
	# occlusion apply split included — without touching the probe dicts.
	var world := _make_world()
	var water := WaterStatsStub.new()
	world.add_child(water)
	add_child_autofree(world)
	var runtime := OcclusionRuntimeStub.new()
	add_child_autofree(runtime)
	var building := MaskedBuildingStub.new()
	world.add_child(building)
	runtime.registry.nodes[42] = building
	runtime.sim.building_vis = PackedInt64Array([42, (1 << 32) | 0x5])
	_install_runtime(world, runtime)
	var board := FrameStatsBoard.new()
	world.set_frame_stats_board(board)

	world.tick(Vector3.ZERO)
	assert_eq(board.drain().sample_frames[FrameStatsBoard.OCCL_APPLY], 0,
			"a disabled board sees no feeds")

	board.set_capture_active(true)
	world.tick(Vector3.ZERO)
	var counts := board.drain().sample_frames
	assert_gt(counts[FrameStatsBoard.OCCL_APPLY], 0, "the GDScript apply leg lands")
	assert_gt(counts[FrameStatsBoard.OCCL_GLUE], 0,
			"a sim without the native split feeds the whole native call as glue")
	assert_eq(counts[FrameStatsBoard.OCCL_BUILD], 0,
			"no native split getters -> no build slot")
	assert_gt(counts[FrameStatsBoard.WORLD_WEATHER], 0)
	assert_gt(counts[FrameStatsBoard.WORLD_BLINK], 0)
	assert_gt(counts[FrameStatsBoard.WORLD_IRIS], 0)
	assert_gt(counts[FrameStatsBoard.WORLD_FOLIAGE], 0)
	assert_gt(counts[FrameStatsBoard.WORLD_RUNTIME], 0)
	assert_gt(counts[FrameStatsBoard.WORLD_AUDIO], 0)
	assert_true(world.is_water_render_stats_measured(),
			"capture enables the reflection viewport's render-time measurement")

	board.set_capture_active(false)
	assert_false(world.is_water_render_stats_measured(),
			"the capture edge tears measurement down without another world tick")
	board.set_capture_active(true)
	world.tick(Vector3.ZERO)
	assert_true(world.is_water_render_stats_measured())
	world.set_frame_stats_board(null)
	assert_false(world.is_water_render_stats_measured(),
			"detaching the board also releases measurement immediately")


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
