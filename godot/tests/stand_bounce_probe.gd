extends SceneTree

# Standing-still vertical-stability probe — NOT a GUT test (needs a retail PFF
# install; *_probe.gd files are manual, never collected). Boots the real game
# shell, waits for the SP spawn, feeds a pinned EMPTY input source (no movement,
# no jump), lets the spawn settle, then samples the local player's position and
# primary anim state every frame for a watch window. FAILS when the standing
# player's height oscillates: peak-to-peak z over the watch window above the
# threshold, with the z trace and any anim-state changes printed so the
# waveform names the mechanism (sawtooth = gravity accumulation between
# resolver snaps; smooth loop-period wave = capsule-bottom-tracked dz;
# state flap to 31 = the airborne edge/jump path firing at rest).
#
#   NW_SP_MISSION=CP01.bms NW_RESOURCE_DIR=<retail install> \
#       "$GODOT_BIN" --headless --path godot -s res://tests/stand_bounce_probe.gd

const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const TIME_SCALE := 2.0
const SETTLE_MISSION_SECONDS := 5.0
const WATCH_MISSION_SECONDS := 12.0
const PEAK_TO_PEAK_LIMIT_U := 0.02
const AI_SCAN_CAP := 2048


func _initialize() -> void:
	call_deferred("_run")


func _mission_wait(seconds: float) -> void:
	var t0 := Time.get_ticks_msec()
	while float(Time.get_ticks_msec() - t0) * TIME_SCALE < seconds * 1000.0:
		await process_frame


func _self_state(sim, world) -> int:
	var ppos: Vector3 = world.get_sim().get_local_player_position()
	for i in AI_SCAN_CAP:
		var d: Dictionary = sim.get_entity_debug(i)
		if d.is_empty():
			break
		var pos: Vector3 = d.get("position", Vector3.INF)
		if pos.distance_to(ppos) < 0.5:
			return int(d.get("state", -1))
	return -1


func _run() -> void:
	if OS.get_environment("NW_SP_MISSION").is_empty():
		push_error("stand_bounce_probe: set NW_SP_MISSION=<mission.bms>")
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
		push_error("stand_bounce_probe: failed to load main_game.tscn")
		quit(1)
		return
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	var presenter = game.get_node_or_null("LocalPlayerPresenter")
	if world == null or presenter == null:
		push_error("stand_bounce_probe: main_game lacks World/LocalPlayerPresenter children")
		quit(1)
		return

	var wall_start := Time.get_ticks_msec()
	while not (world.get_sim() != null and world.get_sim().has_local_player()):
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			push_error("stand_bounce_probe: player never spawned (mission load stalled?)")
			quit(1)
			return
	var sim = world.get_sim()
	if sim == null:
		push_error("stand_bounce_probe: no sim after player spawn")
		quit(1)
		return
	print("PROBE mission=%s loaded, player spawned" % OS.get_environment("NW_SP_MISSION"))

	# Pin the input: no movement, no jump — the player must stand dead still.
	presenter.set_input_source(func() -> Dictionary: return {})

	await _mission_wait(SETTLE_MISSION_SECONDS)
	var z0: float = world.get_sim().get_local_player_position().y
	print("PROBE settled: z=%.4f state=%d — watching %.0f mission-s" %
			[z0, _self_state(sim, world), WATCH_MISSION_SECONDS])

	# Watch: per-frame z samples + the resolver's own view (capsule bottom, foot
	# clearance) + the motor's anim key — the three channels that separate the
	# mechanisms (z rides capsule_bottom = the clip-tracked dz; foot_clearance
	# band-hopping = resolver/gravity; anim flapping = the edge/jump path).
	var zs: Array[float] = []
	var min_z := INF
	var max_z := -INF
	var last_anim := String(sim.get_local_player_anim_key())
	var anim_changes := 0
	var t0 := Time.get_ticks_msec()
	while float(Time.get_ticks_msec() - t0) * TIME_SCALE < WATCH_MISSION_SECONDS * 1000.0:
		await process_frame
		var z: float = world.get_sim().get_local_player_position().y
		zs.append(z)
		min_z = minf(min_z, z)
		max_z = maxf(max_z, z)
		var cdbg: Dictionary = sim.get_collision_debug()
		var pl: Dictionary = cdbg.get("player", {})
		var anim := String(sim.get_local_player_anim_key())
		if anim != last_anim:
			anim_changes += 1
			print("PROBE anim change #%d: %s -> %s (sample %d)" %
					[anim_changes, last_anim, anim, zs.size()])
			last_anim = anim
		print("PROBE s=%03d z=%.4f cb=%.4f fc=%.4f anim=%s" %
				[zs.size(), z, float(pl.get("capsule_bottom", NAN)),
				float(pl.get("foot_clearance", NAN)), anim])

	var p2p := max_z - min_z
	print("PROBE watch done: samples=%d min_z=%.4f max_z=%.4f p2p=%.4fu anim_changes=%d" %
			[zs.size(), min_z, max_z, p2p, anim_changes])

	if p2p > PEAK_TO_PEAK_LIMIT_U:
		print("PROBE FAIL: standing player bounces — p2p %.4fu > %.4fu" %
				[p2p, PEAK_TO_PEAK_LIMIT_U])
		quit(1)
		return
	print("PROBE PASS: standing player is vertically stable (p2p %.4fu)" % p2p)
	quit(0)
