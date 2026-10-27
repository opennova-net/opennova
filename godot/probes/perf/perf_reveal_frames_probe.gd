extends GameProbe

## perf_reveal_frames: the first frames past the reveal, one record each (D-NET-1377 part (ii); the
## perf cell's --reveal, scripts/mcp/perf_cell.py). It arms the shell's FrameStats board as it
## starts, so a probe started while the mission loads sees the whole reveal. Once the local player
## stands it leaves the single-player start-mission splash, as a player's key does (the shell's
## dismiss_start_mission_splash). It drains the board once per process frame, as perf_mission_rows
## does. The reveal frame is the first drain, past the loading presentation, whose window ran the
## session leg (WORLD_RUNTIME). That frame and the `frames` after it are recorded, and so is every
## frame the session ran under the presentation before it (negative indices): each one's wall, its
## drained frame count and every time slot at `min_slot_us` or more. A window's FRAME_WALL is the
## frame before it (the sampler adds it at the next frame's process), so a record's wall is the next
## window's. Beside them goes the world's post-load timeline (GameWorld.last_post_load_frames(): the
## first frames' world legs, timed from the load's end whether a probe was armed or not, so a host,
## whose world runs as its load ends, is timed there too): each frame's world leg and its legs at
## 1 ms or more. The record is JSON in the run's directory. An instrument that changes nothing; it
## needs no --verbose.

const TIMEOUT_MS := 240_000
const US_PER_MS := 1000.0


func run(ctx: ProbeContext) -> ProbeVerdict:
	var frames := int(ctx.args.get("frames", 8))
	var min_slot_us := int(ctx.args.get("min_slot_us", 500))
	var label := String(ctx.args.get("label", "reveal")).strip_edges()
	var board := ctx.frame_stats()
	if board == null:
		return ProbeVerdict.failed("the game shell has no FrameStats")
	var was_capturing := board.is_capture_active()
	ctx.defer_restore(func() -> void:
		if is_instance_valid(board):
			board.set_capture_active(was_capturing))
	board.set_capture_active(true)
	board.drain()
	var slots := _time_slots()
	# Every window from the first that ran the session: [window, loading at its drain].
	var windows: Array = []
	var reveal := -1
	var loading_frames := 0
	var dismissed := false
	var deadline := Time.get_ticks_msec() + TIMEOUT_MS
	# The reveal, the frames past it, and one window more for the last one's wall.
	while not ctx.cancelled and (reveal < 0 or windows.size() < reveal + frames + 2):
		if Time.get_ticks_msec() >= deadline:
			return ProbeVerdict.failed("no frame past the loading presentation ran the session",
					{"loading_frames": loading_frames})
		var shell := ctx.game()
		var live_sim := ctx.sim()
		if shell != null and shell.is_world_loading() and live_sim != null and live_sim.has_local_player():
			dismissed = shell.dismiss_start_mission_splash() or dismissed
		await ctx.tree.process_frame
		var window := board.drain()
		# The drained window is the frame that just ended: the reveal is the first one that ran the
		# session with the loading presentation gone (the load's forced frame runs it under the
		# presentation and is not the reveal).
		shell = ctx.game()
		var loading := shell == null or shell.is_world_loading()
		var ran := window.sums[FrameStats.WORLD_RUNTIME] > 0
		if windows.is_empty() and not ran:
			loading_frames += 1
			continue
		windows.append([window, loading])
		if reveal < 0 and ran and not loading:
			reveal = windows.size() - 1
	if ctx.cancelled:
		return ProbeVerdict.failed("cancelled")
	var records: Array = []
	for i in range(windows.size() - 1):
		var record := _record(i - reveal, windows[i][0], slots, min_slot_us)
		record["loading"] = bool(windows[i][1])
		record["wall_ms"] = (windows[i + 1][0] as FrameStatsWindow).sums[FrameStats.FRAME_WALL] / US_PER_MS
		records.append(record)
	for record: Dictionary in records:
		ctx.log("reveal%+d: wall %.1f ms over %d frame(s): %s" % [int(record["index"]),
				float(record["wall_ms"]), int(record["frames"]), _top(record["slots"], 8)])
	var world := ctx.world()
	var post_load: Array = []
	var timeline: LoadTimeline = world.last_post_load_frames() if world != null else null
	if timeline != null:
		post_load = _post_load_frames(timeline)
		for frame: Dictionary in post_load:
			ctx.log("post-load %s: world %.1f ms: %s" % [frame["name"], float(frame["world_ms"]),
					_top(frame["legs"], 6)])
	var result := {
		"schema_version": 1,
		"probe": "perf_reveal_frames",
		"captured_at_utc": Time.get_datetime_string_from_system(true, true),
		"label": label,
		"mission": world.get_loaded_mission_file() if world != null else "",
		"role": int(ctx.sim().session_role()) if ctx.sim() != null else -1,
		"dismissed_splash": dismissed,
		"loading_frames": loading_frames,
		"min_slot_us": min_slot_us,
		"units": "milliseconds",
		"frames": records,
		"post_load_frames": post_load,
	}
	var path := ctx.artifact_dir.path_join("reveal_frames_%s.json" % label.to_lower().validate_filename())
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return ProbeVerdict.failed("failed to write %s (error %d)" % [path, FileAccess.get_open_error()])
	file.store_string(JSON.stringify(result, "\t"))
	file.close()
	ctx.artifact("reveal_frames", path, "json")
	return ProbeVerdict.passed("the reveal and %d frame(s) past it" % frames,
			{"json": path, "frames": records.size()})


func _record(index: int, window: FrameStatsWindow, slots: PackedInt32Array, min_slot_us: int) -> Dictionary:
	var sums := window.sums
	var named := {}
	for slot in slots:
		if slot != FrameStats.FRAME_WALL and sums[slot] >= min_slot_us:
			named[FrameStats.slot_name(slot).to_lower()] = sums[slot] / US_PER_MS
	return {"index": index, "frames": window.frames, "slots": named}


# The post-load timeline's frames (its depth-0 spans) with their legs at 1 ms or more (ms).
static func _post_load_frames(timeline: LoadTimeline) -> Array:
	var frames: Array = []
	for span: LoadTimelineSpan in timeline.spans():
		if span.depth == 0:
			frames.append({"name": span.name, "world_ms": span.duration_us() / US_PER_MS, "legs": {}})
		elif span.depth == 1 and not frames.is_empty() and span.duration_us() >= 1000:
			frames[-1]["legs"][span.name] = span.duration_us() / US_PER_MS
	return frames


# The board's time slots (a VALUE slot is a count).
static func _time_slots() -> PackedInt32Array:
	var slots := PackedInt32Array()
	for slot in range(FrameStats.SLOT_COUNT):
		if not FrameStats.slot_description(slot).begins_with("VALUE"):
			slots.append(slot)
	return slots


static func _top(named: Dictionary, count: int) -> String:
	var names := named.keys()
	names.sort_custom(func(a: String, b: String) -> bool: return float(named[a]) > float(named[b]))
	var parts := PackedStringArray()
	for name: String in names.slice(0, count):
		parts.append("%s %.1f" % [name, float(named[name])])
	return ", ".join(parts)
