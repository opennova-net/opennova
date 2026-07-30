class_name ShellGameSession
extends RefCounted

## One managed standalone game process for ONED. The session owns launch
## lifecycle; UI and optional runtime-debug adapters use this public surface.
## Starting another mode restarts the one child. Nothing here saves, exports,
## copies, or stages authored data.

const RUNTIME_SCENE := "res://game/main_game.tscn"
const PACKAGED_RUNTIME_CANDIDATES: Array[String] = [
	"opennova.exe", "opennova.x86_64", "opennova",
	"../../../opennova.app/Contents/MacOS/opennova",
]
const STOP_GRACE_MSEC := 1000

enum Mode { GAME, CURRENT_MISSION }
enum State { STOPPED, RUNNING, STOPPING }

signal run_started(run_id: String, descriptor_path: String, pid: int)
signal run_stopped(run_id: String, descriptor_path: String)


class GameRunRequest:
	extends RefCounted

	var mode: int = 0  # ShellGameSession.Mode.GAME
	var resource_dir: String = ""
	var expansion: String = ""
	var game_code: String = "jo"
	var loose_mission: String = ""
	var unsaved_workspaces := PackedStringArray()
	var run_id: String = ""
	var descriptor_path: String = ""
	var log_path: String = ""
	# Opaque extension point for another shell adapter. The session's own
	# identity flags are always appended before these.
	var extra_args := PackedStringArray()

	static func make(
		request_mode: int,
		dir: String,
		exp: String,
		game: String
	) -> GameRunRequest:
		var request := GameRunRequest.new()
		request.mode = request_mode
		request.resource_dir = dir
		request.expansion = exp
		request.game_code = game
		return request


class LaunchPlan:
	extends RefCounted

	var path: String
	var args: PackedStringArray

	static func make(plan_path: String, plan_args: PackedStringArray) -> LaunchPlan:
		var plan := LaunchPlan.new()
		plan.path = plan_path
		plan.args = plan_args
		return plan


# Suppliers.
var _resource_dir: Callable
var _expansion: Callable
var _game_code: Callable
# func() -> {path: String}
var _current_mission: Callable
# func() -> PackedStringArray: labels for every dirty editor workspace.
var _unsaved_workspaces: Callable

# OS/process seams.
var _spawn: Callable
var _file_exists: Callable
var _is_process_running: Callable
var _kill_process: Callable
var _now_msec: Callable
var _show_status: Callable

# Optional cross-process control seams. No MCP dependency lives here.
# forwarder(run_id, descriptor_path, tool, args) -> Variant
# quit_requester(run_id, descriptor_path) -> bool
var _runtime_tool_forwarder: Callable
var _runtime_quit_requester: Callable
var _runtime_debug_enabled := false

var _state: int = State.STOPPED
var _pid := -1
var _active_request: GameRunRequest
var _queued_request: GameRunRequest
var _stop_deadline_msec := 0
var _last_error := ""


func setup(
	resource_dir: Callable,
	expansion: Callable,
	game_code: Callable,
	current_mission: Callable,
	unsaved_workspaces: Callable,
	spawn: Callable,
	file_exists: Callable,
	show_status: Callable,
	is_process_running: Callable = Callable(),
	kill_process: Callable = Callable(),
	now_msec: Callable = Callable()
) -> void:
	_resource_dir = resource_dir
	_expansion = expansion
	_game_code = game_code
	_current_mission = current_mission
	_unsaved_workspaces = unsaved_workspaces
	_spawn = spawn
	_file_exists = file_exists
	_show_status = show_status
	_is_process_running = is_process_running
	_kill_process = kill_process
	_now_msec = now_msec


## Install the optional runtime peer after both shell and MCP service exist.
func set_runtime_control_hooks(
	tool_forwarder: Callable,
	quit_requester: Callable = Callable()
) -> void:
	_runtime_tool_forwarder = tool_forwarder
	_runtime_quit_requester = quit_requester


## Gate the opaque handshake metadata used by an optional runtime-debug
## adapter. Normal F5/F6 launches stay free of debug identity and log flags.
func set_runtime_debug_enabled(value: bool) -> void:
	_runtime_debug_enabled = value


## Forget a live child's retired runtime-debug identity without disturbing the
## child process itself. The expected id prevents a late endpoint-retirement
## continuation from clearing a replacement run.
func retire_runtime_debug_identity(expected_run_id: String) -> bool:
	if _active_request == null or expected_run_id.is_empty() \
			or _active_request.run_id != expected_run_id:
		return false
	_active_request.run_id = ""
	_active_request.descriptor_path = ""
	_active_request.log_path = ""
	return true


## JSON-safe state for toolbar and MCP consumers.
func get_state() -> Variant:
	return {
		"state": _state_name(),
		"running": _state != State.STOPPED,
		"pid": _pid,
		"mode": _mode_name(_active_request.mode) if _active_request != null else "",
		"loose_mission": _active_request.loose_mission if _active_request != null else "",
		"unsaved_workspaces": Array(_active_request.unsaved_workspaces) \
				if _active_request != null else [],
		"run_id": get_current_run_id(),
		"descriptor_path": get_current_descriptor_path(),
		"log_path": _active_request.log_path if _active_request != null else "",
		"last_error": _last_error,
	}


func get_current_run_id() -> String:
	return _active_request.run_id if _active_request != null else ""


func get_current_descriptor_path() -> String:
	return _active_request.descriptor_path if _active_request != null else ""


func get_pid() -> int:
	return _pid


func is_running() -> bool:
	return _state != State.STOPPED


func get_last_error() -> String:
	return _last_error


func available() -> bool:
	if not _file_exists.is_valid():
		return false
	# Runtime availability is independent of whether the user has selected an
	# asset directory; the UI needs to distinguish those two remedies.
	var request := GameRunRequest.make(
		Mode.GAME,
		".",
		String(_expansion.call()) if _expansion.is_valid() else "",
		String(_game_code.call()) if _game_code.is_valid() else "jo")
	return _current_plan(request) != null


func can_run_current_mission() -> bool:
	return get_current_mission_unavailable_reason().is_empty()


func get_current_mission_unavailable_reason() -> String:
	# Toolbar capability polling must not overwrite the last real operation
	# error exposed through get_state()/MCP.
	var previous_error := _last_error
	var ready := _build_request(Mode.CURRENT_MISSION, false) != null
	var reason := "" if ready else _last_error
	_last_error = previous_error
	return reason


## Public MCP/toolbar entry. Accepted values are "game" and "mission".
func start_mode(mode: String) -> bool:
	match mode.strip_edges().to_lower():
		"game":
			return _start_built_request(Mode.GAME)
		"mission":
			return _start_built_request(Mode.CURRENT_MISSION)
		_:
			_last_error = "Unknown run mode '%s'." % mode
			_show(_last_error, &"error")
			return false


func stop() -> bool:
	_queued_request = null
	if _state == State.STOPPED:
		return false
	return _begin_stop()


## Editor shutdown cannot poll a grace window. Ask politely once, then force
## termination. If the OS rejects that request, retain the owned run identity
## and descriptor instead of falsely reporting the still-live child as stopped.
func shutdown() -> bool:
	_queued_request = null
	if _state == State.STOPPED:
		return true
	_request_graceful_quit()
	if not _kill_now():
		_last_error = "Could not stop the running game during editor shutdown."
		_show(_last_error, &"error")
		return false
	_finish_stopped(false)
	return true


## Detect natural exit and complete graceful-stop/restart transitions.
func poll() -> void:
	if _state == State.STOPPED:
		return
	if not _process_is_alive():
		var was_stopping := _state == State.STOPPING
		_finish_stopped(not was_stopping)
		_start_queued_request()
		return
	if _state == State.STOPPING and _ticks_msec() >= _stop_deadline_msec:
		if not _kill_now():
			_queued_request = null
			_state = State.RUNNING
			_last_error = "Could not stop the running game."
			_show(_last_error, &"error")
			return
		_finish_stopped(false)
		_start_queued_request()


func call_runtime_tool(tool: String, arguments: Dictionary = {}) -> Variant:
	if _state != State.RUNNING or not _runtime_tool_forwarder.is_valid():
		return {
			"ok": false,
			"error": "No controllable editor-launched game is running.",
		}
	return _runtime_tool_forwarder.call(
		get_current_run_id(),
		get_current_descriptor_path(),
		tool,
		arguments.duplicate(true))


## The game always mounts this exact directory with /d ([orig: `/d`
## loose-override, Game_ParseCommandLineAndInit @ 0x4a7310]), and --loose-root lets a
## directory holding no packed archives (a loose authoring root) play as the
## loose file set ONED is editing (ADR 0025). An F6 request adds the exact saved
## loose BMS; editor-managed identity is opaque to the game shell.
static func runtime_flags(request: GameRunRequest) -> PackedStringArray:
	var flags := PackedStringArray([
		"/d",
		"--resource-dir", request.resource_dir,
		"--loose-root",
	])
	var exp_name := request.expansion.strip_edges()
	if not exp_name.is_empty():
		flags.append("/exp")
		flags.append(exp_name)
	var code := request.game_code.strip_edges().to_lower()
	flags.append("/game")
	flags.append(code if not code.is_empty() else "jo")
	if request.mode == Mode.CURRENT_MISSION and not request.loose_mission.is_empty():
		flags.append("--loose-mission")
		flags.append(request.loose_mission)
	if not request.run_id.is_empty():
		flags.append("--oned-run-id")
		flags.append(request.run_id)
	if not request.descriptor_path.is_empty():
		flags.append("--oned-run-descriptor")
		flags.append(request.descriptor_path)
	if not request.log_path.is_empty():
		flags.append("--oned-run-log")
		flags.append(request.log_path)
	flags.append_array(request.extra_args)
	return flags


## Packaged and source launches place custom arguments after Godot's `--`.
static func launch_plan(
	editor_exe: String,
	project_dir: String,
	dev_mode: bool,
	request: GameRunRequest,
	file_exists: Callable
) -> LaunchPlan:
	var runtime_args := runtime_flags(request)
	var exe_dir := editor_exe.get_base_dir()
	for candidate in PACKAGED_RUNTIME_CANDIDATES:
		var path := exe_dir.path_join(candidate).simplify_path()
		if bool(file_exists.call(path)):
			var packaged_args := PackedStringArray()
			if not request.log_path.is_empty():
				packaged_args.append_array(PackedStringArray(["--log-file", request.log_path]))
			packaged_args.append("--")
			packaged_args.append_array(runtime_args)
			return LaunchPlan.make(path, packaged_args)
	if dev_mode:
		var args := PackedStringArray(["--path", project_dir])
		if not request.log_path.is_empty():
			args.append_array(PackedStringArray(["--log-file", request.log_path]))
		args.append_array(PackedStringArray([RUNTIME_SCENE, "--"]))
		args.append_array(runtime_args)
		return LaunchPlan.make(editor_exe, args)
	return null


## F6 accepts only an existing top-level loose .bms in the mounted resource
## directory. This validates disk state but never changes it.
static func validate_current_mission(
	path: String,
	resource_dir: String,
	file_exists: Callable
) -> Dictionary:
	var root := resource_dir.strip_edges()
	if root.is_empty():
		return {"ok": false, "reason": "Run game: pick a resource directory first (Settings)."}
	var source := path.strip_edges()
	if source.is_empty():
		return {"ok": false, "reason": "Run current mission (F6): open and save a mission first."}
	if source.get_extension().to_lower() != "bms":
		return {
			"ok": false,
			"reason": "Run current mission (F6) requires a saved loose .bms file.",
		}
	var absolute_root := ProjectSettings.globalize_path(root).simplify_path()
	var absolute_path := ProjectSettings.globalize_path(source).simplify_path()
	if not source.is_absolute_path():
		absolute_path = absolute_root.path_join(source).simplify_path()
	if _canonical_path(absolute_path.get_base_dir()) != _canonical_path(absolute_root):
		return {
			"ok": false,
			"reason": "Run current mission (F6) requires a top-level .bms in the resource directory.",
		}
	if not bool(file_exists.call(absolute_path)):
		return {
			"ok": false,
			"reason": "Run current mission (F6): the saved .bms no longer exists on disk.",
		}
	return {
		"ok": true,
		"name": absolute_path.get_file(),
		"path": absolute_path,
	}


static func _canonical_path(
		path: String,
		case_insensitive: bool = OS.get_name() == "Windows") -> String:
	var normalized := path.replace("\\", "/").rstrip("/")
	return normalized.to_lower() if case_insensitive else normalized


func _start_built_request(mode: int) -> bool:
	var request := _build_request(mode)
	if request == null:
		return false
	if _state != State.STOPPED:
		_queued_request = request
		if not _begin_stop():
			return false
		if _state == State.STOPPED:
			return _start_queued_request()
		return true
	return _spawn_request(request)


func _base_request(mode: int) -> GameRunRequest:
	if not _resource_dir.is_valid() or not _expansion.is_valid() \
			or not _game_code.is_valid():
		_last_error = "Run game is not configured."
		return null
	var dir := String(_resource_dir.call()).strip_edges()
	if dir.is_empty():
		_last_error = "Run game: pick a resource directory first (Settings)."
		return null
	return GameRunRequest.make(
		mode,
		ProjectSettings.globalize_path(dir).simplify_path(),
		String(_expansion.call()),
		String(_game_code.call()))


func _build_request(mode: int, report_error: bool = true) -> GameRunRequest:
	var request := _base_request(mode)
	if request == null:
		if report_error:
			_show(_last_error, &"warn")
		return null
	request.unsaved_workspaces = _read_unsaved_workspaces()
	if mode == Mode.CURRENT_MISSION:
		if not _current_mission.is_valid():
			_last_error = "Run current mission (F6) is unavailable."
			if report_error:
				_show(_last_error, &"warn")
			return null
		var info_v: Variant = _current_mission.call()
		var info: Dictionary = info_v if info_v is Dictionary else {}
		var validated := validate_current_mission(
			String(info.get("path", "")), request.resource_dir, _file_exists)
		if not bool(validated.get("ok", false)):
			_last_error = String(validated.get("reason", "Run current mission is unavailable."))
			if report_error:
				_show(_last_error, &"warn")
			return null
		request.loose_mission = String(validated["name"])
	_last_error = ""
	return request


func _read_unsaved_workspaces() -> PackedStringArray:
	var labels := PackedStringArray()
	if not _unsaved_workspaces.is_valid():
		return labels
	var supplied: Variant = _unsaved_workspaces.call()
	if supplied is PackedStringArray or supplied is Array:
		for value in supplied:
			var label := String(value).strip_edges()
			if not label.is_empty() and not labels.has(label):
				labels.append(label)
	return labels


func _stamp_identity(request: GameRunRequest) -> void:
	request.run_id = ""
	request.descriptor_path = ""
	request.log_path = ""
	if not _runtime_debug_enabled:
		return
	request.run_id = "%d-%08x" % [Time.get_ticks_usec(), randi()]
	request.descriptor_path = ProjectSettings.globalize_path(
		"user://oned-run-%s.json" % request.run_id)
	request.log_path = ProjectSettings.globalize_path(
		"user://oned-run-%s.log" % request.run_id)


func _spawn_request(request: GameRunRequest) -> bool:
	_stamp_identity(request)
	var plan := _current_plan(request)
	if plan == null:
		_last_error = "Run game: no game runtime is available beside this editor."
		_show(_last_error, &"error")
		return false
	var pid := int(_spawn.call(plan.path, plan.args))
	if pid <= 0:
		_last_error = "Could not launch the game runtime."
		_show(_last_error, &"error")
		return false
	_pid = pid
	_active_request = request
	_state = State.RUNNING
	_last_error = ""
	run_started.emit(request.run_id, request.descriptor_path, pid)
	var unsaved_note := _unsaved_note(request.unsaved_workspaces)
	if request.mode == Mode.CURRENT_MISSION:
		if not unsaved_note.is_empty():
			_show("Running saved %s; %s" % [request.loose_mission, unsaved_note], &"warn")
		else:
			_show("Running saved loose mission %s." % request.loose_mission, &"info")
	else:
		if not unsaved_note.is_empty():
			_show("Game launched from the saved loose assets; %s" % unsaved_note, &"warn")
		else:
			_show("Game launched from the saved loose assets.", &"info")
	return true


static func _unsaved_note(workspaces: PackedStringArray) -> String:
	if workspaces.is_empty():
		return ""
	return "unsaved changes in %s are not included." % ", ".join(workspaces)


func _current_plan(request: GameRunRequest) -> LaunchPlan:
	return launch_plan(
		OS.get_executable_path(),
		ProjectSettings.globalize_path("res://"),
		OS.has_feature("editor"),
		request,
		_file_exists)


func _begin_stop() -> bool:
	if _state == State.STOPPED:
		return false
	_last_error = ""
	_state = State.STOPPING
	if _request_graceful_quit():
		_stop_deadline_msec = _ticks_msec() + STOP_GRACE_MSEC
		return true
	if not _kill_now():
		_queued_request = null
		_state = State.RUNNING
		_last_error = "Could not stop the running game."
		_show(_last_error, &"error")
		return false
	_finish_stopped(false)
	return true


func _request_graceful_quit() -> bool:
	if not _runtime_quit_requester.is_valid() or _active_request == null:
		return false
	return bool(_runtime_quit_requester.call(
		_active_request.run_id,
		_active_request.descriptor_path))


func _process_is_alive() -> bool:
	if _pid <= 0:
		return false
	if _is_process_running.is_valid():
		return bool(_is_process_running.call(_pid))
	return OS.is_process_running(_pid)


func _kill_now() -> bool:
	if _pid <= 0 or not _process_is_alive():
		return true
	var err := int(_kill_process.call(_pid)) \
			if _kill_process.is_valid() else int(OS.kill(_pid))
	return err == OK


func _finish_stopped(report_exit: bool) -> void:
	var run_id := get_current_run_id()
	var descriptor_path := get_current_descriptor_path()
	var pid := _pid
	_remove_owned_descriptor(descriptor_path, run_id, pid)
	_state = State.STOPPED
	_pid = -1
	_active_request = null
	_stop_deadline_msec = 0
	if not run_id.is_empty():
		run_stopped.emit(run_id, descriptor_path)
	if report_exit:
		_show("Game process exited.", &"info")


static func _remove_owned_descriptor(path: String, run_id: String, pid: int) -> void:
	if path.is_empty() or run_id.is_empty() or not FileAccess.file_exists(path):
		return
	var file := FileAccess.open(path, FileAccess.READ)
	if file == null:
		return
	var parsed: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if parsed is Dictionary and String(parsed.get("run_id", "")) == run_id \
			and int(parsed.get("pid", -1)) == pid:
		DirAccess.remove_absolute(path)


func _start_queued_request() -> bool:
	if _queued_request == null or _state != State.STOPPED:
		return false
	var request := _queued_request
	_queued_request = null
	return _spawn_request(request)


func _ticks_msec() -> int:
	return int(_now_msec.call()) if _now_msec.is_valid() else Time.get_ticks_msec()


func _show(text: String, kind: StringName) -> void:
	if _show_status.is_valid() and not text.is_empty():
		_show_status.call(text, 0.0, kind)


func _state_name() -> String:
	match _state:
		State.RUNNING:
			return "running"
		State.STOPPING:
			return "stopping"
		_:
			return "stopped"


static func _mode_name(mode: int) -> String:
	return "mission" if mode == Mode.CURRENT_MISSION else "game"
