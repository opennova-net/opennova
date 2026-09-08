extends GameProbe

## Exercise a mounted vehicle through the presenter's public movement-input
## seam. Record actual native positions/headings and capture the vehicle HUD.
## Uses a typed override so desktop focus cannot silently neutralize a run.

func run(ctx: ProbeContext) -> ProbeVerdict:
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player")
	var ssn := int(ctx.args["vehicle_ssn"])
	var index := -1
	for i in ctx.sim().get_entity_count():
		if ctx.sim().get_entity_net_id(i) == ssn:
			index = i
			break
	if index < 0 or ctx.sim().debug_crew_local_player(ssn) != OK:
		return ProbeVerdict.failed("vehicle is missing or cannot seat the local player")
	ctx.defer_restore(func() -> void:
		if ctx.presenter() != null:
			ctx.presenter().set_input_override(null))
	var input := PlayerMoveIntent.new()
	ctx.presenter().set_input_override(input)
	await ctx.wait_frames(30)
	var out_dir := ProbeOutput.resolve(ctx, "")
	var samples: Array[Dictionary] = []
	samples.append(_sample(ctx, index, "mounted"))
	if not await _capture(ctx, out_dir, "mounted"):
		return ProbeVerdict.failed("no mounted frame")
	var ascend_ms := int(ctx.args["ascend_ms"])
	if ascend_ms > 0:
		# A cold rotor takes about eighteen seconds to spool through the motor.
		# Keep the collective test separate from that startup delay.
		await ctx.wait_ms(20000)
		input.lean_right = true
		await ctx.wait_ms(ascend_ms)
		input.lean_right = false
		samples.append(_sample(ctx, index, "ascended"))
	input.forward = true
	await ctx.wait_ms(int(ctx.args["forward_ms"]))
	samples.append(_sample(ctx, index, "forward"))
	if not await _capture(ctx, out_dir, "forward"):
		return ProbeVerdict.failed("no driving frame")
	input.right = true
	await ctx.wait_ms(int(ctx.args["turn_ms"]))
	input.right = false
	input.forward = false
	samples.append(_sample(ctx, index, "turned"))
	input.back = true
	await ctx.wait_ms(1500)
	input.back = false
	await ctx.wait_frames(15)
	samples.append(_sample(ctx, index, "braked"))
	if not await _capture(ctx, out_dir, "braked"):
		return ProbeVerdict.failed("no final frame")
	var first: Vector3 = samples.front()["position"]
	var last: Vector3 = samples.back()["position"]
	var distance := first.distance_to(last)
	var ground_distance := Vector2(first.x, first.z).distance_to(Vector2(last.x, last.z))
	var data := {"vehicle_ssn": ssn, "distance": distance,
			"ground_distance": ground_distance, "samples": samples}
	var path := out_dir.path_join("motion.json")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return ProbeVerdict.failed("cannot write motion record")
	file.store_string(JSON.stringify(data, "	"))
	file.close()
	ctx.artifact("motion", path, "json")
	if ctx.cancelled:
		return ProbeVerdict.failed("cancelled", data)
	if ground_distance < 3.0:
		return ProbeVerdict.failed("vehicle did not travel three units horizontally through the input path", data)
	var family := ctx.sim().entity_card_by_net_id(ssn).get_vehicle_family()
	if family != 2 and family != 3 and absf(last.y - first.y) > 20.0:
		return ProbeVerdict.failed("ground or water vehicle lost vertical support", data)
	return ProbeVerdict.passed("vehicle moved %.2f units; driving and HUD frames captured" % distance, data)


func _sample(ctx: ProbeContext, index: int, label: String) -> Dictionary:
	var row := {"stage": label, "position": ctx.sim().get_entity_position(index),
			"yaw": ctx.sim().get_entity_yaw_deg(index), "state": ctx.sim().get_entity_state(index)}
	var water := ctx.world().get_node_or_null("Water")
	if water != null:
		row["water_height"] = water.get("water_height")
	ctx.log("%s position=%s yaw=%.3f state=%d" % [label, str(row.position), row.yaw, row.state])
	return row


func _capture(ctx: ProbeContext, out_dir: String, label: String) -> bool:
	var path := out_dir.path_join(label + ".png")
	if not await ProbeCapture.save_viewport_png(ctx.viewport(), path):
		return false
	ctx.artifact(label, path, "png")
	return true
