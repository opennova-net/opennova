extends SceneTree

# F3 Stats acceptance probe: boots main_game.tscn into an SP mission, opens
# the debug overlay onto the Stats tab (the real F3 path), lets the capture
# window fill, and dumps every Stats row twice — the second reading also
# demonstrates that two consecutive window means agree. Exit 0 when the
# load-bearing rows (world tick, sim step, present, occlusion apply, HUD)
# carry real numbers. Windowed (the render rows need a live RenderingDevice):
#   NW_SP_MISSION=ASH_I5A.bms NW_RESOURCE_DIR=<pff install> "$GODOT_BIN" --path godot \
#       -s res://tests/frame_stats_probe_game.gd
# The persisted dir/expansion are snapshotted and restored on exit.

const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")
const MountGuard := preload("res://tests/perf_probe_mount_guard.gd")

const REQUIRED_ROWS := ["world", "sim", "present", "occl_apply", "hud"]

var _mount_guard = MountGuard.new()
var _requested_exit_code := 1


func _initialize() -> void:
	call_deferred("_run")


func _finalize() -> void:
	if _mount_guard.restore() != OK and _requested_exit_code == 0:
		quit(1)


func _run() -> void:
	var bms := OS.get_environment("NW_SP_MISSION").strip_edges()
	if bms.is_empty():
		push_error("[fsp] set NW_SP_MISSION=<mission.bms>")
		_finish(1)
		return
	if _mount_guard.capture() != OK:
		push_error("[fsp] could not snapshot the shared mount config")
		_finish(1)
		return
	var res_dir := OS.get_environment("NW_RESOURCE_DIR").strip_edges()
	if not res_dir.is_empty():
		ResourceDirSettings.set_resource_dir(res_dir)
		ResourceDirSettings.set_expansion("")

	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		push_error("[fsp] failed to load main_game.tscn")
		_finish(1)
		return
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	if world == null:
		push_error("[fsp] main_game lacks World")
		_finish(1)
		return

	var wall_start := Time.get_ticks_msec()
	while not world.has_local_player():
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			push_error("[fsp] player never spawned (mission load stalled?)")
			_finish(1)
			return
	print("[fsp] mission=%s loaded, player spawned" % bms)
	await _settle_ms(3000)

	# The real F3 path: summon the overlay, switch to the Stats tab.
	game.toggle_debug_overlay()
	var overlay = game.get("_debug_overlay")
	if overlay == null:
		push_error("[fsp] no debug overlay after toggle")
		_finish(1)
		return
	var tabs: TabContainer = overlay.get("_tabs")
	var pane = tabs.get_node_or_null("Stats")
	if pane == null:
		push_error("[fsp] overlay carries no Stats tab")
		_finish(1)
		return
	tabs.current_tab = (pane as Control).get_index()
	await _settle_ms(500)
	if not bool(pane.is_capturing()):
		push_error("[fsp] the Stats tab did not open its capture window")
		_finish(1)
		return

	await _settle_ms(2000)
	var first := _snapshot_rows(pane)
	_dump("READING 1", first)
	await _settle_ms(2000)
	var second := _snapshot_rows(pane)
	_dump("READING 2", second)

	var missing := PackedStringArray()
	for id in REQUIRED_ROWS:
		if String(second.get(id, ["-", "-"])[0]) == "-":
			missing.append(id)
	var world_a := _row_ms(first, "world")
	var world_b := _row_ms(second, "world")
	if world_a > 0.0 and world_b > 0.0:
		print("[fsp] consecutive world-tick readings: %.2f vs %.2f ms (delta %.2f)" % [
				world_a, world_b, absf(world_a - world_b)])
	if missing.is_empty():
		print("[fsp] VERDICT: GREEN — every required Stats row carries live numbers")
		_finish(0)
	else:
		print("[fsp] VERDICT: MISSING rows without numbers: %s" % ", ".join(missing))
		_finish(2)


func _snapshot_rows(pane) -> Dictionary:
	var out := {}
	var items: Dictionary = pane.get("_items")
	for id in items:
		var item: TreeItem = items[id]
		out[id] = [item.get_text(1), item.get_text(2), item.get_text(3)]
	return out


func _row_ms(rows: Dictionary, id: String) -> float:
	var avg := String(rows.get(id, ["-"])[0])
	return avg.to_float() if avg != "-" else -1.0


func _dump(label: String, rows: Dictionary) -> void:
	print("[fsp] ---- %s ----" % label)
	for id in ["frame", "before", "world", "foliage", "sim", "net", "effects",
			"present", "snapshot", "mission_rows", "wire_rows", "fire",
			"destruction", "throwable", "occl", "occl_build",
			"occl_probe", "occl_apply", "env", "audio", "after", "hud",
			"hud_scalars", "hud_attach", "hud_waypoint", "hud_info", "hud_flush",
			"render", "render_root_cpu", "render_root_gpu", "render_water_cpu",
			"render_water_gpu"]:
		var row: Array = rows.get(id, ["-", "-", ""])
		print("[fsp] %-16s avg=%-8s max=%-8s %s" % [id, row[0], row[1], row[2]])


func _finish(exit_code: int) -> void:
	if _mount_guard.restore() != OK and exit_code == 0:
		exit_code = 1
	_requested_exit_code = exit_code
	quit(exit_code)


func _settle_ms(ms: int) -> void:
	var deadline := Time.get_ticks_msec() + ms
	while Time.get_ticks_msec() < deadline:
		await process_frame
