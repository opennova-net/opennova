class_name GameRunSession
extends RefCounted

## Owns ONED's one standalone child process. ONED can run OpenNova
## directly over a loose or packed game-data directory, or stage it for retail.
## Starting either target first stops the currently managed child.

signal status_changed(text: String, kind: StringName)
signal state_changed(state: Dictionary)

const RUNTIME_SCENE := "res://game/game_runtime_root.tscn"
const PACKAGED_RUNTIME_CANDIDATES: Array[String] = [
	"opennova.exe", "opennova.x86_64", "opennova",
	"../../../opennova.app/Contents/MacOS/opennova",
]
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

	static func opennova(root: String, game: String, exp: String) -> Request:
		var request := Request.new()
		request.mode = Mode.OPENNOVA
		request.resource_dir = root
		request.game_code = game
		request.expansion = exp
		return request

	static func retail(root: String) -> Request:
		var request := Request.new()
		request.mode = Mode.RETAIL
		request.resource_dir = root
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


func opennova_readiness(resource_dir: String, game_code: String = "jo",
		expansion: String = "") -> String:
	var root := _absolute_root(resource_dir)
	if root.is_empty() or not _valid_resource_dir(root):
		return "Select a valid resource directory first."
	var request := Request.opennova(root, game_code, expansion)
	if _current_plan(request) == null:
		return "No OpenNova runtime is available beside ONED."
	return ""


func retail_readiness(resource_dir: String, retail_dir: String) -> String:
	var root := _absolute_root(resource_dir)
	if root.is_empty() or not _valid_resource_dir(root):
		return "Select a valid resource directory first."
	if not _supports_working_directory():
		return "Retail launch requires working-directory process support (Windows only)."
	return _retail_install_error(retail_dir.strip_edges())


func run_opennova(resource_dir: String, game_code: String = "jo",
		expansion: String = "") -> bool:
	var reason := opennova_readiness(resource_dir, game_code, expansion)
	if not reason.is_empty():
		return _fail(reason, &"warn")
	var request := Request.opennova(
			_absolute_root(resource_dir), _clean_game_code(game_code), expansion.strip_edges())
	return _replace_with(request)


func run_retail(resource_dir: String, retail_dir: String) -> bool:
	var reason := retail_readiness(resource_dir, retail_dir)
	if not reason.is_empty():
		return _fail(reason, &"warn")
	return _replace_with(Request.retail(_absolute_root(resource_dir)), retail_dir.strip_edges())


## Ask the managed process to stop. Returns true when the stop is under way
## (poll() reports the exit, or the STOP_WAIT_MSEC deadline) and false when
## nothing was running or the kill failed. Never blocks the surface.
func stop() -> bool:
	if _state == State.STOPPED:
		return false
	if _state == State.STOPPING:
		return true
	if not _process_is_alive(_pid):
		_finish_stopped(false)
		return true
	if not _kill_process(_pid):
		return _fail("Could not stop the running process.")
	_state = State.STOPPING
	_stop_deadline_msec = _now_msec() + STOP_WAIT_MSEC
	state_changed.emit(get_state())
	_status("Stopping the managed process...")
	return true


func poll() -> void:
	match _state:
		State.RUNNING:
			if not _process_is_alive(_pid):
				_finish_stopped(true)
		State.STOPPING:
			if not _process_is_alive(_pid):
				_finish_stopped(false)
				_status("Stopped the managed process.")
				_spawn_pending()
			elif _now_msec() >= _stop_deadline_msec:
				_pending_request = null
				_pending_retail_dir = ""
				_state = State.RUNNING
				_fail("The running process did not exit after it was stopped.")


## The synchronous last resort for the app's exit: stop and wait for the
## process here, because nothing polls once the tree is gone.
func shutdown() -> bool:
	if _state == State.STOPPED:
		return true
	if _state == State.RUNNING and _process_is_alive(_pid) and not _kill_process(_pid):
		_last_error = "Could not stop the running process during ONED shutdown."
		_status(_last_error, &"error")
		return false
	if _process_is_alive(_pid) and not _wait_for_exit(_pid, STOP_WAIT_MSEC):
		_last_error = "Could not stop the running process during ONED shutdown."
		_status(_last_error, &"error")
		return false
	_pending_request = null
	_pending_retail_dir = ""
	_finish_stopped(false)
	return true


static func runtime_flags(request: Request) -> PackedStringArray:
	var flags := PackedStringArray([
		"/d",
		"--resource-dir", request.resource_dir,
		"--loose-root",
	])
	if not request.expansion.strip_edges().is_empty():
		flags.append("/exp")
		flags.append(request.expansion.strip_edges())
	flags.append("/game")
	flags.append(_clean_game_code(request.game_code))
	return flags


static func launch_plan(
	oned_exe: String,
	project_dir: String,
	dev_mode: bool,
	request: Request,
	file_exists: Callable
) -> LaunchPlan:
	if request.mode == Mode.RETAIL:
		if request.exe_path.is_empty() or not bool(file_exists.call(request.exe_path)):
			return null
		return LaunchPlan.make(
				request.exe_path,
				PackedStringArray(["/w", "/d", "/FRISK"]),
				request.exe_path.get_base_dir())

	var runtime_args := runtime_flags(request)
	var exe_dir := oned_exe.get_base_dir()
	for candidate in PACKAGED_RUNTIME_CANDIDATES:
		var path := exe_dir.path_join(candidate).simplify_path()
		if bool(file_exists.call(path)):
			var args := PackedStringArray(["--"])
			args.append_array(runtime_args)
			return LaunchPlan.make(path, args)
	if dev_mode:
		var args := PackedStringArray(["--path", project_dir, RUNTIME_SCENE, "--"])
		args.append_array(runtime_args)
		return LaunchPlan.make(oned_exe, args)
	return null


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
		var staged: Dictionary = _stage_retail(request.resource_dir, retail_dir)
		if not bool(staged.get("ok", false)):
			return _fail(String(staged.get("error", "Retail staging failed.")))
		request.exe_path = String(staged.get("exe", ""))
		request.staged_dir = String(staged.get("packed_dir", ""))

	var plan := _current_plan(request)
	if plan == null:
		return _fail(
				"The staged retail executable is unavailable."
				if request.mode == Mode.RETAIL else
				"No OpenNova runtime is available beside ONED.")
	var pid := _spawn_process(plan.path, plan.args, plan.cwd)
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
			_oned_executable_path(),
			_project_dir(),
			_is_dev_mode(),
			request,
			func(path: String) -> bool: return _file_exists(path))


func _finish_stopped(report_exit: bool) -> void:
	var old_pid := _pid
	_state = State.STOPPED
	_pid = -1
	_active_request = null
	if old_pid > 0:
		_release_process(old_pid)
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


# Internal seams used by the deterministic session tests. Production has one
# implementation: Godot/Process plus GamePacker.
func _valid_resource_dir(path: String) -> bool:
	return ResourceDirSettings.is_valid_root(path)


func _file_exists(path: String) -> bool:
	return FileAccess.file_exists(path)


func _supports_working_directory() -> bool:
	return Process.supports_working_directory()


func _now_msec() -> int:
	return Time.get_ticks_msec()


func _retail_install_error(retail_dir: String) -> String:
	return GamePacker.retail_install_error(retail_dir)


func _stage_retail(resource_dir: String, retail_dir: String) -> Dictionary:
	return GamePacker.stage_retail(resource_dir, retail_dir)


func _spawn_process(path: String, args: PackedStringArray, cwd: String) -> int:
	if Process.supports_working_directory():
		return Process.spawn_in_dir(path, args, cwd)
	return OS.create_process(path, args)


func _process_is_alive(pid: int) -> bool:
	return Process.is_running(pid) if Process.supports_working_directory() \
			else OS.is_process_running(pid)


func _kill_process(pid: int) -> bool:
	return Process.kill_pid(pid) if Process.supports_working_directory() \
			else OS.kill(pid) == OK


func _wait_for_exit(pid: int, timeout_msec: int) -> bool:
	if Process.supports_working_directory():
		return Process.wait_for_exit(pid, timeout_msec)
	# The portable fallback polls to the same deadline.
	var deadline := Time.get_ticks_msec() + timeout_msec
	while OS.is_process_running(pid):
		if Time.get_ticks_msec() >= deadline:
			return false
		OS.delay_msec(EXIT_POLL_MSEC)
	return true


func _release_process(pid: int) -> void:
	if Process.supports_working_directory():
		Process.release(pid)


func _oned_executable_path() -> String:
	return OS.get_executable_path()


func _project_dir() -> String:
	return ProjectSettings.globalize_path("res://")


func _is_dev_mode() -> bool:
	return OS.has_feature("editor")
