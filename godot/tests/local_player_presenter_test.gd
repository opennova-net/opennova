extends GutTest

# LocalPlayerPresenter over a REAL GameWorld + Simulation (the ADR 0033 typed
# boundary: setup(world: GameWorld, camera: Camera3D, fly_camera: FlyCamera)).
# Every test stages the minimal mission fixture, loads mnml.bms through the packaged
# world scene (playable auto-spawn: the ADR 0011 listen-server host player), and
# observes behavior through the sim's own getters, the PlayerLocalView snapshot,
# real ObjectModel observables, and the real terrain raycast.
#
# Old fake-driven contracts that could not be honestly observed on real components
# were dropped (not faked) — the fixture root has no .ptl effect catalog, no
# retail soundsets, and no emplacement/interior geometry:
# - muzzle/casing particle routing + owner-bound effect anchors (FIRE scope gates,
#   Always transients, catch-up ages, slot keys, REVX02 recoil-authored muzzle):
#   the spawn side needs a .ptl catalog; the FSM event production is ctest-pinned.
# - action begin/end sound legs + catch-up audio ordering: soundset resolution
#   needs retail .LWF banks.
# - interior blink lighting transfer: needs portal-carrying buildings, and the
#   model lighting context has no read-back observable (set-only surface).
# - emplaced EWEAP_GUNYAW/GUNPITCH ctrl pair + UseGun switch/clear events: need a
#   real emplacement mount; TEX_TEAM sign-extension (team 0xFE) needs a wire team.
# - the composed camera pose VALUES (recoil doubling, march back-off, blend
#   tuples): composed in the engine now — this file pins the presenter's 1:1
#   stamp of the real composed pose; the ctest player_view suite pins the math.
# - the fixed-tick catch-up phase math (age-two resumes, double-advance guards):
#   needs authored multi-tick batches; the real 62.5 Hz drive here consumes one
#   tick per frame, and the phase math is ctest-pinned engine state.
# - the router's capture-gated trigger sampling (LMB only while captured): a
#   headless display cannot hold real mouse buttons; the fire test injects on
#   the same sim seam the router drives.
# - aim_range on a world with NO terrain object: unreachable on a loaded real
#   world; the raycast-miss -> 1000 fallback covers the same readout behavior.


const TEST_ROOT := "local_player_presenter_test"
var TICK := Simulation.tick_dt()

const MINIMAL_FIXTURE_DIR := "res://../assets"
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
	TestFs.remove_dir_recursive(OS.get_cache_dir().path_join(TEST_ROOT))


func after_each() -> void:
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


# --- real-world staging -------------------------------------------------------
# The minimal fixture plus the committed model/anim fixtures arranged under the
# names the production resolvers ask for: the full weapon.def (WPN_M4AUTO with
# its gfx/animadm/pos rows), a person items.def row for the player visual item
# (105310 -> CharModel + soldier.adm), the infantry clip set (E_STAND.adm) so
# the motor's body selection runs, and the 19-bone CharModel staged as the M4's
# FP gun/arms rig with a wpn-key clip set over the committed .bads.

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
	# The player's character registry: retail's ONLY first-person arms source is
	# the selected combo's arms part (weapon.def gfx1a is a discarded token), so
	# one good-side combo binds the staged CharModel head/body + the armsG arms.
	var avatars := FileAccess.open(root_dir.path_join("Avatars.def"), FileAccess.WRITE)
	assert_not_null(avatars, "staged Avatars.def is writable")
	avatars.store_string("""define head STAGED_HEAD
{
	graphic CharModel.3di
	camo 0 0 0
	voice 1
	sex m
}
define body STAGED_BODY
{
	graphic CharModel.3di
	camo 0 0 0
}
define arms STAGED_ARMS
{
	graphic armsG.3di
	camo 4 2 0
}
nationality 0 STAGED_NAT
{
	alignment good
	division 0 STAGED_DIV
	{
		combo 1 STAGED_HEAD STAGED_BODY STAGED_ARMS
	}
}
""")
	avatars.close()
	for pair in [
		["CharModel.3di", CHARMODEL_FIXTURE],
		["M4_1st.3di", CHARMODEL_FIXTURE],
		["M4_3RD.3di", CHARMODEL_FIXTURE],
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
## has a real surface to measure.
func _load_player_world(baked_terrain: bool = false) -> GameWorld:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	assert_not_null(packed, "the packaged world scene loads")
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_shared_root), OK)
	world.set_resource_root(root)
	world.set_local_player_spawn_loadout({
		"primary": "WPN_M4AUTO",
		"accessory": "WPN_SATCHEL_CHARGE",
		"player_class": 8,
	})
	var mission := MissionData.new()
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
	var terrain := Terrain.new()
	terrain.name = "Terrain"
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
# inmatch::Session batch + the fixed-tick weapon consumer) -> presentation.
func _frame(world: GameWorld, presenter: LocalPlayerPresenter, camera: Camera3D,
		ticks: int = 1, gameplay_active: bool = true) -> void:
	for i in ticks:
		var frame_input := presenter.before_world_tick(TICK, false, gameplay_active)
		world.tick(camera.global_position, camera.global_transform, TICK, frame_input)
		presenter.after_world_tick()


# Frames until `predicate` (no arguments -> bool) holds; false when `max_frames`
# elapse first.
func _frame_until(world: GameWorld, presenter: LocalPlayerPresenter, camera: Camera3D,
		predicate: Callable, max_frames: int = 64) -> bool:
	for i in max_frames:
		_frame(world, presenter, camera, 1)
		if bool(predicate.call()):
			return true
	return false


func _key(keycode: Key, physical: bool = false) -> InputEventKey:
	var key := InputEventKey.new()
	if physical:
		key.physical_keycode = keycode
	else:
		key.keycode = keycode
	key.pressed = true
	return key


func _look(presenter: LocalPlayerPresenter, relative: Vector2, active: bool = true) -> bool:
	var motion := InputEventMouseMotion.new()
	motion.relative = relative
	return presenter.handle_input(motion, active)


# Mission yaw -> the Godot forward the presenter documents: (sin yaw, 0, -cos yaw).
func _forward_for(yaw_deg: float, pitch_deg: float = 0.0) -> Vector3:
	var yr := deg_to_rad(yaw_deg)
	var pr := deg_to_rad(pitch_deg)
	return Vector3(sin(yr) * cos(pr), sin(pr), -cos(yr) * cos(pr))


# Every VisualInstance3D under `root`, inclusive (mirrors the presenter's stamping walk).
func _visual_instances(root: Node) -> Array:
	var out: Array = []
	if root is VisualInstance3D:
		out.append(root)
	for child in root.get_children():
		out.append_array(_visual_instances(child))
	return out


func _local_avatar(world: GameWorld) -> ObjectModel:
	return world.get_node_or_null("PlayerAvatar_CharModel") as ObjectModel


# --- input routing into the sim ----------------------------------------------


func test_input_source_movement_reaches_the_motor_and_neutralizes_when_inactive() -> void:
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	var sim := world.get_sim()
	_frame(world, presenter, camera, 2)
	assert_eq(String(sim.get_local_player_anim_key()), "anim_idle",
			"the freshly spawned player idles")

	# Held forward reaches the motor's body selection: the walk/run promotion
	# leaves idle [orig: Player_PackInputStateToEntity @0x4df450; promotion @0x4b729d].
	presenter.set_input_source(func() -> Dictionary:
		return {"forward": true})
	assert_true(_frame_until(world, presenter, camera, func() -> bool:
		return String(sim.get_local_player_anim_key()) != "anim_idle"),
			"held forward promotes the body selection off idle")

	# Releasing the source settles the motor back to idle.
	presenter.set_input_source(func() -> Dictionary:
		return {})
	assert_true(_frame_until(world, presenter, camera, func() -> bool:
		return String(sim.get_local_player_anim_key()) == "anim_idle"),
			"releasing input settles the motor back to idle")

	# A live UI overlay keeps the world ticking but submits a NEUTRAL movement
	# frame: the same held source no longer reaches the motor.
	presenter.set_input_source(func() -> Dictionary:
		return {"forward": true})
	var tick_before := int(sim.get_logic_tick())
	for i in 30:
		var frame_input := presenter.before_world_tick(TICK, false, false)
		world.tick(camera.global_position, camera.global_transform, TICK, frame_input)
		presenter.after_world_tick()
	assert_eq(String(sim.get_local_player_anim_key()), "anim_idle",
			"the armory overlay's neutral submit keeps the player standing")
	assert_gt(int(sim.get_logic_tick()), tick_before,
			"the world keeps ticking under the inactive overlay")

	# Q/E lean is entity state the sim composes into the camera roll (lean/4
	# rides fp_roll). Sign: positive roll tilts right, so lean LEFT is negative
	# [orig: roll = entity+0x2DC + (entity+0xB0)/4 @0x437fe6].
	presenter.set_input_source(func() -> Dictionary:
		return {"lean_left": true})
	_frame(world, presenter, camera, 30)
	var view := world.local_player_view()
	assert_lt(view.camera_roll_deg, -5.0, "held lean-left composes a leftward camera roll")
	assert_lt(camera.global_basis.y.x, -0.01,
			"negative composed roll tilts the stamped view left")
	presenter.set_input_source(func() -> Dictionary:
		return {})
	_frame(world, presenter, camera, 60)
	assert_almost_eq(world.local_player_view().camera_roll_deg, 0.0, 1.0,
			"releasing the lean eases the composed roll back out")


func test_mouse_motion_forwards_raw_pixels_to_the_sim_pipeline() -> void:
	# The presenter no longer scales or accumulates look: raw pixel deltas go to
	# Simulation.add_local_player_look and the witnessed integer pipeline —
	# sens<<11, scoped zoom reduction, the pitch clamps — folds them in at the
	# tick [orig: Input_ProcessMouseAxisBindings @0x499680].
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	var sim := world.get_sim()
	_frame(world, presenter, camera, 1)
	var yaw0 := float(sim.get_local_player_yaw_deg())

	assert_true(_look(presenter, Vector2(300.0, 0.0)), "active mouse motion is consumed")
	_frame(world, presenter, camera, 1)
	# The default-sensitivity pipeline is exact: 300 px through the fresh sim's
	# sens<<11 scale lands 6.591796875 degrees of yaw (mouse right = yaw right).
	assert_almost_eq(float(sim.get_local_player_yaw_deg()) - yaw0, 6.591796875, 0.001,
			"raw pixels reach the sim's witnessed integer look pipeline")

	var pitch0 := float(sim.get_local_player_pitch_deg())
	assert_true(_look(presenter, Vector2(0.0, 600.0)))
	_frame(world, presenter, camera, 1)
	assert_lt(float(sim.get_local_player_pitch_deg()), pitch0 - 5.0,
			"mouse down pitches the aim down (retail non-inverted default)")

	var yaw_now := float(sim.get_local_player_yaw_deg())
	assert_false(_look(presenter, Vector2(500.0, 0.0), false),
			"inactive input is not forwarded")
	_frame(world, presenter, camera, 1)
	assert_almost_eq(float(sim.get_local_player_yaw_deg()), yaw_now, 0.001,
			"inactive motion leaves the sim's look state untouched")


func test_stance_keys_are_three_key_select_requests() -> void:
	# The witnessed 3-key SELECT (Z prone, X crouch, C stand — catalog ids 9/10/11):
	# each key REQUESTS its stance; the sim owns mutual exclusion + the ForceCrouch
	# refusal. [orig: input cases 170/169/172 -> C2S 0x1D @0x501c60]
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	var sim := world.get_sim()
	_frame(world, presenter, camera, 2)
	assert_eq(int(sim.get_local_player_stance()), 0, "the spawned player stands")

	for pair in [[KEY_Z, 2], [KEY_X, 1], [KEY_C, 0]]:
		assert_true(presenter.handle_key_input(_key(pair[0]), true),
				"stance key is consumed")
		_frame(world, presenter, camera, 3)
		assert_eq(int(sim.get_local_player_stance()), int(pair[1]),
				"the sim grants the requested stance for key %d" % pair[0])


func test_binoculars_nvg_and_gain_keys_route_retail_actions() -> void:
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame

	assert_true(presenter.handle_key_input(_key(KEY_B, true), true))
	assert_true(_frame_until(world, presenter, camera, func() -> bool:
		return world.local_player_view().binoculars_view_active),
			"B raises the binocular view through the sim's own ease")
	assert_true(presenter.handle_key_input(_key(KEY_B, true), true))
	assert_true(_frame_until(world, presenter, camera, func() -> bool:
		return not world.local_player_view().binoculars_view_active),
			"a second B lowers the binoculars again")

	assert_true(presenter.handle_key_input(_key(KEY_N, true), true))
	_frame(world, presenter, camera, 2)
	var view := world.local_player_view()
	assert_true(view.nvg_active, "N toggles the sim's NVG state")
	assert_eq(view.nvg_gain, 0, "NVG starts at the base gain step")
	assert_true(presenter.handle_key_input(_key(KEY_EQUAL, true), true))
	_frame(world, presenter, camera, 2)
	assert_eq(world.local_player_view().nvg_gain, 1, "'+' steps the gain up")
	assert_true(presenter.handle_key_input(_key(KEY_MINUS, true), true))
	assert_true(presenter.handle_key_input(_key(KEY_MINUS, true), true))
	_frame(world, presenter, camera, 2)
	assert_eq(world.local_player_view().nvg_gain, 0,
			"'-' steps the gain down and the sim clamps at the floor")


# --- the camera cluster -------------------------------------------------------


func test_camera_stamps_the_sim_composed_pose_and_policy_fov() -> void:
	# ADR 0016 + world/player_view.h (S8): the ADS ease, the fov policy, and the
	# whole FP/TP camera composition are SIM state at the world cadence; this
	# presenter converts the mission-euler pose to the Godot frame and stamps the
	# node 1:1 [orig: Camera_ComputeThirdPersonView @0x437d10 — the on-foot person
	# leg @0x437f9c..0x438031].
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var saved_fov := camera.fov
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	var sim := world.get_sim()
	_frame(world, presenter, camera, 3)

	var view := world.local_player_view()
	assert_true(view.camera_pose_valid, "a live local player composes a camera pose")
	assert_almost_eq((camera.global_position - view.camera_eye).length(), 0.0, 0.001,
			"the camera sits exactly at the sim-composed eye")
	# The eye is the POSED HEAD BONE pulled back 0.1875 u along the view forward
	# [orig: CameraOffset = head - Position @0x4b6bb3; kFpEyePullback]. The head
	# sample comes from the avatar's real render skeleton (D-INF-18).
	var head: Vector3 = presenter.avatar_head_world()
	assert_ne(head, Vector3.INF, "the avatar serves a posed head bone")
	var yaw := float(sim.get_local_player_yaw_deg())
	assert_almost_eq(view.camera_eye, head - _forward_for(yaw) * 0.1875,
			Vector3(0.01, 0.01, 0.01),
			"the composed eye is the posed head bone plus the FP pull-back")

	# The policy fov (80 unscoped) through the ONE shared h->v conversion
	# [orig: @0x58d900] against the live viewport aspect.
	assert_almost_eq(view.fov_h_deg, 80.0, 0.001, "the unscoped policy fov is 80")
	var size := camera.get_viewport().get_visible_rect().size
	assert_almost_eq(camera.fov,
			Simulation.fov_vertical_from_horizontal(view.fov_h_deg, size.x / size.y),
			0.001, "the camera fov is the sim's policy value through the shared conversion")

	# Pitch rides the pose 1:1: look down, then compare the stamped forward.
	assert_true(_look(presenter, Vector2(0.0, 800.0)))
	_frame(world, presenter, camera, 1)
	view = world.local_player_view()
	assert_lt(view.camera_pitch_deg, -10.0, "the composed pose follows the look pitch")
	var fp_forward := -camera.global_basis.z
	assert_almost_eq(rad_to_deg(asin(fp_forward.y)), view.camera_pitch_deg, 0.01,
			"the camera pitches to the composed pitch exactly")

	# No gameplay key moves the camera on foot: F4 is retail's `viewchase` row,
	# a PREFERENCE the sim's arbiter resolves to third person only in a control
	# seat [orig: Input_HandleActionBinding case 402 @0x49c0f6; the arbiter
	# Render_ProcessMainSceneFrame @0x5ca1d2]. The on-foot chase is the debug
	# menu's override; its third-person composition pulls the eye away from
	# the player.
	var player_pos: Vector3 = sim.get_local_player_position()
	var fp_distance := (camera.global_position - player_pos).length()
	assert_false(presenter.handle_key_input(_key(KEY_F4), true),
			"F4 is not a shell camera key")
	assert_false(presenter.is_third_person(), "on foot the chase preference stays first person")
	presenter.set_debug_third_person(true)
	assert_true(presenter.is_third_person(), "the debug override resolves third person on foot")
	_frame(world, presenter, camera, 20)
	var tp_distance := (camera.global_position - sim.get_local_player_position()).length()
	assert_gt(tp_distance, fp_distance + 0.1,
			"the sim's third-person composition chases the eye back from the player")
	presenter.set_debug_third_person(false)
	assert_false(presenter.is_third_person(), "clearing the override returns to first person")

	presenter.teardown()
	assert_almost_eq(camera.fov, saved_fov, 0.001,
			"teardown restores the camera's original fov")


func test_gameplay_camera_collects_hidden_player_shadows_without_drawing_fp_models() -> void:
	# Retail draws the local body/held weapon separately from the near-Z first-person
	# overlay [orig: Player_RenderFirstPersonViewModel @ 0x4ded60]. The hidden
	# body and held weapon are camera-renderable (cast ON) but hidden by LAYER
	# alone, so the render-slot capture cameras can photograph the posed body
	# (SHADOWS_ONLY geometry is invisible to every camera); the beauty camera
	# excludes the FP layer, the capture channels, AND the caster marker
	# layers (the entity-shadow shadow map is retired — nothing needs the
	# markers beauty-admitted). The dedicated viewmodel pass stays
	# non-casting.
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var saved_mask := camera.cull_mask
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame  # setup() mounts the FP pass deferred
	_frame(world, presenter, camera, 2)

	assert_eq(camera.cull_mask & Water.VISUAL_LAYER_FP_BODY_SHADOW_ONLY, 0,
			"setup() masks the FP body layer off the player camera")
	# The FP viewmodel renders through the dedicated renderfov pass, never the
	# player camera [orig: Player_RenderFirstPersonViewModel @0x4ded60 — own
	# projection + flush].
	assert_eq(camera.cull_mask & Water.VISUAL_LAYER_VIEWMODEL, 0,
			"setup() masks the viewmodel layer off the player camera (the FP pass draws it)")
	assert_eq(camera.cull_mask & Water.VISUAL_LAYER_SHADOW_CASTER_MASK, 0,
			"the gameplay camera excludes the caster marker layers (the"
			+ " camera-renderable hidden body must not leak through them)")
	assert_eq(camera.cull_mask & Water.VISUAL_LAYER_SLOT_CAPTURE_MASK, 0,
			"the gameplay camera excludes the render-slot capture channels")
	var rig: PlayerViewmodelRig = presenter.viewmodel_rig()
	var pass_cam: Camera3D = rig.get("_vm_camera")
	assert_not_null(pass_cam, "setup() builds the FP render pass camera")
	if pass_cam != null:
		assert_eq(pass_cam.cull_mask, Water.VISUAL_LAYER_VIEWMODEL,
				"the pass camera draws ONLY the viewmodel layer")
		assert_almost_eq(pass_cam.near, 0.05, 0.0001,
				"the pass near plane is the witnessed 0.05 swap [orig: @0x4dee29]")
	var pass_layer: CanvasLayer = rig.get("_vm_pass_layer")
	assert_not_null(pass_layer, "setup() mounts the FP pass")
	if pass_layer != null:
		assert_eq(pass_layer.get_parent(), camera.get_viewport(),
				"the pass composites into the camera's viewport")

	var avatar := _local_avatar(world)
	assert_not_null(avatar, "the shared presenter built the real 3P avatar")
	var viewmodel: Node3D = presenter.viewmodel()
	assert_not_null(viewmodel, "the shared presenter built the real FP viewmodel")
	var held_weapon: ObjectModel = presenter.held_weapon()
	assert_not_null(held_weapon, "the shared presenter built the real held weapon")
	assert_true(avatar.visible,
			"the body stays VISIBLE in first person - it remains a live shadow source")
	assert_true(viewmodel.visible, "the FP overlay shows in first person")
	var body_instances := _visual_instances(avatar)
	var vm_instances := _visual_instances(viewmodel)
	var held_instances := _visual_instances(held_weapon) if held_weapon != null else []
	assert_gt(body_instances.size(), 0, "the real body carries visual instances")
	assert_gt(vm_instances.size(), 0, "the real viewmodel carries visual instances")
	assert_gt(held_instances.size(), 0, "the real held weapon carries visual instances")
	for vi in body_instances:
		assert_eq(vi.layers & ~(Water.VISUAL_LAYER_SHADOW_CASTER_MASK | Water.VISUAL_LAYER_SLOT_CAPTURE_MASK),
				Water.VISUAL_LAYER_FP_BODY_SHADOW_ONLY,
				"first person: the body's visual instances ride the hidden FP layer")
		if vi is GeometryInstance3D:
			assert_ne(vi.layers & Water.VISUAL_LAYER_DYNAMIC_SHADOW_CASTER, 0,
					"first person: body geometry keeps its dynamic-caster marker")
			assert_eq(vi.cast_shadow,
					GeometryInstance3D.SHADOW_CASTING_SETTING_ON,
					"first person: body geometry stays camera-renderable so the"
					+ " slot capture cameras can photograph it (hidden by layer)")
	for vi in held_instances:
		assert_eq(vi.layers & ~(Water.VISUAL_LAYER_SHADOW_CASTER_MASK | Water.VISUAL_LAYER_SLOT_CAPTURE_MASK),
				Water.VISUAL_LAYER_FP_BODY_SHADOW_ONLY,
				"first person: the held weapon rides the hidden FP layer")
		if vi is GeometryInstance3D:
			assert_ne(vi.layers & Water.VISUAL_LAYER_DYNAMIC_SHADOW_CASTER, 0,
					"first person: held-weapon geometry keeps its dynamic-caster marker")
			assert_eq(vi.cast_shadow,
					GeometryInstance3D.SHADOW_CASTING_SETTING_ON,
					"first person: held-weapon geometry stays camera-renderable"
					+ " for the slot capture (hidden by layer)")
	for vi in vm_instances:
		assert_eq(vi.layers, Water.VISUAL_LAYER_VIEWMODEL,
				"the dedicated viewmodel pass clears every shadow-caster marker")
		if vi is GeometryInstance3D:
			assert_eq(vi.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF,
					"the dedicated viewmodel pass never casts into the world")

	presenter.set_debug_third_person(true)
	_frame(world, presenter, camera, 1)
	for vi in _visual_instances(avatar):
		assert_eq(vi.layers & ~(Water.VISUAL_LAYER_SHADOW_CASTER_MASK | Water.VISUAL_LAYER_SLOT_CAPTURE_MASK),
				Water.VISUAL_LAYER_WORLD,
				"third person: the body returns to the normal world layer")
		if vi is GeometryInstance3D:
			assert_eq(vi.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_ON,
					"third person: body geometry renders and casts normally")
	if held_weapon != null:
		for vi in _visual_instances(held_weapon):
			assert_eq(vi.layers & ~(Water.VISUAL_LAYER_SHADOW_CASTER_MASK | Water.VISUAL_LAYER_SLOT_CAPTURE_MASK),
					Water.VISUAL_LAYER_WORLD,
					"third person: the held weapon returns to the normal world layer")
			if vi is GeometryInstance3D:
				assert_eq(vi.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_ON,
						"third person: held-weapon geometry renders and casts normally")
	assert_true(avatar.visible, "the body shows in third person")
	assert_false(presenter.viewmodel().visible,
			"the FP overlay hides entirely in third person")
	for vi in _visual_instances(viewmodel):
		assert_eq(vi.layers, Water.VISUAL_LAYER_VIEWMODEL,
				"the hidden viewmodel remains outside every shadow-caster layer")
		if vi is GeometryInstance3D:
			assert_eq(vi.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF,
					"the hidden viewmodel remains non-casting")

	presenter.set_debug_third_person(false)
	presenter.set_debug_body_in_first_person(true)
	_frame(world, presenter, camera, 1)
	for vi in _visual_instances(avatar):
		assert_eq(vi.layers & ~(Water.VISUAL_LAYER_SHADOW_CASTER_MASK | Water.VISUAL_LAYER_SLOT_CAPTURE_MASK),
				Water.VISUAL_LAYER_WORLD,
				"the debug first-person body uses the visible world layer")
		if vi is GeometryInstance3D:
			assert_eq(vi.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_ON,
					"the debug first-person body renders and casts normally")
	if held_weapon != null:
		for vi in _visual_instances(held_weapon):
			if vi is GeometryInstance3D:
				assert_eq(vi.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_ON,
						"the debug first-person held weapon renders and casts normally")

	presenter.teardown()
	assert_eq(camera.cull_mask, saved_mask,
			"teardown() restores the player camera's cull mask exactly")


# The game shell calls setup() from its own _ready — while the player camera's
# viewport is still making its children ready, so it rejects add_child ("parent
# busy": _propagate_ready blocks the parent for the whole walk). A direct FP-pass
# mount fails then, leaving the viewmodel layer masked off the player camera with
# nothing drawing it: an invisible FP viewmodel in the runtime. The pass mount
# is deferred for exactly this boot shape; this pins it.
class BootTrigger:
	extends Node
	var presenter: LocalPlayerPresenter
	var world: GameWorld
	var camera: Camera3D

	func _ready() -> void:
		presenter.setup(world, camera)


func test_setup_during_scene_ready_still_mounts_the_fp_pass() -> void:
	var vp := SubViewport.new()  # the play viewport the pass composites into
	var trigger := BootTrigger.new()
	trigger.world = _bare_world()
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


func test_shared_presenter_teardown_releases_captured_mouse() -> void:
	var world := _bare_world()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	presenter.setup(world, camera)

	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)
	presenter.teardown()

	assert_eq(Input.get_mouse_mode(), Input.MOUSE_MODE_VISIBLE)


# --- binocular rangefinder ----------------------------------------------------
# The range readout traces the ported retail terrain raycast
# (TerrainData.raycast_terrain -> engine/runtime/terrain_query
# [orig: Terrain_RaycastHeightmapHiRes_0 @0x60e710]), measured from entity
# Position to the collision/far endpoint and clamped to the 1..1000 display.


func test_aim_range_measures_the_real_terrain_and_clamps_to_the_projection() -> void:
	var world := _load_player_world(true)
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	var sim := world.get_sim()
	var terrain := world.get_terrain_data()
	_frame(world, presenter, camera, 2)
	# Hold the player above open terrain so the ray has a real surface below.
	var h := float(terrain.get_height(Vector3(256.0, 0.0, -256.0)))
	sim.debug_teleport_local_player(Vector3(256.0, 256.0, h + 40.0), 0.0, 0.0)
	_frame(world, presenter, camera, 1)

	# Aim well below the horizon.
	assert_true(_look(presenter, Vector2(0.0, 2500.0)))
	_frame(world, presenter, camera, 1)
	assert_lt(float(sim.get_local_player_pitch_deg()), -40.0)
	var range_down := presenter.aim_range_units()
	assert_between(range_down, 5, 400, "the downward readout measures a real hit")
	# The readout equals the same measure taken through the public sampler: the
	# eye is the posed head (floored), the endpoint the 1000-unit projection.
	var pos: Vector3 = sim.get_local_player_position()
	var eye: Vector3 = presenter.avatar_head_world()
	eye.y = maxf(eye.y, pos.y + Simulation.player_eye_min_above_position())
	var forward := _forward_for(float(sim.get_local_player_yaw_deg()),
			float(sim.get_local_player_pitch_deg()))
	var hit: Vector3 = terrain.raycast_terrain(
			eye, eye + forward * Simulation.player_aim_project_range())
	assert_false(is_nan(hit.x), "the downward ray hits the real heightfield")
	var expected := clampi(int(pos.distance_to(hit)), 1, 1000)
	assert_between(range_down, expected - 1, expected + 1,
			"the readout is the terrain-raycast distance from entity Position")

	# Aim at the sky: the raycast misses and the readout clamps to the retail
	# 1000-unit projection endpoint.
	assert_true(_look(presenter, Vector2(0.0, -8000.0)))
	_frame(world, presenter, camera, 1)
	assert_gt(float(sim.get_local_player_pitch_deg()), 45.0)
	assert_eq(presenter.aim_range_units(), 1000,
			"a terrain miss falls back to the far endpoint of the projection")


# --- the real FP viewmodel ----------------------------------------------------


func test_viewmodel_ctrl_registers_follow_visibility_and_team() -> void:
	# The FP CTRL writers execute only on a visible FP submit: TEX_TEAM is the
	# store immediately before the FP lighting/heat/model-submit path, and heat
	# publishes per submit [orig: Player_RenderFirstPersonViewModel
	# @0x4DEE96..0x4DEE9F, @0x4DEEC2..0x4DEEF5]. Hidden, carded, binocular and
	# third-person frames never execute those writers.
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	var sim := world.get_sim()
	_frame(world, presenter, camera, 2)

	assert_eq(presenter.vm_parts().size(), 2,
			"the real M4 viewmodel builds both parts (arms + gun)")
	var weapon_view: PlayerWeaponView = world.local_player_weapon_view()
	assert_not_null(weapon_view, "the installed M4 serves a live FSM view")
	for part_v in presenter.vm_parts():
		var part := part_v as ObjectModel
		var ctrl: Dictionary = part.get_ctrl_values()
		assert_eq(int(ctrl.get("TEX_TEAM", -999)), int(sim.get_local_player_team()),
				"the visible FP submit stores the sim's team byte")
		assert_eq(int(ctrl.get("HEAT_GLOW", -999)), int(weapon_view.heat_glow),
				"the FP writer publishes the FSM's literal heat value (cold zero)")
	# The arms part alone carries the character's raw camo triplet, stored by
	# the same per-submit writer family (Avatar_SetArmsCamoCtrl before the arms
	# submit); the gun part never does.
	var arms_part := presenter.vm_parts()[0] as ObjectModel
	var gun_part := presenter.vm_parts()[1] as ObjectModel
	assert_eq(String(arms_part.get_meta("avatar_part", "")), "arms",
			"the first viewmodel part is the character's arms")
	assert_eq(int(arms_part.get_ctrl_values().get("TEX_CAMO1", -1)), 4)
	assert_eq(int(arms_part.get_ctrl_values().get("TEX_CAMO2", -1)), 2)
	assert_eq(int(arms_part.get_ctrl_values().get("TEX_CAMO3", -1)), 0,
			"the visible FP submit stores the staged combo's raw arms camo bytes")
	assert_false(gun_part.get_ctrl_values().has("TEX_CAMO1"),
			"the arms camo never lands on the gun part")

	presenter.set_debug_third_person(true)
	_frame(world, presenter, camera, 1)
	for part_v in presenter.vm_parts():
		assert_true((part_v as ObjectModel).get_ctrl_values().is_empty(),
				"a third-person frame does not execute any FP CTRL writer")

	presenter.set_debug_third_person(false)
	_frame(world, presenter, camera, 1)
	var part0 := presenter.vm_parts()[0] as ObjectModel
	assert_eq(int(part0.get_ctrl_values().get("TEX_TEAM", -999)),
			int(sim.get_local_player_team()),
			"returning to first person re-runs the CTRL writers")

	# The binocular card path suppresses the FP model submit entirely.
	assert_true(presenter.handle_key_input(_key(KEY_B, true), true))
	assert_true(_frame_until(world, presenter, camera, func() -> bool:
		return world.local_player_view().binoculars_view_active))
	_frame(world, presenter, camera, 1)
	assert_false(presenter.viewmodel().visible,
			"the binocular view replaces the FP viewmodel for the frame")
	assert_true(part0.get_ctrl_values().is_empty(),
			"the binocular card path suppresses the FP CTRL writers too")


func test_fire_event_plays_the_fsm_clip_on_both_real_viewmodel_parts() -> void:
	# The equipped FSM's FIRE begin lands its clip on BOTH viewmodel parts (arms
	# + gun share the animadm) at the production-tick phase, and the fire ->
	# recoil action chain follows [orig: ActionSlot_BeginActivePhase @0x53f830
	# plays the action clip on the owner's animadm channel].
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	var sim := world.get_sim()
	_frame(world, presenter, camera, 2)
	assert_eq(presenter.vm_parts().size(), 2)
	for part_v in presenter.vm_parts():
		assert_eq(String((part_v as ObjectModel).get_active_body_clip()),
				"anim_wpn_idle", "the mounted weapon idles in its FP holding pose")
	var serial_before := int(world.local_player_weapon_view().play_serial)

	# One trigger pull, injected on the same seam the input router drives (the
	# router's own sample overwrites earlier in this frame; last write wins).
	var frame_input := presenter.before_world_tick(TICK, false, true)
	frame_input.set_weapon_input(true, true, false)
	world.tick(camera.global_position, camera.global_transform, TICK, frame_input)
	presenter.after_world_tick()

	assert_eq(String(world.local_player_weapon_view().anim_key), "anim_wpn_fire",
			"the real FSM entered its fire action")
	for part_v in presenter.vm_parts():
		var part := part_v as ObjectModel
		assert_eq(String(part.get_active_body_clip()), "anim_wpn_fire",
				"the fire event's clip starts on both real parts")
		assert_almost_eq(part.get_animation_time(), 0.0, 0.00001,
				"a current-tick event starts at its production-tick pose")

	# Hold the trigger: the fire action finishes and chains into recoil.
	var part0 := presenter.vm_parts()[0] as ObjectModel
	var reached_recoil := false
	for i in 30:
		frame_input = presenter.before_world_tick(TICK, false, true)
		frame_input.set_weapon_input(true, false, false)
		world.tick(camera.global_position, camera.global_transform, TICK, frame_input)
		presenter.after_world_tick()
		if String(part0.get_active_body_clip()) == "anim_wpn_recoil":
			reached_recoil = true
			break
	assert_true(reached_recoil, "the held trigger chains fire -> recoil on the parts")
	assert_gt(int(world.local_player_weapon_view().play_serial), serial_before,
			"the FSM's play serial advances with the served clips")


func test_weapon_switch_events_are_owned_by_the_presenter_from_setup_to_teardown() -> void:
	# The sim answers category REQUESTS through the event drain
	# (switch_to_weapon); the presenter is the ONE registered fixed-tick
	# consumer from setup to teardown. Attachment is the adoption boundary:
	# pre-setup history is discarded, never replayed; live events reinstall the
	# FP presentation [orig: the ACCEPT re-mount WeaponLoadout_ApplyFromBuffer
	# @0x565cd0 tail -> Player_MountWeaponSlot @0x4dfa40]; teardown releases the
	# world-to-presenter callback.
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	await get_tree().process_frame
	var sim := world.get_sim()

	# Backlog with no consumer: the FSM switches (sim truth), the presentation
	# events accumulate undrained, and the render-side weapon stays put.
	sim.request_local_player_weapon_category(7)
	for i in 45:
		world.tick(camera.global_position, camera.global_transform, TICK)
	assert_eq(String(sim.get_local_player_weapon_name()), "WPN_SATCHEL_CHARGE",
			"the sim's switch walk runs without any presenter")
	assert_eq(String(world.local_player_weapon_name()), "WPN_M4AUTO",
			"the presentation weapon waits for a consumer")

	# setup() discards exactly the pre-attachment history.
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	assert_eq(world.drain_local_player_weapon_events().size(), 0,
			"setup drained the stale backlog")
	assert_eq(String(world.local_player_weapon_name()), "WPN_M4AUTO",
			"discarded history is never replayed into the presentation")

	# Live: switch back to the primary, then to the satchel again — the second
	# leg changes the installed def, so the FP viewmodel is reinstalled.
	sim.request_local_player_weapon_category(3)
	assert_true(_frame_until(world, presenter, camera, func() -> bool:
		return String(sim.get_local_player_weapon_name()) == "WPN_M4AUTO"),
			"the live switch back to the primary completes")
	_frame(world, presenter, camera, 2)
	var vm_before: Node3D = presenter.viewmodel()
	assert_not_null(vm_before)
	sim.request_local_player_weapon_category(7)
	assert_true(_frame_until(world, presenter, camera, func() -> bool:
		return String(world.local_player_weapon_name()) == "WPN_SATCHEL_CHARGE"),
			"the live switch event reinstalls the parent weapon definition")
	_frame(world, presenter, camera, 2)
	assert_not_null(presenter.viewmodel(), "the satchel viewmodel rebuilt")
	assert_ne(presenter.viewmodel(), vm_before,
			"the stale M4 viewmodel is replaced through refresh_viewmodel")
	assert_eq(world.drain_local_player_weapon_events().size(), 0,
			"the live presenter drains every fixed-tick batch")

	# teardown() releases the callback: later events accumulate again and the
	# presentation freezes at its last owned state.
	presenter.teardown()
	sim.request_local_player_weapon_category(3)
	for i in 45:
		world.tick(camera.global_position, camera.global_transform, TICK)
	assert_gt(world.drain_local_player_weapon_events().size(), 0,
			"teardown releases the world-to-presenter callback")
	assert_eq(String(world.local_player_weapon_name()), "WPN_SATCHEL_CHARGE",
			"no presenter, no presentation change")


# --- the real 3P avatar -------------------------------------------------------


func test_local_avatar_body_channel_follows_the_sim_tuple() -> void:
	# The avatar consumes the sim's authoritative body tuple each presentation
	# frame; at spawn that is the idle key at the motor's own playhead. (The
	# death-switch BLEND tuple of the old harness needs a lethal event and is
	# ctest-pinned engine state now.)
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	var sim := world.get_sim()
	_frame(world, presenter, camera, 2)

	var avatar := _local_avatar(world)
	assert_not_null(avatar, "the shared presenter owns the real 3P avatar build")
	assert_eq(String(sim.get_local_player_anim_key()), "anim_idle")
	assert_eq(String(avatar.get_active_body_clip()), "anim_idle",
			"the avatar plays the sim's served body key")
	assert_true(avatar.get_body_blend().is_empty(),
			"a single-key tuple presents with no blend channel")
	assert_almost_eq((avatar.global_position - sim.get_local_player_position()).length(),
			0.0, 0.001, "the avatar node is stamped at the sim position")

	# The posed head bone the eye/aim legs sample [orig: the local bone path
	# @0x4b6bb3] comes from this avatar's real skeleton.
	var head: Vector3 = presenter.avatar_head_world()
	assert_ne(head, Vector3.INF, "the 19-bone rig serves the head-bone sample")
	assert_between(head.y - sim.get_local_player_position().y, 0.3, 1.2,
			"the posed head sits at standing height above the entity position")

	presenter.teardown()
	assert_true(_local_avatar(world) == null or _local_avatar(world).is_queued_for_deletion(),
			"teardown releases the presenter-owned avatar")
