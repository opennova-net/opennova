extends SceneTree
# Headless boot smoke for a resource root: mounts NW_RESOURCE_DIR, boots
# main_game.tscn straight into NW_SP_MISSION, waits for the local player, then
# runs a few seconds of mission time so the HUD compiler, the label fonts and
# every per-frame presenter execute once, and exits 0. Written for the bundled
# minimal game (assets/, loose -> pass `-- --loose-root`) but any root works:
#   NW_SP_MISSION=mnml.bms NW_RESOURCE_DIR=<repo>/assets "$GODOT_BIN" --headless \
#       --path godot -s res://tests/minimal_boot_probe.gd -- --loose-root
const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const SETTLE_MISSION_SECONDS := 3.0


func _initialize() -> void:
	call_deferred("_run")


func _fail(msg: String) -> void:
	push_error("minimal_boot_probe: " + msg)
	quit(1)


func _run() -> void:
	var bms := OS.get_environment("NW_SP_MISSION").strip_edges()
	var res_dir := OS.get_environment("NW_RESOURCE_DIR").strip_edges()
	if bms.is_empty() or res_dir.is_empty():
		_fail("set NW_SP_MISSION=<mission.bms> and NW_RESOURCE_DIR=<root>")
		return
	var settings := load("res://game/resource_index/resource_dir_settings.gd")
	settings.set_resource_dir(res_dir)
	settings.set_expansion("")
	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		_fail("failed to load main_game.tscn")
		return
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	if world == null:
		_fail("main_game lacks a World child")
		return
	var wall_start := Time.get_ticks_msec()
	while not (world.get_sim() != null and world.get_sim().has_local_player()):
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			_fail("player never spawned (mission load stalled?)")
			return
	var settle_start := Time.get_ticks_msec()
	while float(Time.get_ticks_msec() - settle_start) / 1000.0 < SETTLE_MISSION_SECONDS:
		await process_frame
	print("PROBE PASS: %s booted from %s and ran %.1f s" % [bms, res_dir, SETTLE_MISSION_SECONDS])
	quit(0)
