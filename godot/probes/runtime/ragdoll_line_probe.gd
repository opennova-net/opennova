extends GameProbe

## ragdoll_line: the opt-in ragdoll corpses (ADR 0060) on the live process. The
## local player is NoDie for the run; the live NPCs on foot nearest it are taken
## in order (with `line_up`, first pinned in a row `standoff` m north of the
## player, `spacing` m apart, as a mission of blind enemies stands them). Each
## in turn: the view set straight onto its body plus `height` m, its health
## dropped to 1, `burst_frames` fired through the real input path, then
## `watch_frames` watched with a capture every `capture_every` frames, logging
## the session's ragdoll counts [awake, sleeping, blending, started]. Passes
## when every target died and each death started a ragdoll; with ragdolls off
## for the session (game.cfg NoBlood bit 1 clear) it fails unless `force` turns
## them on for the run.

const NODIE := 0x40000000


func run(ctx: ProbeContext) -> ProbeVerdict:
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	var sim := ctx.sim()
	var world := ctx.world()
	var camera := ctx.camera()
	var viewport := ctx.viewport()
	if sim == null or world == null or camera == null or viewport == null:
		return ProbeVerdict.failed("the shell seams are unavailable")
	var count := int(ctx.args.get("count", 6))
	var height := float(ctx.args.get("height", 0.4))
	var burst := int(ctx.args.get("burst_frames", 4))
	var watch := int(ctx.args.get("watch_frames", 150))
	var every := maxi(1, int(ctx.args.get("capture_every", 30)))
	var out_dir := ProbeOutput.resolve(ctx, String(ctx.args.get("output_dir", "")))

	var session_on: bool = world.is_session_ragdolls()
	ctx.log("session ragdolls from game.cfg: %s" % str(session_on))
	if not session_on:
		if not bool(ctx.args.get("force", false)):
			return ProbeVerdict.failed("ragdolls are off for this session (game.cfg NoBlood bit 1 clear)")
		world.set_session_ragdolls(true)
		ctx.defer_restore(func() -> void: world.set_session_ragdolls(false))
	if bool(ctx.args.get("observe", false)):
		return await _observe(ctx, world, viewport, out_dir, count, watch, every)

	var player := _local_player_row(sim)
	if player == null:
		return ProbeVerdict.failed("no local player row")
	var card: EntityCard = sim.entity_card(player.get_wire_handle())
	var attrib := int(card.get_item_attrib())
	var attrib2 := int(card.get_item_attrib2())
	ctx.defer_restore(func() -> void:
		ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false)
		sim.debug_set_entity_item_attrib(player.get_wire_handle(), attrib, attrib2))
	sim.debug_set_entity_item_attrib(player.get_wire_handle(), attrib | NODIE, attrib2)

	var targets := _targets(sim, count)
	if targets.is_empty():
		return ProbeVerdict.failed("no live NPC on foot to shoot")
	if bool(ctx.args.get("line_up", false)):
		_line_up(sim, targets, float(ctx.args.get("standoff", 8.0)), float(ctx.args.get("spacing", 2.5)))
		await ctx.wait_frames(30)

	var shots: Array = []
	for i in range(targets.size()):
		if ctx.cancelled:
			return ProbeVerdict.failed("cancelled")
		var target := targets[i] as EntityRow
		var handle := target.get_wire_handle()
		var before := Array(world.get_ragdoll_counts())
		for _f in range(4):
			_aim(sim, camera, handle, height)
			await ctx.wait_frames(1)
		sim.debug_set_entity_health(target.get_ai_index(), 1)
		ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, true)
		for _f in range(burst):
			_aim(sim, camera, handle, height)
			await ctx.wait_frames(1)
		ProbeInput.mouse_btn(MOUSE_BUTTON_LEFT, false)
		var died := false
		for f in range(watch + 1):
			_aim(sim, camera, handle, -0.6)
			var row := _row(sim, handle)
			if row != null and not row.is_alive():
				died = true
			if f % every == 0:
				var path := out_dir.path_join("target%d_frame_%03d.png" % [i, f])
				if await ProbeCapture.save_viewport_png(viewport, path):
					ctx.artifact("target%d_frame_%03d" % [i, f], path, "png")
				ctx.log("target %d (handle %d) frame %d: dead %s ragdolls %s" % [i, handle, f, str(died),
						str(Array(world.get_ragdoll_counts()))])
			await ctx.wait_frames(1)
		var after := Array(world.get_ragdoll_counts())
		shots.append({"handle": handle, "item": target.get_item_name(), "died": died,
				"started": int(after[3]) - int(before[3]), "counts": after})
	var data := {"session_ragdolls": session_on, "shots": shots, "counts": Array(world.get_ragdoll_counts())}
	for shot in shots:
		if not bool(shot.died):
			return ProbeVerdict.failed("handle %d did not die" % int(shot.handle), data)
		if int(shot.started) < 1:
			return ProbeVerdict.failed("handle %d died without a ragdoll" % int(shot.handle), data)
	return ProbeVerdict.passed("%d shot, %d ragdolls started" % [shots.size(), shots.size()], data)


# Shoot nothing: watch `watch` frames (a joiner while its host shoots) and pass
# once `count` ragdolls have started on this machine.
func _observe(ctx: ProbeContext, world: GameWorld, viewport: Viewport, out_dir: String, count: int,
		watch: int, every: int) -> ProbeVerdict:
	var counts := Array(world.get_ragdoll_counts())
	for f in range(watch + 1):
		if ctx.cancelled:
			return ProbeVerdict.failed("cancelled")
		counts = Array(world.get_ragdoll_counts())
		if f % every == 0:
			var path := out_dir.path_join("observe_frame_%04d.png" % f)
			if await ProbeCapture.save_viewport_png(viewport, path):
				ctx.artifact("observe_frame_%04d" % f, path, "png")
			ctx.log("observe frame %d: ragdolls %s" % [f, str(counts)])
		if int(counts[3]) >= count:
			break
		await ctx.wait_frames(1)
	var data := {"counts": counts}
	if int(counts[3]) < count:
		return ProbeVerdict.failed("%d of %d ragdolls started" % [int(counts[3]), count], data)
	return ProbeVerdict.passed("%d ragdolls started on this machine" % int(counts[3]), data)


# Pin the targets in a row `standoff` m north of the player, `spacing` m apart
# across it (mission metres: x east, y north, z up).
func _line_up(sim: Simulation, targets: Array, standoff: float, spacing: float) -> void:
	var body := sim.get_local_player_position()
	var stand := Vector3(body.x, -body.z, body.y)
	sim.debug_teleport_local_player(stand, 0.0, 0.0)
	var half := (targets.size() - 1) * 0.5
	for i in range(targets.size()):
		var at := stand + Vector3((i - half) * spacing, standoff, 0.0)
		sim.debug_set_entity_position((targets[i] as EntityRow).get_ai_index(), at)


# The view set straight onto the target point (the debug teleport at the body's
# own position, the yaw/pitch solved from the camera: Godot (x, y up, z) ->
# mission (x east, y north = -z, z up)).
func _aim(sim: Simulation, camera: Camera3D, handle: int, height: float) -> void:
	var row := _row(sim, handle)
	if row == null:
		return
	var point: Vector3 = row.get_world_position() + Vector3(0.0, height, 0.0)
	var to := point - camera.global_position
	var yaw := rad_to_deg(atan2(to.x, -to.z))
	var pitch := rad_to_deg(atan2(to.y, Vector2(to.x, to.z).length()))
	var body := sim.get_local_player_position()
	sim.debug_teleport_local_player(Vector3(body.x, -body.z, body.y), yaw, pitch)


func _local_player_row(sim: Simulation) -> EntityRow:
	for row_v in sim.entity_directory():
		var row := row_v as EntityRow
		# The local player: no SSN, a brain (entity_pick's rule).
		if row != null and row.get_net_id() <= 0 and row.get_ai_index() >= 0:
			return row
	return null


# The live NPCs on foot, nearest the player first.
func _targets(sim: Simulation, count: int) -> Array:
	var player: Vector3 = sim.get_local_player_position()
	var rows: Array = []
	for row_v in sim.entity_directory():
		var row := row_v as EntityRow
		if row == null or not row.is_alive() or row.get_net_id() <= 0 or row.get_ai_index() < 0:
			continue
		var card: EntityCard = sim.entity_card(row.get_wire_handle())
		var json: Dictionary = card.to_json_value() if card != null else {}
		if not bool(json.get("infantry", false)) or int(json.get("parent", -1)) >= 0:
			continue
		rows.append(row)
	rows.sort_custom(func(a: EntityRow, b: EntityRow) -> bool:
		return a.get_world_position().distance_squared_to(player) \
				< b.get_world_position().distance_squared_to(player))
	return rows.slice(0, count)


func _row(sim: Simulation, handle: int) -> EntityRow:
	for row_v in sim.entity_directory():
		var row := row_v as EntityRow
		if row != null and row.get_wire_handle() == handle:
			return row
	return null
