class_name DeployScreenPresenter
extends Node

## The deploy-map screen (death.mnu's DEATH) for the authority's initial
## deployment hold and later death picks: the device half. The screen's logic
## (the show, the shroud reveal and the 16-tick refresh, the spawn rows and
## their params, the pick rule, the pass-through word, SWAP_TEAMS, the
## statics) is the engine's DEATH controller (runtime/menu/death_screen.h,
## the DeathScreen binding), claimed on this presenter's driver at each open.
## This node owns the frame (its mount, fit and visibility), the per-frame
## pump and clock, the input forwarding and the MAP window: the windowed map
## view (MapViewWindow over the engine's DeathMapView and the HUD compiler's
## DEATH pass, hud/hud_map_view.h), whose view state outlives the menu
## rebuild like retail's globals (hud-re D-HUD-19).

const MENU_FILE := "death.mnu"
const MENU_SCREEN := "DEATH"
# The custom-draw MAP window and the shroud window whose authored size the
# show-time zoom fit reads (hud/hud_map_view.h DeathMapView::fit).
const MAP_WIDGET := "MAP"
const SHROUD_WIDGET := "DEATH_SHROUD"

signal opened
signal closed
## The exit confirmation's YES, after the screen closed: the shell runs input
## action 3 (the Exit Mission tone, then the exit; engine
## world::pass_through_close_axis carries the key witness).
signal exit_confirmed

var _view: WorldView = null
var _ui_parent: Node = null
# The layout source, converted ONCE at setup: a Control parent (test overlays)
# drives the fit from its own size/resized; a CanvasLayer parent (the game HUD)
# has no size, so the fit follows the viewport instead.
var _layout_control: Control = null
# The compiled menu surface (freed with the presenter; only the two nodes need
# queue_free) and the screen's engine controller.
var _frame: MenuFrame = null
var _audio: MenuAudio = null
var _driver: MenuDriver = null
var _screen := DeathScreen.new()
# The MAP window host mounted over the MAP widget (null until the menu builds
# on a screen that authors MAP), and the shell's HUD overlay getter: the map
# pass compiles over the HUD's terrain / markers / fonts.
var _map_window: MapViewWindow = null
var _hud_source: Callable = Callable()
# The DEATH map's pan/zoom state, held for the presenter's (the process's)
# lifetime and handed to every MapViewWindow the menu rebuild mounts.
var _map_view_state := MapViewState.new()


func _init() -> void:
	_screen.close_requested.connect(close)
	_screen.exit_confirmed.connect(exit_confirmed.emit)


func setup(view: WorldView, ui_parent: Node) -> void:
	_view = view
	_ui_parent = ui_parent
	_layout_control = ui_parent as Control
	MenuFrameSurface.connect_layout_source(_layout_control, _ui_parent, _recompute_fit)


func is_open() -> bool:
	return _frame != null and is_instance_valid(_frame) and _frame.visible


## Whether the screen passes input through: its close then runs the input frame
## that takes the menu cursor's offset (the shell's _leave_screen).
func is_input_pass_through() -> bool:
	return _screen.is_input_pass_through()


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


## The visible spawn rows, each {label, param} (the engine controller's row
## model, aligned with the compiled list's rows) — ADR 0018 read seam.
func get_spawn_rows() -> Array:
	return _screen.get_spawn_rows()


## Open over the live world while the screen is held: the join owes a
## deployment pick, the host drives the deploy-map overlay, or the local player
## lies dead in a session (the engine's DeathScreen::can_open over the
## session's death-menu hold).
func open() -> bool:
	if is_open() or _view == null or _ui_parent == null:
		return false
	var sim: Simulation = _view.sim()
	if sim == null:
		return false
	_screen.set_simulation(sim)
	if not _screen.can_open():
		return false
	if not _ensure_menu():
		return false
	_screen.set_text_tables(Strings.get_table(Strings.TABLE_GAMETEXT),
			Strings.get_table(Strings.TABLE_MENUTXT), Strings.get_table(Strings.TABLE_GAMEUI))
	_screen.set_medic_key_label(ControlsBindings.model().display_text_for_token("MedicReq"))
	# The show (the engine controller's open): the team pair, the shroud hidden,
	# the pass-through word raised.
	_driver.claim_screen(_screen)
	_ui_parent.move_child(_frame, _ui_parent.get_child_count() - 1)
	_frame.visible = true
	_show_map_window(sim)
	set_process(true)
	opened.emit()
	return true


func close() -> void:
	set_process(false)
	var map_window := get_map_window()
	if map_window != null:
		map_window.screen_unload()
	if _driver != null:
		_driver.release_screen()
	if MenuFrameSurface.hide_frame(_frame):
		closed.emit()


func _unhandled_key_input(event: InputEvent) -> void:
	if handle_key(event as InputEventKey):
		get_viewport().set_input_as_handled()


## One key-down over the open screen, in the original's order (engine
## world::pass_through_close_axis carries the key witness): the menu's
## keyboard hook runs the screen's hotkeys at once, whose click lands ahead of
## the pass-through test; the key itself waits in the shell's key ring for the
## input frame the word lets run (KeyboardPass). True only when the hotkey
## closed the screen (the confirmation's YES): the close resets the ring.
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
	var sim: Simulation = view_sim()
	if sim == null:
		return
	_screen.set_simulation(sim)
	if _screen.send_pick(param):
		close()


func teardown() -> void:
	set_process(false)
	if _driver != null:
		_driver.release_screen()
	if _frame != null and is_instance_valid(_frame):
		_frame.queue_free()
	if _audio != null and is_instance_valid(_audio):
		_audio.queue_free()
	_frame = null
	_audio = null
	_driver = null
	_map_window = null
	_screen.reset()


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
	_screen.set_simulation(sim)
	# The engine's frame (DeathScreen::frame): a dead session tears the screen
	# down, its triggers falling on a drawn scene frame close it, else the
	# shroud's reveal and the content refresh.
	match _screen.frame(_driver, _view.scene_drawn()):
		DeathScreen.FRAME_TEARDOWN:
			teardown()
			return
		DeathScreen.FRAME_CLOSE:
			close()
			return
	# The map window follows its widget, which the reveal just showed.
	_place_map_window()


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
			func(_driver_built: MenuDriver) -> void: _register_text_tables(root))
	if surface == null:
		return false
	_frame = surface.frame
	_audio = surface.audio
	_driver = surface.driver
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


func _recompute_fit() -> void:
	# MenuFrameSurface.fit_frame (shared with the other presenters).
	MenuFrameSurface.fit_frame(_frame, _layout_control, _ui_parent)
	_place_map_window()
