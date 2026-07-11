extends SceneTree

# Headless probe (NOT collected by GUT): times NovaResourceRoot.mount_runtime at
# a PFF install, step by step, to diagnose a hang/slow mount observed when the
# sound_pff_install_test mounted the JO:TR install with the revx02 expansion.
#
# Use: godot --headless --path godot -s res://tests/mount_timing_probe.gd -- <dir> [expansion]


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	var dir := args[0] if args.size() >= 1 else "C:/GAMES/JOTAC/Game/JO"
	var expansion := args[1] if args.size() >= 2 else ""
	print("[probe] mount_runtime dir=%s exp='%s'" % [dir, expansion])
	var t0 := Time.get_ticks_msec()
	var root := NovaResourceRoot.new()
	var err: int = root.mount_runtime(dir, expansion, false, "jo")
	var t1 := Time.get_ticks_msec()
	print("[probe] mount_runtime -> %d in %d ms" % [err, t1 - t0])
	if err == OK:
		for n in ["game.lwf", "gamelocl.LWF", "items.def", "menumus.bin", "gamemus.bin",
				"M" + expansion + ".bin", "G" + expansion + ".bin",
				expansion + "L.lwf", expansion + ".lwf", "00TRa.bms", "00TRg.bms"]:
			var t2 := Time.get_ticks_msec()
			var have: bool = root.has_file(n)
			var size: int = root.read_file(n).size() if have else 0
			print("[probe] %-16s has=%-5s size=%-9d (%d ms)" % [n, str(have), size, Time.get_ticks_msec() - t2])
	quit(0)
