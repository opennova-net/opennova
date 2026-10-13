extends GameProbe

## dismember_shot: an aimlocked shot at one body height on a live NPC, for the
## dismemberment leg (world-wac-ai-re §19.2). Clears nodismember on every
## organic and makes the local player NoDie for the run (both restored), stands
## the player `standoff` metres north of the target (the given `wire_handle`,
## else the nearest live NPC) facing it, then every frame projects the target's
## live position plus `height` metres and steers the real look input onto it;
## once locked it fires `burst_frames` through the real input path while still
## tracking, then watches `watch_frames`, capturing every `capture_every`
## frames and logging every pool-0 organic row within 6 m of the victim (alive,
## position, section mask, shared slot, anim, velocity). Fails when it never
## locks or the victim never dies.

const NODISMEMBER := 0x800000
const NODIE := 0x40000000
const LOCK_PX := 3.0
const LOCK_FRAMES := 3
const TRACK_LIMIT := 300
const NEAR_METRES := 6.0


func run(ctx: ProbeContext) -> ProbeVerdict:
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	var sim := ctx.sim()
	var camera := ctx.camera()
	var viewport := ctx.viewport()
	if sim == null or camera == null or viewport == null:
		return ProbeVerdict.failed("the shell seams are unavailable")
	var height := float(ctx.args.get("height", 0.55))
	var burst := int(ctx.args.get("burst_frames", 4))
	var watch := int(ctx.args.get("watch_frames", 240))
	var every := maxi(1, int(ctx.args.get("capture_every", 30)))
	var standoff := float(ctx.args.get("standoff", 4.0))
	var wanted := int(ctx.args.get("wire_handle", -1))
	var out_dir := ProbeOutput.resolve(ctx, String(ctx.args.get("output_dir", "")))

	var player := _local_player_row(sim)
	if player == null:
		return ProbeVerdict.failed("no local player row")
	var player_card: EntityCard = sim.entity_card(player.get_wire_handle())
	var player_attrib := int(player_card.get_item_attrib())
	var player_attrib2 := int(player_card.get_item_attrib2())
	var dismember_was := sim.debug_organics_nodismember()
	ctx.defer_restore(func() -> void:
		ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false)
		sim.debug_set_organics_nodismember(dismember_was)
		sim.debug_set_entity_item_attrib(player.get_wire_handle(), player_attrib, player_attrib2))
	sim.debug_set_organics_nodismember(false)
	sim.debug_set_entity_item_attrib(player.get_wire_handle(), player_attrib | NODIE, player_attrib2)

	var target := _target_row(sim, wanted)
	if target == null:
		return ProbeVerdict.failed("no live NPC to shoot (wire_handle %d)" % wanted)
	var handle := target.get_wire_handle()
	var target_ai := target.get_ai_index()
	# The player stays on its spawn (dry ground). For each of eight headings
	# the target is pinned `standoff` metres ahead of it (a static mark) and
	# the view set onto the target point; the first heading whose crosshair
	# pick meets the target itself wins.
	var spawn := sim.get_local_player_position()
	var stand := Vector3(spawn.x, -spawn.z, spawn.y)
	var at := Vector3.ZERO
	var clear := false
	var only := int(ctx.args.get("bearing", -1))
	for bearing in range(8):
		if only >= 0 and bearing != only:
			continue
		var angle := bearing * PI / 4.0
		at = stand + Vector3(sin(angle), cos(angle), 0.0) * standoff
		sim.debug_teleport_local_player(stand, rad_to_deg(angle), 0.0)
		for _i in range(4):
			sim.debug_set_entity_position(target_ai, at)
			_aim(sim, camera, handle, height)
			await ctx.wait_frames(1)
		var pick := DebugEntityPicker.pick_at_crosshair(sim, camera, null)
		var bearing_png := out_dir.path_join("bearing_%d.png" % bearing)
		if await ProbeCapture.save_viewport_png(viewport, bearing_png):
			ctx.artifact("bearing_%d" % bearing, bearing_png, "png")
		ctx.log("bearing %d: pick hit %s handle %d" % [bearing, str(pick.hit),
				pick.entity_handle if pick.hit else -1])
		if pick.hit and pick.entity_handle == handle:
			clear = true
			ctx.log("clear line on bearing %d: target at %s" % [bearing, str(at)])
			break
	if not clear:
		return ProbeVerdict.failed("no bearing gives a clear line to handle %d" % handle)
	ctx.log("target '%s' handle %d (ai %d) pinned at %s; height %.2f m" % [
			target.get_item_name(), handle, target_ai, str(at), height])

	# Track until locked, then fire while still tracking: each frame the view
	# is set straight onto the target (the debug teleport at the body's own
	# position, the yaw/pitch solved from the camera to the point), since a
	# synthetic mouse look needs the captured cursor of a focused window.
	var centre := viewport.get_visible_rect().size * 0.5
	var locked_frames := 0
	var fired := -1
	var frame := 0
	while frame < TRACK_LIMIT and (fired < 0 or frame - fired < burst):
		if ctx.cancelled:
			return ProbeVerdict.failed("cancelled")
		var row := _row(sim, handle)
		if row == null:
			break
		if fired < 0:
			sim.debug_set_entity_position(target_ai, at)
		var point: Vector3 = _aim(sim, camera, handle, height)
		var delta := Vector2.ZERO if camera.is_position_behind(point) 				else camera.unproject_position(point) - centre
		if frame % 15 == 0:
			ctx.log("frame %d: aim error %s px" % [frame, str(delta)])
		locked_frames = locked_frames + 1 				if not camera.is_position_behind(point) and delta.length() <= LOCK_PX else 0
		if fired < 0 and locked_frames >= LOCK_FRAMES:
			fired = frame
			# One health left: the round that lands is the killing blow, the
			# one the cut reads (health <= 0).
			sim.debug_set_entity_health(target_ai, 1)
			ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, true)
			ctx.log("locked and firing at frame %d" % frame)
		await ctx.wait_frames(1)
		frame += 1
	ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false)
	if fired < 0:
		return ProbeVerdict.failed("never locked on the target in %d frames" % TRACK_LIMIT)

	# Watch: the victim, every clone near it, and the frames.
	var samples: Array = []
	var victim_died := false
	var origin: Vector3 = at
	for f in range(watch + 1):
		if ctx.cancelled:
			return ProbeVerdict.failed("cancelled")
		# Keep the victim in view: the view on its body at ground height.
		_aim(sim, camera, handle, 0.0)
		if f % every == 0:
			var path := out_dir.path_join("frame_%03d.png" % f)
			if await ProbeCapture.save_viewport_png(viewport, path):
				ctx.artifact("frame_%03d" % f, path, "png")
			var near: Array = []
			for row_v in sim.entity_directory():
				var row := row_v as EntityRow
				if row == null:
					continue
				var mission: Vector3 = row.get_mission_position()
				if Vector2(mission.x - origin.x, mission.y - origin.y).length() > NEAR_METRES:
					continue
				var card: EntityCard = sim.entity_card(row.get_wire_handle())
				var ai: Dictionary = card.to_json_value() if card != null else {}
				var entry := {
					"handle": row.get_wire_handle(),
					"alive": row.is_alive(),
					"health": row.get_health(),
					"mission": [snappedf(mission.x, 0.01), snappedf(mission.y, 0.01), snappedf(mission.z, 0.01)],
					"section_mask": "%08x" % (int(ai.get("section_mask", 0)) & 0xFFFFFFFF),
					"shared_slot": int(ai.get("shared_anim_slot", -1)),
					"anim": String(ai.get("anim_key", "")),
					"playing": int(ai.get("anim_playing_state", -1)),
					"phase": int(ai.get("clip_phase", 0)),
					"blend": snappedf(float(ai.get("anim_blend_weight", 1.0)), 0.01),
					"vel": ai.get("vel", []),
				}
				if entry.handle == handle and not entry.alive:
					victim_died = true
				near.append(entry)
			samples.append({"frame": f, "rows": near})
			ctx.log("frame %d: %s" % [f, JSON.stringify(near)])
		await ctx.wait_frames(1)
	var data := {"target": handle, "fired_frame": fired, "samples": samples}
	if not victim_died:
		return ProbeVerdict.failed("the victim did not die", data)
	return ProbeVerdict.passed("shot handle %d at %.2f m; %d samples" % [handle, height, samples.size()], data)


# Set the view straight onto the target point (the debug teleport at the
# body's own position, the yaw/pitch solved from the camera; Godot (x, y up,
# z) -> mission (x east, y north = -z, z up)), since a synthetic mouse look
# needs the captured cursor of a focused window. Returns the point.
func _aim(sim: Simulation, camera: Camera3D, handle: int, height: float) -> Vector3:
	var row := _row(sim, handle)
	if row == null:
		return Vector3.ZERO
	var point: Vector3 = row.get_world_position() + Vector3(0.0, height, 0.0)
	var to := point - camera.global_position
	var yaw := rad_to_deg(atan2(to.x, -to.z))
	var pitch := rad_to_deg(atan2(to.y, Vector2(to.x, to.z).length()))
	var body := sim.get_local_player_position()
	sim.debug_teleport_local_player(Vector3(body.x, -body.z, body.y), yaw, pitch)
	return point


func _local_player_row(sim: Simulation) -> EntityRow:
	for row_v in sim.entity_directory():
		var row := row_v as EntityRow
		# The local player: no SSN, a brain (entity_pick's rule).
		if row != null and row.get_net_id() <= 0 and row.get_ai_index() >= 0:
			return row
	return null


func _target_row(sim: Simulation, wanted: int) -> EntityRow:
	var player: Vector3 = sim.get_local_player_position()
	var best: EntityRow = null
	for row_v in sim.entity_directory():
		var row := row_v as EntityRow
		if row == null or not row.is_alive() or row.get_net_id() <= 0 or row.get_ai_index() < 0:
			continue
		# A body on foot only: a crew member is re-posed onto its seat.
		var card: EntityCard = sim.entity_card(row.get_wire_handle())
		var card_json: Dictionary = card.to_json_value() if card != null else {}
		if not bool(card_json.get("infantry", false)) or int(card_json.get("parent", -1)) >= 0:
			continue
		if wanted >= 0:
			if row.get_wire_handle() == wanted:
				return row
			continue
		if best == null or row.get_world_position().distance_squared_to(player) \
				< best.get_world_position().distance_squared_to(player):
			best = row
	return best


func _row(sim: Simulation, handle: int) -> EntityRow:
	for row_v in sim.entity_directory():
		var row := row_v as EntityRow
		if row != null and row.get_wire_handle() == handle:
			return row
	return null
