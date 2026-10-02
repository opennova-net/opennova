extends GutTest

# CommandMapPresenter over a staged cmap.mnu (hud-re D-HUD-19): the commander
# map opens over live play with ONE command-kind map window over its MAP
# control, the zoom buttons step the shared view, the GRID toggle stores its
# control's state and re-checks its ORDERS_ twin, the tab radios take their
# gates, the CREATE_WAYPOINTS press runs the name dialog through to a placed
# waypoint, the chat send reads and clears its line, the ORDERS composer lists
# and cancels an order, and OK closes the screen.
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
	body += MenuDriverFixture.wnd("radio", "RADIO_TAB_MAP", 200)
	for tab in ["RADIO_TAB_ORDERS", "RADIO_TAB_PLAYERS", "RADIO_TAB_TEAM", "RADIO_TAB_RULES"]:
		body += MenuDriverFixture.wnd("radio", tab, 230)
	body += MenuDriverFixture.wnd("button", "CLEAR_WAYPOINTS", 260)
	body += MenuDriverFixture.wnd("button", "GOCODE_UNIFORM", 290)
	body += MenuDriverFixture.wnd("edit", "CHAT_TEXT", 320)
	body += MenuDriverFixture.wnd("button", "CHAT_TEAM", 350)
	# The name dialog (240x20 at (10, 520)) and the 240-wide delete button.
	var hide := '<ACTION type="window" state="HIDE">WAYPOINTNAME_DLG</ACTION>'
	var dialog := MenuDriverFixture.wnd("edit", "WPNAME", 0)
	dialog += MenuDriverFixture.wnd("button", "WPNAME_CANCEL", 0, hide)
	dialog += MenuDriverFixture.wnd("button", "WPNAME_OK", 0, hide)
	body += MenuDriverFixture.wnd("window", "WAYPOINTNAME_DLG", 520, dialog, " HIDDEN")
	body += MenuDriverFixture.wnd("button", "USERWP_CLOSE", 550, "", " HIDDEN")
	# The TEAM / PLAYERS tables and the ORDERS composer's controls.
	body += MenuDriverFixture.wnd("table", "TEAMLIST", 380,
			'<COLUMN count="10"><HEADER column="0" width="50">R</HEADER></COLUMN>')
	body += MenuDriverFixture.wnd("table", "PLAYERLIST", 400,
			'<COLUMN count="7"><HEADER column="0" width="50">N</HEADER></COLUMN>')
	body += MenuDriverFixture.wnd("table", "CURRENT_ORDERS", 420,
			'<COLUMN count="2"><HEADER column="0" width="50">D</HEADER>'
			+ '<HEADER column="1" width="150">O</HEADER></COLUMN>')
	var list_box := ('<LIST_BOX><POSITION><LEFT>0</LEFT><TOP>20</TOP><RIGHT>200</RIGHT>'
			+ '<BOTTOM>80</BOTTOM></POSITION><ITEMS>%s</ITEMS></LIST_BOX>')
	body += MenuDriverFixture.wnd("combobox", "GROUP", 440, list_box % "")
	body += MenuDriverFixture.wnd("combobox", "COMMAND_ORDER", 460, list_box %
			'<ITEM value="0">Halt</ITEM><ITEM value="1">Attack</ITEM>')
	body += MenuDriverFixture.wnd("combobox", "LOCATION", 480, list_box % "")
	body += MenuDriverFixture.wnd("button", "NEW_ORDER", 500)
	body += MenuDriverFixture.wnd("button", "ADDTO_FIRETEAM_A", 510)
	body += MenuDriverFixture.wnd("window", "CHAT_MSGS", 530,
			'<APPEARANCE type="custom" state="default"></APPEARANCE>')
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
	# A click is the activation: the checkbox's own class step (CCheckboxWnd's
	# toggle) clears the box that opened checked, then the callback stores the
	# control's state.
	driver.activate(grid)
	assert_false(driver.is_widget_checked(grid), "the click clears the open GRID")
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


# The show gates the tab radios (the bare local role: in no session, leading
# nobody) [orig: CMap_PopulateTeamList @0x547a50].
func test_the_tab_radios_take_their_gates() -> void:
	var presenter := _make_presenter()
	assert_true(presenter.open())
	var driver := presenter.get_menu_driver()
	for tab in ["RADIO_TAB_ORDERS", "RADIO_TAB_PLAYERS", "RADIO_TAB_TEAM", "RADIO_TAB_RULES"]:
		assert_true(driver.is_widget_disabled(driver.widget_id(tab)), tab + " is gated off")
	assert_false(driver.is_widget_disabled(driver.widget_id("RADIO_TAB_MAP")))


# The CREATE_WAYPOINTS press: the name dialog centred on the click and pushed
# onto the MAP control's own rect, shown with WPNAME focused; WPNAME's Enter
# presses WPNAME_OK, which places the waypoint, clears the field, unchecks
# CREATE_WAYPOINTS and hides the dialog (its authored action).
func test_the_waypoint_dialog_places_a_waypoint() -> void:
	var presenter := _make_presenter()
	var hud := HudOverlay.new()
	hud.size = Vector2(800, 600)
	add_child_autofree(hud)
	presenter.set_hud_source(func() -> HudOverlay: return hud)
	assert_true(presenter.open())
	var map_window := presenter.get_map_window()
	var driver := presenter.get_menu_driver()
	if map_window == null or driver == null:
		fail_test("the screen did not build")
		return
	var create := driver.widget_id("CREATE_WAYPOINTS")
	driver.activate(create)
	assert_true(map_window.get_command_toggle(MapViewWindow.COMMAND_TOGGLE_CREATE_WAYPOINTS))
	map_window.push_map_event(MapViewWindow.MAP_EVENT_LEFT_DOWN, Vector2i(400, 200), 0, 0)
	var dialog := driver.widget_id("WAYPOINTNAME_DLG")
	assert_true(driver.is_widget_shown(dialog), "the press shows the dialog")
	# (400 - 120, 200 - 10) pushed right onto MAP's left edge 300.
	assert_eq(driver.widget_local_rect(dialog), Rect2(300, 190, 240, 20))
	var name_id := driver.widget_id("WPNAME")
	assert_eq(driver.get_focused_widget(), name_id, "the name field takes the focus")
	await get_tree().process_frame
	assert_eq(map_window.mouse_filter, Control.MOUSE_FILTER_IGNORE,
			"the modal dialog keeps the map from the mouse")
	driver.set_widget_text(name_id, "RALLY")
	var enter := InputEventKey.new()
	enter.keycode = KEY_ENTER
	enter.pressed = true
	assert_true(driver.handle_key_input(enter))
	assert_eq(map_window.get_waypoint_count(), 1, "WPNAME's Enter presses WPNAME_OK")
	assert_eq(driver.get_widget_text(name_id), "", "the confirm clears the field")
	assert_false(driver.is_widget_checked(create), "the confirm unchecks CREATE_WAYPOINTS")
	assert_false(map_window.get_command_toggle(MapViewWindow.COMMAND_TOGGLE_CREATE_WAYPOINTS))
	assert_false(driver.is_widget_shown(dialog), "the dialog hides by its action")
	driver.activate(driver.widget_id("CLEAR_WAYPOINTS"))
	assert_eq(map_window.get_waypoint_count(), 0, "CLEAR_WAYPOINTS removes it")


# The chat panel's send reads CHAT_TEXT and clears it; the go-code buttons
# route to the session (the bare local role has no wire).
func test_chat_send_and_go_codes() -> void:
	var presenter := _make_presenter()
	var hud := HudOverlay.new()
	hud.size = Vector2(800, 600)
	add_child_autofree(hud)
	presenter.set_hud_source(func() -> HudOverlay: return hud)
	assert_true(presenter.open())
	var driver := presenter.get_menu_driver()
	var chat := driver.widget_id("CHAT_TEXT")
	driver.set_widget_text(chat, "on me")
	driver.activate(driver.widget_id("CHAT_TEAM"))
	assert_eq(driver.get_widget_text(chat), "", "the send clears the line")
	driver.activate(driver.widget_id("GOCODE_UNIFORM"))
	assert_true(presenter.is_open())


# The CMAP view state is the presenter's: a menu rebuild keeps the zoom (the
# CMAP show runs no fit).
func test_the_view_state_outlives_the_menu() -> void:
	var presenter := _make_presenter()
	assert_true(presenter.open())
	var driver := presenter.get_menu_driver()
	driver.activate(driver.widget_id("ZOOMIN"))
	presenter.teardown()
	assert_true(presenter.open())
	var map_window := presenter.get_map_window()
	assert_not_null(map_window)
	if map_window != null:
		assert_eq(map_window.get_view_state(), presenter.get_map_view_state())
		assert_almost_eq(map_window.get_zoom(), 3.4, 0.0001)


# The ORDERS radio fills LOCATION (My Position first; the bare local role has
# no squad, banks or locations); NEW_ORDER composes the group, command and
# location rows into a CURRENT_ORDERS row (the delete box "0", the text); the
# delete box's press cancels the order and removes its row; ADDTO_* with no
# selected member sends nothing [orig: CMap_OnOpenPopulate @0x5492a0;
# CMap_BuildAndSendOrderCommand @0x5472d0; CCommandMap_HandleOrderAction
# @0x548990].
func test_the_orders_composer() -> void:
	var presenter := _make_presenter()
	assert_true(presenter.open())
	var driver := presenter.get_menu_driver()
	driver.activate(driver.widget_id("RADIO_TAB_ORDERS"))
	var location := driver.widget_id("LOCATION")
	assert_eq(driver.get_widget_items(location), PackedStringArray(["My Position"]))
	driver.activate(driver.widget_id("NEW_ORDER"))
	var orders := driver.widget_id("CURRENT_ORDERS")
	assert_eq(driver.table_row_count(orders), 1, "NEW_ORDER lists the order")
	assert_eq(driver.table_cell_text(orders, 0, 0), "0")
	assert_eq(driver.table_cell_text(orders, 0, 1), "-Halt-My Position")
	driver.activate(driver.widget_id("ADDTO_FIRETEAM_A"))
	driver.emit_signal("table_cell_clicked", orders, "CURRENT_ORDERS", 0, 0, 3, 0, false)
	assert_eq(driver.table_row_count(orders), 0, "the delete box cancels the order")
	assert_true(presenter.is_open())


# The show populates both tables (empty on the bare local role) and the
# session's end empties the order store: a reopened screen lists nothing.
func test_the_tables_populate_and_the_store_ends_with_the_session() -> void:
	var presenter := _make_presenter()
	assert_true(presenter.open())
	var driver := presenter.get_menu_driver()
	assert_eq(driver.table_row_count(driver.widget_id("TEAMLIST")), 0)
	assert_eq(driver.table_row_count(driver.widget_id("PLAYERLIST")), 0)
	driver.activate(driver.widget_id("RADIO_TAB_ORDERS"))
	driver.activate(driver.widget_id("NEW_ORDER"))
	presenter.teardown()
	assert_true(presenter.open())
	driver = presenter.get_menu_driver()
	assert_eq(driver.table_row_count(driver.widget_id("CURRENT_ORDERS")), 0)


# CHAT_MSGS is the frame's custom-draw slot (its CUSTOM appearance pass is
# where the HUD's console messages draw) [orig: CMap_OnChatMsgsCustomDraw
# @0x5482d0].
func test_chat_msgs_is_the_custom_draw_slot() -> void:
	var presenter := _make_presenter()
	var hud := HudOverlay.new()
	hud.size = Vector2(800, 600)
	add_child_autofree(hud)
	presenter.set_hud_source(func() -> HudOverlay: return hud)
	assert_true(presenter.open())
	var driver := presenter.get_menu_driver()
	var frame: MenuFrame = driver.get_frame()
	assert_eq(frame.get_custom_slot_widget(), driver.frame_index(driver.widget_id("CHAT_MSGS")))
	assert_true(frame.get_custom_slot_canvas_item().is_valid(), "the slot has its canvas item")
