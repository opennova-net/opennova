extends SceneTree

# Deterministic retail-asset regression for D-VEH-1: the parked transport
# trucks on 00TRa must rest their wheel-contact ORIGIN on the terrain under
# the real SP listen-server boot (main_game + GameFramePipeline — the same
# authority vehicle pass a player runs). Before the witnessed probe-box
# provenance landed (the CMDL header bbox Z pair —
# vehicle-client-movers-re.md §3), the per-COBJ AABB-union stand-in floated
# every DTruck by its below-origin wheel depth (~0.33 u): the user-visible
# floating wheels right of the 00TRa spawn.
#
# Run:
#   NW_SP_MISSION=00TRa.bms NW_RESOURCE_DIR=<retail JO dir or loose JOX> \
#     [NW_EXPANSION=] "$GODOT_BIN" --headless --path godot \
#     -s res://tests/00tra_truck_rest_probe.gd
#
# godot-pos <-> mission-pos: mission = (x, -z, y) of the godot vector; mission
# z is up.

const LOAD_TIMEOUT_WALL_SECONDS := 300.0
const TIME_SCALE := 10.0
const SETTLE_TICK := 620
# The CMDL floor sits at +0.01 for DTruck1/2, so the settled origin lands at
# ground - 0.01; allow modest slack for slope under the wheelbase (the solve
# averages four pad corners).
const MAX_REST_ERROR := 0.15
# The union stand-in floated the hull ~0.334 u — anything near that is the
# regression this probe exists to catch.
const OLD_FLOAT := 0.334
# 00TRa's three placed DTrucks (bms ids): 11 + 6 = "Drivable Transport Truck"
# (DTruck1; 11 is the truck right of the player spawn), 1714 = the armory
# truck (DTruck2). Spawn-area trucks first — the drop test picks the first.
const TRUCK_SSNS := [11, 1714, 6]

var _sim = null
var _fail_lines: PackedStringArray = []


func _init() -> void:
	call_deferred("_run")


func _fail_boot(message: String) -> void:
	push_error("[truck-rest] " + message)
	quit(1)


func _run() -> void:
	if OS.get_environment("NW_SP_MISSION").is_empty():
		_fail_boot("set NW_SP_MISSION=00TRa.bms")
		return
	var res_dir := OS.get_environment("NW_RESOURCE_DIR")
	if not res_dir.is_empty():
		var settings := load("res://game/resource_index/resource_dir_settings.gd")
		settings.set_resource_dir(res_dir)
		settings.set_expansion(OS.get_environment("NW_EXPANSION"))
	Engine.time_scale = TIME_SCALE

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

	while int(_sim.get_logic_tick()) < SETTLE_TICK:
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			_fail_boot("settle wait stalled at tick %d" % int(_sim.get_logic_tick()))
			return

	var terrain = world.get_terrain_data()
	if terrain == null or not terrain.is_loaded():
		_fail_boot("world terrain unavailable for height sampling")
		return

	var failures := 0
	for ssn in TRUCK_SSNS:
		var veh: Dictionary = _sim.get_world_entity_debug(int(ssn))
		if veh.is_empty():
			print("[truck-rest] ssn=%d MISSING from sim" % int(ssn))
			failures += 1
			continue
		var mission_pos: Vector3 = veh.get("mission_position", Vector3.ZERO)
		var ground := float(terrain.get_height_world_bilinear(
				Vector3(mission_pos.x, 0.0, -mission_pos.y)))
		if is_nan(ground):
			print("[truck-rest] ssn=%d off-terrain (skipped)" % int(ssn))
			continue
		var rest_error := mission_pos.z - ground
		var verdict := "OK" if absf(rest_error) <= MAX_REST_ERROR else "FLOATING"
		print("[truck-rest] ssn=%d pool=%d pos=%s ground=%.3f rest_error=%+.3f alive=%s hidden=%s %s" % [
			int(ssn), int(veh.get("pool", -1)), str(mission_pos), ground, rest_error,
			str(veh.get("alive", false)), str(veh.get("hidden", false)), verdict])
		if absf(rest_error) > MAX_REST_ERROR:
			failures += 1
			if absf(rest_error - OLD_FLOAT) < 0.1:
				print("[truck-rest] ssn=%d floats by the pre-D-VEH-1 wheel depth" % int(ssn))

	# Discriminating leg: the parked pose can coincide with the authored BMS Z,
	# so lift one LIVE truck 2 u and require the authority ground solve to
	# settle it BACK to the wheel-contact rest — proving the solve runs AND
	# rests the ORIGIN on the terrain (the union stand-in settled to
	# ground + ~0.33). A hidden/not-yet-live row never ticks its motor.
	# Pool-1 only: the authority vehicle pass walks pool-1 rows; a static-kind
	# placement (pool 2) never runs a motor — in retail either.
	var drop_ssn := -1
	for ssn in TRUCK_SSNS:
		var veh: Dictionary = _sim.get_world_entity_debug(int(ssn))
		if not veh.is_empty() and bool(veh.get("alive", false)) and \
				not bool(veh.get("hidden", false)) and int(veh.get("pool", -1)) == 1:
			drop_ssn = int(ssn)
			break
	var first: Dictionary = {} if drop_ssn < 0 \
			else _sim.get_world_entity_debug(drop_ssn)
	if not first.is_empty():
		var pos: Vector3 = first.get("mission_position", Vector3.ZERO)
		var lift_tick := int(_sim.get_logic_tick())
		# The lift carries a planar nudge on purpose: the solve's sleep
		# fast-path compares the pose PLANAR-only (Z is excluded by witness —
		# vehicle-client-movers-re.md §7 sleep gates), so a straight-up
		# teleport would leave a sleeping hull floating in retail too.
		_sim.debug_set_world_entity_position(
				int(first.get("net_id", 0)),
				Vector3(pos.x + 0.5, pos.y, pos.z + 2.0))
		while int(_sim.get_logic_tick()) < lift_tick + 310:
			await process_frame
		var after: Dictionary = _sim.get_world_entity_debug(drop_ssn)
		var settled: Vector3 = after.get("mission_position", Vector3.ZERO)
		var ground := float(terrain.get_height_world_bilinear(
				Vector3(settled.x, 0.0, -settled.y)))
		var rest_error := settled.z - ground
		print("[truck-rest] drop-test ssn=%d settled=%.3f ground=%.3f rest_error=%+.3f" % [
			drop_ssn, settled.z, ground, rest_error])
		if absf(settled.z - (pos.z + 2.0)) < 0.05:
			failures += 1
			print("[truck-rest] drop-test never settled — the ground solve did not run")
		elif absf(rest_error) > MAX_REST_ERROR:
			failures += 1
			if absf(rest_error - OLD_FLOAT) < 0.1:
				print("[truck-rest] drop-test floats by the pre-D-VEH-1 wheel depth")

	if failures > 0:
		push_error("[truck-rest] %d DTruck check(s) failed" % failures)
		quit(1)
		return
	print("[truck-rest] PASS — every placed DTruck rests its origin on the terrain")
	quit(0)
