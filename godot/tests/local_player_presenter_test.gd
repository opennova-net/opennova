extends GutTest

const LocalPlayerPresenter := preload("res://engine/world/local_player_presenter.gd")
const MissionRuntime := preload("res://engine/world/mission_runtime.gd")


class FakeWeaponPart:
	extends Node3D
	var plays: Array = []
	var times: Array[float] = []
	var ctrl_values: Dictionary = {}
	var lighting_contexts: Array = []

	func play_body_clip(key: String) -> void:
		plays.append({"key": key, "variant": 0})

	func play_body_clip_variant(key: String, variant: int) -> void:
		plays.append({"key": key, "variant": variant})

	func play_body_clip_variant_at_time(key: String, variant: int,
			seconds: float) -> void:
		plays.append({"key": key, "variant": variant})
		times.append(seconds)

	func set_animation_time(seconds: float) -> void:
		times.append(seconds)

	func get_object_data():
		return null

	func set_ctrl_value(name: String, value: int) -> void:
		ctrl_values[name] = value

	func clear_ctrl_value(name: String) -> void:
		ctrl_values.erase(name)

	func set_entity_lighting_context(effect_scale: float, interior_lerp: bool,
			interior_daylight: float) -> void:
		lighting_contexts.append([
			effect_scale, interior_lerp, interior_daylight])


class FakeInteriorItemDb:
	extends RefCounted
	var transfers := {}

	func get_light_transfer(item_id: int) -> float:
		return float(transfers.get(item_id, 0.0))


class FakeUserPointData:
	extends RefCounted
	var info := {
		"name": "mflash01",
		"position": Vector3(0.25, 0.0, 0.0),
		"rotation": Vector3.RIGHT,
		"subobject": 0,
	}

	func get_user_point_count() -> int:
		return 1

	func get_user_point_info(_index: int) -> Dictionary:
		return info


class FakePosedWeaponPart:
	extends Node3D
	var data := FakeUserPointData.new()
	var skeleton: Skeleton3D

	func get_object_data() -> FakeUserPointData:
		return data

	func get_skeleton() -> Skeleton3D:
		return skeleton


# A minimal LocalPlayerPresenter stand-in serving only the public accessors
# PlayerWeaponEffects resolves userpoints against.
class FakeEffectsPresenter:
	extends RefCounted
	var parts: Array = []

	func vm_parts() -> Array:
		return parts

	func viewmodel() -> Node3D:
		return null

	func held_weapon() -> Node3D:
		return null

	func camera() -> Camera3D:
		return null

	func is_third_person() -> bool:
		return false


class ActionParticleEffectsHarness:
	extends PlayerWeaponEffects
	var effects_mount := FakeEffectsPresenter.new()

	func _init() -> void:
		setup(null, effects_mount)

	func configure_viewmodel_parts(parts: Array) -> void:
		effects_mount.parts = parts

	func action_particle_world_position(userpoint: String) -> Vector3:
		return _action_particle_world_position(userpoint)

	func action_particle_world_forward(userpoint: String) -> Vector3:
		return _action_particle_world_forward(userpoint)


# Stands in for NovaTerrainData's raycast_terrain: the ported retail heightmap
# raycast the binocular rangefinder measures against. A miss reports all-NAN.
class FakeTerrainData:
	extends RefCounted
	var hit := Vector3(NAN, NAN, NAN)
	var calls: Array = []

	func raycast_terrain(from: Vector3, to: Vector3) -> Vector3:
		calls.append({"from": from, "to": to})
		return hit


# The value-only sim double behind FakeWorld.get_sim(): records the same state
# the old GameWorld-forwarder stubs recorded, under NovaSimulation's native names.
class FakeSim:
	extends RefCounted
	var has_player := true
	var player_position := Vector3.ZERO
	var player_yaw_deg := 0.0
	var player_pitch_deg := 0.0
	var player_team := 1
	var input_calls: Array = []
	var look_calls: Array = []
	var stance_requests: Array = []
	var eye_calls: Array = []
	var weapon_input_calls: Array = []
	var weapon_category_requests: Array[int] = []
	var weapon_cycle_requests: Array[int] = []
	var scope_toggle_requests := 0
	var binocular_toggle_requests := 0
	var nvg_toggle_requests := 0
	var nvg_gain_requests: Array[int] = []
	var camera_mode_calls: Array = []
	var interior_item_id := 0
	var anim_key := ""
	var anim_phase_ticks := 0
	var anim_source_key := ""
	var anim_source_phase_ticks := 0
	var anim_blend_weight := 1.0

	func has_local_player() -> bool:
		return has_player

	func get_local_player_position() -> Vector3:
		return player_position

	func get_local_player_yaw_deg() -> float:
		return player_yaw_deg

	func get_local_player_pitch_deg() -> float:
		return player_pitch_deg

	func get_local_player_team() -> int:
		return player_team

	func get_local_player_anim_key() -> String:
		return anim_key

	func get_local_player_anim_phase_ticks() -> int:
		return anim_phase_ticks

	func get_local_player_anim_source_key() -> String:
		return anim_source_key

	func get_local_player_anim_source_phase_ticks() -> int:
		return anim_source_phase_ticks

	func get_local_player_anim_blend_weight() -> float:
		return anim_blend_weight

	func get_local_player_body_anim_slot() -> int:
		return -1

	func set_player_input(forward: bool, back: bool, left: bool, right: bool,
			lean_left: bool, lean_right: bool, jump: bool) -> void:
		input_calls.append({
			"forward": forward,
			"back": back,
			"left": left,
			"right": right,
			"lean_left": lean_left,
			"lean_right": lean_right,
			"jump": jump,
		})

	func add_local_player_look(dx_px: float, dy_px: float) -> void:
		look_calls.append(Vector2(dx_px, dy_px))

	func request_local_player_stance(stance: int) -> bool:
		stance_requests.append(stance)
		return true

	func set_local_player_eye(eye: Vector3, valid: bool) -> void:
		eye_calls.append([eye, valid])

	func set_local_player_weapon_input(fire_held: bool, fire_pressed: bool,
			reload_pressed: bool) -> void:
		weapon_input_calls.append([fire_held, fire_pressed, reload_pressed])

	func request_local_player_weapon_category(category: int) -> void:
		weapon_category_requests.append(category)

	func request_local_player_weapon_cycle(direction: int) -> void:
		weapon_cycle_requests.append(direction)

	func request_local_player_scope_toggle() -> bool:
		scope_toggle_requests += 1
		return true

	func request_local_player_binoculars_toggle() -> bool:
		binocular_toggle_requests += 1
		return true

	func request_local_player_nvg_toggle() -> bool:
		nvg_toggle_requests += 1
		return true

	func request_local_player_nvg_gain(delta: int) -> int:
		nvg_gain_requests.append(delta)
		return delta

	func set_local_player_camera_third_person(third_person: bool) -> void:
		camera_mode_calls.append(third_person)

	func local_player_interior_item_id() -> int:
		return interior_item_id


class FakeAvatar:
	extends FakeWeaponPart
	var body_calls: Array = []

	func play_body_clip_at(key: String, phase_ticks: int) -> void:
		body_calls.append(["at", key, phase_ticks])

	func play_body_blend_at(source_key: String, source_phase_ticks: int,
			target_key: String, target_phase_ticks: int, weight: float) -> void:
		body_calls.append([
			"blend", source_key, source_phase_ticks,
			target_key, target_phase_ticks, weight])


class FakeWorld:
	extends Node3D
	var terrain_data: FakeTerrainData = null
	var avatar_count := 0
	var viewmodel_count := 0
	var last_avatar: Node3D = null
	var last_viewmodel: Node3D = null
	var last_weapon_part: FakeWeaponPart = null
	var _loaded := true
	var sim := FakeSim.new()
	var item_db: FakeInteriorItemDb = null

	func is_loaded() -> bool:
		return _loaded

	func get_sim() -> FakeSim:
		return sim

	# No MissionRuntime in the harness: the aim-overlay seam reads null (no overlay).
	func get_runtime():
		return null

	# The real builders return NovaObjectModel subtrees whose MeshInstance3D
	# children hang under container/Robj/Skeleton3D nodes; a plain child mesh
	# models that shape (layers live on the VisualInstance3D, not the root).
	func build_local_player_avatar() -> Node3D:
		avatar_count += 1
		var node := FakeAvatar.new()
		node.add_child(MeshInstance3D.new())
		add_child(node)
		last_avatar = node
		return node

	func build_local_player_viewmodel() -> Node3D:
		viewmodel_count += 1
		var node := Node3D.new()
		node.add_child(MeshInstance3D.new())
		var part := FakeWeaponPart.new()
		node.add_child(part)
		add_child(node)
		last_viewmodel = node
		last_weapon_part = part
		return node

	func get_terrain_data() -> FakeTerrainData:
		return terrain_data

	func get_item_db():
		return item_db

	# The equipped-weapon FSM seam (null = no weapon installed, the default).
	var weapon_view = null  # PlayerWeaponView
	var weapon_events: Array[PlayerWeaponEvent] = []
	var installed_weapon_name := "WPN_M4AUTO"
	var weapon_switch_calls: Array[Dictionary] = []
	var weapon_clear_calls := 0
	# The sim-owned view state seam (ADS ease / fov policy / 3P anchor).
	var view = null  # PlayerLocalView
	var nvg_view_calls: Array = []
	# The ordered action-sound + effect-world seams the presenter drains on the event batch.
	var mission_audio = null  # FakeMissionAudio
	var effect_world = null   # FakeEffectWorld
	var weapon_tick_consumer := Callable()
	var effect_anchors: Dictionary = {}

	func register_effect_anchor(owner_key: Variant, resolver: Callable) -> void:
		effect_anchors[owner_key] = resolver

	func unregister_effect_anchor(owner_key: Variant) -> void:
		effect_anchors.erase(owner_key)

	func get_mission_audio():
		return mission_audio

	func get_effect_world():
		return effect_world

	func local_player_weapon_view():
		return weapon_view

	func local_player_weapon_name() -> String:
		return installed_weapon_name

	func set_local_player_weapon_by_name(name: String,
			preserve_slot_state: bool = false) -> bool:
		weapon_switch_calls.append({
			"name": name,
			"preserve_slot_state": preserve_slot_state,
		})
		installed_weapon_name = name
		return true

	func clear_local_player_weapon() -> void:
		weapon_clear_calls += 1
		installed_weapon_name = ""

	func drain_local_player_weapon_events() -> Array[PlayerWeaponEvent]:
		var drained: Array[PlayerWeaponEvent] = []
		for event in weapon_events:
			drained.append(event)
		weapon_events.clear()
		return drained

	func set_local_player_weapon_tick_consumer(consumer: Callable) -> void:
		weapon_tick_consumer = consumer

	func present_weapon_tick(events: Array[PlayerWeaponEvent]) -> void:
		if weapon_tick_consumer.is_valid():
			weapon_tick_consumer.call(events)

	func local_player_view():
		return view

	func set_local_player_nvg_view(active: bool, gain: int) -> void:
		nvg_view_calls.append([active, gain])

	# No weapon.def in the harness: no viewmodel def and no held-weapon model.
	func local_player_viewmodel_def() -> PlayerViewmodelDef:
		return null

	func build_local_player_held_weapon(_graphic: String) -> Node3D:
		return null


func after_each() -> void:
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


func test_shared_presenter_drives_simultaneous_raw_input_before_world_tick() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary:
		return {"forward": true, "left": true, "lean_left": true, "jump": true})

	presenter.before_world_tick(0.016)

	assert_eq(world.sim.input_calls.size(), 1)
	var call: Dictionary = world.sim.input_calls[0]
	assert_true(call["forward"])
	assert_true(call["left"])
	assert_true(call["lean_left"])
	assert_true(call["jump"])
	assert_false(call["back"])
	assert_false(call["right"])
	assert_false(call["lean_right"])
	assert_eq(world.avatar_count, 1, "3P avatar is owned by the shared presenter")
	assert_eq(world.viewmodel_count, 1, "FP viewmodel is owned by the shared presenter")


func test_viewmodel_lighting_tracks_first_blink_parent_not_indoors_flag() -> void:
	var world := FakeWorld.new()
	world.sim.interior_item_id = 101216
	world.item_db = FakeInteriorItemDb.new()
	world.item_db.transfers[101216] = 0.2
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary:
		return {})

	presenter.before_world_tick(0.016)
	presenter.after_world_tick()

	assert_not_null(world.last_weapon_part)
	assert_eq(world.last_weapon_part.lighting_contexts.back(),
			[1.0, true, 0.2],
			"the FP submit keeps effectScale 1 and uses the blink parent's transfer")
	world.sim.interior_item_id = 0
	presenter.after_world_tick()
	assert_eq(world.last_weapon_part.lighting_contexts.back(),
			[1.0, false, 0.0],
			"walking out refreshes the model lighting on the live render frame")


func test_local_avatar_consumes_authoritative_primary_blend_tuple() -> void:
	var world := FakeWorld.new()
	world.sim.anim_source_key = "anim_idle"
	world.sim.anim_source_phase_ticks = 12
	world.sim.anim_key = "anim_death_bullet_head_left"
	world.sim.anim_phase_ticks = 0
	world.sim.anim_blend_weight = 0.0
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary:
		return {})

	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	var avatar := world.last_avatar as FakeAvatar
	assert_not_null(avatar)
	assert_eq(avatar.body_calls[0].slice(0, 5), [
		"blend", "anim_idle", 12, "anim_death_bullet_head_left", 0])
	assert_almost_eq(float(avatar.body_calls[0][5]), 0.0, 0.000001,
			"the local third-person avatar keeps the outgoing death-switch pose")


func test_usegun_switch_event_rebuilds_borrowed_viewmodel_without_resetting_slot() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary:
		return {})
	world.weapon_view = _weapon_view()
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(world.viewmodel_count, 1)

	var event := PlayerWeaponEvent.new()
	event.switch_to_weapon = "WPN_EMPLCD50"
	event.preserve_slot_state = true
	world.weapon_events.append(event)
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()

	assert_eq(world.weapon_switch_calls, [{
		"name": "WPN_EMPLCD50",
		"preserve_slot_state": true,
	}], "the mount commit installs the parent weapon definition through the FP seam")
	# refresh_viewmodel invalidates immediately; the normal next presenter frame owns
	# the asynchronous scene rebuild.
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(world.viewmodel_count, 2,
			"the stale personal viewmodel is replaced by the emplacement viewmodel")

	var clear_event := PlayerWeaponEvent.new()
	clear_event.clear_weapon = true
	world.weapon_events.append(clear_event)
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(world.weapon_clear_calls, 1,
			"an unarmed detach explicitly clears the emplaced presentation")


func test_unarmed_usegun_switch_is_consumed_without_a_weapon_view() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary:
		return {})
	world.weapon_view = null
	var event := PlayerWeaponEvent.new()
	event.switch_to_weapon = "WPN_EMPLCD50"
	event.preserve_slot_state = true
	world.weapon_events.append(event)

	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(world.weapon_switch_calls, [{
		"name": "WPN_EMPLCD50",
		"preserve_slot_state": true,
	}], "slot-control events cannot depend on an existing viewmodel")


func test_inactive_gameplay_submits_neutral_movement_while_world_keeps_ticking() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary:
		return {"forward": true, "left": true, "lean_left": true, "jump": true})

	presenter.before_world_tick(0.016, false, false)

	assert_eq(world.sim.input_calls.size(), 1, "the live overlay still submits one input frame")
	var call: Dictionary = world.sim.input_calls[0]
	for key in ["forward", "back", "left", "right", "lean_left", "lean_right", "jump"]:
		assert_false(bool(call[key]), "%s is neutral while the armory owns input" % key)


func test_mouse_motion_forwards_raw_pixels_to_the_sim_pipeline() -> void:
	# The presenter no longer scales or accumulates look: raw pixel deltas go to
	# NovaSimulation.add_local_player_look (the witnessed integer pipeline —
	# sens<<11, scoped zoom reduction, the prone 40-degree up-clamp — is SIM
	# state, covered by the ctest player_look suite).
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	presenter.setup(world, camera)

	var motion := InputEventMouseMotion.new()
	motion.relative = Vector2(17.0, -6.0)
	assert_true(presenter.handle_input(motion, true))
	assert_eq(world.sim.look_calls.size(), 1)
	assert_eq(world.sim.look_calls[0], Vector2(17.0, -6.0))
	assert_false(presenter.handle_input(motion, false), "inactive input is not forwarded")
	assert_eq(world.sim.look_calls.size(), 1)


func test_stance_keys_are_three_key_select_requests() -> void:
	# The witnessed 3-key SELECT (Z prone, X crouch, C stand — catalog ids 9/10/11):
	# each key REQUESTS its stance; the sim owns mutual exclusion + the ForceCrouch
	# refusal. [orig: input cases 170/169/172 -> C2S 0x1D @0x501c60]
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	presenter.setup(world, camera)

	for keycode in [KEY_Z, KEY_X, KEY_C]:
		var key := InputEventKey.new()
		key.keycode = keycode
		key.pressed = true
		assert_true(presenter.handle_key_input(key, true))
	assert_eq(world.sim.stance_requests, [2, 1, 0])


func test_binoculars_nvg_and_gain_keys_route_retail_actions() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	presenter.setup(world, camera)

	for keycode in [KEY_B, KEY_N, KEY_EQUAL, KEY_MINUS]:
		var key := InputEventKey.new()
		key.physical_keycode = keycode
		key.pressed = true
		assert_true(presenter.handle_key_input(key, true))
	assert_eq(world.sim.binocular_toggle_requests, 1)
	assert_eq(world.sim.nvg_toggle_requests, 1)
	assert_eq(world.sim.nvg_gain_requests, [1, -1])


func test_first_person_routes_the_body_to_the_water_mirror_by_layer() -> void:
	# The water-reflection player-model pin: retail's mirror re-renders the
	# world scene, which CONTAINS the local player's body; the first-person
	# arms/weapon are a separate near-Z overlay that never enters it
	# [orig: Water_ReflectionPrerender @ 0x5c2780 -> render_main_scene
	# @ 0x5c1240; Player_RenderFirstPersonViewModel @ 0x4ded60]. World-driven, that
	# is LAYER plumbing: in first person the body stays visible on the
	# reflection-only layer (masked off the player camera, kept by NovaWater's
	# mirror camera) and the viewmodel rides its own mirror-excluded layer.
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary:
		return {})
	await get_tree().process_frame  # setup() mounts the FP pass deferred

	presenter.before_world_tick(0.016)  # builds the avatar + viewmodel
	presenter.after_world_tick()        # first person by default: placement + layer stamps

	assert_eq(camera.cull_mask & NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY, 0,
		"setup() masks the reflection-only body layer off the player camera")
	# The FP viewmodel renders through the dedicated renderfov pass, never the player
	# camera [orig: Player_RenderFirstPersonViewModel @0x4ded60 — own projection + flush].
	assert_eq(camera.cull_mask & NovaWater.VISUAL_LAYER_VIEWMODEL, 0,
		"setup() masks the viewmodel layer off the player camera (the FP pass draws it)")
	assert_eq(camera.cull_mask & NovaWater.VISUAL_LAYER_SHADOW_CASTER_MASK, 0,
		"caster marker layers cannot make the reflection-only body visible to the player")
	var pass_cam: Camera3D = presenter.viewmodel_rig().get("_vm_camera")
	assert_not_null(pass_cam, "setup() builds the FP render pass camera")
	if pass_cam != null:
		assert_eq(pass_cam.cull_mask, NovaWater.VISUAL_LAYER_VIEWMODEL,
			"the pass camera draws ONLY the viewmodel layer")
		assert_almost_eq(pass_cam.near, 0.05, 0.0001,
			"the pass near plane is the witnessed 0.05 swap [orig: @0x4dee29]")
	assert_true(world.last_avatar.visible,
		"the body stays VISIBLE in first person - the mirror renders it")
	assert_true(world.last_viewmodel.visible, "the FP overlay shows in first person")
	var body_instances := _visual_instances(world.last_avatar)
	var vm_instances := _visual_instances(world.last_viewmodel)
	assert_gt(body_instances.size(), 0, "the fake body carries a visual instance")
	assert_gt(vm_instances.size(), 0, "the fake viewmodel carries a visual instance")
	for vi in body_instances:
		assert_eq(vi.layers, NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY,
			"first person: the body's visual instances ride the reflection-only layer")
	for vi in vm_instances:
		assert_eq(vi.layers, NovaWater.VISUAL_LAYER_VIEWMODEL,
			"the viewmodel's visual instances ride the mirror-excluded viewmodel layer")

	presenter.set_third_person(true)
	presenter.after_world_tick()

	for vi in _visual_instances(world.last_avatar):
		assert_eq(vi.layers, NovaWater.VISUAL_LAYER_WORLD,
			"third person: the body returns to the normal world layer")
	assert_true(world.last_avatar.visible, "the body shows in third person")
	assert_false(world.last_viewmodel.visible, "the FP overlay hides entirely in third person")

	presenter.teardown()
	assert_ne(camera.cull_mask & NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY, 0,
		"teardown() restores the player camera's cull mask")
	assert_ne(camera.cull_mask & NovaWater.VISUAL_LAYER_SHADOW_CASTER_MASK, 0,
		"teardown() restores the camera's original marker-layer bits exactly")


func test_camera_state_rides_the_sim_view() -> void:
	# ADR 0016: the ADS ease, the fov policy, and the 3P anchor are SIM state at
	# the world cadence — the presenter reads the PlayerLocalView snapshot and places
	# nodes. Scoped fov: the sim's horizontal policy value through the ONE shared
	# h->v conversion. 3P: the camera aims at the sim's chased anchor.
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary:
		return {})

	world.view = PlayerLocalView.new()
	world.view.fov_h_deg = 20.0  # the sim's sighted 80/4 policy value
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	var size := camera.get_viewport().get_visible_rect().size
	assert_almost_eq(camera.fov,
		NovaSimulation.fov_vertical_from_horizontal(20.0, size.x / size.y), 0.001,
		"the camera fov is the sim's policy value through the shared conversion")

	# The camera consumes retail's already-doubled recoil pitch only in first
	# person. It must not mutate the sim aim or leak into the 3P orbit.
	# [orig: Player_UpdateFirstPersonCamera @0x437fdb]
	world.view.fp_pitch_recoil_deg = 8.0
	presenter.after_world_tick()
	var fp_forward := -camera.global_basis.z
	assert_almost_eq(rad_to_deg(asin(fp_forward.y)), 8.0, 0.001,
			"first-person camera adds the 2*recoil pitch term")

	# Third person: the camera backs off the NUDGED pivot — the sim's chased
	# anchor + R*(0.125 fwd/left/up) — by the round-start reset distance 1.0
	# with orbit yaw/pitch 0 [orig: Camera_ResetToLocalPlayer @0x4a3d30; the
	# nudge @0x43818a]. Level look at yaw 0: fwd = (0,0,-1), left = (-1,0,0),
	# up = (0,1,0).
	world.view.tp_anchor = Vector3(4.0, 2.0, -6.0)
	world.view.tp_anchor_valid = true
	presenter.set_third_person(true)
	presenter.after_world_tick()
	var tp_forward_actual := -camera.global_basis.z
	assert_almost_eq(rad_to_deg(asin(tp_forward_actual.y)), 0.0, 0.001,
			"third-person orbit excludes the first-person recoil doubling")
	var pivot: Vector3 = world.view.tp_anchor \
			+ (Vector3(0, 0, -1) + Vector3(-1, 0, 0) + Vector3(0, 1, 0)) \
			* presenter.PLAYER_TP_PIVOT_NUDGE
	var to_pivot: Vector3 = pivot - camera.global_position
	assert_almost_eq(presenter.PLAYER_TP_DISTANCE, 1.0, 0.001,
		"the in-play chase distance is the reset 1.0 [orig: @0x4a3d4c]")
	# The march's no-collision landing: (floor(1.0/0.25) - 1) * 0.25 = 0.75 back
	# [orig: @0x438213..0x43832e — the eye stays on the LAST 0.25u step].
	assert_almost_eq(to_pivot.length(), 0.75, 0.001,
		"the camera lands on the march's last 0.25u step, not the full distance")
	var expected_eye: Vector3 = pivot - Vector3(0, 0, -1) \
			* LocalPlayerPresenter.tp_effective_distance(presenter.PLAYER_TP_DISTANCE)
	assert_almost_eq((camera.global_position - expected_eye).length(), 0.0, 0.001,
		"the eye is pivot - effective_dist*forward (orbit pitch 0 at the reset)")


func test_camera_mode_and_scope_toggle_reach_the_sim() -> void:
	# The sim owns g_camera_mode's consequences and the ADS gates: F4 pushes the
	# mode; the presenter never carries scope state of its own.
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	presenter.setup(world, camera)  # _reset_state syncs the initial mode
	world.sim.camera_mode_calls.clear()

	var f4 := InputEventKey.new()
	f4.keycode = KEY_F4
	f4.pressed = true
	assert_true(presenter.handle_key_input(f4, true))
	assert_eq(world.sim.camera_mode_calls, [true], "F4 pushes third person into the sim")
	assert_true(presenter.handle_key_input(f4, true))
	assert_eq(world.sim.camera_mode_calls, [true, false], "and back")


# The game shell calls setup() from its own _ready — while the player camera's
# viewport is still making its children ready, so it rejects add_child ("parent
# busy": _propagate_ready blocks the parent for the whole walk). A direct FP-pass
# mount fails then, leaving the viewmodel layer masked off the player camera with
# nothing drawing it: an invisible FP viewmodel in the runtime. The pass mount
# is deferred for exactly this boot shape; this pins it.
class BootTrigger:
	extends Node
	var presenter
	var world: Node3D
	var camera: Camera3D

	func _ready() -> void:
		presenter.setup(world, camera)


func test_setup_during_scene_ready_still_mounts_the_fp_pass() -> void:
	var vp := SubViewport.new()  # the play viewport the pass composites into
	var trigger := BootTrigger.new()
	trigger.world = FakeWorld.new()
	trigger.camera = Camera3D.new()
	trigger.presenter = LocalPlayerPresenter.new()
	vp.add_child(trigger.world)
	vp.add_child(trigger.camera)
	vp.add_child(trigger.presenter)
	vp.add_child(trigger)  # last: world/camera/presenter are in-tree when _ready fires
	# Entering the tree makes vp's children ready — vp is "busy" exactly while
	# BootTrigger's _ready runs setup(), the game shell's boot shape.
	add_child_autofree(vp)
	await get_tree().process_frame

	var pass_layer: CanvasLayer = trigger.presenter.viewmodel_rig().get("_vm_pass_layer")
	assert_not_null(pass_layer, "the FP pass survives a setup() issued during scene _ready")
	if pass_layer != null:
		assert_true(pass_layer.is_inside_tree(),
			"the FP pass mounted despite the busy boot (a failed add_child leaves it orphaned)")
		assert_eq(pass_layer.get_parent(), vp,
			"the pass composites into the camera's viewport, not this presenter's ancestor")


# Every VisualInstance3D under `root`, inclusive (mirrors the presenter's stamping walk).
func _visual_instances(root: Node) -> Array:
	var out: Array = []
	if root is VisualInstance3D:
		out.append(root)
	for child in root.get_children():
		out.append_array(_visual_instances(child))
	return out


func test_shared_presenter_teardown_releases_captured_mouse() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	presenter.setup(world, camera)

	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	presenter.teardown()

	assert_eq(Input.get_mouse_mode(), Input.MOUSE_MODE_VISIBLE)


class FakeMissionAudio:
	extends Node
	var oneshots: Array = []

	func fire_soundset(set_name: String, world_pos: Vector3, source_bms_id: int = 0) -> bool:
		oneshots.append({"set": set_name, "pos": world_pos, "source_bms_id": source_bms_id})
		return true


func _weapon_view() -> PlayerWeaponView:
	var v := PlayerWeaponView.new()
	v.active = true
	return v


func test_viewmodel_tracks_and_clears_emplaced_weapon_controls() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	world.view = PlayerLocalView.new()
	world.weapon_view = _weapon_view()
	world.weapon_view.emplaced_controls_valid = true
	world.weapon_view.emplaced_gun_yaw = 0x2345
	world.weapon_view.emplaced_gun_pitch = 0xDCBA
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary: return {})
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()

	assert_not_null(world.last_weapon_part)
	assert_eq(world.last_weapon_part.ctrl_values, {
		"TEX_TEAM": 1,
		"HEAT_GLOW": 0,
		"EWEAP_GUNYAW": 0x2345,
		"EWEAP_GUNPITCH": 0xDCBA,
	}, "the FP weapon receives cold heat plus the emplaced semantic pair")

	world.weapon_view.emplaced_controls_valid = false
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(world.last_weapon_part.ctrl_values, {
		"TEX_TEAM": 1,
		"HEAT_GLOW": 0,
	},
			"leaving UseGun clears only the turret pair; FP heat still owns cold zero")


func test_viewmodel_tex_team_is_signed_and_only_written_on_visible_submit() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	world.sim.player_team = 0xFE
	world.view = PlayerLocalView.new()
	world.weapon_view = _weapon_view()
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary: return {})
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()

	assert_eq(world.last_weapon_part.ctrl_values.get("TEX_TEAM"), -2,
			"the FP store sign-extends retail's entity team byte")
	presenter.set_third_person(true)
	presenter.after_world_tick()
	assert_true(world.last_weapon_part.ctrl_values.is_empty(),
			"a third-person frame does not execute any FP CTRL writer")

	presenter.set_third_person(false)
	world.sim.player_team = 2
	presenter.after_world_tick()
	assert_eq(world.last_weapon_part.ctrl_values.get("TEX_TEAM"), 2)
	world.view.binoculars_view_active = true
	presenter.after_world_tick()
	assert_true(world.last_weapon_part.ctrl_values.is_empty(),
			"the binocular card path suppresses the FP model submit and TEX_TEAM")


func test_viewmodel_publishes_full_range_heat_glow() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	world.view = PlayerLocalView.new()
	world.weapon_view = _weapon_view()
	world.weapon_view.heat_glow = 0x10000
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary: return {})
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(int(world.last_weapon_part.ctrl_values.get("HEAT_GLOW", -1)),
			0x10000, "the FP writer keeps retail's exact 1.0 endpoint")

	world.weapon_view.heat_glow = 0
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(int(world.last_weapon_part.ctrl_values.get("HEAT_GLOW", -1)), 0,
			"the first-person writer stores literal zero on every cool submit")


func _weapon_end_event(set_name: String) -> PlayerWeaponEvent:
	var event := PlayerWeaponEvent.new()
	event.action_finished = 2
	event.action_end_soundset = set_name
	return event


func _weapon_begin_event(set_name: String) -> PlayerWeaponEvent:
	var event := PlayerWeaponEvent.new()
	event.action_started = 2
	event.action_soundset = set_name
	return event


class FakeEffectWorld:
	extends Node
	var spawns: Array = []

	func spawn_effect_request(effect: String, transform: Transform3D,
			options: Dictionary = {}) -> Dictionary:
		spawns.append({
			"kind": "request",
			"effect": effect,
			"pos": transform.origin,
			"orientation": transform.basis.z,
			"owner": options.get("slot_key"),
			"options": options.duplicate(),
		})
		return {"spawned": true, "accepted": true, "effect_handle": 1, "group_id": 1}

	func spawn_effect_transient(effect: String, pos: Vector3, orientation: Vector3,
			initial_age_ticks: int, render_domain: int,
			source_tick: int, source_order: int) -> int:
		spawns.append({
			"kind": "transient",
			"effect": effect,
			"pos": pos,
			"orientation": orientation,
			"initial_age_ticks": initial_age_ticks,
			"render_domain": render_domain,
			"source_tick": source_tick,
			"source_order": source_order,
		})
		return 1


func _weapon_particle_event(kind: int, effect: String, scope_settled := false,
		third_person := false, vehicle_attack_context := false) -> PlayerWeaponEvent:
	var event := PlayerWeaponEvent.new()
	event.action_started = kind
	event.action_particle = effect
	event.action_particle_userpoint = "muzzle1"
	event.scope_settled = scope_settled
	event.third_person = third_person
	event.vehicle_attack_context = vehicle_attack_context
	return event


func test_action_particle_userpoint_follows_the_live_weapon_bone_pose() -> void:
	var effects := ActionParticleEffectsHarness.new()
	var part := FakePosedWeaponPart.new()
	part.transform = Transform3D(Basis.from_euler(Vector3(0.0, 0.3, 0.0)), Vector3(4, 2, -3))
	add_child_autofree(part)
	var skeleton := Skeleton3D.new()
	skeleton.add_bone("muzzle")
	var rest := Transform3D(Basis.from_euler(Vector3(0.0, -0.2, 0.0)),
			Vector3(-0.4, 0.2, 0.3))
	skeleton.set_bone_rest(0, rest)
	skeleton.reset_bone_pose(0)
	skeleton.set_bone_pose_position(0, Vector3(0.0, 0.1, -1.0))
	skeleton.set_bone_pose_rotation(0, Quaternion(Vector3.UP, PI * 0.5))
	part.add_child(skeleton)
	part.skeleton = skeleton
	skeleton.force_update_all_bone_transforms()
	effects.configure_viewmodel_parts([part])

	var raw_position: Vector3 = part.data.info["position"]
	var global_rest := skeleton.get_bone_global_rest(0)
	var rest_local := global_rest.affine_inverse() * raw_position
	var rest_local_forward := global_rest.basis.inverse() * Vector3(part.data.info["rotation"])
	var posed := skeleton.get_bone_global_pose(0)
	var expected_position := skeleton.global_transform * (posed * rest_local)
	var expected_forward := (skeleton.global_transform.basis * posed.basis
			* rest_local_forward).normalized()

	assert_almost_eq(effects.action_particle_world_position("mflash01"),
			expected_position, Vector3(0.0001, 0.0001, 0.0001),
			"muzzle position follows the fake-skinned weapon bone")
	assert_almost_eq(effects.action_particle_world_forward("mflash01"),
			expected_forward, Vector3(0.0001, 0.0001, 0.0001),
			"muzzle direction follows the same live weapon bone")


func test_first_tick_muzzle_uses_the_current_viewmodel_root() -> void:
	# setup() discards only pre-attachment history: a FIRE produced on the first
	# live tick must place against that tick's camera/viewmodel, not the fresh
	# model's default transform at the scene origin.
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	var fx := FakeEffectWorld.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	add_child_autofree(fx)
	world.sim.player_position = Vector3(17.0, 2.0, -9.0)
	world.sim.player_yaw_deg = 35.0
	world.view = PlayerLocalView.new()
	world.weapon_view = _weapon_view()
	world.effect_world = fx
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary:
		return {})
	world.weapon_events.append(_weapon_particle_event(2, "Effect_FirstTickMF"))

	presenter.before_world_tick(0.016)
	presenter.after_world_tick()

	assert_eq(fx.spawns.size(), 1, "the post-setup first-tick FIRE event remains live")
	assert_almost_eq(Vector3(fx.spawns[0]["pos"]), world.last_weapon_part.global_position,
			Vector3(0.0001, 0.0001, 0.0001),
			"first-tick muzzle placement uses the current viewmodel transform")


func test_fixed_tick_weapon_callback_spawns_before_frame_finalization_once() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	var fx := FakeEffectWorld.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	add_child_autofree(fx)
	world.sim.player_position = Vector3(6.0, 1.0, -4.0)
	world.view = PlayerLocalView.new()
	world.weapon_view = _weapon_view()
	world.effect_world = fx
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary: return {})
	presenter.before_world_tick(0.016)

	var event := PlayerWeaponEvent.new()
	event.action_effect = 3
	event.effect_particle = "Effect_SameTick"
	event.effect_particle_userpoint = "mflash01"
	var events: Array[PlayerWeaponEvent] = [event]
	world.present_weapon_tick(events)

	assert_eq(fx.spawns.size(), 1,
			"the production tick presents its direct effect before EffectWorld advances")
	assert_eq(int(fx.spawns[0].initial_age_ticks), 0,
			"a current-tick action is not artificially pre-aged")
	presenter.after_world_tick()
	assert_eq(fx.spawns.size(), 1,
			"render-frame finalization drains no duplicate after the fixed-tick callback")
	presenter.teardown()
	assert_false(world.weapon_tick_consumer.is_valid(),
			"teardown releases the world-to-presenter callback")


func test_fixed_tick_viewmodel_clip_keeps_its_catchup_position() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	world.view = PlayerLocalView.new()
	world.weapon_view = _weapon_view()
	world.weapon_view.play_serial = 1
	world.weapon_view.anim_key = "anim_wpn_fire"
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary: return {})
	presenter.before_world_tick(0.048)

	var early := PlayerWeaponEvent.new()
	early.anim_key = "anim_wpn_fire"
	var early_batch: Array[PlayerWeaponEvent] = [early]
	world.weapon_view.anim_age_ticks = 0
	world.present_weapon_tick(early_batch)
	var empty_batch: Array[PlayerWeaponEvent] = []
	world.weapon_view.anim_age_ticks = 1
	world.present_weapon_tick(empty_batch)
	world.weapon_view.anim_age_ticks = 2
	world.present_weapon_tick(empty_batch)

	assert_not_null(world.last_weapon_part)
	assert_almost_eq(world.last_weapon_part.times.back(), 2.0 * MissionRuntime.TICK_DT,
			0.00001,
			"an early catch-up event reaches the final frame at age two")

	world.weapon_view.play_serial = 2
	world.weapon_view.anim_age_ticks = 0
	var late := PlayerWeaponEvent.new()
	late.anim_key = "anim_wpn_recoil"
	var late_batch: Array[PlayerWeaponEvent] = [late]
	world.present_weapon_tick(late_batch)
	assert_almost_eq(world.last_weapon_part.times.back(), 0.0, 0.00001,
			"a late catch-up event remains at its production-tick pose")
	presenter.after_world_tick()
	assert_almost_eq(world.last_weapon_part.times.back(), 0.0, 0.00001,
			"render-frame finalization does not double-advance fixed-tick phase")


func test_action_particles_gate_on_fire_and_scope() -> void:
	# The witnessed local-player particle routing [orig: ActionSlot_ExecuteActionTick
	# @0x541a70]: only FIRE takes the with-effect shim, and scoped FP fire suppresses
	# the muzzle flash [orig: @0x541aba !g_weaponScopeActive]; non-fire local begins
	# (casing ejects on RECOIL rows) route through the no-effect shim @0x5419e0.
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	var fx := FakeEffectWorld.new()
	add_child_autofree(fx)
	world.effect_world = fx
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary:
		return {})

	# Adopt the view baseline (snapshot payloads never replay).
	world.weapon_view = _weapon_view()
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(fx.spawns.size(), 0, "snapshot adoption spawns nothing")

	# RECOIL begin with a particle (the casing row): no local spawn.
	world.weapon_events.append(_weapon_particle_event(3, "Effect_TestCas"))
	world.weapon_view = _weapon_view()
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(fx.spawns.size(), 0, "non-fire local begins spawn no particle [orig: @0x541b17]")

	# FIRE begin unscoped: the muzzle flash spawns through the slot+24-style guard.
	var fire_event := _weapon_particle_event(2, "Effect_TestMF")
	fire_event.age_ticks = 2
	world.weapon_events.append(fire_event)
	world.weapon_view = _weapon_view()
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(fx.spawns.size(), 1, "FIRE begins spawn the muzzle particle")
	assert_eq(String(fx.spawns[0]["effect"]), "Effect_TestMF")
	assert_true(fx.spawns[0].owner is String,
			"the muzzle guard is keyed by action-slot generation, not the presenter object")
	assert_almost_eq((fx.spawns[0].orientation as Vector3).length(), 1.0, 0.001,
			"the user-point/camera direction reaches the particle descriptor")
	var first_options: Dictionary = fx.spawns[0].options
	assert_eq(int(first_options.admission), NovaEffectScene.ADMISSION_SUPPRESS_WHILE_OWNED,
			"FIRE owns the retail slot+24 live handle")
	assert_eq(int(first_options.render_domain), NovaEffectScene.RENDER_DOMAIN_WORLD,
			"local FP muzzle particles join retail's global world particle pass")
	assert_eq(int(first_options.initial_age_ticks), 2,
			"delayed FIRE effects are pre-aged to their production tick")
	# The live group is owner-bound and follows the spawning action's userpoint —
	# retail re-anchors the recorded actionEffectHandle to that action's bone every
	# pump tick [orig: WeaponAction_ProcessFrame tracker @0x540edf ->
	# CEffectEmitter_UpdatePositionAndParams @0x5f6810].
	assert_eq(int(first_options.binding), NovaEffectScene.BINDING_FOLLOW_OWNER,
			"the muzzle flash follows its anchor, never world-fixed")
	assert_eq(first_options.owner_key, fx.spawns[0].owner,
			"the anchor owner is the action-slot key")
	assert_true(first_options.owner_transform is Transform3D,
			"the spawn seeds the anchor pose")
	assert_true(world.effect_anchors.has(first_options.owner_key),
			"the presenter registers the live anchor resolver on the world")
	var resolver: Callable = world.effect_anchors[first_options.owner_key]
	var live_pose: Variant = resolver.call()
	assert_true(live_pose is Transform3D, "the anchor resolves the live userpoint pose")
	assert_almost_eq((live_pose as Transform3D).origin,
			(first_options.owner_transform as Transform3D).origin, Vector3.ONE * 0.001,
			"the resolver and the spawn read the same userpoint")
	presenter.refresh_viewmodel()
	assert_true(world.effect_anchors.is_empty(),
			"a viewmodel re-mount drops the stale anchors [slot generation turnover]")

	# FIRE begin scoped MID-RAISE: retail's g_weaponScopeActive is promoted to 1 only
	# when the ADS ease completes (the settle promoter — the only site setting it),
	# so the flash still spawns during the raise.
	# [orig: the promoter @0x4de4f7 g_weaponScopeActive = (g_scopeEngaged != 0) on
	#  fp-interp completion; gate @0x541aba]
	var scoped_view := PlayerLocalView.new()
	scoped_view.scope_engaged = true
	scoped_view.scope_fraction = 0.5
	world.view = scoped_view
	world.weapon_events.append(_weapon_particle_event(2, "Effect_TestMF"))
	world.weapon_view = _weapon_view()
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(fx.spawns.size(), 2, "mid-raise scoped fire keeps the muzzle flash [orig: promoter @0x4de4f7]")

	# FIRE begin SETTLED-scoped in first person: suppressed.
	scoped_view.scope_fraction = 1.0
	world.weapon_events.append(_weapon_particle_event(2, "Effect_TestMF", true))
	world.weapon_view = _weapon_view()
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(fx.spawns.size(), 2, "settled-scoped FP fire shows no muzzle flash [orig: @0x541aba]")

	# Mounted local fire uses the vehicle-capable leg and is not hidden by ADS.
	scoped_view.mounted = true
	scoped_view.vehicle_attack_context = true
	world.weapon_events.append(_weapon_particle_event(2, "Effect_TestMF", true, false, true))
	world.weapon_view = _weapon_view()
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(fx.spawns.size(), 3, "mounted scoped fire keeps the muzzle flash")
	assert_eq(int((fx.spawns[2].options as Dictionary).render_domain),
			NovaEffectScene.RENDER_DOMAIN_WORLD,
			"mounted weapon particles stay in the world pass")
	scoped_view.mounted = false
	scoped_view.vehicle_attack_context = false

	# The same scoped fire in THIRD person spawns (the 3P leg [orig: @0x541a70]).
	presenter.set_third_person(true)
	world.weapon_events.append(_weapon_particle_event(2, "Effect_TestMF", true, true))
	world.weapon_view = _weapon_view()
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(fx.spawns.size(), 4, "3P scoped fire keeps the muzzle flash")
	assert_eq(int((fx.spawns[3].options as Dictionary).render_domain),
			NovaEffectScene.RENDER_DOMAIN_WORLD,
			"third-person weapon particles stay in the world pass")

	var first_owner: String = fx.spawns[0].owner
	presenter.refresh_viewmodel()
	world.view = PlayerLocalView.new()
	presenter.set_third_person(false)
	world.weapon_events.append(_weapon_particle_event(2, "Effect_TestMF"))
	world.weapon_view = _weapon_view()
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_ne(String(fx.spawns[4].owner), first_owner,
			"a weapon re-mount gets a fresh action-slot effect handle")


func test_catch_up_muzzle_events_keep_each_ticks_scope_gate() -> void:
	# A render frame can drain several fixed ticks. When those ticks cross the
	# retail settle promoter, the earlier muzzle remains visible and the later
	# one is suppressed; the final settled view must not suppress both.
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	var fx := FakeEffectWorld.new()
	add_child_autofree(fx)
	world.effect_world = fx
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary:
		return {})
	var final_view := PlayerLocalView.new()
	final_view.scope_engaged = true
	final_view.scope_fraction = 1.0
	world.view = final_view
	world.weapon_view = _weapon_view()
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()

	world.weapon_events.append(_weapon_particle_event(2, "Effect_Earlier", false))
	world.weapon_events.append(_weapon_particle_event(2, "Effect_Settled", true))
	world.weapon_view = _weapon_view()
	presenter.before_world_tick(0.032)
	presenter.after_world_tick()

	assert_eq(fx.spawns.size(), 1, "only the pre-settle tick spawns a muzzle")
	assert_eq(String(fx.spawns[0].effect), "Effect_Earlier")


func test_recoil_direct_casing_spawns_every_authored_transient() -> void:
	# Direct rows are generic Always transients even while settled in ADS. Their
	# user-point name does not classify the effect, and each fixed-tick event
	# keeps its own catch-up age. Both FP-authored and third-person effects enter
	# retail's one global EffectWorld render pass.
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	var fx := FakeEffectWorld.new()
	add_child_autofree(fx)
	world.effect_world = fx
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary:
		return {})
	var settled := PlayerLocalView.new()
	settled.scope_engaged = true
	settled.scope_fraction = 1.0
	world.view = settled
	world.weapon_view = _weapon_view()
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	for shot in range(2):
		var event := PlayerWeaponEvent.new()
		event.action_effect = 3
		event.effect_particle = "Effect_TestCas"
		event.effect_particle_userpoint = "bcasing"
		event.age_ticks = shot + 1
		event.third_person = shot == 1
		world.weapon_events.append(event)
		world.weapon_view = _weapon_view()
		presenter.before_world_tick(0.016)
		presenter.after_world_tick()
	assert_eq(fx.spawns.size(), 2,
			"every authored casing event reaches the batched transient path")
	assert_eq([fx.spawns[0].initial_age_ticks, fx.spawns[1].initial_age_ticks], [1, 2])
	assert_eq(int(fx.spawns[0].render_domain), NovaEffectScene.RENDER_DOMAIN_WORLD)
	assert_eq(int(fx.spawns[1].render_domain), NovaEffectScene.RENDER_DOMAIN_WORLD)


func test_revx02_recoil_authored_muzzle_effect_is_presented() -> void:
	# REVX02 WPN_M4AUTO swaps the usual authored roles: FIRE ejects its casing at
	# BCASING, while RECOIL carries EFFECT_M16MF at MFLASH01. Deferring every
	# recoil particle therefore removed the weapon's muzzle flash entirely.
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	var fx := FakeEffectWorld.new()
	add_child_autofree(fx)
	world.effect_world = fx
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary:
		return {})
	var settled := PlayerLocalView.new()
	settled.scope_engaged = true
	settled.scope_fraction = 1.0
	world.view = settled
	world.weapon_view = _weapon_view()
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()

	var event := PlayerWeaponEvent.new()
	event.action_effect = 3
	event.effect_particle = "EFFECT_M16MF"
	event.effect_particle_userpoint = "MFLASH01"
	world.weapon_events.append(event)
	world.weapon_events.append(event)
	world.weapon_view = _weapon_view()
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()

	assert_eq(fx.spawns.size(), 2,
			"repeated direct effects are Always transients, not live-slot suppressed")
	assert_eq(String(fx.spawns[0].effect), "EFFECT_M16MF")
	assert_eq(String(fx.spawns[1].effect), "EFFECT_M16MF")
	assert_false(fx.spawns[0].has("owner"),
			"direct rows never acquire the FIRE action slot's live handle")


func test_catch_up_weapon_action_events_are_not_coalesced() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	var audio := FakeMissionAudio.new()
	add_child_autofree(audio)
	world.mission_audio = audio
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary:
		return {})

	var baseline := _weapon_view()
	baseline.action_end_serial = 7
	world.weapon_view = baseline
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(audio.oneshots.size(), 0, "snapshot diagnostics never replay sounds")

	world.weapon_events.append(_weapon_end_event("GS_FIRST"))
	world.weapon_events.append(_weapon_end_event("GS_SECOND"))
	var catch_up := _weapon_view()
	catch_up.action_end_serial = 9
	catch_up.action_end_soundset = "GS_SECOND"
	world.weapon_view = catch_up
	presenter.before_world_tick(0.032)
	presenter.after_world_tick()

	assert_eq(audio.oneshots.size(), 2, "two fixed ticks drain two ordered END legs")
	if audio.oneshots.size() == 2:
		assert_eq(String(audio.oneshots[0]["set"]), "GS_FIRST")
		assert_eq(String(audio.oneshots[1]["set"]), "GS_SECOND")


func test_action_sound_legs_drain_to_mission_audio() -> void:
	# The two ACTION sound legs [orig: ActionSlot_PlaySound @0x4010c0 at begin;
	# ActionSlot_FinishActivePhase @0x53f7b0 -> the end shim @0x401100]: the begin
	# and END payloads drain in their tick order. Snapshot-only payloads are never
	# replayed, so a viewmodel rebuild cannot refire historical sounds.
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	var audio := FakeMissionAudio.new()
	add_child_autofree(audio)
	world.mission_audio = audio
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary:
		return {})

	# Snapshot diagnostics may arrive non-zero; without a queued event nothing plays.
	var v := _weapon_view()
	v.action_serial = 4
	v.action_end_serial = 7
	v.action_soundset = "GF_RL_TEST"
	v.action_end_soundset = "GS_TEST"
	world.weapon_view = v
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(audio.oneshots.size(), 0, "snapshot diagnostics do not replay sounds")

	# End leg: the finished action's soundsetend plays at the player.
	world.weapon_events.append(_weapon_end_event("GS_TEST"))
	v.action_end_serial = 8
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(audio.oneshots.size(), 1, "the drained end leg plays one set")
	if audio.oneshots.size() == 1:
		assert_eq(String(audio.oneshots[0]["set"]), "GS_TEST",
			"the END leg plays the finished action's soundsetend [orig: ActionDef+12]")
		assert_eq(int(audio.oneshots[0]["source_bms_id"]), -1,
			"local action sounds retain the local-player source identity")

	# Begin leg: its soundset plays too.
	world.weapon_events.append(_weapon_begin_event("GF_RL_TEST"))
	v.action_serial = 5
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()
	assert_eq(audio.oneshots.size(), 2, "the drained begin leg plays one set")
	if audio.oneshots.size() == 2:
		assert_eq(String(audio.oneshots[1]["set"]), "GF_RL_TEST",
			"the begin leg plays the started action's soundset [orig: ActionDef+8]")
		assert_eq(int(audio.oneshots[1]["source_bms_id"]), -1)


func test_weapon_event_attachment_discards_backlog_but_keeps_first_live_batch() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	var audio := FakeMissionAudio.new()
	add_child_autofree(audio)
	world.mission_audio = audio
	world.weapon_events.append(_weapon_end_event("GS_STALE"))
	presenter.setup(world, camera)

	world.weapon_view = _weapon_view()
	var live := PlayerWeaponEvent.new()
	live.action_started = 2
	live.action_soundset = "GF_LIVE"
	live.action_finished = 2
	live.action_end_soundset = "GS_LIVE"
	live.world_position = Vector3(4.0, 5.0, 6.0)
	world.weapon_events.append(live)
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()

	assert_eq(audio.oneshots.size(), 2, "setup discards only pre-attachment history")
	if audio.oneshots.size() == 2:
		assert_eq(String(audio.oneshots[0]["set"]), "GF_LIVE")
		assert_eq(String(audio.oneshots[1]["set"]), "GS_LIVE")
		assert_eq(audio.oneshots[0]["pos"], Vector3(4.0, 5.0, 6.0),
			"catch-up audio keeps its production-tick origin")
		assert_eq(audio.oneshots[1]["pos"], Vector3(4.0, 5.0, 6.0))


func test_catch_up_clip_resumes_at_its_tick_age() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	presenter.setup(world, camera)

	var view := _weapon_view()
	view.anim_key = "anim_wpn_fire"
	view.anim_variant = 2
	view.play_serial = 1
	world.weapon_view = view
	var event := PlayerWeaponEvent.new()
	event.age_ticks = 2
	event.anim_key = "anim_wpn_fire"
	event.anim_variant = 2
	world.weapon_events.append(event)
	presenter.before_world_tick(0.016)
	presenter.after_world_tick()

	assert_not_null(world.last_weapon_part)
	if world.last_weapon_part != null:
		assert_eq(world.last_weapon_part.plays.size(), 1)
		if world.last_weapon_part.plays.size() == 1:
			assert_eq(String(world.last_weapon_part.plays[0]["key"]), "anim_wpn_fire")
			assert_eq(int(world.last_weapon_part.plays[0]["variant"]), 2)
		assert_eq(world.last_weapon_part.times.size(), 1)
		if world.last_weapon_part.times.size() == 1:
			assert_almost_eq(world.last_weapon_part.times[0], 0.032, 0.00001)


# --- Binocular rangefinder ----------------------------------------------------
# The range readout traces the ported retail terrain raycast, not a Godot physics
# query. The physics query could only ever have hit the terrain heightfield in the
# runtime (object pick bodies are editor-only), so this measures the same surface
# through the same sampler the fired round uses.

func test_aim_range_measures_to_the_terrain_raycast_hit() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	world.sim.player_position = Vector3.ZERO
	world.sim.player_yaw_deg = 0.0
	world.sim.player_pitch_deg = 0.0
	world.terrain_data = FakeTerrainData.new()
	# Yaw 0 / pitch 0 looks down -Z; the eye sits PLAYER_EYE_HEIGHT above the feet,
	# so a hit 250 u ahead at eye height is 250 u from the player position too.
	var eye_h: float = presenter.PLAYER_EYE_HEIGHT
	world.terrain_data.hit = Vector3(0.0, eye_h, -250.0)
	presenter.setup(world, camera)

	assert_eq(presenter.aim_range_units(), 250)
	assert_eq(world.terrain_data.calls.size(), 1, "one terrain trace per query")
	var call: Dictionary = world.terrain_data.calls[0]
	assert_almost_eq(float(call["from"].y), eye_h, 0.001, "traced from the eye")
	assert_almost_eq(float(call["to"].z), -presenter.AIM_PROJECT_RANGE, 0.001,
			"traced out to the retail 1000-unit projection")


func test_aim_range_falls_back_to_the_far_endpoint_when_the_terrain_misses() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	world.sim.player_position = Vector3.ZERO
	world.terrain_data = FakeTerrainData.new()  # all-NAN = miss
	presenter.setup(world, camera)

	# Retail clamps the four-digit readout to 1..1000, and the projection is 1000.
	assert_eq(presenter.aim_range_units(), 1000)


func test_aim_range_survives_a_world_with_no_terrain() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	world.sim.player_position = Vector3.ZERO
	world.terrain_data = null
	presenter.setup(world, camera)

	assert_eq(presenter.aim_range_units(), 1000)
