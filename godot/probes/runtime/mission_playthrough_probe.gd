extends GameProbe

## mission_playthrough: the first SP mission played through its authored
## sequence on the real input path — the "Mission lifecycle" acceptance gate
## of docs/world/npc-mission-completion.md. Loads `mission` when no world is
## up, then walks the gates in order: (1) the load and the retail-initialized
## player, (2) movement, view and stance, (3) the walk from spawn to the truck
## `truck_ssn`, (4) boarding it with USE, (5) the instructor-driven ride with
## its area-triggered text, (6) arrival and dismount, (7) the armory zone,
## (8) the authored friendly-fire failure, (9) the clean exit to the menu,
## (10) the repeat launch from the menu, (11) the in-game RESTART (the pause
## menu's RESTART button: the same mission again with no menu between).
## `auto` drives every key and look itself; `observe` only watches the
## maintainer play and advances a gate when the world reaches it. `travel`
## selects how the probe gets from place to place between gates: `teleport`
## (the default for now) moves the player through the debug teleport as a
## navigation stand-in, `walk` steers on the camera through the real
## movement keys. Every gate's own interaction — the movement gate's keys and
## stances, USE on the release edge, the rounds, ESC, the menu — always rides
## the real input path, and no other debug staging (crew, kill_group, mission
## variables, time scale) is ever used. The witness is published as progress
## every heartbeat; playthrough.jsonl carries every sample, verdict.json the
## per-gate verdict (with the travel mode), and a PNG lands per gate. Needs a
## window (input and capture).

const HEARTBEAT_MS := 500
const SAMPLE_MS := 500
const LOAD_TIMEOUT_MS := 240_000
const WALK_TIMEOUT_MS := 240_000
const RIDE_TIMEOUT_MS := 300_000
const ARRIVAL_STILL_MS := 6_000
const OBSERVE_GATE_TIMEOUT_MS := 900_000
const FAILURE_TIMEOUT_MS := 420_000
const END_SCREEN_WAIT_MS := 20_000
const MENU_WAIT_MS := 60_000
const FRESH_WORLD_MAX_TICKS := 62 * 5
const BOARD_SCAN_RANGE := 3.5
const STUCK_WINDOW_MS := 2_500
const STUCK_MIN_MOVE := 0.25
const AIM_TOLERANCE_DEG := 3.0
const AIM_MAX_STEP_PX := 240.0
const AIM_CALIBRATION_PX := 60.0
const TEAM_BLUE := 1
const ENTITY_KIND_ORGANIC := 3
const FIRE_BEARINGS := 8
const FIRE_STAGE_DISTANCE := 5.0
const FIRE_HULL_CLEARANCE := 3.5
const FIRE_BEARING_WALK_MS := 20_000
const FIRE_MAX_BURSTS := 10
const STANCE_KEYS := [KEY_X, KEY_Z, KEY_C]

var _ctx: ProbeContext
var _mode := "auto"
var _travel := "teleport"
var _target_ssn := 0
var _mission := "00TRa.bms"
var _truck_ssn := 11
var _route: Array[Array] = []
var _out_dir := ""
var _gates: Array[Dictionary] = []
var _samples := 0
var _jsonl: FileAccess = null
var _last_heartbeat := 0
var _last_sample := 0
var _fired_seen: Array[int] = []
var _effects: Array[Dictionary] = []
var _effects_world_id := 0
var _spawn_pos := Vector3.ZERO
var _truck_rest := Vector3.ZERO
var _look_sign := 1.0
var _look_px_per_deg := 0.0
var _pitch_sign := 1.0
var _pitch_px_per_deg := 0.0
var _held: Array[Key] = []
var _mouse_held := false
var _blue_candidates_logged := false
var _start_gate := 1
var _boot_fired: Array[int] = []


func run(ctx: ProbeContext) -> ProbeVerdict:
	_ctx = ctx
	_mode = String(ctx.args.get("mode", "auto"))
	_travel = String(ctx.args.get("travel", "teleport"))
	_target_ssn = int(ctx.args.get("target_ssn", 0))
	_mission = String(ctx.args.get("mission", "00TRa.bms"))
	_truck_ssn = int(ctx.args.get("truck_ssn", 11))
	for leg_v in ctx.args.get("route", []):
		var leg := leg_v as Array
		if leg != null and leg.size() >= 2:
			_route.append([float(leg[0]), float(leg[1])])
	_out_dir = ProbeOutput.resolve(ctx, String(ctx.args.get("output_dir", "")))
	ctx.defer_restore(_release_all)
	var jsonl_path := _out_dir.path_join("playthrough.jsonl")
	_jsonl = FileAccess.open(jsonl_path, FileAccess.WRITE)
	if _jsonl == null:
		return ProbeVerdict.failed("cannot write %s" % jsonl_path)
	ctx.defer_restore(func() -> void:
		if _jsonl != null:
			_jsonl.close()
			_jsonl = null)
	# `start_gate` > 1 resumes over the loaded world's current state (an
	# iteration aid): such a run is partial and never acceptance evidence.
	var start_gate := int(ctx.args.get("start_gate", 1))
	_start_gate = start_gate
	ctx.log("mission_playthrough %s mode=%s travel=%s truck=%d start_gate=%d" % [
			_mission, _mode, _travel, _truck_ssn, start_gate])

	var gate_error := ""
	if start_gate > 1:
		if not await ctx.wait_for_local_player(LOAD_TIMEOUT_MS):
			return ProbeVerdict.failed("no local player to resume over")
		_connect_effects()
		if _mode != "observe" and start_gate > 2:
			gate_error = await _calibrate_look()
	var gates: Array[Callable] = [_gate_1_load, _gate_2_movement, _gate_3_walk_to_truck, _gate_4_board,
			_gate_5_ride, _gate_6_arrival_dismount, _gate_7_armory, _gate_8_friendly_fire_failure,
			_gate_9_clean_exit, _gate_10_repeat_launch, _gate_11_ingame_restart]
	for i in gates.size():
		if not gate_error.is_empty():
			break
		if i + 1 < start_gate:
			continue
		gate_error = await gates[i].call()
	_release_all()
	var passed := 0
	for gate in _gates:
		if bool(gate.ok):
			passed += 1
	var data := _verdict_data(gate_error)
	_write_json("verdict", data)
	ctx.artifact("playthrough", jsonl_path, "jsonl")
	ctx.progress(_witness())
	if ctx.cancelled:
		return ProbeVerdict.failed("cancelled after %d/%d gates" % [passed, _gates.size()], data)
	var partial := " (partial run from gate %d, not acceptance evidence)" % start_gate if start_gate > 1 else ""
	if not gate_error.is_empty():
		var stopped := int(_gates.back().id) if not _gates.is_empty() else start_gate
		return ProbeVerdict.failed("%d/%d gates passed; stopped at gate %d: %s%s" % [
				passed, _gates.size(), stopped, gate_error, partial], data)
	return ProbeVerdict.passed("%d/%d gates passed on %s (%s)%s" % [passed, _gates.size(), _mission, _mode, partial], data)


# --- gates ----------------------------------------------------------------------------

## The load and the retail-initialized player: a world for `mission`, the
## local player present, the single-player role, the event table and the
## briefing rows, the round not ended.
func _gate_1_load() -> String:
	var started := Time.get_ticks_msec()
	# Always a fresh mission. A `--mission` launch parks its world under the
	# start splash un-ticked: that one is the fresh load. A world that has
	# already run (an earlier probe's player wherever it stopped) is left and
	# the mission started again.
	var live_world := _ctx.world()
	var need_start := true
	if live_world != null and live_world.is_loaded():
		if not await _ctx.wait_for_local_player(LOAD_TIMEOUT_MS):
			return _gate_done(1, "load", false, "no local player within the load timeout", started)
		var parked := _ctx.sim()
		if parked != null and parked.get_logic_tick() <= FRESH_WORLD_MAX_TICKS:
			need_start = false
		else:
			var leave := _ctx.return_to_menu()
			if leave != OK:
				return _gate_done(1, "load", false, "could not leave the loaded world: %s" % error_string(leave), started)
			var unloaded := await _wait_until(func() -> bool:
				var w := _ctx.world()
				return w == null or not w.is_loaded(), MENU_WAIT_MS, "the previous world unloading")
			if not unloaded:
				return _gate_done(1, "load", false, "the previous world never unloaded", started)
			await _ctx.wait_ms(500)
	if need_start:
		var start := _ctx.start_mission(_mission)
		if start != OK:
			return _gate_done(1, "load", false, "start_mission failed: %s" % error_string(start), started)
		if not await _ctx.wait_for_local_player(LOAD_TIMEOUT_MS):
			return _gate_done(1, "load", false, "no local player within the load timeout", started)
	_connect_effects()
	var sim := _ctx.sim()
	if sim == null:
		return _gate_done(1, "load", false, "no simulation", started)
	await _ctx.wait_ms(1500)
	_spawn_pos = sim.get_local_player_position()
	var truck := sim.entity_card_by_net_id(_truck_ssn)
	if truck != null:
		_truck_rest = truck.get_position()
	var outcome: RoundOutcome = sim.get_round_outcome_debug()
	_boot_fired = _fired_indices()
	var briefing := _mission_string("info", "title")
	var detail := "role=%s events=%d entities=%d spawn=%s truck=%s title=%s" % [
			sim.session_role(), sim.get_event_count(), sim.get_entity_count(), str(_spawn_pos),
			"present" if truck != null else "MISSING", briefing]
	await _sample("gate1_load")
	await _capture("gate1_spawn")
	if not sim.has_local_player():
		return _gate_done(1, "load", false, "local player vanished after the reveal", started)
	if outcome != null and outcome.get_ended():
		return _gate_done(1, "load", false, "the round is already ended at spawn", started)
	if truck == null:
		return _gate_done(1, "load", false, "truck SSN %d is not in the world" % _truck_ssn, started)
	return _gate_done(1, "load", true, detail, started)


## Movement, view and stance through the real bindings: forward displacement
## on W, the look moving the camera, and X / Z / C cycling the stance record.
func _gate_2_movement() -> String:
	var started := Time.get_ticks_msec()
	var sim := _ctx.sim()
	if sim == null:
		return _gate_done(2, "movement", false, "no simulation", started)
	if _mode == "observe":
		_ctx.log("gate 2: move, look around and change stance")
		var moved := await _wait_until(func() -> bool:
			var s := _ctx.sim()
			return s != null and _planar(s.get_local_player_position(), _spawn_pos) > 3.0,
				OBSERVE_GATE_TIMEOUT_MS, "observe: displacement")
		await _capture("gate2_moved")
		return _gate_done(2, "movement", moved, "observed displacement > 3 u" if moved else "no displacement", started)
	var calibration := await _calibrate_look()
	if not calibration.is_empty():
		return _gate_done(2, "movement", false, calibration, started)
	var before := sim.get_local_player_position()
	_hold(KEY_W, true)
	await _wait_sampling(1500)
	_hold(KEY_W, false)
	await _ctx.wait_ms(300)
	sim = _ctx.sim()
	var moved_u := _planar(sim.get_local_player_position(), before)
	var stances: Array[int] = []
	for key in STANCE_KEYS:
		await _tap(key)
		await _ctx.wait_ms(700)
		stances.append(int(_ctx.sim().get_local_player_stance()))
	await _sample("gate2_movement")
	await _capture("gate2_stance")
	var detail := "moved %.2f u on W; stances after X/Z/C = %s; look %.2f px/deg (sign %d)" % [
			moved_u, str(stances), _look_px_per_deg, int(_look_sign)]
	if moved_u < 1.0:
		return _gate_done(2, "movement", false, "W moved the player only %.2f u" % moved_u, started)
	if stances[0] != 1 or stances[1] != 2 or stances[2] != 0:
		return _gate_done(2, "movement", false, "stance record did not follow X/Z/C: %s" % str(stances), started)
	return _gate_done(2, "movement", true, detail, started)


## The walk from spawn to the truck: on foot, steering on the camera, no teleport.
func _gate_3_walk_to_truck() -> String:
	var started := Time.get_ticks_msec()
	if _mode == "observe":
		_ctx.log("gate 3: walk to truck SSN %d" % _truck_ssn)
		var near := await _wait_until(func() -> bool: return _truck_distance() <= BOARD_SCAN_RANGE,
				OBSERVE_GATE_TIMEOUT_MS, "observe: near the truck")
		await _capture("gate3_truck")
		return _gate_done(3, "walk_to_truck", near, "distance %.2f u" % _truck_distance(), started)
	# An authored route (mission-space x, y waypoints) walks its legs first: the
	# steering is still the camera and the keys, only the target moves.
	for i in _route.size():
		var leg: Array = _route[i]
		var goal := Vector3(float(leg[0]), 0.0, -float(leg[1]))
		var leg_ok := await _go_to(func() -> Vector3: return goal, 2.0, WALK_TIMEOUT_MS)
		_ctx.log("route leg %d/%d to mission (%.1f, %.1f): %s" % [i + 1, _route.size(), float(leg[0]), float(leg[1]),
				"reached" if leg_ok else "NOT reached"])
	var reached := await _go_to(_truck_position, BOARD_SCAN_RANGE, WALK_TIMEOUT_MS)
	await _sample("gate3_walk")
	await _capture("gate3_truck")
	var detail := "distance to the truck %.2f u after %d ms (travel=%s)" % [
			_truck_distance(), Time.get_ticks_msec() - started, _travel]
	return _gate_done(3, "walk_to_truck", reached, detail, started)


## Board through the USE key's release edge: mounted on `truck_ssn`, then the
## PLYRATTACHED event 2 (the ride kickoff) fires.
func _gate_4_board() -> String:
	var started := Time.get_ticks_msec()
	if _mode == "observe":
		_ctx.log("gate 4: board the truck with USE")
	else:
		for attempt in 3:
			await _face(_truck_position())
			await _tap(KEY_SHIFT)
			var seated := await _wait_until(_mounted_on_truck, 4000, "mounted")
			if seated:
				break
			_ctx.log("board attempt %d: not mounted (distance %.2f u)" % [attempt + 1, _truck_distance()])
			await _go_to(_truck_position, BOARD_SCAN_RANGE - 1.0, 20_000)
	var mounted := await _wait_until(_mounted_on_truck,
			OBSERVE_GATE_TIMEOUT_MS if _mode == "observe" else 2000, "mounted on the truck")
	if not mounted:
		await _capture("gate4_not_mounted")
		return _gate_done(4, "board", false, "USE did not mount the player on SSN %d (distance %.2f u)" % [
				_truck_ssn, _truck_distance()], started)
	var fired := await _wait_until(func() -> bool:
		var s := _ctx.sim()
		return s != null and s.has_event_fired(2), 15_000, "event 2 PLYRATTACHED")
	await _sample("gate4_board")
	await _capture("gate4_mounted")
	var card := _player_card()
	var detail := "mounted seat=%d type=%d; event 2 fired=%s" % [
			card.get_mount_seat() if card != null else -1, card.get_mount_type() if card != null else -1, str(fired)]
	return _gate_done(4, "board", fired, detail, started)


## The instructor-driven ride: the truck moves, the rider is carried, the
## fired-event set grows and the area-triggered text arrives.
func _gate_5_ride() -> String:
	var started := Time.get_ticks_msec()
	var truck0 := _truck_position()
	var ride_u := 0.0
	var max_gap := 0.0
	var fired_before := _fired_count()
	var texts_before := _effect_count("text")
	var dialogs_before := _effect_count("dialog")
	var deadline := Time.get_ticks_msec() + RIDE_TIMEOUT_MS
	var still_since := -1
	var last_truck := truck0
	var first_text_captured := false
	while Time.get_ticks_msec() < deadline and not _ctx.cancelled:
		await _wait_sampling(SAMPLE_MS)
		var truck_now := _truck_position()
		ride_u = maxf(ride_u, _planar(truck_now, truck0))
		var sim := _ctx.sim()
		if sim != null:
			max_gap = maxf(max_gap, _planar(sim.get_local_player_position(), truck_now))
		if not first_text_captured and _effect_count("text") > texts_before:
			first_text_captured = true
			await _capture("gate5_first_text")
		# Arrival: the truck has driven and then stood still for ARRIVAL_STILL_MS.
		if ride_u >= 8.0:
			if _planar(truck_now, last_truck) < 0.05:
				if still_since < 0:
					still_since = Time.get_ticks_msec()
				elif Time.get_ticks_msec() - still_since >= ARRIVAL_STILL_MS:
					break
			else:
				still_since = -1
		last_truck = truck_now
		if not _mounted_on_truck():
			break
	await _sample("gate5_ride")
	await _capture("gate5_ride_end")
	var fired_after := _fired_count()
	var texts := _effect_count("text") - texts_before
	var dialogs := _effect_count("dialog") - dialogs_before
	var detail := "truck drove %.1f u; rider gap max %.1f u; events fired %d -> %d; text effects %d (%s); dialog effects %d" % [
			ride_u, max_gap, fired_before, fired_after, texts, str(_text_lines()), dialogs]
	if ride_u < 8.0:
		return _gate_done(5, "ride", false, "the truck did not drive (%.1f u)" % ride_u, started)
	if max_gap > 10.0:
		return _gate_done(5, "ride", false, "the rider was not carried (gap %.1f u)" % max_gap, started)
	return _gate_done(5, "ride", fired_after > fired_before, detail, started)


## Arrival and dismount: the truck at rest, USE releases the seat.
func _gate_6_arrival_dismount() -> String:
	var started := Time.get_ticks_msec()
	if _mode == "observe":
		_ctx.log("gate 6: dismount at the range")
	elif _mounted_on_truck():
		await _tap(KEY_SHIFT)
	var down := await _wait_until(func() -> bool: return not _mounted_on_truck(),
			OBSERVE_GATE_TIMEOUT_MS if _mode == "observe" else 5000, "dismounted")
	# The camera returns to the body over a few frames; let it settle before
	# the next gate steers on it.
	await _wait_sampling(1500)
	await _sample("gate6_dismount")
	await _capture("gate6_dismounted")
	return _gate_done(6, "arrival_dismount", down, "mounted=%s at %s" % [
			str(_mounted_on_truck()), str(_ctx.sim().get_local_player_position()) if _ctx.sim() != null else "?"], started)


## The armory: the player standing in a type-6 volume, USE opening weapon.mnu
## over live play, ESC closing it back to the world.
func _gate_7_armory() -> String:
	var started := Time.get_ticks_msec()
	var in_zone := false
	if _mode == "observe":
		_ctx.log("gate 7: walk into the armory and open it with USE")
		in_zone = await _wait_until(func() -> bool:
			var s := _ctx.sim()
			return s != null and s.local_player_in_armory_zone(), OBSERVE_GATE_TIMEOUT_MS, "observe: armory zone")
	else:
		var sim := _ctx.sim()
		in_zone = sim != null and sim.local_player_in_armory_zone()
		if not in_zone:
			# No authored armory position is known to the probe: probe the
			# range around the dismount point for the zone, bounded.
			in_zone = await _search_armory_zone(90_000)
	if not in_zone:
		await _capture("gate7_no_zone")
		return _gate_done(7, "armory", false, "the player never stood in an armory zone", started)
	if _mode != "observe":
		await _tap(KEY_SHIFT)
	var opened := await _wait_until(func() -> bool:
		var g := _ctx.game()
		return g != null and g.shell_state_name() == "armory",
			OBSERVE_GATE_TIMEOUT_MS if _mode == "observe" else 5000, "armory open")
	await _capture("gate7_armory")
	if opened and _mode != "observe":
		await _tap(KEY_ESCAPE)
	var closed := await _wait_until(func() -> bool:
		var g := _ctx.game()
		return g != null and g.shell_state_name() == "world",
			OBSERVE_GATE_TIMEOUT_MS if _mode == "observe" else 5000, "armory closed")
	await _sample("gate7_armory")
	var weapon := _ctx.sim().get_local_player_weapon_name() if _ctx.sim() != null else ""
	return _gate_done(7, "armory", opened and closed, "zone=%s opened=%s closed=%s weapon=%s" % [
			str(in_zone), str(opened), str(closed), weapon], started)


## The authored failure: real rounds at a blue person until the WAC
## `true(bluekills) -> Lose(1)` ends the round with winner 2, the lose and
## round_end effects arrive, and the shell parks gameplay input.
func _gate_8_friendly_fire_failure() -> String:
	var started := Time.get_ticks_msec()
	var ended := false
	if _mode == "observe":
		_ctx.log("gate 8: shoot a friendly (blue) soldier until the mission fails")
		ended = await _wait_until(_round_ended, OBSERVE_GATE_TIMEOUT_MS, "observe: round ended")
	else:
		ended = await _fire_at_blue_until_ended(FAILURE_TIMEOUT_MS)
	await _sample("gate8_failure")
	await _capture("gate8_failed")
	var sim := _ctx.sim()
	var outcome: RoundOutcome = sim.get_round_outcome_debug() if sim != null else null
	if not ended or outcome == null:
		return _gate_done(8, "friendly_fire_failure", false, "the round did not end (%s)" % _outcome_text(), started)
	var lose := _effect_count("lose") > 0
	var round_end := _effect_count("round_end") > 0
	var input_parked := await _wait_until(func() -> bool:
		var g := _ctx.game()
		return g != null and not g.is_gameplay_input_active(), 5000, "gameplay input parked")
	var screen := await _wait_until(func() -> bool: return _end_screen() != null, END_SCREEN_WAIT_MS, "end screen")
	await _wait_sampling(1500)
	await _capture("gate8_end_screen")
	var banner := _ctx.hud_presenter().endround_banner_line() if _ctx.hud_presenter() != null else ""
	var detail := "%s; lose=%s round_end=%s input_parked=%s end_screen=%s banner=%s" % [
			_outcome_text(), str(lose), str(round_end), str(input_parked), str(screen), banner]
	var ok := outcome.get_winner_team() == 2 and outcome.get_bluekills() >= 1 and lose and round_end \
			and input_parked and screen
	return _gate_done(8, "friendly_fire_failure", ok, detail, started)


## The clean exit: ESC on the end screen returns to the menu with the world
## unloaded and no script errors in the run's log.
func _gate_9_clean_exit() -> String:
	var started := Time.get_ticks_msec()
	if _mode == "observe":
		_ctx.log("gate 9: leave the end screen to the menu")
	else:
		await _tap(KEY_ESCAPE)
	var menu := await _wait_until(func() -> bool:
		var g := _ctx.game()
		var w := _ctx.world()
		return g != null and g.shell_state_name() == "menu" and (w == null or not w.is_loaded()),
			OBSERVE_GATE_TIMEOUT_MS if _mode == "observe" else MENU_WAIT_MS, "menu")
	await _ctx.wait_ms(1000)
	await _capture("gate9_menu")
	var g := _ctx.game()
	return _gate_done(9, "clean_exit", menu, "shell=%s world_loaded=%s" % [
			g.shell_state_name() if g != null else "?",
			str(_ctx.world() != null and _ctx.world().is_loaded())], started)


## The repeat launch (retry stand-in): the same mission again from the menu
## resets the outcome, the fired events, the spawn pose and the truck's rest.
func _gate_10_repeat_launch() -> String:
	var started := Time.get_ticks_msec()
	var start := _ctx.start_mission(_mission)
	if start != OK:
		return _gate_done(10, "repeat_launch", false, "start_mission failed: %s" % error_string(start), started)
	if not await _ctx.wait_for_local_player(LOAD_TIMEOUT_MS):
		return _gate_done(10, "repeat_launch", false, "no local player on the repeat launch", started)
	_connect_effects()
	await _ctx.wait_ms(1500)
	var check := await _fresh_launch_check("gate10_repeat", "gate10_respawn")
	return _gate_done(10, "repeat_launch", bool(check[0]), String(check[1]), started)


## The in-game RESTART: ESC opens the in-game menu, its RESTART button restarts
## the same mission with no menu between (the engine's
## World::ingame_restart_command and the main frame's reason-4 route); the
## restarted world is a fresh launch (the spawn, the truck's rest, the boot
## fired set, the round live), checked like gate 10.
func _gate_11_ingame_restart() -> String:
	var started := Time.get_ticks_msec()
	var before := _ctx.sim()
	if _mode == "observe":
		_ctx.log("gate 11: open the in-game menu (ESC) and press RESTART")
	else:
		await _tap(KEY_ESCAPE)
		var menu_up := await _wait_until(func() -> bool:
			var g := _ctx.game()
			return g != null and g.shell_state_name() == "paused", 5000, "in-game menu")
		await _ctx.wait_ms(500)
		await _capture("gate11_menu")
		var pressed := menu_up and _ctx.menu_shell() != null \
				and _ctx.menu_shell().menu_press("RESTART")
		if not pressed:
			return _gate_done(11, "ingame_restart", false,
					"menu_up=%s: the RESTART control was not pressed" % str(menu_up), started)
	var restarted := await _wait_until(func() -> bool:
		var g := _ctx.game()
		var w := _ctx.world()
		var sim := _ctx.sim()
		return sim != null and sim != before and w != null and w.is_loaded() \
				and g != null and g.shell_state_name() == "world",
			OBSERVE_GATE_TIMEOUT_MS if _mode == "observe" else LOAD_TIMEOUT_MS, "restarted world")
	if not restarted or not await _ctx.wait_for_local_player(LOAD_TIMEOUT_MS):
		return _gate_done(11, "ingame_restart", false, "the mission did not restart", started)
	_connect_effects()
	await _ctx.wait_ms(1500)
	var check := await _fresh_launch_check("gate11_restart", "gate11_respawn")
	var g := _ctx.game()
	var detail := "%s; shell=%s" % [String(check[1]), g.shell_state_name() if g != null else "?"]
	return _gate_done(11, "ingame_restart", bool(check[0]), detail, started)


## The fresh-launch checks over the loaded world: the spawn and the truck's rest
## at the first launch's, the boot fired set (the trigger-less events the
## PreMission pass fires at every boot; none of the ride or failure events),
## and the round live. Returns [ok, detail].
func _fresh_launch_check(sample_label: String, capture_label: String) -> Array:
	var sim := _ctx.sim()
	var spawn := sim.get_local_player_position()
	var truck := sim.entity_card_by_net_id(_truck_ssn)
	var truck_pos := truck.get_position() if truck != null else Vector3.INF
	var outcome: RoundOutcome = sim.get_round_outcome_debug()
	var boot_fired := _fired_indices()
	await _sample(sample_label)
	await _capture(capture_label)
	var spawn_gap := _planar(spawn, _spawn_pos)
	var truck_gap := _planar(truck_pos, _truck_rest) if truck != null else INF
	var same_boot_set := boot_fired == _boot_fired
	var detail := "spawn gap %.2f u; truck gap %.2f u; fired at boot %s (first launch %s); ended=%s" % [
			spawn_gap, truck_gap, str(boot_fired), str(_boot_fired), str(outcome != null and outcome.get_ended())]
	var ok := spawn_gap <= 1.0 and truck_gap <= 1.0 and same_boot_set \
			and outcome != null and not outcome.get_ended()
	return [ok, detail]


# --- drivers ---------------------------------------------------------------------------

## One look of `px` through the sim's accumulator, and the camera heading
## before/after: the sign and scale of a horizontal look, then of a vertical
## one. "" or the failure text.
func _calibrate_look() -> String:
	var cam := _ctx.camera()
	if cam == null:
		return "no current camera"
	var h0 := _camera_heading_deg()
	_ctx.sim().add_local_player_look(AIM_CALIBRATION_PX, 0.0)
	await _ctx.wait_frames(8)
	var dh := _wrap_deg(_camera_heading_deg() - h0)
	if absf(dh) < 0.2:
		return "a horizontal look of %.0f px did not move the camera" % AIM_CALIBRATION_PX
	_look_sign = signf(dh)
	_look_px_per_deg = AIM_CALIBRATION_PX / absf(dh)
	var p0 := _camera_pitch_deg()
	_ctx.sim().add_local_player_look(0.0, AIM_CALIBRATION_PX)
	await _ctx.wait_frames(8)
	var dp := _camera_pitch_deg() - p0
	if absf(dp) < 0.2:
		return "a vertical look of %.0f px did not move the camera" % AIM_CALIBRATION_PX
	_pitch_sign = signf(dp)
	_pitch_px_per_deg = AIM_CALIBRATION_PX / absf(dp)
	_ctx.sim().add_local_player_look(-AIM_CALIBRATION_PX, -AIM_CALIBRATION_PX)
	await _ctx.wait_frames(4)
	_ctx.log("look calibrated: yaw %.2f px/deg sign %d, pitch %.2f px/deg sign %d" % [
			_look_px_per_deg, int(_look_sign), _pitch_px_per_deg, int(_pitch_sign)])
	return ""


## Turn the camera onto `target` (planar), within AIM_TOLERANCE_DEG, bounded.
func _face(target: Vector3, level := true) -> void:
	for _i in 40:
		if _ctx.cancelled:
			return
		var cam := _ctx.camera()
		var sim := _ctx.sim()
		if cam == null or sim == null:
			return
		var err := _heading_error_deg(target)
		var done := absf(err) <= AIM_TOLERANCE_DEG
		var dy := 0.0
		if level and _pitch_px_per_deg > 0.0:
			var pitch := _camera_pitch_deg()
			if absf(pitch) > AIM_TOLERANCE_DEG:
				dy = clampf(-pitch * _pitch_px_per_deg, -AIM_MAX_STEP_PX, AIM_MAX_STEP_PX) * _pitch_sign
				done = false
		if done:
			return
		var dx := clampf(err * _look_px_per_deg, -AIM_MAX_STEP_PX, AIM_MAX_STEP_PX) * _look_sign
		sim.add_local_player_look(dx, dy)
		await _ctx.wait_frames(3)


## Aim the camera at a world point (yaw and pitch), bounded.
func _aim_at(target: Vector3) -> void:
	for _i in 40:
		if _ctx.cancelled:
			return
		var cam := _ctx.camera()
		var sim := _ctx.sim()
		if cam == null or sim == null:
			return
		var yaw_err := _heading_error_deg(target)
		var to := target - cam.global_transform.origin
		var want_pitch := rad_to_deg(atan2(to.y, Vector2(to.x, to.z).length()))
		var pitch_err := want_pitch - _camera_pitch_deg()
		if absf(yaw_err) <= AIM_TOLERANCE_DEG and absf(pitch_err) <= AIM_TOLERANCE_DEG:
			return
		var dx := clampf(yaw_err * _look_px_per_deg, -AIM_MAX_STEP_PX, AIM_MAX_STEP_PX) * _look_sign
		var dy := clampf(pitch_err * _pitch_px_per_deg, -AIM_MAX_STEP_PX, AIM_MAX_STEP_PX) * _pitch_sign
		sim.add_local_player_look(dx, dy)
		await _ctx.wait_frames(3)


## Get to the moving point `target_fn`, within `stop_within` u (planar), the
## way `travel` says: `walk` steers on the camera through the real keys,
## `teleport` moves the player through the debug teleport (a navigation
## stand-in: the point is landed on and the look then turns onto the target
## through the look accumulator). `stop_when` ends either early.
func _go_to(target_fn: Callable, stop_within: float, timeout_ms: int,
		stop_when: Callable = Callable()) -> bool:
	if _travel != "teleport":
		return await _walk_to(target_fn, stop_within, timeout_ms, stop_when)
	var sim := _ctx.sim()
	if sim == null:
		return false
	var target: Vector3 = target_fn.call()
	var me := sim.get_local_player_position()
	var to := target - me
	to.y = 0.0
	var direction := to.normalized() if to.length() > 0.01 else Vector3.RIGHT
	# Land short of the point by most of the allowance (right on it when the
	# allowance is a firing spot's), at the target's own height.
	var short := stop_within * 0.8 if stop_within > 1.5 else 0.0
	var landing := target - direction * short
	var mission_pos := Vector3(landing.x, -landing.z, target.y + 0.3)
	var err := sim.debug_teleport_local_player(mission_pos, sim.get_local_player_yaw_deg(), 0.0)
	if err != OK:
		_ctx.log("teleport to %s refused: %s" % [str(landing), error_string(err)])
		return false
	await _wait_sampling(500)
	await _face(target)
	await _wait_sampling(300)
	sim = _ctx.sim()
	if sim == null:
		return false
	var here := sim.get_local_player_position()
	var d := _planar(here, target_fn.call())
	_ctx.log("teleported to %s (%.2f u from the target)" % [str(here), d])
	return d <= stop_within + 1.0 or (stop_when.is_valid() and bool(stop_when.call()))


## Walk on foot toward the moving point `target_fn` until within `stop_within`
## u (planar): steer on the camera every few frames, detect a stall and try to
## walk around it. False on timeout or cancellation.
func _walk_to(target_fn: Callable, stop_within: float, timeout_ms: int,
		stop_when: Callable = Callable()) -> bool:
	var deadline := Time.get_ticks_msec() + timeout_ms
	var sim := _ctx.sim()
	if sim == null:
		return false
	var last_pos := sim.get_local_player_position()
	var last_move_check := Time.get_ticks_msec()
	var detours := 0
	while Time.get_ticks_msec() < deadline and not _ctx.cancelled:
		sim = _ctx.sim()
		if sim == null:
			return false
		var target: Vector3 = target_fn.call()
		var pos := sim.get_local_player_position()
		if _planar(pos, target) <= stop_within or (stop_when.is_valid() and bool(stop_when.call())):
			_hold(KEY_W, false)
			return true
		await _face(target)
		_hold(KEY_W, true)
		await _wait_sampling(400)
		var now := Time.get_ticks_msec()
		if now - last_move_check >= STUCK_WINDOW_MS:
			sim = _ctx.sim()
			if sim == null:
				return false
			var here := sim.get_local_player_position()
			if _planar(here, last_pos) < STUCK_MIN_MOVE:
				detours += 1
				_ctx.log("walk: stalled at %s (%.1f u from the target), detour %d" % [
						str(here), _planar(here, target), detours])
				await _detour(detours)
			last_pos = here
			last_move_check = now
	_hold(KEY_W, false)
	return false


## A stall: back off the obstacle, then walk one leg along it — a heading
## offset from the target bearing that grows with every consecutive stall
## (90, 135, 180 degrees, alternating the side every three legs) — so a wall,
## a fence or a doorway edge gets walked around; the caller re-aims at the
## target after every leg. Positions are logged so a leg that moved nothing
## (a wedge, a blocked key) is visible in the run's log.
func _detour(attempt: int) -> void:
	_hold(KEY_W, false)
	var sim := _ctx.sim()
	if sim == null:
		return
	var before := sim.get_local_player_position()
	_hold(KEY_S, true)
	await _wait_sampling(700)
	_hold(KEY_S, false)
	var leg := (attempt - 1) % 6
	var side := 1.0 if ((attempt - 1) / 3) % 2 == 0 else -1.0
	var offset := side * (90.0 + 45.0 * float(leg % 3))
	await _turn_by(offset)
	await _tap(KEY_SPACE)
	_hold(KEY_W, true)
	await _wait_sampling(2200)
	_hold(KEY_W, false)
	sim = _ctx.sim()
	if sim != null:
		var after := sim.get_local_player_position()
		_ctx.log("detour %d: back-off then %+.0f deg leg moved %.2f u (%s -> %s)" % [
				attempt, offset, _planar(after, before), str(before), str(after)])


## Turn the camera by `delta_deg` (planar), in bounded steps.
func _turn_by(delta_deg: float) -> void:
	var want := _wrap_deg(_camera_heading_deg() + delta_deg)
	for _i in 60:
		if _ctx.cancelled:
			return
		var sim := _ctx.sim()
		if sim == null or _look_px_per_deg <= 0.0:
			return
		var err := _wrap_deg(want - _camera_heading_deg())
		if absf(err) <= AIM_TOLERANCE_DEG:
			return
		var dx := clampf(err * _look_px_per_deg, -AIM_MAX_STEP_PX, AIM_MAX_STEP_PX) * _look_sign
		sim.add_local_player_look(dx, 0.0)
		await _ctx.wait_frames(2)


## Look for the armory zone from where the ride ended: walk to the placed
## armory items nearest the player (their item names carry "Armory"), ending
## as soon as the sim reports the local player inside a type-6 volume. False
## when the budget runs out or no armory item is placed.
func _search_armory_zone(timeout_ms: int) -> bool:
	var deadline := Time.get_ticks_msec() + timeout_ms
	var sim := _ctx.sim()
	if sim == null:
		return false
	var me := sim.get_local_player_position()
	var armories: Array[Dictionary] = []
	for row_v in sim.entity_directory():
		var row: EntityRow = row_v
		if row.get_item_name().findn("armory") < 0 or not row.is_alive():
			continue
		var pos := row.get_world_position()
		armories.append({ "net_id": row.get_net_id(), "name": row.get_item_name(),
				"pos": pos, "d": _planar(pos, me) })
	armories.sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return float(a.d) < float(b.d))
	_ctx.log("armory items by distance: %s" % str(armories.map(
			func(a: Dictionary) -> String: return "%s#%d@%.0fu" % [a.name, a.net_id, a.d])))
	var in_zone := func() -> bool:
		var s := _ctx.sim()
		return s != null and s.local_player_in_armory_zone()
	for armory in armories:
		if Time.get_ticks_msec() >= deadline or _ctx.cancelled:
			break
		var goal: Vector3 = armory.pos
		var reached := await _go_to(func() -> Vector3: return goal, 2.0,
				mini(60_000, deadline - Time.get_ticks_msec()), in_zone)
		if bool(in_zone.call()):
			return true
		# The volume may sit beside the item rather than on it: try the four
		# sides at a few units.
		for side in [Vector3(4, 0, 0), Vector3(-4, 0, 0), Vector3(0, 0, 4), Vector3(0, 0, -4)]:
			if Time.get_ticks_msec() >= deadline or _ctx.cancelled:
				break
			var beside: Vector3 = goal + side
			await _go_to(func() -> Vector3: return beside, 1.2,
					mini(20_000, deadline - Time.get_ticks_msec()), in_zone)
			if bool(in_zone.call()):
				return true
		_ctx.log("armory %s#%d: %s, not in a zone" % [armory.name, armory.net_id,
				"reached" if reached else "not reached"])
	return false


## Real rounds at the nearest blue person until the round ends. Retargets a
## person whose health does not move (scripted indestructibles).
func _fire_at_blue_until_ended(timeout_ms: int) -> bool:
	var deadline := Time.get_ticks_msec() + timeout_ms
	var blacklist: Array[int] = []
	while Time.get_ticks_msec() < deadline and not _ctx.cancelled:
		if _round_ended():
			return true
		var target := _nearest_blue_person(blacklist)
		if target == null:
			_ctx.log("failure gate: no blue person available (blacklist %s)" % str(blacklist))
			await _wait_sampling(2000)
			continue
		# A person riding a moving vehicle is no target for a staged volley:
		# leave it for later and take the next one.
		var pos_before := target.get_position()
		await _wait_sampling(600)
		var moved_card := _ctx.sim().entity_card_by_ai_index(target.get_ai_index())
		if moved_card != null and _planar(moved_card.get_position(), pos_before) > 0.5:
			_ctx.log("failure gate: net=%d is moving (%.1f u in 0.6 s); deferring it" % [
					target.get_net_id(), _planar(moved_card.get_position(), pos_before)])
			blacklist.append(target.get_ai_index())
			continue
		var ai_index := target.get_ai_index()
		var net_id := target.get_net_id()
		_ctx.log("failure gate: targeting blue person net=%d ai=%d hp=%d at %s mounted=%s on %d" % [
				net_id, ai_index, target.get_health(), str(target.get_position()), str(target.is_mounted()),
				target.get_mount_target_net_id()])
		var hp0 := target.get_health()
		# The firing positions: eight bearings around the person at a few
		# units (the lose_flow ctest's staging, walked to instead of teleported);
		# a seated person's mount hull hides it from some of them, so a bearing
		# that lands no round yields to the next. Positions inside the mount's
		# own footprint are skipped.
		var mount_pos := Vector3.INF
		if target.is_mounted():
			var mount := _ctx.sim().entity_card_by_net_id(target.get_mount_target_net_id())
			if mount != null:
				mount_pos = mount.get_position()
		var outcome := ""
		for k in FIRE_BEARINGS:
			if Time.get_ticks_msec() >= deadline or _ctx.cancelled:
				break
			var card := _ctx.sim().entity_card_by_ai_index(ai_index)
			if card == null or not card.is_alive():
				break
			var angle := float(k) * TAU / float(FIRE_BEARINGS)
			var goal := card.get_position() + Vector3(cos(angle), 0.0, sin(angle)) * FIRE_STAGE_DISTANCE
			if mount_pos != Vector3.INF and _planar(goal, mount_pos) < FIRE_HULL_CLEARANCE:
				continue
			var there := await _go_to(func() -> Vector3: return goal, 1.2,
					mini(FIRE_BEARING_WALK_MS, deadline - Time.get_ticks_msec()))
			if not there:
				_ctx.log("failure gate: bearing %d not reachable" % k)
				continue
			outcome = await _volley_at(ai_index, net_id, deadline)
			_ctx.log("failure gate: bearing %d -> %s" % [k, outcome])
			if outcome == "ended" or outcome == "dead":
				break
		if outcome == "ended" or _round_ended():
			return true
		var after := _ctx.sim().entity_card_by_ai_index(ai_index)
		if after != null and after.is_alive() and after.get_health() >= hp0:
			_ctx.log("failure gate: net=%d took no damage from any bearing; retargeting" % net_id)
			blacklist.append(ai_index)
			continue
		# Dead or damaged: give the WAC tick its second.
		var ended := await _wait_until(_round_ended, 4000, "round end after the kill")
		if ended:
			return true
		if after == null or not after.is_alive():
			_ctx.log("failure gate: net=%d died but the round did not end (%s)" % [net_id, _outcome_text()])
			blacklist.append(ai_index)
	return _round_ended()


## Bursts at the person from where the player stands until the round ends
## ("ended"), the person dies ("dead") or the bearing's burst budget is spent
## ("dry"); a spent magazine is reloaded through the real binding every three
## bursts that moved neither health read (the auto-reload never resumes a
## volley). A seated person's card health can stay put while the occupant
## damage accumulates, so the budget, not the read, ends a bearing.
func _volley_at(ai_index: int, net_id: int, deadline: int) -> String:
	var bursts := 0
	var dry := 0
	var reloads := 0
	while bursts < FIRE_MAX_BURSTS and Time.get_ticks_msec() < deadline and not _ctx.cancelled:
		var card := _ctx.sim().entity_card_by_ai_index(ai_index)
		if card == null or not card.is_alive():
			return "dead"
		var hp_before := card.get_health()
		var ai_hp_before := card.get_ai_health()
		await _aim_at(_target_aim_point(card))
		_mouse(true)
		await _wait_sampling(350)
		_mouse(false)
		await _wait_sampling(500)
		bursts += 1
		if _round_ended():
			return "ended"
		card = _ctx.sim().entity_card_by_ai_index(ai_index)
		if card == null or not card.is_alive():
			return "dead"
		var me := _ctx.sim().get_local_player_position()
		_ctx.log("burst %d at net=%d: hp %d -> %d (ai %d -> %d), aim %s from %s (%.1f u)" % [bursts, net_id,
				hp_before, card.get_health(), ai_hp_before, card.get_ai_health(), str(_target_aim_point(card)),
				str(me), _planar(me, card.get_position())])
		if card.get_health() >= hp_before and card.get_ai_health() >= ai_hp_before:
			dry += 1
		else:
			dry = 0
		if dry > 0 and dry % 3 == 0 and reloads < 3:
			_ctx.log("failure gate: %d dry bursts at net=%d, reloading" % [dry, net_id])
			await _tap(KEY_R)
			await _wait_sampling(3500)
			reloads += 1
	return "dry"


## Where to aim at a person: its widest posed hit section (the sphere the
## round's person leg walks, the lose_flow ctest's aim point) from the hitbox
## oracle, which sits well below a seated person's entity position; the
## entity position plus a pelvis-to-chest lift when the oracle has no row.
func _target_aim_point(card: EntityCard) -> Vector3:
	var sim := _ctx.sim()
	var best := Vector3.INF
	var best_radius := 0.0
	if sim != null:
		var report: HitboxDebugReport = sim.get_hitbox_debug()
		if report != null:
			for row_v in report.get_organics():
				var row: HitboxDebugOrganic = row_v
				if row.get_entity_handle() != card.get_wire_handle() or row.get_masked():
					continue
				if row.get_radius() > best_radius:
					best_radius = row.get_radius()
					best = row.get_pos()
	if best == Vector3.INF:
		return card.get_position() + Vector3(0, 0.3, 0)
	return best


func _nearest_blue_person(blacklist: Array[int]) -> EntityCard:
	var sim := _ctx.sim()
	if sim == null:
		return null
	var me := sim.get_local_player_position()
	var best: EntityCard = null
	var best_d := INF
	var considered: Array[String] = []
	for row_v in sim.entity_directory():
		var row: EntityRow = row_v
		if not row.is_alive() or row.get_team() != TEAM_BLUE:
			continue
		if row.get_ai_index() < 0 or blacklist.has(row.get_ai_index()):
			continue
		if _target_ssn > 0 and row.get_net_id() != _target_ssn:
			continue
		# A seated person (00TRa's two friendlies are the trucks' drivers) is a
		# target too: the rounds reach the occupant through the vehicle. The
		# card's infantry bit is the motor's, parked while seated, so the row's
		# kind selects people.
		var card := sim.entity_card_by_ai_index(row.get_ai_index())
		if card == null or card.get_pool() != 0 or row.get_kind() != ENTITY_KIND_ORGANIC:
			continue
		if card.get_wire_handle() == sim.get_local_player_wire_handle():
			continue
		var d := _planar(card.get_position(), me)
		considered.append("net=%d kind=%d ai=%d inf=%s mounted=%s hp=%d d=%.0f" % [row.get_net_id(), row.get_kind(),
				row.get_ai_index(), str(card.is_infantry()), str(card.is_mounted()), card.get_health(), d])
		if d < 0.5 or d >= best_d:
			continue
		best_d = d
		best = card
	if best == null and not _blue_candidates_logged:
		_blue_candidates_logged = true
		_ctx.log("blue rows considered: %s" % str(considered))
	return best


# --- readers ---------------------------------------------------------------------------

func _player_card() -> EntityCard:
	var sim := _ctx.sim()
	if sim == null or not sim.has_local_player():
		return null
	return sim.entity_card(sim.get_local_player_wire_handle())


func _mounted_on_truck() -> bool:
	var card := _player_card()
	return card != null and card.is_mounted() and card.get_mount_target_net_id() == _truck_ssn


func _truck_position() -> Vector3:
	var sim := _ctx.sim()
	if sim == null:
		return Vector3.ZERO
	var truck := sim.entity_card_by_net_id(_truck_ssn)
	return truck.get_position() if truck != null else Vector3.ZERO


func _truck_distance() -> float:
	var sim := _ctx.sim()
	if sim == null:
		return INF
	return _planar(sim.get_local_player_position(), _truck_position())


func _round_ended() -> bool:
	var sim := _ctx.sim()
	if sim == null:
		return false
	var outcome: RoundOutcome = sim.get_round_outcome_debug()
	return outcome != null and outcome.get_ended()


func _outcome_text() -> String:
	var sim := _ctx.sim()
	if sim == null:
		return "no sim"
	var o: RoundOutcome = sim.get_round_outcome_debug()
	if o == null:
		return "no outcome"
	return "ended=%s winner=%d bluekills=%d greenkills=%d enemy=%d" % [
			str(o.get_ended()), o.get_winner_team(), o.get_bluekills(), o.get_greenkills(), o.get_enemy_kills()]


func _end_screen() -> Node:
	if _ctx.tree == null:
		return null
	return _ctx.tree.root.find_child("MissionEndScreen", true, false)


func _fired_count() -> int:
	var sim := _ctx.sim()
	if sim == null:
		return 0
	var snapshot: PackedByteArray = sim.get_fired_events_snapshot()
	var count := 0
	for i in snapshot.size():
		if snapshot[i] != 0:
			count += 1
			if not _fired_seen.has(i):
				_fired_seen.append(i)
				_ctx.log("event %d fired (tick %d)" % [i, sim.get_logic_tick()])
	return count


## The indices of the events fired right now (no logging, no memory).
func _fired_indices() -> Array[int]:
	var out: Array[int] = []
	var sim := _ctx.sim()
	if sim == null:
		return out
	var snapshot: PackedByteArray = sim.get_fired_events_snapshot()
	for i in snapshot.size():
		if snapshot[i] != 0:
			out.append(i)
	return out


func _effect_count(kind: String) -> int:
	var n := 0
	for e in _effects:
		if String(e.kind) == kind:
			n += 1
	return n


func _text_lines() -> Array[String]:
	var out: Array[String] = []
	for e in _effects:
		if String(e.kind) == "text":
			out.append(String(e.line))
	return out


## The "Triggered Text" line for a mission text id, or "" (the HUD's own
## lookup: HUD_DisplayTriggeredText reads ID%03i from the mission table).
func _mission_string(section: String, key: String) -> String:
	return Strings.lookup_or(Strings.TABLE_MISSION, section, key, "")


func _connect_effects() -> void:
	var world := _ctx.world()
	if world == null or world.get_instance_id() == _effects_world_id:
		return
	_effects_world_id = world.get_instance_id()
	world.mission_effects.connect(_on_mission_effects)


func _on_mission_effects(effects: Array) -> void:
	for e_v in effects:
		var e := e_v as MissionEffect
		if e == null:
			continue
		var kind := e.get_kind()
		if kind != "text" and kind != "dialog" and kind != "lose" and kind != "win" and kind != "round_end":
			continue
		var entry := { "kind": kind, "a": e.get_a(), "b": e.get_b(), "text": e.get_text(), "line": "" }
		if kind == "text":
			entry["line"] = _mission_string("Triggered Text", "ID%03d" % e.get_a())
		_effects.append(entry)
		_ctx.log("effect %s a=%d str=%s line=%s" % [kind, e.get_a(), e.get_text(), String(entry.line)])


func _camera_heading_deg() -> float:
	var cam := _ctx.camera()
	if cam == null:
		return 0.0
	var fwd := -cam.global_transform.basis.z
	return rad_to_deg(atan2(fwd.x, -fwd.z))


func _camera_pitch_deg() -> float:
	var cam := _ctx.camera()
	if cam == null:
		return 0.0
	var fwd := -cam.global_transform.basis.z
	return rad_to_deg(atan2(fwd.y, Vector2(fwd.x, fwd.z).length()))


## Signed heading error from the camera forward to `target`, degrees, planar.
func _heading_error_deg(target: Vector3) -> float:
	var cam := _ctx.camera()
	if cam == null:
		return 0.0
	var to := target - cam.global_transform.origin
	var want := rad_to_deg(atan2(to.x, -to.z))
	return _wrap_deg(want - _camera_heading_deg())


static func _wrap_deg(d: float) -> float:
	return wrapf(d, -180.0, 180.0)


## A JSON-facing position (the transport carries no Vector3).
static func _v3(v: Vector3) -> Array:
	return [snappedf(v.x, 0.01), snappedf(v.y, 0.01), snappedf(v.z, 0.01)]


static func _planar(a: Vector3, b: Vector3) -> float:
	return Vector2(a.x, a.z).distance_to(Vector2(b.x, b.z))


# --- input ------------------------------------------------------------------------------

func _hold(key: Key, down: bool) -> void:
	ProbeInput.hold(key, down)
	if down and not _held.has(key):
		_held.append(key)
	elif not down:
		_held.erase(key)


func _tap(key: Key) -> void:
	_hold(key, true)
	await _ctx.wait_frames(4)
	_hold(key, false)
	await _ctx.wait_frames(2)


func _mouse(down: bool) -> void:
	ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, down)
	_mouse_held = down


func _release_all() -> void:
	for key in _held.duplicate():
		ProbeInput.hold(key, false)
	_held.clear()
	if _mouse_held:
		ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false)
		_mouse_held = false


# --- waits, samples, artifacts -------------------------------------------------------

func _wait_until(pred: Callable, timeout_ms: int, label: String) -> bool:
	var deadline := Time.get_ticks_msec() + timeout_ms
	while not _ctx.cancelled and _ctx.tree != null:
		if bool(pred.call()):
			return true
		if Time.get_ticks_msec() >= deadline:
			_ctx.log("timeout waiting for %s (%d ms)" % [label, timeout_ms])
			return false
		await _heartbeat_frame()
	return false


func _wait_sampling(duration_ms: int) -> void:
	var deadline := Time.get_ticks_msec() + duration_ms
	while Time.get_ticks_msec() < deadline and not _ctx.cancelled and _ctx.tree != null:
		await _heartbeat_frame()


func _heartbeat_frame() -> void:
	var now := Time.get_ticks_msec()
	if now - _last_heartbeat >= HEARTBEAT_MS:
		_last_heartbeat = now
		_ctx.progress(_witness())
	if now - _last_sample >= SAMPLE_MS:
		_last_sample = now
		_record_sample("tick")
	await _ctx.tree.process_frame


func _sample(stage: String) -> void:
	_record_sample(stage)
	await _ctx.tree.process_frame


func _record_sample(stage: String) -> void:
	var row := _witness()
	row["stage"] = stage
	row["ms"] = Time.get_ticks_msec()
	if _jsonl != null:
		_jsonl.store_line(JSON.stringify(row))
	_samples += 1


func _witness() -> Dictionary:
	var sim := _ctx.sim()
	var g := _ctx.game()
	var card := _player_card()
	var w := {
		"mission": _mission,
		"mode": _mode,
		"travel": _travel,
		"gate": _gates.size(),
		"gates_passed": _gates.filter(func(x: Dictionary) -> bool: return bool(x.ok)).size(),
		"shell": g.shell_state_name() if g != null else "",
		"gameplay_input": g.is_gameplay_input_active() if g != null else false,
		"tick": sim.get_logic_tick() if sim != null else -1,
		"player": _v3(sim.get_local_player_position() if sim != null and sim.has_local_player() else Vector3.ZERO),
		"mounted": card.is_mounted() if card != null else false,
		"mount_target": card.get_mount_target_net_id() if card != null else -1,
		"truck": _v3(_truck_position()),
		"fired_events": _fired_seen.size(),
		"effects": _effects.size(),
		"outcome": _outcome_text(),
		"objective": _ctx.hud_presenter().hud_objective_line() if _ctx.hud_presenter() != null else "",
	}
	return w


func _capture(label: String) -> void:
	var path := _out_dir.path_join("%s.png" % label)
	if await ProbeCapture.save_viewport_png(_ctx.viewport(), path):
		_ctx.artifact(label, path, "png")


func _write_json(label: String, data: Dictionary) -> void:
	var path := _out_dir.path_join("%s.json" % label)
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return
	file.store_string(JSON.stringify(data, "\t"))
	file.close()
	_ctx.artifact(label, path, "json")


func _gate_done(id: int, name: String, ok: bool, detail: String, started_ms: int) -> String:
	_gates.append({ "id": id, "name": name, "ok": ok, "detail": detail,
			"ms": Time.get_ticks_msec() - started_ms })
	_ctx.log("gate %d %s: %s (%s)" % [id, name, "PASS" if ok else "FAIL", detail])
	_ctx.progress(_witness())
	return "" if ok else detail


func _verdict_data(gate_error: String) -> Dictionary:
	return {
		"mission": _mission,
		"mode": _mode,
		"truck_ssn": _truck_ssn,
		"gates": _gates.duplicate(true),
		"stopped_at": gate_error,
		"start_gate": _start_gate,
		"travel": _travel,
		"samples": _samples,
		"fired_events": _fired_seen.duplicate(),
		"effects": _effects.duplicate(true),
		"spawn": _v3(_spawn_pos),
		"truck_rest": _v3(_truck_rest),
		"route": _route.duplicate(),
	}
