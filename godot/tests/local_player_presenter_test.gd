extends GutTest

# LocalPlayerPresenter over a REAL GameWorld + NovaSimulation (the ADR 0033 typed
# boundary: setup(world: GameWorld, camera: Camera3D, fly_camera: NovaFlyCamera)).
# Every test stages the minimal mission fixture, loads mnml.bms through the packaged
# world scene, and observes behavior through the sim's own getters, the
# PlayerLocalView snapshot, real NovaObjectModel observables, and the real terrain.
#
# Old fake-driven contracts that could not be honestly observed on real components
# were dropped (not faked) — the fixture root has no .ptl effect catalog, no
# retail soundsets, and no emplacement/interior geometry:
# - muzzle/casing particle routing + effect anchors (FIRE scope gates, transients,
#   catch-up ages, owner keys): the spawn side needs a .ptl catalog; the FSM event
#   production is pinned by the ctest weapon suite.
# - action begin/end sound legs: soundset resolution needs retail .LWF banks.
# - interior blink lighting transfer: needs portal-carrying buildings; the model
#   lighting context has no read-back observable (set_entity_lighting_context only).
# - emplaced EWEAP_GUNYAW/GUNPITCH ctrl pair + UseGun switch/clear events: need a
#   real emplacement mount; TEX_TEAM sign-extension (team 0xFE) needs a wire team.
# - the sim-composed camera pose VALUES (recoil doubling, roll sign vs a chosen
#   pose): the pose is composed by the engine now; this file pins the presenter's
#   1:1 stamp of the real composed pose, the ctest player_view suite pins the math.

const MissionRuntime := preload("res://adapter/world/mission_runtime.gd")

const TEST_ROOT := "local_player_presenter_test"
const TICK := MissionRuntime.TICK_DT

const MINIMAL_FIXTURE_DIR := "res://../fixtures/minimal/resources"
const DVXI5_FIXTURE_DIR := "res://../fixtures/godot/dvxi5"
const WEAPON_DEF_FIXTURE := "res://../fixtures/def/weapon.def"
const CHARMODEL_FIXTURE := "res://../fixtures/threedi/3di3/CharModel.3di"
const SOLDIER_ADM_FIXTURE := "res://../fixtures/anim/soldier.adm"
const IDLE_BAD_FIXTURE := "res://../fixtures/anim/idle.bad"
const WALK_BAD_FIXTURE := "res://../fixtures/anim/walk.bad"


var _shared_root := ""


func before_all() -> void:
	_shared_root = _stage_root()


func after_all() -> void:
	_remove_dir_recursive(OS.get_cache_dir().path_join(TEST_ROOT))


func after_each() -> void:
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


# --- real-world staging -------------------------------------------------------
# The minimal fixture plus the committed model/anim fixtures arranged as the
# names the production resolvers ask for: the full weapon.def (WPN_M4AUTO with
# its gfx/animadm/pos rows), a person items.def row for the player visual item
# (105310 -> CharModel + soldier.adm), and the 19-bone CharModel staged as the
# M4's FP gun/arms rig with a wpn-key clip set over the committed .bads.

func _stage_root() -> String:
	var root_dir := OS.get_cache_dir().path_join(TEST_ROOT).path_join(
			"root_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir), OK)
	# dvxi5 first (its own items.def loses to minimal's below), then the minimal
	# mission set: one root serves both the mnml TRN and the baked Dvxi5 CPT
	# heightfield (the mission header picks the terrain per load).
	for dir in [DVXI5_FIXTURE_DIR, MINIMAL_FIXTURE_DIR]:
		var source_dir := ProjectSettings.globalize_path(dir)
		for file_name in DirAccess.get_files_at(source_dir):
			var target := root_dir.path_join(file_name)
			if FileAccess.file_exists(target):
				assert_eq(DirAccess.remove_absolute(target), OK)
			assert_eq(DirAccess.copy_absolute(
					source_dir.path_join(file_name), target), OK)
	# The full weapon.def: WPN_M4AUTO / WPN_SATCHEL_CHARGE with real action rows.
	assert_eq(DirAccess.remove_absolute(root_dir.path_join("weapon.def")), OK)
	assert_eq(DirAccess.copy_absolute(
			ProjectSettings.globalize_path(WEAPON_DEF_FIXTURE),
			root_dir.path_join("weapon.def")), OK)
	# The player's third-person avatar: items.def person row 105310 (the placer's
	# PLAYER_VISUAL_ITEM_ID) over the committed 19-bone CharModel + soldier.adm.
	var items := FileAccess.open(root_dir.path_join("items.def"), FileAccess.READ_WRITE)
	assert_not_null(items, "staged items.def is writable")
	items.seek_end()
	items.store_string("""

begin "Player Character"
  id 105310
  type person
  graphic CharModel
  sid player
  anim_def soldier
  hp 100
end
""")
	items.close()
	for pair in [
		["CharModel.3di", CHARMODEL_FIXTURE],
		["M4_1st.3di", CHARMODEL_FIXTURE],
		["armsG.3di", CHARMODEL_FIXTURE],
		["soldier.adm", SOLDIER_ADM_FIXTURE],
		["E_STAND.adm", SOLDIER_ADM_FIXTURE],
		["idle.bad", IDLE_BAD_FIXTURE],
		["walk.bad", WALK_BAD_FIXTURE],
	]:
		assert_eq(DirAccess.copy_absolute(
				ProjectSettings.globalize_path(pair[1]),
				root_dir.path_join(pair[0])), OK)
	# The M4's FP clip set (weapon.def animadm M4_1st): the wpn keys over the
	# committed .bads so the real viewmodel parts carry playable clips.
	var adm := FileAccess.open(root_dir.path_join("M4_1st.adm"), FileAccess.WRITE)
	assert_not_null(adm, "staged M4_1st.adm is writable")
	adm.store_string("""anim_reset\t"idle.bad"
anim_wpn_idle\t"idle.bad"
anim_wpn_fire\t"walk.bad"
anim_wpn_recoil\t"idle.bad"
anim_wpn_reload\t"walk.bad"
anim_wpn_empty\t"idle.bad"
anim_wpn_switchto\t"idle.bad"
anim_wpn_switchfrom\t"idle.bad"
anim_wpn_switchrank\t"idle.bad"
""")
	adm.close()
	return root_dir


## The production load: packaged world scene + injected root + playable auto-spawn
## with the M4/satchel kit (the armory-proven canonical profile). `baked_terrain`
## swaps the mission onto the Dvxi5 CPT heightfield so the ported terrain raycast
## and the infantry motor have a real surface.
func _load_player_world(baked_terrain: bool = false) -> GameWorld:
	var packed := load("res://adapter/world/game_world.tscn") as PackedScene
	assert_not_null(packed, "the packaged world scene loads")
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var root := NovaResourceRoot.new()
	assert_eq(root.set_root_dir(_shared_root), OK)
	world.set_resource_root(root)
	world.set_local_player_spawn_loadout({
		"primary": "WPN_M4AUTO",
		"accessory": "WPN_SATCHEL_CHARGE",
		"player_class": 8,
	})
	var mission := NovaMissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	if baked_terrain:
		assert_true(mission.set_header_string("terrain", "Dvxi5"))
		assert_true(mission.set_header_string("environment", "mnml"))
	assert_eq(world.load_mission_data(mission, "mnml.bms"), OK)
	assert_true(world.get_sim().has_local_player(),
			"the playable load auto-spawns the host player (ADR 0011 listen server)")
	return world


func _bare_world() -> GameWorld:
	var world := GameWorld.new()
	var terrain := NovaTerrain.new()
	terrain.name = "NovaTerrain"
	world.add_child(terrain)
	return world


func _attach_presenter(world: GameWorld, camera: Camera3D) -> LocalPlayerPresenter:
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, camera)
	presenter.set_input_source(func() -> Dictionary:
		return {})
	return presenter


# One shell frame, in main_game's order: input sample -> world tick (the engine
# FrameDriver batch + the fixed-tick weapon consumer) -> presentation.
func _frame(world: GameWorld, presenter: LocalPlayerPresenter, camera: Camera3D,
		ticks: int = 1, gameplay_active: bool = true) -> void:
	for i in ticks:
		presenter.before_world_tick(TICK, false, gameplay_active)
		world.tick(camera.global_position, camera.global_transform, TICK)
		presenter.after_world_tick()


# Mission yaw -> the Godot forward the presenter documents: (sin yaw, 0, -cos yaw).
func _forward_for_yaw(yaw_deg: float) -> Vector3:
	var yr := deg_to_rad(yaw_deg)
	return Vector3(sin(yr), 0.0, -cos(yr))


# Every VisualInstance3D under `root`, inclusive (mirrors the presenter's stamping walk).
func _visual_instances(root: Node) -> Array:
	var out: Array = []
	if root is VisualInstance3D:
		out.append(root)
	for child in root.get_children():
		out.append_array(_visual_instances(child))
	return out


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


# --- discovery (temporary) ---------------------------------------------------


func test_discovery_a_motion() -> void:
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	var sim := world.get_sim()
	# Fresh-world movement, before any other state.
	presenter.set_input_source(func() -> Dictionary:
		return {"forward": true})
	var p0: Vector3 = sim.get_local_player_position()
	_frame(world, presenter, camera, 30)
	var p1: Vector3 = sim.get_local_player_position()
	print("mnml move=", p1 - p0, " dist=", (p1 - p0).length(),
			" anim=", sim.get_local_player_anim_key())
	# lean
	presenter.set_input_source(func() -> Dictionary:
		return {"lean_left": true})
	_frame(world, presenter, camera, 30)
	var view := world.local_player_view()
	print("lean_left roll=", view.camera_roll_deg, " fp_roll=", view.fp_roll_deg)
	presenter.set_input_source(func() -> Dictionary:
		return {})
	_frame(world, presenter, camera, 60)
	view = world.local_player_view()
	print("neutral roll=", view.camera_roll_deg)
	# look immediacy: no frame between input and read
	var yaw0 := float(sim.get_local_player_yaw_deg())
	var motion := InputEventMouseMotion.new()
	motion.relative = Vector2(300.0, 0.0)
	presenter.handle_input(motion, true)
	print("yaw before frame=", float(sim.get_local_player_yaw_deg()) - yaw0)
	_frame(world, presenter, camera, 1)
	print("yaw after frame=", float(sim.get_local_player_yaw_deg()) - yaw0)
	# F4 third person
	var head_fp: Vector3 = presenter.avatar_head_world()
	var eye_fp: Vector3 = camera.global_position
	var f4 := InputEventKey.new()
	f4.keycode = KEY_F4
	f4.pressed = true
	presenter.handle_key_input(f4, true)
	_frame(world, presenter, camera, 30)
	view = world.local_player_view()
	print("3P: is_tp=", presenter.is_third_person(), " pose_valid=", view.camera_pose_valid,
			" eye=", view.camera_eye, " cam=", camera.global_position,
			" dist_from_player=", (camera.global_position - sim.get_local_player_position()).length(),
			" fp_dist=", (eye_fp - sim.get_local_player_position()).length())
	print("3P: viewmodel visible=", presenter.viewmodel().visible)
	var avatar := world.get_node("PlayerAvatar_CharModel") as Node3D
	var layers_3p: Array = []
	for vi in _visual_instances(avatar):
		if not layers_3p.has(vi.layers):
			layers_3p.append(vi.layers)
	print("3P avatar layers=", layers_3p, " count=", _visual_instances(avatar).size(),
			" ctrl=", presenter.vm_parts()[0].get_ctrl_values())
	presenter.handle_key_input(f4, true)
	_frame(world, presenter, camera, 2)
	var layers_fp: Array = []
	for vi in _visual_instances(avatar):
		if not layers_fp.has(vi.layers):
			layers_fp.append(vi.layers)
	print("FP avatar layers=", layers_fp, " vm visible=", presenter.viewmodel().visible)
	var vm_layers: Array = []
	for vi in _visual_instances(presenter.viewmodel()):
		if not vm_layers.has(vi.layers):
			vm_layers.append(vi.layers)
	print("FP vm layers=", vm_layers, " head=", head_fp)
	# pass camera
	var rig: PlayerViewmodelRig = presenter.viewmodel_rig()
	var pass_cam: Camera3D = rig.get("_vm_camera")
	var pass_layer: CanvasLayer = rig.get("_vm_pass_layer")
	print("pass cam=", pass_cam, " mask=", pass_cam.cull_mask if pass_cam != null else -1,
			" near=", pass_cam.near if pass_cam != null else -1,
			" layer parent=", pass_layer.get_parent() if pass_layer != null else null)
	print("player cam mask=", camera.cull_mask, " default=", (1 << 20) - 1)


func test_discovery_b_baked_terrain() -> void:
	var world := _load_player_world(true)
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	var sim := world.get_sim()
	var terrain := world.get_terrain_data()
	# scan for a grounded spot
	var spot := Vector3.INF
	for zi in range(4, 28):
		for xi in range(4, 28):
			var p := Vector3(xi * 32.0, 0.0, -zi * 32.0)
			var h := float(terrain.get_height(p))
			if h > 2.0:
				spot = Vector3(p.x, h, p.z)
				break
		if spot != Vector3.INF:
			break
	print("spot=", spot)
	if spot == Vector3.INF:
		for zi in range(4, 28):
			for xi in range(4, 28):
				var p := Vector3(xi * 32.0, 0.0, zi * 32.0)
				var h := float(terrain.get_height(p))
				if h > 2.0:
					spot = Vector3(p.x, h, p.z)
					break
			if spot != Vector3.INF:
				break
		print("spot(+z)=", spot)
	# godot (x, up, z) -> mission (x, -z, up)
	sim.debug_teleport_local_player(Vector3(spot.x, -spot.z, spot.y + 0.5), 0.0, 0.0)
	_frame(world, presenter, camera, 30)
	var pos: Vector3 = sim.get_local_player_position()
	print("settled pos=", pos, " terrain h=", terrain.get_height(pos))
	presenter.set_input_source(func() -> Dictionary:
		return {"forward": true})
	var p0: Vector3 = sim.get_local_player_position()
	_frame(world, presenter, camera, 40)
	var p1: Vector3 = sim.get_local_player_position()
	print("baked move=", p1 - p0, " dist=", (p1 - p0).length(),
			" anim=", sim.get_local_player_anim_key(),
			" yaw=", sim.get_local_player_yaw_deg())
	var avatar := world.get_node("PlayerAvatar_CharModel") as Node3D
	print("avatar clip while moving=", avatar.get_active_body_clip())
	presenter.set_input_source(func() -> Dictionary:
		return {})
	_frame(world, presenter, camera, 40)
	print("avatar clip after stop=", avatar.get_active_body_clip(),
			" settled y=", sim.get_local_player_position().y,
			" h=", terrain.get_height(sim.get_local_player_position()))
	# aim down
	var motion := InputEventMouseMotion.new()
	motion.relative = Vector2(0.0, 1500.0)
	presenter.handle_input(motion, true)
	_frame(world, presenter, camera, 2)
	print("down pitch=", sim.get_local_player_pitch_deg(), " range=", presenter.aim_range_units())
	# cross-check the raycast through the public sampler
	var eye: Vector3 = presenter.avatar_head_world()
	var yr := deg_to_rad(float(sim.get_local_player_yaw_deg()))
	var pr := deg_to_rad(float(sim.get_local_player_pitch_deg()))
	var fwd := Vector3(sin(yr) * cos(pr), sin(pr), -cos(yr) * cos(pr))
	var hit: Vector3 = terrain.raycast_terrain(eye, eye + fwd * 1000.0)
	print("hit=", hit, " expected range=", int(sim.get_local_player_position().distance_to(hit)))
	# aim up to the sky
	motion = InputEventMouseMotion.new()
	motion.relative = Vector2(0.0, -4000.0)
	presenter.handle_input(motion, true)
	_frame(world, presenter, camera, 2)
	print("up pitch=", sim.get_local_player_pitch_deg(), " range=", presenter.aim_range_units())
	# fov reality
	var size: Vector2 = camera.get_viewport().get_visible_rect().size
	print("viewport size=", size, " camera.fov=", camera.fov, " expected=",
			NovaSimulation.fov_vertical_from_horizontal(80.0, size.x / size.y))
	# firing: real FSM fire on the real vm parts
	var part0 = presenter.vm_parts()[0]
	print("pre-fire clip=", part0.get_active_body_clip(), " heat=",
			part0.get_ctrl_values().get("HEAT_GLOW"))
	presenter.before_world_tick(TICK, false, true)
	sim.set_local_player_weapon_input(true, true, false)
	world.tick(camera.global_position, camera.global_transform, TICK)
	presenter.after_world_tick()
	print("post-fire-tick1 clip=", part0.get_active_body_clip(),
			" time=", part0.get_animation_time(),
			" view anim=", world.local_player_weapon_view().anim_key,
			" heat=", part0.get_ctrl_values().get("HEAT_GLOW"))
	for i in 3:
		presenter.before_world_tick(TICK, false, true)
		sim.set_local_player_weapon_input(true, false, false)
		world.tick(camera.global_position, camera.global_transform, TICK)
		presenter.after_world_tick()
	print("post-fire-tick4 clip=", part0.get_active_body_clip(),
			" heat=", part0.get_ctrl_values().get("HEAT_GLOW"),
			" ammo... weapon_view serial=", world.local_player_weapon_view().play_serial)


func test_discovery_c_events_and_binoc() -> void:
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	await get_tree().process_frame
	var sim := world.get_sim()
	# Backlog with NO presenter: category request events accumulate in the sim.
	sim.request_local_player_weapon_category(7)
	for i in 45:
		world.tick(camera.global_position, camera.global_transform, TICK)
	print("no-consumer sim weapon=", sim.get_local_player_weapon_name(),
			" world weapon=", world.local_player_weapon_name())
	# Attach: setup drains the backlog.
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	var leftover := world.drain_local_player_weapon_events()
	print("post-setup manual drain=", leftover.size(),
			" world weapon still=", world.local_player_weapon_name())
	# Live: presenter consumes each batch; the switch presentation applies.
	sim.request_local_player_weapon_category(3)
	for i in 60:
		_frame(world, presenter, camera, 1)
	print("live switch: sim=", sim.get_local_player_weapon_name(),
			" world=", world.local_player_weapon_name(),
			" drain=", world.drain_local_player_weapon_events().size())
	# binoculars: ctrl suppression + viewmodel visibility
	var b := InputEventKey.new()
	b.physical_keycode = KEY_B
	b.pressed = true
	presenter.handle_key_input(b, true)
	_frame(world, presenter, camera, 30)
	var view := world.local_player_view()
	print("binoc active=", view.binoculars_view_active,
			" vm visible=", presenter.viewmodel().visible,
			" ctrl=", presenter.vm_parts()[0].get_ctrl_values() if presenter.vm_parts().size() > 0 else null,
			" yaw_off=", view.binocular_yaw_offset_deg)
	print("aim range while binoc=", presenter.aim_range_units())
	# drop binoculars again (a raised binocular gates weapon switches)
	presenter.handle_key_input(b, true)
	_frame(world, presenter, camera, 30)
	print("binoc off=", world.local_player_view().binoculars_view_active)
	# nvg gain sequence
	var n := InputEventKey.new()
	n.physical_keycode = KEY_N
	n.pressed = true
	presenter.handle_key_input(n, true)
	_frame(world, presenter, camera, 2)
	print("nvg on gain=", world.local_player_view().nvg_gain,
			" active=", world.local_player_view().nvg_active)
	var plus := InputEventKey.new()
	plus.physical_keycode = KEY_EQUAL
	plus.pressed = true
	presenter.handle_key_input(plus, true)
	_frame(world, presenter, camera, 2)
	print("after + gain=", world.local_player_view().nvg_gain)
	var minus := InputEventKey.new()
	minus.physical_keycode = KEY_MINUS
	minus.pressed = true
	presenter.handle_key_input(minus, true)
	presenter.handle_key_input(minus, true)
	_frame(world, presenter, camera, 2)
	print("after -- gain=", world.local_player_view().nvg_gain)
	# teardown releases the consumer
	presenter.teardown()
	sim.request_local_player_weapon_category(7)
	for i in 45:
		world.tick(camera.global_position, camera.global_transform, TICK)
	print("post-teardown drain=", world.drain_local_player_weapon_events().size(),
			" sim weapon=", sim.get_local_player_weapon_name())
