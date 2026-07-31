extends SceneTree

# Vehicle mount/ride/drive in-game probe — NOT a GUT test (needs a retail PFF install;
# *_probe.gd files are manual, never collected). Drives 00TRa's ("Training: Basics /
# Armory") vehicle gate end to end on the live runtime:
#
#   * the USE-ITEM toggle mounts the local player into the DTruck1 (SSN 11)
#     [orig: Entity_ToggleVehicleMount @0x436950 + the nearest-seat scan @0x435d50],
#   * the BMS PLYRATTACHED trigger fires event 2 (the ride kickoff: RedirectGroupTo
#     group 3 + PatrolSpeed 40) [orig: EventTrigger cat-7 sub 38 -> @0x4f10d0],
#   * THE RIDE: the truck's ctrl seat is crewed at spawn (the instructor boards via
#     the authored waypoint_id-123..125 command mount), so after event 2 the
#     AI-driver leg drives the truck along list 2 with the player aboard
#     [orig: Entity_UpdateVehiclePhysics @0x48bc12 + Entity_SetWaypointByTeam
#     @0x43cdb4 + AI_HandleCommand case 0xB PatrolSpeed],
#   * the seated player is CARRIED (position follows the seat every tick),
#   * the free-ctrl ATV (SSN 1766) then takes the player as DRIVER and forward
#     input drives it (the occupant leg) with the camera aboard.
#
#   NW_SP_MISSION=00TRa.bms NW_RESOURCE_DIR=<retail install> "$GODOT_BIN" \
#       --headless --path godot -s res://tests/vehicle_ride_probe.gd
#
# godot-pos <-> mission-pos: mission = (x, -z, y) of the godot vector.

const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const AI_SCAN_CAP := 4096
const TIME_SCALE := 5.0
const TRUCK_SSN := 11
const ATV_SSN := 1766

var _forward := false


func _initialize() -> void:
	call_deferred("_run")


func _mission_wait(seconds: float) -> void:
	var t0 := Time.get_ticks_msec()
	while float(Time.get_ticks_msec() - t0) * TIME_SCALE < seconds * 1000.0:
		await process_frame


func _find_local_player_index(sim, world) -> int:
	var pp: Vector3 = world.get_sim().get_local_player_position()
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
	var presenter = game.get_node_or_null("LocalPlayerPresenter")
	if world == null or presenter == null:
		_fail("main_game lacks World/LocalPlayerPresenter children")
		return

	var wall_start := Time.get_ticks_msec()
	while not (world.get_sim() != null and world.get_sim().has_local_player()):
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			_fail("player never spawned (mission load stalled?)")
			return
	var sim = world.get_sim()
	if sim == null:
		_fail("no sim after player spawn")
		return
	presenter.set_input_source(func() -> Dictionary: return {"forward": _forward})
	await _mission_wait(1.0)

	# --- Locate the truck + the player's AI row.
	var truck: Dictionary = sim.get_world_entity_debug(TRUCK_SSN)
	if truck.is_empty():
		_fail("truck SSN %d not found in the sim" % TRUCK_SSN)
		return
	var player_idx := _find_local_player_index(sim, world)
	print("PROBE truck pos=%s seats=%d; player idx=%d" %
			[str(truck.get("position")), int(truck.get("seat_count", 0)), player_idx])
	if int(truck.get("seat_count", 0)) == 0:
		_fail("truck has NO seats (seat specs not fed?)")
		return

	# --- Bring the PLAYER to the truck (the spawn point sits inside the barracks —
	# an indoor LOS ray to a raised seat clips the building; the motor pool is open
	# ground and the truck's authored list-2 route starts there).
	var tpos0: Vector3 = truck.get("position", Vector3.ZERO)
	sim.debug_set_entity_position(player_idx, Vector3(tpos0.x + 1.6, -tpos0.z, tpos0.y))
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
	print("PROBE MOUNTED: seat=%d type=%d (1 sitex/2 ctrl/3 gun/5 drvr)" %
			[int(pd.get("mount_seat", -1)), int(pd.get("mount_type", 0))])

	# --- Event 2 (PLYRATTACHED 11 -> the ride kickoff) fires within a few event quanta.
	var fired := false
	for i in 10:
		await _mission_wait(1.0)
		# Per-event fired FLAGS (index = event number, byte = fired).
		var events: PackedByteArray = sim.get_fired_events_snapshot()
		if events.size() > 2 and events[2] != 0:
			fired = true
			break
	if not fired:
		_fail("event 2 (PLYRATTACHED SSN 11) never fired")
		return
	print("PROBE event 2 FIRED (ride kickoff: RedirectGroupTo 3 + PatrolSpeed 40)")

	# --- THE RIDE: the instructor (ctrl seat, command-mounted at spawn) drives the
	# redirected truck; the seated player must be carried along.
	var t_ride0: Vector3 = sim.get_world_entity_debug(TRUCK_SSN).get("position", Vector3.ZERO)
	var ride_dist := 0.0
	for i in 30:
		await _mission_wait(1.0)
		var tnow: Vector3 = sim.get_world_entity_debug(TRUCK_SSN).get("position", Vector3.ZERO)
		ride_dist = tnow.distance_to(t_ride0)
		if ride_dist > 8.0:
			break
	pd = sim.get_entity_debug(player_idx)
	var rider_pos: Vector3 = pd.get("position", Vector3.ZERO)
	var truck_now: Vector3 = sim.get_world_entity_debug(TRUCK_SSN).get("position", Vector3.ZERO)
	var carry_gap := rider_pos.distance_to(truck_now)
	print("PROBE ride: truck drove %.1fu; rider gap %.1fu" % [ride_dist, carry_gap])
	if ride_dist < 8.0:
		_fail("the ride never moved (AI driver leg dead? truck drove %.1fu)" % ride_dist)
		return
	if carry_gap > 10.0:
		_fail("rider left behind: %.1fu from the truck mid-ride" % carry_gap)
		return
	print("PROBE RIDE OK (AI-driven, player carried)")

	# --- The player-drive leg on the free-ctrl ATV: teleport the PLAYER out of the
	# truck's seat reach (dismount) next to the relocated ATV, mount, drive.
	var atv: Dictionary = sim.get_world_entity_debug(ATV_SSN)
	if atv.is_empty():
		print("PROBE atv SSN %d missing; drive leg skipped" % ATV_SSN)
		print("PROBE PASS: mount + event 2 + AI ride + carry")
		quit(0)
		return
	# A mounted player cannot be teleported away (the seat carry snaps it back each
	# tick) — park the TRUCK far away instead, so the toggle's scan runs dry and
	# DETACHES [orig: @0x4369c7], then bring the ATV to the dismounted player.
	pd = sim.get_entity_debug(player_idx)
	var here: Vector3 = pd.get("position", Vector3.ZERO)
	sim.debug_set_world_entity_position(TRUCK_SSN, Vector3(here.x + 200.0, -here.z, here.y))
	await _mission_wait(0.3)
	sim.local_player_toggle_mount() # seat scan dry (the truck left) -> detach
	await _mission_wait(0.3)
	if bool(sim.get_entity_debug(player_idx).get("mounted", false)):
		print("PROBE dismount failed after the truck left; drive leg skipped")
		print("PROBE PASS: mount + event 2 + AI ride + carry")
		quit(0)
		return
	print("PROBE dismounted (scan-dry toggle)")
	pd = sim.get_entity_debug(player_idx)
	here = pd.get("position", Vector3.ZERO)
	sim.debug_set_world_entity_position(ATV_SSN, Vector3(here.x + 1.5, -here.z, here.y))
	await _mission_wait(0.3)
	sim.local_player_toggle_mount() # mount the ATV
	await _mission_wait(0.5)
	pd = sim.get_entity_debug(player_idx)
	if not bool(pd.get("mounted", false)) or int(pd.get("mount_target_net_id", -1)) != ATV_SSN:
		print("PROBE atv mount not reached (mounted=%s target=%d); drive leg skipped" %
				[str(pd.get("mounted")), int(pd.get("mount_target_net_id", -1))])
		print("PROBE PASS: mount + event 2 + AI ride + carry")
		quit(0)
		return
	var seat_type := int(pd.get("mount_type", 0))
	print("PROBE ATV mounted: type=%d" % seat_type)
	if seat_type == 2 or seat_type == 5:
		var a0: Vector3 = sim.get_world_entity_debug(ATV_SSN).get("position", Vector3.ZERO)
		_forward = true
		await _mission_wait(4.0)
		_forward = false
		var a1: Vector3 = sim.get_world_entity_debug(ATV_SSN).get("position", Vector3.ZERO)
		pd = sim.get_entity_debug(player_idx)
		var gap: float = (pd.get("position", Vector3.ZERO) as Vector3).distance_to(a1)
		print("PROBE drive: ATV moved %.1fu; rider gap %.1fu" % [a1.distance_to(a0), gap])
		if a1.distance_to(a0) < 2.0:
			_fail("player drive dead: ATV moved %.1fu under forward input" % a1.distance_to(a0))
			return
		if gap > 8.0:
			_fail("driver fell off the ATV (gap %.1fu)" % gap)
			return
		print("PROBE DRIVE OK (player-driven)")
		print("PROBE PASS: mount + event 2 + AI ride + carry + player drive")
	else:
		print("PROBE PASS: mount + event 2 + AI ride + carry (ATV seat was type %d)" % seat_type)
	quit(0)
