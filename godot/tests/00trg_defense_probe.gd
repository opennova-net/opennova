extends SceneTree

# 00TRg "move in to defend" host-side diagnosis probe — NOT a GUT test (needs retail
# assets; *_probe.gd files are manual, never collected). SP is a listen-server, so this
# exercises the exact run_logic_tick(is_authority=true) path a LAN host runs: a retail
# host delivers 00TRg's AI boat/truck defense waves, our host parks them (the
# D-NET-161 deferral tail; D-AI-11 / D-INF-2 residuals).
#
# Mission mechanism (00TRg.mis, decoded 2026-08-06): no vehicle carries an authored
# route; the crews command-mount at spawn (waypoint_id 125 -> ride SSN wpnumber,
# authored ON their vehicles), then BMS events issue the routes:
#   event 18 (no delay) : RedirectGroupTo(5->list 5) + PatrolSpeed 30    patrol boat 60
#   event 19 (delay 15) : RedirectGroupTo(6->list 6) + PatrolSpeed 60    Zodiac 56
#   event 21 (delay 15) : RedirectGroupTo(7->list 7) (+speed on grp 6)   Zodiac 57
#   event 8  (delay 30) : RedirectGroupTo(3->list 3) + PatrolSpeed 40    trucks
#   event 20/22 (GroupAtWaypoint 6@node6 / 7@node10, delay 1): debark redirects for
#   defender groups 14/15 (-> lists 15/16) + red alert.
# Delays arm delay<<6 ticks; triggers evaluate per 64-tick quantum.
#
# Verdict gates (also the fix's acceptance bar):
#   P0 promote : every vehicle has seats + an AI brain row + a live ctrl/drvr occupant
#   P1 orders  : each vehicle brain takes its list (wp_channel) + SM state 16 post-event
#   P2 motion  : planar displacement after the kickoff; boats hold authored z (afloat)
#   P3 arrival : debark events 20/22 fire; defender groups 14/15 take their redirects
#
#   NW_SP_MISSION=00TRg.bms NW_RESOURCE_DIR=<retail JO dir> [NW_EXPANSION=] \
#       [NW_PROBE_TICKS=15000] "$GODOT_BIN" --headless --path godot \
#       -s res://tests/00trg_defense_probe.gd
#
# godot-pos <-> mission-pos: mission = (x, -z, y) of the godot vector; mission z is up.

const LOAD_TIMEOUT_WALL_SECONDS := 300.0
const STALL_TIMEOUT_WALL_SECONDS := 120.0
const AI_SCAN_CAP := 4096
const TIME_SCALE := 10.0
const RUN_TICKS_DEFAULT := 15000
const SNAPSHOT_EVERY_TICKS := 620
const BASELINE_TICK := 400
const BOAT_AUTHORED_Z := 10.5
const BOAT_Z_TOLERANCE := 2.0
const MIN_DEPART_DISTANCE := 15.0
# ssn -> [group, wp list, kickoff event, family label]
const VEHICLES := {
	60: [5, 5, 18, "boat"],
	56: [6, 6, 19, "boat"],
	57: [7, 7, 21, "boat"],
	58: [3, 3, 8, "ground"],
	59: [3, 3, 8, "ground"],
	62: [3, 3, 8, "ground"],
	63: [3, 3, 8, "ground"],
	1664: [3, 3, 8, "ground"],
}
const KICKOFF_EVENTS := [18, 19, 21, 8]
# debark event -> [defender group, redirected-to list]
const DEBARK_EVENTS := {20: [14, 15], 22: [15, 16]}
# The truck follow-on chain (convoy diagnosis): 9 = PATROLSPEED 20 at (3,16),
# 10 = RedirectSingleTo 58->13 / 63->14 at (3,18), 11 = PATROLSPEED 0 +
# infantry redirects at (3,20), 12..15 = the subgoal chain behind (3,20).
const TRUCK_CHAIN_EVENTS := [9, 10, 11, 12, 13, 14, 15]

var _sim = null
var _fail_lines: PackedStringArray = []


func _initialize() -> void:
	call_deferred("_run")


func _fail_boot(msg: String) -> void:
	print("PROBE FAIL: %s" % msg)
	quit(1)


func _wait_until_tick(target: int) -> bool:
	var last_tick := int(_sim.get_logic_tick())
	var last_progress := Time.get_ticks_msec()
	while int(_sim.get_logic_tick()) < target:
		await process_frame
		var now := int(_sim.get_logic_tick())
		if now != last_tick:
			last_tick = now
			last_progress = Time.get_ticks_msec()
		elif float(Time.get_ticks_msec() - last_progress) / 1000.0 > STALL_TIMEOUT_WALL_SECONDS:
			print("PROBE FAIL: sim stalled at tick %d (waiting for %d)" % [now, target])
			return false
	return true


# ssn -> AI-pool index for the pool-1 vehicle brains (missing = no brain attached).
func _map_vehicle_brains() -> Dictionary:
	var out := {}
	for i in AI_SCAN_CAP:
		var d: Dictionary = _sim.get_entity_debug(i)
		if d.is_empty():
			break
		if int(d.get("pool", -1)) != 1:
			continue
		var ssn := int(d.get("bms_id", 0))
		if VEHICLES.has(ssn):
			out[ssn] = i
	return out


# Rows for the command-mount riders: pool-0 organics with waypoint_id 123..125.
func _scan_riders() -> Array:
	var out: Array = []
	for i in AI_SCAN_CAP:
		var d: Dictionary = _sim.get_entity_debug(i)
		if d.is_empty():
			break
		if int(d.get("pool", -1)) != 0:
			continue
		var wid := int(d.get("waypoint_id", 0))
		if wid < 123 or wid > 125:
			continue
		d["ai_index"] = i
		out.append(d)
	return out


func _ctrl_seat_occupied(veh: Dictionary) -> bool:
	for s in veh.get("seats", []):
		# seat types: 1 sitex / 2 ctrl / 3 gun / 5 drvr
		if int(s.get("type", 0)) in [2, 5] and bool(s.get("occupied", false)):
			return true
	return false


func _planar(a: Vector3, b: Vector3) -> float:
	return Vector2(a.x, a.y).distance_to(Vector2(b.x, b.y))


func _gate(name: String, ok: bool, detail: String) -> void:
	print("PROBE VERDICT %s: %s — %s" % [name, "PASS" if ok else "FAIL", detail])
	if not ok:
		_fail_lines.append("%s: %s" % [name, detail])


func _run() -> void:
	if OS.get_environment("NW_SP_MISSION").is_empty():
		_fail_boot("set NW_SP_MISSION=00TRg.bms")
		return
	var res_dir := OS.get_environment("NW_RESOURCE_DIR")
	if not res_dir.is_empty():
		var settings := load("res://adapter/resource_index/resource_dir_settings.gd")
		settings.set_resource_dir(res_dir)
		settings.set_expansion(OS.get_environment("NW_EXPANSION"))
	Engine.time_scale = TIME_SCALE
	var run_ticks := RUN_TICKS_DEFAULT
	if not OS.get_environment("NW_PROBE_TICKS").is_empty():
		run_ticks = int(OS.get_environment("NW_PROBE_TICKS"))

	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		_fail_boot("failed to load main_game.tscn")
		return
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	if world == null:
		_fail_boot("main_game lacks a World child")
		return
	var wall_start := Time.get_ticks_msec()
	while not (world.get_sim() != null and world.get_sim().has_local_player()):
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			_fail_boot("player never spawned (mission load stalled?)")
			return
	_sim = world.get_sim()

	# --- P0 baseline: promote results (seats, brains, crews) settle well before
	# the first event quantum can matter for the kickoffs we watch.
	if not await _wait_until_tick(BASELINE_TICK):
		quit(1)
		return
	var brains := _map_vehicle_brains()
	var riders := _scan_riders()
	print("PROBE t=%d riders(cmd 123..125)=%d" % [int(_sim.get_logic_tick()), riders.size()])
	var riders_by_target := {}
	for r in riders:
		var tgt := int(r.get("wp_number", 0))
		if not riders_by_target.has(tgt):
			riders_by_target[tgt] = []
		riders_by_target[tgt].append(r)
	var p0_ok := true
	var p0_detail: PackedStringArray = []
	for ssn in VEHICLES:
		var veh: Dictionary = _sim.get_world_entity_debug(ssn)
		if veh.is_empty():
			p0_ok = false
			p0_detail.append("ssn %d MISSING from sim" % ssn)
			continue
		var seat_count := int(veh.get("seat_count", 0))
		var has_brain := brains.has(ssn)
		var ctrl := _ctrl_seat_occupied(veh)
		var crew: Array = riders_by_target.get(ssn, [])
		var mounted_crew := 0
		for r in crew:
			if bool(r.get("mounted", false)) and int(r.get("mount_target_net_id", -1)) == ssn:
				mounted_crew += 1
		print("PROBE P0 ssn=%d fam=%s(%d) seats=%d brain=%s ctrl_occupied=%s riders=%d mounted=%d" % [
				ssn, VEHICLES[ssn][3], int(veh.get("vehicle_family", -1)), seat_count,
				str(has_brain), str(ctrl), crew.size(), mounted_crew])
		if seat_count == 0 or not has_brain or not ctrl:
			p0_ok = false
			p0_detail.append("ssn %d: seats=%d brain=%s ctrl=%s (riders=%d mounted=%d)" % [
					ssn, seat_count, str(has_brain), str(ctrl), crew.size(), mounted_crew])
	_gate("P0 promote", p0_ok, "all 8 crewed+brained" if p0_ok else "; ".join(p0_detail))

	# --- Snapshot loop: watch orders land, motion accumulate, events fire.
	var kickoff_fired := {}       # event -> tick observed fired
	var debark_positions := {}    # event -> {ai_index: position at redirect}
	var kickoff_pos := {}         # ssn -> mission position when its kickoff observed
	var order_seen := {}          # ssn -> tick wp_channel==list first observed
	var state16_seen := {}        # ssn -> tick state 16 first observed
	var max_boat_dz := {}         # ssn -> max |z - authored| after kickoff
	var debark_fired := {}        # event -> tick observed
	var next_snapshot := int(_sim.get_logic_tick())
	while int(_sim.get_logic_tick()) < run_ticks:
		if not await _wait_until_tick(next_snapshot):
			quit(1)
			return
		next_snapshot += SNAPSHOT_EVERY_TICKS
		var tick := int(_sim.get_logic_tick())
		var events: PackedByteArray = _sim.get_fired_events_snapshot()
		for ev in KICKOFF_EVENTS:
			if not kickoff_fired.has(ev) and events.size() > ev and events[ev] != 0:
				kickoff_fired[ev] = tick
				print("PROBE t=%d event %d FIRED (kickoff)" % [tick, ev])
				for ssn in VEHICLES:
					if int(VEHICLES[ssn][2]) == ev:
						var vd: Dictionary = _sim.get_world_entity_debug(ssn)
						kickoff_pos[ssn] = vd.get("mission_position", Vector3.ZERO)
		for ev in DEBARK_EVENTS:
			if not debark_fired.has(ev) and events.size() > ev and events[ev] != 0:
				debark_fired[ev] = tick
				print("PROBE t=%d event %d FIRED (debark)" % [tick, ev])
				# Snapshot the defender group's positions at the redirect, so P3
				# can assert they actually WALK (a routed-but-frozen squad fails).
				var grp := int(DEBARK_EVENTS[ev][0])
				var poss := {}
				for i in AI_SCAN_CAP:
					var d: Dictionary = _sim.get_entity_debug(i)
					if d.is_empty():
						break
					if int(d.get("pool", -1)) == 0 and int(d.get("group_id", -1)) == grp:
						poss[i] = d.get("position", Vector3.ZERO)
				debark_positions[ev] = poss
		for ev in TRUCK_CHAIN_EVENTS:
			if not kickoff_fired.has(ev) and events.size() > ev and events[ev] != 0:
				kickoff_fired[ev] = tick
				var chain_rows: PackedStringArray = []
				for tssn in [58, 59, 62, 63, 1664]:
					var tvd: Dictionary = _sim.get_world_entity_debug(tssn)
					var tpos: Vector3 = tvd.get("mission_position", Vector3.ZERO)
					var trow := "ssn=%d (%.0f,%.0f)" % [tssn, tpos.x, tpos.y]
					if brains.has(tssn):
						var tbd: Dictionary = _sim.get_entity_debug(int(brains[tssn]))
						trow += " wp=%d node=%d spd=%d" % [int(tbd.get("wp_channel", -1)),
								int(tbd.get("wp_node", -1)), int(tbd.get("out_speed", 0))]
					chain_rows.append(trow)
				print("PROBE t=%d event %d FIRED (truck chain) | %s" %
						[tick, ev, " | ".join(chain_rows)])
		# Debark forensics: once group 14's redirect fired, watch its members
		# leave the boat — the stuck-exit diagnosis (state/mount/route/motion).
		if debark_fired.has(20):
			var crew_rows: PackedStringArray = []
			var shown := 0
			for i in AI_SCAN_CAP:
				var d: Dictionary = _sim.get_entity_debug(i)
				if d.is_empty():
					break
				if int(d.get("pool", -1)) != 0 or int(d.get("group_id", -1)) != 14:
					continue
				var cpos: Vector3 = d.get("position", Vector3.ZERO)
				crew_rows.append("i=%d (%.1f,%.1f,%.2f) st=%d(%s) mnt=%s wp=%d nd=%d spd=%d anim=%s" % [
						i, cpos.x, cpos.y, cpos.z, int(d.get("state", -1)),
						str(d.get("state_name", "?")), str(d.get("mounted", false)),
						int(d.get("wp_channel", -1)), int(d.get("wp_node", -1)),
						int(d.get("out_speed", 0)), str(d.get("anim_key", "?"))])
				shown += 1
				if shown >= 4:
					break
			print("PROBE t=%d DEBARK14 | %s" % [tick, " | ".join(crew_rows)])
		var lines: PackedStringArray = []
		for ssn in VEHICLES:
			var vd: Dictionary = _sim.get_world_entity_debug(ssn)
			if vd.is_empty():
				continue
			var pos: Vector3 = vd.get("mission_position", Vector3.ZERO)
			var row := "ssn=%d pos=(%.1f,%.1f,%.2f)" % [ssn, pos.x, pos.y, pos.z]
			if brains.has(ssn):
				var bd: Dictionary = _sim.get_entity_debug(int(brains[ssn]))
				var wp_ch := int(bd.get("wp_channel", -1))
				var st := int(bd.get("state", -1))
				row += " st=%d(%s) wp=%d node=%d spd=%d" % [st,
						str(bd.get("state_name", "?")), wp_ch,
						int(bd.get("wp_node", -1)), int(bd.get("out_speed", 0))]
				if wp_ch == int(VEHICLES[ssn][1]) and not order_seen.has(ssn):
					order_seen[ssn] = tick
				if st == 16 and not state16_seen.has(ssn):
					state16_seen[ssn] = tick
			if String(VEHICLES[ssn][3]) == "boat" and kickoff_pos.has(ssn):
				var dz: float = abs(pos.z - BOAT_AUTHORED_Z)
				max_boat_dz[ssn] = max(float(max_boat_dz.get(ssn, 0.0)), dz)
			if kickoff_pos.has(ssn):
				row += " dep=%.1f" % _planar(pos, kickoff_pos[ssn])
			lines.append(row)
		print("PROBE t=%d | %s" % [tick, " | ".join(lines)])

	# --- Verdicts.
	var p1_ok := true
	var p2_ok := true
	var p1_detail: PackedStringArray = []
	var p2_detail: PackedStringArray = []
	for ssn in VEHICLES:
		var ev := int(VEHICLES[ssn][2])
		var wp_list := int(VEHICLES[ssn][1])
		if not kickoff_fired.has(ev):
			p1_ok = false
			p1_detail.append("ssn %d: kickoff event %d never fired" % [ssn, ev])
			continue
		if not order_seen.has(ssn):
			p1_ok = false
			p1_detail.append("ssn %d: wp_channel never became list %d" % [ssn, wp_list])
		elif not state16_seen.has(ssn):
			p1_ok = false
			p1_detail.append("ssn %d: SM state 16 never observed" % ssn)
		var vd: Dictionary = _sim.get_world_entity_debug(ssn)
		var pos: Vector3 = vd.get("mission_position", Vector3.ZERO)
		var dep: float = _planar(pos, kickoff_pos.get(ssn, pos))
		if dep < MIN_DEPART_DISTANCE:
			p2_ok = false
			p2_detail.append("ssn %d (%s): moved %.1fu (< %.0fu)" % [
					ssn, VEHICLES[ssn][3], dep, MIN_DEPART_DISTANCE])
		if String(VEHICLES[ssn][3]) == "boat" and float(max_boat_dz.get(ssn, 0.0)) > BOAT_Z_TOLERANCE:
			p2_ok = false
			p2_detail.append("ssn %d: boat left the water plane (max dz %.2fu)" %
					[ssn, float(max_boat_dz.get(ssn, 0.0))])
	_gate("P1 orders", p1_ok, "routes+state16 on all 8" if p1_ok else "; ".join(p1_detail))
	_gate("P2 motion", p2_ok, "all departed afloat/rolling" if p2_ok else "; ".join(p2_detail))

	var p3_ok := true
	var p3_detail: PackedStringArray = []
	for ev in DEBARK_EVENTS:
		var grp := int(DEBARK_EVENTS[ev][0])
		var lst := int(DEBARK_EVENTS[ev][1])
		if not debark_fired.has(ev):
			p3_ok = false
			p3_detail.append("event %d never fired" % ev)
			continue
		var redirected := 0
		var members := 0
		var max_walk := 0.0
		var start_poss: Dictionary = debark_positions.get(ev, {})
		for i in AI_SCAN_CAP:
			var d: Dictionary = _sim.get_entity_debug(i)
			if d.is_empty():
				break
			if int(d.get("pool", -1)) != 0 or int(d.get("group_id", -1)) != grp:
				continue
			members += 1
			if int(d.get("wp_channel", -1)) == lst:
				redirected += 1
			if start_poss.has(i):
				var now_pos: Vector3 = d.get("position", Vector3.ZERO)
				max_walk = max(max_walk, now_pos.distance_to(start_poss[i]))
		if members == 0 or redirected == 0:
			p3_ok = false
			p3_detail.append("event %d: group %d redirect to list %d not taken (%d/%d)" % [
					ev, grp, lst, redirected, members])
		elif max_walk < 10.0:
			p3_ok = false
			p3_detail.append("event %d: group %d routed but FROZEN (max walk %.1fu)" % [
					ev, grp, max_walk])
	_gate("P3 arrival", p3_ok, "debark chain live" if p3_ok else "; ".join(p3_detail))

	if _fail_lines.is_empty():
		print("PROBE PASS: 00TRg defense delivery is host-live (P0-P3)")
		quit(0)
	else:
		print("PROBE FAIL (%d gates): %s" % [_fail_lines.size(), " || ".join(_fail_lines)])
		quit(1)
