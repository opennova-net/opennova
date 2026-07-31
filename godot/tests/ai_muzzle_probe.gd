extends SceneTree

# D-AI-6 muzzle-seam in-game probe — NOT a GUT test (needs a retail PFF install;
# *_probe.gd files are manual, never collected). Verifies the posed gun-flash
# userpoint flows present-layer -> sim and lands where a rifle muzzle belongs:
#
#   * NPC infantry with a flash userpoint (EIndo bodies: MFlash01/bullet) get a
#     FRESH binding-fed muzzle (`muzzle_valid`, get_entity_debug),
#   * the muzzle sits at CHEST height forward of the body — NOT the old
#     head-height chest-lift stand-in (pos + 0.9 u, zero lateral offset),
#   * the present pass reports muzzle pushes flowing every frame.
#
#   NW_SP_MISSION=CP01.bms NW_RESOURCE_DIR=<retail install> "$GODOT_BIN" \
#       --headless --path godot -s res://tests/ai_muzzle_probe.gd

const LOAD_TIMEOUT_WALL_SECONDS := 240.0
const AI_SCAN_CAP := 2048
const TIME_SCALE := 5.0

func _initialize() -> void:
	call_deferred("_run")


func _mission_wait(seconds: float) -> void:
	var t0 := Time.get_ticks_msec()
	while float(Time.get_ticks_msec() - t0) * TIME_SCALE < seconds * 1000.0:
		await process_frame


func _run() -> void:
	if OS.get_environment("NW_SP_MISSION").is_empty():
		push_error("ai_muzzle_probe: set NW_SP_MISSION=<mission.bms>")
		quit(1)
		return
	var res_dir := OS.get_environment("NW_RESOURCE_DIR")
	if not res_dir.is_empty():
		var settings := load("res://engine/resource_index/resource_dir_settings.gd")
		settings.set_resource_dir(res_dir)
		settings.set_expansion("")
	Engine.time_scale = TIME_SCALE
	var packed := load("res://game/main_game.tscn") as PackedScene
	var game := packed.instantiate()
	root.add_child(game)
	var world = game.get_node_or_null("World")
	if world == null:
		push_error("ai_muzzle_probe: no World")
		quit(1)
		return

	var wall_start := Time.get_ticks_msec()
	while not (world.get_sim() != null and world.get_sim().has_local_player()):
		await process_frame
		if float(Time.get_ticks_msec() - wall_start) / 1000.0 > LOAD_TIMEOUT_WALL_SECONDS:
			push_error("ai_muzzle_probe: mission load stalled")
			quit(1)
			return
	var sim = world.get_sim()
	print("PROBE mission=%s loaded" % OS.get_environment("NW_SP_MISSION"))

	# The present rows render the CLIENT WIRE VIEW, which fills by replication
	# range — the SP player spawns farthest-from-enemy, so no NPC is in view at
	# spawn. Bring a few infantry NPCs to the player (the probe teleport seam) so
	# their rows decode and the muzzle seam flows, exactly as in-range combat does.
	var pp: Vector3 = world.get_sim().get_local_player_position()
	var moved := 0
	for i in AI_SCAN_CAP:
		var d: Dictionary = sim.get_entity_debug(i)
		if d.is_empty():
			break
		if not bool(d.get("alive", false)) or not bool(d.get("infantry", false)):
			continue
		if bool(d.get("mounted", false)):
			continue
		var spot := pp + Vector3(6.0 + 2.0 * moved, 0.0, 3.0 * (moved - 1))
		sim.debug_set_entity_position(i, Vector3(spot.x, -spot.z, spot.y))
		moved += 1
		if moved >= 3:
			break
	print("PROBE teleported %d NPCs into replication range" % moved)

	# Let the wire view decode them and a few present -> sim pushes flow.
	await _mission_wait(6.0)

	# Diagnostic: what did the models resolve?
	var dumped := 0
	var stack: Array[Node] = [root]
	while not stack.is_empty() and dumped < 8:
		var n: Node = stack.pop_back()
		for c in n.get_children():
			stack.push_back(c)
		if n.get("object_data") == null or not n.has_method("has_muzzle"):
			continue
		var od = n.get("object_data")
		if od == null or not od.has_method("get_user_point_count"):
			continue
		var ups := []
		for ui in range(int(od.get_user_point_count())):
			var inf: Dictionary = od.get_user_point_info(ui)
			ups.append("%s@part%d" % [String(inf.get("name", "?")), int(inf.get("subobject", -1))])
		if ups.is_empty():
			continue
		var skel_bones := -1
		if n.has_method("get_skeleton") and n.get_skeleton() != null:
			skel_bones = n.get_skeleton().get_bone_count()
		print("PROBE model=%s ups=%s bones=%d muzzle_bone=%d has_muzzle=%s" %
				[n.name, str(ups), skel_bones, int(n.get("_muzzle_bone")), str(n.has_muzzle())])
		dumped += 1

	var checked := 0
	var stamped := 0
	var good := 0
	var head_height := 0
	for i in AI_SCAN_CAP:
		var d: Dictionary = sim.get_entity_debug(i)
		if d.is_empty():
			break
		if not bool(d.get("alive", false)) or not bool(d.get("infantry", false)):
			continue
		if bool(d.get("mounted", false)):
			continue
		checked += 1
		if not bool(d.get("muzzle_valid", false)):
			continue
		stamped += 1
		var pos: Vector3 = d.get("position", Vector3.ZERO)
		var muz: Vector3 = d.get("muzzle", Vector3.ZERO)
		var delta := muz - pos
		var up := delta.y
		var horiz := Vector2(delta.x, delta.z).length()
		if stamped <= 6:
			print("PROBE muzzle net=%d team=%d up=%.2f horiz=%.2f (pos=%s muz=%s)" %
					[int(d.get("net_id", -1)), int(d.get("team", -1)), up, horiz, str(pos), str(muz)])
		# The rifle muzzle: between waist and shoulder, held away from the spine.
		if up > 0.0 and up < 0.8 and horiz > 0.1 and horiz < 2.0:
			good += 1
		# The OLD bug shape: head height, no lateral offset.
		if up > 0.82 and horiz < 0.05:
			head_height += 1

	print("PROBE totals: infantry=%d stamped=%d good=%d head_height=%d" %
			[checked, stamped, good, head_height])
	if stamped == 0:
		print("PROBE FAIL: no NPC received a binding-fed muzzle (the seam is not flowing)")
		quit(1)
		return
	if head_height > 0:
		print("PROBE FAIL: %d muzzles still at the head-height stand-in" % head_height)
		quit(1)
		return
	if good < stamped / 2:
		print("PROBE FAIL: only %d/%d stamped muzzles land in the rifle envelope" % [good, stamped])
		quit(1)
		return
	print("PROBE PASS: %d/%d posed muzzles in the rifle envelope (chest-height, forward of the body); no head-height origins" %
			[good, stamped])
	quit(0)
