extends GutTest

## The SP start-mission splash mode of LoadingScreen: raise/degrade rules,
## the dismissal edges (fresh key, mouse button, held-button mask — never the
## sound), input consumption, the closing background-only frame, and the
## coordinator's gate seams + dismissal forward
## [orig: show_start_mission_splash @ 0x520820; gate @ 0x525d38].

const LoadingScreen := preload("res://game/ui/loading_screen.gd")
const ARROW_FIXTURE := "res://../assets/newarow1.tga"

var _temp_dirs: Array[String] = []


class UnhandledProbe:
	extends Node
	var events := 0

	func _unhandled_input(_event: InputEvent) -> void:
		events += 1


func after_each() -> void:
	for dir in _temp_dirs:
		TestFs.remove_dir_recursive(dir)
	_temp_dirs.clear()


# --- the engine spec surface ---------------------------------------------------

func test_splash_spec_carries_the_witnessed_names() -> void:
	assert_eq(HudPos.loading_splash_arrow_image(), "newarow1.tga")
	assert_eq(HudPos.loading_splash_sound_set(), "START_MISSION")
	assert_eq(HudPos.loading_splash_continue_key(), "LT_Continue")
	assert_eq(HudPos.loading_splash_continue_font(), "Impac22b.fnt")


func test_splash_continue_colors_ride_the_half_bright_fold() -> void:
	# White and the light-red pulse phase, each through the witnessed
	# ((c >> 1) & 0x7F7F7F) | FF alpha fold
	# [orig: blink select @ 0x5209b0; HUD_DrawTextRightAligned_HalfBright
	# fold @ 0x580850].
	var on := HudPos.loading_splash_continue_color(true)
	assert_true(on.is_equal_approx(Color(127 / 255.0, 127 / 255.0, 127 / 255.0)),
		"phase-on = half-bright white")
	var off := HudPos.loading_splash_continue_color(false)
	assert_true(off.is_equal_approx(Color(127 / 255.0, 64 / 255.0, 64 / 255.0)),
		"phase-off = half-bright 0xFF8080")


# --- raise / degrade -----------------------------------------------------------

func test_begin_requires_a_held_background() -> void:
	var screen: LoadingScreen = autofree(LoadingScreen.new())
	assert_false(screen.begin_start_mission_splash(null),
		"no background -> no splash [orig: no screen without the effect]")
	assert_false(screen.is_splash_active())


func test_begin_raises_over_the_held_background() -> void:
	var screen := _setup_screen()
	assert_true(screen.has_custom_background(),
		"the sidecar resolution feeds the retail custom-bg flag")
	assert_true(screen.begin_start_mission_splash(_root_for(screen)))
	assert_true(screen.is_splash_active())
	assert_true(screen.has_splash_arrow(),
		"the fixture newarow1.tga DECODES — TGA rides read_file + the TGA "
		+ "decoder, not the PCX load_texture path")
	assert_false(screen.begin_start_mission_splash(_root_for(screen)),
		"a raised splash never re-raises")


func test_fallback_background_reports_no_custom_flag() -> void:
	var dir := _make_temp_dir("splash_fallback")
	_write_test_pcx(dir.path_join("loadscrn.pcx"))
	var root := ResourceRoot.new()
	root.set_root_dir(dir)
	var screen: LoadingScreen = autofree(LoadingScreen.new())
	screen.setup(root, LoadingScreenInfo.for_mission("NOSIDE.bms"))
	assert_true(screen.has_background())
	assert_false(screen.has_custom_background(),
		"the stock loadscrn.pcx never sets the custom flag [orig: @ 0x521dcc]")


func test_missing_arrow_and_text_degrade_to_skipped_elements() -> void:
	# No newarow1.tga and no gametext table: the splash still raises and still
	# dismisses — the original's loads are unguarded and a miss just draws
	# nothing [orig: @ 0x520871/0x520975].
	var dir := _make_temp_dir("splash_bare")
	_write_test_pcx(dir.path_join("00trg.pcx"))
	var root := ResourceRoot.new()
	root.set_root_dir(dir)
	var screen: LoadingScreen = autofree(LoadingScreen.new())
	screen.setup(root, LoadingScreenInfo.for_mission("00TRg.bms"))
	add_child(screen)
	assert_true(screen.begin_start_mission_splash(root))
	assert_false(screen.has_splash_arrow(), "no arrow art -> arrowless splash")
	watch_signals(screen)
	_push_key(KEY_SPACE)
	await _pump_frames(3)
	assert_signal_emitted(screen, "splash_dismissed")


# --- dismissal edges -----------------------------------------------------------

func test_fresh_key_press_dismisses_after_the_closing_frame() -> void:
	var screen := _mounted_splash()
	watch_signals(screen)
	_push_key(KEY_SPACE)
	assert_true(screen.is_splash_active(),
		"the closing background-only frame still holds the presentation")
	await _pump_frames(3)
	assert_signal_emitted(screen, "splash_dismissed")
	assert_false(screen.is_splash_active())


func test_mouse_button_press_dismisses() -> void:
	var screen := _mounted_splash()
	watch_signals(screen)
	var ev := InputEventMouseButton.new()
	ev.button_index = MOUSE_BUTTON_LEFT
	ev.pressed = true
	get_viewport().push_input(ev)
	await _pump_frames(3)
	assert_signal_emitted(screen, "splash_dismissed")


func test_mouse_motion_moves_the_arrow_without_dismissing() -> void:
	var screen := _mounted_splash()
	watch_signals(screen)
	var ev := InputEventMouseMotion.new()
	ev.position = Vector2(123, 45)
	get_viewport().push_input(ev)
	await _pump_frames(2)
	assert_signal_not_emitted(screen, "splash_dismissed",
		"motion tracks the arrow, never dismisses [orig: only buttons/keys exit]")
	assert_true(screen.is_splash_active())


func test_key_release_does_not_dismiss() -> void:
	var screen := _mounted_splash()
	watch_signals(screen)
	var ev := InputEventKey.new()
	ev.keycode = KEY_SPACE
	ev.pressed = false
	get_viewport().push_input(ev)
	await _pump_frames(2)
	assert_signal_not_emitted(screen, "splash_dismissed")


func test_dismissing_key_is_consumed_before_unhandled_handlers() -> void:
	# Retail dequeues the key out of its input queue; nothing downstream sees
	# it [orig: Input_DequeueKeyEvent @ 0x520a36]. Our _input + accept_event
	# must starve _unhandled_input the same way.
	var probe: UnhandledProbe = add_child_autofree(UnhandledProbe.new())
	var screen := _mounted_splash()
	_push_key(KEY_ESCAPE)
	await _pump_frames(3)
	assert_eq(probe.events, 0,
		"the dismissing press never reaches unhandled handlers")
	assert_false(screen.is_splash_active())


func test_echo_press_dismisses_like_retail_autorepeat() -> void:
	# Held-key autorepeat WM_KEYDOWNs land in the retail queue and dismiss;
	# Godot's echo events are their analog [orig: Input_QueueKeyEvent @ 0x760c10].
	var screen := _mounted_splash()
	watch_signals(screen)
	var ev := InputEventKey.new()
	ev.keycode = KEY_W
	ev.pressed = true
	ev.echo = true
	get_viewport().push_input(ev)
	await _pump_frames(3)
	assert_signal_emitted(screen, "splash_dismissed")


func test_programmatic_dismissal_uses_the_same_closing_frame() -> void:
	var screen := _mounted_splash()
	watch_signals(screen)
	assert_true(screen.dismiss_start_mission_splash())
	assert_true(screen.is_splash_active(),
		"The probe seam must preserve the witnessed background-only closing frame.")
	await _pump_frames(3)
	assert_signal_emitted(screen, "splash_dismissed")
	assert_false(screen.is_splash_active())
	assert_false(screen.dismiss_start_mission_splash(),
		"An inactive splash cannot report a second dismissal.")


# --- coordinator seams ---------------------------------------------------------

func test_coordinator_progress_completes_only_on_the_shell_edge() -> void:
	var owner: Node = add_child_autofree(Node.new())
	var root := _art_root()
	var world: GameWorld = autofree(GameWorld.new())
	var coordinator := WorldLoadCoordinator.new()
	assert_not_null(coordinator.start(owner, root, world,
			LoadingScreenInfo.for_mission("00TRg.bms"), func() -> int:
				world.load_progress.emit(90)
				return OK))
	await _pump_frames(4)
	assert_eq(coordinator.progress_percent(), 90,
		"a locally ready world remains below presentation completion")
	coordinator.complete_progress()
	assert_eq(coordinator.progress_percent(), 100,
		"only the shell's release edge completes the bar")
	coordinator.finish_presentation()
	assert_eq(coordinator.progress_percent(), -1,
		"no active loading presentation has no progress value")


func test_coordinator_gate_seams_and_dismissal_forward() -> void:
	var owner: Node = add_child_autofree(Node.new())
	var root := _art_root()
	var world: GameWorld = autofree(GameWorld.new())
	var coordinator := WorldLoadCoordinator.new()
	var operation := coordinator.start(owner, root, world,
			LoadingScreenInfo.for_mission("00TRg.bms"), func() -> int: return OK)
	assert_not_null(operation, "the SP load starts")
	await _pump_frames(4)
	assert_false(coordinator.is_session_load(),
		"SP load_info never carries in_session [orig: the session gate half]")
	assert_true(coordinator.has_custom_background())
	assert_false(coordinator.is_splash_active())
	watch_signals(coordinator)
	assert_true(coordinator.begin_start_mission_splash())
	assert_true(coordinator.is_splash_active())
	_push_key(KEY_SPACE)
	await _pump_frames(3)
	assert_signal_emitted(coordinator, "splash_dismissed",
		"the screen's edge is forwarded to the shell")
	coordinator.finish_presentation()


func test_coordinator_can_dismiss_an_active_splash_without_input_synthesis() -> void:
	var owner: Node = add_child_autofree(Node.new())
	var root := _art_root()
	var world: GameWorld = autofree(GameWorld.new())
	var coordinator := WorldLoadCoordinator.new()
	assert_not_null(coordinator.start(owner, root, world,
			LoadingScreenInfo.for_mission("00TRg.bms"), func() -> int: return OK))
	await _pump_frames(4)
	watch_signals(coordinator)
	assert_true(coordinator.begin_start_mission_splash())
	assert_true(coordinator.dismiss_start_mission_splash())
	await _pump_frames(3)
	assert_signal_emitted(coordinator, "splash_dismissed")
	coordinator.finish_presentation()


func test_coordinator_session_load_reports_the_session_flag() -> void:
	var owner: Node = add_child_autofree(Node.new())
	var root := _art_root()
	var world: GameWorld = autofree(GameWorld.new())
	var coordinator := WorldLoadCoordinator.new()
	var operation := coordinator.start(owner, root, world,
			LoadingScreenInfo.make("00TRg.bms", true, "", "", -1, ""),
			func() -> int: return OK)
	assert_not_null(operation)
	await _pump_frames(4)
	assert_true(coordinator.is_session_load(),
		"session load_info carries in_session -> the shell gate skips")
	coordinator.finish_presentation()


func test_coordinator_begin_fails_without_a_screen() -> void:
	var coordinator := WorldLoadCoordinator.new()
	assert_false(coordinator.begin_start_mission_splash())
	assert_false(coordinator.is_splash_active())
	assert_false(coordinator.has_custom_background())


func test_maybe_begin_never_raises_headless() -> void:
	# The shell-policy wrapper's headless skip is load-bearing: fixture SP
	# loads resolve a sidecar (mnml.pcx) and would otherwise hold the
	# presentation forever in the headless lifecycle tests.
	var owner: Node = add_child_autofree(Node.new())
	var root := _art_root()
	var world: GameWorld = autofree(GameWorld.new())
	var coordinator := WorldLoadCoordinator.new()
	var operation := coordinator.start(owner, root, world,
			LoadingScreenInfo.for_mission("00TRg.bms"), func() -> int: return OK)
	assert_not_null(operation)
	await _pump_frames(4)
	assert_true(coordinator.has_custom_background(),
		"the gate data is satisfied — only the headless policy blocks")
	assert_false(coordinator.maybe_begin_start_mission_splash(null))
	assert_false(coordinator.is_splash_active())
	coordinator.finish_presentation()


func test_maybe_begin_positive_leg_raises_and_restores_the_cursor() -> void:
	# The same wrapper with the headless policy injected off: the gate passes,
	# the splash raises, the OS cursor hides for the splash's own arrow
	# [orig: Mouse_SetCallback(0) @ 0x520862], and the dismissal edge
	# restores it.
	var owner: Node = add_child_autofree(Node.new())
	var root := _art_root()
	var world: GameWorld = autofree(GameWorld.new())
	var coordinator := WorldLoadCoordinator.new()
	var operation := coordinator.start(owner, root, world,
			LoadingScreenInfo.for_mission("00TRg.bms"), func() -> int: return OK)
	assert_not_null(operation)
	await _pump_frames(4)
	watch_signals(coordinator)
	assert_true(coordinator.maybe_begin_start_mission_splash(
			null, false), "the positive leg raises with the skip injected off")
	assert_true(coordinator.is_splash_active())
	# The OS mouse-mode set is a no-op under the headless display server, so
	# the cursor hide/restore device leg cannot be asserted here; the raise +
	# the forwarded dismissal edge are the observable positive-leg contract.
	_push_key(KEY_SPACE)
	await _pump_frames(3)
	assert_signal_emitted(coordinator, "splash_dismissed")
	assert_false(coordinator.is_splash_active(),
			"the dismissal edge tears the splash down")
	coordinator.finish_presentation()


func test_splash_survives_ready_after_pre_tree_begin() -> void:
	# begin before add_child: _ready()'s callback disarm must not neuter an
	# already-ACTIVE splash (the guard on _splash_state).
	var root := _art_root()
	var screen: LoadingScreen = LoadingScreen.new()
	screen.setup(root, LoadingScreenInfo.for_mission("00TRg.bms"))
	assert_true(screen.begin_start_mission_splash(root))
	add_child_autofree(screen)
	assert_true(screen.is_splash_active())
	assert_true(screen.is_processing_input(),
			"_ready leaves an active splash armed")
	_push_key(KEY_SPACE)
	await _pump_frames(3)
	assert_false(screen.is_splash_active(),
			"the pre-tree-raised splash still dismisses")


# --- helpers -------------------------------------------------------------------

var _roots := {}


func _setup_screen() -> LoadingScreen:
	var root := _art_root()
	var screen: LoadingScreen = autofree(LoadingScreen.new())
	screen.setup(root, LoadingScreenInfo.for_mission("00TRg.bms"))
	assert_true(screen.has_background(), "the test pcx decodes into a texture")
	_roots[screen] = root
	return screen


func _root_for(screen: LoadingScreen) -> ResourceRoot:
	return _roots.get(screen)


func _mounted_splash() -> LoadingScreen:
	var screen := _setup_screen()
	add_child(screen)
	assert_true(screen.begin_start_mission_splash(_root_for(screen)))
	return screen


func _art_root() -> ResourceRoot:
	var dir := _make_temp_dir("splash_art")
	_write_test_pcx(dir.path_join("00trg.pcx"))
	_write_test_pcx(dir.path_join("loadscrn.pcx"))
	var arrow := FileAccess.get_file_as_bytes(ARROW_FIXTURE)
	if not arrow.is_empty():
		var f := FileAccess.open(dir.path_join("newarow1.tga"), FileAccess.WRITE)
		f.store_buffer(arrow)
		f.close()
	var root := ResourceRoot.new()
	root.set_root_dir(dir)
	return root


func _push_key(keycode: Key) -> void:
	var ev := InputEventKey.new()
	ev.keycode = keycode
	ev.pressed = true
	get_viewport().push_input(ev)


func _pump_frames(count: int) -> void:
	for _i in range(count):
		await get_tree().process_frame


func _make_temp_dir(name: String) -> String:
	var dir := OS.get_cache_dir().path_join(
			"opennova_%s_%d" % [name, Time.get_ticks_usec()])
	DirAccess.make_dir_recursive_absolute(dir)
	_temp_dirs.append(dir)
	return dir


# A minimal valid 8-bit palettized PCX (2x2), mirrored from
# loading_screen_test.gd so this file stays runnable in isolation.
func _write_test_pcx(path: String) -> void:
	var bytes := PackedByteArray()
	bytes.resize(128)
	bytes[0] = 0x0A
	bytes[1] = 5
	bytes[2] = 1
	bytes[3] = 8
	bytes[8] = 1
	bytes[10] = 1
	bytes[65] = 1
	bytes[66] = 2
	for p in [0, 1, 2, 3]:
		bytes.append(p)
	bytes.append(0x0C)
	for i in range(256):
		bytes.append(i)
		bytes.append(i)
		bytes.append(i)
	var f := FileAccess.open(path, FileAccess.WRITE)
	f.store_buffer(bytes)
	f.close()
