extends GutTest

# The scenario driver's scripted input device (ScriptedInput over engine
# devtools/scripted_input.h): the shared script's JSON shape, the retail
# action code -> catalog token map, the device seam (ControlsModel reads a
# scripted token held like a keyboard slot) and the input router playing a
# script on the logic tick through the ordinary binding scan, PlayerActions'
# gates and the mouse-motion path, over a real minimal world, in every
# input frame of a multi-tick frame (the router is the session's per-tick
# source). The engine schedule itself (one flip per token per sample, the
# typed characters' order, the setup pose's conversion) is the scripted_input
# ctest; the session's per-tick pull is the inmatch_tick_input_source ctest.

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
		[[{"tick": 0, "char": "ab"}, {"tick": 2, "end": true}], "\"char\" must be one character"],
		[[{"tick": 0, "char": 13}, {"tick": 2, "end": true}], "\"char\" must be one character"],
		[[{"tick": 0, "char": "\t"}, {"tick": 2, "end": true}],
				"is not backspace, return or printable ASCII"],
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


# A scripted hold also arrives as its bound key's own events (slot 1, else
# slot 2), in step with the token: the readers of raw key events (the deploy
# keys) see the script the way the binding scan does.
func test_the_seam_replays_scripted_holds_as_bound_key_events() -> void:
	var model := ControlsModel.new()
	var driver := ScriptedInput.new()
	assert_eq(driver.load_steps([
		{"tick": 0, "press": 153},
		{"tick": 4, "end": true},
	]), OK, driver.get_error())
	model.set_scripted_input(driver)
	assert_true(driver.start())
	assert_false(Input.is_key_pressed(KEY_SPACE), "nothing held before the first sample")
	model.advance_scripted_input(10)
	assert_true(Input.is_key_pressed(KEY_SPACE), "jump's slot-1 key went down with the press")
	assert_eq(model.pressed_key_for_token("move_jump"), 0x20, "the binding scan sees the key")
	model.advance_scripted_input(11)
	assert_false(Input.is_key_pressed(KEY_SPACE), "the press released its key on the next tick")
	model.advance_scripted_input(14)
	assert_true(driver.is_done())
	assert_eq(driver.get_state(), ScriptedInput.STATE_FINISHED)
	assert_false(model.is_token_pressed("move_jump"), "the end leaves nothing held")
	# A modified row's key arrives with its modifier: seat1's Ctrl+1 holds
	# Ctrl, so the replayed 1 is not the bare 1 the Knife row binds.
	assert_eq(driver.load_steps([
		{"tick": 0, "press": 182},
		{"tick": 4, "end": true},
	]), OK, driver.get_error())
	assert_true(driver.start())
	model.advance_scripted_input(20)
	assert_true(Input.is_key_pressed(KEY_CTRL) and Input.is_key_pressed(KEY_1),
			"seat1's press holds Ctrl+1")
	assert_true(model.is_token_pressed("seat1"), "the seat row reads held")
	assert_false(model.is_token_pressed("Knife"), "the Knife's bare 1 does not read the key")
	model.advance_scripted_input(21)
	assert_false(Input.is_key_pressed(KEY_CTRL) or Input.is_key_pressed(KEY_1),
			"the modifier comes up with its row")
	model.advance_scripted_input(24)
	model.set_scripted_input(null)
	assert_null(model.get_scripted_input(), "null detaches")


# A char step types its character as a keystroke in the input frame it
# applies in: the key's press carrying the character, then its release,
# through the process input path (onHook's driver hands retail the key's
# WM_KEYDOWN, the WM_CHAR and the WM_KEYUP within the tick). An open command
# line's editor takes each as main_game's capture route hands it a typed key
# (HudChatEntry.key_event), so the kick's SERVER PUNT ALL then return types
# the line and submits it.
func test_the_seam_types_scripted_chars_into_an_open_line() -> void:
	var model := ControlsModel.new()
	var driver := ScriptedInput.new()
	var line := "SERVER PUNT ALL"
	var steps: Array = []
	for i in line.length():
		steps.append({"tick": 2 * i, "char": line[i]})
	var enter_tick := 2 * line.length() + 2
	steps.append({"tick": enter_tick, "char": "\r"})
	steps.append({"tick": enter_tick + 2, "end": true})
	assert_eq(driver.load_steps(steps), OK, driver.get_error())
	model.set_scripted_input(driver)
	var chat := HudChatEntry.new()
	var opened := chat.poll_rows(1 << HudChatEntry.ROW_COMMAND, true, null, null, 0)
	assert_eq(opened & HudChatEntry.EVENT_BEGAN, HudChatEntry.EVENT_BEGAN,
			"the command row opens the line")
	var recorder := _KeyRecorder.new()
	recorder.chat = chat
	add_child_autofree(recorder)
	assert_true(driver.start())
	for tick in enter_tick:
		model.advance_scripted_input(100 + tick)
	await get_tree().process_frame
	assert_eq(chat.get_text(), line, "every character reached the editor, in order")
	assert_true(chat.is_capturing(), "the line is still open")
	assert_false(Input.is_key_pressed(KEY_S), "each keystroke released its key")
	model.advance_scripted_input(100 + enter_tick)
	await get_tree().process_frame
	assert_false(chat.is_capturing(), "return submitted the command line")
	assert_eq(chat.get_text(), "", "and emptied it")
	assert_eq(recorder.events.size(), 2 * (line.length() + 1), "a press and a release each")
	var first: InputEventKey = recorder.events[0]
	assert_true(first.pressed and first.keycode == KEY_S and first.unicode == 0x53,
			"S: its key's press carries the character")
	var released: InputEventKey = recorder.events[1]
	assert_true(not released.pressed and released.keycode == KEY_S, "then its release")
	var space: InputEventKey = recorder.events[12]
	assert_true(space.pressed and space.keycode == KEY_SPACE and space.unicode == 0x20, "space")
	var enter: InputEventKey = recorder.events[2 * line.length()]
	assert_true(enter.pressed and enter.keycode == KEY_ENTER, "return is the Enter key")
	model.advance_scripted_input(100 + enter_tick + 2)
	assert_true(driver.is_done(), "the end follows")


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


# Retail runs its input frame in every logic tick (engine
# inmatch::TickInputSource carries the witness): a frame that banks six ticks
# samples the device at each of them, the router being the session's per-tick
# source, so steps due inside the frame land on their own ticks, not on the
# next frame's first, and a press due mid-frame still reaches the sim.
func test_steps_inside_a_multi_tick_frame_land_on_their_own_ticks() -> void:
	var rig := _rig()
	if rig.is_empty():
		return
	var sim: Simulation = rig.world.get_sim()
	# The 7/8 frame-time smoother settles the bank at six ticks a frame.
	_frame(rig, 40, true, 6)
	var before := int(sim.get_logic_tick())
	_frame(rig, 1, true, 6)
	assert_eq(int(sim.get_logic_tick()) - before, 6, "a six-tick frame banks six ticks")
	var driver: ScriptedInput = rig.driver
	assert_eq(driver.load_steps([
		{"tick": 3, "down": 152},
		{"tick": 5, "press": 169},
		{"tick": 9, "up": 152},
		{"tick": 13, "end": true},
	]), OK, driver.get_error())
	assert_true(driver.start())
	var latch_tick := int(sim.get_logic_tick())
	_frame(rig, 3, true, 6)
	assert_eq(driver.get_state_name(), "finished", driver.get_cancel_reason())
	var start := driver.get_start_logic_tick()
	assert_eq(start, latch_tick, "the frame's own sample latches the start at its first tick")
	assert_eq(driver.get_applied_logic_ticks(),
			PackedInt64Array([start + 3, start + 5, start + 9, start + 13]),
			"every step lands on its own tick inside the six-tick frames")
	assert_eq(driver.get_release_logic_ticks()[1], start + 6,
			"the press releases on the next tick, the next frame's first")
	assert_eq(int(sim.get_local_player_stance()), 1,
			"the crouch press sampled mid-frame reached the sim")


# The HUD's key rows run in every input frame too (PlayerInputRouter::
# poll_hud_toggles carries the witness): a one-tick hudcolor press inside a
# six-tick frame fires on its own tick, where one poll after the frame would
# read the row released and never fire it.
func test_a_hud_row_pressed_inside_a_multi_tick_frame_fires_on_its_tick() -> void:
	var rig := _rig()
	if rig.is_empty():
		return
	var presenter: LocalPlayerPresenter = rig.presenter
	var toggles := HudToggles.new()
	presenter.set_hud_toggles(toggles)
	const HUDCOLOR := 10
	assert_eq(ScriptedInput.token_for_action_code(HUDCOLOR),
			HudToggles.row_token(HudToggles.ROW_HUD_COLOR), "code 10 is the hudcolor row")
	_frame(rig, 40, true, 6)
	assert_eq(presenter.take_hud_toggle_events(), PackedInt64Array(), "no row fired while settling")
	var color_before := toggles.get_hud_color_index()
	var driver: ScriptedInput = rig.driver
	assert_eq(driver.load_steps([
		{"tick": 3, "press": HUDCOLOR},
		{"tick": 8, "end": true},
	]), OK, driver.get_error())
	assert_true(driver.start())
	_frame(rig, 2, true, 6)
	assert_eq(driver.get_state_name(), "finished", driver.get_cancel_reason())
	var start := driver.get_start_logic_tick()
	assert_eq(driver.get_applied_logic_ticks()[0], start + 3, "the press lands on tick 3")
	assert_eq(driver.get_release_logic_ticks()[0], start + 4,
			"and releases on tick 4, inside the same six-tick frame")
	assert_eq(presenter.take_hud_toggle_events(),
			PackedInt64Array([HudToggles.EVENT_HUD_COLOR_CYCLED]), "the row fired once")
	assert_eq(toggles.get_hud_color_index(), (color_before + 1) % 6, "the colour cycled")
	assert_eq(presenter.take_hud_toggle_events(), PackedInt64Array(), "reading cleared them")


# The talk rows run in every input frame too (PlayerInputRouter::
# poll_talk_rows carries the witness): a one-tick press of the command row
# (code 1) inside a six-tick frame opens the command line on its tick, where
# one poll after the frame would read the row released and never open it;
# a char step in the next frame types into the open line.
func test_a_talk_row_pressed_inside_a_multi_tick_frame_opens_its_line() -> void:
	var rig := _rig()
	if rig.is_empty():
		return
	var presenter: LocalPlayerPresenter = rig.presenter
	var chat := HudChatEntry.new()
	presenter.set_hud_chat(chat, null)
	const COMMAND := 1
	assert_eq(ScriptedInput.token_for_action_code(COMMAND),
			HudChatEntry.row_token(HudChatEntry.ROW_COMMAND), "code 1 is the command row")
	_frame(rig, 40, true, 6)
	assert_false(chat.is_capturing(), "no line open while settling")
	var recorder := _KeyRecorder.new()
	recorder.chat = chat
	add_child_autofree(recorder)
	var driver: ScriptedInput = rig.driver
	assert_eq(driver.load_steps([
		{"tick": 3, "press": COMMAND},
		{"tick": 7, "char": "K"},
		{"tick": 9, "end": true},
	]), OK, driver.get_error())
	assert_true(driver.start())
	_frame(rig, 1, true, 6)
	var start := driver.get_start_logic_tick()
	assert_eq(driver.get_applied_logic_ticks()[0], start + 3, "the press lands on tick 3")
	assert_eq(driver.get_release_logic_ticks()[0], start + 4,
			"and releases on tick 4, inside the same six-tick frame")
	assert_true(chat.is_capturing(), "the command line opened on the press's tick")
	_frame(rig, 1, true, 6)
	await get_tree().process_frame
	assert_eq(driver.get_state_name(), "finished", driver.get_cancel_reason())
	assert_eq(chat.get_text(), "K", "the next frame's char typed into the open line")
	presenter.set_hud_chat(null, null)


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


# Shell frames in main_game's order, each banking `ticks` fixed ticks' time.
func _frame(rig: Dictionary, count: int, active := true, ticks := 1) -> void:
	var world: GameWorld = rig.world
	var camera: Camera3D = rig.camera
	var presenter: LocalPlayerPresenter = rig.presenter
	for i in count:
		var frame_input := presenter.before_world_tick(TICK * ticks, false, active)
		world.tick(camera.global_position, camera.global_transform, TICK * ticks, frame_input)


# The process input path's key events, each handed to an open chat line's
# editor the way main_game's capture route hands it a typed key.
class _KeyRecorder extends Node:
	var chat: HudChatEntry
	var events: Array = []

	func _input(event: InputEvent) -> void:
		if not (event is InputEventKey):
			return
		events.append(event)
		if chat != null and chat.is_capturing():
			chat.key_event(event, null, null, 0)
