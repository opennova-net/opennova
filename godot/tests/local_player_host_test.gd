extends GutTest

const LocalPlayerHost := preload("res://engine/world/local_player_host.gd")


class FakeWeaponPart:
	extends Node3D
	var plays: Array = []
	var times: Array[float] = []

	func play_body_clip(key: String) -> void:
		plays.append({"key": key, "variant": 0})

	func play_body_clip_variant(key: String, variant: int) -> void:
		plays.append({"key": key, "variant": variant})

	func set_animation_time(seconds: float) -> void:
		times.append(seconds)


class FakeWorld:
	extends Node3D
	var input_calls: Array = []
	var avatar_count := 0
	var viewmodel_count := 0
	var last_avatar: Node3D = null
	var last_viewmodel: Node3D = null
	var last_weapon_part: FakeWeaponPart = null
	var _has_player := true
	var _loaded := true

	func is_loaded() -> bool:
		return _loaded

	func has_local_player() -> bool:
		return _has_player

	# The real builders return NovaObjectModel subtrees whose MeshInstance3D
	# children hang under container/Robj/Skeleton3D nodes; a plain child mesh
	# models that shape (layers live on the VisualInstance3D, not the root).
	func build_local_player_avatar() -> Node3D:
		avatar_count += 1
		var node := Node3D.new()
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

	func set_local_player_input(forward: bool, back: bool, left: bool, right: bool,
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

	var look_calls: Array = []
	func add_local_player_look(dx_px: float, dy_px: float) -> void:
		look_calls.append(Vector2(dx_px, dy_px))

	var stance_requests: Array = []
	func request_local_player_stance(stance: int) -> bool:
		stance_requests.append(stance)
		return true

	func local_player_position() -> Vector3:
		return Vector3.ZERO

	func local_player_yaw_deg() -> float:
		return 0.0

	func local_player_pitch_deg() -> float:
		return 0.0

	func local_player_anim_key() -> String:
		return ""

	func local_player_anim_phase_ticks() -> int:
		return 0

	func local_player_body_anim_slot() -> int:
		return -1

	# The equipped-weapon FSM seam (null = no weapon installed, the default).
	var weapon_view = null  # PlayerWeaponView
	var weapon_events: Array[PlayerWeaponEvent] = []
	# The sim-owned view state seam (ADS ease / fov policy / 3P anchor).
	var view = null  # PlayerLocalView
	var scope_toggle_requests := 0
	var camera_mode_calls: Array = []
	# The ordered action-sound + effect-world seams the host drains on the event batch.
	var mission_audio = null  # FakeMissionAudio
	var effect_world = null   # FakeEffectWorld

	func get_mission_audio():
		return mission_audio

	func get_effect_world():
		return effect_world

	func local_player_weapon_view():
		return weapon_view

	func drain_local_player_weapon_events() -> Array[PlayerWeaponEvent]:
		var drained: Array[PlayerWeaponEvent] = []
		for event in weapon_events:
			drained.append(event)
		weapon_events.clear()
		return drained

	func local_player_view():
		return view

	func request_local_player_scope_toggle() -> bool:
		scope_toggle_requests += 1
		return true

	func set_local_player_camera_third_person(third_person: bool) -> void:
		camera_mode_calls.append(third_person)


func after_each() -> void:
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


func test_shared_host_drives_simultaneous_raw_input_before_world_tick() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	host.setup(world, camera)
	host.set_input_source(func() -> Dictionary:
		return {"forward": true, "left": true, "lean_left": true, "jump": true})

	host.before_world_tick(0.016)

	assert_eq(world.input_calls.size(), 1)
	var call: Dictionary = world.input_calls[0]
	assert_true(call["forward"])
	assert_true(call["left"])
	assert_true(call["lean_left"])
	assert_true(call["jump"])
	assert_false(call["back"])
	assert_false(call["right"])
	assert_false(call["lean_right"])
	assert_eq(world.avatar_count, 1, "3P avatar is owned by the shared host")
	assert_eq(world.viewmodel_count, 1, "FP viewmodel is owned by the shared host")


func test_inactive_gameplay_submits_neutral_movement_while_world_keeps_ticking() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	host.setup(world, camera)
	host.set_input_source(func() -> Dictionary:
		return {"forward": true, "left": true, "lean_left": true, "jump": true})

	host.before_world_tick(0.016, false, false)

	assert_eq(world.input_calls.size(), 1, "the live overlay still submits one input frame")
	var call: Dictionary = world.input_calls[0]
	for key in ["forward", "back", "left", "right", "lean_left", "lean_right", "jump"]:
		assert_false(bool(call[key]), "%s is neutral while the armory owns input" % key)


func test_mouse_motion_forwards_raw_pixels_to_the_sim_pipeline() -> void:
	# The host no longer scales or accumulates look: raw pixel deltas go to
	# NovaSimulation.add_local_player_look (the witnessed integer pipeline —
	# sens<<11, scoped zoom reduction, the prone 40-degree up-clamp — is SIM
	# state, covered by the ctest player_look suite).
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	host.setup(world, camera)

	var motion := InputEventMouseMotion.new()
	motion.relative = Vector2(17.0, -6.0)
	assert_true(host.handle_input(motion, true))
	assert_eq(world.look_calls.size(), 1)
	assert_eq(world.look_calls[0], Vector2(17.0, -6.0))
	assert_false(host.handle_input(motion, false), "inactive input is not forwarded")
	assert_eq(world.look_calls.size(), 1)


func test_stance_keys_are_three_key_select_requests() -> void:
	# The witnessed 3-key SELECT (Z prone, X crouch, C stand — catalog ids 9/10/11):
	# each key REQUESTS its stance; the sim owns mutual exclusion + the ForceCrouch
	# refusal. [orig: input cases 170/169/172 -> C2S 0x1D @0x501c60]
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	host.setup(world, camera)

	for keycode in [KEY_Z, KEY_X, KEY_C]:
		var key := InputEventKey.new()
		key.keycode = keycode
		key.pressed = true
		assert_true(host.handle_key_input(key, true))
	assert_eq(world.stance_requests, [2, 1, 0])


func test_first_person_routes_the_body_to_the_water_mirror_by_layer() -> void:
	# The water-reflection player-model pin: retail's mirror re-renders the
	# world scene, which CONTAINS the local player's body; the first-person
	# arms/weapon are a separate near-Z overlay that never enters it
	# [orig: Water_ReflectionPrerender @ 0x5c2780 -> render_main_scene
	# @ 0x5c1240; Player_RenderFirstPersonViewModel @ 0x4ded60]. Hosted, that
	# is LAYER plumbing: in first person the body stays visible on the
	# reflection-only layer (masked off the player camera, kept by NovaWater's
	# mirror camera) and the viewmodel rides its own mirror-excluded layer.
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	host.setup(world, camera)
	host.set_input_source(func() -> Dictionary:
		return {})
	await get_tree().process_frame  # setup() mounts the FP pass deferred

	host.before_world_tick(0.016)  # builds the avatar + viewmodel
	host.after_world_tick()        # first person by default: placement + layer stamps

	assert_eq(camera.cull_mask & NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY, 0,
		"setup() masks the reflection-only body layer off the player camera")
	# The FP viewmodel renders through the dedicated renderfov pass, never the player
	# camera [orig: Player_RenderFirstPersonViewModel @0x4ded60 — own projection + flush].
	assert_eq(camera.cull_mask & NovaWater.VISUAL_LAYER_VIEWMODEL, 0,
		"setup() masks the viewmodel layer off the player camera (the FP pass draws it)")
	var pass_cam: Camera3D = host.get("_vm_camera")
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

	host.set_third_person(true)
	host.after_world_tick()

	for vi in _visual_instances(world.last_avatar):
		assert_eq(vi.layers, NovaWater.VISUAL_LAYER_WORLD,
			"third person: the body returns to the normal world layer")
	assert_true(world.last_avatar.visible, "the body shows in third person")
	assert_false(world.last_viewmodel.visible, "the FP overlay hides entirely in third person")

	host.teardown()
	assert_ne(camera.cull_mask & NovaWater.VISUAL_LAYER_BODY_REFLECTION_ONLY, 0,
		"teardown() restores the player camera's cull mask")


func test_camera_state_rides_the_sim_view() -> void:
	# ADR 0016: the ADS ease, the fov policy, and the 3P anchor are SIM state at
	# the world cadence — the host reads the PlayerLocalView snapshot and places
	# nodes. Scoped fov: the sim's horizontal policy value through the ONE shared
	# h->v conversion. 3P: the camera aims at the sim's chased anchor.
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	host.setup(world, camera)
	host.set_input_source(func() -> Dictionary:
		return {})

	world.view = PlayerLocalView.new()
	world.view.fov_h_deg = 20.0  # the sim's sighted 80/4 policy value
	host.before_world_tick(0.016)
	host.after_world_tick()
	var size := camera.get_viewport().get_visible_rect().size
	assert_almost_eq(camera.fov,
		NovaSimulation.fov_vertical_from_horizontal(20.0, size.x / size.y), 0.001,
		"the camera fov is the sim's policy value through the shared conversion")

	# Third person: the camera backs off the NUDGED pivot — the sim's chased
	# anchor + R*(0.125 fwd/left/up) — by the round-start reset distance 1.0
	# with orbit yaw/pitch 0 [orig: Camera_ResetToLocalPlayer @0x4a3d30; the
	# nudge @0x43818a]. Level look at yaw 0: fwd = (0,0,-1), left = (-1,0,0),
	# up = (0,1,0).
	world.view.tp_anchor = Vector3(4.0, 2.0, -6.0)
	world.view.tp_anchor_valid = true
	host.set_third_person(true)
	host.after_world_tick()
	var pivot: Vector3 = world.view.tp_anchor \
			+ (Vector3(0, 0, -1) + Vector3(-1, 0, 0) + Vector3(0, 1, 0)) \
			* host.PLAYER_TP_PIVOT_NUDGE
	var to_pivot: Vector3 = pivot - camera.global_position
	assert_almost_eq(host.PLAYER_TP_DISTANCE, 1.0, 0.001,
		"the in-play chase distance is the reset 1.0 [orig: @0x4a3d4c]")
	# The march's no-collision landing: (floor(1.0/0.25) - 1) * 0.25 = 0.75 back
	# [orig: @0x438213..0x43832e — the eye stays on the LAST 0.25u step].
	assert_almost_eq(to_pivot.length(), 0.75, 0.001,
		"the camera lands on the march's last 0.25u step, not the full distance")
	var expected_eye: Vector3 = pivot - Vector3(0, 0, -1) \
			* LocalPlayerHost.tp_effective_distance(host.PLAYER_TP_DISTANCE)
	assert_almost_eq((camera.global_position - expected_eye).length(), 0.0, 0.001,
		"the eye is pivot - effective_dist*forward (orbit pitch 0 at the reset)")


func test_camera_mode_and_scope_toggle_reach_the_sim() -> void:
	# The sim owns g_camera_mode's consequences and the ADS gates: F4 pushes the
	# mode; the host never carries scope state of its own.
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	host.setup(world, camera)  # _reset_state syncs the initial mode
	world.camera_mode_calls.clear()

	var f4 := InputEventKey.new()
	f4.keycode = KEY_F4
	f4.pressed = true
	assert_true(host.handle_key_input(f4, true))
	assert_eq(world.camera_mode_calls, [true], "F4 pushes third person into the sim")
	assert_true(host.handle_key_input(f4, true))
	assert_eq(world.camera_mode_calls, [true, false], "and back")


# The game shell calls setup() from its own _ready — while the player camera's
# viewport is still making its children ready, so it rejects add_child ("parent
# busy": _propagate_ready blocks the parent for the whole walk). A direct FP-pass
# mount fails then, leaving the viewmodel layer masked off the player camera with
# nothing drawing it: an invisible FP viewmodel in the runtime (but not in ONED,
# whose play controller enters an already-running tree). The pass mount is
# deferred for exactly this boot shape; this pins it.
class BootTrigger:
	extends Node
	var host
	var world: Node3D
	var camera: Camera3D

	func _ready() -> void:
		host.setup(world, camera)


func test_setup_during_scene_ready_still_mounts_the_fp_pass() -> void:
	var vp := SubViewport.new()  # the play viewport the pass composites into
	var trigger := BootTrigger.new()
	trigger.world = FakeWorld.new()
	trigger.camera = Camera3D.new()
	trigger.host = LocalPlayerHost.new()
	vp.add_child(trigger.world)
	vp.add_child(trigger.camera)
	vp.add_child(trigger.host)
	vp.add_child(trigger)  # last: world/camera/host are in-tree when _ready fires
	# Entering the tree makes vp's children ready — vp is "busy" exactly while
	# BootTrigger's _ready runs setup(), the game shell's boot shape.
	add_child_autofree(vp)
	await get_tree().process_frame

	var pass_layer: CanvasLayer = trigger.host.get("_vm_pass_layer")
	assert_not_null(pass_layer, "the FP pass survives a setup() issued during scene _ready")
	if pass_layer != null:
		assert_true(pass_layer.is_inside_tree(),
			"the FP pass mounted despite the busy boot (a failed add_child leaves it orphaned)")
		assert_eq(pass_layer.get_parent(), vp,
			"the pass composites into the camera's viewport, not this host's ancestor")


# Every VisualInstance3D under `root`, inclusive (mirrors the host's stamping walk).
func _visual_instances(root: Node) -> Array:
	var out: Array = []
	if root is VisualInstance3D:
		out.append(root)
	for child in root.get_children():
		out.append_array(_visual_instances(child))
	return out


func test_shared_host_teardown_releases_captured_mouse() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	host.setup(world, camera)

	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	host.teardown()

	assert_eq(Input.get_mouse_mode(), Input.MOUSE_MODE_VISIBLE)


class FakeMissionAudio:
	extends Node
	var oneshots: Array = []

	func fire_soundset(set_name: String, world_pos: Vector3) -> bool:
		oneshots.append({"set": set_name, "pos": world_pos})
		return true


func _weapon_view() -> PlayerWeaponView:
	var v := PlayerWeaponView.new()
	v.active = true
	return v


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

	func spawn_effect_unless_alive(owner_key, effect: String, pos: Vector3,
			orientation: Vector3 = Vector3.ZERO) -> int:
		spawns.append({"owner": owner_key, "effect": effect, "pos": pos, "orientation": orientation})
		return 1

	func spawn_effect(effect: String, pos: Vector3, orientation: Vector3 = Vector3.ZERO) -> int:
		spawns.append({"effect": effect, "pos": pos, "orientation": orientation})
		return 1


func _weapon_particle_event(kind: int, effect: String) -> PlayerWeaponEvent:
	var event := PlayerWeaponEvent.new()
	event.action_started = kind
	event.action_particle = effect
	event.action_particle_userpoint = "muzzle1"
	return event


func test_action_particles_gate_on_fire_and_scope() -> void:
	# The witnessed local-player particle routing [orig: ActionSlot_ExecuteActionTick
	# @0x541a70]: only FIRE takes the with-effect shim, and scoped FP fire suppresses
	# the muzzle flash [orig: @0x541aba !g_weaponScopeActive]; non-fire local begins
	# (casing ejects on RECOIL rows) route through the no-effect shim @0x5419e0.
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	var fx := FakeEffectWorld.new()
	add_child_autofree(fx)
	world.effect_world = fx
	host.setup(world, camera)
	host.set_input_source(func() -> Dictionary:
		return {})

	# Adopt the view baseline (snapshot payloads never replay).
	world.weapon_view = _weapon_view()
	host.before_world_tick(0.016)
	host.after_world_tick()
	assert_eq(fx.spawns.size(), 0, "snapshot adoption spawns nothing")

	# RECOIL begin with a particle (the casing row): no local spawn.
	world.weapon_events.append(_weapon_particle_event(3, "Effect_TestCas"))
	world.weapon_view = _weapon_view()
	host.before_world_tick(0.016)
	host.after_world_tick()
	assert_eq(fx.spawns.size(), 0, "non-fire local begins spawn no particle [orig: @0x541b17]")

	# FIRE begin unscoped: the muzzle flash spawns through the slot+24-style guard.
	world.weapon_events.append(_weapon_particle_event(2, "Effect_TestMF"))
	world.weapon_view = _weapon_view()
	host.before_world_tick(0.016)
	host.after_world_tick()
	assert_eq(fx.spawns.size(), 1, "FIRE begins spawn the muzzle particle")
	assert_eq(String(fx.spawns[0]["effect"]), "Effect_TestMF")
	assert_true(fx.spawns[0].owner is String,
			"the muzzle guard is keyed by action-slot generation, not the host object")
	assert_almost_eq((fx.spawns[0].orientation as Vector3).length(), 1.0, 0.001,
			"the user-point/camera direction reaches the particle descriptor")

	# FIRE begin scoped in first person: suppressed.
	var scoped_view := PlayerLocalView.new()
	scoped_view.scope_engaged = true
	world.view = scoped_view
	world.weapon_events.append(_weapon_particle_event(2, "Effect_TestMF"))
	world.weapon_view = _weapon_view()
	host.before_world_tick(0.016)
	host.after_world_tick()
	assert_eq(fx.spawns.size(), 1, "scoped FP fire shows no muzzle flash [orig: @0x541aba]")

	# Mounted local fire uses the vehicle-capable leg and is not hidden by ADS.
	scoped_view.mounted = true
	scoped_view.vehicle_attack_context = true
	world.weapon_events.append(_weapon_particle_event(2, "Effect_TestMF"))
	world.weapon_view = _weapon_view()
	host.before_world_tick(0.016)
	host.after_world_tick()
	assert_eq(fx.spawns.size(), 2, "mounted scoped fire keeps the muzzle flash")
	scoped_view.mounted = false
	scoped_view.vehicle_attack_context = false

	# The same scoped fire in THIRD person spawns (the 3P leg [orig: @0x541a70]).
	host.set_third_person(true)
	world.weapon_events.append(_weapon_particle_event(2, "Effect_TestMF"))
	world.weapon_view = _weapon_view()
	host.before_world_tick(0.016)
	host.after_world_tick()
	assert_eq(fx.spawns.size(), 3, "3P scoped fire keeps the muzzle flash")

	var first_owner: String = fx.spawns[0].owner
	host.refresh_viewmodel()
	world.view = PlayerLocalView.new()
	host.set_third_person(false)
	world.weapon_events.append(_weapon_particle_event(2, "Effect_TestMF"))
	world.weapon_view = _weapon_view()
	host.before_world_tick(0.016)
	host.after_world_tick()
	assert_ne(String(fx.spawns[3].owner), first_owner,
			"a weapon re-mount gets a fresh action-slot effect handle")


func test_catch_up_weapon_action_events_are_not_coalesced() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	var audio := FakeMissionAudio.new()
	add_child_autofree(audio)
	world.mission_audio = audio
	host.setup(world, camera)
	host.set_input_source(func() -> Dictionary:
		return {})

	var baseline := _weapon_view()
	baseline.action_end_serial = 7
	world.weapon_view = baseline
	host.before_world_tick(0.016)
	host.after_world_tick()
	assert_eq(audio.oneshots.size(), 0, "snapshot diagnostics never replay sounds")

	world.weapon_events.append(_weapon_end_event("GS_FIRST"))
	world.weapon_events.append(_weapon_end_event("GS_SECOND"))
	var catch_up := _weapon_view()
	catch_up.action_end_serial = 9
	catch_up.action_end_soundset = "GS_SECOND"
	world.weapon_view = catch_up
	host.before_world_tick(0.032)
	host.after_world_tick()

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
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	var audio := FakeMissionAudio.new()
	add_child_autofree(audio)
	world.mission_audio = audio
	host.setup(world, camera)
	host.set_input_source(func() -> Dictionary:
		return {})

	# Snapshot diagnostics may arrive non-zero; without a queued event nothing plays.
	var v := _weapon_view()
	v.action_serial = 4
	v.action_end_serial = 7
	v.action_soundset = "GF_RL_TEST"
	v.action_end_soundset = "GS_TEST"
	world.weapon_view = v
	host.before_world_tick(0.016)
	host.after_world_tick()
	assert_eq(audio.oneshots.size(), 0, "snapshot diagnostics do not replay sounds")

	# End leg: the finished action's soundsetend plays at the player.
	world.weapon_events.append(_weapon_end_event("GS_TEST"))
	v.action_end_serial = 8
	host.before_world_tick(0.016)
	host.after_world_tick()
	assert_eq(audio.oneshots.size(), 1, "the drained end leg plays one set")
	if audio.oneshots.size() == 1:
		assert_eq(String(audio.oneshots[0]["set"]), "GS_TEST",
			"the END leg plays the finished action's soundsetend [orig: ActionDef+12]")

	# Begin leg: its soundset plays too.
	world.weapon_events.append(_weapon_begin_event("GF_RL_TEST"))
	v.action_serial = 5
	host.before_world_tick(0.016)
	host.after_world_tick()
	assert_eq(audio.oneshots.size(), 2, "the drained begin leg plays one set")
	if audio.oneshots.size() == 2:
		assert_eq(String(audio.oneshots[1]["set"]), "GF_RL_TEST",
			"the begin leg plays the started action's soundset [orig: ActionDef+8]")


func test_weapon_event_attachment_discards_backlog_but_keeps_first_live_batch() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	var audio := FakeMissionAudio.new()
	add_child_autofree(audio)
	world.mission_audio = audio
	world.weapon_events.append(_weapon_end_event("GS_STALE"))
	host.setup(world, camera)

	world.weapon_view = _weapon_view()
	var live := PlayerWeaponEvent.new()
	live.action_started = 2
	live.action_soundset = "GF_LIVE"
	live.action_finished = 2
	live.action_end_soundset = "GS_LIVE"
	live.world_position = Vector3(4.0, 5.0, 6.0)
	world.weapon_events.append(live)
	host.before_world_tick(0.016)
	host.after_world_tick()

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
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	host.setup(world, camera)

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
	host.before_world_tick(0.016)
	host.after_world_tick()

	assert_not_null(world.last_weapon_part)
	if world.last_weapon_part != null:
		assert_eq(world.last_weapon_part.plays.size(), 1)
		if world.last_weapon_part.plays.size() == 1:
			assert_eq(String(world.last_weapon_part.plays[0]["key"]), "anim_wpn_fire")
			assert_eq(int(world.last_weapon_part.plays[0]["variant"]), 2)
		assert_eq(world.last_weapon_part.times.size(), 1)
		if world.last_weapon_part.times.size() == 1:
			assert_almost_eq(world.last_weapon_part.times[0], 0.032, 0.00001)
