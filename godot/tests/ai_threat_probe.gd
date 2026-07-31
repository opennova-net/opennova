extends SceneTree

# D-AI-5 in-game verification probe — NOT a GUT test (needs a retail PFF install;
# *_probe.gd files are manual, never collected). Boots the real game shell
# (main_game.tscn), whose NW_SP_MISSION=<bms> path starts a single-player mission
# through the exact menu-Start flow, walks the local player toward the nearest NPC
# (retail start markers deliberately spawn FARTHEST from the enemy set, net-re
# §5.2c — out of perception range), then stands still and watches: prints the NPC
# census and the player's health once per mission second, and exits PASS when
# hostile AI fire has damaged and then killed the player — the slice-1 threat loop
# observed live (world-wac-ai-re §17).
#
#   NW_SP_MISSION=CP01.bms "$GODOT_BIN" --headless --path godot \
#       -s res://tests/ai_threat_probe.gd
#
# The resource dir is the persisted one (a PFF install — the game runtime cannot
# mount flat extracts). Movement rides the LocalPlayerPresenter input_source seam +
# add_local_player_look; steering self-calibrates by comparing the player's actual
# movement vector against the target bearing, so no yaw-convention assumptions.

const MAX_MISSION_SECONDS := 420
const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const AI_SCAN_CAP := 2048
const TIME_SCALE := 5.0
const STOP_DISTANCE_U := 15.0

var _forward := false


func _initialize() -> void:
	call_deferred("_run")


func _mission_wait(seconds: float) -> void:
	var t0 := Time.get_ticks_msec()
	while float(Time.get_ticks_msec() - t0) * TIME_SCALE < seconds * 1000.0:
		await process_frame


func _nearest_npc(sim, world) -> Dictionary:
	var player_pos: Vector3 = world.get_sim().get_local_player_position()
	var best := {}
	var best_dist := INF
	for i in AI_SCAN_CAP:
		var d: Dictionary = sim.get_entity_debug(i)
		if d.is_empty():
			break
		if not bool(d.get("alive", false)):
			continue
		# Foot infantry only: a mounted occupant rides its seat (pose_if_mounted) and
		# fires via the D-AI-2 emplacement solver — out of the slice-1 loop.
		if bool(d.get("mounted", false)) or not bool(d.get("infantry", false)):
			continue
		var pos: Vector3 = d.get("position", Vector3.INF)
		var dist := pos.distance_to(player_pos)
		if dist < 0.5:
			continue # self
		if dist < best_dist:
			best_dist = dist
			best = d
			best["distance"] = dist
	return best


func _run() -> void:
	if OS.get_environment("NW_SP_MISSION").is_empty():
		push_error("ai_threat_probe: set NW_SP_MISSION=<mission.bms>")
		quit(1)
		return
	# NW_RESOURCE_DIR pins the mount for this run (the persisted user:// dir is
	# SHARED with ONED and other sessions repoint it — a stale dir/expansion
	# poisons the runtime mounts and the mission never loads). Applied through
	# the app's own settings seam; the expansion resets to base alongside it.
	var res_dir := OS.get_environment("NW_RESOURCE_DIR")
	if not res_dir.is_empty():
		var settings := load("res://engine/resource_index/resource_dir_settings.gd")
		settings.set_resource_dir(res_dir)
		settings.set_expansion("")
	Engine.time_scale = TIME_SCALE
	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		push_error("ai_threat_probe: failed to load main_game.tscn")
		quit(1)
		return
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	var presenter = game.get_node_or_null("LocalPlayerPresenter")
	if world == null or presenter == null:
		push_error("ai_threat_probe: main_game lacks World/LocalPlayerPresenter children")
		quit(1)
		return

	# Wait out the menu -> loading screen -> mission load -> player spawn chain.
	var wall_start := Time.get_ticks_msec()
	while not (world.get_sim() != null and world.get_sim().has_local_player()):
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			push_error("ai_threat_probe: player never spawned (mission load stalled?)")
			quit(1)
			return
	var sim = world.get_sim()
	if sim == null:
		push_error("ai_threat_probe: no sim after player spawn")
		quit(1)
		return
	print("PROBE mission=%s loaded, player spawned" % OS.get_environment("NW_SP_MISSION"))

	var first := _nearest_npc(sim, world)
	print("PROBE census: nearest NPC %.1f u (%s/item %d)" %
			[float(first.get("distance", INF)), String(first.get("name", "")),
			int(first.get("item_id", 0))])
	# Self + nearest diagnostic rows (team, perception ranges, the D-AI-5 seed).
	var ppos: Vector3 = world.get_sim().get_local_player_position()
	for i in AI_SCAN_CAP:
		var d: Dictionary = sim.get_entity_debug(i)
		if d.is_empty():
			break
		var pos: Vector3 = d.get("position", Vector3.INF)
		if pos.distance_to(ppos) < 0.5:
			print("PROBE self: team=%d item=%d hp=%d" %
					[int(d.get("team", -1)), int(d.get("item_id", 0)), int(d.get("ai_health", 0))])
			break
	print("PROBE nearest: net=%d infantry=%s team=%d sight=%.1fu attack=%.1fu ammo=%d clip=%d mag=%d tgt=%s state=%d" %
			[int(first.get("net_id", 0)), str(first.get("infantry", false)),
			int(first.get("team", -1)), float(first.get("sight_range_u", -1.0)),
			float(first.get("attack_range_u", -1.0)), int(first.get("ammo_primary", -99)),
			int(first.get("clip_size", -1)), int(first.get("magazine", -1)),
			str(first.get("combat_target_valid", false)), int(first.get("state", -1))])

	# The movement seam: the presenter polls this callable INSTEAD of the keyboard.
	presenter.set_input_source(func() -> Dictionary: return {"forward": _forward})

	# --- Approach: walk at the nearest NPC, correcting heading off the actual
	# movement vector (convention-free); bang-bang gain sign self-calibrates.
	var hp0: int = world.get_sim().get_local_player_health()
	var seconds := 0
	var px_per_deg := 8.0
	var prev_err := 0.0
	var prev_pos: Vector3 = world.get_sim().get_local_player_position()
	_forward = true
	while seconds < MAX_MISSION_SECONDS:
		await _mission_wait(1.0)
		seconds += 1
		var hp: int = world.get_sim().get_local_player_health()
		if hp < hp0:
			break # already under fire — skip to the watch phase
		var npc := _nearest_npc(sim, world)
		if npc.is_empty():
			continue
		var dist := float(npc.get("distance", INF))
		if dist <= STOP_DISTANCE_U:
			break
		var pos: Vector3 = world.get_sim().get_local_player_position()
		var moved := Vector2(pos.x - prev_pos.x, pos.z - prev_pos.z)
		var tgt: Vector3 = npc.get("position", pos)
		var want := Vector2(tgt.x - pos.x, tgt.z - pos.z)
		if moved.length() > 0.2 and want.length() > 0.01:
			var err := rad_to_deg(moved.angle_to(want))
			if abs(err) > abs(prev_err) + 1.0 and abs(prev_err) > 0.5:
				px_per_deg = -px_per_deg # gain sign was wrong; flip
			world.get_sim().add_local_player_look(clampf(err * px_per_deg, -400.0, 400.0), 0.0)
			prev_err = err
		prev_pos = pos
		if seconds % 10 == 0:
			print("PROBE approach t=%ds dist=%.1fu hp=%d npc_state=%d" %
					[seconds, dist, hp, int(npc.get("state", -1))])
	_forward = false
	var here := _nearest_npc(sim, world)
	print("PROBE holding at %.1fu from %s (t=%ds) — waiting for hostile fire" %
			[float(here.get("distance", INF)), String(here.get("name", "")), seconds])

	# --- Watch: stand still until NPC fire kills the player.
	var damaged_at := -1
	while seconds < MAX_MISSION_SECONDS:
		var hp2: int = world.get_sim().get_local_player_health()
		if hp2 < hp0 and damaged_at < 0:
			damaged_at = seconds
			print("PROBE DAMAGED t=%ds hp %d -> %d (hostile fire landed)" % [seconds, hp0, hp2])
		if hp2 <= 0:
			print("PROBE DEAD t=%ds — NPC fire killed the player" % seconds)
			# The §17.4 presentation legs must have run for the shots that landed:
			# fire events drained, the ai_launch sound played (immediate or delayed),
			# the ai_launcheffect muzzle spawned, tracer rounds drawn (rate-gated).
			var stats: Dictionary = world.get_fire_present_stats()
			print("PROBE fire-present stats: %s" % str(stats))
			if int(stats.get("fires", 0)) <= 0 or \
					int(stats.get("sounds", 0)) + int(stats.get("delayed_sounds", 0)) <= 0:
				print("PROBE FAIL: NPC fire presented no sound (stats above)")
				quit(1)
				return
			if int(stats.get("effects", 0)) <= 0:
				print("PROBE WARN: no muzzle effect spawns (ai_launcheffect missing from data?)")
			if int(stats.get("tracer_peak", 0)) <= 0:
				print("PROBE WARN: no tracer rounds observed (tracer_rate 0 on this ammo?)")
			print("PROBE PASS: acquire -> fire (heard+seen) -> damage -> kill all observed in-game")
			quit(0)
			return
		await _mission_wait(1.0)
		seconds += 1
		if seconds % 10 == 0:
			var npc2 := _nearest_npc(sim, world)
			print("PROBE t=%ds hp=%d nearest=%.1fu state=%d npc_hp=%d fire=%s" %
					[seconds, hp2, float(npc2.get("distance", INF)),
					int(npc2.get("state", -1)), int(npc2.get("ai_health", 0)),
					str(world.get_fire_present_stats())])
	print("PROBE FAIL: player hp=%d after %ds (damaged_at=%d) — no kill observed" %
			[world.get_sim().get_local_player_health(), MAX_MISSION_SECONDS, damaged_at])
	quit(1)
