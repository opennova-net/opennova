extends GutTest
# The join screen (pre.mnu PRE_GAME_MENU) over an authored pre.mnu: the panel
# table shows the MESSAGES box with Cancel during the join and after a failure,
# the status line replaces the box's text, and Cancel reports the abandoned join.
# The shipped pre.mnu stays covered by the retail menu-corpus compilation test.

const TMP_DIR := "res://.godot/pre_game_menu_presenter_test"
const STAGED := ["pre.mnu", "menutxt.bin"]


func before_each() -> void:
	var dir := ProjectSettings.globalize_path(TMP_DIR)
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	var w := MenuDriverFixture.wnd
	var body: String = (
			w.call("window", "GAME_PASSWORD_WRAPPER", 20,
					w.call("edit", "GAME_PASSWORD", 30), " HIDDEN")
			+ w.call("window", "SPECTATE_WRAPPER", 60,
					w.call("radio", "SPECTATE", 70), " HIDDEN")
			+ w.call("window", "TEAM_PASSWORD_WRAPPER", 100,
					w.call("window", "TEAM_STATIC_WRAPPER", 110, "", " HIDDEN")
					+ w.call("window", "SPECTATOR_STATIC_WRAPPER", 130, "", " HIDDEN")
					+ w.call("edit", "TEAM_PASSWORD", 150), " HIDDEN")
			+ w.call("window", "ERROR_WRAPPER", 200,
					w.call("multiline_edit", "MESSAGES", 210, "", " READONLY"), " HIDDEN")
			+ w.call("window", "ABORT_WRAPPER", 300,
					w.call("button", "ABORT_CANCEL", 310), " HIDDEN")
			+ w.call("window", "ABORTRETRY_WRAPPER", 400,
					w.call("button", "BACK", 410) + w.call("button", "ACCEPT", 440), " HIDDEN"))
	_write(dir.path_join("pre.mnu"),
			MenuDriverFixture.screen_xml("PRE_GAME_MENU", body).to_utf8_buffer())
	_write(dir.path_join("menutxt.bin"), RtxtStringFile.new().to_byte_array())


func after_all() -> void:
	PresenterFixture.unstage(TMP_DIR, PackedStringArray(STAGED))


func _write(path: String, bytes: PackedByteArray) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	if file != null:
		file.store_buffer(bytes)
		file.close()


func _open() -> PreGameMenuPresenter:
	var presenter := PreGameMenuPresenter.new()
	add_child_autofree(presenter)
	var owner := Node.new()
	add_child_autofree(owner)
	assert_true(presenter.open(PresenterFixture.root_over(self, TMP_DIR), owner),
			"the join screen opens over pre.mnu")
	return presenter


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
	_write(ProjectSettings.globalize_path(TMP_DIR).path_join("pre.mnu"),
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
