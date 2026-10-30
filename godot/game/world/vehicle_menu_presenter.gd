class_name VehicleMenuPresenter
extends Node

## The in-world vehicle screen's device half: vehicle.mnu's VEHICLE screen over
## live play on the compiled-menu surface the armory's weapon.mnu rides
## (MenuFrameSurface). The screen is the engine's VehicleMenuController
## (runtime/inmatch/vehicle_menu_controller.h, over inmatch::VehicleMenu): its
## open, the ITEM_LIST rows and pick, the ACCEPT / CANCEL click. This node mounts
## the frame and the ITEM_DISPLAY preview, pumps them, and closes the frame when
## the controller asks. The shell opens it
## (LocalPlayerPresenter.take_use_item_vehicle_menu_request); the world keeps
## ticking underneath.

signal opened
signal closed

const MENU_FILE := "vehicle.mnu"
const MENU_SCREEN := "VEHICLE"
const DISPLAY_CONTROL := "ITEM_DISPLAY"

var _view: WorldView = null
var _ui_parent: Node = null
# The layout source (MenuFrameSurface.fit_frame): a Control parent drives the fit
# from its own size, a CanvasLayer parent (the game HUD) follows the viewport.
var _layout_control: Control = null
# The compiled menu surface (MenuFrameSurface.Surface's members).
var _frame: MenuFrame = null
var _audio: MenuAudio = null
var _driver: MenuDriver = null
# The ITEM_DISPLAY pane over the screen's widget (null when the screen authors none).
var _preview: VehiclePreview = null
# The screen's controller (claimed on the driver at each open).
var _controller: VehicleMenuController = null


## The world view (its Simulation and resource root) and the node the screen
## overlays (the HUD layer in the game shell; tests pass their own parent).
func setup(view: WorldView, ui_parent: Node) -> void:
	_view = view
	_ui_parent = ui_parent
	_layout_control = ui_parent as Control
	MenuFrameSurface.connect_layout_source(_layout_control, _ui_parent, _recompute_fit)


func is_open() -> bool:
	return _frame != null and is_instance_valid(_frame) and _frame.visible


## The live menu driver and frame over the screen (read seams for tests and
## diagnostics; null until the first open builds the menu).
func get_menu_driver() -> MenuDriver:
	return _driver


func get_menu_frame() -> MenuFrame:
	return _frame


func get_preview() -> VehiclePreview:
	return _preview


## Open the screen over live play: the controller runs the menu's open events
## (the first parse's init, every open's activate) and lists what they hold.
func open() -> bool:
	if is_open() or _view == null or _ui_parent == null:
		return false
	var sim: Simulation = _view.sim()
	if sim == null or not _ensure_menu():
		return false
	_controller.setup(_driver, sim)
	_driver.claim_screen(_controller)
	_refresh_preview()
	# The menu draws over every HUD element, as the armory's does.
	_ui_parent.move_child(_frame, _ui_parent.get_child_count() - 1)
	_frame.visible = true
	set_process(true)
	opened.emit()
	return true


## The Escape close: CANCEL's click (the engine sends nothing for it).
func close() -> void:
	if is_open():
		_controller.click(true)


## The screen's controller (ADR 0018 read seam; null until the first open builds the menu).
func get_controller() -> VehicleMenuController:
	return _controller


func teardown() -> void:
	set_process(false)
	if _frame != null and is_instance_valid(_frame):
		_frame.queue_free()
	if _audio != null and is_instance_valid(_audio):
		_audio.queue_free()
	_frame = null
	_audio = null
	_driver = null
	_preview = null
	_controller = null


func _process(_delta: float) -> void:
	if not is_open():
		set_process(false)
		return
	# The menu's pump, once a frame after the input events (MenuFrameSurface).
	_driver.pump_mouse()
	_driver.tick(_view.frame_clock_ms)
	# The engine's poll refills the list every 64 frames while a reply stands.
	_controller.refresh()
	_refresh_preview()


# Build the compiled surface over the gameplay view from the world's root, the
# screen's text tables registered as the armory's are.
func _ensure_menu() -> bool:
	if _driver != null and _frame != null and is_instance_valid(_frame):
		return true
	var root: ResourceRoot = _view.resource_root()
	var surface := MenuFrameSurface.open_surface(root, _ui_parent, _layout_control,
			MENU_FILE, MENU_SCREEN, "VehicleMenu", "VehicleMenuPresenter", _on_frame_gui_input,
			func(_built: MenuDriver) -> void: _register_text_tables(root))
	if surface == null:
		return false
	_frame = surface.frame
	_audio = surface.audio
	_driver = surface.driver
	_frame.visible = false
	_controller = VehicleMenuController.new()
	_controller.close_requested.connect(_on_close_requested)
	_mount_preview(root)
	return true


# The ITEM_DISPLAY pane, a frame child over the widget's rect, as the game registers the
# widget's custom draw (engine inmatch::vehicle_menu_preview_graphic).
func _mount_preview(root: ResourceRoot) -> void:
	var display := _driver.widget_id(DISPLAY_CONTROL)
	if display < 0:
		return
	_preview = VehiclePreview.new()
	_preview.name = "VehicleItemDisplay"
	_preview.set_resource_root(root)
	_frame.add_child(_preview)
	_place_preview()


func _place_preview() -> void:
	if _preview == null or not is_instance_valid(_preview):
		return
	var rect := _driver.widget_frame_rect(_driver.widget_id(DISPLAY_CONTROL))
	_preview.position = rect.position
	_preview.size = rect.size
	_preview.visible = rect.size.x > 0.0 and rect.size.y > 0.0


# The selected group's model and its turn on the process clock (GetTickCount).
func _refresh_preview() -> void:
	if _preview != null and is_instance_valid(_preview):
		_preview.show_graphic(_controller.preview_graphic(), GameWorld.tick_count_ms())


# The controller's click (ACCEPT, CANCEL or the Escape close): the frame closes.
func _on_close_requested() -> void:
	if not is_open():
		return
	_driver.release_screen()
	MenuFrameSurface.hide_frame(_frame)
	set_process(false)
	closed.emit()


func _on_frame_gui_input(event: InputEvent) -> void:
	if _driver != null and is_open():
		MenuFrameSurface.forward_gui_input(event, _driver, _frame)


# The screen's text tables from the world's root (the armory's rule): a table the
# root lacks is unregistered.
func _register_text_tables(root: ResourceRoot) -> void:
	for spec in [[Strings.TABLE_MENUTXT, "menutxt.BIN"], [Strings.TABLE_GAMETEXT, "gametext.bin"],
			[Strings.TABLE_GAMEUI, "Game.bin"]]:
		Strings.register_table(spec[0], Strings.load_rtxt(root, spec[1]))


func _recompute_fit() -> void:
	MenuFrameSurface.fit_frame(_frame, _layout_control, _ui_parent)
	_place_preview()
