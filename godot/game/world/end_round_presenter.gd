class_name EndRoundPresenter
extends Node

## The multiplayer end-of-round presentation (net-re §5.68): from the S2C 0x1D
## announcement the HUD overlay shows the Impact38 text ladder over the whole
## frame (headline, game-type line, score lines, game time) while the deploy /
## armory screens are torn down; six seconds later, once the S2C 0x56 board
## has reassembled, stat.mnu's STAT screen opens ONCE with its RESULTLIST
## table filled, the RADIO_TAB_* trio hidden for non-team modes, and the tab
## filter + the HIDDEN_BACK / CONFIRM_* exits wired.
## [orig: UI_ProcessEndRoundScreenTransition @0x5b8600 (every HUD frame while
##  g_spawn_success_gate && is_in_session from sub_5C0060 @0x5c0072): first pass
##  Server_ResetBalanceCounters + Game_InitRespawnState +
##  Overlay_ComputeStatFieldColumnLayout(40, 984) + byte_28E561C; every pass
##  sub_54E650 (the UI scene teardown) then draw_endround_stats_overlay
##  @0x5b7cd0; g_scoreboardDirty && now - dword_A81B2C >= 6000 ms ->
##  UI_OpenMenuScreen("stat.mnu", "STAT") once (byte_28E561D); the STAT show
##  callback sub_562840 (populate + tab visibility); stat_filter_tab_handler
##  @0x562140; both once-only bytes cleared by sub_5B71B0 at Game_StartMission
##  @0x525903]

const MENU_FILE := "stat.mnu"
const MENU_SCREEN := "STAT"
const STYLESHEET_FILE := "menu_style.mns"
const RESULT_LIST := "RESULTLIST"
const MUSIC_VAR_INDEX := MusicDirector.MENU_MUSIC_VAR_SLOT
# The stat-screen delay: 6000 ms after the announcement [orig: 0x1770 @0x5b8615].
const STAT_SCREEN_DELAY_MSEC := 6000
# The overlay safe area retail stamps at display-mode set (dword_24C1900/04);
# the full design frame here.
const OVERLAY_TOP := 0
const OVERLAY_BOTTOM := 768
# The RESULTLIST's authored width (jo_stat.mnu: the STATS window spans 20..770)
# when the compiled frame has not laid the table out yet.
const RESULT_LIST_DEFAULT_WIDTH := 750

signal opened
signal closed

var _world: GameWorld = null
var _ui_parent: Node = null
var _layout_control: Control = null
var _hud_presenter: GameHudPresenter = null
var _deploy_presenter: DeployScreenPresenter = null
var _armory_presenter = null
var _frame: MenuFrame = null
var _audio: MenuAudio = null
var _driver: MenuDriver = null
var _header_seen := false
var _header_edge_msec := 0
var _stat_opened := false
var _overlay_shown := false
var _last_game_type := 0


func setup(world: GameWorld, ui_parent: Node, hud_presenter: GameHudPresenter) -> void:
	_world = world
	_ui_parent = ui_parent
	_layout_control = ui_parent as Control
	_hud_presenter = hud_presenter


## The shell seam: the announcement edge closes the deploy/armory screens (the
## UI scene teardown every transition pass runs [orig: sub_54E650 @0x5b8674]);
## `opened`/`closed` report the STAT screen's cursor ownership through the two
## callables (the overlay phase is not a UI state: the world keeps the cursor).
func connect_shell(deploy_presenter: DeployScreenPresenter, armory_presenter,
		on_opened: Callable, on_closed: Callable) -> void:
	_deploy_presenter = deploy_presenter
	_armory_presenter = armory_presenter
	opened.connect(func() -> void: on_opened.call())
	closed.connect(func() -> void: on_closed.call())


## Build + wire the presenter under `parent` in one call (the shell's seam).
static func install(parent: Node, world: GameWorld, ui_parent: Node,
		hud_presenter: GameHudPresenter, deploy_presenter: DeployScreenPresenter,
		armory_presenter, on_opened: Callable, on_closed: Callable) -> EndRoundPresenter:
	var presenter := EndRoundPresenter.new()
	presenter.name = "EndRoundPresenter"
	parent.add_child(presenter)
	presenter.setup(world, ui_parent, hud_presenter)
	presenter.connect_shell(deploy_presenter, armory_presenter, on_opened, on_closed)
	return presenter


# The UI scene teardown at the announcement [orig: sub_54E650 @0x5b8674].
func _tear_down_screens() -> void:
	if _deploy_presenter != null and _deploy_presenter.is_open():
		_deploy_presenter.close()
	if _armory_presenter != null and _armory_presenter.is_open():
		_armory_presenter.close()


func _hud() -> HudOverlay:
	return _hud_presenter.get_game_hud() if _hud_presenter != null else null


func is_open() -> bool:
	return _frame != null and is_instance_valid(_frame) and _frame.visible


func is_overlay_shown() -> bool:
	return _overlay_shown


func get_menu_driver() -> MenuDriver:
	return _driver


## Mission (re)start clears the once-only latches [orig: sub_5B71B0 @0x525903].
func reset() -> void:
	_header_seen = false
	_stat_opened = false
	_hide_overlay()
	if is_open():
		_frame.visible = false


## One HUD frame: the announcement edge shows the overlay (and reports it so
## the shell can tear the deploy/armory screens down); the 6 s + board gate
## opens STAT once.
func tick() -> void:
	if _world == null:
		return
	var sim: Simulation = _world.get_sim()
	if sim == null:
		return
	var state: Dictionary = sim.get_end_round_state()
	if not bool(state.get("header_known", false)):
		if _header_seen:
			reset()
		return
	if not _header_seen:
		_header_seen = true
		_header_edge_msec = Time.get_ticks_msec()
		_stat_opened = false
		_last_game_type = int(state.get("game_type", 0))
	_tear_down_screens()
	if not _stat_opened:
		_apply_overlay(sim)
		if bool(state.get("board_known", false)) \
				and Time.get_ticks_msec() - _header_edge_msec >= STAT_SCREEN_DELAY_MSEC:
			_open_stat_screen(sim)
	elif is_open():
		_driver.tick(Time.get_ticks_msec())


func _process(_delta: float) -> void:
	if is_open() and _driver != null:
		_driver.tick(Time.get_ticks_msec())


# The overlay ladder: resolve every line's key through the gametext Overlays
# table (empty resolves fall to the fallback like GameText_GetStringWithFallback),
# format the printf arguments, and hand the text + y pairs to the HUD element.
func _apply_overlay(sim: Simulation) -> void:
	var hud := _hud()
	if hud == null:
		return
	var texts := PackedStringArray()
	var ys := PackedInt32Array()
	for value in sim.get_end_round_lines():
		var line := value as Dictionary
		var fmt := _resolve(String(line.get("key", "")), String(line.get("fallback", "")),
				String(line.get("literal", "")))
		var args: Array = []
		for raw in line.get("args", []):
			var a := raw as Dictionary
			if bool(a.get("is_number", false)):
				args.append(int(a.get("number", 0)))
			else:
				args.append(_resolve(String(a.get("key", "")), String(a.get("fallback", "")),
						String(a.get("literal", ""))))
		var text := fmt
		if not args.is_empty():
			# Retail's printf: %s / %ld / %d / %02d.
			var g := fmt.replace("%ld", "%d")
			text = g % args if g.count("%") == args.size() else fmt
		texts.append(text)
		ys.append(int(line.get("y", 0)))
	hud.set_end_round_overlay(true, OVERLAY_TOP, OVERLAY_BOTTOM, texts, ys)
	_overlay_shown = true


func _hide_overlay() -> void:
	var hud := _hud()
	if hud != null and _overlay_shown:
		hud.set_end_round_overlay(false, OVERLAY_TOP, OVERLAY_BOTTOM,
				PackedStringArray(), PackedInt32Array())
	_overlay_shown = false


func _resolve(key: String, fallback: String, literal: String) -> String:
	if key.is_empty():
		return literal
	var t: RtxtStringFile = Strings.get_table("gametext")
	if t != null and t.has_string_in_section("Overlays", key):
		var s := t.get_string_in_section("Overlays", key)
		if not s.is_empty():
			return s
	# The "!..." fallback marker is stripped like every other fallback string.
	return fallback.trim_prefix("!")


func _open_stat_screen(sim: Simulation) -> void:
	_stat_opened = true
	_hide_overlay()
	if not _ensure_menu():
		return
	_populate(sim)
	_ui_parent.move_child(_frame, _ui_parent.get_child_count() - 1)
	_frame.visible = true
	set_process(true)
	opened.emit()


# The STAT show callback [orig: sub_562840]: populate the RESULTLIST, then
# hide the three tab radios for non-team modes (team modes select OVERALL).
func _populate(sim: Simulation) -> void:
	var list_id := _driver.widget_id(RESULT_LIST)
	if list_id < 0:
		return
	var rect := _driver.widget_frame_rect(list_id)
	var table_width := int(rect.size.x) if rect.size.x > 0.0 else RESULT_LIST_DEFAULT_WIDTH
	_driver.table_clear_rows(list_id)
	var team_mode := (_last_game_type & 0x10000) != 0
	for tab in ["RADIO_TAB_OVERALL", "RADIO_TAB_REDTEAM", "RADIO_TAB_BLUETEAM"]:
		var id := _driver.widget_id(tab)
		if id >= 0:
			_driver.set_widget_shown(id, team_mode)
	if team_mode:
		var overall := _driver.widget_id("RADIO_TAB_OVERALL")
		if overall >= 0:
			_driver.set_widget_checked(overall, true)
	# The header row: NAME (the rtxt "NAME" lookup or "!Name"), "Squad", then
	# the field labels through the Overlays table.
	var headers := PackedStringArray()
	for value in sim.get_end_round_columns(table_width):
		var col := value as Dictionary
		var key := String(col.get("header_key", ""))
		var fallback := String(col.get("header_fallback", ""))
		headers.append(_resolve(key, fallback, String(col.get("literal", ""))) \
				if not key.is_empty() else fallback.trim_prefix("!"))
	_driver.table_add_row(list_id, headers)
	var row_index := 1
	var selected_row := -1
	for value in sim.get_end_round_rows():
		var row := value as Dictionary
		var cells := PackedStringArray([String(row.get("name", "")), String(row.get("squad", "-"))])
		cells.append_array(PackedStringArray(row.get("cells", PackedStringArray())))
		_driver.table_add_row(list_id, cells)
		if bool(row.get("selected", false)):
			selected_row = row_index
		row_index += 1
	if selected_row >= 0:
		_driver.table_select_row(list_id, selected_row)


func close() -> void:
	set_process(false)
	if not is_open():
		return
	_frame.visible = false
	closed.emit()


func teardown() -> void:
	set_process(false)
	_hide_overlay()
	if _frame != null and is_instance_valid(_frame):
		_frame.queue_free()
	if _audio != null and is_instance_valid(_audio):
		_audio.queue_free()
	_frame = null
	_audio = null
	_driver = null
	_header_seen = false
	_stat_opened = false


# The stat.mnu exits: HIDDEN_BACK and the CONFIRM_YES/CONFIRM_NO pair close the
# screen (the round cycle itself is the host's).
func _on_widget_value_changed(widget_name: String, kind: String, index: int,
		_value: String) -> void:
	if kind == "button" and (widget_name.nocasecmp_to("HIDDEN_BACK") == 0
			or widget_name.nocasecmp_to("CONFIRM_YES") == 0
			or widget_name.nocasecmp_to("CONFIRM_NO") == 0
			or widget_name.nocasecmp_to("CONFIRM_EXIT") == 0):
		close()
		return
	# The tab filter [orig: stat_filter_tab_handler @0x562140]: tab 0 shows
	# every row, tab 1 only team 2, tab 2 only team 1 — applied by re-filling
	# the table with the filtered rows (the compiled table has no per-row hide).
	if kind == "radio" and widget_name.begins_with("RADIO_TAB_"):
		var tab := 0
		if widget_name.nocasecmp_to("RADIO_TAB_REDTEAM") == 0:
			tab = 1
		elif widget_name.nocasecmp_to("RADIO_TAB_BLUETEAM") == 0:
			tab = 2
		_apply_tab_filter(tab)


func _apply_tab_filter(tab: int) -> void:
	var sim: Simulation = _world.get_sim() if _world != null else null
	if sim == null or _driver == null:
		return
	var list_id := _driver.widget_id(RESULT_LIST)
	if list_id < 0:
		return
	var rect := _driver.widget_frame_rect(list_id)
	var table_width := int(rect.size.x) if rect.size.x > 0.0 else RESULT_LIST_DEFAULT_WIDTH
	_driver.table_clear_rows(list_id)
	var headers := PackedStringArray()
	for value in sim.get_end_round_columns(table_width):
		var col := value as Dictionary
		var key := String(col.get("header_key", ""))
		headers.append(_resolve(key, String(col.get("header_fallback", "")),
				String(col.get("literal", ""))) if not key.is_empty()
				else String(col.get("header_fallback", "")).trim_prefix("!"))
	_driver.table_add_row(list_id, headers)
	for value in sim.get_end_round_rows():
		var row := value as Dictionary
		var team := int(row.get("team", 0))
		var visible := true
		if tab == 1:
			visible = team == 2
		elif tab == 2:
			visible = team == 1
		if not visible:
			continue
		var cells := PackedStringArray([String(row.get("name", "")), String(row.get("squad", "-"))])
		cells.append_array(PackedStringArray(row.get("cells", PackedStringArray())))
		_driver.table_add_row(list_id, cells)


func _ensure_menu() -> bool:
	if _driver != null and _frame != null and is_instance_valid(_frame):
		return true
	var root: ResourceRoot = _world.get_resource_root()
	if root == null:
		return false
	var bytes := root.read_file(MENU_FILE)
	if bytes.is_empty():
		push_warning("EndRoundPresenter: %s not found in the resource root" % MENU_FILE)
		return false
	var doc := MnuDocument.new()
	if doc.load_from_bytes(bytes) != OK:
		push_warning("EndRoundPresenter: %s did not parse" % MENU_FILE)
		return false
	_frame = MenuFrame.new()
	_frame.name = "EndRoundMenu"
	_frame.set_anchors_preset(Control.PRESET_TOP_LEFT)
	_frame.mouse_filter = Control.MOUSE_FILTER_STOP
	_frame.gui_input.connect(_on_frame_gui_input)
	_ui_parent.add_child(_frame)
	_recompute_fit()
	_audio = MenuAudio.new()
	_audio.name = "EndRoundMenuAudio"
	_audio.set_resource_root(root)
	_ui_parent.add_child(_audio)
	_driver = MenuDriver.new()
	_driver.attach(_frame, _audio)
	_driver.set_music_director(NovaMusicService.director())
	_driver.set_music_var_index(MUSIC_VAR_INDEX)
	_driver.widget_value_changed.connect(_on_widget_value_changed)
	var style := _load_style(root)
	var menu_text: RtxtStringFile = Strings.get_table("menutxt")
	if not _driver.open_document(doc, root, style, menu_text, MENU_FILE, MENU_SCREEN):
		push_warning("EndRoundPresenter: %s has no screens" % MENU_FILE)
		teardown()
		return false
	return true


func _on_frame_gui_input(event: InputEvent) -> void:
	if _driver == null or not is_open():
		return
	if event is InputEventMouseMotion:
		var motion := event as InputEventMouseMotion
		_driver.process_mouse(motion.position,
				(motion.button_mask & MOUSE_BUTTON_MASK_LEFT) != 0)
	elif event is InputEventMouseButton:
		var button := event as InputEventMouseButton
		if button.button_index == MOUSE_BUTTON_LEFT:
			_driver.process_mouse(button.position, button.pressed)
			_frame.accept_event()


func _load_style(root: ResourceRoot) -> MnsStyleSheet:
	var bytes := root.read_file(STYLESHEET_FILE)
	if bytes.is_empty():
		return null
	var s := MnsStyleSheet.new()
	return s if s.load_from_bytes(bytes) == OK and s.is_runtime_valid() else null


func _recompute_fit() -> void:
	if _frame == null or not is_instance_valid(_frame):
		return
	var target_size := Vector2.ZERO
	if _layout_control != null:
		target_size = _layout_control.size
	elif _ui_parent != null and _ui_parent.get_viewport() != null:
		target_size = _ui_parent.get_viewport().get_visible_rect().size
	if target_size.x <= 1.0 or target_size.y <= 1.0:
		return
	_frame.position = Vector2.ZERO
	_frame.size = target_size
