extends GutTest
# The join screen (pre.mnu PRE_GAME_MENU) over an authored pre.mnu: the panel
# table shows the MESSAGES box with Cancel during the join and after a failure,
# the status line replaces the box's text, Cancel reports the abandoned join, and
# the choice panels walk a discovered flag word to the commit (BACK abandons).
# The shipped pre.mnu stays covered by the retail menu-corpus compilation test.

const TMP_DIR := "res://.godot/pre_game_menu_presenter_test"


func before_each() -> void:
	JoinScreenFixture.stage(self, TMP_DIR)


func after_all() -> void:
	JoinScreenFixture.unstage(TMP_DIR)


func _open() -> PreGameMenuPresenter:
	return JoinScreenFixture.open(self, TMP_DIR)


func test_the_join_shows_its_status_line_with_cancel() -> void:
	var presenter := _open()
	presenter.show_progress("Connecting to server...")
	assert_eq(presenter.panel(), JoinScreenStatus.PANEL_PROGRESS)
	assert_eq(presenter.message_text(), "Connecting to server...")
	assert_true(presenter.is_window_shown("ERROR_WRAPPER"), "the MESSAGES box shows")
	assert_true(presenter.is_window_shown("ABORT_WRAPPER"), "Cancel shows")
	assert_false(presenter.is_window_shown("ABORTRETRY_WRAPPER"), "no BACK / ACCEPT")
	assert_false(presenter.is_window_shown("GAME_PASSWORD_WRAPPER"), "no password panel")
	presenter.show_progress("Verifying Session...")
	assert_eq(presenter.message_text(), "Verifying Session...",
			"the status line replaces the text")


func test_a_failure_parks_the_reason_with_cancel_only() -> void:
	var presenter := _open()
	presenter.show_failure("Your game is incompatible with this server. (NCC007)")
	assert_eq(presenter.panel(), JoinScreenStatus.PANEL_ERROR)
	assert_eq(presenter.message_text(), "Your game is incompatible with this server. (NCC007)")
	assert_true(presenter.is_window_shown("ERROR_WRAPPER"))
	assert_true(presenter.is_window_shown("ABORT_WRAPPER"))
	assert_false(presenter.is_window_shown("ABORTRETRY_WRAPPER"))


func test_cancel_reports_the_abandoned_join() -> void:
	var presenter := _open()
	watch_signals(presenter)
	presenter.cancel()
	assert_signal_emitted(presenter, "cancelled")
	presenter.close()
	assert_false(presenter.is_open(), "close takes the screen down")


# The join screen takes each mouse event as its message and pumps its menu once
# a frame after them, as every in-game menu surface does (MenuFrameSurface;
# engine/runtime/menu/menu_runtime.h): a press and release on Cancel inside one
# frame click nothing, and across two frames Cancel's click reports the
# abandoned join, a failure's panel included.
func test_cancel_clicks_once_a_frame_after_the_events() -> void:
	var w := MenuDriverFixture.wnd
	var body: String = (
			w.call("window", "ERROR_WRAPPER", 200,
					w.call("multiline_edit", "MESSAGES", 5, "", " READONLY"), " HIDDEN")
			+ w.call("window", "ABORT_WRAPPER", 300, w.call("button", "ABORT_CANCEL", 0), " HIDDEN"))
	TestFs.write_bytes(self, ProjectSettings.globalize_path(TMP_DIR).path_join("pre.mnu"),
			MenuDriverFixture.screen_xml("PRE_GAME_MENU", body).to_utf8_buffer())
	# The join screen fits its frame to the window, and the GUT runner's own
	# panel covers a headless run's 64x64 window: an 800x600 one puts Cancel
	# where a click reaches it.
	var window := get_window()
	var size0 := window.size
	window.size = Vector2i(800, 600)
	var presenter := PreGameMenuPresenter.new()
	add_child_autofree(presenter)
	var owner := Node.new()
	add_child_autofree(owner)
	assert_true(presenter.open(PresenterFixture.root_over(self, TMP_DIR), owner))
	presenter.show_failure("Your game is incompatible with this server. (NCC007)")
	assert_true(presenter.is_window_shown("ABORT_WRAPPER"), "Cancel shows")
	var frame := owner.get_node("PreGameMenuLayer/PreGameMenu") as MenuFrame
	assert_not_null(frame, "the join screen's frame")
	if frame == null:
		window.size = size0
		return
	var at := Vector2.ZERO
	for i in frame.widget_count():
		if frame.widget_name(i) == "ABORT_CANCEL":
			var center := frame.widget_rect(i).get_center()
			at = frame.get_global_transform_with_canvas() * Vector2(
					center.x * frame.size.x / MenuFrame.DESIGN_WIDTH,
					center.y * frame.size.y / MenuFrame.DESIGN_HEIGHT)
	watch_signals(presenter)
	_send_left(at, true)
	_send_left(at, false)
	await _frames(2)
	assert_signal_not_emitted(presenter, "cancelled",
			"a press and release inside one frame click nothing")
	_send_left(at, true)
	await _frames(2)
	_send_left(at, false)
	await _frames(2)
	assert_signal_emitted(presenter, "cancelled", "Cancel's click, the press held over a frame")
	window.size = size0


func _frames(count: int) -> void:
	for i in count:
		await get_tree().process_frame


func _send_left(at: Vector2, pressed: bool) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = MOUSE_BUTTON_LEFT
	event.pressed = pressed
	event.position = at
	event.global_position = at
	if pressed:
		event.button_mask = MOUSE_BUTTON_MASK_LEFT
	Input.parse_input_event(event)


func test_the_panel_table_matches_the_engine() -> void:
	var names := []
	for i in JoinScreenStatus.window_count():
		names.append(JoinScreenStatus.window_name(i))
	assert_eq(names, ["ERROR_WRAPPER", "ABORT_WRAPPER", "ABORTRETRY_WRAPPER",
			"GAME_PASSWORD_WRAPPER", "SPECTATE_WRAPPER", "TEAM_PASSWORD_WRAPPER",
			"TEAM_STATIC_WRAPPER", "SPECTATOR_STATIC_WRAPPER"])
	assert_true(JoinScreenStatus.panel_shows_window(JoinScreenStatus.PANEL_TEAM_PASSWORD, 6),
			"the team-password panel shows the team text")
	assert_false(JoinScreenStatus.panel_shows_window(JoinScreenStatus.PANEL_TEAM_PASSWORD, 7),
			"and not the spectator text")


# The choice (engine inmatch/pre_game_menu.h, ctest npruntime_pre_game_menu): a
# plain team game offers none; a server password opens GAME_PASSWORD, its ACCEPT
# goes on to SPECTATE, a checked SPECTATE to the spectator password in the shared
# TEAM_PASSWORD control, whose ACCEPT commits the three controls and returns the
# screen to its progress panel.
func test_the_choice_walks_the_flag_word_to_the_commit() -> void:
	var presenter := _open()
	watch_signals(presenter)
	assert_false(presenter.show_choice(JoinTarget.FLAG_TEAM_CHOICE),
			"a plain team game offers no choice")
	assert_true(presenter.show_choice(JoinTarget.FLAG_SERVER_PASSWORD
			| JoinTarget.FLAG_ALLOW_SPECTATORS | JoinTarget.FLAG_SPECTATOR_PASSWORD))
	assert_eq(presenter.panel(), JoinScreenStatus.PANEL_GAME_PASSWORD)
	assert_true(presenter.is_window_shown("GAME_PASSWORD_WRAPPER"))
	assert_true(presenter.is_window_shown("ABORTRETRY_WRAPPER"), "BACK and ACCEPT show")
	assert_false(presenter.is_window_shown("ERROR_WRAPPER"))
	JoinScreenFixture.set_text(presenter, "GAME_PASSWORD", "srv")
	JoinScreenFixture.press(presenter, "ACCEPT")
	assert_eq(presenter.panel(), JoinScreenStatus.PANEL_SPECTATE)
	assert_true(presenter.is_window_shown("SPECTATE_WRAPPER"))
	JoinScreenFixture.set_checked(presenter, "SPECTATE", true)
	JoinScreenFixture.press(presenter, "ACCEPT")
	assert_eq(presenter.panel(), JoinScreenStatus.PANEL_SPECTATOR_PASSWORD)
	assert_true(presenter.is_window_shown("SPECTATOR_STATIC_WRAPPER"))
	assert_false(presenter.is_window_shown("TEAM_STATIC_WRAPPER"))
	JoinScreenFixture.set_text(presenter, "TEAM_PASSWORD", "spec")
	JoinScreenFixture.press(presenter, "ACCEPT")
	assert_signal_emitted_with_parameters(presenter, "choice_committed", ["srv", true, "spec"])
	assert_eq(presenter.panel(), JoinScreenStatus.PANEL_PROGRESS)


# A side password off a plain player: SPECTATE unchecked goes on to TEAM_PASSWORD
# with the team static; BACK abandons the join like Cancel.
func test_back_abandons_the_choice() -> void:
	var presenter := _open()
	watch_signals(presenter)
	assert_true(presenter.show_choice(JoinTarget.FLAG_ALLOW_SPECTATORS
			| JoinTarget.FLAG_BLUE_PASSWORD))
	JoinScreenFixture.press(presenter, "ACCEPT")
	assert_eq(presenter.panel(), JoinScreenStatus.PANEL_TEAM_PASSWORD)
	assert_true(presenter.is_window_shown("TEAM_STATIC_WRAPPER"))
	JoinScreenFixture.press(presenter, "BACK")
	assert_signal_emitted(presenter, "cancelled")
	assert_signal_not_emitted(presenter, "choice_committed")
