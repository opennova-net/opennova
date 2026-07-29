extends SceneTree

# Frame-cost decomposition sweep (game shell): boots main_game.tscn on
# NW_SP_MISSION like perf_fire_probe_game.gd, then — instead of firing —
# enumerates every processing group (nodes sharing a script, or a native class
# for scriptless nodes) with _process/_physics_process enabled, disables one
# group at a time, and measures the frame-time saving. Two render-side phases
# (3D at half resolution scale, then cull mask 0) bound the GPU share. Output
# is a sorted savings table — the owners of the idle frame cost, measured.
#   NW_SP_MISSION=03TR.bms NW_RESOURCE_DIR=<pff install> "$GODOT_BIN" --path godot \
#       -s res://tests/perf_sweep_probe_game.gd

const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const PHASE_MS := 2600
const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const MountGuard := preload("res://tests/perf_probe_mount_guard.gd")
const ProcessGuard := preload("res://tests/perf_probe_process_guard.gd")

var _mount_guard = MountGuard.new()


func _initialize() -> void:
	call_deferred("_run")


func _finalize() -> void:
	_restore_mount()


func _run() -> void:
	var bms := OS.get_environment("NW_SP_MISSION").strip_edges()
	if bms.is_empty():
		push_error("[sw] set NW_SP_MISSION=<mission.bms>")
		quit(1)
		return
	if _mount_guard.capture() != OK:
		push_error("[sw] could not snapshot the shared mount config")
		quit(1)
		return
	var res_dir := OS.get_environment("NW_RESOURCE_DIR").strip_edges()
	if not res_dir.is_empty():
		ResourceDirSettings.set_resource_dir(res_dir)
		ResourceDirSettings.set_expansion("")
	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		push_error("[sw] failed to load main_game.tscn")
		_restore_mount()
		quit(1)
		return
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	if world == null:
		push_error("[sw] main_game lacks World")
		_restore_mount()
		quit(1)
		return
	var wall_start := Time.get_ticks_msec()
	while not (world.get_sim() != null and world.get_sim().has_local_player()):
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			push_error("[sw] player never spawned")
			_restore_mount()
			quit(1)
			return
	print("[sw] mission=%s loaded" % bms)
	await _settle_ms(5000)
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	await _settle_ms(1000)

	# --- Enumerate processing groups. ---
	var groups: Dictionary = {}  # key -> {nodes: [], phys: int, proc: int}
	var stack: Array = [root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		for ch in n.get_children():
			stack.push_back(ch)
		# No has_method filter: native nodes (e.g. the particle renderer) run on
		# NOTIFICATION_PROCESS without exposing a script _process at all.
		var has_proc := n.is_processing()
		var has_phys := n.is_physics_processing()
		if not has_proc and not has_phys:
			continue
		var scr = n.get_script()
		var key := ""
		if scr != null and scr is Resource:
			key = (scr as Resource).resource_path.get_file()
		else:
			key = "<native> " + n.get_class()
		if not groups.has(key):
			groups[key] = {nodes = [], phys = 0, proc = 0}
		(groups[key].nodes as Array).append(n)
		if has_proc:
			groups[key].proc += 1
		if has_phys:
			groups[key].phys += 1
	print("[sw] %d processing groups:" % groups.size())
	for key in groups.keys():
		print("[sw]   %-46s nodes=%-3d proc=%d phys=%d" % [
				key, (groups[key].nodes as Array).size(), groups[key].proc,
				groups[key].phys])

	# --- Baseline. ---
	var base := await _measure_ms(PHASE_MS * 2)
	print("[sw] BASELINE avg=%.2fms (n=%d)" % [base.avg, base.n])

	# --- Toggle sweep. ---
	var results: Array = []
	for key in groups.keys():
		var nodes: Array = groups[key].nodes
		# Processing modes can legitimately change while earlier groups are being
		# measured. Snapshot this group's live state immediately before its phase,
		# not during the minutes-earlier census.
		var node_states := ProcessGuard.disable_processing(nodes)
		var st := await _measure_ms(PHASE_MS)
		ProcessGuard.restore_processing(node_states)
		var saved: float = base.avg - st.avg
		results.append({key = key, avg = st.avg, saved = saved})
		print("[sw] off:%-46s avg=%7.2fms saved=%+7.2fms" % [key, st.avg, saved])
		await _settle_ms(300)

	# --- Render-side bounds. ---
	var vp := root.get_viewport()
	var prev_scale := vp.scaling_3d_scale
	vp.scaling_3d_scale = 0.5
	var half := await _measure_ms(PHASE_MS)
	vp.scaling_3d_scale = prev_scale
	print("[sw] render: 3d_scale=0.5 avg=%.2fms saved=%+.2fms vs base" % [
			half.avg, base.avg - half.avg])
	var cam := vp.get_camera_3d()
	var prev_mask := 0
	if cam != null:
		prev_mask = cam.cull_mask
		cam.cull_mask = 0
	var culled := await _measure_ms(PHASE_MS)
	if cam != null:
		cam.cull_mask = prev_mask
	print("[sw] render: cull_mask=0 avg=%.2fms saved=%+.2fms vs base" % [
			culled.avg, base.avg - culled.avg])

	# --- Sorted verdict table. ---
	results.sort_custom(func(a, b): return float(a.saved) > float(b.saved))
	print("[sw] ===== savings, largest first =====")
	for r_v in results:
		var r: Dictionary = r_v
		if float(r.saved) > 2.0:
			print("[sw]   %-46s saved=%+8.2fms -> avg %.2fms" % [r.key, r.saved, r.avg])
	print("[sw] baseline %.2fms | half-res saves %.2fms | cull-all saves %.2fms" % [
			base.avg, base.avg - half.avg, base.avg - culled.avg])
	var restore_err := _restore_mount()
	quit(1 if restore_err != OK else 0)


func _measure_ms(duration_ms: int) -> Dictionary:
	var samples: Array[float] = []
	var deadline := Time.get_ticks_msec() + duration_ms
	var last := Time.get_ticks_usec()
	# One throwaway frame so a just-restored group's first tick doesn't pollute.
	await process_frame
	last = Time.get_ticks_usec()
	while Time.get_ticks_msec() < deadline:
		await process_frame
		var now := Time.get_ticks_usec()
		samples.append(float(now - last) / 1000.0)
		last = now
	var n := samples.size()
	if n == 0:
		return {avg = 0.0, n = 0}
	var sum := 0.0
	for v in samples:
		sum += v
	return {avg = sum / n, n = n}


func _restore_mount() -> Error:
	var err: Error = _mount_guard.restore()
	if err != OK:
		push_error("[sw] failed to restore the shared mount config (error %d)" % err)
	return err


func _settle_ms(ms: int) -> void:
	var deadline := Time.get_ticks_msec() + ms
	while Time.get_ticks_msec() < deadline:
		await process_frame
