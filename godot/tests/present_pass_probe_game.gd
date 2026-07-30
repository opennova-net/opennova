extends SceneTree

# Manual probe: boots main_game.tscn through the NW_SP_MISSION single-player
# start flow (same recipe as ambient_audio_probe_game.gd), warms up, then samples
# the runtime's PRESENT leg for a fixed window — the native-row-walk A/B meter.
# Reports avg/p95/max of the runtime perf counters' present_us (the entity-row
# walk + fire/destruction/throwable passes on tick frames) plus sim_us and frame
# wall time. Windowed:
#   NW_SP_MISSION=ASH_I5A.bms NW_RESOURCE_DIR=<pff install> "$GODOT_BIN" \
#       --path godot -s res://tests/present_pass_probe_game.gd

const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const WARM_MS := 6000
const SAMPLE_MS := 10000
const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const MountGuard := preload("res://tests/perf_probe_mount_guard.gd")

var _mount_guard = MountGuard.new()
var _requested_exit_code := 1
var _world = null
var _runtime = null
var _sampling := false
var _present_us: Array[int] = []
var _sim_us: Array[int] = []
var _frame_us: Array[int] = []
var _last_frame_t := 0


func _initialize() -> void:
	call_deferred("_run")


func _finalize() -> void:
	if _mount_guard.restore() != OK and _requested_exit_code == 0:
		quit(1)


func _finish(code: int) -> void:
	_requested_exit_code = code
	quit(code)


func _find_by_method(node: Node, method: String) -> Node:
	if node.has_method(method):
		return node
	for child in node.get_children():
		var found := _find_by_method(child, method)
		if found != null:
			return found
	return null


func _on_frame() -> void:
	if not _sampling:
		return
	var now := Time.get_ticks_usec()
	if _last_frame_t > 0:
		_frame_us.append(now - _last_frame_t)
	_last_frame_t = now
	if _runtime != null and _runtime.has_method("get_perf_counters"):
		var counters: Dictionary = _runtime.get_perf_counters()
		_present_us.append(int(counters.get("present_us", 0)))
		_sim_us.append(int(counters.get("sim_us", 0)))


func _settle_ms(ms: int) -> void:
	var t0 := Time.get_ticks_msec()
	while Time.get_ticks_msec() - t0 < ms:
		await process_frame


static func _avg(values: Array[int]) -> float:
	if values.is_empty():
		return 0.0
	var total := 0
	for v in values:
		total += v
	return float(total) / float(values.size())


static func _pct(values: Array[int], p: float) -> int:
	if values.is_empty():
		return 0
	var sorted := values.duplicate()
	sorted.sort()
	return sorted[mini(int(float(sorted.size() - 1) * p), sorted.size() - 1)]


func _run() -> void:
	var bms := OS.get_environment("NW_SP_MISSION").strip_edges()
	if bms.is_empty():
		push_error("[ppp] set NW_SP_MISSION=<mission.bms>")
		_finish(1)
		return
	if _mount_guard.capture() != OK:
		push_error("[ppp] could not snapshot the shared mount config")
		_finish(1)
		return
	var res_dir := OS.get_environment("NW_RESOURCE_DIR").strip_edges()
	if not res_dir.is_empty():
		ResourceDirSettings.set_resource_dir(res_dir)
		ResourceDirSettings.set_expansion(
				OS.get_environment("NW_EXPANSION").strip_edges())
	print("[ppp] mount: dir=%s expansion=%s" % [
			ResourceDirSettings.get_resource_dir(), ResourceDirSettings.get_expansion()])

	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		push_error("[ppp] failed to load main_game.tscn")
		_finish(1)
		return
	var game := packed.instantiate()
	root.add_child(game)
	_world = game.get_node_or_null("World")
	if _world == null:
		push_error("[ppp] main_game lacks World")
		_finish(1)
		return

	var wall_start := Time.get_ticks_msec()
	while not (_world.get_sim() != null and _world.get_sim().has_local_player()):
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			push_error("[ppp] player never spawned (mission load stalled?)")
			_finish(1)
			return
	print("[ppp] mission=%s loaded, player spawned" % bms)

	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0
	_runtime = _find_by_method(root, "tick_realtime")
	if _runtime == null:
		push_error("[ppp] no mission runtime found")
		_finish(1)
		return
	process_frame.connect(_on_frame)

	await _settle_ms(WARM_MS)
	_sampling = true
	_last_frame_t = 0
	await _settle_ms(SAMPLE_MS)
	_sampling = false

	print("[ppp] frames=%d window=%.1fs" % [_present_us.size(), float(SAMPLE_MS) / 1000.0])
	print("[ppp] PRESENT leg (runtime present_us): avg=%.1fus p95=%dus max=%dus" % [
			_avg(_present_us), _pct(_present_us, 0.95),
			_present_us.max() if not _present_us.is_empty() else 0])
	print("[ppp] sim_us: avg=%.1fus p95=%dus" % [
			_avg(_sim_us), _pct(_sim_us, 0.95)])
	print("[ppp] frame wall: avg=%.2fms (%.0f fps)" % [
			_avg(_frame_us) / 1000.0,
			1000000.0 / _avg(_frame_us) if _avg(_frame_us) > 0.0 else 0.0])
	_finish(0)
