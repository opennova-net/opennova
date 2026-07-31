extends SceneTree

# Headless validation that main_game.tscn boots into the menu front-end (not
# straight into a world) and never blocks on the resource-dir dialog. Mirrors the
# real boot path: point the persisted resource dir at a throwaway dir holding a
# main.mnu, instantiate the runtime scene, and let its _ready -> _enter_menu run.
# Checks it landed on the main menu with the world unloaded, then restores the
# prior persisted dir. A clean result is written to
# user://nova_menu_shell_probe_result.txt (the stdout pipe mangles Godot's colored
# output on Windows).
#
# Use: godot --headless --path godot -s res://tests/nova_menu_shell_probe.gd

const MAIN_FIXTURE := "res://../fixtures/mnu/jo_main.mnu"
const SP_FIXTURE := "res://../fixtures/mnu/jo_loadout.mnu"
const RESULT_PATH := "user://nova_menu_shell_probe_result.txt"
const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var diag := {}
	var failures: Array[String] = []
	var prev := ResourceDirSettings.get_resource_dir()
	var dir := _build_resource_dir()
	ResourceDirSettings.set_resource_dir(dir)
	diag["temp_dir"] = dir
	# is_valid_root may reject temp/user-data paths in some environments; the
	# persisted dir then reads back empty. Skip cleanly rather than fail.
	diag["persisted_ok"] = (ResourceDirSettings.get_resource_dir() == dir)
	if not diag["persisted_ok"]:
		_write_result(diag, [], true)
		ResourceDirSettings.set_resource_dir(prev)
		_cleanup(dir)
		quit(0)
		return

	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		_write_result(diag, ["failed to load main_game.tscn"], false)
		ResourceDirSettings.set_resource_dir(prev)
		_cleanup(dir)
		quit(1)
		return

	var scene := packed.instantiate()
	root.add_child(scene)  # _ready -> _enter_menu(persisted temp dir)
	for _i in range(10):
		await process_frame

	var shell = scene.get_node_or_null("MenuLayer/MenuShell")
	var world = scene.get_node_or_null("World")
	diag["menu_file"] = shell.get_current_menu_file() if shell != null else "<none>"
	diag["menu_built"] = (shell != null and shell.get_menu() != null)
	diag["world_loaded"] = world.is_loaded() if world != null else null

	if shell == null:
		failures.append("MenuLayer/MenuShell missing from the runtime scene")
	elif String(diag["menu_file"]) != "main.mnu":
		failures.append("expected boot into main.mnu, got '%s'" % diag["menu_file"])
	elif not diag["menu_built"]:
		failures.append("menu node was not built")
	if world == null:
		failures.append("World node missing")
	elif world.is_loaded():
		failures.append("world is loaded at boot (should be in the menu)")

	scene.queue_free()
	await process_frame
	_write_result(diag, failures, false)
	ResourceDirSettings.set_resource_dir(prev)
	_cleanup(dir)
	quit(0 if failures.is_empty() else 1)


func _write_result(diag: Dictionary, failures: Array, skipped: bool) -> void:
	var f := FileAccess.open(RESULT_PATH, FileAccess.WRITE)
	if f == null:
		return
	f.store_line("skipped=%s" % str(skipped))
	for k in diag.keys():
		f.store_line("%s=%s" % [k, str(diag[k])])
	f.store_line("failures=%d" % failures.size())
	for x in failures:
		f.store_line("FAIL: %s" % x)
	f.store_line("RESULT=%s" % ("OK" if failures.is_empty() else "FAIL"))
	f.close()


func _build_resource_dir() -> String:
	var dir := OS.get_temp_dir().path_join("nova_menu_shell_probe_root")
	DirAccess.make_dir_recursive_absolute(dir)
	_copy(MAIN_FIXTURE, dir.path_join("main.mnu"))
	_copy(SP_FIXTURE, dir.path_join("sp.mnu"))
	return dir


func _copy(res_path: String, dst: String) -> void:
	var f := FileAccess.open(dst, FileAccess.WRITE)
	if f != null:
		f.store_buffer(FileAccess.get_file_as_bytes(res_path))
		f.close()


func _cleanup(dir: String) -> void:
	for f in ["main.mnu", "sp.mnu"]:
		DirAccess.remove_absolute(dir.path_join(f))
	DirAccess.remove_absolute(dir)
