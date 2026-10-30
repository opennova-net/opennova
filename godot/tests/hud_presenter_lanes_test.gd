extends GutTest

# The HUD panels the session's frame pushes (HudSession over engine hud/hud_session.h: the
# Recent Messages window, the Tab board, the chat line), exercised over a REAL HudOverlay with the
# sim absent: the toggle edge machine and the overlay's own window seams. The native row feeds
# (Simulation.fill_vehicle_panel / inmatch::hud_lfp_panel_feed / drain_feed_posts) need a live
# mission and are pinned by the engine ctests (vehicle_panel_feed, lfp_feed, client_replica_chat).
const FONT_FIXTURE := "res://../fixtures/fnt/synth_1page.fnt"  # staged as Gunpl22b.fnt

var _temp_dirs: Array[String] = []


func after_each() -> void:
	for dir_path in _temp_dirs:
		TestFs.remove_dir_recursive(dir_path)
	_temp_dirs.clear()


func _font_overlay() -> HudOverlay:
	var dir_path := OS.get_temp_dir().path_join("hud_lanes_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir_path), OK)
	_temp_dirs.append(dir_path)
	var def_file := FileAccess.open(dir_path.path_join("hudpos.def"), FileAccess.WRITE)
	def_file.store_string("fonthud1_hi Gunpl22b.fnt\r\nHUDCHATTEXT 142 , 711\r\n"
			+ HudFixture.declutter_text())
	def_file.close()
	var fnt := FileAccess.open(dir_path.path_join("Gunpl22b.fnt"), FileAccess.WRITE)
	fnt.store_buffer(FileAccess.get_file_as_bytes(FONT_FIXTURE))
	fnt.close()
	var layout := HudPos.new()
	assert_eq(layout.load(dir_path.path_join("hudpos.def")), OK)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir_path), OK)
	var hud := HudOverlay.new()
	hud.size = Vector2(1024, 768)
	add_child_autofree(hud)
	hud.configure(layout, root)
	hud.set_frame_clocks(50, 50, 50, 50)
	hud.set_player_state(1.0, 0, 80.0)
	return hud


# The OldMessages action is an EDGE that toggles the window flag on the
# engine's HudToggles and runs the respawn init that keeps one window up
# (the rule itself is pinned by the hud_toggles ctest); the binding carries
# the flag across polls and reset_mission() clears it like retail's respawn
# init [orig: xor g_ShowMessageLog,1 @0x49b55a; Game_InitRespawnState
# @0x49939a].
func test_message_log_toggle_edge() -> void:
	var toggles := HudToggles.new()
	var poll := func(down: bool, active: bool) -> int:
		return toggles.poll((1 << HudToggles.ROW_OLD_MESSAGES) if down else 0,
				active, false, false)
	assert_eq(poll.call(true, true),
			HudToggles.EVENT_MESSAGE_LOG_TOGGLED | HudToggles.EVENT_OVERLAY_WINDOWS_CLEARED)
	assert_true(toggles.is_message_log_open(), "The first down-edge opens the window.")
	assert_eq(poll.call(true, true), 0, "A held key does not re-toggle.")
	poll.call(false, true)
	poll.call(true, false)
	assert_true(toggles.is_message_log_open(),
			"A press while gameplay input is inactive is ignored.")
	poll.call(false, true)
	poll.call(true, true)
	assert_false(toggles.is_message_log_open(), "The next down-edge closes the window.")
	poll.call(false, true)
	poll.call(true, true)
	assert_true(toggles.is_message_log_open())
	toggles.reset_mission()
	assert_false(toggles.is_message_log_open(), "reset_mission() clears the flag.")


# The window the session opens for the flag lists a chat line the feed has already expired, and
# hides it again when the flag drops.
func test_message_log_window_shows_history() -> void:
	var hud := _font_overlay()
	hud.push_chat_line("Taylor: moving to bravo", -1)
	hud.set_frame_clocks(50 + 930, 50 + 930, 50 + 930, 50 + 930)
	hud.set_player_state(1.0, 0, 80.0)
	assert_eq(hud.get_draw_list_stats().glyphs, 0,
			"The feed has expired the line.")
	hud.set_message_log_shown(true)
	assert_gt(hud.get_draw_list_stats().glyphs, 0,
			"Opening the window lists the expired line.")
	await get_tree().process_frame
	hud.set_message_log_shown(false)
	assert_eq(hud.get_draw_list_stats().glyphs, 0,
			"Closing the window hides the history.")


# The Tab board takes PgUp/PgDn only in a session with the board up (the
# engine's special-key arm); out of a session no talk row opens a chat line,
# a closed line draws nothing and takes no key (engine hud_chat_entry).
func test_board_page_keys_and_chat_line_gates() -> void:
	var hud := _font_overlay()
	assert_false(hud.scoreboard_page_key(true, false, true),
			"Out of a session the board leaves PgDn to the other windows.")
	assert_false(hud.scoreboard_page_key(true, true, false),
			"A closed board leaves PgDn to the other windows.")
	assert_true(hud.scoreboard_page_key(false, true, true),
			"An open in-session board takes PgUp.")
	hud.reset_scoreboard_page()
	var chat := HudChatEntry.new()
	var talk_rows := (1 << HudChatEntry.ROW_COMMAND) - 1
	chat.poll_rows(0, true, null, null, 100)
	chat.poll_rows(talk_rows, true, null, null, 101)
	assert_false(chat.is_capturing(), "No session, no chat line.")
	var before := hud.get_draw_list_stats().glyphs
	hud.set_frame_clocks(101, 101, 101, 101)
	hud.set_chat_input(chat, false)
	assert_eq(hud.get_draw_list_stats().glyphs, before, "A closed chat line draws nothing.")
	var key := InputEventKey.new()
	key.keycode = KEY_A
	key.unicode = 97
	key.pressed = true
	assert_false(chat.key_event(key, null, null, 102), "A closed chat line takes no key.")
	assert_eq(HudChatEntry.row_token(1), "ltalk", "The talk rows ride the catalog tokens.")
	# The command entry (input actions 1 and 20) has no session gate: its row opens the
	# line out of a session too (engine hud_chat_entry kChatDispatchCommand).
	assert_eq(HudChatEntry.row_token(HudChatEntry.ROW_COMMAND), "command",
			"The command entry rides the catalog's command row.")
	var command := HudChatEntry.new()
	command.poll_rows(0, true, null, null, 200)
	command.poll_rows(1 << HudChatEntry.ROW_COMMAND, true, null, null, 201)
	assert_true(command.is_capturing(), "The command row opens its line out of a session.")
	assert_eq(command.get_open_dispatch(), 1, "The command row dispatches input action 1.")
