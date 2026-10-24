extends Node

## The process boundary to the updater. Checks never hold the game thread or
## change files. The native updater owns protocol, planning, installation and
## relaunch; this node only publishes availability and hands the process over.
signal availability_changed(available: bool)
signal check_finished(exit_code: int)

const CHECK_TIMEOUT_MS := 15000

var available := false
var last_error := ""
var _enabled := false
var _product := "opennova"
var _runtime_dir := ""
var _resource_dir := ""
var _run_args := PackedStringArray()
var _check_pid := -1
var _check_started := 0


func _ready() -> void:
	set_process(false)


func configure(product: String, runtime_dir: String, resource_dir: String,
		run_args: PackedStringArray, enabled: bool) -> void:
	_cancel_check()
	_product = product
	_runtime_dir = runtime_dir
	_resource_dir = resource_dir
	_run_args = run_args.duplicate()
	_enabled = enabled
	last_error = ""
	_set_available(false)


func is_enabled() -> bool:
	return _enabled


func updater_path() -> String:
	return _runtime_dir.path_join("update.exe")


func check_arguments() -> PackedStringArray:
	var arguments := PackedStringArray(["--check"])
	arguments.append_array(_product_arguments())
	return arguments


func install_arguments(parent_pid: int) -> PackedStringArray:
	var arguments := PackedStringArray(["/RUN", "--parent-pid", str(parent_pid)])
	arguments.append_array(_product_arguments())
	for argument in _run_args:
		arguments.append("--run-arg")
		arguments.append(argument)
	return arguments


func _product_arguments() -> PackedStringArray:
	return PackedStringArray(["/game", _product, "--runtime-dir", _runtime_dir,
			"--resource-dir", _resource_dir])


func begin_check() -> void:
	if not _enabled or _check_pid > 0:
		return
	last_error = ""
	if not FileAccess.file_exists(updater_path()):
		last_error = "update.exe is missing from the OpenNova installation."
		complete_check(1)
		return
	_check_pid = OS.create_process(updater_path(), check_arguments(), false)
	if _check_pid <= 0:
		last_error = "Could not start update.exe."
		complete_check(1)
		return
	_check_started = Time.get_ticks_msec()
	set_process(true)


func _process(_delta: float) -> void:
	if _check_pid <= 0:
		return
	if not OS.is_process_running(_check_pid):
		var result := OS.get_process_exit_code(_check_pid)
		_check_pid = -1
		complete_check(result)
	elif Time.get_ticks_msec() - _check_started >= CHECK_TIMEOUT_MS:
		_cancel_check()
		last_error = "The update check timed out."
		complete_check(1)


## Native check protocol: current=0, available=10, all other results unavailable.
## A failed/offline check never blocks an installed game's menus or play.
func complete_check(exit_code: int) -> void:
	set_process(false)
	_set_available(_enabled and exit_code == 10)
	if exit_code != 0 and exit_code != 10 and last_error.is_empty():
		last_error = "Could not check for updates. You can keep playing offline."
	check_finished.emit(exit_code)


func launch_update() -> bool:
	if not _enabled:
		last_error = "Updates are disabled for this launch."
		return false
	_cancel_check()
	if not FileAccess.file_exists(updater_path()):
		last_error = "update.exe is missing from the OpenNova installation."
		return false
	if OS.create_process(updater_path(), install_arguments(OS.get_process_id()), false) <= 0:
		last_error = "Could not start update.exe."
		return false
	return true


func _set_available(value: bool) -> void:
	if available != value:
		available = value
		availability_changed.emit(available)


func _cancel_check() -> void:
	if _check_pid > 0 and OS.is_process_running(_check_pid):
		OS.kill(_check_pid)
	_check_pid = -1
	set_process(false)


func _exit_tree() -> void:
	_cancel_check()
