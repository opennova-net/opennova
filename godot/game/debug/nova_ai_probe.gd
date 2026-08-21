class_name NovaAiProbe
extends RefCounted
## Env-gated sim-truth probe for the autonomous self-test loop (net-capture
## branch): with NW_AI_PROBE=<path> set, appends one JSON line every interval
## carrying a compact card for EVERY AI entity — position, anim, mount state —
## so a capture diff can separate "the sim never moved it" from "the 0x0A
## interest scheduler never resent it". Inert without the env var.

const INTERVAL_S := 5.0

var _path := OS.get_environment("NW_AI_PROBE")
var _accum := 0.0

# Screenshot hook (NW_SHOT_DIR + NW_SHOT_SSN): each interval a chase camera
# tracks the target entity and the main viewport is dumped to PNG, so the
# self-test loop hands out a human-checkable frame. Inert without the envs.
var _shot_dir := OS.get_environment("NW_SHOT_DIR")
var _shot_ssn := OS.get_environment("NW_SHOT_SSN").to_int()
var _shot_cam: Camera3D = null
var _shot_n := 0

# One-shot teleport probe (NW_TP_BMS + NW_TP_DX/NW_TP_DY [+ NW_TP_AT s]):
# displaces that entity by a mission-space offset mid-round — the frozen-clump
# environmental-pin experiment (does a pinned soldier walk once moved clear?).
var _tp_bms := OS.get_environment("NW_TP_BMS").to_int()
var _tp_dx := OS.get_environment("NW_TP_DX").to_float()
var _tp_dy := OS.get_environment("NW_TP_DY").to_float()
var _tp_at := maxf(OS.get_environment("NW_TP_AT").to_float(), 1.0) \
		if not OS.get_environment("NW_TP_AT").is_empty() else 60.0
var _tp_elapsed := 0.0
var _tp_done := false

func tick(world: GameWorld, delta: float) -> void:
	if world == null or (_path.is_empty() and _shot_dir.is_empty()):
		return
	_tp_elapsed += delta
	_accum += delta
	if _accum < INTERVAL_S:
		return
	_accum = 0.0
	_tp_tick(world)
	_tp_player_tick(world)
	_shot_tick(world)
	if _path.is_empty():
		return
	var sim: Simulation = world.get_sim()
	if sim == null:
		return
	var cards: Array = []
	for i in range(int(sim.get_entity_count())):
		var card: Dictionary = sim.get_entity_debug(i)
		if card.is_empty():
			continue
		var pos: Vector3 = card.get("position", Vector3.ZERO)
		cards.append({
			"i": i,
			"bms": int(card.get("bms_id", 0)),
			"item": int(card.get("item_id", 0)),
			"team": int(card.get("team", -1)),
			"wp": int(card.get("waypoint_id", 0)),
			"num": int(card.get("wp_number", 0)),
			"hp": int(card.get("health", 0)),
			"mounted": bool(card.get("mounted", false)),
			"pos": [pos.x, pos.y, pos.z],
			"yaw": float(card.get("yaw_deg", 0.0)),
			"anim": int(card.get("anim_state", -1)),
			# think state: why a soldier is (not) walking
			"mm": int(card.get("infantry_move_mode", -1)),
			"ch": int(card.get("wp_channel", -1)),
			"node": int(card.get("wp_node", -1)),
			"wpd": int(card.get("wp_distance", -1)),
			"spd": int(card.get("out_speed", -1)),
			"st": int(card.get("state", -1)),
			"adm": String(card.get("adm_name", "")),
			"inf": bool(card.get("infantry", false)),
			"flags": int(card.get("engine_flags", 0)),
			"bflags": int(card.get("bms_flags", 0)),
			# frozen-clump instrument: root step vs resolver correction
			"rdx": int(card.get("root_dx", 0)),
			"rdy": int(card.get("root_dy", 0)),
			"cdx": int(card.get("res_dx", 0)),
			"cdy": int(card.get("res_dy", 0)),
			"citem": int(card.get("contact_item", 0)),
			"fa": int(card.get("fires_aimed", 0)),
			"fb": int(card.get("fires_body", 0)),
			# fall-through instrument
			"gc": int(card.get("ground_cache", 0)),
			"gv": bool(card.get("ground_valid", false)),
			"air": bool(card.get("airborne", false)),
			"alert": int(card.get("alert", -1)),
			"tgt": bool(card.get("combat_target_valid", false)),
				# rotor spin (helicopters): speed ramp + blade phase
				"rs": int(card.get("rotor_speed", 0)),
				"rp": int(card.get("rotor_phase", 0)),
				"vf": int(card.get("veh_family", -1)),
		})
	if cards.is_empty():
		return
	var f: FileAccess
	if FileAccess.file_exists(_path):
		f = FileAccess.open(_path, FileAccess.READ_WRITE)
		if f != null:
			f.seek_end()
	else:
		f = FileAccess.open(_path, FileAccess.WRITE)
	if f == null:
		return
	# Which mission events have fired. Without this a live round can only be
	# compared to the headless report by inference - and the two DISAGREED (the
	# offline zone tour reaches the scripted KillGroups; a live round walking the
	# same route did not), which is exactly the kind of gap that stays invisible
	# until the live side is instrumented too.
	var fired: PackedByteArray = sim.get_fired_events_snapshot()
	var nfired := 0
	var fired_idx: Array = []
	for i in range(fired.size()):
		if fired[i] != 0:
			nfired += 1
			if fired_idx.size() < 80:
				fired_idx.append(i)
	# The WAC `humans` count. It now gates the whole mission script
	# (World::script_may_advance), so when the script advances earlier than
	# expected this is the first number to look at.
	var humans := -1
	var outcome: Dictionary = sim.get_round_outcome_debug()
	if outcome.has("humans"):
		humans = int(outcome["humans"])
	f.store_line(JSON.stringify({"ms": Time.get_ticks_msec(), "ai": cards,
			"ev": {"n": nfired, "of": fired.size(), "idx": fired_idx},
			"humans": humans}))
	f.close()


func _shot_tick(world: GameWorld) -> void:
	if _shot_dir.is_empty() or _shot_ssn == 0:
		return
	var sim: Simulation = world.get_sim()
	if sim == null:
		return
	var stride: int = sim.get_present_stride()
	if stride <= 0:
		return
	var snap: PackedFloat32Array = sim.get_present_snapshot()
	var target := Vector3.ZERO
	var found := false
	var count: int = snap.size() / stride
	for i in range(count):
		var base := i * stride
		if int(snap[base + Simulation.PF_BMS_ID]) == _shot_ssn:
			target = Vector3(
					snap[base + Simulation.PF_POS_X],
					snap[base + Simulation.PF_POS_Y],
					snap[base + Simulation.PF_POS_Z])
			found = true
			break
	if not found:
		return
	# Capture the frame the previous placement rendered before re-aiming, so
	# every saved PNG shows the tracked entity, not a mid-swing camera.
	if _shot_cam != null and is_instance_valid(_shot_cam) and _shot_n > 0:
		var img: Image = world.get_viewport().get_texture().get_image()
		if img != null:
			img.save_png(_shot_dir.path_join("shot.%03d.png" % _shot_n))
	if _shot_cam == null or not is_instance_valid(_shot_cam):
		_shot_cam = Camera3D.new()
		_shot_cam.far = 8000.0
		world.add_child(_shot_cam)
	_shot_cam.make_current()
	# Camera offset is tunable (NW_SHOT_DX/DY/DZ) so a subject inside a
	# building can be framed from within the room instead of through a wall.
	var ofs := Vector3(18.0, 9.0, 18.0)
	if not OS.get_environment("NW_SHOT_DX").is_empty():
		ofs = Vector3(OS.get_environment("NW_SHOT_DX").to_float(),
				OS.get_environment("NW_SHOT_DY").to_float(),
				OS.get_environment("NW_SHOT_DZ").to_float())
	_shot_cam.global_position = target + ofs
	_shot_cam.look_at(target)
	_shot_n += 1


func _tp_tick(world: GameWorld) -> void:
	if _tp_done or _tp_bms == 0 or _tp_elapsed < _tp_at:
		return
	var sim: Simulation = world.get_sim()
	if sim == null:
		return
	for i in range(int(sim.get_entity_count())):
		var card: Dictionary = sim.get_entity_debug(i)
		if card.is_empty() or int(card.get("bms_id", 0)) != _tp_bms:
			continue
		# The card position is presenter-space (x, up, z); mission space is
		# (x, -z, up). debug_set_entity_position takes mission coordinates.
		var p: Vector3 = card.get("position", Vector3.ZERO)
		# NW_TP_Z sets an ABSOLUTE mission height; without it the subject keeps
		# its own. Keeping the source height is a trap when moving between areas
		# at different elevations — the subject lands in mid-air and terrain then
		# blocks every sightline, which reads as broken perception.
		var tz: float = p.y
		if not OS.get_environment("NW_TP_Z").is_empty():
			tz = OS.get_environment("NW_TP_Z").to_float()
		var mission := Vector3(p.x + _tp_dx, -p.z + _tp_dy, tz)
		var err := sim.debug_set_entity_position(i, mission)
		_tp_done = true
		if not _path.is_empty():
			var f := FileAccess.open(_path, FileAccess.READ_WRITE)
			if f != null:
				f.seek_end()
				f.store_line(JSON.stringify({"tp": {"bms": _tp_bms, "i": i,
						"to": [mission.x, mission.y, mission.z], "err": err}}))
				f.close()
		return


# One-shot LOCAL-PLAYER teleport (NW_TP_PLAYER_X/Y[/Z], mission coords, at
# NW_TP_AT seconds). 05TRcoop starts its two AI sides ~857u apart, so an
# unattended round never produces contact; dropping the firing client into the
# enemy base is how the combat path gets exercised without a human.
var _tpp_hop := 0

func _tp_player_tick(world: GameWorld) -> void:
	# Up to two hops (NW_TP_PLAYER_X/Y/Z at NW_TP_AT, then NW_TP_PLAYER_X2/Y2/Z2
	# at NW_TP_AT2). A linear mission opens each phase with the previous one, so
	# ONE placement only ever unlocks the first gate — the offline zone tour in
	# mission_script_report_test finds the route, and this walks a live player
	# along it so the later phases (the scripted KillGroups) actually run.
	if _tpp_hop >= 2:
		return
	var sfx := "" if _tpp_hop == 0 else "2"
	if OS.get_environment("NW_TP_PLAYER_X" + sfx).is_empty():
		_tpp_hop = 2
		return
	var at := _tp_at
	if _tpp_hop == 1:
		var at2 := OS.get_environment("NW_TP_AT2")
		at = at2.to_float() if not at2.is_empty() else (_tp_at * 3.0)
	if _tp_elapsed < at:
		return
	var sim: Simulation = world.get_sim()
	if sim == null:
		return
	var zs := OS.get_environment("NW_TP_PLAYER_Z" + sfx)
	var mission := Vector3(
			OS.get_environment("NW_TP_PLAYER_X" + sfx).to_float(),
			OS.get_environment("NW_TP_PLAYER_Y" + sfx).to_float(),
			zs.to_float() if not zs.is_empty() else 40.0)
	var err := sim.debug_teleport_local_player(mission, 0.0, 0.0)
	_tpp_hop += 1
	if not _path.is_empty():
		var f := FileAccess.open(_path, FileAccess.READ_WRITE)
		if f != null:
			f.seek_end()
			f.store_line(JSON.stringify({"tp_player": {"hop": _tpp_hop,
					"to": [mission.x, mission.y, mission.z], "err": err}}))
			f.close()
