class_name ArmoryPresenter
extends Node

## The in-world armory surface mounted by the game shell: the device half of
## weapon.mnu's WEAPON screen over live play (the world keeps ticking under it).
## The screen's behavior, its on-show, picks, class pages and the ACCEPT / CANCEL
## past the press latch, is the engine controller ArmoryScreen (engine
## menu::ArmoryScreen, runtime/menu/armory_screen.h carries the witnesses), which
## this presenter claims the opened document with; this mounts the frame, pumps it
## each frame, forwards its input, and refreshes the FP view when a local ACCEPT
## presents a weapon.

const MENU_FILE := "weapon.mnu"
const MENU_SCREEN := "WEAPON"

# The ACCEPT hotkeys are the USE-ITEM binding row's runtime keys, the open key's
# own row (engine ArmoryScreen's on-show carries the witness): a key of that row
# is the ACCEPT accelerator while the screen is up.
static func is_accept_hotkey(keycode: Key) -> bool:
	return ControlsBindings.model().godot_keys_for_token("useitem").has(int(keycode))


# The key a binding row matches an event by: the layout's VK (Godot's keycode), as
# the polled rows read it (Input.is_key_pressed, ControlsModel::vk_down_); a
# synthetic event that carries none falls back to its physical_keycode.
static func bound_key_of(key: InputEventKey) -> Key:
	return key.keycode if key.keycode != KEY_NONE else key.physical_keycode

signal opened
signal closed

var _view: ArmoryWorldView = null
var _player_presenter: LocalPlayerPresenter = null  # viewmodel rebuild on ACCEPT
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
var _menu_root: ResourceRoot = null  # the root the built menu was fed from
var _armory := ArmoryMenuCompanion.new()


func _init() -> void:
	_armory.armory_screen.close_requested.connect(close)
	_armory.armory_screen.viewmodel_changed.connect(_on_viewmodel_changed)


## Wire the presenter to its world view + player presenter and the node the menu
## overlays (the HUD layer in the game shell; tests pass their own Control parent).
func setup(view: ArmoryWorldView, player_presenter_in: LocalPlayerPresenter,
		ui_parent: Node) -> void:
	_view = view
	_player_presenter = player_presenter_in
	_ui_parent = ui_parent
	_layout_control = ui_parent as Control
	_armory.armory_screen.setup(view)
	MenuFrameSurface.connect_layout_source(_layout_control, _ui_parent, _recompute_fit)


func set_player_profiles(profiles: PlayerProfiles) -> void:
	_armory.armory_screen.set_player_profiles(profiles)


## The mission start's stage of the screen's class pages (engine
## ArmoryScreen::stage_mission_start).
func on_mission_started() -> void:
	_armory.armory_screen.stage_mission_start()


func is_open() -> bool:
	return _frame != null and is_instance_valid(_frame) and _frame.visible


## The live menu driver over the armory frame (ADR 0018 read seam for tests and
## diagnostics; null until the first open builds the menu).
func get_menu_driver() -> MenuDriver:
	return _driver


# --- MCP menu-driving seam (the game_menu tool while the armory is up) -----------
# MenuShell's seam shape over this surface (MenuFrameSurface's ops); a closed armory
# answers nothing.


func menu_snapshot(include_widgets: bool = true) -> Dictionary:
	if not is_open():
		return {}
	return MenuFrameSurface.menu_state(_driver, _frame, {
		"visible": true,
		"surface": "armory",
		"file": MENU_FILE,
		"screen": _driver.get_current_screen(),
		"screens": _driver.get_screen_names(),
		"in_game": true,
		"stack_depth": _driver.get_screen_history_depth(),
	}, include_widgets)


func menu_press(widget_name: String) -> bool:
	return is_open() and MenuFrameSurface.press_widget(_driver, _frame, widget_name)


func menu_move_at(design_pos: Vector2, down: bool) -> int:
	return MenuFrameSurface.pump_at(_driver, _frame, design_pos, down) if is_open() else -1


func menu_press_at(design_pos: Vector2) -> int:
	return MenuFrameSurface.press_at(_driver, _frame, design_pos) if is_open() else -1


func menu_click_at(design_pos: Vector2) -> bool:
	if not is_open():
		return false
	MenuFrameSurface.click_at(_frame, design_pos)
	return true


# One key press as the screen takes it: the useitem key is the ACCEPT accelerator,
# the press latch permitting (_unhandled_key_input); the screen takes no other key.
func menu_key(keycode: int, unicode: int = 0) -> bool:
	if not is_open():
		return false
	var ev := InputEventKey.new()
	ev.keycode = keycode as Key
	ev.physical_keycode = keycode as Key
	ev.unicode = unicode
	ev.pressed = true
	if not is_accept_hotkey(bound_key_of(ev)) or not can_dismiss():
		return false
	_armory.armory_screen.trigger_accept()
	return true


## Whether the WEAPON screen's ACCEPT and CANCEL act (the engine's press latch;
## ArmoryScreen's ACCEPT reads the same).
func can_dismiss() -> bool:
	var sim: Simulation = _view.sim() if _view != null else null
	return sim == null or not sim.is_armory_press_latched()


## Build the WEAPON screen over live play and claim it: the controller's on-show
## runs the class resolve, the stage and the populate from the live sim. The engine
## decides when (the held USE row's armory arm, engine controls::PlayerActions and
## world::LocalPlayer::take_use_item_press); the shell and the tests call this.
func open() -> bool:
	if is_open() or _view == null or _ui_parent == null or _view.sim() == null:
		return false
	if not _ensure_menu():
		return false
	_armory.mount(_driver, _menu_root)
	_armory.armory_screen.set_text_tables(Strings.get_table(Strings.TABLE_GAMETEXT),
			Strings.get_table(Strings.TABLE_MENUTXT), Strings.get_table(Strings.TABLE_GAMEUI))
	if not _driver.claim_screen(_armory.armory_screen):
		return false
	# The menu draws over every HUD element (the lazily built GameHud may have been
	# added after us).
	_ui_parent.move_child(_frame, _ui_parent.get_child_count() - 1)
	_frame.visible = true
	set_process(true)
	opened.emit()
	return true


func close() -> void:
	if not is_open():
		return
	# A dropdown left open would come back mid-popup on the next open (the frame
	# only hides; the driver's per-widget state persists across shows).
	_driver.close_active_combo_popup()
	_driver.release_screen()
	_armory.on_menu_released()
	_frame.visible = false
	# The menu's pointer is the menu's alone (MenuFrameSurface.hide_frame).
	Input.set_custom_mouse_cursor(null, Input.CURSOR_ARROW)
	set_process(false)
	closed.emit()


func _process(_delta: float) -> void:
	if not is_open():
		set_process(false)
		return
	# The menu's pump, once a frame after the input events (MenuFrameSurface).
	_driver.pump_mouse()
	# The blink/marquee clock rides the OS tick like the original's GetTickCount
	# gate (the shell does the same for the front-end menus).
	_driver.tick(_view.frame_clock_ms)


# A useitem key's press while the screen is open is the ACCEPT accelerator,
# unless the engine's armory press latch holds it (MainGame._input).
func _unhandled_key_input(event: InputEvent) -> void:
	if not is_open():
		return
	var key := event as InputEventKey
	if key == null or key.echo or not key.pressed or not is_accept_hotkey(bound_key_of(key)):
		return
	if can_dismiss():
		_armory.armory_screen.trigger_accept()
		get_viewport().set_input_as_handled()


func teardown() -> void:
	set_process(false)
	if _frame != null and is_instance_valid(_frame):
		_frame.queue_free()
	if _audio != null and is_instance_valid(_audio):
		_audio.queue_free()
	_frame = null
	_audio = null
	_driver = null
	_menu_root = null


# Build the compiled menu surface over the gameplay view: the same
# MenuFrame + MenuAudio + MenuDriver stack the menu shell drives, fed weapon.mnu
# from the WORLD's mounted resource root, with the canonical stylesheet and the
# current root's menutxt/gametext tables. Direct/headless world owners may not
# have run the front-end text bootstrap.
func _ensure_menu() -> bool:
	if _driver != null and _frame != null and is_instance_valid(_frame):
		return true
	var root: ResourceRoot = _view.resource_root()
	var surface := MenuFrameSurface.open_surface(root, _ui_parent, _layout_control,
			MENU_FILE, MENU_SCREEN, "ArmoryMenu", "ArmoryPresenter", _on_frame_gui_input,
			func(_built: MenuDriver) -> void: _register_text_tables(root))
	if surface == null:
		return false
	_frame = surface.frame
	_audio = surface.audio
	_driver = surface.driver
	_menu_root = root
	return true


# The compiled frame is a passive surface; MenuFrameSurface.forward_gui_input
# forwards its gui input to the driver the way MenuShell does. The armory's
# lists also take the wheel.
func _on_frame_gui_input(event: InputEvent) -> void:
	if _driver == null or not is_open():
		return
	MenuFrameSurface.forward_gui_input(event, _driver, _frame)
	var button := event as InputEventMouseButton
	if button != null and button.pressed and (button.button_index == MOUSE_BUTTON_WHEEL_DOWN \
			or button.button_index == MOUSE_BUTTON_WHEEL_UP):
		# One notch = one row tick (D-MNU-18 deliberate divergence).
		if _driver.process_wheel(button.position,
				1 if button.button_index == MOUSE_BUTTON_WHEEL_DOWN else -1):
			_frame.accept_event()


# A local ACCEPT presented the equipped slot's viewmodel: the FP view rebuilds.
func _on_viewmodel_changed() -> void:
	if _player_presenter != null:
		_player_presenter.refresh_viewmodel()


# The armory's text lookups (WepDes weapon names, CHARCLASS_* rows, TOTAL_WEIGHT)
# ride the shared Strings registry. The game shell registers these at boot, but
# direct/headless world owners may not, so every menu build re-registers all
# three from the world's current root, replacing what the registry held; a table
# that root lacks is unregistered (register_table(null) erases it).
# [orig: Game_InitSubsystems @0x4a6cd0 loads menutxt/gametext at boot]
func _register_text_tables(root: ResourceRoot) -> void:
	for spec in [[Strings.TABLE_MENUTXT, "menutxt.BIN"], [Strings.TABLE_GAMETEXT, "gametext.bin"],
			[Strings.TABLE_GAMEUI, "Game.bin"]]:
		Strings.register_table(spec[0], Strings.load_rtxt(root, spec[1]))


func _recompute_fit() -> void:
	# MenuFrameSurface.fit_frame (shared with the other presenters).
	MenuFrameSurface.fit_frame(_frame, _layout_control, _ui_parent)
