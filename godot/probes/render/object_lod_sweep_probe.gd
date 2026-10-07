extends GameProbe

## object_lod_sweep: the authored RLOD level the live process draws for one
## placed entity of each named graphic, the local player stood at each of a
## list of distances from it and facing it (renderer::select_object_lod through
## ObjectModel.update_authored_lods for an individual model, through
## MissionObjectPlacer.update_static_lods for a static population row). An
## individual model reports ObjectModel.get_active_lod; a static row reports
## the level of the population that carries its live row. Each sample records
## the camera's distance to the entity and, with `capture`, a frame.

const SETTLE_FRAMES := 6

var _populations: Array[StaticPopulationInstance] = []
var _models: Array[ObjectModel] = []
var _pitch_sign := 1


func run(ctx: ProbeContext) -> ProbeVerdict:
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	var sim := ctx.sim()
	var world := ctx.world()
	var camera := ctx.camera()
	if sim == null or world == null or camera == null:
		return ProbeVerdict.failed("the shell seams are unavailable")
	await ctx.wait_ms(1250)
	for node in world.find_children("*", "StaticPopulationInstance", true, false):
		_populations.append(node as StaticPopulationInstance)
	for node in world.find_children("*", "ObjectModel", true, false):
		_models.append(node as ObjectModel)

	var wanted: Array = ctx.args["graphics"]
	var targets := _targets(sim, wanted)
	if targets.is_empty():
		return ProbeVerdict.failed("no placed entity draws any of %s" % str(wanted))
	var yaw_map := await _calibrate_yaw(ctx, sim, camera, targets[0])
	var rows: Array[Dictionary] = []
	for target: Dictionary in targets:
		var levels := PackedInt32Array()
		for distance_v in ctx.args["distances"]:
			var distance := float(distance_v)
			var sample := await _sample(ctx, sim, camera, target, distance, yaw_map)
			levels.append(int(sample["lod"]))
			rows.append(sample)
		ctx.log("%s (%s): %s" % [target["graphic"], target["kind"], str(levels)])
	var path := ProbeOutput.resolve(ctx, "").path_join("object_lod_sweep.json")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return ProbeVerdict.failed("cannot save the samples")
	file.store_string(JSON.stringify(rows, "\t"))
	file.close()
	ctx.artifact("object_lod_sweep", path, "json")
	return ProbeVerdict.passed("%d samples over %d entities" % [rows.size(), targets.size()])


## One entity per wanted graphic (every graphic when the list is empty): a
## static row the populations carry, else the individual model nearest the
## entity's position.
func _targets(sim: Simulation, wanted: Array) -> Array[Dictionary]:
	var out: Array[Dictionary] = []
	var seen := {}
	for row_v in sim.entity_directory():
		var row := row_v as EntityRow
		if row == null or not row.is_alive():
			continue
		if row.get_net_id() <= 0 and row.get_ai_index() >= 0:
			continue  # the local player
		var position: Vector3 = row.get_world_position()
		var target := {}
		var graphic := _static_graphic(row.get_bms_id())
		if not graphic.is_empty():
			target = {"kind": "static", "graphic": graphic, "bms_id": row.get_bms_id()}
		else:
			var model := _nearest_model(position)
			if model == null:
				continue
			target = {"kind": "model", "graphic": model.get_graphic_name(), "model": model}
		if seen.has(target["graphic"]):
			continue
		if not wanted.is_empty() and not wanted.has(target["graphic"]):
			continue
		seen[target["graphic"]] = true
		target["item"] = row.get_item_name()
		target["mission"] = row.get_mission_position()
		target["world"] = position
		out.append(target)
	return out


func _static_graphic(bms_id: int) -> String:
	if bms_id <= 0:
		return ""
	for population in _populations:
		if population.get_slot_bms_ids().has(bms_id):
			return population.get_graphic()
	return ""


func _nearest_model(position: Vector3) -> ObjectModel:
	var best: ObjectModel = null
	var best_distance := 0.75
	for model in _models:
		if not is_instance_valid(model) or model.get_graphic_name().is_empty():
			continue
		var distance := model.global_position.distance_to(position)
		if distance < best_distance:
			best = model
			best_distance = distance
	return best


## The drawn level of the target: an individual model's active level, or the
## level of the population whose live rows carry the static entity, found by
## its row transform (only shadow-tagged populations publish their row slots);
## -1 when no population draws it this frame.
func _level(target: Dictionary) -> int:
	if target["kind"] == "model":
		var model: ObjectModel = target["model"]
		return model.get_active_lod() if is_instance_valid(model) else -1
	var at: Vector3 = target["world"]
	for population in _populations:
		if not is_instance_valid(population) or population.multimesh == null:
			continue
		if population.get_graphic() != target["graphic"] or not population.is_visible_in_tree():
			continue
		if not population.get_slot_bms_ids().has(int(target["bms_id"])):
			continue
		var mesh := population.multimesh
		for row in range(mesh.visible_instance_count):
			var origin := population.global_transform * mesh.get_instance_transform(row).origin
			if origin.distance_to(at) < 0.25:
				return population.get_lod_index()
	return -1


## The teleport takes a mission yaw; the world heading the camera faces for
## it, measured off the camera at yaw 0 and 90: [heading at 0, sign].
func _calibrate_yaw(ctx: ProbeContext, sim: Simulation, camera: Camera3D,
		target: Dictionary) -> PackedFloat32Array:
	var at: Vector3 = target["mission"]
	var headings := PackedFloat32Array()
	for yaw: float in [0.0, 90.0]:
		sim.debug_teleport_local_player(at + Vector3(30, 0, 0), yaw, 0.0)
		await ctx.wait_frames(SETTLE_FRAMES)
		var forward := -camera.global_transform.basis.z
		headings.append(rad_to_deg(atan2(forward.x, forward.z)))
	sim.debug_teleport_local_player(at + Vector3(30, 0, 0), 0.0, 20.0)
	await ctx.wait_frames(SETTLE_FRAMES)
	_pitch_sign = 1 if (-camera.global_transform.basis.z).y > 0.0 else -1
	var turn := wrapf(headings[1] - headings[0], -180.0, 180.0)
	ctx.log("yaw 0 faces heading %.1f, yaw 90 faces %.1f" % [headings[0], headings[1]])
	return PackedFloat32Array([headings[0], signf(turn)])


func _sample(ctx: ProbeContext, sim: Simulation, camera: Camera3D, target: Dictionary,
		distance: float, yaw_map: PackedFloat32Array) -> Dictionary:
	var at: Vector3 = target["mission"]
	var world_at: Vector3 = target["world"]
	# Stand on the mission +x side, then turn to face the entity: the first
	# teleport finds the eye, the second sets the heading and pitch to it.
	var stand := at + Vector3(distance, 0, 0)
	sim.debug_teleport_local_player(stand, 0.0, 0.0)
	await ctx.wait_frames(SETTLE_FRAMES)
	var to := world_at - camera.global_transform.origin
	var want := rad_to_deg(atan2(to.x, to.z))
	var yaw := wrapf((want - yaw_map[0]) * yaw_map[1], -180.0, 180.0)
	var pitch := rad_to_deg(atan2(to.y, Vector2(to.x, to.z).length())) * float(_pitch_sign)
	sim.debug_teleport_local_player(stand, yaw, pitch)
	await ctx.wait_frames(SETTLE_FRAMES)
	var eye := camera.global_transform.origin
	var forward := -camera.global_transform.basis.z
	var to_target := (world_at - eye).normalized()
	var sample := {
		"graphic": target["graphic"], "item": target["item"], "kind": target["kind"],
		"distance": distance, "camera_distance": eye.distance_to(world_at),
		"facing_deg": rad_to_deg(forward.angle_to(to_target)),
		"lod": _level(target), "fov": camera.fov,
		"viewport": [ctx.viewport().get_visible_rect().size.x,
				ctx.viewport().get_visible_rect().size.y],
	}
	if bool(ctx.args["capture"]):
		var label := "%s_%05.1fm_lod%d" % [target["graphic"], distance, sample["lod"]]
		var path := ProbeOutput.resolve(ctx, "").path_join(label + ".png")
		if await ProbeCapture.save_viewport_png(ctx.viewport(), path):
			ctx.artifact(label, path, "png")
			sample["png"] = path
	return sample
