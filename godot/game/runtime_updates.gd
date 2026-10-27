extends Node

## The process boundary to the updater. The native updater owns protocol,
## planning, installation and relaunch, and finds out itself whether an update
## is waiting; this node only hands the process over (the menu's routes:
## engine menu_commands.h MenuCommand::Update).

## The relaunch marker a first-run install passes to the game (boot_policy.h).
const AFTER_UPDATE_FLAG := "--after-update"
## The NovaWorld server a mod's install was for, which the relaunched game joins
## once signed in (boot_policy.h, ADR 0074).
const NOVAWORLD_JOIN_FLAG := "--novaworld-join"

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
## `join_rid`: the NovaWorld server the relaunch joins (NOVAWORLD_JOIN_FLAG). An
## earlier launch's markers are never carried into a later relaunch.
func install_arguments(parent_pid: int, bootstrap := false, join_rid := 0) -> PackedStringArray:
	var arguments := PackedStringArray(["/RUN", "--parent-pid", str(parent_pid)])
	arguments.append_array(_product_arguments())
	var relaunch := PackedStringArray()
	var skip_value := false
	for argument in _run_args:
		if skip_value:
			skip_value = false
		elif argument == NOVAWORLD_JOIN_FLAG:
			skip_value = true
		elif argument != AFTER_UPDATE_FLAG:
			relaunch.append(argument)
	var marks := PackedStringArray()
	if bootstrap:
		marks.append(AFTER_UPDATE_FLAG)
	if join_rid > 0:
		marks.append_array([NOVAWORLD_JOIN_FLAG, str(join_rid)])
	if not marks.is_empty():
		if not relaunch.has("--"):
			relaunch.append("--")
		relaunch.append_array(marks)
	for argument in relaunch:
		arguments.append("--run-arg")
		arguments.append(argument)
	return arguments


func _product_arguments() -> PackedStringArray:
	return PackedStringArray(["/game", _product, "--runtime-dir", _runtime_dir,
			"--resource-dir", _resource_dir])


func launch_update(bootstrap := false, join_rid := 0) -> bool:
	last_error = _launch_refusal()
	if not last_error.is_empty():
		return false
	if OS.create_process(updater_path(),
			install_arguments(OS.get_process_id(), bootstrap, join_rid), false) <= 0:
		last_error = "Could not start update.exe."
		return false
	return true


## Why the updater cannot start for this launch, or "" when it can.
func _launch_refusal() -> String:
	if not _enabled:
		return "Updates are disabled for this launch."
	if not FileAccess.file_exists(updater_path()):
		return "update.exe is missing from the OpenNova installation."
	return ""


## A mod's folder in this game (ADR 0074): expansion/<code>/, which holds the
## mod once its update ran.
func mod_dir(code: String) -> String:
	return _resource_dir.path_join("expansion").path_join(code)


func has_mod(code: String) -> bool:
	return not code.is_empty() and DirAccess.dir_exists_absolute(mod_dir(code))


## A mod's install, as the NovaWorld screen runs it (ADR 0074): its folder's
## UPDATE.WIZ naming its product (the updater takes every expansion folder with a
## wizard as a product of its own), then the updater as UPDATE starts it, the game
## ending for it. `join_rid`: the NovaWorld server the relaunch joins. Nothing is
## written when the updater cannot start.
func install_mod(mod: ModListing, join_rid := 0) -> bool:
	last_error = _launch_refusal()
	if not last_error.is_empty():
		return false
	last_error = mod.write_wizard(_resource_dir, _runtime_dir)
	if not last_error.is_empty():
		return false
	return launch_update(false, join_rid)


## A mod's removal: its folder deleted with everything in it.
func remove_mod(code: String) -> bool:
	last_error = ModListing.remove_folder(_resource_dir, code)
	return last_error.is_empty()
