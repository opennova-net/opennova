extends GameProbe

# Captures the same view at rest, during real local-player movement, and after
# stopping. This is a device/render observation; movement correctness is native.
func run(ctx: ProbeContext) -> ProbeVerdict:
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player")
	var input := PlayerMoveIntent.new()
	ctx.presenter().set_input_override(input)
	ctx.defer_restore(func() -> void:
		if ctx.presenter() != null:
			ctx.presenter().set_input_override(null))
	# Let the opening mission-event cycle finish before debug staging.
	await ctx.wait_ms(1250)
	var position: Array = ctx.args["position"]
	if position.size() == 3:
		var target := Vector3(float(position[0]), float(position[1]), float(position[2]))
		if ctx.sim().debug_teleport_local_player(target, float(ctx.args["yaw_deg"]), 0.0) != OK:
			return ProbeVerdict.failed("cannot stage player")
	await ctx.wait_frames(30)
	var out_dir := ProbeOutput.resolve(ctx, "")
	var samples: Array[Dictionary] = []
	for frame in range(18):
		input.right = bool(ctx.args["move"]) and frame >= 3 and frame < 15
		await ctx.wait_frames(2)
		await ctx.wait_ms(int(ctx.args["interval_ms"]))
		var label := "%02d_%s" % [frame, "moving" if input.right else "still"]
		var path := out_dir.path_join(label + ".png")
		if not await ProbeCapture.save_viewport_png(ctx.viewport(), path):
			return ProbeVerdict.failed("no frame")
		ctx.artifact(label, path, "png")
		var player: Vector3 = ctx.sim().get_local_player_position()
		var camera: Vector3 = ctx.camera().global_transform.origin
		samples.append({"label": label, "time_us": Time.get_ticks_usec(),
				"render_frame": Engine.get_frames_drawn(),
				"player": [player.x, player.y, player.z],
				"camera": [camera.x, camera.y, camera.z]})
	input.right = false
	var path := out_dir.path_join("motion.json")
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return ProbeVerdict.failed("cannot save samples")
	file.store_string(JSON.stringify(samples, "\t"))
	file.close()
	ctx.artifact("motion", path, "json")
	return ProbeVerdict.passed("captured player-view frames and motion samples")
