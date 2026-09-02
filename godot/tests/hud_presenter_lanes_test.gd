extends GutTest

# The three HUD panel lanes of GameHudPresenter (vehicle panel, Recent
# Messages + the chat drain, AAS zone status), exercised over a REAL HudOverlay
# with the sim absent: the toggle edge machine, the hide-once bookkeeping, and
# the null paths. The native row feeds (Simulation.fill_vehicle_panel /
# fill_lfp_zones / drain_chat_lines) need a live mission and are pinned by the
# engine ctests (vehicle_panel_feed, lfp_feed, client_replica_chat).

const VehiclePanelPresenter := preload("res://game/world/vehicle_panel_presenter.gd")
const MessageLogPresenter := preload("res://game/world/message_log_presenter.gd")
const LfpPanelPresenter := preload("res://game/world/lfp_panel_presenter.gd")
const FONT_FIXTURE := "res://../fixtures/fnt/synth_1page.fnt"  # staged as Gunpl22b.fnt

var _temp_dirs: Array[String] = []


func after_each() -> void:
	for dir_path in _temp_dirs:
		for file_name in DirAccess.get_files_at(dir_path):
			DirAccess.remove_absolute(dir_path.path_join(file_name))
		DirAccess.remove_absolute(dir_path)
	_temp_dirs.clear()


func _font_overlay() -> HudOverlay:
	var dir_path := OS.get_temp_dir().path_join("hud_lanes_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(dir_path), OK)
	_temp_dirs.append(dir_path)
	var def_file := FileAccess.open(dir_path.path_join("hudpos.def"), FileAccess.WRITE)
	def_file.store_string("fonthud1_hi Gunpl22b.fnt\nHUDCHATTEXT 142 , 711\n")
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


# The OldMessages action is an EDGE that toggles the window; a held key does
# not re-toggle, a chorded or inactive press is ignored, and reset() clears
# the flag like retail's respawn init [orig: xor g_showMessageLog,1
# @0x49b55a; Game_InitRespawnState @0x49939a].
func test_message_log_toggle_edge() -> void:
	var hud := _font_overlay()
	var lane := MessageLogPresenter.new()
	lane.update(hud, null, true, false, true)
	assert_true(lane.is_open(), "The first down-edge opens the window.")
	lane.update(hud, null, true, false, true)
	assert_true(lane.is_open(), "A held key does not re-toggle.")
	lane.update(hud, null, false, false, true)
	lane.update(hud, null, true, true, true)
	assert_true(lane.is_open(), "A chorded press is ignored.")
	lane.update(hud, null, false, false, true)
	lane.update(hud, null, true, false, false)
	assert_true(lane.is_open(), "A press while gameplay input is inactive is ignored.")
	lane.update(hud, null, false, false, true)
	lane.update(hud, null, true, false, true)
	assert_false(lane.is_open(), "The next down-edge closes the window.")
	lane.update(hud, null, false, false, true)
	lane.update(hud, null, true, false, true)
	assert_true(lane.is_open())
	lane.reset()
	assert_false(lane.is_open(), "reset() clears the flag with the mission.")


# The window the lane opens lists a chat line the feed has already expired.
func test_message_log_lane_shows_history() -> void:
	var hud := _font_overlay()
	var lane := MessageLogPresenter.new()
	hud.push_chat_line("Taylor: moving to bravo", -1)
	hud.set_player_state(50 + 930, 1.0, 0, 80.0)
	assert_eq(hud.get_draw_list_stats().glyphs, 0,
			"The feed has expired the line.")
	lane.update(hud, null, true, false, true)
	assert_gt(hud.get_draw_list_stats().glyphs, 0,
			"Opening the window through the lane lists the expired line.")
	await get_tree().process_frame
	lane.update(hud, null, false, false, true)
	lane.update(hud, null, true, false, true)
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
