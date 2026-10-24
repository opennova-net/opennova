extends GameProbe

## perf_round_end: the frame-span breakdown of a session's round ends. With the
## shell's and the world's frame spans on and the FrameStats board drained once
## per process frame (the perf_mission_rows pattern), it keeps the last
## `frames_before` frames and, at each round end, records them and the next
## `frames_after`: per frame the wall time between two process frames, every
## non-zero board slot (microseconds, or a count for the VALUE slots), and the
## two round-end edges: the world's match outcome (the server's round-end tick
## on a host) and the local client's S2C 0x1D header (EndRoundState); plus the
## HUD overlay's draw-list counts three frames after the edge. Waits through
## the rotation's mission loads for `rounds` round ends and writes one JSON
## record into the run's artifact dir (or `output`). Headless runs no canvas
## draw (the HUD overlay's emit and the draw legs read zero); a window has them.

var _ctx: ProbeContext
var _board: FrameStats = null
var _names := PackedStringArray()


func run(ctx: ProbeContext) -> ProbeVerdict:
	if ctx.sim() == null:
		return ProbeVerdict.failed("no loaded session world")
	_ctx = ctx
	var rounds := int(ctx.args.get("rounds", 3))
	var frames_before := int(ctx.args.get("frames_before", 20))
	var frames_after := int(ctx.args.get("frames_after", 90))
	_board = ctx.frame_stats()
	if _board == null:
		return ProbeVerdict.failed("the game shell has no FrameStats")
	for slot in range(FrameStats.SLOT_COUNT):
		_names.append(FrameStats.slot_name(slot).to_lower())
	ProbePerfSetup.enable_spans(ctx)
	var was_capturing := _board.is_capture_active()
	var board := _board
	ctx.defer_restore(func() -> void:
		if is_instance_valid(board):
			board.set_capture_active(was_capturing))
	_board.set_capture_active(true)
	await ctx.tree.process_frame
	_board.drain()
	ctx.log("capturing %d round end(s): %d frames before, %d after" % [rounds, frames_before, frames_after])

	var ring: Array = []
	var ends: Array = []
	var current: Dictionary = {}
	var after_left := 0
	var armed := false
	var last_us := Time.get_ticks_usec()
	var frame := 0
	while ends.size() < rounds and not ctx.cancelled:
		await ctx.tree.process_frame
		var now := Time.get_ticks_usec()
		var row := _row(frame, now - last_us, _board.drain())
		last_us = now
		frame += 1
		var round_over: bool = row["round_over"]
		var header: bool = row["header"]
		if not round_over and not header:
			armed = true
		if after_left > 0:
			(current["frames"] as Array).append(row)
			after_left -= 1
			if after_left == frames_after - 3:
				current["hud_draw_list"] = _hud_draw_list()
			if after_left == 0:
				_close(current)
				ends.append(current)
				ctx.log(_summary_line(current))
				ctx.progress({"round_ends": ends.size()})
			continue
		ring.append(row)
		if ring.size() > frames_before:
			ring.pop_front()
		if armed and (round_over or header):
			armed = false
			current = {
				"index": ends.size() + 1,
				"mission": _mission(),
				"edge_frame": frame - 1,
				"frames": ring.duplicate(),
			}
			ring.clear()
			after_left = frames_after
	if ctx.cancelled:
		return ProbeVerdict.failed("cancelled after %d round end(s)" % ends.size(), {"round_ends": ends})

	var result := {
		"schema": 1,
		"probe": "perf_round_end",
		"headless": DisplayServer.get_name() == "headless",
		"frames_before": frames_before,
		"frames_after": frames_after,
		"round_ends": ends,
	}
	var output := String(ctx.args.get("output", "")).strip_edges()
	var path := output if not output.is_empty() else ctx.artifact_dir.path_join("perf_round_end.json")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return ProbeVerdict.failed("failed to write %s (error %d)" % [path, FileAccess.get_open_error()])
	file.store_string(JSON.stringify(result, "\t"))
	file.close()
	ctx.artifact("perf_round_end", path, "json")
	return ProbeVerdict.passed("%d round end(s) recorded" % ends.size(), {"json": path})


# One drained frame: the probe's wall time since the previous process frame,
# the round-end edges and every non-zero slot. The drain at a process frame
# holds the previous frame's process-phase spans, its flush and its draw.
func _row(frame: int, wall_us: int, window: FrameStatsWindow) -> Dictionary:
	var sim := _ctx.sim()
	var state: EndRoundState = sim.get_end_round_state() if sim != null else null
	var slots := {}
	var sums := window.sums
	var counts := window.sample_frames
	for slot in range(sums.size()):
		if counts[slot] > 0 and sums[slot] != 0:
			slots[_names[slot]] = sums[slot]
	return {
		"frame": frame,
		"t_ms": Time.get_unix_time_from_system() * 1000.0,
		"wall_us": wall_us,
		"drained_frames": window.frames,
		"round_over": sim != null and sim.is_round_over(),
		"header": state != null and state.is_header_known(),
		"board": state != null and state.is_board_known(),
		"slots": slots,
	}


# The HUD overlay's draw-list counts three frames after the edge (one extra
# compile, in the next frame's process): the end-round box's fill tiles count
# as quads_stage2.
func _hud_draw_list() -> Dictionary:
	var hud_presenter := _ctx.hud_presenter()
	var hud: HudOverlay = hud_presenter.get_game_hud() if hud_presenter != null else null
	return hud.get_draw_list_stats().to_json_value() if hud != null else {}


func _mission() -> String:
	var world := _ctx.world()
	return world.get_loaded_mission_file() if world != null else ""


# The window's longest frame and where its time went (the edge-relative index).
func _close(end: Dictionary) -> void:
	var longest: Dictionary = {}
	for row: Dictionary in end["frames"]:
		if longest.is_empty() or int(row["wall_us"]) > int(longest["wall_us"]):
			longest = row
	end["longest_frame"] = int(longest.get("frame", -1)) - int(end["edge_frame"])
	end["longest_wall_us"] = int(longest.get("wall_us", 0))


func _summary_line(end: Dictionary) -> String:
	var longest: Dictionary = {}
	for row: Dictionary in end["frames"]:
		if int(row["frame"]) - int(end["edge_frame"]) == int(end["longest_frame"]):
			longest = row
	var top: Array = []
	var slots: Dictionary = longest.get("slots", {})
	for key: String in slots:
		if not _is_value_slot(key):
			top.append([int(slots[key]), key])
	top.sort_custom(func(a: Array, b: Array) -> bool: return a[0] > b[0])
	var parts := PackedStringArray()
	for pair in top.slice(0, 8):
		parts.append("%s=%.1f" % [pair[1], float(pair[0]) / 1000.0])
	return "round end %d (%s): longest frame %+d at %.1f ms: %s" % [
			int(end["index"]), String(end["mission"]), int(end["longest_frame"]),
			float(end["longest_wall_us"]) / 1000.0, ", ".join(parts)]


# The board's VALUE slots (counts, and Godot's once-a-second peaks) and the
# sampler's own wall row, kept in the JSON but not ranked as spans.
static func _is_value_slot(name: String) -> bool:
	for marker in ["frame_wall", "frame_time_process", "frame_physics_server",
			"frame_physics_iterations", "frame_nodes_", "_models", "_rows", "_objects",
			"_draws", "_captures", "_vertices", "_skinned", "_readbacks", "sim_ticks",
			"sim_role", "sim_entity_count", "net_peer_count", "trace_calls",
			"_survivors", "_faces"]:
		if name.contains(marker):
			return true
	return false
