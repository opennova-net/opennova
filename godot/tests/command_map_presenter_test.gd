extends GutTest

# CommandMapPresenter over a staged cmap.mnu (hud-re D-HUD-19): the commander
# map opens over live play with ONE command-kind map window over its MAP
# control, the zoom buttons step the shared view, the GRID toggle stores its
# control's state and re-checks its ORDERS_ twin, and OK closes the screen.
# The shipped cmap.mnu stays covered by the retail menu-corpus compilation.

const TMP_DIR := "res://.godot/command_map_presenter_test"
const STAGED: Array[String] = ["cmap.mnu", "menutxt.BIN"]

var _sims: Array[Simulation] = []


func before_each() -> void:
	var dir := ProjectSettings.globalize_path(TMP_DIR)
	assert_eq(DirAccess.make_dir_recursive_absolute(dir), OK)
	var body := ('<WINDOW type="window" name="MAP"><APPEARANCE type="custom" state="default">'
			+ '</APPEARANCE><POSITION><LEFT>300</LEFT><TOP>20</TOP><RIGHT>780</RIGHT>'
			+ '<BOTTOM>500</BOTTOM></POSITION></WINDOW>')
	body += MenuDriverFixture.wnd("button", "ZOOMIN", 20)
	body += MenuDriverFixture.wnd("button", "ZOOMOUT", 50)
	body += MenuDriverFixture.wnd("checkbox", "GRID", 80)
	body += MenuDriverFixture.wnd("checkbox", "ORDERS_GRID", 110, "", " HIDDEN")
	body += MenuDriverFixture.wnd("checkbox", "CREATE_WAYPOINTS", 140)
	body += MenuDriverFixture.wnd("button", "OK", 170)
	_write(dir.path_join("cmap.mnu"), MenuDriverFixture.screen_xml("CMAP", body).to_utf8_buffer())
	_write(dir.path_join("menutxt.BIN"), RtxtStringFile.new().to_byte_array())


func after_each() -> void:
	_sims.clear()


func after_all() -> void:
	PresenterFixture.unstage(TMP_DIR, STAGED)


func _write(path: String, bytes: PackedByteArray) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file)
	if file != null:
		file.store_buffer(bytes)
		file.close()


func _make_presenter() -> CommandMapPresenter:
	var mission := MissionData.new()
	assert_eq(mission.create_default(), OK)
	var sim := Simulation.new()
	_sims.append(sim)
	assert_true(sim.load_from_mission_data(mission))
	assert_true(sim.spawn_local_player(Vector3.ZERO, 0.0, 1))
	var view := PresenterFixture.FakeWorldView.new()
	view.root = PresenterFixture.root_over(self, TMP_DIR)
	view.sim_value = sim
	var overlay := Control.new()
	overlay.size = Vector2(800, 600)
	add_child_autofree(overlay)
	var presenter := CommandMapPresenter.new()
	presenter.setup(view, overlay)
	add_child_autofree(presenter)
	return presenter


func test_open_hosts_the_command_map() -> void:
	var presenter := _make_presenter()
	watch_signals(presenter)
	assert_true(presenter.open(), "cmap.mnu's CMAP opens over live play")
	assert_signal_emitted(presenter, "opened")
	var map_window := presenter.get_map_window()
	assert_not_null(map_window, "the MAP control hosts the map window")
	if map_window == null:
		return
	assert_eq(map_window.get_view_kind(), MapViewWindow.VIEW_COMMAND)
	assert_eq(map_window.get_zoom(), 4.0, "the first open seeds zoom 4.0")
	var driver := presenter.get_menu_driver()
	var rect := driver.widget_frame_rect(driver.widget_id("MAP"))
	assert_eq(map_window.position, rect.position)
	assert_eq(map_window.get_widget_design_rect(), Rect2i(300, 20, 480, 480))
	assert_true(map_window.visible)
	for toggle_name in ["GRID", "ORDERS_GRID"]:
		assert_true(driver.is_widget_checked(driver.widget_id(toggle_name)),
				"the draw toggles open checked")


func test_zoom_buttons_toggles_and_ok() -> void:
	var presenter := _make_presenter()
	assert_true(presenter.open())
	var map_window := presenter.get_map_window()
	var driver := presenter.get_menu_driver()
	if map_window == null or driver == null:
		fail_test("the screen did not build")
		return
	driver.activate(driver.widget_id("ZOOMIN"))
	assert_almost_eq(map_window.get_zoom(), 3.4, 0.0001, "ZOOMIN steps x0.85")
	driver.activate(driver.widget_id("ZOOMOUT"))
	assert_almost_eq(map_window.get_zoom(), 4.0, 0.0001, "ZOOMOUT steps x1/0.85")
	var grid := driver.widget_id("GRID")
	driver.set_widget_checked(grid, false)
	driver.activate(grid)
	assert_false(map_window.get_command_toggle(MapViewWindow.COMMAND_TOGGLE_GRID),
			"GRID stores its control's state")
	assert_false(driver.is_widget_checked(driver.widget_id("ORDERS_GRID")),
			"the ORDERS_ twin follows the byte")
	var create := driver.widget_id("CREATE_WAYPOINTS")
	driver.activate(create)
	assert_true(map_window.get_command_toggle(MapViewWindow.COMMAND_TOGGLE_CREATE_WAYPOINTS),
			"CREATE_WAYPOINTS flips its byte")
	watch_signals(presenter)
	driver.activate(driver.widget_id("OK"))
	assert_signal_emitted(presenter, "closed", "OK closes the screen")
	assert_false(presenter.is_open())


func test_the_command_window_compiles_through_the_hud() -> void:
	var presenter := _make_presenter()
	var hud := HudOverlay.new()
	hud.size = Vector2(800, 600)
	add_child_autofree(hud)
	presenter.set_hud_source(func() -> HudOverlay: return hud)
	assert_true(presenter.open())
	var map_window := presenter.get_map_window()
	if map_window == null:
		fail_test("no map window")
		return
	await get_tree().process_frame
	# An unconfigured overlay compiles nothing (no layout, no fonts).
	assert_false(map_window.is_pass_visible())
