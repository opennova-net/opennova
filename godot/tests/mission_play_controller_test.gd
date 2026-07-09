extends GutTest

# MissionPlayController: the play-in-editor host — the real game world
# (game_world.tscn) in an editor-owned SubViewport. Headless coverage is the
# state machine and rejection paths (a successful boot needs real game assets:
# terrain + env + models; see screenshot_capture's mission_play target and the
# manual A/B against the standalone game).

const PlayController := preload("res://modtools/mission/mission_play_controller.gd")
const BMS_PATH := "res://../fixtures/bms/ash_i5b.reference.bms"


class FakeWorld:
	extends Node3D
	var input_calls: Array = []

	func is_loaded() -> bool:
		return true

	func has_local_player() -> bool:
		return true

	func build_local_player_avatar() -> Node3D:
		var node := Node3D.new()
		add_child(node)
		return node

	func build_local_player_viewmodel() -> Node3D:
		var node := Node3D.new()
		add_child(node)
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


func after_each() -> void:
	Input.set_mouse_mode(Input.MOUSE_MODE_VISIBLE)


func _make_play():
	var play = PlayController.new()
	add_child_autofree(play)
	return play


func _fixture_root() -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	root.set_root_dir(ProjectSettings.globalize_path("res://../fixtures/godot/dvxi5"))
	return root


func test_builds_the_play_stack() -> void:
	var play = _make_play()
	await get_tree().process_frame
	assert_not_null(play.get_node_or_null("PlayViewportContainer/PlayViewport"), "embedded SubViewport exists")
	assert_not_null(play.get_node_or_null("PlayViewportContainer/PlayViewport/World"), "the packaged game world is instanced")
	assert_not_null(play.get_play_camera(), "the shared local-player host owns the play camera")
	assert_null(play.get_play_camera().get_script(), "editor Play Mission no longer uses FlyCamera movement")
	assert_not_null(play.get_player_host(), "editor Play Mission installs the same local-player host as the game")
	assert_not_null(play.get_node_or_null("PlayViewportContainer/PlayViewport/PlayInputRouter"),
		"embedded Play Mission routes SubViewport input to the player host")
	assert_true(play.get_node("PlayViewportContainer/PlayViewport").own_world_3d,
		"the play world is isolated from the editor's 3D world")
	assert_false(play.is_playing())


func test_play_viewport_input_route_drives_player_keys_and_mouse_look() -> void:
	var play = _make_play()
	await get_tree().process_frame
	var world := FakeWorld.new()
	add_child_autofree(world)
	var host = play.get_player_host()
	host.setup(world, play.get_play_camera())
	play._playing = true

	var motion := InputEventMouseMotion.new()
	motion.relative = Vector2(40.0, -20.0)
	assert_true(play.handle_viewport_input(motion), "SubViewport input path must feed gameplay look")
	host.before_world_tick(0.016)

	assert_eq(world.input_calls.size(), 1)
	assert_gt(world.input_calls[0]["yaw"], 0.0)
	assert_gt(world.input_calls[0]["pitch"], 0.0)

	# The stance/camera keys ride the same route the input-stage interception
	# feeds: F4 flips the shared host's first/third person...
	assert_false(host.is_third_person())
	assert_true(play.handle_viewport_input(_pressed_key(KEY_F4)), "F4 is claimed by the play session")
	assert_true(host.is_third_person(), "and toggles third person on the shared host")

	# ...and C / Z edge-toggle the stance, mutually exclusive, observed through
	# the input the host packs into the world (the public seam).
	assert_true(play.handle_viewport_input(_pressed_key(KEY_C)), "C (crouch) is claimed")
	host.before_world_tick(0.016)
	assert_true(play.handle_viewport_input(_pressed_key(KEY_Z)), "Z (prone) is claimed")
	host.before_world_tick(0.016)
	assert_eq(world.input_calls.size(), 3)
	assert_true(world.input_calls[1]["crouch"], "C toggles crouch on")
	assert_false(world.input_calls[1]["prone"])
	assert_false(world.input_calls[2]["crouch"], "prone clears crouch (mutually exclusive)")
	assert_true(world.input_calls[2]["prone"], "Z toggles prone on")

	# While playing the game owns the keyboard: a key pushed through the root
	# viewport's real input pipeline is claimed at the input stage, before the
	# focused editor control (the thing that used to eat F4/C/Z) ever sees it.
	var editor_box := LineEdit.new()
	add_child_autofree(editor_box)
	editor_box.grab_focus()
	get_tree().root.push_input(_pressed_key(KEY_C, "c"))
	host.before_world_tick(0.016)
	assert_eq(world.input_calls.size(), 4)
	assert_true(world.input_calls[3]["crouch"], "the pushed key reached the player host, not the editor UI")
	assert_eq(editor_box.text, "", "the focused control never saw the key while playing")

	# Esc still stops via the same path.
	watch_signals(play)
	assert_true(play.handle_viewport_input(_pressed_key(KEY_ESCAPE)), "Esc is claimed while playing")
	assert_signal_emitted(play, "stop_requested")
	play._playing = false

	# Stopped: the same pushed key falls through to the editor UI again.
	get_tree().root.push_input(_pressed_key(KEY_C, "c"))
	assert_eq(editor_box.text, "c", "not playing: the editor gets its keyboard back")


func test_input_stage_interception_is_gated_on_playing() -> void:
	# While playing, the play session claims the keyboard at the earliest input
	# stage (the script overrides the _input virtual) so focused editor controls,
	# menu accelerators, and workspace key handlers cannot eat the game keys; the
	# unhandled-stage route stays as the backstop. Not playing, every event is
	# declined and the editor keeps its input untouched.
	var controller_script: Script = PlayController
	var script_methods: Array = []
	for m in controller_script.get_script_method_list():
		script_methods.append(String(m.get("name", "")))
	assert_has(script_methods, "_input", "the play controller intercepts at the input stage")
	assert_has(script_methods, "_unhandled_input", "the unhandled-stage backstop remains")

	var play = _make_play()
	await get_tree().process_frame
	assert_false(play.handle_viewport_input(_pressed_key(KEY_F4)), "not playing: F4 is not claimed")
	assert_false(play.handle_viewport_input(_pressed_key(KEY_ESCAPE)), "not playing: Esc is not claimed")
	assert_false(play.get_player_host().is_third_person(), "and the host state never moved")


func test_f3_requests_the_debug_overlay_while_playing() -> void:
	# The game shell binds F3 to the NovaDebugOverlay (main_game.DEBUG_OVERLAY_KEY);
	# play-in-editor shares the gesture by forwarding it to the workspace's overlay.
	var play = _make_play()
	await get_tree().process_frame
	play._playing = true
	watch_signals(play)
	assert_true(play.handle_viewport_input(_pressed_key(KEY_F3)), "F3 is claimed while playing")
	assert_signal_emitted(play, "debug_overlay_requested")
	play._playing = false
	assert_false(play.handle_viewport_input(_pressed_key(KEY_F3)),
		"not playing: F3 falls through to the editor")


func test_debug_overlay_suspends_mouse_capture_for_its_controls() -> void:
	# The game shell frees the mouse for the overlay via its pause menu (Esc);
	# play-in-editor has no pause state, so the workspace suspends the play
	# session's capture while the overlay is up — clicks drive the overlay,
	# not the look, and closing it lets the next tick recapture.
	var play = _make_play()
	await get_tree().process_frame
	play._playing = true
	Input.set_mouse_mode(Input.MOUSE_MODE_CAPTURED)

	play.set_capture_suspended(true)
	assert_eq(Input.get_mouse_mode(), Input.MOUSE_MODE_VISIBLE,
		"suspending frees the mouse for the overlay")
	var motion := InputEventMouseMotion.new()
	motion.relative = Vector2(10.0, 0.0)
	assert_false(play.handle_viewport_input(motion),
		"a free mouse drives the overlay GUI, not the gameplay look")

	play.set_capture_suspended(false)
	assert_eq(Input.get_mouse_mode(), Input.MOUSE_MODE_VISIBLE,
		"resuming does not itself grab the mouse; the per-tick capture does")
	play._playing = false


# A pressed, non-echo key event; `typed` fills unicode so a focused text field
# would type it if the event ever reached the GUI stage.
func _pressed_key(keycode: Key, typed := "") -> InputEventKey:
	var key := InputEventKey.new()
	key.keycode = keycode
	key.physical_keycode = keycode
	key.pressed = true
	if not typed.is_empty():
		key.unicode = typed.unicode_at(0)
	return key


func test_start_rejects_missing_or_unloaded_mission() -> void:
	var play = _make_play()
	await get_tree().process_frame
	var statuses: Array = []
	play.status_reported.connect(func(msg, is_err): statuses.append([msg, is_err]))

	assert_eq(play.start(null, "x.bms", _fixture_root()), ERR_UNAVAILABLE)
	assert_eq(play.start(NovaMissionData.new(), "x.bms", _fixture_root()), ERR_UNAVAILABLE,
		"an unloaded document cannot play")
	assert_false(play.is_playing())
	assert_eq(statuses.size(), 2, "both rejections explain themselves")


func test_start_rejects_missing_root() -> void:
	var play = _make_play()
	await get_tree().process_frame
	var mission := NovaMissionData.new()
	assert_eq(mission.create_default(), OK)
	assert_eq(play.start(mission, "x.bms", null), ERR_UNCONFIGURED)
	assert_false(play.is_playing())


func test_failed_world_boot_leaves_a_clean_stopped_state() -> void:
	# The dvxi5 fixture has the terrain but no .env, so the real mission load
	# fails inside GameWorld; the controller must come back not-playing with the
	# failure surfaced, ready for the workspace to swap the edit viewport back.
	var play = _make_play()
	await get_tree().process_frame
	var mission := NovaMissionData.new()
	assert_eq(mission.open_file(ProjectSettings.globalize_path(BMS_PATH)), OK)

	var statuses: Array = []
	play.status_reported.connect(func(msg, is_err): statuses.append([String(msg), bool(is_err)]))
	var err: int = play.start(mission, "ash_i5b.bms", _fixture_root())
	assert_ne(err, OK, "the fixture root cannot boot a full world (no .env)")
	assert_false(play.is_playing(), "a failed boot is fully stopped")
	assert_gt(statuses.size(), 0, "the failure reason reached the host")
	assert_true(statuses[0][1], "and it is an error status")

	# Stop on a never-started controller is inert, repeatedly.
	play.stop()
	play.stop()
	assert_false(play.is_playing())
