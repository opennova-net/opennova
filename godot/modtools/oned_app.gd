class_name OnedApp
extends Node

## ONED intentionally has one small surface. It remembers where game data and
## a retail install live, then owns one OpenNova-or-retail child process.
##
## The surface itself is the engine's ImGui window (OnedUi, ADR 0039): this
## app seeds its fields, pushes the state only it has (recents, readiness, the
## status line), and executes the typed requests the surface reports —
## settings persistence, the native directory dialogs and the child process
## stay here.

const MIN_WINDOW_SIZE := Vector2i(760, 430)

@onready var _ui: OnedUi = $Ui
@onready var resource_dir_dialog: FileDialog = $ResourceDirDialog
@onready var retail_dir_dialog: FileDialog = $RetailDirDialog

var _session: GameRunSession
var _previous_window_min_size := Vector2i.ZERO
var _resource_dir_is_implicit := false
var _implicit_resource_dir := ""
# Set by the window close: quit once the managed process has stopped.
var _quit_when_stopped := false


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
	resource_dir_dialog.dir_selected.connect(_select_resource_dir)
	retail_dir_dialog.dir_selected.connect(_select_retail_dir)
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
	_drain_requests()
	if _quit_when_stopped and (_session == null or not _session.is_stopping()):
		_quit_when_stopped = false
		if _session == null or _session.shutdown():
			get_tree().quit()


# The window close: stop the managed process without blocking the surface,
# then quit from _process once it has exited (shutdown() is the synchronous
# last resort for a stop that never completes).
func _notification(what: int) -> void:
	if what != NOTIFICATION_WM_CLOSE_REQUEST:
		return
	_commit_settings()
	if _session != null and _session.is_running():
		_session.stop()
	_quit_when_stopped = true


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


## The surface seam: tests and automation drive the same edges the mouse does
## (OnedUi.push_request) and read the same fields the window shows.
func get_ui() -> OnedUi:
	return _ui


## Execute every queued surface request now (the frame loop does this each frame).
func drain_ui_requests() -> void:
	_drain_requests()


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
	_ui.set_resource_dir(path)
	_resource_dir_is_implicit = true
	_implicit_resource_dir = path
	_refresh_actions()


static func bundled_assets_dir(executable_dir: String) -> String:
	var candidate := executable_dir.path_join("assets").simplify_path()
	return candidate if ResourceDirSettings.is_valid_root(candidate) else ""


func _drain_requests() -> void:
	if _ui == null:
		return
	while true:
		var request: OnedUiRequest = _ui.take_request()
		if request == null:
			return
		_handle_request(request)


func _handle_request(request: OnedUiRequest) -> void:
	match request.action:
		OnedUi.EDIT_RESOURCE_DIR:
			# A keystroke over the displayed fallback makes it the user's value.
			_resource_dir_is_implicit = false
			_refresh_actions()
		OnedUi.APPLY_RESOURCE_DIR:
			_apply_resource_dir()
		OnedUi.APPLY_RETAIL_DIR:
			_apply_retail_dir()
		OnedUi.COMMIT_PROFILE:
			_commit_profile()
		OnedUi.BROWSE_RESOURCE_DIR:
			_open_dir_dialog(resource_dir_dialog, _ui.get_resource_dir())
		OnedUi.BROWSE_RETAIL_DIR:
			_open_dir_dialog(retail_dir_dialog, _ui.get_retail_dir())
		OnedUi.SELECT_RECENT:
			_select_recent(request.index)
		OnedUi.CLEAR_RECENTS:
			OnedSettings.clear_recent_dirs()
			_rebuild_recents()
		OnedUi.RUN_OPENNOVA:
			_run_opennova()
		OnedUi.RUN_RETAIL:
			_run_retail()
		OnedUi.STOP:
			_stop_game()


func _load_settings() -> void:
	var configured := OnedSettings.get_resource_dir()
	var use_bundled_default := configured.is_empty()
	if use_bundled_default:
		configured = bundled_assets_dir(OS.get_executable_path().get_base_dir())
	_ui.set_resource_dir(configured)
	# Record this after seeding the field so merely displaying the packaged
	# assets/ fallback never turns it into persisted user state.
	_resource_dir_is_implicit = use_bundled_default
	_implicit_resource_dir = configured if use_bundled_default else ""
	_ui.set_game_code(OnedSettings.get_game())
	_ui.set_expansion(OnedSettings.get_expansion())
	_ui.set_retail_dir(OnedSettings.get_retail_dir())


func _commit_settings() -> void:
	if _should_persist_resource_dir():
		OnedSettings.set_resource_dir(_ui.get_resource_dir())
	OnedSettings.set_game(_ui.get_game_code())
	OnedSettings.set_expansion(_ui.get_expansion())
	OnedSettings.set_retail_dir(_ui.get_retail_dir())


func _apply_resource_dir() -> void:
	if _should_persist_resource_dir():
		_resource_dir_is_implicit = false
		OnedSettings.set_resource_dir(_ui.get_resource_dir())
	_rebuild_recents()
	_refresh_actions()


func _apply_retail_dir() -> void:
	OnedSettings.set_retail_dir(_ui.get_retail_dir())
	_refresh_actions()


func _commit_profile() -> void:
	OnedSettings.set_game(_ui.get_game_code())
	OnedSettings.set_expansion(_ui.get_expansion())
	_ui.set_game_code(OnedSettings.get_game())
	_ui.set_expansion(OnedSettings.get_expansion())
	_refresh_actions()


func _open_dir_dialog(dialog: FileDialog, current: String) -> void:
	var clean := current.strip_edges()
	if DirAccess.dir_exists_absolute(clean):
		dialog.current_dir = clean
	dialog.popup_centered_ratio(0.8)


func _select_resource_dir(path: String) -> void:
	_resource_dir_is_implicit = false
	_ui.set_resource_dir(path)
	_apply_resource_dir()


func _select_retail_dir(path: String) -> void:
	_ui.set_retail_dir(path)
	_apply_retail_dir()


func _rebuild_recents() -> void:
	_ui.set_recent_dirs(OnedSettings.get_recent_dirs())


func _select_recent(index: int) -> void:
	var dirs := OnedSettings.get_recent_dirs()
	if index < 0 or index >= dirs.size():
		return
	_resource_dir_is_implicit = false
	_ui.set_resource_dir(dirs[index])
	_apply_resource_dir()


func _should_persist_resource_dir() -> bool:
	return not _resource_dir_is_implicit or _ui.get_resource_dir() != _implicit_resource_dir


func _run_opennova() -> void:
	_commit_settings()
	_rebuild_recents()
	_session.run_opennova(_ui.get_resource_dir(), _ui.get_game_code(), _ui.get_expansion())
	_refresh_actions()


func _run_retail() -> void:
	_commit_settings()
	_rebuild_recents()
	_session.run_retail(_ui.get_resource_dir(), _ui.get_retail_dir())
	_refresh_actions()


func _stop_game() -> void:
	if not _session.is_running():
		_show_status("Nothing is running.")
	else:
		_session.stop()
	_refresh_actions()


func _refresh_actions() -> void:
	if _session == null or _ui == null:
		return
	_ui.set_readiness(
			_session.opennova_readiness(
					_ui.get_resource_dir(), _ui.get_game_code(), _ui.get_expansion()),
			_session.retail_readiness(_ui.get_resource_dir(), _ui.get_retail_dir()),
			_session.is_running())


func _on_status_changed(text: String, kind: StringName) -> void:
	_show_status(text, kind)


func _on_session_state_changed(_state: Dictionary) -> void:
	_refresh_actions()


func _show_status(text: String, kind: StringName = &"info") -> void:
	if _ui != null:
		_ui.set_status(text, kind)


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
