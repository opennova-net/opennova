extends GutTest

const LocalPlayerHost := preload("res://engine/world/local_player_host.gd")


class FakeWorld:
	extends Node3D
	var input_calls: Array = []
	var avatar_count := 0
	var viewmodel_count := 0
	var last_avatar: Node3D = null
	var last_viewmodel: Node3D = null
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
		add_child(node)
		last_viewmodel = node
		return node

	func set_local_player_input(forward: bool, back: bool, left: bool, right: bool, run: bool,
			crouch: bool, prone: bool, jump: bool, look_yaw_deg: float, look_pitch_deg: float) -> void:
		input_calls.append({
			"forward": forward,
			"back": back,
			"left": left,
			"right": right,
			"run": run,
			"crouch": crouch,
			"prone": prone,
			"jump": jump,
			"yaw": look_yaw_deg,
			"pitch": look_pitch_deg,
		})

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
	# The sim-owned view state seam (ADS ease / fov policy / 3P anchor).
	var view = null  # PlayerLocalView
	var scope_toggle_requests := 0
	var camera_mode_calls: Array = []
	# The action sound seam the host drains on the serial edges (the effect-world
	# seam rides the particles slice).
	var mission_audio = null  # FakeMissionAudio

	func get_mission_audio():
		return mission_audio

	func local_player_weapon_view():
		return weapon_view

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
		return {"forward": true, "left": true, "run": true, "jump": true})

	host.before_world_tick(0.016)

	assert_eq(world.input_calls.size(), 1)
	var call: Dictionary = world.input_calls[0]
	assert_true(call["forward"])
	assert_true(call["left"])
	assert_true(call["run"])
	assert_true(call["jump"])
	assert_false(call["back"])
	assert_false(call["right"])
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
		return {"forward": true, "left": true, "run": true, "jump": true})

	host.before_world_tick(0.016, false, false)

	assert_eq(world.input_calls.size(), 1, "the live overlay still submits one input frame")
	var call: Dictionary = world.input_calls[0]
	for key in ["forward", "back", "left", "right", "run", "jump"]:
		assert_false(bool(call[key]), "%s is neutral while the armory owns input" % key)


func test_shared_host_mouse_yaw_wraps_and_pitch_clamps() -> void:
	var world := FakeWorld.new()
	var camera := Camera3D.new()
	var host := LocalPlayerHost.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(host)
	host.setup(world, camera)
	host.set_input_source(func() -> Dictionary:
		return {})

	var motion := InputEventMouseMotion.new()
	motion.relative = Vector2(4000.0, -4000.0)
	assert_true(host.handle_input(motion, true))
	host.before_world_tick(0.016)

	var call: Dictionary = world.input_calls[0]
	assert_gte(call["yaw"], 0.0)
	assert_lt(call["yaw"], 360.0)
	assert_eq(call["pitch"], 80.0)


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

	# Third person: the sim's chased anchor is the look target; the eye sits back
	# along the orbit. (level look, yaw 0 -> anchor - back*3 with 22.5 deg orbit)
	world.view.tp_anchor = Vector3(4.0, 2.0, -6.0)
	world.view.tp_anchor_valid = true
	host.set_third_person(true)
	host.after_world_tick()
	var to_anchor: Vector3 = world.view.tp_anchor - camera.global_position
	assert_almost_eq(to_anchor.length(), 3.0, 0.001,
		"the camera orbits the SIM anchor at the witnessed 3.0 distance")


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


func test_action_sound_legs_drain_to_mission_audio() -> void:
	# The two ACTION sound legs [orig: ActionSlot_PlaySound @0x4010c0 at begin;
	# ActionSlot_FinishActivePhase @0x53f7b0 -> the end shim @0x401100]: the begin
	# leg rides action_serial/action_soundset, the END leg (the per-shot gunshot,
	# GS_*) rides action_end_serial/action_end_soundset. The first snapshot adopts
	# silently — a viewmodel rebuild must not refire sounds.
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

	# Adopt tick: serials arrive non-zero on the first snapshot; nothing plays.
	var v := _weapon_view()
	v.action_serial = 4
	v.action_end_serial = 7
	v.action_soundset = "GF_RL_TEST"
	v.action_end_soundset = "GS_TEST"
	world.weapon_view = v
	host.before_world_tick(0.016)
	host.after_world_tick()
	assert_eq(audio.oneshots.size(), 0, "the first snapshot adopts serials silently")

	# End-leg edge: the finished action's soundsetend plays at the player.
	var v2 := _weapon_view()
	v2.action_serial = 4
	v2.action_end_serial = 8
	v2.action_end_soundset = "GS_TEST"
	world.weapon_view = v2
	host.before_world_tick(0.016)
	host.after_world_tick()
	assert_eq(audio.oneshots.size(), 1, "the end-leg serial edge plays one set")
	if audio.oneshots.size() == 1:
		assert_eq(String(audio.oneshots[0]["set"]), "GS_TEST",
			"the END leg plays the finished action's soundsetend [orig: ActionDef+12]")

	# Begin-leg edge alongside: soundset plays too.
	var v3 := _weapon_view()
	v3.action_serial = 5
	v3.action_soundset = "GF_RL_TEST"
	v3.action_end_serial = 8
	world.weapon_view = v3
	host.before_world_tick(0.016)
	host.after_world_tick()
	assert_eq(audio.oneshots.size(), 2, "the begin-leg serial edge plays one set")
	if audio.oneshots.size() == 2:
		assert_eq(String(audio.oneshots[1]["set"]), "GF_RL_TEST",
			"the begin leg plays the started action's soundset [orig: ActionDef+8]")

