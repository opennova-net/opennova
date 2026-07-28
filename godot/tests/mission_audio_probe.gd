extends SceneTree

# Headless probe (NOT collected by GUT): runs the runtime mission-audio setup
# (NovaMissionAudio) for one or more missions against a mounted install and
# prints the per-mission stats — how many "snd:" markers resolved to candidates and
# which banks loaded. Used to compare stock vs mod missions when in-game
# ambience is reported silent.
#
# Use: godot --headless --path godot -s res://tests/mission_audio_probe.gd -- <dir> <expansion> [mission.bms ...]
# With no missions listed, probes every .bms the mount's index lists (cap 12).

const NovaMissionAudioScript = preload("res://engine/world/nova_mission_audio.gd")


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	if args.is_empty():
		print("[probe] usage: -- <dir> <expansion> [mission.bms ...]")
		quit(1)
		return
	var dir := args[0]
	var expansion := args[1] if args.size() >= 2 else ""
	var missions: PackedStringArray = []
	for i in range(2, args.size()):
		missions.append(args[i])

	var root := NovaResourceRoot.new()
	var err: int = root.mount_runtime(dir, expansion, false, "jo")
	print("[probe] mount %s exp='%s' -> %d" % [dir, expansion, err])
	if err != OK:
		quit(1)
		return

	if missions.is_empty():
		for f in root.list_files(".bms"):
			missions.append(String(f).get_file())
			if missions.size() >= 12:
				break
	print("[probe] missions: %s" % str(missions))

	var item_db := NovaItemDatabase.new()
	var item_err: int = item_db.load_from_resource_root(root, "items.def")
	print("[probe] items.def load -> %d (items=%d)" % [item_err, item_db.get_count()])

	for m in missions:
		if not root.has_file(m):
			print("[probe] %-28s NOT FOUND in mount" % m)
			continue
		var mission := NovaMissionData.new()
		if mission.open_from_resource_root(root, m) != OK:
			print("[probe] %-28s PARSE FAILED" % m)
			continue
		var container := Node3D.new()
		get_root().add_child(container)
		var audio = NovaMissionAudioScript.new(root, item_db)
		var stats: Dictionary = audio.setup(mission, m, container)
		print("[probe] %-28s resolved %3d/%3d markers, %d banks, %d candidates, %d dialogs" % [
			m, int(stats.get("markers_resolved", 0)), int(stats.get("markers_total", 0)),
			int(stats.get("banks_loaded", 0)), int(stats.get("ambient_candidates", 0)),
			int(stats.get("dialogs", 0))])
		# Materialize only the current top-eight candidates, then verify the physical
		# pool's loop regions. A loop_end <= loop_begin wraps at sample 0 forever.
		audio.tick(Vector3.ZERO, 0.2)
		var loop_voices := 0
		var loop_empty := 0
		for p in container.find_children("*", "AudioStreamPlayer3D", true, false):
			var sw := (p as AudioStreamPlayer3D).stream as AudioStreamWAV
			if sw == null or sw.loop_mode == AudioStreamWAV.LOOP_DISABLED:
				continue
			loop_voices += 1
			if sw.loop_end <= sw.loop_begin:
				loop_empty += 1
		if loop_empty > 0:
			print("[probe] %-28s WARNING: %d/%d looping voices have an EMPTY loop region -> silent" % [
				m, loop_empty, loop_voices])
		audio.teardown()
		container.queue_free()
	quit(0)
