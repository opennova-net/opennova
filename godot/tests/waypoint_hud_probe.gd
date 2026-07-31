extends SceneTree

# Waypoint-HUD probe — NOT a GUT test (needs a retail PFF install; *_probe.gd
# files are manual, never collected). Boots the real game shell on an SP
# mission, waits for the spawn, and verifies the waypoint chain end to end:
# the sim's waypoint track (list built from the mission's blue route), the
# presenter's HUD info entry (resolved WPNames name + live distance), and the
# proximity advance after teleporting the player onto the current waypoint.
# PASSes only when the label data is live and the advance fires; a mission
# with NO blue route reports that and exits 0 (nothing to probe).
#
#   NW_SP_MISSION=CP01.bms NW_RESOURCE_DIR=<retail install> \
#       "$GODOT_BIN" --headless --path godot -s res://tests/waypoint_hud_probe.gd

const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const TIME_SCALE := 2.0
const SETTLE_MISSION_SECONDS := 2.0


func _initialize() -> void:
	call_deferred("_run")


func _mission_wait(seconds: float) -> void:
	var t0 := Time.get_ticks_msec()
	while float(Time.get_ticks_msec() - t0) * TIME_SCALE < seconds * 1000.0:
		await process_frame


func _run() -> void:
	if OS.get_environment("NW_SP_MISSION").is_empty():
		push_error("waypoint_hud_probe: set NW_SP_MISSION=<mission.bms>")
		quit(1)
		return
	var res_dir := OS.get_environment("NW_RESOURCE_DIR")
	if not res_dir.is_empty():
		var settings := load("res://engine/resource_index/resource_dir_settings.gd")
		settings.set_resource_dir(res_dir)
		settings.set_expansion("")
	Engine.time_scale = TIME_SCALE
	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		push_error("waypoint_hud_probe: failed to load main_game.tscn")
		quit(1)
		return
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	if world == null:
		push_error("waypoint_hud_probe: main_game lacks a World child")
		quit(1)
		return

	var wall_start := Time.get_ticks_msec()
	while not (world.get_sim() != null and world.get_sim().has_local_player()):
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			push_error("waypoint_hud_probe: player never spawned (mission load stalled?)")
			quit(1)
			return
	var sim = world.get_sim()
	if sim == null:
		push_error("waypoint_hud_probe: no sim after player spawn")
		quit(1)
		return
	await _mission_wait(SETTLE_MISSION_SECONDS)

	var wp: Dictionary = sim.get_waypoint_hud_view()
	print("PROBE track: show=%s count=%d current=%d" %
			[wp.get("show"), int(wp.get("count", 0)), int(wp.get("current", -1))])
	if int(wp.get("count", 0)) == 0:
		print("PROBE PASS (vacuous): %s authors no blue waypoint route" %
				OS.get_environment("NW_SP_MISSION"))
		quit(0)
		return
	if int(wp.get("current", -1)) < 0:
		push_error("waypoint_hud_probe: track has entries but no current selection")
		quit(1)
		return

	# The presenter's HUD entry: the game HUD presenter must resolve a name + distance.
	var hud_presenter = game.get_node_or_null("GameHudPresenter")
	var entry: WaypointHudEntry = null
	if hud_presenter != null and hud_presenter.has_method("waypoint_hud_entry"):
		entry = hud_presenter.waypoint_hud_entry()
	if entry == null:
		push_error("waypoint_hud_probe: the HUD presenter built no waypoint entry")
		quit(1)
		return
	print("PROBE label: name=\"%s\" distance=%dm number=%d" %
			[entry.text_name, entry.distance_m, int(wp.get("number", 0))])

	# Teleport ONTO the current waypoint (mission space) and verify the advance
	# (the LAST entry never proximity-advances — verify the hold instead).
	var cur := int(wp.get("current", -1))
	var count := int(wp.get("count", 0))
	var pos: Vector3 = wp.get("position", Vector3.ZERO)
	var mission_pos := Vector3(pos.x, -pos.z, pos.y) # Godot -> mission
	var player_idx := -1
	var ppos: Vector3 = world.get_sim().get_local_player_position()
	for i in 4096:
		var d: Dictionary = sim.get_entity_debug(i)
		if d.is_empty():
			break
		var epos: Vector3 = d.get("position", Vector3.INF)
		if epos != Vector3.INF and epos.distance_to(ppos) < 0.5:
			player_idx = i
			break
	if player_idx < 0:
		push_error("waypoint_hud_probe: could not locate the player's AI index")
		quit(1)
		return
	sim.debug_set_entity_position(player_idx, mission_pos)
	await _mission_wait(1.0)
	wp = sim.get_waypoint_hud_view()
	var now := int(wp.get("current", -1))
	print("PROBE advance: current %d -> %d (count %d)" % [cur, now, count])
	if cur == count - 1:
		if now != cur:
			push_error("waypoint_hud_probe: the LAST waypoint must hold, moved to %d" % now)
			quit(1)
			return
	elif now == cur:
		push_error("waypoint_hud_probe: standing on waypoint %d did not advance" % cur)
		quit(1)
		return
	print("PROBE PASS: waypoint track live, label resolved, proximity advance verified")
	quit(0)
