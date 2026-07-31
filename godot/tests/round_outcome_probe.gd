extends SceneTree

# P2 round-outcome in-game probe — NOT a GUT test (needs a retail PFF install;
# *_probe.gd files are manual, never collected). Drives the 04TR training mission's
# witnessed lose flow end to end:
#
#   kill a green/blue PERSON with real player rounds (the full C2S 0x06 -> RoundSim
#   damage/death chain) ->
#   * the kill tally lands in the sim (greenkills/bluekills, the WAC builtin source)
#     [orig: Score_TallyKillByLocalPlayer @0x4fd160],
#   * 04TR.WAC's `true(greenkills) -> Lose(0)` / `true(bluekills) -> Lose(1)` fires
#     [orig: WacAction_Lose @0x4ed3f0]: the "lose" effect carries the witnessed
#     Misc gametext key and the round ends winner 2
#     [orig: Server_ProcessRoundEnd @0x5164f0],
#   * the shell mounts the MISSION FAILED end screen after the lead-in beat
#     [orig: the Cinematic_EpilogUpdate mode-2 leg],
#   * ESC leaves to the menu [orig: g_mission_exit_reason=1 -> the "Post Menu" push].
#
#   NW_SP_MISSION=04TR.bms NW_RESOURCE_DIR=<retail install> "$GODOT_BIN" \
#       --headless --path godot -s res://tests/round_outcome_probe.gd

const MAX_MISSION_SECONDS := 420
const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const AI_SCAN_CAP := 2048
const TIME_SCALE := 5.0
const FIRE_DISTANCE_U := 26.0

var _forward := false
var _seen_effects: Array[Dictionary] = []


func _initialize() -> void:
	call_deferred("_run")


func _mission_wait(seconds: float) -> void:
	var t0 := Time.get_ticks_msec()
	while float(Time.get_ticks_msec() - t0) * TIME_SCALE < seconds * 1000.0:
		await process_frame


func _tap_key(keycode: Key) -> void:
	var down := InputEventKey.new()
	down.keycode = keycode
	down.physical_keycode = keycode
	down.pressed = true
	Input.parse_input_event(down)
	var up := InputEventKey.new()
	up.keycode = keycode
	up.physical_keycode = keycode
	up.pressed = false
	Input.parse_input_event(up)


var _blacklist: Array[int] = []


# Nearest live pool-0 person NPC, preferring a LOSE-triggering team (green 0, then
# blue 1). The round sim's entity hits scan pool 0 only, so victims elsewhere can
# never take player rounds — skip them (and anything blacklisted after a stall).
func _pick_victim(sim, world) -> Dictionary:
	var player_pos: Vector3 = world.get_sim().get_local_player_position()
	var best := {}
	var best_key := [99, INF] # [team preference rank, distance]
	for i in AI_SCAN_CAP:
		var d: Dictionary = sim.get_entity_debug(i)
		if d.is_empty():
			break
		if i in _blacklist:
			continue
		if not bool(d.get("alive", false)):
			continue
		if bool(d.get("mounted", false)) or not bool(d.get("infantry", false)):
			continue
		if int(d.get("pool", -1)) != 0:
			continue
		if (int(d.get("engine_flags", 0)) & 0x4000000) != 0:
			continue # indestructible scripted NPC — rounds never damage it [orig: @0x4e7ff6]
		var pos: Vector3 = d.get("position", Vector3.INF)
		var dist := pos.distance_to(player_pos)
		if dist < 0.5:
			continue # self
		var team := int(d.get("team", -1))
		var rank := 0 if team == 0 else (1 if team == 1 else 2)
		if rank < best_key[0] or (rank == best_key[0] and dist < best_key[1]):
			best_key = [rank, dist]
			best = d
			best["ai_index"] = i
			best["distance"] = dist
	return best


func _on_effects(effects: Array) -> void:
	for e in effects:
		if e is Dictionary:
			var kind := String(e.get("kind", ""))
			if kind == "lose" or kind == "win" or kind == "round_end":
				_seen_effects.append(e)
				print("PROBE effect: %s a=%d str=%s" % [kind, int(e.get("a", 0)), String(e.get("str", ""))])


func _run() -> void:
	if OS.get_environment("NW_SP_MISSION").is_empty():
		push_error("round_outcome_probe: set NW_SP_MISSION=04TR.bms")
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
		push_error("round_outcome_probe: failed to load main_game.tscn")
		quit(1)
		return
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	var presenter = game.get_node_or_null("LocalPlayerPresenter")
	if world == null or presenter == null:
		push_error("round_outcome_probe: main_game lacks World/LocalPlayerPresenter children")
		quit(1)
		return
	world.mission_effects.connect(_on_effects)

	var wall_start := Time.get_ticks_msec()
	while not (world.get_sim() != null and world.get_sim().has_local_player()):
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			push_error("round_outcome_probe: player never spawned (mission load stalled?)")
			quit(1)
			return
	var sim = world.get_sim()
	if sim == null:
		push_error("round_outcome_probe: no sim after player spawn")
		quit(1)
		return
	var outcome0: Dictionary = sim.get_round_outcome_debug()
	print("PROBE mission=%s loaded; outcome0=%s" % [OS.get_environment("NW_SP_MISSION"), str(outcome0)])
	if bool(outcome0.get("ended", false)):
		print("PROBE FAIL: round already ended at spawn")
		quit(1)
		return

	presenter.set_input_source(func() -> Dictionary: return {"forward": _forward})

	# --- Aim calibration once (closed-loop gains off the real FP camera).
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
	print("PROBE aim gains: yaw %.1f pitch %.1f px/deg" % [yaw_gain, pitch_gain])
	if not is_finite(yaw_gain) or not is_finite(pitch_gain) \
			or absf(yaw_gain) > 1000.0 or absf(pitch_gain) > 1000.0:
		print("PROBE FAIL: aim-gain calibration degenerate")
		quit(1)
		return

	# --- Approach + fire, retargeting away from stalled victims (a scripted range
	# dummy or an unreachable one costs ~15 fire-seconds, then the next candidate).
	var seconds := 0
	var target_team := -1
	var killed := false
	while seconds < MAX_MISSION_SECONDS and not killed:
		# Approach the current best candidate.
		var px_per_deg := 8.0
		var prev_err := 0.0
		var prev_pos: Vector3 = world.get_sim().get_local_player_position()
		var target_index := -1
		_forward = true
		while seconds < MAX_MISSION_SECONDS:
			await _mission_wait(1.0)
			seconds += 1
			var npc := _pick_victim(sim, world)
			if npc.is_empty():
				continue
			var dist := float(npc.get("distance", INF))
			if dist <= FIRE_DISTANCE_U:
				target_index = int(npc.get("ai_index", -1))
				target_team = int(npc.get("team", -1))
				print("PROBE target lock: net=%d team=%d pool=%d flags=0x%x at %.1fu" %
						[int(npc.get("net_id", -1)), target_team, int(npc.get("pool", -1)),
						int(npc.get("engine_flags", 0)), dist])
				break
			if seconds % 5 == 0:
				# Out of reach (mission geography defeats straight-line walking):
				# bring the victim to the probe. Outcome loop under test, not nav.
				var pp: Vector3 = world.get_sim().get_local_player_position()
				var camf: Vector3 = -cam.global_transform.basis.z
				var flat := Vector2(camf.x, camf.z)
				if flat.length() < 0.1:
					flat = Vector2(1, 0)
				flat = flat.normalized() * 5.0
				var spot := pp + Vector3(flat.x, 0.0, flat.y)
				var mission := Vector3(spot.x, -spot.z, spot.y)
				sim.debug_set_entity_position(int(npc.get("ai_index", -1)), mission)
				print("PROBE teleport: net=%d team=%d brought to %.1fu in front" %
						[int(npc.get("net_id", -1)), int(npc.get("team", -1)), 5.0])
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
				print("PROBE approach t=%ds dist=%.1fu team=%d" %
						[seconds, dist, int(npc.get("team", -1))])
		_forward = false
		if target_index < 0:
			break

		# Fire at the locked target.
		sim.debug_set_entity_health(target_index, 10)
		var hp_prev := 10
		var fire_ticks := 0
		world.get_sim().set_local_player_weapon_input(true, true, false)
		while seconds < MAX_MISSION_SECONDS:
			var t0 := Time.get_ticks_msec()
			while float(Time.get_ticks_msec() - t0) * TIME_SCALE < 1000.0:
				var row0: Dictionary = sim.get_entity_debug(target_index)
				var tpos: Vector3 = row0.get("position", Vector3.INF)
				if tpos != Vector3.INF:
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
			var row: Dictionary = sim.get_entity_debug(target_index)
			var hp := int(row.get("ai_health", -1))
			if hp < hp_prev and hp >= 0:
				print("PROBE HIT t=%ds target hp %d -> %d" % [seconds, hp_prev, hp])
			hp_prev = hp
			if not bool(row.get("alive", true)) or hp <= 0:
				killed = true
				_forward = false
				world.get_sim().set_local_player_weapon_input(false, false, false)
				print("PROBE KILLED t=%ds team-%d person down" % [seconds, target_team])
				break
			if world.get_sim().get_local_player_health() <= 0:
				print("PROBE FAIL: the player died first (t=%ds) — rerun" % seconds)
				quit(1)
				return
			if fire_ticks >= 15:
				var tp: Vector3 = row.get("position", Vector3.INF)
				var pp: Vector3 = world.get_sim().get_local_player_position()
				print("PROBE stall: net=%d pool=%d hp=%d tgt=%s ply=%s — retargeting" %
						[int(row.get("net_id", -1)), int(row.get("pool", -1)), hp, str(tp), str(pp)])
				_blacklist.append(target_index)
				world.get_sim().set_local_player_weapon_input(false, false, false)
				break
	if not killed:
		print("PROBE FAIL: no candidate victim could be killed before the mission timer")
		quit(1)
		return

	# --- Outcome phase: the tally, the WAC lose, the round end. The WAC VM runs
	# every 62nd tick, so the lose lands within a mission-second or two.
	var expected_counter := "greenkills" if target_team == 0 else "bluekills"
	var expected_key := "STRMISC_KILLEDGREEN" if target_team == 0 else "STRMISC_KILLEDBLUE"
	var ended := false
	for i in range(10):
		await _mission_wait(1.0)
		seconds += 1
		var oc: Dictionary = sim.get_round_outcome_debug()
		print("PROBE outcome t=%ds: %s" % [seconds, str(oc)])
		if bool(oc.get("ended", false)):
			ended = true
			if int(oc.get("winner_team", -1)) != 2:
				print("PROBE FAIL: round ended with winner %d, expected 2 (lose)" % int(oc.get("winner_team", -1)))
				quit(1)
				return
			if int(oc.get(expected_counter, 0)) < 1:
				print("PROBE FAIL: %s stayed 0 after the kill" % expected_counter)
				quit(1)
				return
			break
	if not ended:
		print("PROBE FAIL: the round never ended after killing a team-%d person" % target_team)
		quit(1)
		return

	var saw_lose := false
	var saw_round_end := false
	for e in _seen_effects:
		if String(e.get("kind", "")) == "lose" and String(e.get("str", "")) == expected_key:
			saw_lose = true
		if String(e.get("kind", "")) == "round_end" and int(e.get("a", -1)) == 2:
			saw_round_end = true
	if not saw_lose or not saw_round_end:
		print("PROBE FAIL: effects missing (lose(%s)=%s round_end(2)=%s)" %
				[expected_key, str(saw_lose), str(saw_round_end)])
		quit(1)
		return

	# --- Presentation phase: the shell's end screen mounts after the lead-in beat,
	# with the banner line composed; ESC then leaves to the menu.
	var screen: Node = null
	for i in range(12):
		await _mission_wait(0.5)
		screen = game.get_node_or_null("HUD/MissionEndScreen")
		if screen != null:
			break
	if screen == null:
		print("PROBE FAIL: MissionEndScreen never mounted after the round end")
		quit(1)
		return
	var strings := root.get_node_or_null("NovaStrings")
	if strings == null:
		print("PROBE FAIL: NovaStrings autoload is unavailable")
		quit(1)
		return
	var banner: String = strings.lookup_display("gametext", "Misc", expected_key)
	var banner_visible := false
	for node in screen.find_children("*", "Label", true, false):
		var label := node as Label
		if label != null and label.text == banner:
			banner_visible = true
			break
	print("PROBE end screen up; banner='%s'" % banner)
	if not banner_visible:
		print("PROBE FAIL: the expected endround banner is absent from the lose screen")
		quit(1)
		return

	_tap_key(KEY_ESCAPE) # [orig: ESC -> g_mission_exit_reason=1 -> Post Menu]
	await _mission_wait(1.0)
	if world.is_loaded():
		print("PROBE FAIL: ESC did not tear the world down to the menu")
		quit(1)
		return
	print("PROBE PASS: team-%d person kill -> %s -> Lose -> round end (winner 2) -> FAILED screen (banner '%s') -> ESC to menu" %
			[target_team, expected_counter, banner])
	quit(0)
