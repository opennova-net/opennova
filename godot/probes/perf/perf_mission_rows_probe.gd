extends GameProbe

## perf_mission_rows: the exact Mission Rows benchmark. Enables the shell's
## FrameStats directly (no dev-tools UI cost) and drains the board once per
## process frame, so every sample is the exact span that feeds the Stats
## window's PRESENT_MISSION row; the same drains retain whole-Present, World,
## wall-frame and outside-shell numbers so an optimization cannot merely move
## work beyond the present span. Writes the JSON record (schema 1) into the
## run's artifact dir (or `output`). `show_overlay` opens the dev tools with
## their Stats window for the whole run (the in-game reading condition) and
## feeds the window the probe's own drains at the window's cadence, so the
## ImGui layout cost lands in the frame exactly like in-game. Needs a window:
## rendering must stay live.

# The Stats window reads a ~0.5 s window in-game (StatsWindow::kRefreshSeconds).
const OVERLAY_RENDER_FRAMES := 30
const ResourceDirSettings := preload("res://game/resource_index/resource_dir_settings.gd")

const PRESENT_SLOTS := [
	FrameStats.PRESENT_SNAPSHOT,
	FrameStats.PRESENT_MISSION,
	FrameStats.PRESENT_WIRE,
	FrameStats.PRESENT_FIRE,
	FrameStats.PRESENT_DESTRUCTION,
	FrameStats.PRESENT_THROWABLE,
	FrameStats.PRESENT_SCARS,
]
const SHELL_LEG_SLOTS := [
	FrameStats.FRAME_PLAYER_BEFORE,
	FrameStats.FRAME_WORLD,
	FrameStats.FRAME_PLAYER_AFTER,
	FrameStats.FRAME_HUD,
]
# The engine-frame decomposition (RootFramePhaseSampler's draw-signal split
# plus the per-viewport render samplers): one sample per drained frame each,
# so a slice that moves work between the callbacks and the residual is seen.
const ENGINE_SLOT_SAMPLES := {
	"process_callbacks": FrameStats.FRAME_PROCESS_CALLBACKS,
	"deferred_flush": FrameStats.FRAME_DEFERRED_FLUSH,
	"flush_queued": FrameStats.FRAME_FLUSH_QUEUED,
	"flush_tail": FrameStats.FRAME_FLUSH_TAIL,
	"draw": FrameStats.FRAME_DRAW,
	"pacing_input": FrameStats.FRAME_PACING_INPUT,
	"hud_draw_compile": FrameStats.HUD_DRAW_COMPILE,
	"hud_draw_emit": FrameStats.HUD_DRAW_EMIT,
	"render_root_cpu": FrameStats.RENDER_ROOT_CPU,
	"render_root_gpu": FrameStats.RENDER_ROOT_GPU,
	"render_water_cpu": FrameStats.RENDER_WATER_CPU,
	"render_water_gpu": FrameStats.RENDER_WATER_GPU,
	"render_q3_gpu": FrameStats.RENDER_Q3_GPU,
	"render_slot_gpu": FrameStats.RENDER_SLOT_GPU,
}
# Every FrameStats slot under the World tick (GameWorld.tick legs, the
# awake-model walk, the occlusion frame, the sim step tree, traces, effects,
# and the present rows) is sampled by prefix so one JSON carries the whole
# world breakdown; the VALUE slots among them are counts, not spans.
const WORLD_SLOT_PREFIXES := ["WORLD_", "MODEL_", "OCCL_", "SIM_", "TRACE_",
		"EFFECTS_", "PRESENT_"]
const WORLD_VALUE_SLOTS := [
	"MODEL_AWAKE_MODELS",
	"MODEL_RENDERABLE_MODELS",
	"SIM_TICKS",
	"TRACE_CALLS",
	"TRACE_STATIC_SURVIVORS",
	"TRACE_DYNAMIC_SURVIVORS",
	"TRACE_PERSON_SURVIVORS",
	"TRACE_STATIC_FACES",
	"TRACE_DYNAMIC_FACES",
	"PRESENT_MISSION_ROWS",
	"PRESENT_MISSION_SUBMITTED_ROWS",
	"PRESENT_MISSION_BODY_ROWS",
]
# Per-pass submission counts (objects), averaged per drained frame.
const ENGINE_COUNT_SAMPLES := {
	"nodes_freed": FrameStats.FRAME_NODES_FREED,
	"nodes_added": FrameStats.FRAME_NODES_ADDED,
	"render_main_objects": FrameStats.RENDER_MAIN_OBJECTS,
	"render_main_draws": FrameStats.RENDER_MAIN_DRAWS,
	"render_water_objects": FrameStats.RENDER_WATER_OBJECTS,
	"render_water_draws": FrameStats.RENDER_WATER_DRAWS,
	"render_q3_objects": FrameStats.RENDER_Q3_OBJECTS,
	"render_q3_draws": FrameStats.RENDER_Q3_DRAWS,
	"render_slot_objects": FrameStats.RENDER_SLOT_OBJECTS,
	"render_slot_draws": FrameStats.RENDER_SLOT_DRAWS,
	"render_slot_captures": FrameStats.RENDER_SLOT_CAPTURES,
	"render_slot_packed_vertices": FrameStats.RENDER_SLOT_PACKED_VERTICES,
	"render_slot_skinned": FrameStats.RENDER_SLOT_SKINNED,
}
const COUNTER_KEYS := [
	"plan_rebuilds",
	"transform_builds",
	"aim_dispatches",
	"rhc_dispatches",
	"part_dispatches",
	"control_dispatches",
	"body_dispatches",
	"moved",
	"posed",
	"hidden",
]

var _ctx: ProbeContext
var _board: FrameStats = null
# Overlay mode: the dev tools whose Stats window the probe feeds from its own
# drains (null = the tools stay closed).
var _dev_tools: DevTools = null
var _overlay_sums := PackedInt64Array()
var _overlay_peaks := PackedInt64Array()
var _overlay_counts := PackedInt32Array()
var _overlay_frames := 0
var _overlay_drains := 0
# ENGINE_SLOT_SAMPLES + every prefixed world span slot (key = lower-cased slot
# name), and ENGINE_COUNT_SAMPLES + the world VALUE slots.
var _slot_samples: Dictionary = {}
var _count_samples: Dictionary = {}


func run(ctx: ProbeContext) -> ProbeVerdict:
	# The launch lands under the start-mission splash with the world held
	# un-ticked; leave it and wait for the local player before measuring.
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	_ctx = ctx
	_index_slots()
	var warmup_seconds := maxf(float(ctx.args.get("warmup_seconds", 6.0)), 0.0)
	var window_seconds := maxf(float(ctx.args.get("window_seconds", 10.0)), 0.1)
	var window_count := maxi(int(ctx.args.get("windows", 5)), 1)
	var label := String(ctx.args.get("label", "unlabeled")).strip_edges()
	if label.is_empty():
		label = "unlabeled"
	var show_overlay := bool(ctx.args.get("show_overlay", false))
	var output := String(ctx.args.get("output", "")).strip_edges()

	var world := ctx.world()
	var runtime := ctx.runtime()
	if world == null or runtime == null:
		return ProbeVerdict.failed("no loaded world/runtime")
	var bms := world.get_loaded_mission_file()
	_board = ctx.frame_stats()
	if _board == null:
		return ProbeVerdict.failed("the game shell has no FrameStats")
	if show_overlay:
		# The reading condition: the dev tools open with their Stats window. The
		# probe's own drains starve the window's refresh, so it is fed below.
		var dev_tools := ctx.dev_tools()
		if dev_tools == null or not dev_tools.is_available():
			return ProbeVerdict.failed("the dev tools are unavailable (no window, or the imgui-godot addon is missing)")
		var was_open := dev_tools.is_open()
		ctx.defer_restore(func() -> void:
			if is_instance_valid(dev_tools):
				dev_tools.set_open(was_open))
		dev_tools.set_open(true)
		_dev_tools = dev_tools
		_reset_overlay_window()
		ctx.log("dev tools open with the Stats window for the whole run")

	var was_capturing := _board.is_capture_active()
	var board := _board
	ctx.defer_restore(func() -> void:
		if is_instance_valid(board):
			board.set_capture_active(was_capturing))
	_board.set_capture_active(true)
	ctx.log("mission=%s; warming %.2f s with exact capture active" % [bms, warmup_seconds])
	await _drain_for_seconds(warmup_seconds)
	if ctx.cancelled:
		return ProbeVerdict.failed("cancelled during warm-up")
	# Start every measured window from an empty board even when warmup is zero.
	_board.drain()

	var fingerprint := _build_fingerprint(world, runtime, bms)
	ctx.log("models=%d hidden=%d entities=%d bms_bytes=%d topology=%s" % [
			int(fingerprint.get("model_count", 0)),
			int(fingerprint.get("hidden_model_count", 0)),
			int(fingerprint.get("entity_count", 0)),
			int(fingerprint.get("bms_bytes", 0)),
			String(fingerprint.get("topology_sha256", ""))])
	# Reading/hash-building the topology is intentionally outside the timed
	# windows. Flush its wall-time tail before the first sample.
	await _drain_frames(2)

	var aggregate_samples := _empty_samples()
	var aggregate_counter_delta := _empty_counter_delta()
	var windows: Array = []
	for i in range(window_count):
		if ctx.cancelled:
			return ProbeVerdict.failed("cancelled during window %d" % (i + 1))
		var counter_start: MissionPresentStats = runtime.get_mission_present_stats()
		var measured: Dictionary = await _measure_window(window_seconds)
		var counter_end: MissionPresentStats = runtime.get_mission_present_stats()
		var counter_delta := _counter_delta(counter_start, counter_end)
		var samples: Dictionary = measured["samples"]
		_append_samples(aggregate_samples, samples)
		_add_counter_delta(aggregate_counter_delta, counter_delta)
		var summaries := _summaries(samples)
		var sample_frames := (samples["mission_rows_us"] as Array).size()
		windows.append({
			"index": i + 1,
			"target_seconds": window_seconds,
			"elapsed_seconds": measured["elapsed_seconds"],
			"drains": measured["drains"],
			"coalesced_drains": measured["coalesced_drains"],
			"missing_mission_drains": measured["missing_mission_drains"],
			"metrics": _metrics_with_samples(samples, summaries),
			"presenter_counter_delta": counter_delta,
			"presenter_counter_per_mission_frame": _counter_per_frame(counter_delta, sample_frames),
		})
		_log_window(i + 1, window_count, sample_frames, summaries)
		ctx.progress({"windows_done": i + 1, "windows": window_count})
		# Formatting is diagnostic work, not game work. Keep its wall tail out
		# of the next window without counting these frames' presenter calls in
		# any counter delta.
		if i + 1 < window_count:
			await _drain_frames(2)

	if (aggregate_samples["mission_rows_us"] as Array).is_empty():
		return ProbeVerdict.failed("the capture produced no PRESENT_MISSION samples")

	var aggregate_summaries := _summaries(aggregate_samples)
	var aggregate_frames := (aggregate_samples["mission_rows_us"] as Array).size()
	var result := {
		"schema_version": 1,
		"probe": "perf_mission_rows",
		"captured_at_utc": Time.get_datetime_string_from_system(true, true),
		"label": label,
		"mission": {
			"file": bms,
			"resource_dir": ResourceDirSettings.get_resource_dir(),
			"expansion": ResourceDirSettings.get_expansion(),
		},
		"configuration": {
			"warmup_seconds": warmup_seconds,
			"window_seconds": window_seconds,
			"window_count": window_count,
			"overlay_visible": show_overlay,
			"capture": "FrameStats drained once per process frame",
			"units": "raw samples are integer microseconds; summaries are milliseconds",
		},
		"environment": {
			"godot": Engine.get_version_info(),
			"os": OS.get_name(),
			"processor": OS.get_processor_name(),
			"video_adapter": RenderingServer.get_video_adapter_name(),
		},
		"fingerprint": fingerprint,
		"windows": windows,
		"aggregate": {
			"mission_frames": aggregate_frames,
			"metrics": aggregate_summaries,
			"presenter_counter_delta": aggregate_counter_delta,
			"presenter_counter_per_mission_frame": _counter_per_frame(aggregate_counter_delta, aggregate_frames),
		},
	}

	var output_path := _output_path(ctx, output, bms, label)
	var write_error := _write_json(output_path, result)
	if write_error != OK:
		return ProbeVerdict.failed("failed to write %s (error %d)" % [output_path, write_error])
	ctx.artifact("mission_rows", output_path, "json")
	var mission_summary: Dictionary = aggregate_summaries["mission_rows"]
	ctx.log("aggregate frames=%d mission mean=%.3f ms p95=%.3f ms p99=%.3f ms max=%.3f ms" % [
			aggregate_frames,
			float(mission_summary["mean_ms"]),
			float(mission_summary["p95_ms"]),
			float(mission_summary["p99_ms"]),
			float(mission_summary["max_ms"])])
	ctx.log("JSON %s" % output_path)
	return ProbeVerdict.passed("mission rows mean %.3f ms over %d frames" % [
			float(mission_summary["mean_ms"]), aggregate_frames], {
		"json": output_path,
		"mission_frames": aggregate_frames,
		"aggregate": aggregate_summaries,
		"fingerprint": fingerprint,
	})


func _index_slots() -> void:
	_slot_samples = ENGINE_SLOT_SAMPLES.duplicate()
	_count_samples = ENGINE_COUNT_SAMPLES.duplicate()
	for slot in range(FrameStats.SLOT_COUNT):
		var slot_name := FrameStats.slot_name(slot)
		var prefixed := false
		for prefix in WORLD_SLOT_PREFIXES:
			if slot_name.begins_with(prefix):
				prefixed = true
				break
		if not prefixed:
			continue
		if WORLD_VALUE_SLOTS.has(slot_name):
			_count_samples[slot_name.to_lower()] = slot
		else:
			_slot_samples[slot_name.to_lower()] = slot


func _reset_overlay_window() -> void:
	_overlay_sums.resize(FrameStats.SLOT_COUNT)
	_overlay_sums.fill(0)
	_overlay_peaks.resize(FrameStats.SLOT_COUNT)
	_overlay_peaks.fill(0)
	_overlay_counts.resize(FrameStats.SLOT_COUNT)
	_overlay_counts.fill(0)
	_overlay_frames = 0
	_overlay_drains = 0


# Overlay mode: fold every drain into the window's reading and hand it over at
# the window's cadence, so the ImGui layout cost lands in the frame as in-game.
func _feed_overlay(captured: FrameStatsWindow) -> void:
	if _dev_tools == null:
		return
	for slot in range(FrameStats.SLOT_COUNT):
		_overlay_sums[slot] += captured.sums[slot]
		_overlay_peaks[slot] = maxi(_overlay_peaks[slot], captured.peaks[slot])
		_overlay_counts[slot] += captured.sample_frames[slot]
	_overlay_frames += captured.frames
	_overlay_drains += 1
	if _overlay_drains < OVERLAY_RENDER_FRAMES or _overlay_frames <= 0:
		return
	_dev_tools.feed_stats_window(_overlay_frames, _overlay_sums, _overlay_peaks, _overlay_counts)
	_reset_overlay_window()


func _drain_for_seconds(seconds: float) -> void:
	var deadline := Time.get_ticks_usec() + int(seconds * 1_000_000.0)
	while Time.get_ticks_usec() < deadline and not _ctx.cancelled:
		await _ctx.tree.process_frame
		_feed_overlay(_board.drain())


func _drain_frames(count: int) -> void:
	for _i in range(count):
		if _ctx.cancelled:
			return
		await _ctx.tree.process_frame
		_feed_overlay(_board.drain())


func _measure_window(seconds: float) -> Dictionary:
	var samples := _empty_samples()
	var start := Time.get_ticks_usec()
	var deadline := start + int(seconds * 1_000_000.0)
	var drains := 0
	var coalesced_drains := 0
	var missing_mission_drains := 0
	while Time.get_ticks_usec() < deadline and not _ctx.cancelled:
		await _ctx.tree.process_frame
		var captured := _board.drain()
		_feed_overlay(captured)
		drains += 1
		if captured.frames > 1:
			coalesced_drains += 1
			# A summed multi-frame window cannot provide an exact percentile
			# sample. Preserve the diagnostic count and leave it out.
			continue
		if captured.sample_frames[FrameStats.PRESENT_MISSION] <= 0:
			missing_mission_drains += 1
			continue
		(samples["mission_rows_us"] as Array).append(int(captured.sums[FrameStats.PRESENT_MISSION]))
		(samples["present_us"] as Array).append(_sum_slots(captured.sums, PRESENT_SLOTS))
		if captured.sample_frames[FrameStats.FRAME_WORLD] > 0:
			(samples["world_us"] as Array).append(int(captured.sums[FrameStats.FRAME_WORLD]))
		if captured.sample_frames[FrameStats.FRAME_WALL] > 0:
			var wall_us := int(captured.sums[FrameStats.FRAME_WALL])
			(samples["frame_us"] as Array).append(wall_us)
			(samples["outside_shell_us"] as Array).append(maxi(
					wall_us - _sum_slots(captured.sums, SHELL_LEG_SLOTS), 0))
		for key in _slot_samples:
			var slot := int(_slot_samples[key])
			if captured.sample_frames[slot] > 0:
				(samples[key + "_us"] as Array).append(int(captured.sums[slot]))
		for key in _count_samples:
			var slot := int(_count_samples[key])
			if captured.sample_frames[slot] > 0:
				(samples[key] as Array).append(int(captured.sums[slot]))
	return {
		"elapsed_seconds": float(Time.get_ticks_usec() - start) / 1_000_000.0,
		"drains": drains,
		"coalesced_drains": coalesced_drains,
		"missing_mission_drains": missing_mission_drains,
		"samples": samples,
	}


func _empty_samples() -> Dictionary:
	var samples := {
		"mission_rows_us": [],
		"present_us": [],
		"world_us": [],
		"outside_shell_us": [],
		"frame_us": [],
	}
	for key in _slot_samples:
		samples[key + "_us"] = []
	for key in _count_samples:
		samples[key] = []
	return samples


func _append_samples(destination: Dictionary, source: Dictionary) -> void:
	for key in destination:
		(destination[key] as Array).append_array(source[key] as Array)


func _summaries(samples: Dictionary) -> Dictionary:
	var summaries := {
		"mission_rows": _summary(samples["mission_rows_us"]),
		"present": _summary(samples["present_us"]),
		"world": _summary(samples["world_us"]),
		"outside_shell": _summary(samples["outside_shell_us"]),
		"frame": _summary(samples["frame_us"]),
	}
	for key in _slot_samples:
		summaries[key] = _summary(samples[key + "_us"])
	for key in _count_samples:
		summaries[key] = _count_summary(samples[key])
	return summaries


func _metrics_with_samples(samples: Dictionary, summaries: Dictionary) -> Dictionary:
	var metrics := {}
	for key in ["mission_rows", "present", "world", "outside_shell", "frame"]:
		metrics[key] = {"summary_ms": summaries[key], "samples_us": samples[key + "_us"]}
	for key in _slot_samples:
		metrics[key] = {"summary_ms": summaries[key], "samples_us": samples[key + "_us"]}
	for key in _count_samples:
		metrics[key] = {"summary": summaries[key], "samples": samples[key]}
	return metrics


static func _count_summary(values: Array) -> Dictionary:
	if values.is_empty():
		return {"samples": 0, "mean": 0.0, "max": 0}
	var total := 0
	var peak := 0
	for value in values:
		total += int(value)
		peak = maxi(peak, int(value))
	return {"samples": values.size(), "mean": float(total) / float(values.size()), "max": peak}


static func _summary(values: Array) -> Dictionary:
	if values.is_empty():
		return {"samples": 0, "mean_ms": 0.0, "p50_ms": 0.0, "p95_ms": 0.0, "p99_ms": 0.0, "max_ms": 0.0}
	var ordered := values.duplicate()
	ordered.sort()
	var total := 0
	for value in ordered:
		total += int(value)
	return {
		"samples": ordered.size(),
		"mean_ms": (float(total) / float(ordered.size())) / 1000.0,
		"p50_ms": float(_percentile_us(ordered, 0.50)) / 1000.0,
		"p95_ms": float(_percentile_us(ordered, 0.95)) / 1000.0,
		"p99_ms": float(_percentile_us(ordered, 0.99)) / 1000.0,
		"max_ms": float(int(ordered.back())) / 1000.0,
	}


# Nearest-rank over zero-based samples. Keeping the raw microseconds in JSON
# makes alternate percentile conventions reproducible without rerunning.
static func _percentile_us(ordered: Array, quantile: float) -> int:
	if ordered.is_empty():
		return 0
	var index := ceili(quantile * float(ordered.size())) - 1
	return int(ordered[clampi(index, 0, ordered.size() - 1)])


static func _sum_slots(sums: PackedInt64Array, slots: Array) -> int:
	var total := 0
	for slot in slots:
		total += int(sums[int(slot)])
	return total


static func _counter_delta(before: MissionPresentStats, after: MissionPresentStats) -> Dictionary:
	return {
		"plan_rebuilds": after.plan_rebuilds - before.plan_rebuilds,
		"transform_builds": after.transform_builds - before.transform_builds,
		"aim_dispatches": after.aim_dispatches - before.aim_dispatches,
		"rhc_dispatches": after.rhc_dispatches - before.rhc_dispatches,
		"part_dispatches": after.part_dispatches - before.part_dispatches,
		"control_dispatches": after.control_dispatches - before.control_dispatches,
		"body_dispatches": after.body_dispatches - before.body_dispatches,
		"moved": after.moved - before.moved,
		"posed": after.posed - before.posed,
		"hidden": after.hidden - before.hidden,
	}


static func _empty_counter_delta() -> Dictionary:
	var delta := {}
	for key in COUNTER_KEYS:
		delta[key] = 0
	return delta


static func _add_counter_delta(total: Dictionary, addition: Dictionary) -> void:
	for key in COUNTER_KEYS:
		total[key] = int(total.get(key, 0)) + int(addition.get(key, 0))


static func _counter_per_frame(delta: Dictionary, frame_count: int) -> Dictionary:
	var per_frame := {}
	if frame_count <= 0:
		return per_frame
	for key in COUNTER_KEYS:
		per_frame[key] = float(delta.get(key, 0)) / float(frame_count)
	return per_frame


static func _build_fingerprint(world: GameWorld, runtime: MissionPresentation, bms: String) -> Dictionary:
	var signatures := PackedStringArray()
	var model_count := 0
	var hidden_count := 0
	var identified_count := 0
	var stack: Array = [world]
	while not stack.is_empty():
		var node: Node = stack.pop_back()
		for child in node.get_children():
			stack.push_back(child)
		if not (node is ObjectModel):
			continue
		model_count += 1
		if not (node as Node3D).is_visible_in_tree():
			hidden_count += 1
		var ref: EntityRef = (node as ObjectModel).entity_ref
		if ref != null:
			identified_count += 1
			signatures.append("%s|%d|%d|%d|%d" % [
					node.name, ref.bms_id, ref.kind, ref.index, ref.item_id])
		else:
			signatures.append("%s|unidentified" % node.get_path())
	signatures.sort()
	var hash := HashingContext.new()
	hash.start(HashingContext.HASH_SHA256)
	for signature in signatures:
		hash.update((signature + "\n").to_utf8_buffer())
	var topology_sha256 := hash.finish().hex_encode()
	var bms_bytes := 0
	var resource_root := world.get_resource_root()
	if resource_root != null and not bms.is_empty():
		var bytes: PackedByteArray = resource_root.read_file(bms)
		bms_bytes = bytes.size()
	return {
		"bms_bytes": bms_bytes,
		"entity_count": runtime.entity_count(),
		"model_count": model_count,
		"identified_model_count": identified_count,
		"hidden_model_count": hidden_count,
		"topology_sha256": topology_sha256,
		"placement_stats": world.get_mission_stats(),
		"static_live_populations": world.get_static_live_population_count(),
	}


func _log_window(index: int, count: int, frames: int, summaries: Dictionary) -> void:
	var mission: Dictionary = summaries["mission_rows"]
	var present: Dictionary = summaries["present"]
	var world: Dictionary = summaries["world"]
	var outside: Dictionary = summaries["outside_shell"]
	var frame: Dictionary = summaries["frame"]
	_ctx.log(("window %d/%d frames=%d | mission %.3f mean / %.3f p95 / %.3f max ms | "
			+ "present %.3f | world %.3f | outside %.3f | frame %.3f") % [
			index, count, frames,
			float(mission["mean_ms"]), float(mission["p95_ms"]),
			float(mission["max_ms"]), float(present["mean_ms"]),
			float(world["mean_ms"]), float(outside["mean_ms"]),
			float(frame["mean_ms"])])
	# The engine-frame decomposition, mean ms per drained frame.
	var engine_parts := PackedStringArray()
	for key in ["process_callbacks", "deferred_flush", "draw", "pacing_input",
			"hud_draw_compile", "hud_draw_emit", "render_root_cpu",
			"render_root_gpu", "render_water_cpu", "render_water_gpu"]:
		var part: Dictionary = summaries[key]
		if int(part["samples"]) > 0:
			engine_parts.append("%s %.3f" % [key, float(part["mean_ms"])])
	_ctx.log("  engine: " + " | ".join(engine_parts))
	var count_parts := PackedStringArray()
	for key in _count_samples:
		var part: Dictionary = summaries[key]
		if int(part["samples"]) > 0:
			count_parts.append("%s %.1f" % [key, float(part["mean"])])
	_ctx.log("  passes: " + " | ".join(count_parts))
	# The world breakdown: every prefixed world span, ranked by mean, so the
	# log alone names the heaviest rows.
	var ranked: Array = []
	for key in _slot_samples:
		if ENGINE_SLOT_SAMPLES.has(key):
			continue
		var part: Dictionary = summaries[key]
		if int(part["samples"]) > 0 and float(part["mean_ms"]) > 0.0:
			ranked.append([float(part["mean_ms"]), key, float(part["p95_ms"])])
	ranked.sort_custom(func(a, b): return float(a[0]) > float(b[0]))
	var world_parts := PackedStringArray()
	for entry in ranked:
		world_parts.append("%s %.3f/%.3f" % [entry[1], entry[0], entry[2]])
	_ctx.log("  world (mean/p95 ms): " + " | ".join(world_parts))


static func _output_path(ctx: ProbeContext, override: String, bms: String, label: String) -> String:
	if not override.is_empty():
		var chosen := override.replace("\\", "/")
		if chosen.begins_with("res://") or chosen.begins_with("user://"):
			return ProjectSettings.globalize_path(chosen).simplify_path()
		if chosen.is_absolute_path():
			return chosen.simplify_path()
		return ProjectSettings.globalize_path("res://../".path_join(chosen)).simplify_path()
	return ctx.artifact_dir.path_join("mission_rows_%s_%s.json" % [
			_safe_filename(bms.get_file().get_basename()), _safe_filename(label)])


static func _safe_filename(value: String) -> String:
	var pattern := RegEx.new()
	if pattern.compile("[^a-z0-9_-]+") != OK:
		return "run"
	var safe := pattern.sub(value.to_lower(), "_", true).strip_edges()
	return safe if not safe.is_empty() else "run"


static func _write_json(path: String, result: Dictionary) -> Error:
	var directory := path.get_base_dir()
	var mkdir_error := DirAccess.make_dir_recursive_absolute(directory)
	if mkdir_error != OK and mkdir_error != ERR_ALREADY_EXISTS:
		return mkdir_error
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return FileAccess.get_open_error()
	file.store_string(JSON.stringify(result, "\t"))
	var error := file.get_error()
	file.close()
	return error
