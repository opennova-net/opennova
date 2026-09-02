class_name EndRoundPresenter
extends Node

## The multiplayer end-of-round presentation (net-re §5.68): from the S2C 0x1D
## announcement the HUD overlay shows the Impact38 text ladder over the whole
## frame (headline, game-type line, score lines, game time) while the deploy /
## armory screens are torn down; six seconds later, once the S2C 0x56 board
## has reassembled, stat.mnu's STAT screen opens ONCE with its RESULTLIST
## table filled, the RADIO_TAB_* trio hidden for non-team modes, and the tab
## filter + the HIDDEN_BACK / CONFIRM_* exits wired.
## The ladder's key selection, the empty-resolve folds, the printf forms, the
## column headers, the tab filter and the 6 s delay are the engine's
## (hud/end_round_overlay.h, npruntime/stat_screen_feed.h through the
## Simulation feeds); this node owns only the device work: the HUD element,
## the compiled stat.mnu frame, its widgets and the cursor.
## [orig: UI_ProcessEndRoundScreenTransition @0x5b8600 (every HUD frame while
##  g_spawn_success_gate && is_in_session from HUD_DrawOverlayPanels @0x5c0072): first pass
##  Server_ResetBalanceCounters + Game_InitRespawnState +
##  Overlay_ComputeStatFieldColumnLayout(40, 984) + byte_28E561C; every pass
##  UI_TeardownScene (ex sub_54E650) (the UI scene teardown) then draw_endround_stats_overlay
##  @0x5b7cd0; g_scoreboardDirty && now - dword_A81B2C >= 6000 ms ->
##  UI_OpenMenuScreen("stat.mnu", "STAT") once (byte_28E561D); the STAT show
##  callback StatScreen_ShowCallback (ex sub_562840) (populate + tab visibility); stat_filter_tab_handler
##  @0x562140; both once-only bytes cleared by Game_InitMissionRoundState (ex sub_5B71B0) at Game_StartMission
##  @0x525903]

const MENU_FILE := "stat.mnu"
const MENU_SCREEN := "STAT"
const STYLESHEET_FILE := "menu_style.mns"
const RESULT_LIST := "RESULTLIST"
const MUSIC_VAR_INDEX := MusicDirector.MENU_MUSIC_VAR_SLOT
# The RESULTLIST's authored width (jo_stat.mnu: the STATS window spans 20..770)
# when the compiled frame has not laid the table out yet — a device fallback
# for a frame that has not measured its widget.
const RESULT_LIST_DEFAULT_WIDTH := 750
# The stat.mnu tab radios, in the engine's tab order (0 all, 1 team 2, 2 team 1).
const TAB_WIDGETS: Array[String] = ["RADIO_TAB_OVERALL", "RADIO_TAB_REDTEAM", "RADIO_TAB_BLUETEAM"]

signal opened
signal closed
# The witnessed CONFIRM_YES command exits the mission (the same close-screens
# + action-3 pair as the pause menu's CONFIRM_YES; docs/mnu/menu-re.md "The
# in-game exit confirmation"); the shell routes it to the return-to-menu
# teardown.
signal exit_to_menu_requested

var _world: GameWorld = null
var _ui_parent: Node = null
var _layout_control: Control = null
var _hud_presenter: GameHudPresenter = null
var _deploy_presenter: DeployScreenPresenter = null
var _armory_presenter: ArmoryPresenter = null
var _frame: MenuFrame = null
var _audio: MenuAudio = null
var _driver: MenuDriver = null
var _header_seen := false
var _header_edge_msec := 0
var _stat_opened := false
var _overlay_shown := false
var _team_mode := false


func setup(world: GameWorld, ui_parent: Node, hud_presenter: GameHudPresenter) -> void:
	_world = world
	_ui_parent = ui_parent
	_layout_control = ui_parent as Control
	_hud_presenter = hud_presenter


## The shell seam: the announcement edge closes the deploy/armory screens (the
## UI scene teardown every transition pass runs [orig: UI_TeardownScene @0x5b8674]);
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


# The UI scene teardown at the announcement [orig: UI_TeardownScene @0x5b8674].
func _tear_down_screens() -> void:
	if _deploy_presenter != null and _deploy_presenter.is_open():
		_deploy_presenter.close()
	if _armory_presenter != null and _armory_presenter.is_open():
		_armory_presenter.close()


func _hud() -> HudOverlay:
	return _hud_presenter.get_game_hud() if _hud_presenter != null else null


func _gametext() -> RtxtStringFile:
	return Strings.get_table(Strings.TABLE_GAMETEXT)


func is_open() -> bool:
	return _frame != null and is_instance_valid(_frame) and _frame.visible


func is_overlay_shown() -> bool:
	return _overlay_shown


func get_menu_driver() -> MenuDriver:
	return _driver


## Mission (re)start clears the once-only latches [orig: Game_InitMissionRoundState @0x525903].
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
	var state: EndRoundState = sim.get_end_round_state()
	if not state.is_header_known():
		if _header_seen:
			reset()
		return
	if not _header_seen:
		_header_seen = true
		_header_edge_msec = Time.get_ticks_msec()
		_stat_opened = false
		_team_mode = state.is_team_mode()
	if not _stat_opened:
		# Only the PRE-STAT phase tears the deploy/armory scene down (every
		# pass) and draws the overlay; once the STAT phase latches, retail's
		# transition returns immediately each frame — no teardown, no overlay
		# — and after the player closes stat.mnu NOTHING from this path
		# redraws until the host's round cycle exits the mission.
		# [orig: UI_ProcessEndRoundScreenTransition @0x5b8600 — the locret
		#  @0x5b864a once byte_28E561D is set; UI_TeardownScene @0x5b8674 pre-STAT
		#  and once more on the open pass @0x5b862a]
		_tear_down_screens()
		_apply_overlay(sim)
		if state.is_board_known() \
				and Time.get_ticks_msec() - _header_edge_msec \
						>= Simulation.end_round_stat_screen_delay_msec():
			_tear_down_screens()
			_open_stat_screen(sim)
	elif is_open():
		_driver.tick(Time.get_ticks_msec())


func _process(_delta: float) -> void:
	if is_open() and _driver != null:
		_driver.tick(Time.get_ticks_msec())


# The resolved overlay ladder (the engine's end_round_overlay_resolve over the
# gametext Overlays table) handed to the HUD element as text + y pairs.
func _apply_overlay(sim: Simulation) -> void:
	var hud := _hud()
	if hud == null:
		return
	var overlay := sim.get_end_round_overlay(_gametext())
	hud.set_end_round_overlay(true, overlay.top, overlay.bottom, overlay.texts, overlay.ys)
	_overlay_shown = true


func _hide_overlay() -> void:
	var hud := _hud()
	if hud != null and _overlay_shown:
		var sim: Simulation = _world.get_sim() if _world != null else null
		var overlay: EndRoundOverlay = (
				sim.get_end_round_overlay(null) if sim != null else EndRoundOverlay.new())
		hud.set_end_round_overlay(false, overlay.top, overlay.bottom,
				PackedStringArray(), PackedInt32Array())
	_overlay_shown = false


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


# The STAT show callback [orig: StatScreen_ShowCallback @0x562840 (ex
# sub_562840)]: fill the RESULTLIST, then hide the three tab radios for
# non-team modes (team modes select OVERALL). The team-mode arm is the sim
# state's `team_mode` (world/game_type.h).
func _populate(sim: Simulation) -> void:
	var list_id := _driver.widget_id(RESULT_LIST)
	if list_id < 0:
		return
	for tab in TAB_WIDGETS:
		var id := _driver.widget_id(tab)
		if id >= 0:
			_driver.set_widget_shown(id, _team_mode)
	if _team_mode:
		var overall := _driver.widget_id(TAB_WIDGETS[0])
		if overall >= 0:
			_driver.set_widget_checked(overall, true)
	_fill_table(sim, list_id, 0)


# One table fill: the resolved header row, then the rows the engine's tab
# filter admits (0 all, 1 team 2, 2 team 1), the local player's row selected.
func _fill_table(sim: Simulation, list_id: int, tab: int) -> void:
	var rect := _driver.widget_frame_rect(list_id)
	var table_width := int(rect.size.x) if rect.size.x > 0.0 else RESULT_LIST_DEFAULT_WIDTH
	_driver.table_clear_rows(list_id)
	var headers := PackedStringArray()
	for column: EndRoundColumn in sim.get_end_round_columns(table_width, _gametext()):
		headers.append(column.header)
	_driver.table_add_row(list_id, headers)
	var row_index := 1
	var selected_row := -1
	for row: EndRoundRow in sim.get_end_round_rows(tab):
		var cells := PackedStringArray([row.name, row.squad])
		cells.append_array(row.cells)
		_driver.table_add_row(list_id, cells)
		if row.selected:
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


# Buttons and radios arrive on the driver's widget_activated (value_changed
# never fires for them). The stat.mnu exit is confirmed, like the shipped
# screen: HIDDEN_BACK's authored actions raise the CONFIRM_EXIT "Are you
# sure?" panel (SHOW CONFIRM_EXIT + HIDE STATS), CONFIRM_NO's restore STATS,
# and the CONFIRM_YES Command EXITS THE MISSION — the witnessed handler is the
# same close-screens + action-3 pair as the pause menu's (docs/mnu/menu-re.md
# "The in-game exit confirmation"), not a board hide. Closing on the other
# names swallowed the confirmation.
func _on_widget_activated(_id: int, widget_name: String) -> void:
	if widget_name.nocasecmp_to("CONFIRM_YES") == 0:
		close()
		exit_to_menu_requested.emit()
		return
	# The tab radios map onto the engine's tab index [orig:
	# stat_filter_tab_handler @0x562140, registered with params 0/1/2 by
	# HUD_CacheStatPanelValues @0x5627a8..0x5627f5]; the filter itself is the
	# sim feed's.
	if widget_name.begins_with("RADIO_TAB_"):
		var tab := 0
		for i in TAB_WIDGETS.size():
			if widget_name.nocasecmp_to(TAB_WIDGETS[i]) == 0:
				tab = i
		var sim: Simulation = _world.get_sim() if _world != null else null
		var list_id := _driver.widget_id(RESULT_LIST) if _driver != null else -1
		if sim != null and list_id >= 0:
			_fill_table(sim, list_id, tab)


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
	_driver.set_music_director(MusicService.director())
	_driver.set_music_var_index(MUSIC_VAR_INDEX)
	_driver.widget_activated.connect(_on_widget_activated)
	var style := MenuFrameSurface.load_style(root, STYLESHEET_FILE)
	var menu_text: RtxtStringFile = Strings.get_table("menutxt")
	if not _driver.open_document(doc, root, style, menu_text, MENU_FILE, MENU_SCREEN):
		push_warning("EndRoundPresenter: %s has no screens" % MENU_FILE)
		teardown()
		return false
	return true


func _on_frame_gui_input(event: InputEvent) -> void:
	if _driver == null or not is_open():
		return
	MenuFrameSurface.forward_gui_input(event, _driver, _frame)


func _recompute_fit() -> void:
	# MenuFrameSurface.fit_frame (shared with the other presenters).
	MenuFrameSurface.fit_frame(_frame, _layout_control, _ui_parent)
