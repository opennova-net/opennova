class_name OnedApp
extends Control

## ONED intentionally has one small surface. It remembers where game data and
## a retail install live, then owns one OpenNova-or-retail child process.

const MIN_WINDOW_SIZE := Vector2i(760, 430)
const RECENT_PLACEHOLDER := "__placeholder__"
const CLEAR_RECENTS := "__clear__"

@onready var resource_dir_edit: LineEdit = %ResourceDirEdit
@onready var recent_dirs_option: OptionButton = %RecentDirsOption
@onready var game_code_edit: LineEdit = %GameCodeEdit
@onready var expansion_edit: LineEdit = %ExpansionEdit
@onready var retail_dir_edit: LineEdit = %RetailDirEdit
@onready var run_opennova_button: Button = %RunOpenNovaButton
@onready var run_retail_button: Button = %RunRetailButton
@onready var stop_button: Button = %StopButton
@onready var status_label: Label = %StatusLabel
@onready var resource_dir_dialog: FileDialog = %ResourceDirDialog
@onready var retail_dir_dialog: FileDialog = %RetailDirDialog

var _session: GameRunSession
var _previous_window_min_size := Vector2i.ZERO
var _resource_dir_is_implicit := false
var _implicit_resource_dir := ""


func _ready() -> void:
	var user_args := OS.get_cmdline_user_args()
	if PackGameCli.wants_run(user_args):
		get_tree().quit(PackGameCli.run(user_args))
		return

	get_tree().auto_accept_quit = false
	_configure_window()
	_session = GameRunSession.new()
	_session.status_changed.connect(_on_status_changed)
	_session.state_changed.connect(_on_session_state_changed)
	_connect_controls()
	_load_settings()
	_rebuild_recents()
	_refresh_actions()
	_show_status("Choose loose or packed game data, then run OpenNova or retail.")


func _exit_tree() -> void:
	if _session != null:
		_session.shutdown()
	var window := get_window()
	if window != null:
		window.min_size = _previous_window_min_size


func _process(_delta: float) -> void:
	if _session != null:
		_session.poll()


func _notification(what: int) -> void:
	if what != NOTIFICATION_WM_CLOSE_REQUEST:
		return
	_commit_settings()
	if _session == null or _session.shutdown():
		get_tree().quit()


func _shortcut_input(event: InputEvent) -> void:
	if not (event is InputEventKey):
		return
	var key := event as InputEventKey
	if not key.pressed or key.is_echo() or key.ctrl_pressed or key.alt_pressed:
		return
	match key.keycode:
		KEY_F5:
			_run_opennova()
			get_viewport().set_input_as_handled()
		KEY_F7:
			_run_retail()
			get_viewport().set_input_as_handled()
		KEY_F8:
			_stop_game()
			get_viewport().set_input_as_handled()


func get_run_session() -> GameRunSession:
	return _session


## Replace the managed run session (tests inject a stub); its status and
## state signals are rewired to the app.
func attach_run_session(session: GameRunSession) -> void:
	if _session != null and _session != session:
		if _session.status_changed.is_connected(_on_status_changed):
			_session.status_changed.disconnect(_on_status_changed)
		if _session.state_changed.is_connected(_on_session_state_changed):
			_session.state_changed.disconnect(_on_session_state_changed)
	_session = session
	if _session != null:
		if not _session.status_changed.is_connected(_on_status_changed):
			_session.status_changed.connect(_on_status_changed)
		if not _session.state_changed.is_connected(_on_session_state_changed):
			_session.state_changed.connect(_on_session_state_changed)
	_refresh_actions()


## The F8 action: stop the managed process (a no-op status when nothing runs).
func stop_game() -> void:
	_stop_game()


## Persist the four fields to the ONED config (the close/run path).
func commit_settings() -> void:
	_commit_settings()


## True when the resource directory field holds an explicit user value rather
## than the displayed packaged-assets fallback.
func should_persist_resource_dir() -> bool:
	return _should_persist_resource_dir()


## Show `path` as the implicit packaged-assets fallback: displayed, never
## persisted until the user types over it.
func set_implicit_resource_dir(path: String) -> void:
	resource_dir_edit.text = path
	_resource_dir_is_implicit = true
	_implicit_resource_dir = path
	_refresh_actions()


static func bundled_assets_dir(executable_dir: String) -> String:
	var candidate := executable_dir.path_join("assets").simplify_path()
	return candidate if ResourceDirSettings.is_valid_root(candidate) else ""


func _connect_controls() -> void:
	%BrowseResourceButton.pressed.connect(_browse_resource_dir)
	%BrowseRetailButton.pressed.connect(_browse_retail_dir)
	resource_dir_dialog.dir_selected.connect(_select_resource_dir)
	retail_dir_dialog.dir_selected.connect(_select_retail_dir)
	recent_dirs_option.item_selected.connect(_select_recent)
	run_opennova_button.pressed.connect(_run_opennova)
	run_retail_button.pressed.connect(_run_retail)
	stop_button.pressed.connect(_stop_game)

	resource_dir_edit.text_changed.connect(_on_resource_dir_text_changed)
	game_code_edit.text_changed.connect(func(_text: String) -> void: _refresh_actions())
	expansion_edit.text_changed.connect(func(_text: String) -> void: _refresh_actions())
	retail_dir_edit.text_changed.connect(func(_text: String) -> void: _refresh_actions())
	resource_dir_edit.text_submitted.connect(func(_text: String) -> void: _apply_resource_dir())
	game_code_edit.text_submitted.connect(func(_text: String) -> void: _commit_profile())
	expansion_edit.text_submitted.connect(func(_text: String) -> void: _commit_profile())
	retail_dir_edit.text_submitted.connect(func(_text: String) -> void: _apply_retail_dir())
	resource_dir_edit.focus_exited.connect(_apply_resource_dir)
	game_code_edit.focus_exited.connect(_commit_profile)
	expansion_edit.focus_exited.connect(_commit_profile)
	retail_dir_edit.focus_exited.connect(_apply_retail_dir)


func _load_settings() -> void:
	var configured := OnedSettings.get_resource_dir()
	var use_bundled_default := configured.is_empty()
	if use_bundled_default:
		configured = bundled_assets_dir(OS.get_executable_path().get_base_dir())
	resource_dir_edit.text = configured
	# Record this after assigning the field so merely displaying the packaged
	# assets/ fallback never turns it into persisted user state.
	_resource_dir_is_implicit = use_bundled_default
	_implicit_resource_dir = configured if use_bundled_default else ""
	game_code_edit.text = OnedSettings.get_game()
	expansion_edit.text = OnedSettings.get_expansion()
	retail_dir_edit.text = OnedSettings.get_retail_dir()


func _commit_settings() -> void:
	if _should_persist_resource_dir():
		OnedSettings.set_resource_dir(resource_dir_edit.text)
	OnedSettings.set_game(game_code_edit.text)
	OnedSettings.set_expansion(expansion_edit.text)
	OnedSettings.set_retail_dir(retail_dir_edit.text)


func _apply_resource_dir() -> void:
	if _should_persist_resource_dir():
		_resource_dir_is_implicit = false
		OnedSettings.set_resource_dir(resource_dir_edit.text)
	_rebuild_recents()
	_refresh_actions()


func _apply_retail_dir() -> void:
	OnedSettings.set_retail_dir(retail_dir_edit.text)
	_refresh_actions()


func _commit_profile() -> void:
	OnedSettings.set_game(game_code_edit.text)
	OnedSettings.set_expansion(expansion_edit.text)
	game_code_edit.text = OnedSettings.get_game()
	expansion_edit.text = OnedSettings.get_expansion()
	_refresh_actions()


func _browse_resource_dir() -> void:
	_open_dir_dialog(resource_dir_dialog, resource_dir_edit.text)


func _browse_retail_dir() -> void:
	_open_dir_dialog(retail_dir_dialog, retail_dir_edit.text)


func _open_dir_dialog(dialog: FileDialog, current: String) -> void:
	var clean := current.strip_edges()
	if DirAccess.dir_exists_absolute(clean):
		dialog.current_dir = clean
	dialog.popup_centered_ratio(0.8)


func _select_resource_dir(path: String) -> void:
	_resource_dir_is_implicit = false
	resource_dir_edit.text = path
	_apply_resource_dir()


func _select_retail_dir(path: String) -> void:
	retail_dir_edit.text = path
	_apply_retail_dir()


func _rebuild_recents() -> void:
	recent_dirs_option.clear()
	recent_dirs_option.add_item("Recent resource directories…")
	recent_dirs_option.set_item_metadata(0, RECENT_PLACEHOLDER)
	recent_dirs_option.set_item_disabled(0, true)
	for dir in OnedSettings.get_recent_dirs():
		recent_dirs_option.add_item(String(dir))
		recent_dirs_option.set_item_metadata(recent_dirs_option.item_count - 1, String(dir))
	if recent_dirs_option.item_count > 1:
		recent_dirs_option.add_separator()
		recent_dirs_option.add_item("Clear recent directories")
		recent_dirs_option.set_item_metadata(recent_dirs_option.item_count - 1, CLEAR_RECENTS)
	recent_dirs_option.select(0)


func _select_recent(index: int) -> void:
	var value := String(recent_dirs_option.get_item_metadata(index))
	if value == CLEAR_RECENTS:
		OnedSettings.clear_recent_dirs()
		_rebuild_recents()
		return
	if value == RECENT_PLACEHOLDER or value.is_empty():
		return
	_resource_dir_is_implicit = false
	resource_dir_edit.text = value
	_apply_resource_dir()


func _on_resource_dir_text_changed(_text: String) -> void:
	_resource_dir_is_implicit = false
	_refresh_actions()


func _should_persist_resource_dir() -> bool:
	return not _resource_dir_is_implicit or resource_dir_edit.text != _implicit_resource_dir


func _run_opennova() -> void:
	_commit_settings()
	_rebuild_recents()
	_session.run_opennova(resource_dir_edit.text, game_code_edit.text, expansion_edit.text)
	_refresh_actions()


func _run_retail() -> void:
	_commit_settings()
	_rebuild_recents()
	_session.run_retail(resource_dir_edit.text, retail_dir_edit.text)
	_refresh_actions()


func _stop_game() -> void:
	if not _session.is_running():
		_show_status("Nothing is running.")
	else:
		_session.stop()
	_refresh_actions()


func _refresh_actions() -> void:
	if _session == null:
		return
	var open_reason := _session.opennova_readiness(
			resource_dir_edit.text, game_code_edit.text, expansion_edit.text)
	run_opennova_button.disabled = not open_reason.is_empty()
	run_opennova_button.tooltip_text = open_reason if not open_reason.is_empty() \
			else "Run OpenNova directly from this loose or packed game-data directory (F5)."

	var retail_reason := _session.retail_readiness(resource_dir_edit.text, retail_dir_edit.text)
	run_retail_button.disabled = not retail_reason.is_empty()
	run_retail_button.tooltip_text = retail_reason if not retail_reason.is_empty() \
			else "Stage the selected game data and run the retail install (F7)."
	stop_button.disabled = not _session.is_running()
	stop_button.tooltip_text = "Stop the managed process (F8)."


func _on_status_changed(text: String, kind: StringName) -> void:
	_show_status(text, kind)


func _on_session_state_changed(_state: Dictionary) -> void:
	_refresh_actions()


func _show_status(text: String, kind: StringName = &"info") -> void:
	status_label.text = text
	var color := Color("b8c0cc")
	if kind == &"error":
		color = Color("ff8f8f")
	elif kind == &"warn":
		color = Color("ffd37a")
	status_label.add_theme_color_override("font_color", color)


func _configure_window() -> void:
	var window := get_window()
	if window == null:
		return
	_previous_window_min_size = window.min_size
	window.title = "ONED"
	window.min_size = MIN_WINDOW_SIZE
	if window.mode == Window.MODE_WINDOWED:
		var next_size := window.size
		next_size.x = maxi(next_size.x, MIN_WINDOW_SIZE.x)
		next_size.y = maxi(next_size.y, MIN_WINDOW_SIZE.y)
		window.size = next_size
