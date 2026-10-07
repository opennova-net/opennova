extends GutTest

# The DEATH MAP window host (hud/map_view_window.h over the engine's
# runtime/hud/hud_map_view.h, hud-re D-HUD-19): mounted in a frame-sized
# parent, it compiles the windowed pass through a configured HUD overlay's
# frame state plus the session's world facts, draws it clipped to the widget,
# and routes the mouse-class events to the witnessed pan/zoom model.

var _temp_dirs: Array[String] = []
# The window keeps only the session's ObjectID: the test holds the Ref.
var _sims: Array[Simulation] = []


func after_each() -> void:
	for dir_path in _temp_dirs:
		TestFs.remove_dir_recursive(dir_path)
	_temp_dirs.clear()
	_sims.clear()


# A configured overlay over a scratch hudpos + icon strip, with a 16x16 sector
# terrain so the window's terrain leg has tiles to clip.
func _configured_overlay() -> HudOverlay:
	var dir := TestFs.cache_dir(self, "map_view_window")
	_temp_dirs.append(dir)
	TestFs.write_bytes(self, dir.path_join("TSDicon.tga"), TestFs.tga_bytes(Vector2i(16, 480)))
	TestFs.write_text(self, dir.path_join("hudpos.def"), "\n".join(PackedStringArray([
		"HUDSPINMAPX1 810",
		"HUDSPINMAPX2 1020",
		"HUDSPINMAPY1 552",
		"HUDSPINMAPY2 762",
	] + HudFixture.DECLUTTER_ROWS)))
	var layout := HudPos.new()
	assert_eq(layout.load(dir.path_join("hudpos.def")), OK)
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(dir), OK)
	var hud := HudOverlay.new()
	hud.size = Vector2(800, 600)
	add_child_autofree(hud)
	hud.configure(layout, root)
	var terrain := TerrainData.new()
	terrain.set_sector_count(16)
	terrain.set_sector_rows(16)
	terrain.set_origin_x(0)
	terrain.set_origin_y(0)
	var sectors := PackedInt32Array()
	sectors.resize(256)
	sectors.fill(1)
	terrain.set_sector_grid(sectors)
	hud.set_minimap_terrain(terrain)
	return hud


# An offline session with a spawned local player (no replica runtime: the
# deploy overlay latch reads clear, so the crosshair draws).
func _offline_sim() -> Simulation:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var sim := Simulation.new()
	_sims.append(sim)
	assert_true(sim.load_from_mission_data(mission))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	return sim


# death.mnu's MAP window at 800x600: the authored (10,10)-(540,470) inside the
# DEATH_SHROUD (20,20)-(780,545).
func _mounted_window(hud: HudOverlay, sim: Simulation) -> MapViewWindow:
	var frame := Control.new()
	frame.size = Vector2(800, 600)
	add_child_autofree(frame)
	var window := MapViewWindow.new()
	frame.add_child(window)
	window.position = Vector2(30, 30)
	window.size = Vector2(530, 460)
	window.set_widget_design_rect(Rect2i(30, 30, 530, 460))
	window.set_hud_overlay(hud)
	window.set_simulation(sim)
	return window


func test_show_seeds_and_fits_the_zoom() -> void:
	var window := _mounted_window(_configured_overlay(), _offline_sim())
	window.screen_load()
	assert_eq(window.get_zoom(), 4.0, "the first load seeds zoom 4.0")
	# The default mission registers no spawn zone: the all-zero AABB gives a
	# zero extent, which resets zoom 4.0 and a zero pan.
	window.screen_show(true, Vector2i(760, 525))
	assert_eq(window.get_zoom(), 4.0)
	assert_eq(window.get_pan(), Vector2i.ZERO)


func test_the_pass_compiles_through_the_hud_overlay() -> void:
	var window := _mounted_window(_configured_overlay(), _offline_sim())
	window.screen_load()
	window.screen_show(true, Vector2i(760, 525))
	await get_tree().process_frame
	await get_tree().process_frame
	assert_true(window.is_pass_visible(), "the windowed pass compiles")
	assert_gt(window.get_pass_terrain_tris(), 0,
			"the HUD overlay's terrain reaches the window's terrain leg")
	assert_eq(window.get_pass_over_lines(), 2,
			"the player crosshair draws its two full-window lines")
	assert_true(window.clip_contents, "the window clips like the retail viewport")


func test_the_window_draws_nothing_without_sources() -> void:
	var window := _mounted_window(null, null)
	window.screen_load()
	await get_tree().process_frame
	assert_false(window.is_pass_visible())


func test_mouse_events_pan_and_zoom() -> void:
	var window := _mounted_window(_configured_overlay(), _offline_sim())
	window.screen_load()
	window.screen_show(true, Vector2i(760, 525))
	await get_tree().process_frame
	# The wheel forward zooms in by 0.85, back out by 1/0.85.
	window.push_map_event(MapViewWindow.MAP_EVENT_WHEEL, Vector2i(200, 200), 0, 1)
	assert_almost_eq(window.get_zoom(), 3.4, 0.0001)
	window.push_map_event(MapViewWindow.MAP_EVENT_WHEEL, Vector2i(200, 200), 0, -1)
	assert_almost_eq(window.get_zoom(), 4.0, 0.0001)
	# A left drag right moves the view west (the pan and its target alike).
	window.push_map_event(MapViewWindow.MAP_EVENT_LEFT_DOWN, Vector2i(200, 200), 0, 0)
	window.push_map_event(MapViewWindow.MAP_EVENT_MOVE, Vector2i(210, 200),
			MOUSE_BUTTON_MASK_LEFT, 0)
	assert_lt(window.get_pan().x, 0)
	assert_eq(window.get_pan_target(), window.get_pan())
	# A right drag left zooms out (1.005^(-dx)).
	window.push_map_event(MapViewWindow.MAP_EVENT_RIGHT_DOWN, Vector2i(200, 200), 0, 0)
	window.push_map_event(MapViewWindow.MAP_EVENT_MOVE, Vector2i(190, 200),
			MOUSE_BUTTON_MASK_RIGHT, 0)
	assert_gt(window.get_zoom(), 4.0)


func test_the_pan_eases_toward_its_target() -> void:
	var window := _mounted_window(_configured_overlay(), _offline_sim())
	window.screen_load()
	window.screen_show(true, Vector2i(760, 525))
	await get_tree().process_frame
	window.push_map_event(MapViewWindow.MAP_EVENT_LEFT_DOWN, Vector2i(200, 200), 0, 0)
	window.push_map_event(MapViewWindow.MAP_EVENT_MOVE, Vector2i(210, 200),
			MOUSE_BUTTON_MASK_LEFT, 0)
	var dragged := window.get_pan()
	# The drag moves the pan and the target together: the ease holds it.
	window.advance_frame()
	assert_eq(window.get_pan(), dragged)


# The command kind (cmap.mnu MAP / ORDERS_MAP): the CMAP view's own load seed,
# no show-time fit, the zoom buttons, and the mode-4 pass with its crosshair.
func test_the_command_kind_compiles_the_cmap_view() -> void:
	var window := _mounted_window(_configured_overlay(), _offline_sim())
	window.set_view_kind(MapViewWindow.VIEW_COMMAND)
	window.screen_load()
	assert_eq(window.get_zoom(), 4.0)
	assert_true(window.get_command_toggle(MapViewWindow.COMMAND_TOGGLE_GRID))
	window.zoom_button(1)
	assert_almost_eq(window.get_zoom(), 3.4, 0.0001)
	window.screen_show(true, Vector2i(760, 525))
	assert_almost_eq(window.get_zoom(), 3.4, 0.0001, "the fit is the DEATH screen's alone")
	await get_tree().process_frame
	await get_tree().process_frame
	assert_true(window.is_pass_visible())
	assert_gt(window.get_pass_terrain_tris(), 0)
	assert_eq(window.get_pass_over_lines(), 2)


# The window drives the MapViewState it is handed (the presenters hold one for
# the process, like the retail globals): two windows on one state share the
# view, and the load seed runs once per state.
func test_windows_share_a_handed_view_state() -> void:
	var state := MapViewState.new()
	var sim := _offline_sim()
	var hud := _configured_overlay()
	var first := _mounted_window(hud, sim)
	first.set_view_kind(MapViewWindow.VIEW_COMMAND)
	first.set_view_state(state)
	first.screen_load()
	first.zoom_button(1)
	var second := _mounted_window(hud, sim)
	second.set_view_kind(MapViewWindow.VIEW_COMMAND)
	second.set_view_state(state)
	second.screen_load()
	assert_eq(second.get_view_state(), state)
	assert_almost_eq(second.get_zoom(), 3.4, 0.0001, "the second load keeps the stepped zoom")


# The CMAP user-waypoint legs over the bare local role (no wire, the world
# halves alone): the CREATE_WAYPOINTS press asks for the name dialog, the
# confirm places at the stored click, CLEAR_WAYPOINTS removes them all.
func test_the_command_waypoint_legs() -> void:
	var window := _mounted_window(_configured_overlay(), _offline_sim())
	window.set_view_kind(MapViewWindow.VIEW_COMMAND)
	window.screen_load()
	await get_tree().process_frame
	await get_tree().process_frame
	window.command_toggle_changed(MapViewWindow.COMMAND_TOGGLE_CREATE_WAYPOINTS, true)
	assert_true(window.get_command_toggle(MapViewWindow.COMMAND_TOGGLE_CREATE_WAYPOINTS))
	watch_signals(window)
	window.push_map_event(MapViewWindow.MAP_EVENT_LEFT_DOWN, Vector2i(295, 260), 0, 0)
	assert_signal_emitted_with_parameters(window, "waypoint_dialog_requested",
			[Vector2i(295, 260)])
	window.store_waypoint_click(Vector2i(295, 260))
	assert_true(window.confirm_waypoint("RALLY"), "the confirm places a waypoint")
	assert_eq(window.get_waypoint_count(), 1)
	window.clear_create_waypoints()
	assert_false(window.get_command_toggle(MapViewWindow.COMMAND_TOGGLE_CREATE_WAYPOINTS))
	assert_false(window.delete_hovered_waypoint(), "nothing is hovered")
	window.clear_waypoints()
	assert_eq(window.get_waypoint_count(), 0)
	# The name dialog centres on the click and is pushed onto the map's rect.
	assert_eq(MapViewWindow.waypoint_dialog_rect(Vector2i(10, 100), Rect2i(0, 0, 200, 50),
			Rect2i(0, 0, 730, 440)), Rect2i(0, 75, 200, 50))
	assert_eq(MapViewWindow.waypoint_dialog_rect(Vector2i(700, 300), Rect2i(0, 0, 200, 50),
			Rect2i(0, 0, 730, 440)), Rect2i(530, 275, 200, 50))
	# Without a bound delete button the idle move runs no hover test and the
	# render reports no button.
	window.push_map_event(MapViewWindow.MAP_EVENT_MOVE, Vector2i(300, 260), 0, 0)
	assert_false(window.is_close_button_shown())

