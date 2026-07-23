extends Node

# Full-auto fire performance probe: boots ONED play-in-editor on a real mission
# (destruction_probe's boot shape), then measures frame-time distributions over
# three wall-clock phases — BASELINE (idle), FIRING (two full-auto windows with
# a reload between), COOLDOWN (after release) — with vsync disabled and FPS
# uncapped so frame time reflects true cost. Per-second counter rows (draw
# calls, node/orphan counts, process/physics ms, particle stats when exposed)
# localize what grows while firing. Windowed run:
#   NOVA_RESOURCE_DIR=<assets> "$GODOT_BIN" --path godot res://tests/perf_fire_probe.tscn
# Optional: NOVA_MISSION_BMS (default 05TR.bms), NOVA_PF_FIRE_SECONDS (default
# 3.0 per window), NOVA_PF_CAPTURE=1 (PNG sanity capture during the reload gap).
#
# Verdict: REGRESSION (exit 2) when FIRING is markedly slower than BASELINE
# (avg > 1.5x + 1 ms, or p95 > 2x + 2 ms); LEAK SUSPECT noted when COOLDOWN
# fails to return to baseline. GREEN exits 0.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const MountGuard := preload("res://tests/perf_probe_mount_guard.gd")
const EditorScene := preload("res://modtools/editor/editor_main.tscn")
const OUT_DIR := "res://../.scratch/perf"

var _out_abs := ""
var _mount_guard = MountGuard.new()
var _world = null
var _sim = null
var _runtime = null
var _effect_world = null

var _sampling := false
var _samples: Array[float] = []  # frame ms
var _sample_t0 := 0
var _last_usec := 0
var _sec_accum := 0.0
var _sec_frames := 0
var _phase := ""


func _ready() -> void:
	_out_abs = ProjectSettings.globalize_path(OUT_DIR)
	DirAccess.make_dir_recursive_absolute(_out_abs)
	# The resource dir/expansion are persisted user:// state SHARED with the
	# user's editor — restore both on every exit path.
	if _mount_guard.capture() != OK:
		push_error("[pf] could not snapshot the shared mount config")
		get_tree().quit(1)
		return
	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()
	# Only override the expansion when the env var is set — the default run
	# reproduces the user's persisted mount (resource dir + expansion) exactly.
	var expn := OS.get_environment("NOVA_WR_EXPANSION").strip_edges()
	if not expn.is_empty():
		ResourceDirSettings.set_expansion("" if expn == "none" else expn)
	ResourceDirSettings.set_resource_dir(root)
	print("[pf] mount: dir=%s expansion=%s" % [root, ResourceDirSettings.get_expansion()])

	var app = EditorScene.instantiate()
	add_child(app)
	await get_tree().process_frame
	for _i in 8:
		await get_tree().process_frame
	var ws_station = app.workstation
	ws_station.set_resource_root_dir(root)
	NovaWindow.set_fullscreen(get_window(), true)
	await _settle_ms(1000)

	var bms := OS.get_environment("NOVA_MISSION_BMS").strip_edges()
	if bms.is_empty():
		bms = "05TR.bms"
	var ws = ws_station.get_workspace_adapter(EditorWorkstation.Workspace.MISSION)
	var path := NovaPaths.resolve_file(root, bms)
	if ws.open_file(path) != OK:
		push_error("[pf] open failed")
		_restore_mount()
		get_tree().quit(1)
		return
	ws_station.set_active_workspace(EditorWorkstation.Workspace.MISSION)
	await _settle_ms(3000)

	if int(ws.play_mission()) != OK:
		push_error("[pf] play failed")
		_restore_mount()
		get_tree().quit(1)
		return
	await _settle_ms(5000)
	_world = _find_by_method(get_tree().root, "get_destruction_present_stats")
	# tick_realtime is unique to the mission runtime (GameWorld also exposes
	# get_sim — matching on that grabbed the wrong node and killed the rows).
	_runtime = _find_by_method(get_tree().root, "tick_realtime")
	if _runtime != null and _runtime.has_method("get_sim"):
		_sim = _runtime.get_sim()
	_effect_world = _find_by_method(get_tree().root, "get_debug_group_report")
	if _world == null or _sim == null:
		push_error("[pf] no game world/sim")
		_restore_mount()
		get_tree().quit(1)
		return

	# True frame cost, not vsync cadence.
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	_census()
	# Level the look slightly down so rounds impact terrain a few meters out
	# (impact effects are part of the suspected cost).
	_look(Vector2(0, 120))
	await _settle_ms(1500)

	var fire_s := float(OS.get_environment("NOVA_PF_FIRE_SECONDS").to_float())
	if fire_s <= 0.0:
		fire_s = 3.0

	# --- BASELINE ---
	var base := await _measure("baseline", 5000)

	# --- FIRING window 1 ---
	_mouse_btn(MOUSE_BUTTON_LEFT, true)
	var fire1 := await _measure("firing1", int(fire_s * 1000.0))
	_mouse_btn(MOUSE_BUTTON_LEFT, false)

	# Reload gap (excluded from stats). Optional sanity capture happens here so
	# the GPU readback hitch never pollutes a measured window.
	if OS.get_environment("NOVA_PF_CAPTURE") == "1":
		await _capture("firing_window.png")
	_hold(KEY_R, true)
	await _settle_ms(120)
	_hold(KEY_R, false)
	await _settle_ms(2600)

	# --- FIRING window 2 ---
	_mouse_btn(MOUSE_BUTTON_LEFT, true)
	var fire2 := await _measure("firing2", int(fire_s * 1000.0))
	_mouse_btn(MOUSE_BUTTON_LEFT, false)

	# --- COOLDOWN ---
	await _settle_ms(1500)
	var cool := await _measure("cooldown", 5000)

	_report("BASELINE", base)
	_report("FIRING1 ", fire1)
	_report("FIRING2 ", fire2)
	_report("COOLDOWN", cool)

	var base_avg: float = base.avg
	var fire_avg: float = maxf(fire1.avg, fire2.avg)
	var fire_p95: float = maxf(fire1.p95, fire2.p95)
	var base_p95: float = base.p95
	var cool_avg: float = cool.avg
	var regressed: bool = (fire_avg > base_avg * 1.5 + 1.0) or (fire_p95 > base_p95 * 2.0 + 2.0)
	var leaky: bool = cool_avg > base_avg * 1.3 + 1.0
	print("[pf] deltas: fire_avg %.2fx base | fire_p95 %.2fx base | cooldown %.2fx base" % [
			fire_avg / maxf(base_avg, 0.001), fire_p95 / maxf(base_p95, 0.001),
			cool_avg / maxf(base_avg, 0.001)])
	if leaky:
		print("[pf] LEAK SUSPECT: cooldown never returned to baseline")
	print("[pf] VERDICT: %s" % ("REGRESSION — firing is markedly slower than baseline"
			if regressed else "GREEN — firing within baseline envelope"))
	var exit_code := 2 if regressed else 0
	var restore_err := _restore_mount()
	if restore_err != OK and exit_code == 0:
		exit_code = 1
	get_tree().quit(exit_code)


func _restore_mount() -> Error:
	var err: Error = _mount_guard.restore()
	if err != OK:
		push_error("[pf] failed to restore the shared mount config (error %d)" % err)
	return err


func _exit_tree() -> void:
	_restore_mount()


func _process(_delta: float) -> void:
	if not _sampling:
		return
	var now := Time.get_ticks_usec()
	if _last_usec != 0:
		var ms := float(now - _last_usec) / 1000.0
		_samples.append(ms)
		_sec_accum += ms
		_sec_frames += 1
		if _sec_accum >= 1000.0:
			print(_counter_row())
			_sec_accum = 0.0
			_sec_frames = 0
	_last_usec = now


func _measure(phase: String, duration_ms: int) -> Dictionary:
	_phase = phase
	_samples = []
	_last_usec = 0
	_sec_accum = 0.0
	_sec_frames = 0
	_sample_t0 = Time.get_ticks_msec()
	_sampling = true
	await _settle_ms(duration_ms)
	_sampling = false
	print(_counter_row())
	var s := _samples.duplicate()
	s.sort()
	var n := s.size()
	if n == 0:
		return {avg = 0.0, p50 = 0.0, p95 = 0.0, mx = 0.0, n = 0, worst = []}
	var sum := 0.0
	for v in s:
		sum += v
	# Worst frames with their offsets into the phase — spike pattern beats avg
	# for telling per-shot hitches from monotonic climb.
	var worst: Array[String] = []
	var tagged := []
	var t_ms := 0.0
	for v in _samples:
		tagged.append([v, t_ms])
		t_ms += v
	tagged.sort_custom(func(a, b): return a[0] > b[0])
	for i in mini(8, tagged.size()):
		worst.append("%.1fms@t+%.2fs" % [tagged[i][0], tagged[i][1] / 1000.0])
	return {
		avg = sum / n, p50 = s[n >> 1], p95 = s[int(float(n) * 0.95)], mx = s[n - 1],
		n = n, worst = worst,
	}


# One-time scene shape census: what the present tree is made of.
func _census() -> void:
	var counts := {}
	var stack: Array = [get_tree().root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		for cls in ["MeshInstance3D", "MultiMeshInstance3D", "GPUParticles3D",
				"CPUParticles3D", "Camera3D", "AnimationPlayer"]:
			if n.is_class(cls):
				counts[cls] = int(counts.get(cls, 0)) + 1
		for ch in n.get_children():
			stack.push_back(ch)
	print("[pf] census: ", counts)


func _counter_row() -> String:
	var t := float(Time.get_ticks_msec() - _sample_t0) / 1000.0
	var fps := 0.0
	if _sec_frames > 0 and _sec_accum > 0.0:
		fps = float(_sec_frames) / (_sec_accum / 1000.0)
	# groups/alive-particles via the F3 read model (1 Hz — cheap, value-only).
	var parts := "-"
	if _effect_world != null:
		var groups: Array = _effect_world.get_debug_group_report()
		var alive := 0
		for g_v in groups:
			for e_v in (g_v as Dictionary).get("emitters", []):
				alive += int((e_v as Dictionary).get("alive", 0))
		parts = "%d/%d" % [groups.size(), alive]
	# live tracer channels: rows = per channel [style, age, count, count*4 floats].
	var trails := "-"
	if _sim != null and _sim.has_method("get_tracer_trails"):
		var rows: PackedFloat32Array = _sim.get_tracer_trails()
		var channels := 0
		var i := 0
		while i + 2 < rows.size():
			var count := int(rows[i + 2])
			i += 3
			if count < 0 or i + count * 4 > rows.size():
				break
			channels += 1
			i += count * 4
		trails = "%d(%df)" % [channels, rows.size()]
	# mission_runtime's own frame breakdown: native sim ticks vs present passes
	# vs effect drain, plus the catch-up batch size (the death-spiral signal).
	# Object.get() returns null on a missing property instead of throwing — one
	# bad field must never kill the whole row again.
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
	return ("[pf] %s t+%4.1fs fps=%6.1f proc=%5.2fms phys=%5.2fms %s draws=%5d objs=%5d prims=%8d nodes=%5d orphans=%4d parts=%s trails=%s" % [
			_phase, t, fps,
			Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0,
			Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1000.0,
			rt,
			int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)),
			int(Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME)),
			int(Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME)),
			int(Performance.get_monitor(Performance.OBJECT_NODE_COUNT)),
			int(Performance.get_monitor(Performance.OBJECT_ORPHAN_NODE_COUNT)),
			parts, trails])


func _report(label: String, st: Dictionary) -> void:
	print("[pf] %s frames=%5d avg=%6.2fms p50=%6.2fms p95=%6.2fms max=%7.2fms worst=[%s]" % [
			label, st.n, st.avg, st.p50, st.p95, st.mx, ", ".join(st.worst)])


func _find_by_method(node: Node, method: String) -> Node:
	if node.has_method(method):
		return node
	for ch in node.get_children():
		var f := _find_by_method(ch, method)
		if f != null:
			return f
	return null


func _settle_ms(ms: int) -> void:
	var deadline := Time.get_ticks_msec() + ms
	while Time.get_ticks_msec() < deadline:
		await get_tree().process_frame


func _hold(k: Key, down: bool) -> void:
	var e := InputEventKey.new()
	e.keycode = k
	e.physical_keycode = k
	e.pressed = down
	Input.parse_input_event(e)


func _mouse_btn(b: MouseButton, down: bool) -> void:
	var e := InputEventMouseButton.new()
	e.button_index = b
	e.pressed = down
	Input.parse_input_event(e)


func _look(total: Vector2) -> void:
	for _i in 10:
		var mm := InputEventMouseMotion.new()
		mm.relative = total / 10.0
		Input.parse_input_event(mm)


func _capture(name: String) -> void:
	await RenderingServer.frame_post_draw
	var img: Image = get_viewport().get_texture().get_image()
	if img != null:
		img.save_png(_out_abs.path_join(name))
		print("[pf] wrote ", name)
