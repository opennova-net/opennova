@tool
class_name GameRunSession
extends RefCounted

## Owns one standalone child process for the Godot editor.
## Runs OpenNova over selected game data, or stages it for retail.
## Starting either target first stops the currently managed child.

signal status_changed(text: String, kind: StringName)
signal state_changed(state: Dictionary)

const RUNTIME_SCENE := "res://game/game_runtime_root.tscn"
const STOP_WAIT_MSEC := 5000
# The portable exit-wait fallback's poll interval.
const EXIT_POLL_MSEC := 50

enum Mode { OPENNOVA, RETAIL }
# STOPPING: the kill was sent; poll() finishes the stop (or reports the
# deadline) without blocking the surface.
enum State { STOPPED, RUNNING, STOPPING }


class Request:
	extends RefCounted

	var mode: int = Mode.OPENNOVA
	var resource_dir := ""
	var expansion := ""
	var game_code := "jo"
	var staged_dir := ""
	var exe_path := ""
	var mission_name := ""
	var loose_override := true
	var loose_root := true
	var loose_mission := true

	static func game(root: String, source: WorldSource) -> Request:
		var request := Request.new()
		request.mode = Mode.OPENNOVA
		request.resource_dir = root
		request.game_code = source.game_code
		request.expansion = source.expansion
		request.loose_override = source.source_kind != WorldSource.RETAIL_INSTALL
		request.loose_root = source.source_kind == WorldSource.LOOSE_SOURCE
		request.loose_mission = source.source_kind != WorldSource.RETAIL_INSTALL
		return request

	static func world(root: String, source: WorldSource) -> Request:
		var request := game(root, source)
		request.mission_name = source.mission_name
		return request

	static func retail(root: String, loose_override: bool) -> Request:
		var request := Request.new()
		request.mode = Mode.RETAIL
		request.resource_dir = root
		request.loose_override = loose_override
		return request


class LaunchPlan:
	extends RefCounted

	var path := ""
	var args := PackedStringArray()
	var cwd := ""

	static func make(plan_path: String, plan_args: PackedStringArray,
			plan_cwd: String = "") -> LaunchPlan:
		var plan := LaunchPlan.new()
		plan.path = plan_path
		plan.args = plan_args
		plan.cwd = plan_cwd
		return plan


var _state: int = State.STOPPED
var _pid := -1
var _active_request: Request
var _last_error := ""
# The asynchronous stop: the deadline the exiting process must meet, and the
# request (with its retail dir) queued to spawn once the previous run is gone.
var _stop_deadline_msec := 0
var _pending_request: Request = null
var _pending_retail_dir := ""


# The OS/process/staging platform the session drives (RunSessionPlatform: the
# production body by default; the deterministic tests hand in a fake).
var _platform: RunSessionPlatform


func _init(platform: RunSessionPlatform = null) -> void:
	_platform = platform if platform != null else RunSessionPlatform.new()


func get_state() -> Dictionary:
	return {
		"state": _state_name(_state),
		"running": _state == State.RUNNING,
		"pid": _pid,
		"mode": _mode_name(_active_request.mode) if _active_request != null else "",
		"last_error": _last_error,
	}


func is_running() -> bool:
	return _state == State.RUNNING


## A stop was requested and the process has not exited yet (poll() drives it).
func is_stopping() -> bool:
	return _state == State.STOPPING


func get_last_error() -> String:
	return _last_error


func opennova_readiness(resource_dir: String) -> String:
	var root := _absolute_root(resource_dir)
	if root.is_empty() or not _platform.valid_resource_dir(root):
		return "Select a valid resource directory first."
	return ""


func retail_readiness(resource_dir: String, retail_dir: String) -> String:
	var root := _absolute_root(resource_dir)
	if root.is_empty() or not _platform.valid_resource_dir(root):
		return "Select a valid resource directory first."
	if not _platform.supports_working_directory():
		return "Retail launch requires working-directory process support (Windows only)."
	return _platform.retail_install_error(retail_dir.strip_edges())


## Open the game menu using the selected world's mount policy.
func run_game(resource_dir: String, source: WorldSource) -> bool:
	if source == null:
		return _fail("Select a WorldSource before Run Game.", &"warn")
	var reason := opennova_readiness(resource_dir)
	if not reason.is_empty():
		return _fail(reason, &"warn")
	return _replace_with(Request.game(_absolute_root(resource_dir), source))


## Play the selected saved mission with the same mount policy as its preview.
func run_world(resource_dir: String, source: WorldSource) -> bool:
	if source == null or source.mission_name != source.mission_name.get_file() \
			or source.mission_name.get_extension().to_lower() != "bms":
		return _fail("Select a top-level BMS mission before Play World.", &"warn")
	var reason := opennova_readiness(resource_dir)
	if not reason.is_empty():
		return _fail(reason, &"warn")
	return _replace_with(Request.world(_absolute_root(resource_dir), source))


func run_retail(resource_dir: String, retail_dir: String, loose_override: bool = true) -> bool:
	var reason := retail_readiness(resource_dir, retail_dir)
	if not reason.is_empty():
		return _fail(reason, &"warn")
	return _replace_with(Request.retail(_absolute_root(resource_dir), loose_override), retail_dir.strip_edges())


## Ask the managed process to stop. Returns true when the stop is under way
## (poll() reports the exit, or the STOP_WAIT_MSEC deadline) and false when
## nothing was running or the kill failed. Never blocks the surface.
func stop() -> bool:
	if _state == State.STOPPED:
		return false
	if _state == State.STOPPING:
		return true
	if not _platform.process_is_alive(_pid):
		_finish_stopped(false)
		return true
	if not _platform.kill_process(_pid):
		return _fail("Could not stop the running process.")
	_state = State.STOPPING
	_stop_deadline_msec = _platform.now_msec() + STOP_WAIT_MSEC
	state_changed.emit(get_state())
	_status("Stopping the managed process...")
	return true


func poll() -> void:
	match _state:
		State.RUNNING:
			if not _platform.process_is_alive(_pid):
				_finish_stopped(true)
		State.STOPPING:
			if not _platform.process_is_alive(_pid):
				_finish_stopped(false)
				_status("Stopped the managed process.")
				_spawn_pending()
			elif _platform.now_msec() >= _stop_deadline_msec:
				_pending_request = null
				_pending_retail_dir = ""
				_state = State.RUNNING
				_fail("The running process did not exit after it was stopped.")


## The synchronous last resort when the editor exits: stop and wait for the
## process here, because nothing polls once the tree is gone.
func shutdown() -> bool:
	if _state == State.STOPPED:
		return true
	if _state == State.RUNNING and _platform.process_is_alive(_pid) and not _platform.kill_process(_pid):
		_last_error = "Could not stop the running process during shutdown."
		_status(_last_error, &"error")
		return false
	if _platform.process_is_alive(_pid) and not _platform.wait_for_exit(_pid, STOP_WAIT_MSEC):
		_last_error = "Could not stop the running process during shutdown."
		_status(_last_error, &"error")
		return false
	_pending_request = null
	_pending_retail_dir = ""
	_finish_stopped(false)
	return true


static func runtime_flags(request: Request) -> PackedStringArray:
	var flags := PackedStringArray()
	if request.loose_override:
		flags.append("/d")
	flags.append_array(PackedStringArray(["--resource-dir", request.resource_dir]))
	if request.loose_root:
		flags.append("--loose-root")
	if not request.mission_name.is_empty():
		flags.append("--loose-mission" if request.loose_mission else "--mission")
		flags.append(request.mission_name)
	if not request.expansion.strip_edges().is_empty():
		flags.append("/exp")
		flags.append(request.expansion.strip_edges())
	flags.append("/game")
	flags.append(_clean_game_code(request.game_code))
	return flags


static func launch_plan(
	editor_exe: String,
	project_dir: String,
	request: Request,
	file_exists: Callable
) -> LaunchPlan:
	if request.mode == Mode.RETAIL:
		if request.exe_path.is_empty() or not bool(file_exists.call(request.exe_path)):
			return null
		var flags := PackedStringArray(["/w"])
		if request.loose_override:
			flags.append("/d")
		flags.append("/FRISK")
		return LaunchPlan.make(request.exe_path, flags, request.exe_path.get_base_dir())

	var args := PackedStringArray(["--path", project_dir, RUNTIME_SCENE, "--"])
	args.append_array(runtime_flags(request))
	return LaunchPlan.make(editor_exe, args)


func _replace_with(request: Request, retail_dir: String = "") -> bool:
	if _state != State.STOPPED:
		if not stop():
			return false
		if _state == State.STOPPING:
			# The previous run is still exiting: spawn from poll() once it is gone.
			_pending_request = request
			_pending_retail_dir = retail_dir
			return true
	return _spawn_request(request, retail_dir)


func _spawn_pending() -> void:
	if _pending_request == null:
		return
	var request := _pending_request
	var retail_dir := _pending_retail_dir
	_pending_request = null
	_pending_retail_dir = ""
	_spawn_request(request, retail_dir)


func _spawn_request(request: Request, retail_dir: String) -> bool:
	if request.mode == Mode.RETAIL:
		_status("Staging game data for retail...")
		var staged: RetailStageResult = _platform.stage_retail(request.resource_dir, retail_dir)
		if not staged.ok:
			return _fail(staged.error if not staged.error.is_empty() else "Retail staging failed.")
		request.exe_path = staged.exe
		request.staged_dir = staged.packed_dir

	var plan := _current_plan(request)
	if plan == null:
		return _fail(
				"The staged retail executable is unavailable."
				if request.mode == Mode.RETAIL else
				"The editor runtime is unavailable.")
	var pid := _platform.spawn_process(plan.path, plan.args, plan.cwd)
	if pid <= 0:
		return _fail("Could not launch %s." % _mode_name(request.mode))

	_pid = pid
	_active_request = request
	_state = State.RUNNING
	_last_error = ""
	state_changed.emit(get_state())
	if request.mode == Mode.RETAIL:
		_status("Retail is running from %s." % request.staged_dir)
	else:
		_status("OpenNova is running from the selected game-data directory.")
	return true


func _current_plan(request: Request) -> LaunchPlan:
	return launch_plan(
			_platform.editor_executable_path(),
			_platform.project_dir(),
			request,
			func(path: String) -> bool: return _platform.file_exists(path))


func _finish_stopped(report_exit: bool) -> void:
	var old_pid := _pid
	_state = State.STOPPED
	_pid = -1
	_active_request = null
	if old_pid > 0:
		_platform.release_process(old_pid)
	state_changed.emit(get_state())
	if report_exit:
		_status("The managed process exited.")


func _fail(message: String, kind: StringName = &"error") -> bool:
	_last_error = message
	_status(message, kind)
	state_changed.emit(get_state())
	return false


func _status(message: String, kind: StringName = &"info") -> void:
	status_changed.emit(message, kind)


static func _clean_game_code(value: String) -> String:
	var clean := value.strip_edges().to_lower()
	return clean if not clean.is_empty() else "jo"


func _absolute_root(path: String) -> String:
	var clean := path.strip_edges()
	return ProjectSettings.globalize_path(clean).simplify_path() if not clean.is_empty() else ""


static func _mode_name(mode: int) -> String:
	return "retail" if mode == Mode.RETAIL else "opennova"


static func _state_name(state: int) -> String:
	match state:
		State.RUNNING:
			return "running"
		State.STOPPING:
			return "stopping"
		_:
			return "stopped"
