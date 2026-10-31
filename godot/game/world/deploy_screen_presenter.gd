class_name DeployScreenPresenter
extends Node

## The deploy-map screen (death.mnu's DEATH screen) handles the authority's
## initial deployment hold and later death picks. The show hides DEATH_SHROUD
## (the window holding the MAP, the list and the statics); it reveals at once
## under the deploy overlay, else 240 ticks after the death, and the content
## refreshes only while it is revealed, on each 16-tick boundary (the engine's
## world/deploy_screen_feed.h death_shroud_revealed / deploy_refresh_due).
## SPAWNPOINTS_LIST carries the Default Spawn row
## plus one lettered row per team-owned SECURED zone. Selection always queues a
## spawn request. Initial selection closes the dialog immediately; death re-picks
## remain available until the server releases its pending hold because invalid
## or contested picks can be silently dropped.
## [orig: death.mnu <NAME>DEATH</NAME>; DeathScreen_UpdateShroudReveal @0x554730 (shroud reveal:
##  immediate on g_DeployScreenActive, 240 ticks after death; content refresh
##  every 16 ticks); UI_UpdateDeathScreenContent @0x5536a0 (list populate:
##  row 0 "'<DEFAULT_SPAWN_KEY>' <HOME>" node 0, then per zone
##  "'<A+idx>' <WPNames/STRWPNAME%03d>" node idx+1, team color tags <c4040FF>/
##  <cFF2020>, the whole-list text sort, then the per-zone wave occupant rows
##  + blank separator with node -1 — the engine builder world/deploy_screen_feed
##  owns both loops; the STATIC_RESPAWN_MSG1 penalty/wave line, the
##  STATIC_PSPRESPAWN_MSG1 hold, the STATIC_MEDIC_MSG1/STATIC_CALLMEDIC_MSG
##  revive-window pair); UI_RegisterDeathScreenCallbacks @0x554610 (SPAWNPOINTS_LIST
##  select -> Input_QueueEvent(12, node) @0x55364d, guarded on node != -1);
##  DeathScreen_UpdateUI @0x553150 (map zoom fit from the zone AABB,
##  SWAP_TEAMS/BUTTON_TEAMLIST by the engine's deploy_team_buttons_shown);
##  DeathScreen_OnSwapTeams @0x5535B0 (the SWAP_TEAMS click)]
##
## The MAP window hosts the windowed map view (MapViewWindow over the engine's
## DeathMapView and the HUD compiler's DEATH pass, hud/hud_map_view.h): the
## terrain, the marker walk, the zone blips and letters and the player
## crosshair, with left-drag pan, right-drag / wheel zoom, the show-time zoom
## fit against the DEATH_SHROUD window and the per-tick pan ease. The view
## state is this presenter's MapViewState, which outlives the per-mission menu
## rebuild like retail's globals. Witnesses: hud-re D-HUD-19.

const MENU_FILE := "death.mnu"
const MENU_SCREEN := "DEATH"
const SPAWN_LIST := "SPAWNPOINTS_LIST"
# The table a title whose DEATH screen picks the loadout lists its spawn points in.
const SPAWN_TABLE := "SPAWNPOINTS_TABLE"
# The custom-draw MAP window and the shroud window whose authored size the
# show-time zoom fit reads (hud/hud_map_view.h DeathMapView::fit).
const MAP_WIDGET := "MAP"
const SHROUD_WIDGET := "DEATH_SHROUD"
# The screen's root window a title hides and shows with the shroud (DeployStatus.shroud_main).
const MAIN_WIDGET := "MAIN"
# The team-service pair, shown together; only SWAP_TEAMS has a DEATH callback.
const SWAP_TEAMS := "SWAP_TEAMS"
const TEAM_LIST := "BUTTON_TEAMLIST"
# The LEAVE MISSION button (its Esc hotkey shows CONFIRM_EXIT) and the
# confirmation's NO and YES.
const HIDDEN_BACK := "HIDDEN_BACK"
const CONFIRM_NO := "CONFIRM_NO"
const CONFIRM_YES := "CONFIRM_YES"
# A title whose DEATH screen picks the loadout: its off-screen R-hotkeyed restart and the three
# lists whose picks the armory's handlers take.
const RESTART_LEVEL := "RESTART_LEVEL"
const LOADOUT_LISTS: Array[String] = ["PRIMARY", "SECONDARY", "ACCESSORY"]

signal opened
signal closed
## The exit confirmation's YES, after the screen closed: the shell runs input
## action 3 (the Exit Mission tone, then the exit; engine
## world::pass_through_close_axis carries the key witness).
signal exit_confirmed
## RESTART_LEVEL's click past its gate, after the screen closed and the pick 0 went: the shell
## runs the in-game RESTART's exit (engine world::deploy_restart_level_allowed carries the
## witness).
signal restart_requested

var _view: WorldView = null
var _ui_parent: Node = null
# The layout source, converted ONCE at setup: a Control parent (test overlays)
# drives the fit from its own size/resized; a CanvasLayer parent (the game HUD)
# has no size, so the fit follows the viewport instead.
var _layout_control: Control = null
# The compiled menu surface: the frame draws + hit-tests, the audio node plays
# widget SFX, and the RefCounted driver orchestrates both over the parsed
# document (freed with the presenter; only the two nodes need queue_free).
var _frame: MenuFrame = null
var _audio: MenuAudio = null
var _driver: MenuDriver = null
# The logic tick the last frame sampled: the content refresh runs when the
# ticks since reach a 16-tick boundary (Simulation.deploy_refresh_due).
var _last_tick := 0
# The spawn list's presenter-side row model, aligned with the compiled list's
# visible rows: {label, param} per row, rebuilt by _populate_spawn_list. The
# compiled list carries labels only, so the node parameter the pick serializes
# lives here (the old ItemList's item metadata).
## One visible list row: its stripped label and the node PARAM the pick sends
## (0 default, index + 1 zone, -1 occupant/blank — never a pick).
class SpawnRow extends RefCounted:
	var label: String
	var param: int

	func _init(p_label: String, p_param: int) -> void:
		label = p_label
		param = p_param

var _spawn_rows: Array[SpawnRow] = []
# The MAP window host mounted over the MAP widget (null until the menu builds
# on a screen that authors MAP), and the shell's HUD overlay getter: the map
# pass compiles over the HUD's terrain / markers / fonts.
var _map_window: MapViewWindow = null
var _hud_source: Callable = Callable()
# The DEATH map's pan/zoom state, held for the presenter's (the process's)
# lifetime and handed to every MapViewWindow the menu rebuild mounts.
var _map_view_state := MapViewState.new()
# The menu input pass-through word while DEATH is up: its show raises it, the
# exit confirmation (HIDDEN_BACK) lowers it and the confirmation's NO raises it
# again; the close leaves it as it stands (engine world::pass_through_close_axis
# carries the witness).
var _input_pass_through := false
# Whether MAIN hides and shows with the shroud (the title's, read at the open).
var _shroud_main := false


func setup(view: WorldView, ui_parent: Node) -> void:
	_view = view
	_ui_parent = ui_parent
	_layout_control = ui_parent as Control
	MenuFrameSurface.connect_layout_source(_layout_control, _ui_parent, _recompute_fit)


# The armory presenter whose class pages the DEATH screen's loadout picker fills and applies
# (the title's death_screen_loadout, DFX2-41); null: no picker.
var _armory: ArmoryPresenter = null


func set_armory(armory: ArmoryPresenter) -> void:
	_armory = armory


# Whether the screen's title picks the loadout here: its activation fills PRIMARY / SECONDARY
# / ACCESSORY and its close on the triggers' clear applies them (engine
# Title::death_screen_loadout, world::deploy_loadout_shown).
func _picks_loadout() -> bool:
	return _armory != null and _view != null and _armory.death_screen_loadout(_view.resource_root())


# The activation's picker: the captions and outlines shown or hidden by the engine's gate, and
# the three lists filled when shown (engine world::deploy_loadout_shown carries the witness).
func _activate_loadout(status: DeployStatus) -> void:
	if not _picks_loadout() or _driver == null:
		return
	for control_name in ["STATIC_PRIMARY", "STATIC_SECONDARY", "STATIC_ACCESSORY",
			"PRIMARY_OUTLINE", "SECONDARY_OUTLINE", "ACCESSORY_OUTLINE"]:
		var id := _driver.widget_id(control_name)
		if id >= 0:
			_driver.set_widget_shown(id, status.show_loadout)
	if status.show_loadout:
		_armory.populate_death_screen(_driver)


## The frame's close leg, run when its triggers' clear finds the open latch up (the shell's
## Simulation.take_death_menu_close): a title whose DEATH screen picks the loadout applies
## the screen's picks there, the screen open or not; any other title's runs nothing (engine
## ClientState::take_death_menu_close carries the witness).
func on_death_menu_close() -> void:
	if _picks_loadout() and _driver != null:
		_armory.submit_death_screen(_driver)


func is_open() -> bool:
	return _frame != null and is_instance_valid(_frame) and _frame.visible


## Whether the screen passes input through: its close then runs the input frame
## that takes the menu cursor's offset (the shell's _leave_screen).
func is_input_pass_through() -> bool:
	return _input_pass_through


## The HUD overlay getter the MAP window draws through (the shell hands the HUD
## presenter's get_game_hud; the overlay builds lazily with the first HUD frame).
func set_hud_source(source: Callable) -> void:
	_hud_source = source


## The MAP window host (ADR 0018 read seam; null before the menu builds or when
## the screen authors no MAP widget).
func get_map_window() -> MapViewWindow:
	return _map_window if _map_window != null and is_instance_valid(_map_window) else null


## Build + wire the presenter under `parent` in one call (the shell's seam):
## `on_opened`/`on_closed` report the screen's cursor ownership.
static func install(parent: Node, view: WorldView, ui_parent: Node,
		on_opened: Callable, on_closed: Callable) -> DeployScreenPresenter:
	var presenter := DeployScreenPresenter.new()
	presenter.name = "DeployScreenPresenter"
	parent.add_child(presenter)
	presenter.setup(view, ui_parent)
	presenter.opened.connect(func() -> void: on_opened.call())
	presenter.closed.connect(func() -> void: on_closed.call())
	return presenter


## The live menu driver over the deploy frame (ADR 0018 read seam for tests and
## diagnostics; null until the first open builds the menu).
func get_menu_driver() -> MenuDriver:
	return _driver


## The presenter-side spawn row model (one SpawnRow per visible list row,
## aligned with the compiled list's rows) — ADR 0018 read seam for tests.
func get_spawn_rows() -> Array[SpawnRow]:
	return _spawn_rows


## ADR 0018 test seams over the row model: append one presenter row (the shape
## the engine builder emits — an occupant row carries param -1) and fire the
## list select the compiled list would raise for a row.
func append_spawn_row(label: String, param: int) -> void:
	_spawn_rows.append(SpawnRow.new(label, param))


func select_spawn_row(row: int) -> void:
	_on_widget_value_changed(SPAWN_LIST, "list", row, "")


## Open over the live world while the screen is held: the join owes a
## deployment pick, the host drives the deploy-map OVERLAY (0x0F game_flags
## bit0 / per-frame 0x0A flags1 bit1), or the local player lies dead in a
## session. Retail's frame loop opens this same death.mnu DEATH screen off
## those on every role (the listen host's own client included), and the
## once-per-arming open latch belongs to the engine's ClientState. The
## witnesses live on ClientState.death_menu_open_latch
## (engine/runtime/replication/client_state.h) and hud-re D-HUD-19.
func open() -> bool:
	if is_open() or _view == null or _ui_parent == null:
		return false
	var sim: Simulation = _view.sim()
	if sim == null or not bool(sim.is_death_menu_held()):
		return false
	if not _ensure_menu():
		return false
	var status := sim.get_deploy_status(Strings.get_table(Strings.TABLE_GAMETEXT),
			ControlsBindings.model().display_text_for_token("MedicReq"))
	_apply_team_service_buttons(status.show_team_buttons)
	_activate_loadout(status)
	# The show's other writes: the permanent-death hides, BRIEFING and a title's STATIC_GAMETYPE
	# (engine world::apply_deploy_activation carries the witness).
	_driver.apply_deploy_activation(sim, Strings.get_table(Strings.TABLE_GAMETEXT))
	# The show hides the shroud (and, for a title that hides it with the shroud, MAIN); the
	# per-frame reveal shows them and runs the content refresh (_process).
	_shroud_main = status.shroud_main
	_set_shroud_shown(false)
	_input_pass_through = true
	_ui_parent.move_child(_frame, _ui_parent.get_child_count() - 1)
	_frame.visible = true
	_show_map_window(sim)
	_last_tick = sim.get_logic_tick()
	set_process(true)
	opened.emit()
	return true


# A selection under the initial overlay closes the screen at once; a death
# re-pick (a joiner's owed pick, or a dead local player) keeps it until the
# host releases the hold, because an invalid or contested pick is dropped
# silently and the list's rows are the screen's.
func _initial_overlay(sim: Simulation) -> bool:
	return not bool(sim.is_join_deploy_pick_pending()) and not bool(sim.is_local_player_dead())


func close() -> void:
	set_process(false)
	var map_window := get_map_window()
	if map_window != null:
		map_window.screen_unload()
	if MenuFrameSurface.hide_frame(_frame):
		closed.emit()


func _unhandled_key_input(event: InputEvent) -> void:
	if handle_key(event as InputEventKey):
		get_viewport().set_input_as_handled()


## One key-down over the open screen, in the original's order (engine
## world::pass_through_close_axis carries the key witness): the menu's
## keyboard hook runs the screen's hotkeys at once (death.mnu's LEAVE MISSION
## and the confirmation's NO on Escape, its YES on Return), whose click lands
## ahead of the pass-through test; the key itself waits in the shell's key ring
## for the input frame the word lets run (KeyboardPass), the Escape key and a
## key under the exit confirmation alike. True only when the hotkey closed the
## screen (the confirmation's YES): the close resets the ring, so the key
## reaches nothing else.
func handle_key(key: InputEventKey) -> bool:
	if not is_open() or key == null or not key.pressed or key.echo:
		return false
	if _driver == null or not _driver.handle_key_input(key):
		return false
	return not is_open()


## The session the screen's world view names (the shell's world's), which the
## shell's special-key chain and the pick's send both serve; null with no view.
func view_sim() -> Simulation:
	return _view.sim() if _view != null else null


## The queued pick's dispatch (case 12), the screen open or not: like a list
## select, the initial overlay closes on the send and a death re-pick keeps the
## screen until the host releases the hold.
func send_pick(param: int) -> void:
	var sim: Simulation = _view.sim() if _view != null else null
	if sim == null:
		return
	var initial_overlay := _initial_overlay(sim)
	if sim.send_deployment_pick(param) and initial_overlay:
		close()


func teardown() -> void:
	set_process(false)
	if _frame != null and is_instance_valid(_frame):
		_frame.queue_free()
	if _audio != null and is_instance_valid(_audio):
		_audio.queue_free()
	_frame = null
	_audio = null
	_driver = null
	_map_window = null
	_spawn_rows = []


func _process(_delta: float) -> void:
	if not is_open():
		set_process(false)
		return
	# The menu's pump, once a frame after the input events (MenuFrameSurface).
	_driver.pump_mouse()
	# The blink/marquee clock rides the OS tick like the original's GetTickCount
	# gate (the shell does the same for the front-end menus).
	_driver.tick(_view.frame_clock_ms)
	var sim: Simulation = _view.sim() if _view != null else null
	if sim == null:
		close()
		return
	# A DEAD session is not a deployment release. The host's punt closes the connection,
	# whose terminal phase clears the pick-pending flag below — so closing there would hand
	# the shell back to State.WORLD and re-capture the mouse over a world the player can no
	# longer play. Tear the screen down instead and leave the exit to the session-loss leg:
	# retail's disconnect exits the mission outright on this edge, it never resumes play.
	# [orig: CNapiNetwork_OnDisconnectedFromServer @0x4c63d0 -> Input_QueueEvent(3)
	#  @0x4c67a4 -> g_MissionExitReason = 1 (Input_HandleActionBinding case 3 @0x49af2c),
	#  the nav-push "MainMenu" teardown every abort leg takes @0x568654]
	if bool(sim.is_session_lost()):
		teardown()
		return
	# The screen's lifetime: a pending DEATH pick holds it, and so do the
	# frame loop's two triggers, the host-driven overlay bit (which follows
	# the per-frame 0x0A flags1 bit1, set AND cleared) and the in-session
	# local death. Only all falling closes it: retail's frame loop closes the
	# latched screen exactly when its open triggers are gone (the per-frame
	# fold and close-on-clear witnesses live on ClientState.death_menu_open_latch
	# and hud-re D-HUD-19). That close is the scene frame's: a frame a listen
	# host's status page stood in for closes nothing (its ToggleServer row passes
	# this screen's input frame; the same witness), and the screen's own frame
	# runs on until the next drawn frame closes it.
	if not bool(sim.is_death_menu_held()) and (_view == null or _view.scene_drawn()):
		close()
		return
	# The shroud reveal and, inside it, the content refresh on each 16-tick
	# boundary (engine: runtime/world/deploy_screen_feed.h).
	var tick := sim.get_logic_tick()
	if bool(sim.is_death_shroud_revealed()):
		_set_shroud_shown(true)
		if Simulation.deploy_refresh_due(_last_tick, tick):
			_populate_spawn_list(sim)
			# A title's team lists ride the same refresh (the engine's gate and witness:
			# world::deploy_team_lists).
			_driver.fill_deploy_team_lists(sim, Strings.get_table(Strings.TABLE_GAMETEXT))
	_last_tick = tick
	# The map window follows its widget, which the reveal just showed.
	_place_map_window()


# Selection commands arrive through the driver's aggregate relay. Every click on
# a row queues a pick, including a click on the row that is already highlighted:
# the original's select callback is a COMMAND, fired unconditionally from the
# list event with no "did the selection change" test and no re-entry gate — and
# the driver re-emits widget_value_changed on every list click, which carries the
# old ItemList allow_reselect=true semantics (a re-opened DEATH screen, and any
# re-pick after the host silently drops one, can still send another C2S 0x0E).
# [orig: DeathScreen_OnSpawnListSelect @ 0x553630 -> Input_QueueEvent(12,
#  node) @ 0x55364d, guarded only on node != -1]
func _on_widget_value_changed(widget_name: String, kind: String, index: int,
		_value: String) -> void:
	if kind == "combo" and LOADOUT_LISTS.has(widget_name.to_upper()) and _picks_loadout():
		_armory.pick_death_screen(_driver, widget_name.to_upper())
		return
	if kind != "list" or widget_name.nocasecmp_to(SPAWN_LIST) != 0:
		return
	if index < 0 or index >= _spawn_rows.size():
		return
	var sim: Simulation = _view.sim() if _view != null else null
	if sim == null:
		return
	# [orig: the SPAWNPOINTS_LIST select callback -> Input_QueueEvent(12, node)
	#  @0x55364d; node 0 = the Default Spawn -> the parameter-0 pick; the
	#  occupant/blank rows carry node -1 and the callback guards node != -1]
	var param := _spawn_rows[index].param
	if param == -1:
		return
	var initial_overlay := _initial_overlay(sim)
	if sim.send_deployment_pick(param) and initial_overlay:
		# Input case 12 resets the dialogs and queues the request even for an
		# alive player. Runtime holds gameplay until the host releases the pick.
		close()


# The spawn table's cell event, for a title whose DEATH screen picks the loadout: a press on a
# zone's row pans the map to it, a double click sends that row's pick (engine
# world::deploy_table_click carries the witness).
func _on_table_cell_clicked(id: int, widget_name: String, row: int, _column: int, state: int,
		_cell_value: int, double_click: bool) -> void:
	if widget_name.nocasecmp_to(SPAWN_TABLE) != 0 or _driver == null or not _picks_loadout():
		return
	var click := _driver.deploy_table_click(id, row, state, double_click)
	var map_window := get_map_window()
	if click.x >= 0 and map_window != null:
		map_window.pan_to_spawn_zone(click.x)
	if click.y != -1:
		send_pick(click.y)


func _populate_spawn_list(sim: Simulation) -> void:
	if _driver == null:
		return
	var status := sim.get_deploy_status(Strings.get_table(Strings.TABLE_GAMETEXT),
			ControlsBindings.model().display_text_for_token("MedicReq"))
	_apply_statics(status)
	if status.permanent_death or not status.content_refresh:
		return
	# A title whose DEATH screen picks the loadout lists its spawn points in SPAWNPOINTS_TABLE:
	# the engine collects the zones, builds the rows and refills the table, its scroll top and
	# selected row kept (world::fill_deploy_spawn_table carries the witness); the health colours
	# take the HUD's stance colours.
	if _picks_loadout():
		var hud: HudOverlay = _hud_source.call() if _hud_source.is_valid() else null
		_driver.fill_deploy_spawn_table(sim, hud, Strings.get_table(Strings.TABLE_GAMETEXT))
		return
	var list_id := _driver.widget_id(SPAWN_LIST)
	if list_id < 0:
		return
	# Preserve the pick across the periodic refill by PARAMETER, not row index —
	# zone security/ownership changes can reshuffle the rows.
	var keep_param := -1
	var selected := _driver.selected_row(list_id)
	if selected >= 0 and selected < _spawn_rows.size():
		keep_param = _spawn_rows[selected].param
	# The compiled row set comes from the engine builder (world/deploy_screen_feed):
	# the Default row, the secured team zones, the whole-list text sort, then
	# each zone's wave occupants + blank separator at node -1. The row texts
	# carry retail's team colour tag and the '<b><cFF4040>** name **' self
	# marker; the compiled list renders the tags through its own markup.
	# [orig: UI_UpdateDeathScreenContent @0x553aef..0x553de3]
	_spawn_rows = []
	var zone_names := {}
	for zone: DeployZoneRow in sim.get_deploy_spawn_zones():
		var key := zone.name_key
		zone_names[key] = _game_text(Strings.SECTION_WPNAMES, key, "Spawn Point")
	var list_rows: Array[DeployListRow] = sim.get_deploy_list_rows(
			Strings.menu_text("DEFAULT_SPAWN_KEY", "D"), Strings.menu_text("HOME", "Home Base"),
			zone_names)
	for row: DeployListRow in list_rows:
		# The compiled list has no inline markup channel yet (the row-style
		# residue in D-HUD-19): the engine text keeps retail's <cRRGGBB>/<b>
		# tags, the list shows them stripped. The sort already ran over the
		# tagged text, so the row order is retail's. The stripper is retail's own
		# (the engine's hud::strip_inline_tags (engine: runtime/hud/feed_format.cpp)).
		var label := Simulation.strip_inline_tags(row.text)
		_spawn_rows.append(SpawnRow.new(label, row.value))
	# The list takes the engine's items (MenuDriver.set_deploy_list_items: the same
	# stripped texts in the menu's code page, a member's roster name as its bytes);
	# the spawn rows' labels are the script's own view of them. The fill resets the
	# selection to row 0; restore the previous pick by parameter without emitting
	# (picks ride user clicks only, never the refill).
	_driver.set_deploy_list_items(list_id, list_rows)
	if keep_param >= 0:
		for row in _spawn_rows.size():
			if _spawn_rows[row].param == keep_param:
				_driver.select_row(list_id, row, false)
				break




func _set_shroud_shown(shown: bool) -> void:
	if _shroud_main and _driver != null:
		var main_id := _driver.widget_id(MAIN_WIDGET)
		if main_id >= 0:
			_driver.set_widget_shown(main_id, shown)
	var id := _driver.widget_id(SHROUD_WIDGET) if _driver != null else -1
	if id < 0:
		return
	var was_shown := _driver.is_widget_shown(id)
	_driver.set_widget_shown(id, shown)
	# The loadout icons follow the windows they sit in.
	if was_shown != shown and _picks_loadout():
		_armory.refresh_death_screen(_driver)


# The swap/team pair's show rule is the engine's (world/deploy_screen_feed.h
# deploy_team_buttons_shown). [orig: DeathScreen_UpdateUI @0x553489 / @0x5534AA]
func _apply_team_service_buttons(shown: bool) -> void:
	for control_name in [SWAP_TEAMS, TEAM_LIST]:
		# A -1 id means this screen simply does not author the control.
		var id := _driver.widget_id(control_name)
		if id >= 0:
			_driver.set_widget_shown(id, shown)


# SWAP_TEAMS asks the host for the other side (C2S 0x4D; the engine's
# ClientRuntime::queue_team_change_request carries the witness). HIDDEN_BACK
# and CONFIRM_NO lower and raise the pass-through word (_input_pass_through).
# CONFIRM_YES closes the in-game screens, then asks the shell for the exit
# (exit_confirmed).
func _on_widget_activated(_id: int, widget_name: String) -> void:
	if widget_name.nocasecmp_to(HIDDEN_BACK) == 0:
		_input_pass_through = false
		return
	if widget_name.nocasecmp_to(CONFIRM_NO) == 0:
		_input_pass_through = true
		return
	if widget_name.nocasecmp_to(CONFIRM_YES) == 0:
		close()
		exit_confirmed.emit()
		return
	if widget_name.nocasecmp_to(RESTART_LEVEL) == 0:
		_restart_level()
		return
	if widget_name.nocasecmp_to(SWAP_TEAMS) != 0:
		return
	var sim: Simulation = _view.sim() if _view != null else null
	if sim != null:
		sim.send_team_change_request()


# RESTART_LEVEL, for a title whose DEATH screen picks the loadout: past the engine's gate the
# screen closes, the pick 0 goes and the shell restarts the mission.
func _restart_level() -> void:
	var sim: Simulation = _view.sim() if _view != null else null
	if sim == null or not _picks_loadout():
		return
	var status := sim.get_deploy_status(Strings.get_table(Strings.TABLE_GAMETEXT),
			ControlsBindings.model().display_text_for_token("MedicReq"))
	if not status.restart_level:
		return
	close()
	sim.send_deployment_pick(0)
	restart_requested.emit()


# The witnessed STATIC show/text rules, refreshed with the list
# [orig: UI_UpdateDeathScreenContent @0x5536a0]:
#  * STATIC_RESPAWN_MSG1 (@0x5538e7..0x553a7b): hidden; the penalty timer
#    "<STROVER_PENALTYTIMER>  <cFF4040><n>" wins; else the wave zone listing the
#    local player — numbered "'<WPNames name>':  <cFF4040><n>", lettered
#    "<letter>:  <cFF4040><n>";
#  * STATIC_LIST_TITLE shown with the list (@0x553ab4);
#  * STATIC_PSPRESPAWN_MSG1 (@0x553e10) while the spawn-target hold runs;
#  * STATIC_MEDIC_MSG1 + STATIC_CALLMEDIC_MSG (@0x553e74..0x553f60) while the
#    local revive window runs and the player is not in a seat.
# The three texts are the engine's (world/deploy_screen_feed.h
# deploy_statics_text), resolved through gametext by the sim with the MedicReq
# binding's display string.
func _apply_statics(status: DeployStatus) -> void:
	_apply_team_service_buttons(status.show_team_buttons)
	# Past the permanent-death branch the refresh writes nothing unless the deploy overlay is
	# up or the local player is dead (engine DeployScreenStatus::content_refresh).
	if not status.permanent_death and not status.content_refresh:
		return
	var instruction_id := _driver.widget_id("STATIC_INSTRUCTIONS_MSG")
	var instruction2_id := _driver.widget_id("STATIC_INSTRUCTIONS2_MSG")
	if instruction_id >= 0 and instruction2_id >= 0:
		_driver.set_widget_shown(instruction_id, status.show_instruction)
		_driver.set_widget_shown(instruction2_id, status.show_instruction2)
		if status.replace_instruction:
			_driver.set_widget_text(instruction_id, status.instruction_text)
		_driver.set_widget_text(instruction2_id, status.instruction2_text)
	if status.permanent_death:
		var round_id := _driver.widget_id("STATIC_RESPAWN_MSG1")
		var remaining_id := _driver.widget_id("STATIC_PSPRESPAWN_MSG1")
		if round_id >= 0 and remaining_id >= 0:
			_driver.set_widget_shown(round_id, status.show_round_status)
			_driver.set_widget_shown(remaining_id, status.show_round_status)
			if status.show_round_status:
				_driver.set_widget_text(round_id, status.round_text)
				_driver.set_widget_text(remaining_id, status.remaining_players_text)
		return
	var title_id := _driver.widget_id("STATIC_LIST_TITLE")
	if title_id >= 0:
		_driver.set_widget_shown(title_id, true)
	var respawn_id := _driver.widget_id("STATIC_RESPAWN_MSG1")
	if respawn_id >= 0:
		var kind := status.queued_kind
		_driver.set_widget_shown(respawn_id, kind != 0)
		if kind != 0:
			# The three sprintf arms are the engine's deploy_status_text
			# (world/deploy_screen_feed.h), resolved through gametext by the sim.
			_driver.set_widget_text(respawn_id,
					status.respawn_text)
	var psp_id := _driver.widget_id("STATIC_PSPRESPAWN_MSG1")
	if psp_id >= 0:
		var show_psp := status.show_psp_respawn
		_driver.set_widget_shown(psp_id, show_psp)
		if show_psp:
			_driver.set_widget_text(psp_id, status.psp_respawn_text)
	var medic_id := _driver.widget_id("STATIC_MEDIC_MSG1")
	var call_id := _driver.widget_id("STATIC_CALLMEDIC_MSG")
	if medic_id >= 0 and call_id >= 0:
		var show_medic := status.show_medic
		_driver.set_widget_shown(medic_id, show_medic)
		_driver.set_widget_shown(call_id, show_medic)
		if show_medic:
			_driver.set_widget_text(medic_id, status.medic_timer_text)
			_driver.set_widget_text(call_id, status.call_medic_text)


# Build the compiled menu surface the same way the armory presenter does: the
# shared MenuFrame + MenuAudio + MenuDriver stack fed death.mnu from the world's
# mounted resource root.
func _ensure_menu() -> bool:
	if _driver != null and _frame != null and is_instance_valid(_frame):
		return true
	var root: ResourceRoot = _view.resource_root()
	# death.mnu authors <MUSICVAR>3</MUSICVAR>; the surface wires the slot.
	var surface := MenuFrameSurface.open_surface(root, _ui_parent, _layout_control,
			MENU_FILE, MENU_SCREEN, "DeployScreenMenu", "DeployScreenPresenter",
			_on_frame_gui_input,
			func(driver: MenuDriver) -> void:
				_register_text_tables(root)
				driver.widget_value_changed.connect(_on_widget_value_changed)
				driver.widget_activated.connect(_on_widget_activated)
				driver.table_cell_clicked.connect(_on_table_cell_clicked))
	if surface == null:
		return false
	_frame = surface.frame
	_audio = surface.audio
	_driver = surface.driver
	# A title whose DEATH screen has the spawn table draws its custom cells (engine
	# world::deploy_table_paint carries the witness).
	if _picks_loadout():
		_driver.install_deploy_table_painter()
	_mount_map_window()
	return true


# Mount the MAP window host as a frame child over the authored MAP widget (the
# frame's cursor overlay stays above it).
func _mount_map_window() -> void:
	var id := _driver.widget_id(MAP_WIDGET)
	if id < 0:
		return
	_map_window = MapViewWindow.new()
	_map_window.name = "DeployMapWindow"
	_map_window.set_view_state(_map_view_state)
	# The map's move leg deselects the spawn table's rows (engine world::deploy_map_moved).
	_map_window.death_map_moved.connect(func() -> void:
		if _driver != null:
			_driver.deploy_map_moved())
	_frame.add_child(_map_window)
	_place_map_window()


# Follow the widget: its frame rect, its authored design rect, and its
# effective visibility (the shroud hides with the CONFIRM_EXIT dialog).
func _place_map_window() -> void:
	var map_window := get_map_window()
	if map_window == null or _driver == null:
		return
	var id := _driver.widget_id(MAP_WIDGET)
	var index := _driver.frame_index(id)
	if index < 0:
		map_window.visible = false
		return
	var rect := _driver.widget_frame_rect(id)
	map_window.position = rect.position
	map_window.size = rect.size
	var design := _frame.widget_rect(index)
	map_window.set_widget_design_rect(Rect2i(design))
	map_window.visible = _frame.is_widget_shown(index) and rect.size.x > 0.0 \
			and rect.size.y > 0.0
	# The widgets after MAP (CONFIRM_EXIT) paint above the mounted window.
	_frame.set_mount_widget(index if map_window.visible else -1)


# The screen's load + show events for the MAP window: the sources, then the
# seed (first load) and the zoom fit against the shroud's authored size.
func _show_map_window(sim: Simulation) -> void:
	var map_window := get_map_window()
	if map_window == null:
		return
	var hud: HudOverlay = _hud_source.call() if _hud_source.is_valid() else null
	map_window.set_hud_overlay(hud)
	map_window.set_simulation(sim)
	_place_map_window()
	var shroud_id := _driver.widget_id(SHROUD_WIDGET)
	var shroud_index := _driver.frame_index(shroud_id) if shroud_id >= 0 else -1
	var shroud_size := Vector2i.ZERO
	if shroud_index >= 0:
		shroud_size = Vector2i(_frame.widget_rect(shroud_index).size)
	map_window.screen_load()
	map_window.screen_show(shroud_index >= 0, shroud_size)


# The compiled frame is a passive surface; MenuFrameSurface.forward_gui_input
# forwards its gui input to the driver the way MenuShell does.
func _on_frame_gui_input(event: InputEvent) -> void:
	if _driver == null or not is_open():
		return
	MenuFrameSurface.forward_gui_input(event, _driver, _frame)


func _register_text_tables(root: ResourceRoot) -> void:
	for spec in [[Strings.TABLE_MENUTXT, "menutxt.BIN"], [Strings.TABLE_GAMETEXT, "gametext.bin"],
			[Strings.TABLE_GAMEUI, "Game.bin"]]:
		if Strings.get_table(spec[0]) != null:
			continue
		var loaded := Strings.load_rtxt(root, spec[1])
		if loaded != null:
			Strings.register_table(spec[0], loaded)


func _game_text(section: String, key: String, fallback: String) -> String:
	# [orig: GameText_GetString(Strings.SECTION_WPNAMES, "STRWPNAME%03d") @0x5536a0]
	if key.is_empty():
		return fallback
	return Strings.lookup_or(Strings.TABLE_GAMETEXT, section, key, fallback)


func _recompute_fit() -> void:
	# MenuFrameSurface.fit_frame (shared with the other presenters).
	MenuFrameSurface.fit_frame(_frame, _layout_control, _ui_parent)
	_place_map_window()
	if _driver != null and _picks_loadout():
		_armory.refresh_death_screen(_driver)
