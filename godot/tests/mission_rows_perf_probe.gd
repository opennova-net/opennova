extends SceneTree

# Exact Mission Rows benchmark. Boots the real SP game shell, enables its
# FrameStatsBoard directly (no overlay/UI cost), and drains the board once per
# process frame so every sample is the exact span that feeds F3's
# PRESENT_MISSION row. The same drains retain whole-Present, World, wall-frame,
# and outside-shell numbers so an optimization cannot merely move work beyond
# the present span.
#
# Windowed (rendering must stay live):
#   NW_SP_MISSION=NosXmas.bms NW_RESOURCE_DIR=<pff install> NW_EXPANSION=revx02 \
#   "$GODOT_BIN" --path godot -s res://tests/mission_rows_perf_probe.gd
#
# Defaults: 6 s warmup, then five 10 s measurement windows. Optional controls:
#   NW_MISSION_ROWS_PERF_WARMUP_SECONDS
#   NW_MISSION_ROWS_PERF_WINDOW_SECONDS
#   NW_MISSION_ROWS_PERF_WINDOWS
#   NW_MISSION_ROWS_PERF_LABEL
#   NW_MISSION_ROWS_PERF_OUTPUT
# With no output override, JSON lands under the worktree's ignored
# .scratch/perf/ directory.

const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const DEFAULT_WARMUP_SECONDS := 6.0
const DEFAULT_WINDOW_SECONDS := 10.0
const DEFAULT_WINDOW_COUNT := 5

const ResourceDirSettings := preload(
		"res://engine/resource_index/resource_dir_settings.gd")
const MountGuard := preload("res://tests/perf_probe_mount_guard.gd")

const PRESENT_SLOTS := [
	FrameStatsBoard.PRESENT_SNAPSHOT,
	FrameStatsBoard.PRESENT_MISSION,
	FrameStatsBoard.PRESENT_WIRE,
	FrameStatsBoard.PRESENT_FIRE,
	FrameStatsBoard.PRESENT_DESTRUCTION,
	FrameStatsBoard.PRESENT_THROWABLE,
]
const SHELL_LEG_SLOTS := [
	FrameStatsBoard.FRAME_PLAYER_BEFORE,
	FrameStatsBoard.FRAME_WORLD,
	FrameStatsBoard.FRAME_PLAYER_AFTER,
	FrameStatsBoard.FRAME_HUD,
]
const COUNTER_KEYS := [
	"plan_rebuilds",
	"transform_builds",
	"aim_dispatches",
	"rhc_dispatches",
	"part_dispatches",
	"control_dispatches",
	"body_dispatches",
	"muzzle_queries",
	"moved",
	"posed",
	"hidden",
	"muzzles",
]

var _mount_guard = MountGuard.new()
var _requested_exit_code := 1
var _board: FrameStatsBoard = null


func _initialize() -> void:
	call_deferred("_run")


func _finalize() -> void:
	if _board != null:
		_board.set_capture_active(false)
	if _mount_guard.restore() != OK and _requested_exit_code == 0:
		quit(1)


func _run() -> void:
	var bms := OS.get_environment("NW_SP_MISSION").strip_edges()
	if bms.is_empty():
		push_error("[mrp] set NW_SP_MISSION=<mission.bms>")
		_finish(1)
		return
	if _mount_guard.capture() != OK:
		push_error("[mrp] could not snapshot the shared mount config")
		_finish(1)
		return
	var resource_dir := OS.get_environment("NW_RESOURCE_DIR").strip_edges()
	var expansion := OS.get_environment("NW_EXPANSION").strip_edges()
	if not resource_dir.is_empty():
		ResourceDirSettings.set_resource_dir(resource_dir)
		ResourceDirSettings.set_expansion(expansion)

	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		push_error("[mrp] failed to load main_game.tscn")
		_finish(1)
		return
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	if world == null:
		push_error("[mrp] main_game lacks World")
		_finish(1)
		return

	var wall_start := Time.get_ticks_msec()
	while not world.has_local_player():
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 \
				> LOAD_TIMEOUT_WALL_SECONDS:
			push_error("[mrp] player never spawned (mission load stalled?)")
			_finish(1)
			return
	var runtime = world.get_runtime() if world.has_method("get_runtime") else null
	if runtime == null or not runtime.has_method("get_mission_present_stats"):
		push_error("[mrp] loaded world has no mission-present stats seam")
		_finish(1)
		return
	_board = game.get_frame_stats_board()
	if _board == null:
		push_error("[mrp] main game has no FrameStatsBoard")
		_finish(1)
		return

	var warmup_seconds := _env_float(
			"NW_MISSION_ROWS_PERF_WARMUP_SECONDS",
			DEFAULT_WARMUP_SECONDS, 0.0)
	var window_seconds := _env_float(
			"NW_MISSION_ROWS_PERF_WINDOW_SECONDS",
			DEFAULT_WINDOW_SECONDS, 0.1)
	var window_count := _env_int(
			"NW_MISSION_ROWS_PERF_WINDOWS", DEFAULT_WINDOW_COUNT, 1)
	var label := OS.get_environment(
			"NW_MISSION_ROWS_PERF_LABEL").strip_edges()
	if label.is_empty():
		label = "unlabeled"

	_board.set_capture_active(true)
	print("[mrp] mission=%s loaded; warming %.2f s with exact capture active" % [
			bms, warmup_seconds])
	await _drain_for_seconds(warmup_seconds)
	# Start every measured window from an empty board even when warmup is zero.
	_board.drain()

	var fingerprint := _build_fingerprint(world, runtime, bms)
	print("[mrp] models=%d hidden=%d entities=%d bms_bytes=%d topology=%s" % [
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
		var counter_start: MissionPresentStats = \
				runtime.get_mission_present_stats()
		var measured: Dictionary = await _measure_window(window_seconds)
		var counter_end: MissionPresentStats = runtime.get_mission_present_stats()
		var counter_delta := _counter_delta(counter_start, counter_end)
		var samples: Dictionary = measured["samples"]
		_append_samples(aggregate_samples, samples)
		_add_counter_delta(aggregate_counter_delta, counter_delta)
		var summaries := _summaries(samples)
		var sample_frames := (samples["mission_rows_us"] as Array).size()
		var window_result := {
			"index": i + 1,
			"target_seconds": window_seconds,
			"elapsed_seconds": measured["elapsed_seconds"],
			"drains": measured["drains"],
			"coalesced_drains": measured["coalesced_drains"],
			"missing_mission_drains": measured["missing_mission_drains"],
			"metrics": _metrics_with_samples(samples, summaries),
			"presenter_counter_delta": counter_delta,
			"presenter_counter_per_mission_frame":
					_counter_per_frame(counter_delta, sample_frames),
		}
		windows.append(window_result)
		_print_window(i + 1, window_count, sample_frames, summaries)
		# Formatting/printing is diagnostic work, not game work. Keep its wall
		# tail out of the next window without counting these frames' presenter
		# calls in any counter delta.
		if i + 1 < window_count:
			await _drain_frames(2)

	if (aggregate_samples["mission_rows_us"] as Array).is_empty():
		push_error("[mrp] capture produced no PRESENT_MISSION samples")
		_finish(2)
		return

	var aggregate_summaries := _summaries(aggregate_samples)
	var aggregate_frames := (
			aggregate_samples["mission_rows_us"] as Array).size()
	var result := {
		"schema_version": 1,
		"probe": "mission_rows_perf",
		"captured_at_utc": Time.get_datetime_string_from_system(true, true),
		"label": label,
		"mission": {
			"file": bms,
			"resource_dir": resource_dir,
			"expansion": expansion,
		},
		"configuration": {
			"warmup_seconds": warmup_seconds,
			"window_seconds": window_seconds,
			"window_count": window_count,
			"capture": "FrameStatsBoard drained once per process frame",
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
			"presenter_counter_per_mission_frame":
					_counter_per_frame(
							aggregate_counter_delta, aggregate_frames),
		},
	}

	var output_path := _output_path(bms, label)
	var write_error := _write_json(output_path, result)
	if write_error != OK:
		push_error("[mrp] failed to write %s (error %d)" % [
				output_path, write_error])
		_finish(3)
		return
	var mission_summary: Dictionary = aggregate_summaries["mission_rows"]
	print("[mrp] aggregate frames=%d mission mean=%.3f ms p95=%.3f ms p99=%.3f ms max=%.3f ms" % [
			aggregate_frames,
			float(mission_summary["mean_ms"]),
			float(mission_summary["p95_ms"]),
			float(mission_summary["p99_ms"]),
			float(mission_summary["max_ms"])])
	print("[mrp] JSON %s" % output_path)
	_finish(0)


func _drain_for_seconds(seconds: float) -> void:
	var deadline := Time.get_ticks_usec() + int(seconds * 1_000_000.0)
	while Time.get_ticks_usec() < deadline:
		await process_frame
		_board.drain()


func _drain_frames(count: int) -> void:
	for _i in range(count):
		await process_frame
		_board.drain()


func _measure_window(seconds: float) -> Dictionary:
	var samples := _empty_samples()
	var start := Time.get_ticks_usec()
	var deadline := start + int(seconds * 1_000_000.0)
	var drains := 0
	var coalesced_drains := 0
	var missing_mission_drains := 0
	while Time.get_ticks_usec() < deadline:
		await process_frame
		var captured = _board.drain()
		drains += 1
		if captured.frames > 1:
			coalesced_drains += 1
			# A summed multi-frame window cannot provide an exact percentile
			# sample. Preserve the diagnostic count and leave it out.
			continue
		if captured.sample_frames[FrameStatsBoard.PRESENT_MISSION] <= 0:
			missing_mission_drains += 1
			continue
		(samples["mission_rows_us"] as Array).append(
				int(captured.sums[FrameStatsBoard.PRESENT_MISSION]))
		(samples["present_us"] as Array).append(
				_sum_slots(captured.sums, PRESENT_SLOTS))
		if captured.sample_frames[FrameStatsBoard.FRAME_WORLD] > 0:
			(samples["world_us"] as Array).append(
					int(captured.sums[FrameStatsBoard.FRAME_WORLD]))
		if captured.sample_frames[FrameStatsBoard.FRAME_WALL] > 0:
			var wall_us := int(
					captured.sums[FrameStatsBoard.FRAME_WALL])
			(samples["frame_us"] as Array).append(wall_us)
			(samples["outside_shell_us"] as Array).append(maxi(
					wall_us - _sum_slots(
							captured.sums, SHELL_LEG_SLOTS), 0))
	return {
		"elapsed_seconds":
				float(Time.get_ticks_usec() - start) / 1_000_000.0,
		"drains": drains,
		"coalesced_drains": coalesced_drains,
		"missing_mission_drains": missing_mission_drains,
		"samples": samples,
	}


func _empty_samples() -> Dictionary:
	return {
		"mission_rows_us": [],
		"present_us": [],
		"world_us": [],
		"outside_shell_us": [],
		"frame_us": [],
	}


func _append_samples(destination: Dictionary, source: Dictionary) -> void:
	for key in destination:
		(destination[key] as Array).append_array(source[key] as Array)


func _summaries(samples: Dictionary) -> Dictionary:
	return {
		"mission_rows": _summary(samples["mission_rows_us"]),
		"present": _summary(samples["present_us"]),
		"world": _summary(samples["world_us"]),
		"outside_shell": _summary(samples["outside_shell_us"]),
		"frame": _summary(samples["frame_us"]),
	}


func _metrics_with_samples(samples: Dictionary, summaries: Dictionary) -> Dictionary:
	return {
		"mission_rows": {
			"summary_ms": summaries["mission_rows"],
			"samples_us": samples["mission_rows_us"],
		},
		"present": {
			"summary_ms": summaries["present"],
			"samples_us": samples["present_us"],
		},
		"world": {
			"summary_ms": summaries["world"],
			"samples_us": samples["world_us"],
		},
		"outside_shell": {
			"summary_ms": summaries["outside_shell"],
			"samples_us": samples["outside_shell_us"],
		},
		"frame": {
			"summary_ms": summaries["frame"],
			"samples_us": samples["frame_us"],
		},
	}


func _summary(values: Array) -> Dictionary:
	if values.is_empty():
		return {
			"samples": 0,
			"mean_ms": 0.0,
			"p50_ms": 0.0,
			"p95_ms": 0.0,
			"p99_ms": 0.0,
			"max_ms": 0.0,
		}
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
func _percentile_us(ordered: Array, quantile: float) -> int:
	if ordered.is_empty():
		return 0
	var index := ceili(quantile * float(ordered.size())) - 1
	return int(ordered[clampi(index, 0, ordered.size() - 1)])


func _sum_slots(sums: PackedInt64Array, slots: Array) -> int:
	var total := 0
	for slot in slots:
		total += int(sums[int(slot)])
	return total


func _counter_delta(before: MissionPresentStats,
		after: MissionPresentStats) -> Dictionary:
	return {
		"plan_rebuilds": after.plan_rebuilds - before.plan_rebuilds,
		"transform_builds": after.transform_builds - before.transform_builds,
		"aim_dispatches": after.aim_dispatches - before.aim_dispatches,
		"rhc_dispatches": after.rhc_dispatches - before.rhc_dispatches,
		"part_dispatches": after.part_dispatches - before.part_dispatches,
		"control_dispatches":
				after.control_dispatches - before.control_dispatches,
		"body_dispatches": after.body_dispatches - before.body_dispatches,
		"muzzle_queries": after.muzzle_queries - before.muzzle_queries,
		"moved": after.moved - before.moved,
		"posed": after.posed - before.posed,
		"hidden": after.hidden - before.hidden,
		"muzzles": after.muzzles - before.muzzles,
	}


func _empty_counter_delta() -> Dictionary:
	var delta := {}
	for key in COUNTER_KEYS:
		delta[key] = 0
	return delta


func _add_counter_delta(total: Dictionary, addition: Dictionary) -> void:
	for key in COUNTER_KEYS:
		total[key] = int(total.get(key, 0)) + int(addition.get(key, 0))


func _counter_per_frame(delta: Dictionary, frame_count: int) -> Dictionary:
	var per_frame := {}
	if frame_count <= 0:
		return per_frame
	for key in COUNTER_KEYS:
		per_frame[key] = float(delta.get(key, 0)) / float(frame_count)
	return per_frame


func _build_fingerprint(world, runtime, bms: String) -> Dictionary:
	var signatures := PackedStringArray()
	var model_count := 0
	var hidden_count := 0
	var identified_count := 0
	var stack: Array = [world]
	while not stack.is_empty():
		var node: Node = stack.pop_back()
		for child in node.get_children():
			stack.push_back(child)
		if not (node is NovaObjectModel):
			continue
		model_count += 1
		if not (node as Node3D).is_visible_in_tree():
			hidden_count += 1
		if node.has_meta("entity_ref"):
			identified_count += 1
			var ref: Dictionary = node.get_meta("entity_ref")
			signatures.append("%s|%d|%d|%d|%d" % [
					node.name,
					int(ref.get("bms_id", 0)),
					int(ref.get("kind", -1)),
					int(ref.get("index", -1)),
					int(ref.get("item_id", 0))])
		else:
			signatures.append("%s|unidentified" % node.get_path())
	signatures.sort()
	var hash := HashingContext.new()
	hash.start(HashingContext.HASH_SHA256)
	for signature in signatures:
		hash.update((signature + "\n").to_utf8_buffer())
	var topology_sha256 := hash.finish().hex_encode()
	var bms_bytes := 0
	if world.has_method("get_resource_root"):
		var resource_root = world.get_resource_root()
		if resource_root != null:
			var bytes: PackedByteArray = resource_root.read_file(bms)
			bms_bytes = bytes.size()
	return {
		"bms_bytes": bms_bytes,
		"entity_count":
				int(runtime.entity_count())
				if runtime.has_method("entity_count") else 0,
		"model_count": model_count,
		"identified_model_count": identified_count,
		"hidden_model_count": hidden_count,
		"topology_sha256": topology_sha256,
		"placement_stats":
				world.get_mission_stats()
				if world.has_method("get_mission_stats") else {},
	}


func _print_window(index: int, count: int, frames: int,
		summaries: Dictionary) -> void:
	var mission: Dictionary = summaries["mission_rows"]
	var present: Dictionary = summaries["present"]
	var world: Dictionary = summaries["world"]
	var outside: Dictionary = summaries["outside_shell"]
	var frame: Dictionary = summaries["frame"]
	var format := (
			"[mrp] window %d/%d frames=%d | "
			+ "mission %.3f mean / %.3f p95 / %.3f max ms | "
			+ "present %.3f | world %.3f | outside %.3f | frame %.3f")
	print(format % [
			index, count, frames,
			float(mission["mean_ms"]), float(mission["p95_ms"]),
			float(mission["max_ms"]), float(present["mean_ms"]),
			float(world["mean_ms"]), float(outside["mean_ms"]),
			float(frame["mean_ms"])])


func _output_path(bms: String, label: String) -> String:
	var override := OS.get_environment(
			"NW_MISSION_ROWS_PERF_OUTPUT").strip_edges().replace("\\", "/")
	if not override.is_empty():
		if override.begins_with("res://") \
				or override.begins_with("user://"):
			return ProjectSettings.globalize_path(override).simplify_path()
		if override.is_absolute_path():
			return override.simplify_path()
		return ProjectSettings.globalize_path(
				"res://../".path_join(override)).simplify_path()
	var safe_mission := _safe_filename(bms.get_file().get_basename())
	var safe_label := _safe_filename(label)
	var timestamp_ms := int(Time.get_unix_time_from_system() * 1000.0)
	return ProjectSettings.globalize_path(
			"res://../.scratch/perf/mission_rows_%s_%s_%d.json" % [
					safe_mission, safe_label, timestamp_ms]).simplify_path()


func _safe_filename(value: String) -> String:
	var pattern := RegEx.new()
	if pattern.compile("[^a-z0-9_-]+") != OK:
		return "run"
	var safe := pattern.sub(value.to_lower(), "_", true).strip_edges()
	return safe if not safe.is_empty() else "run"


func _write_json(path: String, result: Dictionary) -> Error:
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


func _env_float(name: String, fallback: float, minimum: float) -> float:
	var raw := OS.get_environment(name).strip_edges()
	if raw.is_empty() or not raw.is_valid_float():
		return fallback
	return maxf(raw.to_float(), minimum)


func _env_int(name: String, fallback: int, minimum: int) -> int:
	var raw := OS.get_environment(name).strip_edges()
	if raw.is_empty() or not raw.is_valid_int():
		return fallback
	return maxi(raw.to_int(), minimum)


func _finish(exit_code: int) -> void:
	if _board != null:
		_board.set_capture_active(false)
	if _mount_guard.restore() != OK and exit_code == 0:
		exit_code = 1
	_requested_exit_code = exit_code
	quit(exit_code)
