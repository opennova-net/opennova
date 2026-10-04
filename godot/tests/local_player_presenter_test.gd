extends GutTest

# LocalPlayerPresenter over a REAL GameWorld + Simulation (the ADR 0033 typed
# boundary: setup(world: GameWorld, camera: Camera3D, fly_camera: GameplayCamera,
# controls: ControlsModel); scripted movement rides a PlayerMoveIntent).
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
#   the same sim seam the router drives; native player_actions covers capture gates.
# - aim_range on a world with NO terrain object: unreachable on a loaded real
#   world; the raycast-miss -> 1000 fallback covers the same readout behavior.


const TEST_ROOT := "local_player_presenter_test"
var TICK := Simulation.tick_dt()

static var MINIMAL_FIXTURE_DIR := RuntimeFixture.directory()
const TMAP_FIXTURE_DIR := "res://../fixtures/terrain/tmap"
const PERSON_FIXTURE := "res://../fixtures/threedi/synth/person.3di"
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
	# A key a failed assertion left down must not leak into the next case.
	for keycode in [KEY_SHIFT, KEY_CTRL, KEY_7, KEY_Z, KEY_X, KEY_C]:
		_hold(keycode, false)


# Physical key state through Input, flushed so the router's next sample sees it.
func _hold(keycode: Key, pressed: bool) -> void:
	var event := InputEventKey.new()
	event.keycode = keycode
	event.physical_keycode = keycode
	event.pressed = pressed
	Input.parse_input_event(event)
	Input.flush_buffered_events()


# One press of a polled binding row over the live key state (Ctrl held for a
# modified row), a frame down and a frame up.
func _tap_row(world: GameWorld, presenter: LocalPlayerPresenter, camera: Camera3D,
		keycode: Key, ctrl: bool = false) -> void:
	if ctrl:
		_hold(KEY_CTRL, true)
	_hold(keycode, true)
	_frame(world, presenter, camera, 1)
	_hold(keycode, false)
	if ctrl:
		_hold(KEY_CTRL, false)
	_frame(world, presenter, camera, 1)


# --- real-world staging -------------------------------------------------------
# The minimal fixture plus the committed model/anim fixtures arranged under the
# names the production resolvers ask for: a small authored weapon table
# (WPN_M4AUTO with gfx/animadm/pos rows), a person items.def row for the player visual item
# (105310 -> person + soldier.adm), the infantry clip set (E_STAND.adm) so
# the motor's body selection runs, and the 19-bone person staged as the M4's
# FP gun/arms rig with a wpn-key clip set over the committed .bads.

func _stage_root() -> String:
	var root_dir := OS.get_cache_dir().path_join(TEST_ROOT).path_join(
			"root_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir), OK)
	# tmap first (its own items.def loses to minimal's below), then the minimal
	# mission set: one root serves both the mnml TRN and the synthetic Tmap CPT
	# heightfield (the mission header picks the terrain per load).
	for dir in [TMAP_FIXTURE_DIR, MINIMAL_FIXTURE_DIR]:
		var source_dir := ProjectSettings.globalize_path(dir)
		for file_name in DirAccess.get_files_at(source_dir):
			var target := root_dir.path_join(file_name)
			if FileAccess.file_exists(target):
				assert_eq(DirAccess.remove_absolute(target), OK)
			assert_eq(DirAccess.copy_absolute(
					source_dir.path_join(file_name), target), OK)
	# The authored weapon table: WPN_M4AUTO / WPN_SATCHEL_CHARGE with real action rows.
	assert_eq(DirAccess.remove_absolute(root_dir.path_join("weapon.def")), OK)
	assert_eq(DirAccess.copy_absolute(
			DefFixture.directory().path_join("weapon.def"),
			root_dir.path_join("weapon.def")), OK)
	# The player's third-person avatar: items.def person row 105310 (the placer's
	# PLAYER_VISUAL_ITEM_ID) over the committed 19-bone person + soldier.adm.
	# The minimal pack already authors a 105310 row (graphic US01), and a type
	# id resolves to its FIRST row (retail's ItemList_FindIndexByTypeId stops
	# at the first match), so the staged row leads the file.
	var items_path := root_dir.path_join("items.def")
	var base_items := FileAccess.get_file_as_string(items_path)
	var items := FileAccess.open(items_path, FileAccess.WRITE)
	assert_not_null(items, "staged items.def is writable")
	items.store_string("""begin "Player Character"
  id 105310
  type person
  graphic person
  sid player
  anim_def soldier
  hp 100
end

begin "Night Vision Goggles"
  id 101904
  type object
  graphic person
end

""" + base_items)
	items.close()
	# The player's character registry: retail's ONLY first-person arms source is
	# the selected combo's arms part (weapon.def gfx1a is a discarded token), so
	# one good-side combo binds the staged person head/body + the armsG arms.
	var avatars := FileAccess.open(root_dir.path_join("Avatars.def"), FileAccess.WRITE)
	assert_not_null(avatars, "staged Avatars.def is writable")
	avatars.store_string("""define head STAGED_HEAD
{
	graphic person.3di
	camo 0 0 0
	voice 1
	sex m
}
define body STAGED_BODY
{
	graphic person.3di
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
		["person.3di", PERSON_FIXTURE],
		["M4_1st.3di", PERSON_FIXTURE],
		["M4_3RD.3di", PERSON_FIXTURE],
		["armsG.3di", PERSON_FIXTURE],
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
""".replace("\n", "\r\n"))
	adm.close()
	return root_dir


## The production load: packaged world scene + injected root + playable auto-spawn
## with the M4/satchel kit (the armory-proven canonical profile). `baked_terrain`
## swaps the mission onto the Tmap CPT heightfield so the ported terrain raycast
## has a real surface to measure.
func _load_player_world(baked_terrain: bool = false) -> GameWorld:
	var packed := load("res://game/world/game_world.tscn") as PackedScene
	assert_not_null(packed, "the packaged world scene loads")
	var world := packed.instantiate() as GameWorld
	add_child_autofree(world)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(_shared_root), OK)
	world.set_resource_root(root)
	var loadout := PlayerSpawnLoadout.new()
	loadout.primary = "WPN_M4AUTO"
	loadout.accessory = "WPN_SATCHEL_CHARGE"
	loadout.player_class = 8
	world.set_local_player_spawn_loadout(loadout)
	var mission := MissionData.new()
	assert_eq(mission.open_from_resource_root(root, "mnml.bms"), OK)
	if baked_terrain:
		assert_true(mission.set_header_string("terrain", "Tmap"))
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
	presenter.setup(world, camera, null, ControlsModel.new())
	presenter.set_input_override(_move_intent())
	return presenter


# A scripted movement frame (the neutral intent by default): the same seven
# bits the live binding table would produce.
func _move_intent(forward: bool = false, lean_left: bool = false) -> PlayerMoveIntent:
	var intent := PlayerMoveIntent.new()
	intent.forward = forward
	intent.lean_left = lean_left
	return intent


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
	return world.get_node_or_null("PlayerAvatar_person") as ObjectModel


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
	presenter.set_input_override(_move_intent(true))
	assert_true(_frame_until(world, presenter, camera, func() -> bool:
		return String(sim.get_local_player_anim_key()) != "anim_idle"),
			"held forward promotes the body selection off idle")

	# Releasing the source settles the motor back to idle.
	presenter.set_input_override(_move_intent())
	assert_true(_frame_until(world, presenter, camera, func() -> bool:
		return String(sim.get_local_player_anim_key()) == "anim_idle"),
			"releasing input settles the motor back to idle")

	# A live UI overlay keeps the world ticking but submits a NEUTRAL movement
	# frame: the same held source no longer reaches the motor.
	presenter.set_input_override(_move_intent(true))
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
	presenter.set_input_override(_move_intent(false, true))
	_frame(world, presenter, camera, 30)
	var view := world.local_player_view()
	assert_lt(view.camera_roll_deg, -5.0, "held lean-left composes a leftward camera roll")
	assert_lt(camera.global_basis.y.x, -0.01,
			"negative composed roll tilts the stamped view left")
	presenter.set_input_override(_move_intent())
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
		_hold(pair[0], true)
		_frame(world, presenter, camera, 3)
		_hold(pair[0], false)
		_frame(world, presenter, camera)
		assert_eq(int(sim.get_local_player_stance()), int(pair[1]),
				"the sim grants the requested stance for key %d" % pair[0])


func test_overlay_key_latch_and_remapped_wheel_reach_the_sim() -> void:
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var model := ControlsModel.new()
	var rows: Array = model.get_rows(ControlsModel.DEVICE_KEYBOARD)
	var crouch := -1
	for i in rows.size():
		if (rows[i] as PackedStringArray)[1] == "Crouch":
			crouch = model.action_index_for_row(i)
	assert_gte(crouch, 0)
	model.assign_mouse_mask(crouch,
			ControlsModel.mouse_mask_from_godot_button(MOUSE_BUTTON_WHEEL_UP))
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, camera, null, model)
	presenter.set_input_override(_move_intent())
	await get_tree().process_frame
	var sim := world.get_sim()

	_hold(KEY_Z, true)
	_frame(world, presenter, camera, 2, false)
	assert_eq(int(sim.get_local_player_stance()), 0, "an overlay blocks the new prone press")
	_frame(world, presenter, camera, 2)
	assert_eq(int(sim.get_local_player_stance()), 0, "closing it does not re-fire the held key")
	_hold(KEY_Z, false)
	_frame(world, presenter, camera)
	_hold(KEY_Z, true)
	_frame(world, presenter, camera, 3)
	assert_eq(int(sim.get_local_player_stance()), 2, "a fresh press reaches the sim")
	_hold(KEY_Z, false)
	_frame(world, presenter, camera)

	var wheel := InputEventMouseButton.new()
	wheel.button_index = MOUSE_BUTTON_WHEEL_UP
	wheel.pressed = true
	assert_false(presenter.handle_input(wheel, false), "an overlay also blocks wheel requests")
	assert_eq(int(sim.get_local_player_stance()), 2)
	assert_true(presenter.handle_input(wheel, true), "the remapped wheel reaches the native table")
	_frame(world, presenter, camera, 3)
	assert_eq(int(sim.get_local_player_stance()), 1, "the wheel requests crouch without mouse capture")


func test_wheel_factor_accumulates_whole_notches() -> void:
	# A high-resolution wheel reports sub-notch messages (factor = |delta| /
	# WHEEL_DELTA): they add up before any wheel binding fires, and deltas of
	# the other sign cancel inside one notch (controls/player_actions.h
	# WheelRemainder).
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var model := ControlsModel.new()
	var rows: Array = model.get_rows(ControlsModel.DEVICE_KEYBOARD)
	var crouch := -1
	var stand := -1
	for i in rows.size():
		var label := (rows[i] as PackedStringArray)[1]
		if label == "Crouch":
			crouch = model.action_index_for_row(i)
		elif label == "Stand":
			stand = model.action_index_for_row(i)
	assert_gte(crouch, 0)
	assert_gte(stand, 0)
	model.assign_mouse_mask(crouch,
			ControlsModel.mouse_mask_from_godot_button(MOUSE_BUTTON_WHEEL_UP))
	model.assign_mouse_mask(stand,
			ControlsModel.mouse_mask_from_godot_button(MOUSE_BUTTON_WHEEL_DOWN))
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, camera, null, model)
	presenter.set_input_override(_move_intent())
	await get_tree().process_frame
	var sim := world.get_sim()
	var wheel := func(button: MouseButton, factor: float) -> bool:
		var event := InputEventMouseButton.new()
		event.button_index = button
		event.pressed = true
		event.factor = factor
		return presenter.handle_input(event, true)

	assert_true(wheel.call(MOUSE_BUTTON_WHEEL_UP, 0.5), "a sub-notch message is consumed")
	_frame(world, presenter, camera, 3)
	assert_eq(int(sim.get_local_player_stance()), 0, "half a notch dispatches nothing")
	assert_true(wheel.call(MOUSE_BUTTON_WHEEL_UP, 0.5))
	_frame(world, presenter, camera, 3)
	assert_eq(int(sim.get_local_player_stance()), 1, "the second half completes one crouch notch")
	assert_true(wheel.call(MOUSE_BUTTON_WHEEL_DOWN, 0.4))
	_frame(world, presenter, camera, 3)
	assert_eq(int(sim.get_local_player_stance()), 1, "a partial reverse notch dispatches nothing")
	assert_true(wheel.call(MOUSE_BUTTON_WHEEL_DOWN, 0.6))
	_frame(world, presenter, camera, 3)
	assert_eq(int(sim.get_local_player_stance()), 0, "the reverse deltas complete one stand notch")


# While the NVG composite is up the world pass is the NVG scene: the world
# renders once, into the NVG raster (retail's 512 square at the frame's own
# frustum, non-square texels; engine world::nvg_view_projection, served
# through TargetProjectionXrInterface), the surface's own 3D pass off, and the
# target goes away with NVG. [orig: Render_ProcessMainSceneFrame
# @0x5ca516..0x5ca5b0; NVG_RenderScene @0x5d2954..0x5d296d]
func test_nvg_composite_renders_the_world_into_the_nvg_raster() -> void:
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	var size := camera.get_viewport().get_visible_rect().size
	if size.x <= 0.0 or size.y <= 0.0:
		pending("the headless viewport reports no size, so no raster can be pinned")
		return
	var sim := world.get_sim()
	sim.set_local_player_aspect_mode(-1)
	_frame(world, presenter, camera, 2)
	assert_null(presenter.projection_viewport(), "a native mode draws the surface directly")
	assert_false(presenter.is_nvg_raster_active())
	assert_true(sim.request_local_player_nvg_toggle())
	_frame(world, presenter, camera, 2)
	assert_true(presenter.is_nvg_raster_active(), "the NVG composite takes the world pass")
	var target: SubViewport = presenter.projection_viewport()
	var through: Camera3D = presenter.projection_camera()
	assert_not_null(target)
	assert_not_null(through)
	if target == null or through == null:
		return
	assert_eq(target.size, Vector2i(512, 512), "retail's 512 square, its columns included")
	assert_true(target.use_xr, "the frame's frustum is served over the square")
	assert_true(TargetProjectionXrInterface.is_serving(target))
	assert_almost_eq(through.fov, 80.0, 0.001, "the frame's horizontal fov")
	assert_eq(through.keep_aspect, Camera3D.KEEP_WIDTH)
	var aspect := size.x / size.y
	assert_eq(target.size_2d_override, Vector2i(roundi(512.0 * aspect), 512),
			"the camera node's own frame keeps the frustum's aspect")
	# The raster's matrix: the horizontal fov across the 512 columns, the
	# frame's vertical half-extent across the 512 rows.
	var projection: Projection = presenter.view_projection()
	var half_h := tan(deg_to_rad(40.0))
	assert_eq(projection, TargetProjectionXrInterface.served_projection(target),
			"the frame projection is the served one")
	assert_almost_eq(projection.x.x, 1.0 / half_h, 0.0001, "proj[0][0] = cot(fov_h/2)")
	assert_almost_eq(projection.y.y, aspect / half_h, 0.0001,
			"proj[1][1] = aspect / tan(fov_h/2): non-square texels on the square")
	# A vertical edge 20 degrees right of the view axis lands on retail's
	# column 256 + 256 tan(20) / tan(40) of the 512.
	var angle := deg_to_rad(20.0)
	var clip := projection * Vector4(tan(angle), 0.0, -1.0, 1.0)
	assert_almost_eq((clip.x / clip.w * 0.5 + 0.5) * 512.0,
			256.0 + 256.0 * tan(angle) / half_h, 0.01, "the edge's raster column")
	assert_true(camera.get_viewport().disable_3d,
			"one world render a frame: the surface's own 3D pass is off")
	assert_false(sim.request_local_player_nvg_toggle())
	_frame(world, presenter, camera, 2)
	assert_false(presenter.is_nvg_raster_active())
	assert_null(presenter.projection_viewport(), "NVG off releases the raster")
	assert_false(TargetProjectionXrInterface.is_serving(target),
			"and stops serving it")
	assert_false(camera.get_viewport().disable_3d)


# The binocular, NVG and NVG-gain rows over the live binding table: B and N
# bare, the gain rows behind their Ctrl modifier, so a bare '=' stays the
# radarin row alone [orig: rows 103/104/45/46 -> dispatch 26 / 41 / 56 / 57].
func test_binoculars_nvg_and_gain_rows_route_retail_actions() -> void:
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame

	_tap_row(world, presenter, camera, KEY_B)
	assert_true(_frame_until(world, presenter, camera, func() -> bool:
		return world.local_player_view().binoculars_view_active),
			"B raises the binocular view through the sim's own ease")
	_tap_row(world, presenter, camera, KEY_B)
	assert_true(_frame_until(world, presenter, camera, func() -> bool:
		return not world.local_player_view().binoculars_view_active),
			"a second B lowers the binoculars again")

	_tap_row(world, presenter, camera, KEY_N)
	_frame(world, presenter, camera, 2)
	var view := world.local_player_view()
	assert_true(view.nvg_active, "N toggles the sim's NVG state")
	assert_eq(view.nvg_gain, 0, "NVG starts at the base gain step")
	var sim := world.get_sim()
	var zoom := sim.get_hud_radar_zoom_q16()
	_tap_row(world, presenter, camera, KEY_EQUAL)
	assert_eq(world.local_player_view().nvg_gain, 0, "a bare '=' leaves the NVG gain")
	assert_lt(sim.get_hud_radar_zoom_q16(), zoom, "a bare '=' is radarin")
	zoom = sim.get_hud_radar_zoom_q16()
	_tap_row(world, presenter, camera, KEY_EQUAL, true)
	_frame(world, presenter, camera, 1)
	assert_eq(world.local_player_view().nvg_gain, 1, "Ctrl+'=' steps the gain up")
	assert_eq(sim.get_hud_radar_zoom_q16(), zoom, "Ctrl+'=' does not zoom the radar")
	_tap_row(world, presenter, camera, KEY_MINUS, true)
	_tap_row(world, presenter, camera, KEY_MINUS, true)
	_frame(world, presenter, camera, 1)
	assert_eq(world.local_player_view().nvg_gain, 0,
			"Ctrl+'-' steps the gain down and the sim clamps at the floor")


# The local avatar's goggles (retail draw 3) follow the body's first-person rule:
# the camera-tracked gate returns before every draw after the canopy
# (BoneCallback_org0_World, the gate @0x4e3ab3..0x4e3aca after the canopy submit
# @0x4e3aa5), so in first person they leave the camera by LAYER and keep casting
# into the render slot; in third person they draw. The held weapon draws at its
# avatar's first (head) submit level, never through its own threshold walk
# (@0x4e3ce1..0x4e3cf2).
func test_local_avatar_overlays_follow_the_first_person_rule() -> void:
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	_frame(world, presenter, camera, 2)
	var avatar := _local_avatar(world)
	assert_not_null(avatar)
	var held: ObjectModel = presenter.held_weapon()
	assert_not_null(held, "the kit's held weapon is built")
	if held != null:
		var owner := held.get_authored_lod_owner()
		assert_true(owner == avatar or (owner != null and owner.get_parent() == avatar),
				"the held weapon's RLOD owner is the avatar's first submit part")

	_tap_row(world, presenter, camera, KEY_N)
	_frame(world, presenter, camera, 1)
	var overlays: PersonOverlayModels = presenter.person_overlays()
	var nvg := overlays.get_node(PersonOverlayModels.KIND_NVG,
			PersonOverlayModels.PASS_FIRST) as ObjectModel
	assert_not_null(nvg, "worn goggles build their item model")
	if nvg == null:
		return
	assert_true(nvg.visible, "the goggles stay a live shadow source in first person")
	var fp_mask := 0
	for vi in _visual_instances(nvg):
		fp_mask |= (vi as VisualInstance3D).layers
	assert_ne(fp_mask & Water.VISUAL_LAYER_FP_BODY_SHADOW_ONLY, 0,
			"first person hides the goggles from the camera by layer")
	assert_eq(fp_mask & Water.VISUAL_LAYER_WORLD, 0, "and keeps them off the world layer")
	presenter.set_debug_third_person(true)
	_frame(world, presenter, camera, 2)
	var tp_mask := 0
	for vi in _visual_instances(nvg):
		tp_mask |= (vi as VisualInstance3D).layers
	assert_ne(tp_mask & Water.VISUAL_LAYER_WORLD, 0, "third person draws the goggles")
	_tap_row(world, presenter, camera, KEY_N)
	_frame(world, presenter, camera, 1)
	assert_false(nvg.visible, "removed goggles hide")


# The router's digit rows over the LIVE binding table and key state, on a row
# that samples without mouse capture (the weapon-category rows need it and
# headless has no cursor to capture): radarout rebound to 7 fires on a bare 7
# (the dispatcher's fallback pass), a 7 pressed while USE (the `useitem` row,
# default Shift) is held is the special-key seat chord and reaches no row,
# Ctrl+7 is the seat7 row alone (the modifier pass claims the key), and the
# same bare 7 afterwards fires again.
# [orig: Input_HandleSpecialKeys @0x49c6d8..0x49c730;
#  Input_ProcessKeyboardEvents @0x49d327..0x49d3ac (modifier pass),
#  @0x49d3ba..0x49d488 (fallback)]
func test_use_hold_swallows_digit_rows_and_ctrl_digit_claims_them() -> void:
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var model := ControlsModel.new()
	var radar_out := -1
	var rows: Array = model.get_rows(ControlsModel.DEVICE_KEYBOARD)
	for i in rows.size():
		if (rows[i] as PackedStringArray)[1] == "Radar Zoom Out":
			radar_out = model.action_index_for_row(i)
	assert_gte(radar_out, 0, "the radarout row is in the table")
	assert_true(model.assign_godot_key(radar_out, KEY_7, false), "radarout takes 7 as its second key")
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, camera, null, model)
	presenter.set_input_override(_move_intent())
	await get_tree().process_frame
	_frame(world, presenter, camera, 2)
	var sim := world.get_sim()
	var zoom := sim.get_hud_radar_zoom_q16()

	_hold(KEY_7, true)
	_frame(world, presenter, camera, 2)
	assert_gt(sim.get_hud_radar_zoom_q16(), zoom, "a bare 7 fires the rebound radarout row")
	_hold(KEY_7, false)
	_frame(world, presenter, camera, 2)
	zoom = sim.get_hud_radar_zoom_q16()

	_hold(KEY_SHIFT, true)
	_frame(world, presenter, camera, 2)  # the hold must have been live LAST frame
	_hold(KEY_7, true)
	_frame(world, presenter, camera, 3)
	assert_eq(sim.get_hud_radar_zoom_q16(), zoom,
			"a digit under the USE hold is the seat chord and reaches no row")
	_hold(KEY_7, false)
	_hold(KEY_SHIFT, false)
	_frame(world, presenter, camera, 2)
	assert_eq(sim.get_hud_radar_zoom_q16(), zoom, "releasing the hold fires nothing")

	_hold(KEY_CTRL, true)
	_hold(KEY_7, true)
	_frame(world, presenter, camera, 3)
	assert_eq(sim.get_hud_radar_zoom_q16(), zoom,
			"Ctrl+7 is the seat7 row: the modifier pass claims the key")
	_hold(KEY_7, false)
	_hold(KEY_CTRL, false)
	_frame(world, presenter, camera, 2)

	_hold(KEY_7, true)
	_frame(world, presenter, camera, 2)
	assert_gt(sim.get_hud_radar_zoom_q16(), zoom, "the same bare 7 fires again")
	_hold(KEY_7, false)
	_frame(world, presenter, camera, 2)


# The router's menu arms: while the HUD's F9 Emotes menu is open a digit is its
# pick, closing the menu and reaching no row (radarout rebound to 7 stays put);
# once the menu is closed the same 7 fires the row again.
# [orig: Input_HandleSpecialKeys @0x49c731..0x49c77c; the consumed key skips
#  the binding scan, Input_ProcessKeyboardEvents @0x49d2fb]
func test_open_voice_menu_takes_the_digit_before_the_rows() -> void:
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var model := ControlsModel.new()
	var radar_out := -1
	var rows: Array = model.get_rows(ControlsModel.DEVICE_KEYBOARD)
	for i in rows.size():
		if (rows[i] as PackedStringArray)[1] == "Radar Zoom Out":
			radar_out = model.action_index_for_row(i)
	assert_gte(radar_out, 0, "the radarout row is in the table")
	assert_true(model.assign_godot_key(radar_out, KEY_7, false), "radarout takes 7 as its second key")
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, camera, null, model)
	presenter.set_input_override(_move_intent())
	var toggles := HudToggles.new()
	presenter.set_hud_toggles(toggles)
	await get_tree().process_frame
	_frame(world, presenter, camera, 2)
	var sim := world.get_sim()
	var zoom := sim.get_hud_radar_zoom_q16()
	# F9's row opens the menu (the HUD presenter's poll, here by hand).
	toggles.poll(1 << HudToggles.ROW_AUDIO_EMOTE, false, false, true, false, false)
	toggles.poll(0, false, false, true, false, false)
	assert_true(toggles.is_emotes_menu_open(), "the emotes menu is open")

	_hold(KEY_7, true)
	_frame(world, presenter, camera, 2)
	assert_false(toggles.is_emotes_menu_open(), "the digit picks and closes the menu")
	assert_eq(sim.get_hud_radar_zoom_q16(), zoom, "the picked digit reaches no row")
	_hold(KEY_7, false)
	_frame(world, presenter, camera, 2)

	_hold(KEY_7, true)
	_frame(world, presenter, camera, 2)
	assert_gt(sim.get_hud_radar_zoom_q16(), zoom, "with the menu closed the 7 fires the row")
	_hold(KEY_7, false)
	_frame(world, presenter, camera, 2)


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
	# motor resolves the retail head anchor, including its .15 up/.10 forward
	# offset, before publishing CameraOffset. No render-to-simulation feedback.
	var head := sim.get_local_player_position() + sim.get_local_player_eye_offset()
	assert_gt(sim.get_local_player_eye_offset().y, 0.0, "the motor serves a posed head")
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


func test_aspect_mode_draws_through_a_target_of_the_selected_ratio() -> void:
	# The frame's projection keeps the policy HORIZONTAL fov across the real
	# width in every aspect mode; the vertical half-extent follows the
	# SELECTED ratio (docs/mnu/menu-re.md, 16x9DISPLAY; engine
	# world::view_projection). A mode off the surface's ratio therefore draws
	# through a target of the selected aspect stretched onto the surface,
	# never through a widened camera fov.
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	var sim := world.get_sim()
	var size := camera.get_viewport().get_visible_rect().size
	if size.x <= 0.0 or size.y <= 0.0:
		pending("the headless viewport reports no size, so no projection can be pinned")
		return
	var surface_ratio := size.y / size.x
	# Retail's stock mode 0 (4:3) unless the surface is 4:3 itself, then 16:9.
	var mode := 0 if absf(surface_ratio - 0.75) > 0.001 else 2
	var selected := 0.75 if mode == 0 else 0.5625
	var stretch := selected / surface_ratio
	var half_h := tan(deg_to_rad(40.0))
	sim.set_local_player_aspect_mode(mode)
	_frame(world, presenter, camera, 2)
	var view := world.local_player_view()
	assert_almost_eq(view.fov_h_deg, 80.0, 0.001, "the unscoped policy fov is 80")

	assert_almost_eq(presenter.projection_scale_y(), stretch, 0.001,
			"the frame stretches selected/(h/w) onto the surface")
	var target: SubViewport = presenter.projection_viewport()
	assert_not_null(target, "a mode off the surface ratio draws through a target")
	var through: Camera3D = presenter.projection_camera()
	assert_not_null(through, "the target has its own drawing camera")
	if target == null or through == null:
		return
	var expected_size := Vector2i(int(size.x), roundi(size.x * selected))
	if stretch < 1.0:
		expected_size = Vector2i(roundi(size.y / selected), int(size.y))
	assert_eq(target.size, expected_size,
			"the target has the selected aspect and never undersamples the surface")
	assert_almost_eq(through.fov, 80.0, 0.001, "the target camera keeps the horizontal fov")
	assert_eq(through.keep_aspect, Camera3D.KEEP_WIDTH)
	assert_true(through.global_transform.is_equal_approx(camera.global_transform),
			"the target camera mirrors the gameplay pose")
	assert_true(camera.get_viewport().disable_3d,
			"the surface's own 3D draw yields to the blitted target")
	# The viewport_debug_draw row writes the surface; the world it names is
	# the target's.
	var surface := camera.get_viewport()
	var previous_draw := surface.debug_draw
	surface.debug_draw = Viewport.DEBUG_DRAW_WIREFRAME
	_frame(world, presenter, camera, 1)
	assert_eq(target.debug_draw, Viewport.DEBUG_DRAW_WIREFRAME,
			"the target mirrors the surface's debug draw")
	surface.debug_draw = previous_draw
	_frame(world, presenter, camera, 1)
	var projection: Projection = presenter.view_projection()
	assert_almost_eq(projection.x.x, 1.0 / half_h, 0.001,
			"proj[0][0] = cot(fov_h/2) across the real width")
	# The target is an integer-sized viewport, so its ratio meets the selected
	# one to within a pixel (a 64x64 headless surface rounds 85.3 to 85).
	var target_ratio := float(target.size.y) / float(target.size.x)
	assert_true(absf(target_ratio - selected) <= 1.0 / float(target.size.x),
			"the target ratio is the selected ratio within one pixel")
	assert_almost_eq(projection.y.y, 1.0 / (half_h * target_ratio), 0.001,
			"proj[1][1] = 1/(tan(fov_h/2)*ratio) across the target height")
	# The gameplay camera carries the culling superset: the vertical fov of the
	# selected ratio under the surface's wider horizontal when the selected
	# ratio is the taller, else the horizontal fov under the surface's taller
	# vertical.
	if stretch > 1.0:
		assert_eq(camera.keep_aspect, Camera3D.KEEP_HEIGHT)
		assert_almost_eq(camera.fov,
				Simulation.fov_vertical_from_horizontal(80.0, size.x / size.y, mode), 0.001,
				"the gameplay camera keeps the selected vertical fov")
	else:
		assert_eq(camera.keep_aspect, Camera3D.KEEP_WIDTH)
		assert_almost_eq(camera.fov, 80.0, 0.001, "the gameplay camera keeps the horizontal fov")
	# The FP fold's focal ratio is the ratio of the two horizontal
	# half-tangents, whatever the surface: sweep the renderfov tunable to 60.
	var rig: PlayerViewmodelRig = presenter.viewmodel_rig()
	rig.set_player_viewmodel_renderfov_h_deg(60.0)
	_frame(world, presenter, camera, 1)
	if rig.projection_feed() == Vector4.ZERO:
		pending("no FP viewmodel built, so no projection feed to pin")
	else:
		assert_almost_eq(rig.projection_feed().x, half_h / tan(deg_to_rad(30.0)), 0.001,
				"the renderfov focal ratio is aspect-invariant")

	# Back to the surface's own ratio: the target goes away and the surface
	# draws directly through the shared conversion.
	sim.set_local_player_aspect_mode(-1)
	_frame(world, presenter, camera, 1)
	assert_null(presenter.projection_viewport(), "a native mode releases the target")
	assert_almost_eq(presenter.projection_scale_y(), 1.0, 0.0001)
	assert_false(camera.get_viewport().disable_3d, "the surface draws its own 3D again")
	assert_eq(camera.keep_aspect, Camera3D.KEEP_HEIGHT)
	assert_almost_eq(camera.fov,
			Simulation.fov_vertical_from_horizontal(view.fov_h_deg, size.x / size.y), 0.001,
			"the native camera fov is the shared conversion at the surface aspect")


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
	assert_ne(camera.cull_mask & Water.VISUAL_LAYER_VIEWMODEL, 0,
			"setup() keeps the viewmodel layer on the player camera: the gun draws"
			+ " inside the beauty pass through its shader-side projection")
	assert_eq(camera.cull_mask & Water.VISUAL_LAYER_SHADOW_CASTER_MASK, 0,
			"the gameplay camera excludes the caster marker layers (the"
			+ " camera-renderable hidden body must not leak through them)")
	var rig: PlayerViewmodelRig = presenter.viewmodel_rig()
	# The witnessed FP projection rides one shader global: the renderfov focal
	# ratio against the live beauty projection, the 0.05 near swap, the far
	# plane [orig: @0x4dee29; Render_SetViewportDepth01 @0x58a7b0].
	var feed := rig.projection_feed()
	if feed == Vector4.ZERO:
		pending("the headless viewport reports no size, so no projection feed was pushed")
	else:
		assert_almost_eq(feed.y, 0.05, 0.0001,
				"the FP near plane is the witnessed 0.05 swap [orig: @0x4dee29]")
		assert_gt(feed.x, 0.0, "the renderfov focal ratio is positive")
		assert_almost_eq(feed.z, camera.far, 0.001, "the far plane is the camera's")

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
		assert_eq(vi.layers & ~Water.VISUAL_LAYER_SHADOW_CASTER_MASK,
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
		assert_eq(vi.layers & ~Water.VISUAL_LAYER_SHADOW_CASTER_MASK,
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
				"the viewmodel layer clears every shadow-caster marker")
		if vi is GeometryInstance3D:
			var geometry := vi as GeometryInstance3D
			assert_eq(geometry.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_OFF,
					"the viewmodel never casts into the world")
			assert_true(bool(geometry.get_instance_shader_parameter("u_viewmodel_pass")),
					"every gun instance applies the renderfov projection + depth band")
			assert_almost_eq(geometry.extra_cull_margin, 8.0, 0.001,
					"the cull margin keeps the eye inside every gun part's box")

	presenter.set_debug_third_person(true)
	_frame(world, presenter, camera, 1)
	for vi in _visual_instances(avatar):
		assert_eq(vi.layers & ~Water.VISUAL_LAYER_SHADOW_CASTER_MASK,
				Water.VISUAL_LAYER_WORLD,
				"third person: the body returns to the normal world layer")
		if vi is GeometryInstance3D:
			assert_eq(vi.cast_shadow, GeometryInstance3D.SHADOW_CASTING_SETTING_ON,
					"third person: body geometry renders and casts normally")
	if held_weapon != null:
		for vi in _visual_instances(held_weapon):
			assert_eq(vi.layers & ~Water.VISUAL_LAYER_SHADOW_CASTER_MASK,
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
		assert_eq(vi.layers & ~Water.VISUAL_LAYER_SHADOW_CASTER_MASK,
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
func test_shared_presenter_teardown_releases_captured_mouse() -> void:
	var world := _bare_world()
	var camera := Camera3D.new()
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(world)
	add_child_autofree(camera)
	add_child_autofree(presenter)
	presenter.setup(world, camera, null, ControlsModel.new())

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


# OpenNova's magazine registers as world::fp_magazine_registers derives them
# from the view's live clip and the def's clip size (D-3DI-7): the spent share
# in 16.16 and round k from the top spent once the clip holds fewer than k.
func _assert_magazine_registers(ctrl: Dictionary, view: PlayerWeaponView) -> void:
	var capacity := int(view.clip_capacity)
	var clip := clampi(int(view.clip), 0, maxi(capacity, 0))
	assert_eq(int(ctrl.get("WPN_SPENT", -999)),
			((capacity - clip) * 0x10000) / capacity if capacity > 0 else 0,
			"WPN_SPENT is the clip's spent share")
	for k in range(1, 9):
		assert_eq(int(ctrl.get("WPN_ROUND_%d" % k, -999)),
				0x10000 if capacity > 0 and clip < k else 0,
				"WPN_ROUND_%d steps with the clip" % k)


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
		# OpenNova's magazine registers ride the same submit, gun and arms
		# alike (world::fp_magazine_registers, D-3DI-7).
		_assert_magazine_registers(ctrl, weapon_view)
	# The arms part alone carries the character's raw camo triplet, stored by
	# the same per-submit writer family (Avatar_SetArmsCamoCtrl before the arms
	# submit); the gun part never does.
	var arms_part := presenter.vm_parts()[0] as ObjectModel
	var gun_part := presenter.vm_parts()[1] as ObjectModel
	assert_eq(arms_part.avatar_part, ObjectModel.AVATAR_PART_ARMS,
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
	_tap_row(world, presenter, camera, KEY_B)
	assert_true(_frame_until(world, presenter, camera, func() -> bool:
		return world.local_player_view().binoculars_view_active))
	_frame(world, presenter, camera, 1)
	assert_false(presenter.viewmodel().visible,
			"the binocular view replaces the FP viewmodel for the frame")
	assert_true(part0.get_ctrl_values().is_empty(),
			"the binocular card path suppresses the FP CTRL writers too")


func test_the_world_only_capture_latch_hides_the_viewmodel_without_a_frame() -> void:
	# A render fixture freezes the shell before it captures, so the latch must
	# apply to the node at once instead of waiting for the next per-frame
	# viewmodel update; releasing it restores the last submission verdict.
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	_frame(world, presenter, camera, 2)
	assert_true(presenter.viewmodel().visible, "the FP viewmodel submits")

	presenter.set_viewmodel_capture_hidden(true)
	assert_false(presenter.viewmodel().visible,
			"a world-only capture hides the gun immediately")
	presenter.set_viewmodel_capture_hidden(false)
	assert_true(presenter.viewmodel().visible,
			"releasing the latch shows it again without a frame")

	# The frozen-fixture path: the camera moves without a frame, and the
	# restamp seam re-places the gun at it (the fold draws the world pose).
	var before: Transform3D = presenter.viewmodel().global_transform
	var old_relative: Transform3D = camera.global_transform.affine_inverse() * before
	camera.global_transform = Transform3D(Basis.from_euler(Vector3(0.0, PI * 0.5, 0.0)),
			camera.global_position + Vector3(40.0, 0.0, 0.0))
	assert_eq(presenter.viewmodel().global_transform, before,
			"without a frame the gun stays where the last pass put it")
	presenter.restamp_viewmodel_at_camera()
	var relative: Transform3D = camera.global_transform.affine_inverse() \
			* presenter.viewmodel().global_transform
	assert_false(presenter.viewmodel().global_transform == before,
			"the restamp moves the gun with the camera")
	assert_true(relative.origin.distance_to(old_relative.origin) < 0.01,
			"the restamped gun keeps its camera-relative offset at the moved camera")


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

	# The consumed round moves OpenNova's magazine registers on both parts on
	# the next submit (D-3DI-7).
	_frame(world, presenter, camera, 1)
	var view: PlayerWeaponView = world.local_player_weapon_view()
	assert_eq(int(view.clip_capacity), 30, "the M4 carries a 30-round clip")
	assert_lt(int(view.clip), 30, "the trigger pull consumed rounds")
	for part_v in presenter.vm_parts():
		var ctrl: Dictionary = (part_v as ObjectModel).get_ctrl_values()
		_assert_magazine_registers(ctrl, view)
		assert_gt(int(ctrl.get("WPN_SPENT", 0)), 0, "the spent share moved off full")


func test_fire_mode_change_keeps_one_posed_viewmodel_per_frame() -> void:
	await _assert_fire_mode_change_frames(1)


func test_fire_mode_change_keeps_the_pose_during_catch_up() -> void:
	await _assert_fire_mode_change_frames(4)


# Exercise the real frame boundary, including a switch in the middle of a
# multi-tick batch. The outgoing model must retire immediately and its
# replacement must be placed and posed before the frame reaches rendering.
func _assert_fire_mode_change_frames(ticks_per_frame: int) -> void:
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	await get_tree().process_frame
	_frame(world, presenter, camera, 2)
	var sim := world.get_sim()
	var previous_model := presenter.viewmodel()
	var starting_weapon := String(sim.get_local_player_weapon_name())
	sim.request_local_player_weapon_category(3)
	var switched := false
	for frame in 45:
		var frame_dt := TICK * float(ticks_per_frame)
		var frame_input := presenter.before_world_tick(frame_dt, false, true)
		world.tick(camera.global_position, camera.global_transform, frame_dt, frame_input)
		presenter.after_world_tick()
		var model := presenter.viewmodel()
		assert_not_null(model, "frame %d retains the first-person weapon" % frame)
		if is_instance_valid(previous_model) and model != previous_model:
			assert_false(previous_model.is_visible_in_tree(),
					"the retired viewmodel stops drawing before its replacement appears")
		var view := world.local_player_weapon_view()
		assert_ne(view.current_action, 6, "fire-mode changes never enter SWITCHTO")
		assert_ne(view.next_action, 6, "fire-mode changes never queue SWITCHTO")
		assert_eq(presenter.vm_parts().size(), 2, "the arms and gun remain present")
		for part in presenter.vm_parts():
			var clip := String(view.anim_key) if not String(view.anim_key).is_empty() else "anim_wpn_idle"
			assert_eq(String(part.get_active_body_clip()), clip,
					"frame %d renders the current weapon animation" % frame)
			var animation := part.get_skeletal_anim()
			var phase: float = animation.get_clip_phase_seconds(
					clip, view.anim_advance_ticks, view.anim_variant)
			assert_almost_eq(part.get_animation_time(), phase, 0.00001,
					"frame %d preserves the simulation animation phase" % frame)
		previous_model = model
		switched = switched or String(sim.get_local_player_weapon_name()) != starting_weapon
	assert_true(switched, "reselecting the rifle category changes its fire mode")
	assert_eq(world.local_player_weapon_view().next_action, 0,
			"an in-place fire-mode change does not queue a draw animation")


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
	assert_false(avatar.has_body_blend(),
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


func test_camera_direction_keeps_precision_far_from_origin() -> void:
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	# The training tank is hundreds of units from the origin. Adding a unit
	# aim vector to that float position before look_at quantizes its direction.
	for distance in [800.0, 8192.0]:
		for offset in [0.0, 0.03125, 0.0625]:
			assert_eq(world.get_sim().debug_teleport_local_player(
					Vector3(distance + offset, -distance, 10.0), 123.456, 7.891), OK)
			presenter.after_world_tick()
			var view := presenter.presented_view()
			var expected := _forward_for(view.camera_yaw_deg, view.camera_pitch_deg)
			assert_lt((-camera.global_basis.z - expected).length(), 0.000001,
					"camera orientation is independent of world-coordinate magnitude")
	presenter.teardown()


# Straight up or straight down the view keeps its heading: the basis is the
# composed yaw / pitch / roll rotation itself, which has no pole, never a
# look-at along the up axis (whose right vector degenerates there)
# [orig: Viewport_BuildProjectionMatrix @0x410FB0 -- the view's three axis
#  rotations @0x4112A4..0x4112EB].
func test_camera_keeps_its_heading_looking_straight_up_or_down() -> void:
	var world := _load_player_world()
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var presenter := _attach_presenter(world, camera)
	for pitch in [90.0, -90.0]:
		for yaw in [0.0, 30.0, 135.0]:
			assert_eq(world.get_sim().debug_teleport_local_player(
					Vector3(256.0, 256.0, 40.0), yaw, pitch), OK)
			presenter.after_world_tick()
			var view := presenter.presented_view()
			assert_almost_eq(view.camera_pitch_deg, pitch, 0.001, "the view looks along the pole")
			assert_almost_eq(view.camera_roll_deg, 0.0, 0.001, "a calm body composes no roll")
			var heading := deg_to_rad(view.camera_yaw_deg)
			assert_lt((camera.global_basis.x - Vector3(cos(heading), 0.0, sin(heading))).length(),
					0.00001, "the camera's right keeps the composed heading at the pole")
			assert_lt((-camera.global_basis.z - Vector3(0.0, signf(pitch), 0.0)).length(),
					0.00001, "the camera looks straight along the pole")
	presenter.teardown()
