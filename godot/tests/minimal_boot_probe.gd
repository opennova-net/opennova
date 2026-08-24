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
	# The SP listen server auto-spawns through the retail marker chain keyed on the mission's own
	# game-type word; a wrong word finds no start and parks the player at the origin
	# (the post-#564 regression). Pin the spawn to the authored 6001 player start.
	var sim = world.get_sim()
	var spawn_pos: Vector3 = sim.get_local_player_position()
	var mission := MissionData.new()
	if mission.open_from_resource_root(world.get_resource_root(), bms) != OK:
		_fail("could not re-open %s to read its player start" % bms)
		return
	# The word itself: the SP listen server seeds g_GameType from the mission's attrib mode (no
	# multiplayer bit -> stock Co-op 0x10020), never the GameConfig default.
	var expected_game_type := int(NetProtocol.game_type_for_mission_mode(int(mission.get_game_mode())))
	var session_game_type := int(sim.get_session_game_type())
	if session_game_type != expected_game_type:
		_fail("session game type 0x%X, expected the mission-derived 0x%X" % [
				session_game_type, expected_game_type])
		return
	var start_marker := Vector3.INF
	for row in mission.get_all_entities():
		if int(row.get("type_id", 0)) == 6001:
			start_marker = MissionObjectPlacer.bms_to_godot_position(row.get("position", Vector3.ZERO))
			break
	if start_marker == Vector3.INF:
		_fail("%s authors no 6001 player start" % bms)
		return
	var start_xz := Vector2(start_marker.x, start_marker.z)
	var spawn_xz := Vector2(spawn_pos.x, spawn_pos.z)
	if start_xz.distance_to(spawn_xz) > 1.0:
		_fail("player spawned at %s, not the 6001 player start at %s" % [spawn_pos, start_marker])
		return
	var settle_start := Time.get_ticks_msec()
	while float(Time.get_ticks_msec() - settle_start) / 1000.0 < SETTLE_MISSION_SECONDS:
		await process_frame
	print("PROBE PASS: %s booted from %s, spawned at %s (6001 start %s) and ran %.1f s" % [
			bms, res_dir, spawn_pos, start_marker, SETTLE_MISSION_SECONDS])
	quit(0)
