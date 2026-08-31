extends GameProbe

## frame_stats: the F3 Stats acceptance probe. Opens the dev tools with their
## Stats window (the real F3 path), lets the capture window fill, snapshots
## every Stats row twice (two consecutive window means must both carry
## numbers), and passes when the load-bearing rows (world tick, sim step,
## present, occlusion apply, HUD) carry live values. Needs a window: the
## tools never attach headless.

const REQUIRED_ROWS := ["world", "sim", "present", "occl_apply", "hud"]
const DUMP_ROWS := ["frame", "before", "world", "foliage", "runtime", "sim", "net",
		"trace", "trace_terrain", "trace_static", "trace_dynamic",
		"trace_person", "effects_drain", "effects", "present", "snapshot",
		"mission_rows", "wire_rows", "fire",
		"destruction", "throwable", "occl", "occl_build",
		"occl_probe", "occl_apply", "occl_glue", "env", "audio", "after", "hud",
		"hud_scalars", "hud_attach", "hud_waypoint", "hud_info", "hud_flush",
		"other_process", "physics_callbacks", "deferred_flush",
		"flush_queued", "hud_draw_compile", "hud_draw_emit",
		"flush_tail", "render_draw", "pacing_input",
		"engine_frame",
		"render", "render_main", "render_shadow", "render_root_cpu",
		"render_root_gpu", "render_water", "render_water_cpu",
		"render_water_gpu"]


func run(ctx: ProbeContext) -> ProbeVerdict:
	# The launch lands under the start-mission splash with the world held
	# un-ticked; leave it and wait for the local player before measuring.
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	var settle_ms := int(ctx.args.get("settle_ms", 3000))
	var window_ms := int(ctx.args.get("window_ms", 2000))
	await ctx.wait_ms(settle_ms)
	if ctx.cancelled:
		return ProbeVerdict.failed("cancelled")
	var census := _model_census(ctx.tree)
	ctx.log("model census: %d models, %d hidden" % [census["models"], census["hidden"]])

	# The real F3 path: open the dev tools; their Stats window (open by default)
	# arms the board's capture and drains it every half second.
	var dev_tools := ctx.dev_tools()
	if dev_tools == null or not dev_tools.is_available():
		return ProbeVerdict.failed("the dev tools are unavailable (no window, or the imgui-godot addon is missing)")
	var was_open := dev_tools.is_open()
	ctx.defer_restore(func() -> void:
		if is_instance_valid(dev_tools):
			dev_tools.set_open(was_open))
	dev_tools.set_open(true)
	# ImGui's ini remembers where the Stats window last sat (another monitor,
	# a collapsed tab); bring every window home so the Stats window draws and
	# refreshes its reading.
	dev_tools.reset_layout()
	await ctx.wait_ms(500)
	var board := ctx.frame_stats()
	if board == null or not board.is_capture_active():
		return ProbeVerdict.failed("the Stats window did not open its capture window")
	ctx.log("dev tools open=%s capture=%s reading_frames=%d" % [
			str(dev_tools.is_open()), str(board.is_capture_active()), dev_tools.stats_reading_frames()])

	await ctx.wait_ms(window_ms)
	var first := _snapshot_rows(dev_tools)
	ctx.log("reading 1 covers %d render frames" % dev_tools.stats_reading_frames())
	_dump(ctx, "reading 1", first)
	await ctx.wait_ms(window_ms)
	if ctx.cancelled:
		return ProbeVerdict.failed("cancelled")
	var second := _snapshot_rows(dev_tools)
	ctx.log("reading 2 covers %d render frames" % dev_tools.stats_reading_frames())
	_dump(ctx, "reading 2", second)

	var missing := PackedStringArray()
	for id in REQUIRED_ROWS:
		if String(second.get(id, ["-", "-"])[0]) == "-":
			missing.append(id)
	var world_a := _row_ms(first, "world")
	var world_b := _row_ms(second, "world")
	if world_a > 0.0 and world_b > 0.0:
		ctx.log("consecutive world-tick readings: %.2f vs %.2f ms (delta %.2f)" % [
				world_a, world_b, absf(world_a - world_b)])
	var data := {
		"readings": [first, second],
		"missing": Array(missing),
		"world_ms": [world_a, world_b],
		"census": census,
	}
	if missing.is_empty():
		return ProbeVerdict.passed("every required Stats row carries live numbers", data)
	return ProbeVerdict.failed("rows without numbers: %s" % ", ".join(missing), data)


static func _snapshot_rows(dev_tools: DevTools) -> Dictionary:
	var out := {}
	for id in dev_tools.stats_row_ids():
		var average := dev_tools.stats_row_average(id)
		out[id] = [average if not average.is_empty() else "-",
				dev_tools.stats_row_peak(id), dev_tools.stats_row_info(id)]
	return out


static func _row_ms(rows: Dictionary, id: String) -> float:
	var avg := String(rows.get(id, ["-"])[0])
	return avg.to_float() if avg != "-" else -1.0


static func _dump(ctx: ProbeContext, label: String, rows: Dictionary) -> void:
	ctx.log("---- %s ----" % label)
	for id in DUMP_ROWS:
		var row: Array = rows.get(id, ["-", "-", ""])
		ctx.log("%-16s avg=%-8s max=%-8s %s" % [id, row[0], row[1], row[2]])


# Placed-model census on public Node/visibility behavior; work-class
# attribution belongs to the Stats rows.
static func _model_census(tree: SceneTree) -> Dictionary:
	var total := 0
	var hidden := 0
	for node in ProbePerfSetup.nodes_of_class(tree, "ObjectModel"):
		total += 1
		if not (node as Node3D).is_visible_in_tree():
			hidden += 1
	return {"models": total, "hidden": hidden}
