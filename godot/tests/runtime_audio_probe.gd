extends SceneTree

# Headless audio probe (NOT collected by GUT). Verifies the music loaders resolve
# the menu/game music: the .bin MUS script through the VFS (PFF-archived), and the
# .sbf bank as a DIRECT loose file from the root dir (the original opens it via
# CreateFileA, not the PFF system) — issue 1. Results go to
# user://runtime_audio_probe_result.txt AND stdout.
#
# Use: godot --headless --path godot -s res://tests/runtime_audio_probe.gd -- <resource_dir>

const RESULT_PATH := "user://runtime_audio_probe_result.txt"


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	var dir := args[0] if args.size() >= 1 else ""
	var out: Array[String] = ["dir=%s" % dir]

	if dir.is_empty():
		_write(out + ["RESULT=SKIP (no dir)"])
		quit(0)
		return

	# Default runtime mount: packed (no /d loose-override), exactly as main_game mounts.
	var res_root := NovaResourceRoot.new()
	var rc := res_root.mount_runtime(dir, "", false, "jo")
	out.append("mount_rc=%d last_error=%s" % [rc, res_root.get_last_error()])
	var root_dir := res_root.get_root_dir()

	for pair in [["menumus.bin", "menumus.sbf"], ["gamemus.bin", "gamemus.sbf"]]:
		var sname: String = pair[0]
		var bname: String = pair[1]
		# Script: through the VFS (PFF-aware), like the original's File_LoadResource.
		var sbytes := res_root.read_file(sname)
		var script := NovaMusicScript.new()
		if not sbytes.is_empty():
			script.load_from_decrypted_bytes(sbytes, sname)
		# Bank: direct loose file from the root dir, like the original's CreateFileA.
		var bank := NovaSbfBank.new()
		var loose := root_dir.path_join(bname)
		var loose_exists := FileAccess.file_exists(loose)
		if loose_exists:
			bank.load_from_path(loose)
		out.append("%s: script_count=%d  |  %s: loose_exists=%s entries=%d" % [
			sname, script.get_script_count(), bname, str(loose_exists), bank.get_entry_count()])

	_write(out + ["RESULT=DONE"])
	quit(0)


func _write(lines: Array) -> void:
	var text := "\n".join(lines)
	print("==== runtime_audio_probe ====\n", text, "\n=============================")
	var f := FileAccess.open(RESULT_PATH, FileAccess.WRITE)
	if f != null:
		f.store_string(text + "\n")
		f.close()
