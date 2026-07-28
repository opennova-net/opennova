extends SceneTree

# Game-shell twin of perf_fire_probe.gd: boots main_game.tscn (no ONED editor
# chrome) through the NW_SP_MISSION single-player start flow, then measures the
# same three phases — BASELINE / FIRING x2 (reload between) / COOLDOWN — with
# vsync off and per-second counter rows. Splits "editor chrome cost" from
# "engine cost" against the play-in-editor probe on the same mission. Windowed:
#   NW_SP_MISSION=03TR.bms NW_RESOURCE_DIR=<pff install> "$GODOT_BIN" --path godot \
#       -s res://tests/perf_fire_probe_game.gd
# The game runtime cannot mount flat extracts — NW_RESOURCE_DIR must be a PFF
# install. The persisted dir/expansion are snapshotted and restored on exit.

const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const MountGuard := preload("res://tests/perf_probe_mount_guard.gd")
const PropertyGuard := preload("res://tests/perf_probe_property_guard.gd")

var _mount_guard = MountGuard.new()
var _property_guard = PropertyGuard.new()
var _probe_state_error: Error = OK
var _requested_exit_code := 1
var _runtime = null
var _present = null
var _sim = null
var _effect_world = null
var _main = null
var _gw = null
var _hud_host = null
var _vprid := RID()
# Frame segmentation: [post_draw -> process_frame] = swap/present + OS pump +
# physics; [process_frame -> pre_draw] = the process step; [pre -> post] = draw.
var _seg_t_pf := 0
var _seg_t_pre := 0
var _seg_sum := [0, 0, 0]
var _seg_n := 0


func _on_seg_process_frame() -> void:
	var now := Time.get_ticks_usec()
	if _seg_t_pre > 0:
		_seg_sum[2] += now - _seg_t_pre  # post-draw -> next process_frame
		_seg_n += 1
	_seg_t_pf = now


func _on_seg_pre_draw() -> void:
	var now := Time.get_ticks_usec()
	if _seg_t_pf > 0:
		_seg_sum[0] += now - _seg_t_pf  # process step
	_seg_t_pre = now


func _on_seg_post_draw() -> void:
	var now := Time.get_ticks_usec()
	if _seg_t_pre > 0:
		_seg_sum[1] += now - _seg_t_pre  # draw
	_seg_t_pre = now
var _phase := ""
var _sample_t0 := 0


func _initialize() -> void:
	call_deferred("_run")


func _finalize() -> void:
	var state_err := _restore_probe_state()
	var mount_err := _restore_mount()
	if _requested_exit_code == 0 and (state_err != OK or mount_err != OK):
		_requested_exit_code = 1
		quit(1)


func _run() -> void:
	var bms := OS.get_environment("NW_SP_MISSION").strip_edges()
	if bms.is_empty():
		push_error("[pfg] set NW_SP_MISSION=<mission.bms>")
		_finish(1)
		return
	if _mount_guard.capture() != OK:
		push_error("[pfg] could not snapshot the shared mount config")
		_finish(1)
		return
	var res_dir := OS.get_environment("NW_RESOURCE_DIR").strip_edges()
	if not res_dir.is_empty():
		ResourceDirSettings.set_resource_dir(res_dir)
		ResourceDirSettings.set_expansion("")
	print("[pfg] mount: dir=%s expansion=%s" % [
			ResourceDirSettings.get_resource_dir(), ResourceDirSettings.get_expansion()])

	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		push_error("[pfg] failed to load main_game.tscn")
		_finish(1)
		return
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	if world == null:
		push_error("[pfg] main_game lacks World")
		_finish(1)
		return

	var wall_start := Time.get_ticks_msec()
	while not world.has_local_player():
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			push_error("[pfg] player never spawned (mission load stalled?)")
			_finish(1)
			return
	print("[pfg] mission=%s loaded, player spawned" % bms)
	await _settle_ms(5000)

	_runtime = _find_by_method(root, "tick_realtime")
	if _runtime != null and _runtime.has_method("get_sim"):
		_sim = _runtime.get_sim()
	if _runtime != null:
		_present = _runtime.get("_present")
	_effect_world = _find_by_method(root, "get_debug_group_report")
	_main = game
	_gw = world
	_hud_host = _main.get("_hud_host")
	_enable_perf_probe_spans()

	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	# Split the draw step: RS main-thread CPU vs GPU per frame.
	_vprid = root.get_viewport().get_viewport_rid()
	RenderingServer.viewport_set_measure_render_time(_vprid, true)
	process_frame.connect(_on_seg_process_frame)
	RenderingServer.frame_pre_draw.connect(_on_seg_pre_draw)
	RenderingServer.frame_post_draw.connect(_on_seg_post_draw)
	_census()
	_look(Vector2(0, 120))
	await _settle_ms(1500)

	var fire_s := float(OS.get_environment("NOVA_PF_FIRE_SECONDS").to_float())
	if fire_s <= 0.0:
		fire_s = 3.0

	var base := await _measure("baseline", 5000)
	if OS.get_environment("NOVA_PF_BASELINE_ONLY") == "1":
		_report("BASELINE", base)
		_finish(0)
		return
	_mouse_btn(MOUSE_BUTTON_LEFT, true)
	var fire1 := await _measure("firing1", int(fire_s * 1000.0))
	_mouse_btn(MOUSE_BUTTON_LEFT, false)
	_hold(KEY_R, true)
	await _settle_ms(120)
	_hold(KEY_R, false)
	await _settle_ms(2600)
	_mouse_btn(MOUSE_BUTTON_LEFT, true)
	var fire2 := await _measure("firing2", int(fire_s * 1000.0))
	_mouse_btn(MOUSE_BUTTON_LEFT, false)
	await _settle_ms(1500)
	var cool := await _measure("cooldown", 5000)

	# HUD-canvas A/B: hide the WHOLE HUD layer (the _ui_parent subtree — hiding
	# only the GameHud Control left sibling HUD elements visible). Ticks still
	# run — this isolates the _draw/canvas side from the info-build side.
	var hud_node: Node = null
	var hh = _main.get("_hud_host") if _main != null else null
	if hh != null:
		var parent = hh.get("_ui_parent")
		var gh = hh.get("_game_hud")
		if parent is Node and "visible" in parent:
			hud_node = parent
		elif gh is CanvasItem:
			hud_node = gh
	var hudoff := {avg = -1.0}
	if _set_guarded_if_present(hud_node, &"visible", false):
		hudoff = await _measure("hudoff", 3000)
		_restore_guarded(hud_node, &"visible")

	# Locate the water renderer for the hard reflection A/B below. The reflection
	# a second time each frame (its own camera — main-camera cull masks and the
	# root viewport's 3D scale never touch it).
	var water: Node = null
	var stack: Array = [root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		var scr = n.get_script()
		if scr != null and scr is Resource \
				and (scr as Resource).resource_path.ends_with("nova_water.gd"):
			water = n
			break
		for ch in n.get_children():
			stack.push_back(ch)
	# Split A/B: which half of the main-game frame callback drags the out-of-process cost
	# with it (deferred/RS-side work its calls generate)?
	var worldoff := {avg = -1.0}
	var hudtickoff := {avg = -1.0}
	if _set_guarded_if_present(_main, &"_perf_probe_skip_world", true):
		worldoff = await _measure("worldoff", 3000)
		_restore_guarded(_main, &"_perf_probe_skip_world")
	if _set_guarded_if_present(_main, &"_perf_probe_skip_hud", true):
		await _settle_ms(500)
		hudtickoff = await _measure("hudtickoff", 3000)
		_restore_guarded(_main, &"_perf_probe_skip_hud")

	# Existing presenter options provide state-safe A/Bs without a production
	# probe branch: freeze transform/visibility submission independently while
	# simulation, body posing, and muzzle feedback continue normally.
	var xformoff := {avg = -1.0}
	var visoff := {avg = -1.0}
	var bodyoff := {avg = -1.0}
	if _set_guarded_if_present(_present, &"_drive_transform", false):
		await _settle_ms(500)
		xformoff = await _measure("xformoff", 3000)
		_restore_guarded(_present, &"_drive_transform")
	if _set_guarded_if_present(_present, &"_drive_visibility", false):
		await _settle_ms(500)
		visoff = await _measure("visoff", 3000)
		_restore_guarded(_present, &"_drive_visibility")
	# Body-anim A/B: freeze the pose dispatch (and with it the Skeleton3D
	# update chain those writes dirty) — the deferred/off-span suspect the
	# WORLDOFF-minus-spans residual points at.
	if _set_guarded_if_present(_present, &"_drive_body_anim", false):
		await _settle_ms(500)
		bodyoff = await _measure("bodyoff", 3000)
		_restore_guarded(_present, &"_drive_body_anim")

	# Occlusion legs A/B (visibility writes across the occluded set per frame).
	var occloff := {avg = -1.0}
	var occlusion_setter := StringName()
	if _gw != null and _gw.has_method("set_perf_probe_occlusion_suspended"):
		occlusion_setter = &"set_perf_probe_occlusion_suspended"
	if _set_guarded_if_present(
			_gw, &"_perf_probe_skip_occl", true, occlusion_setter):
		await _settle_ms(500)
		occloff = await _measure("occloff", 3000)
		_restore_guarded(_gw, &"_perf_probe_skip_occl")

	# HARD reflection off: stop the water script first (it re-asserts the update
	# mode every frame — the earlier soft toggle was overwritten within a frame),
	# THEN disable the RTT.
	var reflhard := {avg = -1.0}
	if water != null:
		var rvp2 = water.get("reflection_viewport")
		if rvp2 is SubViewport:
			await _settle_ms(500)
			var process_guarded := _set_guarded_method(
					water, &"process_enabled", &"is_processing", &"set_process", false)
			var viewport_guarded := _set_guarded_if_present(
					rvp2, &"render_target_update_mode", SubViewport.UPDATE_DISABLED)
			if process_guarded and viewport_guarded:
				reflhard = await _measure("reflhardoff", 3000)
			if viewport_guarded:
				_restore_guarded(rvp2, &"render_target_update_mode")
			if process_guarded:
				_restore_guarded(water, &"process_enabled")

	# Particle fixed-tick A/B (advance_fixed_tick runs per 62 Hz tick — 8-9x per
	# frame at low FPS).
	var fxtickoff := {avg = -1.0}
	if _set_guarded_if_present(_gw, &"_perf_probe_skip_effect_tick", true):
		await _settle_ms(500)
		fxtickoff = await _measure("fxtickoff", 3000)
		_restore_guarded(_gw, &"_perf_probe_skip_effect_tick")

	# The direct simulation/presentation timing counters remain observational;
	# skipping either bundle would mutate gameplay state and corrupt later legs.
	var handleroff := {avg = -1.0}
	if _set_guarded_if_present(_gw, &"_perf_probe_skip_fixed_handlers", true):
		await _settle_ms(500)
		handleroff = await _measure("handleroff", 3000)
		_restore_guarded(_gw, &"_perf_probe_skip_fixed_handlers")

	# Residual bisect, LAST because it broadly mutates processing state: turn
	# off every OTHER node's _process (main_game keeps ticking the world). If
	# the frame collapses toward the measured shell spans, the process residual
	# is node _process work (bisect by subtree next); if it barely moves, the
	# residual is engine-internal.
	var otherprocoff := {avg = -1.0}
	var process_disabled: Array = []
	var walk: Array = [root]
	while not walk.is_empty():
		var walk_node: Node = walk.pop_back()
		for walk_child in walk_node.get_children():
			walk.push_back(walk_child)
		if walk_node == _main or walk_node == root:
			continue
		if walk_node.is_processing():
			walk_node.set_process(false)
			process_disabled.append(walk_node)
	print("[pfg] otherprocoff: disabled _process on %d node(s)" % process_disabled.size())
	await _settle_ms(500)
	otherprocoff = await _measure("otherprocoff", 3000)
	for restored_node in process_disabled:
		if is_instance_valid(restored_node):
			(restored_node as Node).set_process(true)

	_report("BASELINE", base)
	_report("FIRING1 ", fire1)
	_report("FIRING2 ", fire2)
	_report("COOLDOWN", cool)
	if float(hudoff.avg) >= 0.0:
		print("[pfg] HUDOFF   avg=%.2fms (canvas share vs cooldown: %+.2fms)" % [
				float(hudoff.avg), float(cool.avg) - float(hudoff.avg)])
	if float(worldoff.avg) >= 0.0:
		print("[pfg] WORLDOFF avg=%.2fms (world.tick share vs cooldown: %+.2fms)" % [
				float(worldoff.avg), float(cool.avg) - float(worldoff.avg)])
	if float(hudtickoff.avg) >= 0.0:
		print("[pfg] HUDTICKOFF avg=%.2fms (hud tick share vs cooldown: %+.2fms)" % [
				float(hudtickoff.avg), float(cool.avg) - float(hudtickoff.avg)])
	if float(xformoff.avg) >= 0.0:
		print("[pfg] XFORMOFF avg=%.2fms (present transform share vs cooldown: %+.2fms)" % [
				float(xformoff.avg), float(cool.avg) - float(xformoff.avg)])
	if float(visoff.avg) >= 0.0:
		print("[pfg] VISOFF   avg=%.2fms (present visibility share vs cooldown: %+.2fms)" % [
				float(visoff.avg), float(cool.avg) - float(visoff.avg)])
	if float(bodyoff.avg) >= 0.0:
		print("[pfg] BODYOFF  avg=%.2fms (body-anim/skeleton share vs cooldown: %+.2fms)" % [
				float(bodyoff.avg), float(cool.avg) - float(bodyoff.avg)])
	if float(occloff.avg) >= 0.0:
		print("[pfg] OCCLOFF  avg=%.2fms (occlusion share vs cooldown: %+.2fms)" % [
				float(occloff.avg), float(cool.avg) - float(occloff.avg)])
	if float(reflhard.avg) >= 0.0:
		print("[pfg] REFLHARDOFF avg=%.2fms (hard reflection share vs cooldown: %+.2fms)" % [
				float(reflhard.avg), float(cool.avg) - float(reflhard.avg)])
	if float(fxtickoff.avg) >= 0.0:
		print("[pfg] FXTICKOFF avg=%.2fms (particle tick share vs cooldown: %+.2fms)" % [
				float(fxtickoff.avg), float(cool.avg) - float(fxtickoff.avg)])
	if float(handleroff.avg) >= 0.0:
		print("[pfg] HANDLEROFF avg=%.2fms (fixed handlers share vs cooldown: %+.2fms)" % [
				float(handleroff.avg), float(cool.avg) - float(handleroff.avg)])
	if float(otherprocoff.avg) >= 0.0:
		print("[pfg] OTHERPROCOFF avg=%.2fms (other nodes' _process share vs cooldown: %+.2fms)" % [
				float(otherprocoff.avg), float(cool.avg) - float(otherprocoff.avg)])
	var base_avg: float = base.avg
	var base_p95: float = base.p95
	var cool_avg: float = cool.avg
	var fire_avg: float = maxf(fire1.avg, fire2.avg)
	var fire_p95: float = maxf(fire1.p95, fire2.p95)
	var regressed: bool = (fire_avg > base_avg * 1.5 + 1.0) or (fire_p95 > base_p95 * 2.0 + 2.0)
	var leaky: bool = cool_avg > base_avg * 1.3 + 1.0
	print("[pfg] deltas: fire_avg %.2fx base | fire_p95 %.2fx base | cooldown %.2fx base" % [
			fire_avg / maxf(base_avg, 0.001), fire_p95 / maxf(base_p95, 0.001),
			cool_avg / maxf(base_avg, 0.001)])
	if leaky:
		print("[pfg] LEAK SUSPECT: cooldown never returned to baseline")
	print("[pfg] VERDICT: %s" % ("REGRESSION — firing is markedly slower than baseline"
			if regressed else "GREEN — firing within baseline envelope"))
	var exit_code := 2 if regressed else 0
	_finish(exit_code)


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
			# Do not charge the probe's own diagnostic pulls to the next frame.
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
	var parts := "-"
	if _effect_world != null:
		var groups: Array = _effect_world.get_debug_group_report()
		var alive := 0
		for g_v in groups:
			for e_v in (g_v as Dictionary).get("emitters", []):
				alive += int((e_v as Dictionary).get("alive", 0))
		parts = "%d/%d" % [groups.size(), alive]
	var spans := ""
	var world_producer_skipped := false
	var hud_producer_skipped := false
	if _main != null:
		if _property_guard.has_property(_main, &"_perf_probe_skip_world"):
			world_producer_skipped = bool(_main.get("_perf_probe_skip_world"))
		if _property_guard.has_property(_main, &"_perf_probe_skip_hud"):
			hud_producer_skipped = bool(_main.get("_perf_probe_skip_hud"))
		var mg = _main.get("_perf_probe_spans")
		if mg is Dictionary and not (mg as Dictionary).is_empty():
			spans += " main{before=%.1f world=%.1f after=%.1f hud=%.1f}" % [
					float(mg.get("before", 0)) / 1000.0, float(mg.get("world", 0)) / 1000.0,
					float(mg.get("after", 0)) / 1000.0, float(mg.get("hud", 0)) / 1000.0]
		if _hud_host != null and not hud_producer_skipped:
			var hg = _hud_host.get("_perf_probe_spans")
			if hg is Dictionary and not (hg as Dictionary).is_empty():
				spans += " hud{scal=%.1f attach=%.1f wp=%.1f info=%.1f flush=%.1f}" % [
						float(hg.get("scalars", 0)) / 1000.0,
						float(hg.get("attach", 0)) / 1000.0,
						float(hg.get("waypoint", 0)) / 1000.0,
						float(hg.get("update_info", 0)) / 1000.0,
						float(hg.get("flush", 0)) / 1000.0]
	if _gw != null and not world_producer_skipped:
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
	return ("[pfg] %s t+%4.1fs fps=%6.1f proc=%5.2fms phys=%5.2fms %s draws=%5d objs=%5d prims=%8d nodes=%5d orphans=%4d parts=%s trails=%s%s" % [
			_phase, t, fps,
			Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0,
			Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1000.0,
			rt,
			int(Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)),
			int(Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME)),
			int(Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME)),
			int(Performance.get_monitor(Performance.OBJECT_NODE_COUNT)),
			int(Performance.get_monitor(Performance.OBJECT_ORPHAN_NODE_COUNT)),
			parts, trails, spans + rmeas])


func _census() -> void:
	var counts := {}
	var stack: Array = [root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		for cls in ["MeshInstance3D", "MultiMeshInstance3D", "GPUParticles3D",
				"CPUParticles3D", "Camera3D", "AnimationPlayer", "AudioStreamPlayer3D"]:
			if n.is_class(cls):
				counts[cls] = int(counts.get(cls, 0)) + 1
		for ch in n.get_children():
			stack.push_back(ch)
	print("[pfg] census: ", counts)


func _report(label: String, st: Dictionary) -> void:
	print("[pfg] %s frames=%5d avg=%6.2fms p50=%6.2fms p95=%6.2fms max=%7.2fms worst=[%s]" % [
			label, st.n, st.avg, st.p50, st.p95, st.mx, ", ".join(st.worst)])


func _enable_perf_probe_spans() -> void:
	for target_v in [_main, _hud_host, _gw]:
		var target := target_v as Object
		var setter_method := StringName()
		if is_instance_valid(target) and target.has_method("set_perf_probe_enabled"):
			setter_method = &"set_perf_probe_enabled"
		_set_guarded_if_present(
				target, &"_perf_probe_enabled", true, setter_method)


func _set_guarded_if_present(
		target: Object,
		property_name: StringName,
		value: Variant,
		setter_method: StringName = StringName()) -> bool:
	if not _property_guard.has_property(target, property_name):
		return false
	var err: Error = _property_guard.set_temporary(
			target, property_name, value, setter_method)
	if err != OK:
		_record_probe_state_error(err, "set %s" % property_name)
		return false
	return true


func _set_guarded_method(
		target: Object,
		state_key: StringName,
		getter_method: StringName,
		setter_method: StringName,
		value: Variant) -> bool:
	var err: Error = _property_guard.set_temporary_method(
			target, state_key, getter_method, setter_method, value)
	if err != OK:
		_record_probe_state_error(err, "set %s" % state_key)
		return false
	return true


func _restore_guarded(target: Object, property_name: StringName) -> bool:
	var err: Error = _property_guard.restore_property(target, property_name)
	if err != OK:
		_record_probe_state_error(err, "restore %s" % property_name)
		return false
	return true


func _restore_probe_state() -> Error:
	var err: Error = _property_guard.restore_all()
	if err != OK:
		_record_probe_state_error(err, "restore remaining probe properties")
	return _probe_state_error


func _record_probe_state_error(err: Error, operation: String) -> void:
	if err == OK:
		return
	if _probe_state_error == OK:
		_probe_state_error = err
	push_error("[pfg] failed to %s (error %d)" % [operation, err])


func _finish(exit_code: int) -> void:
	var state_err := _restore_probe_state()
	var mount_err := _restore_mount()
	if exit_code == 0 and (state_err != OK or mount_err != OK):
		exit_code = 1
	_requested_exit_code = exit_code
	quit(exit_code)


func _restore_mount() -> Error:
	var err: Error = _mount_guard.restore()
	if err != OK:
		push_error("[pfg] failed to restore the shared mount config (error %d)" % err)
	return err


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
		await process_frame


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
