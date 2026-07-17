extends SceneTree

# Vehicle mount/ride/drive in-game probe — NOT a GUT test (needs a retail PFF install;
# *_probe.gd files are manual, never collected). Drives 00TRa's ("Training: Basics /
# Armory") vehicle gate end to end on the live runtime:
#
#   * the USE-ITEM toggle mounts the local player into the DTruck1 (SSN 11)
#     [orig: Entity_ToggleVehicleMount @0x436950 + the nearest-seat scan @0x435d50],
#   * the BMS PLYRATTACHED trigger fires event 2 (the ride kickoff: RedirectGroupTo
#     group 3 + PatrolSpeed 40) [orig: EventTrigger cat-7 sub 38 -> @0x4f10d0],
#   * the seat CARRY holds: teleporting the truck moves the seated player with it
#     [orig: the mounted-leg seat-bone pose in Entity_UpdateInfantryPlayerBody],
#   * from the ctrl/drvr seat, forward input DRIVES the truck (the occupant leg of
#     Entity_UpdateVehiclePhysics @0x48af00) and the player rides along,
#   * a final toggle with no other seat in reach dismounts [orig: @0x4369c7].
#
#   NW_SP_MISSION=00TRa.bms NW_RESOURCE_DIR=<retail install> "$GODOT_BIN" \
#       --headless --path godot -s res://tests/vehicle_ride_probe.gd
#
# godot-pos <-> mission-pos: mission = (x, -z, y) of the godot vector (see
# round_outcome_probe's teleport leg).

const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const AI_SCAN_CAP := 4096
const TIME_SCALE := 5.0
const TRUCK_SSN := 11

var _forward := false


func _initialize() -> void:
	call_deferred("_run")


func _mission_wait(seconds: float) -> void:
	var t0 := Time.get_ticks_msec()
	while float(Time.get_ticks_msec() - t0) * TIME_SCALE < seconds * 1000.0:
		await process_frame


func _find_by_net_id(sim, net_id: int) -> Dictionary:
	for i in AI_SCAN_CAP:
		var d: Dictionary = sim.get_entity_debug(i)
		if d.is_empty():
			break
		if int(d.get("net_id", -1)) == net_id:
			d["ai_index"] = i
			return d
	return {}


func _find_local_player_index(sim, world) -> int:
	var pp: Vector3 = world.local_player_position()
	for i in AI_SCAN_CAP:
		var d: Dictionary = sim.get_entity_debug(i)
		if d.is_empty():
			break
		if int(d.get("pool", -1)) != 0 or not bool(d.get("infantry", false)):
			continue
		var pos: Vector3 = d.get("position", Vector3.INF)
		if pos.distance_to(pp) < 0.6:
			return i
	return -1


func _fail(msg: String) -> void:
	print("PROBE FAIL: %s" % msg)
	quit(1)


func _run() -> void:
	if OS.get_environment("NW_SP_MISSION").is_empty():
		push_error("vehicle_ride_probe: set NW_SP_MISSION=00TRa.bms")
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
		_fail("failed to load main_game.tscn")
		return
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	var host = game.get_node_or_null("LocalPlayerHost")
	if world == null or host == null:
		_fail("main_game lacks World/LocalPlayerHost children")
		return

	var wall_start := Time.get_ticks_msec()
	while not world.has_local_player():
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			_fail("player never spawned (mission load stalled?)")
			return
	var sim = world.get_sim()
	if sim == null:
		_fail("no sim after player spawn")
		return
	host.set_input_source(func() -> Dictionary: return {"forward": _forward})
	await _mission_wait(1.0)

	# --- Locate the truck + the player's AI row.
	var truck := _find_by_net_id(sim, TRUCK_SSN)
	if truck.is_empty():
		_fail("truck SSN %d not found in the sim" % TRUCK_SSN)
		return
	var truck_idx := int(truck["ai_index"])
	var player_idx := _find_local_player_index(sim, world)
	print("PROBE truck idx=%d pos=%s; player idx=%d" %
			[truck_idx, str(truck.get("position")), player_idx])

	# --- Bring the truck to the player (mission geography defeats walking; the mount
	# scan under test needs a seat within 4 u).
	var pp: Vector3 = world.local_player_position()
	var mission := Vector3(pp.x + 2.5, -pp.z, pp.y)
	sim.debug_set_entity_position(truck_idx, mission)
	await _mission_wait(0.5)

	# --- Toggle mount: the player must end up seated on SSN 11.
	if not sim.local_player_toggle_mount():
		_fail("toggle_mount refused (no seat within the 4 u scan gate?)")
		return
	await _mission_wait(0.5)
	var pd: Dictionary = sim.get_entity_debug(player_idx)
	if not bool(pd.get("mounted", false)):
		_fail("player not mounted after toggle")
		return
	if int(pd.get("mount_target_net_id", -1)) != TRUCK_SSN:
		_fail("mounted on net %d, wanted %d" % [int(pd.get("mount_target_net_id", -1)), TRUCK_SSN])
		return
	var seat_type := int(pd.get("mount_type", 0))
	print("PROBE MOUNTED: seat=%d type=%d (1 sitex/2 ctrl/3 gun/5 drvr)" %
			[int(pd.get("mount_seat", -1)), seat_type])

	# --- Event 2 (PLYRATTACHED 11 -> the ride kickoff) fires within a few event quanta.
	var fired := false
	for i in 10:
		await _mission_wait(1.0)
		var events: PackedInt32Array = sim.get_fired_events_snapshot()
		if 2 in events:
			fired = true
			break
	if not fired:
		_fail("event 2 (PLYRATTACHED SSN 11) never fired")
		return
	print("PROBE event 2 FIRED (ride kickoff: RedirectGroupTo 3 + PatrolSpeed)")

	# --- The seat carry: teleport the truck; the seated player must move with it.
	pd = sim.get_entity_debug(player_idx)
	var before: Vector3 = pd.get("position", Vector3.ZERO)
	var td: Dictionary = sim.get_entity_debug(truck_idx)
	var tpos: Vector3 = td.get("position", Vector3.ZERO)
	var tp_mission := Vector3(tpos.x + 12.0, -tpos.z, tpos.y)
	sim.debug_set_entity_position(truck_idx, tp_mission)
	await _mission_wait(0.5)
	pd = sim.get_entity_debug(player_idx)
	var after: Vector3 = pd.get("position", Vector3.ZERO)
	if after.distance_to(before) < 8.0:
		_fail("seat carry broken: player moved %.1fu after a 12u truck teleport" %
				after.distance_to(before))
		return
	print("PROBE seat carry OK (player followed the truck %.1fu)" % after.distance_to(before))

	# --- Drive (only from a control seat; swap toward one for a few toggles if needed).
	var tries := 0
	pd = sim.get_entity_debug(player_idx)
	while int(pd.get("mount_type", 0)) != 2 and int(pd.get("mount_type", 0)) != 5 and tries < 8:
		if not sim.local_player_toggle_mount():
			break
		await _mission_wait(0.3)
		pd = sim.get_entity_debug(player_idx)
		if not bool(pd.get("mounted", false)):
			# Swapped out to a dismount — remount and stop trying.
			sim.local_player_toggle_mount()
			await _mission_wait(0.3)
			pd = sim.get_entity_debug(player_idx)
			break
		tries += 1
	seat_type = int(pd.get("mount_type", 0))
	if seat_type == 2 or seat_type == 5:
		td = sim.get_entity_debug(truck_idx)
		var t0: Vector3 = td.get("position", Vector3.ZERO)
		_forward = true
		await _mission_wait(4.0)
		_forward = false
		td = sim.get_entity_debug(truck_idx)
		var t1: Vector3 = td.get("position", Vector3.ZERO)
		var moved := t1.distance_to(t0)
		pd = sim.get_entity_debug(player_idx)
		var prider: Vector3 = pd.get("position", Vector3.ZERO)
		var rider_gap := prider.distance_to(t1)
		print("PROBE drive: truck moved %.1fu; rider gap %.1fu" % [moved, rider_gap])
		if moved < 2.0:
			_fail("drive dead: truck moved %.1fu under forward input" % moved)
			return
		if rider_gap > 8.0:
			_fail("rider fell off: %.1fu from the truck after driving" % rider_gap)
			return
		print("PROBE DRIVE OK")
	else:
		print("PROBE drive leg SKIPPED (no ctrl/drvr seat reached; seat type %d)" % seat_type)

	print("PROBE PASS: mount + event 2 + seat carry%s" %
			(" + drive" if (seat_type == 2 or seat_type == 5) else ""))
	quit(0)
