extends GutTest

# The scenario driver's scripted input device (ScriptedInput over engine
# devtools/scripted_input.h): the shared script's JSON shape, the retail
# action code -> catalog token map, the device seam (ControlsModel reads a
# scripted token held like a keyboard slot) and the input router playing a
# script on the logic tick through the ordinary binding scan, PlayerActions'
# gates and the mouse-motion path, over a real minimal world. The engine
# schedule itself (multi-tick frames, one flip per token per sample, the
# setup pose's conversion) is the scripted_input ctest.

var TICK := Simulation.tick_dt()
var _root := ""
var _world: GameWorld
var _presenter: LocalPlayerPresenter


func after_each() -> void:
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)
	if _presenter != null:
		_presenter.teardown()
		_presenter = null
	if _world != null:
		_world.unload()
		_world = null
	if not _root.is_empty():
		TestFs.remove_dir_recursive(_root)
		_root = ""


func test_load_steps_marshals_the_shared_script_shape() -> void:
	var refused: Array = [
		[["walk"], "step 0: not an object"],
		[[{"down": 152}], "step 0: \"tick\" must be an integer"],
		[[{"tick": 1.5, "down": 152}], "step 0: \"tick\" must be an integer"],
		[[{"tick": 0, "down": 152, "up": 152}], "step 0: needs exactly one"],
		[[{"tick": 0}], "step 0: needs exactly one"],
		[[{"tick": 0, "press": "149"}], "step 0: \"press\" must be an action code"],
		[[{"tick": 0, "look_px": [3]}], "step 0: \"look_px\" must be [dx, dy] numbers"],
		[[{"tick": 0, "end": false}], "step 0: \"end\" must be true"],
		[[{"tick": 0, "press": 9999}, {"tick": 2, "end": true}], "action code 9999"],
		[[{"tick": 0, "press": 149}], "no end step"],
	]
	for case in refused:
		var driver := ScriptedInput.new()
		assert_eq(driver.load_steps(case[0]), ERR_INVALID_PARAMETER, "refused: %s" % [case[0]])
		assert_string_contains(driver.get_error(), case[1])
		assert_eq(driver.get_state_name(), "idle", "a refused script loads nothing")
		assert_false(driver.start(), "nothing to start")

	# JSON carries every number as a double; integral ones are integers, and
	# keys beside the step's one action are ignored.
	var driver := ScriptedInput.new()
	assert_eq(driver.load_steps([
		{"tick": 3.0, "press": 149.0, "note": "fire once"},
		{"tick": 4, "look_px": [12.0, -3]},
		{"tick": 6.0, "end": true},
	]), OK, driver.get_error())
	assert_eq(driver.get_error(), "")
	assert_eq(driver.get_state_name(), "loaded")
	assert_eq(driver.get_applied_logic_ticks(), PackedInt64Array([-1, -1, -1]),
			"one pending record per source step")


func test_action_codes_resolve_to_the_catalog_rows_they_dispatch() -> void:
	var rows := {
		152: "move_forward", 151: "move_back", 156: "strafe_left", 157: "strafe_right",
		153: "move_jump", 148: "LeanRoll_left", 147: "LeanRoll_right", 149: "attack_1",
		211: "magazine", 201: "Knife", 205: "FragGrenade", 177: "useitem", 182: "seat1",
		191: "seat10", 169: "Crouch", 170: "Prone", 172: "Stand", 212: "cycleweaponP",
		214: "cycleweaponN",
	}
	for code in rows:
		assert_eq(ScriptedInput.token_for_action_code(code), rows[code], "code %d" % code)
	assert_eq(ScriptedInput.token_for_action_code(9999), "", "no row dispatches 9999")


# The device seam: a held scripted token reads held through is_token_pressed,
# like a keyboard slot (an open chat line silences it), with no VK behind it.
func test_the_binding_seam_reads_scripted_tokens_held() -> void:
	var model := ControlsModel.new()
	var driver := ScriptedInput.new()
	assert_eq(driver.load_steps([
		{"tick": 0, "down": 152},
		{"tick": 0, "down": 149},
		{"tick": 2, "up": 152},
		{"tick": 4, "end": true},
	]), OK, driver.get_error())
	model.set_scripted_input(driver)
	assert_same(model.get_scripted_input(), driver)
	assert_false(model.is_token_pressed("move_forward"), "a loaded script holds nothing")
	assert_true(driver.start())
	driver.advance(40)
	assert_eq(driver.get_state(), ScriptedInput.STATE_RUNNING)
	assert_eq(driver.get_start_logic_tick(), 40, "the first sample latches the start")
	assert_true(model.is_token_pressed("move_forward"), "the scripted forward reads held")
	assert_true(model.is_token_pressed("attack_1"), "so does the scripted fire")
	assert_false(model.is_token_pressed("move_back"), "an unheld row stays up")
	assert_eq(model.pressed_key_for_token("move_forward"), 0, "no keyboard VK fires it")
	assert_eq(driver.get_held_codes(), PackedInt32Array([152, 149]))

	model.set_keyboard_captured(true)
	assert_false(model.is_token_pressed("move_forward"), "the chat line owns the device")
	model.set_keyboard_captured(false)

	driver.advance(42)
	assert_false(model.is_token_pressed("move_forward"), "tick 2 released forward")
	assert_true(model.is_token_pressed("attack_1"), "fire still held")
	driver.advance(44)
	assert_true(driver.is_done())
	assert_eq(driver.get_state_name(), "finished")
	assert_false(model.is_token_pressed("attack_1"), "the end released everything")
	assert_eq(driver.get_applied_logic_ticks(), PackedInt64Array([40, 40, 42, 44]))
	assert_eq(driver.get_end_logic_tick(), 44)

	driver.load_steps([{"tick": 0, "down": 152}, {"tick": 9, "end": true}])
	driver.start()
	driver.advance(50)
	assert_true(model.is_token_pressed("move_forward"))
	driver.cancel()
	assert_eq(driver.get_state(), ScriptedInput.STATE_CANCELLED)
	assert_false(model.is_token_pressed("move_forward"), "cancel releases")
	model.set_scripted_input(null)
	assert_null(model.get_scripted_input(), "null detaches")


# The router steps the device on the logic tick its sample is keyed to and
# the session sees ordinary device input: the walk moves the player and the
# crouch press reaches the sim through the stance row (both Active rows).
# A headless display holds no mouse capture (Input keeps MOUSE_MODE_VISIBLE),
# so the look pixels meet the capture gate real motion meets in the shell and
# are dropped here; the captured feed is the live scenario run's witness.
func test_the_router_plays_a_script_on_the_logic_tick() -> void:
	var rig := _rig()
	if rig.is_empty():
		return
	var sim: Simulation = rig.world.get_sim()
	_frame(rig, 2)
	var start_position: Vector3 = sim.get_local_player_position()
	var start_yaw := float(sim.get_local_player_yaw_deg())
	assert_eq(int(sim.get_local_player_stance()), 0, "the spawned player stands")
	var driver: ScriptedInput = rig.driver
	assert_eq(driver.load_steps([
		{"tick": 0, "down": 152},
		{"tick": 0, "look_px": [240, 0]},
		{"tick": 20, "up": 152},
		{"tick": 22, "press": 169},
		{"tick": 30, "end": true},
	]), OK, driver.get_error())
	assert_true(driver.start())
	var latch_tick := int(sim.get_logic_tick())
	for i in 40:
		if driver.is_done():
			break
		_frame(rig, 1)
	assert_eq(driver.get_state_name(), "finished", driver.get_cancel_reason())
	var start := driver.get_start_logic_tick()
	assert_eq(start, latch_tick, "the start is the logic tick of the first sample after start()")
	assert_eq(driver.get_applied_logic_ticks(),
			PackedInt64Array([start, start, start + 20, start + 22, start + 30]),
			"one tick per frame: every step applies on its own tick")
	assert_eq(driver.get_release_logic_ticks()[3], start + 23, "the press releases a tick later")
	assert_eq(driver.get_end_logic_tick(), start + 30)
	assert_gt(sim.get_local_player_position().distance_to(start_position), 0.5,
			"the scripted forward walked the player")
	assert_eq(int(sim.get_local_player_stance()), 1, "the scripted crouch press reached the sim")
	assert_ne(Input.get_mouse_mode(), Input.MOUSE_MODE_CAPTURED, "headless: no capture")
	assert_eq(float(sim.get_local_player_yaw_deg()), start_yaw,
			"uncaptured: the scripted look is dropped as real mouse motion is")
	assert_eq(driver.take_look(), Vector2.ZERO, "the router drained the device's look anyway")


# The scripted device passes the gates the real devices pass, never around
# them: behind an overlay (gameplay input inactive) the schedule still runs
# on the logic tick, but a held forward walks nowhere, a stance press is
# dropped (its latch advances, as a held key's does) and the look is dropped.
func test_scripted_input_meets_the_real_devices_gates() -> void:
	var rig := _rig()
	if rig.is_empty():
		return
	var sim: Simulation = rig.world.get_sim()
	_frame(rig, 2)
	var start_position: Vector3 = sim.get_local_player_position()
	var start_yaw := float(sim.get_local_player_yaw_deg())
	var driver: ScriptedInput = rig.driver
	assert_eq(driver.load_steps([
		{"tick": 0, "down": 152},
		{"tick": 0, "look_px": [240, 0]},
		{"tick": 1, "down": 170},
		{"tick": 12, "up": 152},
		{"tick": 14, "end": true},
	]), OK, driver.get_error())
	driver.start()
	_frame(rig, 16, false)
	assert_true(driver.is_done(), "the schedule runs on the logic tick behind the overlay")
	assert_lt(sim.get_local_player_position().distance_to(start_position), 0.05,
			"gameplay input inactive: the held forward never reached the motor")
	assert_eq(float(sim.get_local_player_yaw_deg()), start_yaw, "nor the look")
	assert_eq(int(sim.get_local_player_stance()), 0, "nor the prone press")

	# A prone key held through the overlay's close does not fire on the
	# return: its latch advanced behind the overlay. A fresh press does.
	assert_eq(driver.load_steps([
		{"tick": 0, "down": 170},
		{"tick": 3, "up": 170},
		{"tick": 5, "press": 170},
		{"tick": 9, "end": true},
	]), OK, driver.get_error())
	driver.start()
	_frame(rig, 1, false)
	_frame(rig, 3)
	assert_eq(int(sim.get_local_player_stance()), 0,
			"a key held across the overlay's close is not a new press")
	_frame(rig, 8)
	assert_true(driver.is_done())
	assert_eq(int(sim.get_local_player_stance()), 2, "the fresh prone press reaches the sim")


# The packaged world over the minimal mission (its playable load spawns the
# local player) with the infantry clip set staged so the motor's body
# selection runs, and a presenter over a ControlsModel carrying a scripted
# device.
func _rig() -> Dictionary:
	_root = WorldFixture.stage_minimal_root("scripted_input")
	for name in ["US01.adm", "E_STAND.adm"]:
		assert_eq(DirAccess.copy_absolute(ProjectSettings.globalize_path(
				"res://../fixtures/anim/soldier.adm"), _root.path_join(name)), OK)
	for name in ["idle.bad", "walk.bad"]:
		assert_eq(DirAccess.copy_absolute(ProjectSettings.globalize_path(
				"res://../fixtures/anim/" + name), _root.path_join(name)), OK)
	var world := WorldFixture.boot_minimal(self, _root)
	if not world.get_sim().has_local_player():
		fail_test("the minimal playable load spawns no local player")
		return {}
	var camera := Camera3D.new()
	add_child_autofree(camera)
	var controls := ControlsModel.new()
	var driver := ScriptedInput.new()
	controls.set_scripted_input(driver)
	var presenter := LocalPlayerPresenter.new()
	add_child_autofree(presenter)
	presenter.setup(world, camera, null, controls)
	_world = world
	_presenter = presenter
	return {"world": world, "camera": camera, "presenter": presenter, "driver": driver}


# Shell frames in main_game's order, one fixed tick each.
func _frame(rig: Dictionary, count: int, active := true) -> void:
	var world: GameWorld = rig.world
	var camera: Camera3D = rig.camera
	var presenter: LocalPlayerPresenter = rig.presenter
	for i in count:
		var frame_input := presenter.before_world_tick(TICK, false, active)
		world.tick(camera.global_position, camera.global_transform, TICK, frame_input)
		presenter.after_world_tick()
