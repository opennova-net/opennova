extends Node3D

# Runtime shell: boots into the game's menu front-end (NovaMenuHost, driving the
# .mnu menu set + audio from the chosen resource dir) and hands off to a GameWorld
# when the player starts a mission, with pause + return-to-menu on demand. The
# engine ships no game data; everything (menus, audio, terrain, missions) loads
# from the chosen resource dir. The first-launch directory picker lives here
# (runtime-only); headless probes set the dir explicitly and never block on it.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")

# Re-summon the game-folder picker. The original engine has no "change game dir"
# control (the game *is* its install folder); this is an OpenNova convenience so a
# wrong / menu-less folder can be re-picked without restarting. Front-end only.
const CHANGE_DIR_KEY := KEY_F9

enum State { MENU, WORLD, PAUSED }

@onready var _world: GameWorld = $World
@onready var _camera: Camera3D = $Camera3D
@onready var _hud: CanvasLayer = $HUD
@onready var _menu_host = $MenuLayer/MenuHost

var _picker: FileDialog
var _root: NovaResourceRoot
var _state: int = State.MENU
var _host_wired := false


func _ready() -> void:
	if _world == null or _camera == null or _menu_host == null:
		return
	# Esc toggles pause/resume in a world (the fly camera reports the key; the
	# host decides what it means).
	if _camera.has_signal("escape_pressed") and not _camera.is_connected("escape_pressed", _on_camera_escape):
		_camera.connect("escape_pressed", _on_camera_escape)
	var dir := ResourceDirSettings.get_resource_dir()
	if dir.is_empty():
		_request_resource_dir()
		return
	_enter_menu(dir)


# F9 (re)opens the asset-folder picker from the front-end so the player can point
# the runtime at a different game folder. Restricted to the menu state so an active
# mission is never yanked out from under a remount; ignored while a picker is open.
func _unhandled_key_input(event: InputEvent) -> void:
	var key := event as InputEventKey
	if key == null or not key.pressed or key.echo:
		return
	if key.keycode == CHANGE_DIR_KEY and _can_summon_dir_picker():
		_request_resource_dir()
		get_viewport().set_input_as_handled()


# Whether the folder picker may be summoned right now: only from the menu front-end
# and only when one is not already open. Pure predicate so it is unit-testable
# headless (the native dialog itself cannot be shown without a display).
func _can_summon_dir_picker() -> bool:
	return _state == State.MENU and _picker == null


# --- Menu state ---------------------------------------------------------------

func _enter_menu(dir: String) -> void:
	if _root == null or _root.get_root_dir() != dir:
		var root := _mount_runtime_root(dir)
		if root == null:
			_request_resource_dir()
			return
		_root = root
	_state = State.MENU
	_world.visible = false
	_set_hud_visible(false)
	_wire_host()
	if not _menu_host.setup(_root):
		push_warning("MainGame: no menu found in resource dir (looked for %s)" % _menu_host.main_menu_file)
	_menu_host.show_menu()


func _wire_host() -> void:
	if _host_wired:
		return
	_host_wired = true
	_menu_host.start_requested.connect(_on_start_requested)
	_menu_host.exit_to_desktop_requested.connect(_on_exit_to_desktop)
	_menu_host.return_to_menu_requested.connect(_on_return_to_menu)
	_menu_host.resume_requested.connect(_on_resume)
	if _menu_host.has_signal("novaworld_requested"):
		_menu_host.novaworld_requested.connect(_on_novaworld_requested)


# --- Resource dir picker (first launch) ---------------------------------------

func _request_resource_dir() -> void:
	if DisplayServer.get_name() == "headless" or _picker != null:
		return
	_picker = FileDialog.new()
	_picker.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	_picker.access = FileDialog.ACCESS_FILESYSTEM
	_picker.use_native_dialog = true
	_picker.title = "Select your OpenNova asset directory"
	_picker.dir_selected.connect(_on_dir_selected)
	_picker.canceled.connect(_on_dir_canceled)
	add_child(_picker)
	_picker.popup_centered_ratio(0.6)


func _on_dir_selected(dir: String) -> void:
	_cleanup_picker()
	var root := _mount_runtime_root(dir)
	if root == null:
		_request_resource_dir()
		return
	_root = root
	ResourceDirSettings.set_resource_dir(dir)
	_enter_menu(dir)


# Mount `dir` as the runtime resource root (packed PFFs, `/exp` expansion, `/d` loose
# override). Warns and returns null on failure.
func _mount_runtime_root(dir: String) -> NovaResourceRoot:
	var root := NovaResourceRoot.new()
	var expansion := NovaLaunchFlags.expansion(ResourceDirSettings.get_expansion())
	if root.mount_runtime(dir, expansion, NovaLaunchFlags.loose_override_enabled()) != OK:
		push_warning("MainGame: %s" % root.get_last_error())
		return null
	return root


func _on_dir_canceled() -> void:
	_cleanup_picker()
	_request_resource_dir()


func _cleanup_picker() -> void:
	if _picker != null:
		_picker.queue_free()
		_picker = null


# --- NovaWorld (online multiplayer) ------------------------------------------

var _novaworld_panel: NovaWorldPanel

func _on_novaworld_requested() -> void:
	if _novaworld_panel != null:
		return
	_novaworld_panel = NovaWorldPanel.new()
	# Dev default: localhost. A prod build sets the server host from the
	# resolved server IP before showing the panel.
	_menu_host.hide_menu()
	$MenuLayer.add_child(_novaworld_panel)
	_novaworld_panel.closed.connect(_on_novaworld_closed)


func _on_novaworld_closed() -> void:
	if _novaworld_panel != null:
		_novaworld_panel.queue_free()
		_novaworld_panel = null
	_menu_host.show_menu()


# --- Menu <-> world transitions ----------------------------------------------

func _on_start_requested(bms_name: String) -> void:
	_menu_host.hide_menu()
	_world.visible = true
	_set_hud_visible(true)
	_state = State.WORLD
	if not _world.world_loaded.is_connected(_on_world_loaded):
		_world.world_loaded.connect(_on_world_loaded)
	if not _world.load_failed.is_connected(_on_world_load_failed):
		_world.load_failed.connect(_on_world_load_failed)
	_world.load_mission(bms_name)


func _on_world_loaded() -> void:
	_menu_host.enter_game_music()


func _on_world_load_failed(reason: String) -> void:
	push_warning("MainGame: mission load failed: %s" % reason)
	if _root != null:
		_enter_menu(_root.get_root_dir())


func _on_camera_escape() -> void:
	# Esc: pause <-> resume while in a world; ignored in the main menu (EXIT quits).
	if _state == State.WORLD:
		_pause()
	elif _state == State.PAUSED:
		_on_resume()


func _pause() -> void:
	_state = State.PAUSED
	_menu_host.open_ingame_menu()  # game.mnu overlay over the kept-loaded world
	_menu_host.show_menu()


func _on_resume() -> void:
	if _state != State.PAUSED:
		return
	_menu_host.hide_menu()
	_state = State.WORLD


func _on_return_to_menu() -> void:
	_world.unload()
	if _root != null:
		_enter_menu(_root.get_root_dir())


func _on_exit_to_desktop() -> void:
	get_tree().quit()


# CanvasLayer contents toggle: hide/show the HUD's CanvasItem children (the FPS
# label + debug label) so they do not draw over the menu.
func _set_hud_visible(v: bool) -> void:
	if _hud == null:
		return
	for c in _hud.get_children():
		if c is CanvasItem:
			(c as CanvasItem).visible = v


# Drive the loaded world's per-frame foliage coverage. Tick whenever a world is
# loaded and not paused (the pause menu freezes it); tick() itself no-ops until the
# world finishes loading. Gating on "loaded, not paused" rather than State.WORLD
# also lets a host that drives load_world() directly (the headless runtime probe,
# which stays in MENU) keep dispatching foliage.
func _process(_delta: float) -> void:
	if _state != State.PAUSED and _world.is_loaded():
		_world.tick(_camera.global_position)
