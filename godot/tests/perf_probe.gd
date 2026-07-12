extends Node

# Runtime perf triage probe: boots ONED play-in-editor on a mission, settles, then
# samples FPS + the world's per-subsystem perf counters for a few seconds and prints
# a ranked budget. NOVA_RESOURCE_DIR + NOVA_MISSION_BMS select the scene.

const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const EditorScene := preload("res://modtools/editor/editor_main.tscn")


func _ready() -> void:
	var root := OS.get_environment("NOVA_RESOURCE_DIR").strip_edges()
	if root.is_empty():
		root = ResourceDirSettings.get_resource_dir()
	ResourceDirSettings.set_resource_dir(root)
	var app = EditorScene.instantiate()
	add_child(app)
	for _i in 8:
		await get_tree().process_frame
	var ws_station = app.workstation
	ws_station.set_resource_root_dir(root)
	await _settle(6)
	var bms := OS.get_environment("NOVA_MISSION_BMS").strip_edges()
	if bms.is_empty():
		bms = "00TRa.bms"
	var ws = ws_station.get_workspace_adapter(EditorWorkstation.Workspace.MISSION)
	if ws.open_file(NovaPaths.resolve_file(root, bms)) != OK:
		push_error("[perf] open failed")
		get_tree().quit(1)
		return
	ws_station.set_active_workspace(EditorWorkstation.Workspace.MISSION)
	await _settle(30)
	if int(ws.play_mission()) != OK:
		push_error("[perf] play failed")
		get_tree().quit(1)
		return
	await _settle(240)  # let load spikes drain

	var world = ws._play_node().get_world()
	var frames := 0
	var t0 := Time.get_ticks_msec()
	var worst_ms := 0.0
	while Time.get_ticks_msec() - t0 < 5000:
		var f0 := Time.get_ticks_usec()
		await get_tree().process_frame
		worst_ms = maxf(worst_ms, float(Time.get_ticks_usec() - f0) / 1000.0)
		frames += 1
	var secs := float(Time.get_ticks_msec() - t0) / 1000.0
	print("[perf] fps=%.1f frames=%d worst_frame=%.1f ms" % [frames / secs, frames, worst_ms])
	print("[perf] counters: ", JSON.stringify(world.get_runtime_perf_counters()
			if world != null and world.has_method("get_runtime_perf_counters") else {}))
	print("[perf] render: objects=%d primitives=%d draw_calls=%d" % [
		Performance.get_monitor(Performance.RENDER_TOTAL_OBJECTS_IN_FRAME),
		Performance.get_monitor(Performance.RENDER_TOTAL_PRIMITIVES_IN_FRAME),
		Performance.get_monitor(Performance.RENDER_TOTAL_DRAW_CALLS_IN_FRAME)])
	print("[perf] process=%.2fms physics=%.2fms" % [
		Performance.get_monitor(Performance.TIME_PROCESS) * 1000.0,
		Performance.get_monitor(Performance.TIME_PHYSICS_PROCESS) * 1000.0])
	ws.stop_play_mission()
	get_tree().quit(0)


func _settle(frames: int) -> void:
	for _i in frames:
		await get_tree().process_frame
