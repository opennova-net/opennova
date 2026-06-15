extends SceneTree

# Headless probe (NOT collected by GUT): drives NovaMusicDirector with the real
# menumus script+bank and counts how many times it (re)triggers a play over N
# frames. If the VM re-fires `play` every frame, the SBF stream restarts ~60x/sec
# -> the reported "low buzz". A correctly-paced director fires only a handful of
# plays (one per section/track), not one per frame. Results -> stdout and
# user://music_director_probe_result.txt.
#
# Use: godot --headless --path godot -s res://tests/music_director_probe.gd -- <dir> [script.bin] [bank.sbf] [frames]

const RESULT_PATH := "user://music_director_probe_result.txt"

var _plays := 0
var _sections := 0


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	var dir := args[0] if args.size() >= 1 else ""
	var script_name := args[1] if args.size() >= 2 else "menumus.bin"
	var bank_name := args[2] if args.size() >= 3 else "menumus.sbf"
	var frames := int(args[3]) if args.size() >= 4 else 120
	var out: Array[String] = ["dir=%s script=%s bank=%s frames=%d" % [dir, script_name, bank_name, frames]]
	if dir.is_empty():
		_write(out + ["RESULT=SKIP (no dir)"]); quit(0); return

	var root := NovaResourceRoot.new()
	root.mount_runtime(dir, "", false, "jo")
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
	director.sound_triggered.connect(func(_idx, _name, _wait): _plays += 1)
	director.section_entered.connect(func(_name): _sections += 1)
	director.start()

	for _i in range(frames):
		await process_frame

	out.append("sound_triggered=%d  section_entered=%d  over %d frames" % [_plays, _sections, frames])
	out.append("plays_per_frame=%.2f  vm_state=%d" % [float(_plays) / float(maxi(frames, 1)), director.vm_state()])
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
