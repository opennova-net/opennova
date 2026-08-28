extends GameProbe

## mission_audio: the runtime mission-audio setup (MissionAudio) for one or
## more missions on the launch's mounted root. Per mission: how many "snd:"
## markers resolved to candidates, which banks loaded, how many dialogs the
## .DBF carries; then the current top-eight candidates are materialized and
## the physical pool's loop regions checked (a loop_end <= loop_begin wraps
## at sample 0 forever: a silent voice). With `missions` empty, every .bms
## the mount's index lists (cap 12). Used to compare stock vs mod missions
## when in-game ambience is reported silent.

const MISSION_CAP := 12

var _ctx: ProbeContext


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	var root := ctx.resource_root()
	if root == null:
		return ProbeVerdict.failed("the shell has no mounted resource root")
	var missions: PackedStringArray = []
	for m in (ctx.args.get("missions", []) as Array):
		missions.append(String(m))
	if missions.is_empty():
		for f in root.list_files(".bms"):
			missions.append(String(f).get_file())
			if missions.size() >= MISSION_CAP:
				break
	ctx.log("missions: %s" % str(missions))
	var item_db := ItemDatabase.new()
	var item_err: int = item_db.load_from_resource_root(root, "items.def")
	ctx.log("items.def load -> %d (items=%d)" % [item_err, item_db.get_count()])

	var parent := Node3D.new()
	parent.name = "MissionAudioProbe"
	ctx.tree.root.add_child(parent)
	ctx.defer_restore(func() -> void:
		if is_instance_valid(parent):
			parent.queue_free())

	var failures: PackedStringArray = []
	var reports: Array = []
	for m in missions:
		if ctx.cancelled:
			break
		if not root.has_file(m):
			failures.append("%s not found in the mount" % m)
			ctx.log("%-28s NOT FOUND in mount" % m)
			continue
		var mission := MissionData.new()
		if mission.open_from_resource_root(root, m) != OK:
			failures.append("%s did not parse" % m)
			ctx.log("%-28s PARSE FAILED" % m)
			continue
		var container := Node3D.new()
		parent.add_child(container)
		var audio := MissionAudio.new(root, item_db)
		var stats := audio.setup(mission, m, container)
		ctx.log("%-28s resolved %3d/%3d markers, %d banks, %d candidates, %d dialogs" % [
				m, int(stats.markers_resolved), int(stats.markers_total),
				int(stats.banks_loaded), int(stats.ambient_candidates),
				int(stats.dialogs)])
		# Materialize only the current top-eight candidates, then verify the
		# physical pool's loop regions.
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
			failures.append("%s: %d/%d looping voices have an empty loop region" % [m, loop_empty, loop_voices])
			ctx.log("%-28s WARNING: %d/%d looping voices have an EMPTY loop region -> silent" % [
					m, loop_empty, loop_voices])
		audio.teardown()
		container.queue_free()
		reports.append({
			"mission": m,
			"stats": stats.to_dict(),
			"loop_voices": loop_voices,
			"loop_empty": loop_empty,
		})
		await ctx.tree.process_frame
	var data := {"missions": reports, "failures": Array(failures)}
	if ctx.cancelled:
		return ProbeVerdict.failed("cancelled", data)
	if not failures.is_empty():
		return ProbeVerdict.failed("%d problem(s): %s" % [failures.size(), "; ".join(failures)], data)
	return ProbeVerdict.passed("%d mission(s) set up, every loop region non-empty" % reports.size(), data)
