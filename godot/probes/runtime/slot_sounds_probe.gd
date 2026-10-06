extends GameProbe

## slot_sounds: which sounds the local player's body and rounds play where.
## For each point the player is teleported to the point (teleport_local_player,
## mission frame, with the point's yaw and pitch, then its `look_px` delta
## through the local player's look path, then its `round` straight from the
## round sim when it names one) and plays the point's steps
## through the scripted input device (forward then back by default: a walk
## across a small patch and back; a `fire` press and a pitch down at the ground
## for impacts); every one-shot MissionAudio records meanwhile is reported with
## its set, whether it was a body slot sound, whether the bank played it, and
## where (mission frame), so a run over the base game's surface pad shows the
## set each surface's footsteps and impacts resolve to. `weapon` equips that
## weapon.def row first. Single player or host (the teleport is the
## authority's write).

const LOCAL_PLAYER_TIMEOUT_MS := 60_000
const START_LATCH_TIMEOUT_MS := 10_000
## Forward 0.65 s then back 0.65 s, retail action codes (forward 152, back 151).
const DEFAULT_STEPS := [
	{"tick": 0, "down": 152}, {"tick": 40, "up": 152},
	{"tick": 44, "down": 151}, {"tick": 84, "up": 151},
	{"tick": 90, "end": true},
]


func run(ctx: ProbeContext) -> ProbeVerdict:
	var points: Array = ctx.args.get("points", [])
	if points.is_empty():
		return ProbeVerdict.failed("pass `points`: [{position_bms: [x, y, z], yaw?, label?, steps?}]")
	var settle_ms := int(ctx.args.get("settle_ms", 400))
	var weapon := String(ctx.args.get("weapon", ""))
	if not await ctx.wait_for_local_player(LOCAL_PLAYER_TIMEOUT_MS):
		return ProbeVerdict.failed("no local player to drive")
	var sim := ctx.sim()
	var world := ctx.world()
	if sim == null or world == null or sim.is_joiner():
		return ProbeVerdict.failed("needs the authority's simulation and world")
	var audio: MissionAudio = world.get_mission_audio()
	if audio == null:
		return ProbeVerdict.failed("the world has no mission audio")
	if not weapon.is_empty():
		if not world.set_local_player_weapon_by_name(weapon):
			return ProbeVerdict.failed("%s is not in this root's weapon.def" % weapon)
		var presenter := ctx.presenter()
		if presenter != null:
			presenter.refresh_viewmodel()
		await ctx.wait_ms(1500)
		ctx.log("equipped %s" % weapon)
	var controls := ControlsBindings.model()
	var previous := controls.get_scripted_input()
	if previous != null and not previous.is_done():
		previous.cancel()
	ctx.defer_restore(func() -> void:
		var attached := controls.get_scripted_input()
		if attached != null:
			attached.cancel()
		controls.set_scripted_input(null))

	var results: Array = []
	var total_slot := 0
	for index in points.size():
		if ctx.cancelled:
			return ProbeVerdict.failed("cancelled", {"points": results})
		var point: Dictionary = points[index]
		var pos: Array = point.get("position_bms", [])
		if pos.size() != 3:
			return ProbeVerdict.failed("point %d: position_bms must be [x, y, z]" % index)
		var err := sim.debug_teleport_local_player(Vector3(float(pos[0]), float(pos[1]), float(pos[2])),
				float(point.get("yaw", 0.0)), float(point.get("pitch", 0.0)))
		if err != OK:
			return ProbeVerdict.failed("point %d: the teleport refused (error %d)" % [index, err])
		# Let the body land and the teleport's own sounds pass before watching.
		await ctx.wait_ms(settle_ms)
		# A look delta through the local player's look path (screen pixels, +y down).
		var look: Array = point.get("look_px", [])
		if look.size() == 2:
			sim.add_local_player_look(float(look[0]), float(look[1]))
			await ctx.wait_ms(200)
		var seen := {}
		for fired in audio.recent_fired_soundsets():
			seen[(fired as FiredSoundset).get_instance_id()] = true
		# A round straight from the round sim (debug_spawn_round, the local player its
		# owner): {ammo, from_bms, dir_bms}, the impact played as the sim plays it.
		var round_spec: Dictionary = point.get("round", {})
		if not round_spec.is_empty():
			var from_b: Array = round_spec.get("from_bms", [])
			var dir_b: Array = round_spec.get("dir_bms", [])
			if from_b.size() != 3 or dir_b.size() != 3:
				return ProbeVerdict.failed("point %d: round needs from_bms and dir_bms [x, y, z]" % index)
			var ammo := String(round_spec.get("ammo", ""))
			var from_godot := Vector3(float(from_b[0]), float(from_b[2]), -float(from_b[1]))
			var dir_godot := Vector3(float(dir_b[0]), float(dir_b[2]), -float(dir_b[1]))
			if sim.debug_spawn_round(from_godot, dir_godot, ammo) < 0:
				return ProbeVerdict.failed("point %d: the round sim refused %s" % [index, ammo])
		var driver := ScriptedInput.new()
		if driver.load_steps(point.get("steps", DEFAULT_STEPS)) != OK:
			return ProbeVerdict.failed("point %d: steps refused: %s" % [index, driver.get_error()])
		controls.set_scripted_input(driver)
		driver.start()
		var fires: Array = []
		var deadline := Time.get_ticks_msec() + START_LATCH_TIMEOUT_MS
		while not driver.is_done():
			if ctx.cancelled:
				driver.cancel()
				break
			if driver.get_state() == ScriptedInput.STATE_ARMED and Time.get_ticks_msec() >= deadline:
				driver.cancel()
				return ProbeVerdict.failed("point %d: no device sample latched the start" % index)
			_collect(audio, seen, fires)
			await ctx.tree.process_frame
		await ctx.wait_ms(int(point.get("after_ms", 100)))
		_collect(audio, seen, fires)
		controls.set_scripted_input(null)
		var slot_count := 0
		for f in fires:
			if bool(f.slot):
				slot_count += 1
		total_slot += slot_count
		var row := {
			"label": String(point.get("label", str(index))),
			"position_bms": pos,
			"fires": fires,
			"slot_fires": slot_count,
		}
		results.append(row)
		ctx.log("%-10s %d sounds: %s" % [row.label, fires.size(), ", ".join(_names(fires))])
	var total := 0
	for row in results:
		total += (row.fires as Array).size()
	var summary := {"points": results, "slot_fires": total_slot, "fires": total}
	if total == 0:
		return ProbeVerdict.failed("no sound fired at any point", summary)
	return ProbeVerdict.passed("%d sounds (%d body slot sounds) over %d points" % [
			total, total_slot, results.size()], summary)


## Append the one-shots recorded since `seen` (by instance), mission frame.
func _collect(audio: MissionAudio, seen: Dictionary, fires: Array) -> void:
	for value in audio.recent_fired_soundsets():
		var fired := value as FiredSoundset
		var id := fired.get_instance_id()
		if seen.has(id):
			continue
		seen[id] = true
		var p := fired.get_position()
		fires.append({
			"set": fired.get_set_name(),
			"slot": fired.is_slot(),
			"played": fired.is_played(),
			"position_bms": [snappedf(p.x, 0.01), snappedf(-p.z, 0.01), snappedf(p.y, 0.01)],
		})


func _names(fires: Array) -> PackedStringArray:
	var out: PackedStringArray = []
	for f in fires:
		out.append("%s%s" % [f.set, "" if bool(f.played) else " (not played)"])
	return out
