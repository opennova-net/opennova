extends SceneTree

# Joiner-side twin of perf_fire_probe_game.gd: boots main_game.tscn through the
# NW_LAN_JOIN flow against an already-running LAN host, waits for the joiner's
# local player to spawn (a co-op host auto-releases deployment), then measures
# the same frame segmentation and per-phase spans. Purpose: the MP-vs-SP FPS
# A/B — run perf_fire_probe_game.gd (NOVA_PF_BASELINE_ONLY=1) on the same
# mission for the SP side and diff the rows.
#
#   NW_LAN_JOIN=127.0.0.1:32768 NW_LAN_NAME=PerfJoiner \
#   NW_RESOURCE_DIR=<pff install> "$GODOT_BIN" --path godot \
#       -s res://tests/perf_wire_probe_game.gd
#
# Windowed (not headless): render cost is the question. The persisted dir /
# expansion are snapshotted and restored on exit, matching the SP probe.

const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const MountGuard := preload("res://tests/perf_probe_mount_guard.gd")

var _mount_guard = MountGuard.new()
var _requested_exit_code := 1
var _runtime = null
var _main = null
var _gw = null
var _vprid := RID()
var _seg_t_pf := 0
var _seg_t_pre := 0
var _seg_sum := [0, 0, 0]
var _seg_n := 0
var _phase := ""
var _sample_t0 := 0


func _initialize() -> void:
	call_deferred("_run")


func _finalize() -> void:
	if _mount_guard.restore() != OK and _requested_exit_code == 0:
		_requested_exit_code = 1
		quit(1)


func _on_seg_process_frame() -> void:
	var now := Time.get_ticks_usec()
	if _seg_t_pre > 0:
		_seg_sum[2] += now - _seg_t_pre
		_seg_n += 1
	_seg_t_pf = now


func _on_seg_pre_draw() -> void:
	var now := Time.get_ticks_usec()
	if _seg_t_pf > 0:
		_seg_sum[0] += now - _seg_t_pf
	_seg_t_pre = now


func _on_seg_post_draw() -> void:
	var now := Time.get_ticks_usec()
	if _seg_t_pre > 0:
		_seg_sum[1] += now - _seg_t_pre
	_seg_t_pre = now


func _run() -> void:
	var target := OS.get_environment("NW_LAN_JOIN").strip_edges()
	if target.is_empty():
		push_error("[pwj] set NW_LAN_JOIN=<host:port>")
		_finish(1)
		return
	if _mount_guard.capture() != OK:
		push_error("[pwj] could not snapshot the shared mount config")
		_finish(1)
		return
	var res_dir := OS.get_environment("NW_RESOURCE_DIR").strip_edges()
	if not res_dir.is_empty():
		ResourceDirSettings.set_resource_dir(res_dir)
		ResourceDirSettings.set_expansion("")
	print("[pwj] mount: dir=%s expansion=%s" % [
			ResourceDirSettings.get_resource_dir(), ResourceDirSettings.get_expansion()])

	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		push_error("[pwj] failed to load main_game.tscn")
		_finish(1)
		return
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	if world == null:
		push_error("[pwj] main_game lacks World")
		_finish(1)
		return

	var wall_start := Time.get_ticks_msec()
	while not (world.get_sim() != null and world.get_sim().has_local_player()):
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			push_error("[pwj] joiner never spawned (join/deploy stalled?)")
			_finish(1)
			return
	print("[pwj] joined %s, local player spawned after %.1fs" % [
			target, float(Time.get_ticks_msec() - wall_start) / 1000.0])
	await _settle_ms(5000)

	_runtime = _find_by_method(root, "tick_realtime")
	_main = game
	_gw = world
	for target_v in [_main, _gw]:
		var t := target_v as Object
		if is_instance_valid(t) and t.has_method("set_perf_probe_enabled"):
			t.set_perf_probe_enabled(true)

	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	_vprid = root.get_viewport().get_viewport_rid()
	RenderingServer.viewport_set_measure_render_time(_vprid, true)
	process_frame.connect(_on_seg_process_frame)
	RenderingServer.frame_pre_draw.connect(_on_seg_pre_draw)
	RenderingServer.frame_post_draw.connect(_on_seg_post_draw)
	_census()
	await _settle_ms(1500)

	var base := await _measure("baseline", 8000)

	# One structural A/B: the whole world tick (sim + netsim pump + present
	# passes) off for 3 s. The session tolerates it (peer timeout is 120 s).
	var worldoff := {avg = -1.0}
	if _main != null and "_perf_probe_skip_world" in _main:
		_main.set("_perf_probe_skip_world", true)
		await _settle_ms(300)
		worldoff = await _measure("worldoff", 3000)
		_main.set("_perf_probe_skip_world", false)

	_report("BASELINE", base)
	if float(worldoff.avg) >= 0.0:
		print("[pwj] WORLDOFF avg=%.2fms (world.tick share vs baseline: %+.2fms)" % [
				float(worldoff.avg), float(base.avg) - float(worldoff.avg)])
	_finish(0)


func _measure(phase: String, duration_ms: int) -> Dictionary:
	_phase = phase
	_sample_t0 = Time.get_ticks_msec()
	var samples: Array[float] = []
	var sec_accum := 0.0
	var sec_frames := 0
	var deadline := Time.get_ticks_msec() + duration_ms
	var last := Time.get_ticks_usec()
	while Time.get_ticks_msec() < deadline:
		await process_frame
		var now := Time.get_ticks_usec()
		var ms := float(now - last) / 1000.0
		last = now
		samples.append(ms)
		sec_accum += ms
		sec_frames += 1
		if sec_accum >= 1000.0:
			print(_counter_row(sec_frames, sec_accum))
			last = Time.get_ticks_usec()
			sec_accum = 0.0
			sec_frames = 0
	print(_counter_row(sec_frames, sec_accum))
	var s := samples.duplicate()
	s.sort()
	var n := s.size()
	if n == 0:
		return {avg = 0.0, p50 = 0.0, p95 = 0.0, mx = 0.0, n = 0, worst = []}
	var sum := 0.0
	for v in s:
		sum += v
	var worst: Array[String] = []
	var tagged := []
	var t_ms := 0.0
	for v in samples:
		tagged.append([v, t_ms])
		t_ms += v
	tagged.sort_custom(func(a, b): return a[0] > b[0])
	for i in mini(8, tagged.size()):
		worst.append("%.1fms@t+%.2fs" % [tagged[i][0], tagged[i][1] / 1000.0])
	return {
		avg = sum / n, p50 = s[n >> 1], p95 = s[int(float(n) * 0.95)], mx = s[n - 1],
		n = n, worst = worst,
	}


func _counter_row(sec_frames: int, sec_accum: float) -> String:
	var t := float(Time.get_ticks_msec() - _sample_t0) / 1000.0
	var fps := 0.0
	if sec_frames > 0 and sec_accum > 0.0:
		fps = float(sec_frames) / (sec_accum / 1000.0)
	var rt := "-"
	if _runtime != null:
		var sim_us = _runtime.get("_perf_sim_us")
		var present_us = _runtime.get("_perf_present_us")
		var fx_us = _runtime.get("_perf_effects_us")
		var ticks_n = _runtime.get("_ticks_last_frame")
		if sim_us != null:
			rt = "sim=%.2f present=%.2f fx=%.2f ticks=%d" % [
					float(sim_us) / 1000.0,
					float(present_us) / 1000.0 if present_us != null else -1.0,
					float(fx_us) / 1000.0 if fx_us != null else -1.0,
					int(ticks_n) if ticks_n != null else -1]
	var spans := ""
	if _main != null:
		var mg = _main.get("_perf_probe_spans")
		if mg is Dictionary and not (mg as Dictionary).is_empty():
			spans += " main{before=%.1f world=%.1f after=%.1f hud=%.1f}" % [
					float(mg.get("before", 0)) / 1000.0, float(mg.get("world", 0)) / 1000.0,
					float(mg.get("after", 0)) / 1000.0, float(mg.get("hud", 0)) / 1000.0]
	if _gw != null:
		var gg = _gw.get("_perf_probe_spans")
		if gg is Dictionary and not (gg as Dictionary).is_empty():
			spans += " gw{occl_r=%.1f occl_f=%.1f iris=%.1f weather=%.1f blink=%.1f}" % [
					float(gg.get("occl_restore", 0)) / 1000.0,
					float(gg.get("occl_frame", 0)) / 1000.0,
					float(gg.get("iris", 0)) / 1000.0,
					float(gg.get("weather", 0)) / 1000.0,
					float(gg.get("blink", 0)) / 1000.0]
		var pc = _gw.get_runtime_perf_counters() if _gw.has_method("get_runtime_perf_counters") else null
		if pc is Dictionary:
			spans += " gwtick{total=%.1f foliage=%.1f runtime=%.1f audio=%.1f}" % [
					float(pc.get("tick_us", 0)) / 1000.0, float(pc.get("foliage_us", 0)) / 1000.0,
					float(pc.get("runtime_us", 0)) / 1000.0, float(pc.get("audio_us", 0)) / 1000.0]
	var rmeas := ""
	if _vprid.is_valid():
		rmeas = " rcpu=%.1f rgpu=%.1f" % [
				RenderingServer.viewport_get_measured_render_time_cpu(_vprid),
				RenderingServer.viewport_get_measured_render_time_gpu(_vprid)]
	if _seg_n > 0:
		rmeas += " seg{proc=%.1f draw=%.1f post=%.1f}" % [
				float(_seg_sum[0]) / 1000.0 / _seg_n,
				float(_seg_sum[1]) / 1000.0 / _seg_n,
				float(_seg_sum[2]) / 1000.0 / _seg_n]
		_seg_sum = [0, 0, 0]
		_seg_n = 0
	return ("[pwj] %s t+%4.1fs fps=%6.1f proc=%5.2fms %s draws=%5d objs=%5d prims=%8d nodes=%5d%s" % [
			_phase, t, fps,
			Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0,
			rt,
			int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)),
			int(Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME)),
			int(Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME)),
			int(Performance.get_monitor(Performance.OBJECT_NODE_COUNT)), spans + rmeas])


func _census() -> void:
	var counts := {}
	var stack: Array = [root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		for cls in ["MeshInstance3D", "MultiMeshInstance3D", "GPUParticles3D",
				"CPUParticles3D", "Camera3D", "AnimationPlayer", "AudioStreamPlayer3D",
				"Skeleton3D"]:
			if n.is_class(cls):
				counts[cls] = int(counts.get(cls, 0)) + 1
		for ch in n.get_children():
			stack.push_back(ch)
	print("[pwj] census: ", counts)


func _report(label: String, st: Dictionary) -> void:
	print("[pwj] %s frames=%5d avg=%6.2fms p50=%6.2fms p95=%6.2fms max=%7.2fms worst=[%s]" % [
			label, st.n, st.avg, st.p50, st.p95, st.mx, ", ".join(st.worst)])


func _finish(code: int) -> void:
	_requested_exit_code = code
	quit(code)


func _settle_ms(ms: int) -> void:
	var deadline := Time.get_ticks_msec() + ms
	while Time.get_ticks_msec() < deadline:
		await process_frame


func _find_by_method(node: Node, method: String) -> Node:
	if node.has_method(method):
		return node
	for ch in node.get_children():
		var found := _find_by_method(ch, method)
		if found != null:
			return found
	return null
