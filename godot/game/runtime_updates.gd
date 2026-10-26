extends Node

## The process boundary to the updater. The native updater owns protocol,
## planning, installation and relaunch, and finds out itself whether an update
## is waiting; this node only hands the process over (the menu's routes:
## engine menu_commands.h MenuCommand::Update).

## The relaunch marker a first-run install passes to the game (boot_policy.h).
const AFTER_UPDATE_FLAG := "--after-update"

var last_error := ""
var _enabled := false
var _product := "acidgambit"
var _runtime_dir := ""
var _resource_dir := ""
var _run_args := PackedStringArray()


func configure(product: String, runtime_dir: String, resource_dir: String,
		run_args: PackedStringArray, enabled: bool) -> void:
	_product = product
	_runtime_dir = runtime_dir
	_resource_dir = resource_dir
	_run_args = run_args.duplicate()
	_enabled = enabled
	last_error = ""


func is_enabled() -> bool:
	return _enabled


func updater_path() -> String:
	return _runtime_dir.path_join("update.exe")


## `bootstrap`: a first-run install, whose relaunch carries AFTER_UPDATE_FLAG so a
## game that still finds no content stops instead of starting the updater again.
func install_arguments(parent_pid: int, bootstrap := false) -> PackedStringArray:
	var arguments := PackedStringArray(["/RUN", "--parent-pid", str(parent_pid)])
	arguments.append_array(_product_arguments())
	var relaunch := PackedStringArray()
	for argument in _run_args:
		if argument != AFTER_UPDATE_FLAG:
			relaunch.append(argument)
	if bootstrap:
		if not relaunch.has("--"):
			relaunch.append("--")
		relaunch.append(AFTER_UPDATE_FLAG)
	for argument in relaunch:
		arguments.append("--run-arg")
		arguments.append(argument)
	return arguments


func _product_arguments() -> PackedStringArray:
	return PackedStringArray(["/game", _product, "--runtime-dir", _runtime_dir,
			"--resource-dir", _resource_dir])


func launch_update(bootstrap := false) -> bool:
	last_error = ""
	if not _enabled:
		last_error = "Updates are disabled for this launch."
		return false
	if not FileAccess.file_exists(updater_path()):
		last_error = "update.exe is missing from the OpenNova installation."
		return false
	if OS.create_process(updater_path(), install_arguments(OS.get_process_id(), bootstrap), false) <= 0:
		last_error = "Could not start update.exe."
		return false
	return true
