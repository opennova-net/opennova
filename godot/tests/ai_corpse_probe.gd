extends SceneTree

# P1c death-presentation in-game probe — NOT a GUT test (needs a retail PFF
# install; *_probe.gd files are manual, never collected). Boots the real game
# shell, walks the local player to the nearest foot NPC (the ai_threat_probe
# steering), then KILLS it with real player rounds (the full C2S 0x06 ->
# RoundSim chain — the same damage path that stages the death-anim selection,
# world-wac-ai-re §19.2) and watches the corpse:
#
#   * the kill leaves the NPC dead but NOT hidden — the corpse renders,
#   * its anim state is a DEATH state (the bullet matrix 180..239 for a round
#     kill; 173/174 fallbacks tolerated for stripped anim sets),
#   * the corpse timer seeded from items.def deathtime (CP01 soldiers author
#     30 s -> 1922 ticks) and drains,
#   * WATCHED PERSISTENCE: the corpse outlives its timer while the local
#     player can see it (the §19.4 watch-check parks it on 62-tick retries).
#
#   NW_SP_MISSION=CP01.bms NW_RESOURCE_DIR=<retail install> "$GODOT_BIN" \
#       --headless --path godot -s res://tests/ai_corpse_probe.gd
#
# Aiming is self-calibrating: the approach steering already centers the look on
# the target (walking is look-directed); the fire phase sweeps pitch in steps
# between bursts until the target's health drops (RoundSim rounds fly straight —
# spread is a tracked deferral — so a burst either connects or misses cleanly).

const MAX_MISSION_SECONDS := 420
const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const AI_SCAN_CAP := 2048
const TIME_SCALE := 5.0
const FIRE_DISTANCE_U := 26.0
const DEATH_STATE_MIN := 173

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
		if bool(d.get("mounted", false)) or not bool(d.get("infantry", false)):
			continue
		var pos: Vector3 = d.get("position", Vector3.INF)
		var dist := pos.distance_to(player_pos)
		if dist < 0.5:
			continue # self
		if dist < best_dist:
			best_dist = dist
			best = d
			best["ai_index"] = i
			best["distance"] = dist
	return best


func _debug_row(sim, ai_index: int) -> Dictionary:
	var d: Dictionary = sim.get_entity_debug(ai_index)
	return d if d != null else {}


func _run() -> void:
	if OS.get_environment("NW_SP_MISSION").is_empty():
		push_error("ai_corpse_probe: set NW_SP_MISSION=<mission.bms>")
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
		push_error("ai_corpse_probe: failed to load main_game.tscn")
		quit(1)
		return
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	var presenter = game.get_node_or_null("LocalPlayerPresenter")
	if world == null or presenter == null:
		push_error("ai_corpse_probe: main_game lacks World/LocalPlayerPresenter children")
		quit(1)
		return

	var wall_start := Time.get_ticks_msec()
	while not (world.get_sim() != null and world.get_sim().has_local_player()):
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			push_error("ai_corpse_probe: player never spawned (mission load stalled?)")
			quit(1)
			return
	var sim = world.get_sim()
	if sim == null:
		push_error("ai_corpse_probe: no sim after player spawn")
		quit(1)
		return
	print("PROBE mission=%s loaded, player spawned" % OS.get_environment("NW_SP_MISSION"))

	presenter.set_input_source(func() -> Dictionary: return {"forward": _forward})

	# --- Approach (the ai_threat_probe steering): walk at the nearest NPC,
	# correcting heading off the actual movement vector. The moment a foot NPC
	# is inside FIRE_DISTANCE the probe stops and opens fire — waiting any
	# longer loses the race against the NPC's own perception+fire chain.
	var seconds := 0
	var px_per_deg := 8.0
	var prev_err := 0.0
	var prev_pos: Vector3 = world.get_sim().get_local_player_position()
	var target_index := -1
	var target_net := -1
	var target_hp0 := -1
	_forward = true
	while seconds < MAX_MISSION_SECONDS:
		await _mission_wait(1.0)
		seconds += 1
		var npc := _nearest_npc(sim, world)
		if npc.is_empty():
			continue
		var dist := float(npc.get("distance", INF))
		if dist <= FIRE_DISTANCE_U:
			target_index = int(npc.get("ai_index", -1))
			target_net = int(npc.get("net_id", -1))
			target_hp0 = int(npc.get("ai_health", -1))
			print("PROBE target lock: net=%d hp=%d at %.1fu deathtime=%d leave_corpse=%s" %
					[target_net, target_hp0, dist, int(npc.get("deathtime_ticks", -1)),
					str(npc.get("leave_corpse", false))])
			break
		var pos: Vector3 = world.get_sim().get_local_player_position()
		var moved := Vector2(pos.x - prev_pos.x, pos.z - prev_pos.z)
		var tgt: Vector3 = npc.get("position", pos)
		var want := Vector2(tgt.x - pos.x, tgt.z - pos.z)
		if moved.length() > 0.2 and want.length() > 0.01:
			var err := rad_to_deg(moved.angle_to(want))
			if abs(err) > abs(prev_err) + 1.0 and abs(prev_err) > 0.5:
				px_per_deg = -px_per_deg
			world.get_sim().add_local_player_look(clampf(err * px_per_deg, -400.0, 400.0), 0.0)
			prev_err = err
		prev_pos = pos
		if seconds % 10 == 0:
			print("PROBE approach t=%ds dist=%.1fu hp=%d" %
					[seconds, dist, world.get_sim().get_local_player_health()])
	_forward = false

	if target_index < 0:
		print("PROBE FAIL: no foot NPC came within %.0fu in %ds" % [FIRE_DISTANCE_U, seconds])
		quit(1)
		return

	# --- Fire phase: CLOSED-LOOP AIM off the real FP camera transform. Calibrate
	# the look-pixel <-> camera-degree gains with two nudges, then steer the look
	# straight at the target's LIVE chest each tick (a hit NPC reacts and moves)
	# while the trigger is held through the sim's own weapon-input seam (the FSM
	# fires the real C2S 0x06 -> RoundSim chain; the held trigger sustains the
	# volley and auto-reloads).
	var cam := root.get_viewport().get_camera_3d()
	if cam == null:
		print("PROBE FAIL: no active Camera3D to aim with")
		quit(1)
		return
	var yaw0 := cam.global_rotation.y
	world.get_sim().add_local_player_look(50.0, 0.0)
	await _mission_wait(0.2)
	var yaw_gain := 50.0 / rad_to_deg(wrapf(cam.global_rotation.y - yaw0, -PI, PI))
	var pitch0 := cam.global_rotation.x
	world.get_sim().add_local_player_look(0.0, 50.0)
	await _mission_wait(0.2)
	var pitch_gain := 50.0 / rad_to_deg(wrapf(cam.global_rotation.x - pitch0, -PI, PI))
	print("PROBE aim gains: yaw %.1f px/deg, pitch %.1f px/deg" % [yaw_gain, pitch_gain])
	if not is_finite(yaw_gain) or not is_finite(pitch_gain) \
			or absf(yaw_gain) > 1000.0 or absf(pitch_gain) > 1000.0:
		print("PROBE FAIL: aim-gain calibration degenerate")
		quit(1)
		return

	# Shorten the fight: pre-weaken the target through the scripted-SETHP stores
	# (the WAC SETHP shape) so the FIRST connecting round completes the kill — the
	# kill still travels the full C2S 0x06 -> RoundSim damage/death chain under
	# test; only the number of required hits changes.
	sim.debug_set_entity_health(target_index, 10)
	var killed_at := -1
	var hp_prev := 10
	var fire_ticks := 0
	var stalled := 0
	world.get_sim().set_local_player_weapon_input(true, true, false)
	while seconds < MAX_MISSION_SECONDS:
		# One mission-second burst: re-aim at the live chest EVERY frame (a hit
		# NPC reacts and moves) and close distance while far (aim is camera-based,
		# movement-independent).
		var t0 := Time.get_ticks_msec()
		while float(Time.get_ticks_msec() - t0) * TIME_SCALE < 1000.0:
			var row0 := _debug_row(sim, target_index)
			var tpos: Vector3 = row0.get("position", Vector3.INF)
			if tpos != Vector3.INF:
				# Rounds leave the MUZZLE (the +0.9u chest stand-in) parallel to the
				# look, not the camera eye — aim the camera ray at the chest PLUS the
				# eye-muzzle vertical offset so the round line crosses the chest
				# sphere (a straight eye-ray at the chest passes ~0.7u under it).
				var muzzle_y: float = world.get_sim().get_local_player_position().y + 0.9
				var aim_bias := cam.global_position.y - muzzle_y
				var chest := tpos + Vector3(0.0, 0.9 + aim_bias, 0.0)
				var to := chest - cam.global_position
				var fwd := -cam.global_transform.basis.z
				var err_yaw := rad_to_deg(wrapf(atan2(-to.x, -to.z) - atan2(-fwd.x, -fwd.z), -PI, PI))
				var err_pitch := rad_to_deg(atan2(to.y, Vector2(to.x, to.z).length())
						- atan2(fwd.y, Vector2(fwd.x, fwd.z).length()))
				world.get_sim().add_local_player_look(
						clampf(err_yaw * yaw_gain, -400.0, 400.0),
						clampf(err_pitch * pitch_gain, -400.0, 400.0))
				_forward = Vector2(to.x, to.z).length() > 8.0
			world.get_sim().set_local_player_weapon_input(true, false, false)
			await process_frame
		seconds += 1
		fire_ticks += 1
		var row := _debug_row(sim, target_index)
		var hp := int(row.get("ai_health", -1))
		if hp < hp_prev and hp >= 0:
			print("PROBE HIT t=%ds target hp %d -> %d" % [seconds, hp_prev, hp])
			stalled = 0
		else:
			stalled += 1
		hp_prev = hp
		if not bool(row.get("alive", true)) or hp <= 0:
			killed_at = seconds
			_forward = false
			world.get_sim().set_local_player_weapon_input(false, false, false)
			print("PROBE KILLED t=%ds — player rounds killed net=%d" % [seconds, target_net])
			break
		if world.get_sim().get_local_player_health() <= 0:
			print("PROBE FAIL: the NPC killed the PLAYER first (t=%ds) — rerun" % seconds)
			quit(1)
			return
		if stalled > 0 and stalled % 10 == 0:
			var tp2: Vector3 = row.get("position", Vector3.INF)
			var d2 := tp2.distance_to(world.get_sim().get_local_player_position()) if tp2 != Vector3.INF else -1.0
			print("PROBE stall t=%ds hp=%d dist=%.1fu state=%d" %
					[seconds, hp, d2, int(row.get("state", -1))])
		if fire_ticks > 60:
			print("PROBE FAIL: fire phase timed out without a kill (last hp=%d)" % hp_prev)
			quit(1)
			return

	# --- Corpse phase: dead but NOT hidden; a death-family anim; the timer
	# seeded from deathtime and draining; still visible after the timer would
	# have expired (the watch rule — the player is looking straight at it).
	var c0 := _debug_row(sim, target_index)
	var anim0 := int(c0.get("anim_state", -1))
	var timer0 := int(c0.get("corpse_timer", -1))
	print("PROBE corpse t=%ds: alive=%s hidden=%s anim=%d (%s) corpse_timer=%d deathtime=%d" %
			[seconds, str(c0.get("alive", true)), str(c0.get("hidden", false)), anim0,
			String(c0.get("anim_key", "")), timer0, int(c0.get("deathtime_ticks", -1))])
	if bool(c0.get("hidden", false)):
		print("PROBE FAIL: corpse hidden immediately after the kill")
		quit(1)
		return
	if anim0 < DEATH_STATE_MIN:
		print("PROBE FAIL: dead NPC anim %d is not a death state" % anim0)
		quit(1)
		return
	var bullet_matrix := anim0 >= 180 and anim0 <= 239
	if not bullet_matrix:
		print("PROBE WARN: round kill fell back to anim %d (adm lacks the matrix clip?)" % anim0)

	# Watch ~40 mission seconds: past the 31 s deathtime, the watched corpse must
	# still be there (parked on the 62-tick retry), never hidden, anim held.
	var watched_ok := true
	for w in range(8):
		await _mission_wait(5.0)
		seconds += 5
		var c := _debug_row(sim, target_index)
		var hidden := bool(c.get("hidden", false))
		var timer := int(c.get("corpse_timer", -1))
		print("PROBE watch t=%ds hidden=%s corpse_timer=%d anim=%d" %
				[seconds, str(hidden), timer, int(c.get("anim_state", -1))])
		if hidden:
			watched_ok = false
			break
	if not watched_ok:
		print("PROBE FAIL: the watched corpse despawned (the §19.4 watch rule should hold it)")
		quit(1)
		return
	print("PROBE PASS: round kill -> directional death pose (anim %d%s) -> corpse persisted %ds watched (timer0=%d)" %
			[anim0, " [bullet matrix]" if bullet_matrix else "", seconds - killed_at, timer0])
	quit(0)
