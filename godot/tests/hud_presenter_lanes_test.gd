extends GutTest

# The three HUD panel lanes of GameHudPresenter (vehicle panel, Recent
# Messages + the chat drain, AAS zone status), exercised over a REAL HudOverlay
# with the sim absent: the toggle edge machine, the hide-once bookkeeping, and
# the null paths. The native row feeds (Simulation.fill_vehicle_panel /
# fill_lfp_zones / drain_feed_posts) need a live mission and are pinned by the
# engine ctests (vehicle_panel_feed, lfp_feed, client_replica_chat).

const VehiclePanelPresenter := preload("res://game/world/vehicle_panel_presenter.gd")
const MessageLogPresenter := preload("res://game/world/message_log_presenter.gd")
const LfpPanelPresenter := preload("res://game/world/lfp_panel_presenter.gd")
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
	def_file.store_string("fonthud1_hi Gunpl22b.fnt\r\nHUDCHATTEXT 142 , 711\r\n")
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
	hud.set_player_state(50, 1.0, 0, 80.0)
	return hud


# The OldMessages action is an EDGE that toggles the window flag on the
# engine's HudToggles and runs the respawn init that keeps one window up
# (the rule itself is pinned by the hud_toggles ctest); the binding carries
# the flag across polls and reset_mission() clears it like retail's respawn
# init [orig: xor g_ShowMessageLog,1 @0x49b55a; Game_InitRespawnState
# @0x49939a].
func test_message_log_toggle_edge() -> void:
	var toggles := HudToggles.new()
	var poll := func(down: bool, chorded: bool, active: bool) -> int:
		return toggles.poll((1 << HudToggles.ROW_OLD_MESSAGES) if down else 0, false,
				chorded, active, false, false)
	assert_eq(poll.call(true, false, true),
			HudToggles.EVENT_MESSAGE_LOG_TOGGLED | HudToggles.EVENT_OVERLAY_WINDOWS_CLEARED)
	assert_true(toggles.is_message_log_open(), "The first down-edge opens the window.")
	assert_eq(poll.call(true, false, true), 0, "A held key does not re-toggle.")
	poll.call(false, false, true)
	poll.call(true, true, true)
	assert_true(toggles.is_message_log_open(), "A chorded press is ignored.")
	poll.call(false, false, true)
	poll.call(true, false, false)
	assert_true(toggles.is_message_log_open(),
			"A press while gameplay input is inactive is ignored.")
	poll.call(false, false, true)
	poll.call(true, false, true)
	assert_false(toggles.is_message_log_open(), "The next down-edge closes the window.")
	poll.call(false, false, true)
	poll.call(true, false, true)
	assert_true(toggles.is_message_log_open())
	toggles.reset_mission()
	assert_false(toggles.is_message_log_open(), "reset_mission() clears the flag.")


# The window the lane opens for the flag lists a chat line the feed has
# already expired, and hides it again when the flag drops.
func test_message_log_lane_shows_history() -> void:
	var hud := _font_overlay()
	var lane := MessageLogPresenter.new()
	hud.push_chat_line("Taylor: moving to bravo", -1)
	hud.set_player_state(50 + 930, 1.0, 0, 80.0)
	assert_eq(hud.get_draw_list_stats().glyphs, 0,
			"The feed has expired the line.")
	lane.update(hud, true)
	assert_gt(hud.get_draw_list_stats().glyphs, 0,
			"Opening the window through the lane lists the expired line.")
	await get_tree().process_frame
	lane.update(hud, false)
	assert_eq(hud.get_draw_list_stats().glyphs, 0,
			"Closing through the lane hides the history.")


# The vehicle panel lane hides on every missing input without touching the
# overlay until it has pushed something, and the zone panel lane hides with
# no sim.
func test_vehicle_and_zone_lanes_null_paths() -> void:
	var hud := _font_overlay()
	var before := hud.get_draw_list_stats()
	var vehicle := VehiclePanelPresenter.new()
	vehicle.update(hud, null, null, null, 0)
	vehicle.update(null, null, null, null, 0)
	var zones := LfpPanelPresenter.new()
	zones.update(hud, null, 0)
	zones.update(null, null, 0)
	var after := hud.get_draw_list_stats()
	assert_eq(after.elements_drawn, before.elements_drawn,
			"The null paths draw nothing and crash nothing.")
	vehicle.reset()
	zones.reset()
	assert_true(is_instance_valid(hud))


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
	var every_row := (1 << HudChatEntry.ROW_COUNT) - 1
	chat.poll_rows(0, true, false, null, null, 100)
	chat.poll_rows(every_row, true, false, null, null, 101)
	assert_false(chat.is_capturing(), "No session, no chat line.")
	var before := hud.get_draw_list_stats().glyphs
	hud.set_chat_input(chat, 101, false)
	assert_eq(hud.get_draw_list_stats().glyphs, before, "A closed chat line draws nothing.")
	var key := InputEventKey.new()
	key.keycode = KEY_A
	key.unicode = 97
	key.pressed = true
	assert_false(chat.key_event(key, null, null, 102), "A closed chat line takes no key.")
	assert_eq(HudChatEntry.row_token(1), "ltalk", "The talk rows ride the catalog tokens.")
