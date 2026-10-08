extends GameProbe

## ambient_channels: what the mission's ambience plays where the listener stands. For each point the
## local player is teleported to (teleport_local_player, mission frame, with the point's yaw), after
## `settle_ms` (the sources' cohorts register every 8th tick, the channels' mix every frame) the live
## MissionAudio's ambient channels are read: each channel's wave (its candidate's layer's member 0),
## its volume in dB and the 0..255 byte the engine's volume law sets it from, and its place (mission
## frame), loudest first; and the camera's place, the listener. With no points, where the player
## stands. The editor's Listen (ADR 0046 DI-36: the mission view's body `listen.channels`) is
## compared against it at the same place and hour. Single player or host (the teleport is the
## authority's write).

const DEFAULT_SETTLE_MS := 1500


func run(ctx: ProbeContext) -> ProbeVerdict:
	var points: Array = ctx.args.get("points", [])
	var settle_ms := int(ctx.args.get("settle_ms", DEFAULT_SETTLE_MS))
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player")
	var sim := ctx.sim()
	var world := ctx.world()
	if sim == null or world == null:
		return ProbeVerdict.failed("needs a loaded world")
	var audio: MissionAudio = world.get_mission_audio()
	if audio == null:
		return ProbeVerdict.failed("the world has no mission audio")
	if points.is_empty():
		points = [{}]
	var results: Array = []
	for index in points.size():
		if ctx.cancelled:
			return ProbeVerdict.failed("cancelled", {"points": results})
		var point: Dictionary = points[index]
		var pos: Array = point.get("position_bms", [])
		if not pos.is_empty():
			if pos.size() != 3 or sim.is_joiner():
				return ProbeVerdict.failed("point %d: position_bms is [x, y, z], on the authority" % index)
			var err := sim.debug_teleport_local_player(Vector3(float(pos[0]), float(pos[1]), float(pos[2])),
					float(point.get("yaw", 0.0)), float(point.get("pitch", 0.0)))
			if err != OK:
				return ProbeVerdict.failed("point %d: the teleport refused (error %d)" % [index, err])
		await ctx.wait_ms(settle_ms)
		var channels: Array = []
		for id: Variant in audio.active_ambient_candidate_ids():
			var player := audio.ambient_player_for_candidate(int(id))
			if player == null:
				continue
			var db := player.volume_db
			channels.append({
				"candidate": int(id),
				"wave": audio.ambient_candidate_wave(int(id)),
				"volume_db": snappedf(db, 0.01),
				"volume": roundi(255.0 * db_to_linear(db)),
				"pitch": snappedf(player.pitch_scale, 0.001),
				"at": _mission(player.global_position),
			})
		channels.sort_custom(func(a: Dictionary, b: Dictionary) -> bool: return float(a["volume_db"]) > float(b["volume_db"]))
		var camera := ctx.tree.root.get_viewport().get_camera_3d()
		var row := {
			"label": String(point.get("label", "point %d" % index)),
			"channels": channels,
			"listener": _mission(camera.global_position) if camera != null else [],
		}
		ctx.log("%s: %d channels" % [row["label"], channels.size()])
		for channel: Dictionary in channels:
			ctx.log("  %s %d (%.1f dB) at %s" % [channel["wave"], channel["volume"], channel["volume_db"], str(channel["at"])])
		results.append(row)
	return ProbeVerdict.passed("%d point(s) read" % results.size(), {"points": results})


func _mission(at: Vector3) -> Array:
	var placed := MissionObjectPlacer.godot_to_bms_position(at)
	return [snappedf(placed.x, 0.01), snappedf(placed.y, 0.01), snappedf(placed.z, 0.01)]
