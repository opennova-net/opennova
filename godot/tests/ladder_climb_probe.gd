extends SceneTree

# Ladder-climb in-game probe — NOT a GUT test (needs a retail PFF install;
# *_probe.gd files are manual, never collected). Drives the D-COL-5 climb motor
# on 00TRa's ladder-climbing tutorial section:
#
#   * sweeps the collision debug view around the spawn for a CL/type-4 volume
#     (the ladder is authored purely as a .3di BVOL — no items.def attribute),
#   * teleports the player to the ladder base and tries the 8 approach yaws
#     looking up — the witnessed entry gate needs the view within 60 deg of the
#     authored facing with the look-pitch sign agreeing with the anchor side
#     [orig: @ 0x4b32a5-0x4b32c0],
#   * HOLD FORWARD: the climb block stamps climb_up and the clip's vertical
#     root lane rises the body [orig: @ 0x4b7484-0x4b750a],
#   * release: climb_idle holds height (gravity skipped while latched
#     [orig: @ 0x4b7acd]),
#   * BACK: the dismount pushes off the face and the body falls/grounds
#     [orig: @ 0x4b7681-0x4b76cc].
#
#   NW_SP_MISSION=00TRa.bms NW_RESOURCE_DIR=<retail install> "$GODOT_BIN" \
#       --headless --path godot -s res://tests/ladder_climb_probe.gd
#
# godot-pos <-> mission-pos: mission = (x, -z, y) of the godot vector.

const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const TIME_SCALE := 5.0
const SWEEP_STEP := 100.0
const SWEEP_RADIUS := 400.0

var _forward := false
var _back := false


func _initialize() -> void:
	call_deferred("_run")


func _mission_wait(seconds: float) -> void:
	var t0 := Time.get_ticks_msec()
	while float(Time.get_ticks_msec() - t0) * TIME_SCALE < seconds * 1000.0:
		await process_frame


func _fail(msg: String) -> void:
	print("PROBE FAIL: %s" % msg)
	quit(1)


# All CL volumes among the collision instances currently in range (godot-space
# volume center + base height from the transformed corners).
func _cl_volumes(sim) -> Array:
	var found: Array = []
	var cd: Dictionary = sim.get_collision_debug()
	for inst in cd.get("instances", []):
		for vd in inst.get("volumes", []):
			if int(vd.get("type", -1)) != Simulation.BVOL_LADDER_CL:
				continue
			var corners: PackedVector3Array = vd.get("corners", PackedVector3Array())
			if corners.size() != 8:
				continue
			var center := Vector3.ZERO
			var base := INF
			var top := -INF
			for c in corners:
				center += c
				base = minf(base, c.y)
				top = maxf(top, c.y)
			center /= 8.0
			found.append({
				"center": center,
				"base": base,
				"top": top,
				"entity_handle": int(inst.get("entity_handle", -1)),
				"inst_pos": inst.get("pos", Vector3.INF),
			})
	return found


func _run() -> void:
	if OS.get_environment("NW_SP_MISSION").is_empty():
		push_error("ladder_climb_probe: set NW_SP_MISSION=00TRa.bms")
		quit(1)
		return
	var res_dir := OS.get_environment("NW_RESOURCE_DIR")
	if not res_dir.is_empty():
		var settings := load("res://game/resource_index/resource_dir_settings.gd")
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
	presenter.set_input_source(func() -> Dictionary:
		return {"forward": _forward, "back": _back})
	await _mission_wait(1.0)

	# --- Sweep for a CL volume: the debug view is player-anchored (150u), so
	# hop a teleport grid around the spawn until one shows up.
	var spawn: Vector3 = sim.get_local_player_position() # godot space
	var ladders: Array = []
	var gx := spawn.x
	var gz := spawn.z
	var step := SWEEP_STEP
	var probe_points: Array = [Vector2(gx, gz)]
	var r := step
	while r <= SWEEP_RADIUS:
		var n := int(ceil(TAU * r / step))
		for i in n:
			var a := TAU * float(i) / float(n)
			probe_points.append(Vector2(gx + r * cos(a), gz + r * sin(a)))
		r += step
	for p in probe_points:
		sim.debug_teleport_local_player(Vector3(p.x, -p.y, 60.0), 0.0, 0.0)
		await _mission_wait(0.12)
		ladders = _cl_volumes(sim)
		if not ladders.is_empty():
			break
	if ladders.is_empty():
		_fail("no CL/type-4 volume within %.0fu of the spawn" % SWEEP_RADIUS)
		return
	var ladder: Dictionary = ladders[0]
	var center: Vector3 = ladder["center"]
	var base: float = ladder["base"]
	var top: float = ladder["top"]
	print("PROBE ladder CL at godot %s (base %.1f top %.1f), %d found; owner handle=%d inst_pos=%s" %
			[str(center), base, top, ladders.size(), int(ladder["entity_handle"]),
			str(ladder["inst_pos"])])

	# --- Mount: drop in just ABOVE the anchor (anchorZ = top − 1.0), LOOKING
	# DOWN — the from-above entry arm has no facing requirement (heightDiff < 0
	# short-circuits the 60° gate; the pitch sign must agree). A slanted CL
	# prism only crosses the corner-average column near its top, so the top
	# band is also where contact is guaranteed. The latch detector is the
	# witnessed gravity skip: an idle latched body holds Z exactly — nothing
	# else holds a body mid-air.
	var mx := center.x
	var my := -center.z
	var latched := false
	var hold_z := 0.0
	for k in 8:
		var yaw_deg := 45.0 * float(k)
		sim.debug_teleport_local_player(Vector3(mx, my, top - 0.3), yaw_deg, -30.0)
		await _mission_wait(0.8)
		var z0: float = sim.get_local_player_position().y
		await _mission_wait(0.5)
		var z1: float = sim.get_local_player_position().y
		if absf(z1 - z0) < 0.05 and z1 > base:
			latched = true
			hold_z = z1
			print("PROBE LATCHED: yaw %.0f holds z %.2f (volume %.1f..%.1f)" %
					[yaw_deg, z1, base, top])
			break
		print("PROBE yaw %.0f no latch (z %.2f -> %.2f)" % [yaw_deg, z0, z1])
	if not latched:
		_fail("no entry latched — the CL entry gate or the gravity skip is dead")
		return

	# --- Climb: mouse-look UP (the forward fan picks climb_up by the look-pitch
	# sign), hold forward, and the climb clip's vertical lane must rise the body.
	for i in 8:
		sim.add_local_player_look(0.0, -600.0)
		await process_frame
	_forward = true
	var max_z := hold_z
	for i in 30:
		await _mission_wait(0.1)
		max_z = maxf(max_z, sim.get_local_player_position().y)
	_forward = false
	if max_z <= hold_z + 0.4:
		_fail("forward + look-up did not climb (held %.2f, max %.2f)" % [hold_z, max_z])
		return
	print("PROBE CLIMB OK: %.2f -> %.2f (climb_up root motion)" % [hold_z, max_z])

	# --- Hold again: release the stick — climb_idle keeps the height (gravity
	# stays off while latched). A natural top-out above the volume falls instead,
	# so only assert the hold when still inside the span.
	await _mission_wait(0.5)
	var settle: float = sim.get_local_player_position().y
	if settle < top - 0.5:
		await _mission_wait(0.6)
		var settle2: float = sim.get_local_player_position().y
		if absf(settle2 - settle) > 0.3:
			_fail("height not held after the climb (%.2f -> %.2f)" % [settle, settle2])
			return
		print("PROBE HOLD OK at z %.2f (climb_idle, no gravity)" % settle2)
	else:
		print("PROBE topped out above the volume (z %.2f) — exit leg took over" % settle)
	print("PROBE PASS: CL entry + gravity-off hold + climb_up on 00TRa")
	quit(0)
