extends SceneTree

# Headless probe (NOT collected by GUT): drives NovaMusicDirector with the real
# menumus script+bank, logs the triggered SBF-entry sequence, and dumps the
# main.mnu per-screen MUSICVARs. Used to (1) confirm the VM is paced (plays ~one
# track at a time, not ~60/sec), and (2) confirm the menu music state var index:
# menumus reads its selecting discriminator at var INDEX 2 (golden test), but the
# runtime wrote index 0. Pass `<var_index> <var_value>` to set a music var right
# after start() and watch the triggered sequence change section.
#
# Use: godot --headless --path godot -s res://tests/music_director_probe.gd -- <dir> [script.bin] [bank.sbf] [frames] [var_index] [var_value]

const RESULT_PATH := "user://music_director_probe_result.txt"

var _seq: Array[int] = []
var _sections := 0


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	var dir := args[0] if args.size() >= 1 else ""
	var script_name := args[1] if args.size() >= 2 else "menumus.bin"
	var bank_name := args[2] if args.size() >= 3 else "menumus.sbf"
	var frames := int(args[3]) if args.size() >= 4 else 120
	var var_index := int(args[4]) if args.size() >= 5 else -1
	var var_value := int(args[5]) if args.size() >= 6 else 0
	var out: Array[String] = ["dir=%s script=%s bank=%s frames=%d var=%d:%d" % [
		dir, script_name, bank_name, frames, var_index, var_value]]
	if dir.is_empty():
		_write(out + ["RESULT=SKIP (no dir)"]); quit(0); return

	var root := NovaResourceRoot.new()
	root.mount_runtime(dir, "", false, "jo")

	# Dump main.mnu per-screen MUSICVARs (the value the menu pushes into the VM).
	var mnu_bytes := root.read_file("main.mnu")
	if not mnu_bytes.is_empty():
		var doc := NovaMnuDocument.new()
		if doc.load_from_bytes(mnu_bytes) == OK:
			for sid in doc.get_screen_ids():
				out.append("screen[%d] '%s' music_var=%d" % [
					sid, doc.get_screen_name(sid), doc.get_screen_music_var(sid)])

	var sbytes := root.read_file(script_name)
	var script := NovaMusicScript.new()
	if not sbytes.is_empty():
		script.load_from_decrypted_bytes(sbytes, script_name)
	var bank := NovaSbfBank.new()
	var loose := root.get_root_dir().path_join(bank_name)
	if FileAccess.file_exists(loose):
		bank.load_from_path(loose)
	out.append("script_count=%d bank_entries=%d" % [script.get_script_count(), bank.get_entry_count()])
	if script.get_script_count() <= 0 or bank.get_entry_count() <= 0:
		_write(out + ["RESULT=SKIP (assets missing)"]); quit(0); return

	var director := NovaMusicDirector.new()
	director.auto_start = false
	get_root().add_child(director)
	director.set_bank(bank)
	director.load_mus_script(script)
	director.sound_triggered.connect(func(idx, _name, _wait): _seq.append(int(idx)))
	director.section_entered.connect(func(_name): _sections += 1)
	director.start()
	if var_index >= 0:
		# After start() (which reloads + zeroes globals) but before the first
		# _process tick, so the VM enters the selected section from the top.
		director.set_var(var_index, var_value)

	for _i in range(frames):
		await process_frame

	out.append("triggered_seq=%s" % str(_seq))
	out.append("plays=%d sections=%d vm_state=%d" % [_seq.size(), _sections, director.vm_state()])
	director.queue_free()
	await process_frame
	_write(out + ["RESULT=DONE"])
	quit(0)


func _write(lines: Array) -> void:
	var text := "\n".join(lines)
	print("==== music_director_probe ====\n", text, "\n==============================")
	var f := FileAccess.open(RESULT_PATH, FileAccess.WRITE)
	if f != null:
		f.store_string(text + "\n")
		f.close()
