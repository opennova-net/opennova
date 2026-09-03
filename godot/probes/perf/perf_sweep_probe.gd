extends GameProbe

## perf_sweep: the frame-cost decomposition sweep. Enumerates every processing
## group (nodes sharing a script, or a native class for scriptless nodes) with
## _process/_physics_process enabled, disables one group at a time and
## measures the frame-time saving; two render-side phases (3D at half
## resolution scale, then cull mask 0) bound the GPU share. Reports a savings
## table, largest first. Needs a window: the render share is the question.

# The MCP transport that carries this very probe: disabling its _process would
# stall the runner's watchdog and the client's polling, so its groups are not
# part of the sweep.
const TRANSPORT_SCRIPT_PREFIXES := ["res://game/mcp/", "res://game/probe/"]
# The imgui-godot bridge: its helper runs ImGui::NewFrame at the lowest process
# priority and its controller renders at the highest. Switching one half off
# leaves every frame un-ended (IM_ASSERT spam in the log) and says nothing
# about the game's cost, so neither is swept.
const IMGUI_BRIDGE_CLASSES := ["ImGuiController", "ImGuiControllerHelper", "ImGuiLayer"]

var _pending_states: Array = []


func run(ctx: ProbeContext) -> ProbeVerdict:
	# The launch lands under the start-mission splash with the world held
	# un-ticked; leave it and wait for the local player before measuring.
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	var phase_ms := int(ctx.args.get("phase_ms", 2600))
	ProbePerfSetup.uncap_frame_rate(ctx)
	await ctx.wait_ms(1000)
	# A cancelled run mid-phase still restores the group it was measuring.
	ctx.defer_restore(func() -> void:
		ProbeProcessGuard.restore_processing(_pending_states)
		_pending_states = [])

	# --- Enumerate processing groups. ---
	var groups: Dictionary = {}  # key -> {nodes: [], phys: int, proc: int}
	var stack: Array = [ctx.tree.root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		for ch in n.get_children():
			stack.push_back(ch)
		# No method filter: native nodes (e.g. the particle renderer) run on
		# NOTIFICATION_PROCESS without exposing a script _process at all.
		var has_proc := n.is_processing()
		var has_phys := n.is_physics_processing()
		if not has_proc and not has_phys:
			continue
		var scr = n.get_script()
		var key := ""
		if scr != null and scr is Resource:
			var script_path := (scr as Resource).resource_path
			var transport := false
			for prefix in TRANSPORT_SCRIPT_PREFIXES:
				if script_path.begins_with(prefix):
					transport = true
					break
			if transport:
				continue
			# An inner-class script has no path of its own: name it by class.
			key = script_path.get_file() if not script_path.is_empty() \
					else "<inner> " + n.get_class()
		else:
			if IMGUI_BRIDGE_CLASSES.has(n.get_class()):
				continue
			key = "<native> " + n.get_class()
		if not groups.has(key):
			groups[key] = {nodes = [], phys = 0, proc = 0}
		(groups[key].nodes as Array).append(n)
		if has_proc:
			groups[key].proc += 1
		if has_phys:
			groups[key].phys += 1
	ctx.log("%d processing groups:" % groups.size())
	for key in groups.keys():
		ctx.log("  %-46s nodes=%-3d proc=%d phys=%d" % [
				key, (groups[key].nodes as Array).size(), groups[key].proc, groups[key].phys])

	# --- Baseline. ---
	var base := await _measure_ms(ctx, phase_ms * 2)
	ctx.log("BASELINE avg=%.2fms (n=%d)" % [base.avg, base.n])

	# --- Toggle sweep. ---
	var results: Array = []
	for key in groups.keys():
		if ctx.cancelled:
			return ProbeVerdict.failed("cancelled during the sweep")
		var nodes: Array = groups[key].nodes
		# Processing modes can legitimately change while earlier groups are being
		# measured. Snapshot this group's live state immediately before its phase,
		# not during the minutes-earlier census.
		_pending_states = ProbeProcessGuard.disable_processing(nodes)
		var st := await _measure_ms(ctx, phase_ms)
		ProbeProcessGuard.restore_processing(_pending_states)
		_pending_states = []
		var saved: float = base.avg - st.avg
		results.append({key = key, avg = st.avg, saved = saved, nodes = nodes.size()})
		ctx.log("off:%-46s avg=%7.2fms saved=%+7.2fms" % [key, st.avg, saved])
		ctx.progress({"groups_done": results.size(), "groups": groups.size()})
		await ctx.wait_ms(300)

	# --- Render-side bounds. ---
	var vp := ctx.viewport()
	var half := {avg = -1.0, n = 0}
	var culled := {avg = -1.0, n = 0}
	if vp != null:
		var prev_scale := vp.scaling_3d_scale
		vp.scaling_3d_scale = 0.5
		half = await _measure_ms(ctx, phase_ms)
		vp.scaling_3d_scale = prev_scale
		ctx.log("render: 3d_scale=0.5 avg=%.2fms saved=%+.2fms vs base" % [half.avg, base.avg - half.avg])
		var cam := vp.get_camera_3d()
		if cam != null:
			var prev_mask := cam.cull_mask
			cam.cull_mask = 0
			culled = await _measure_ms(ctx, phase_ms)
			cam.cull_mask = prev_mask
			ctx.log("render: cull_mask=0 avg=%.2fms saved=%+.2fms vs base" % [culled.avg, base.avg - culled.avg])

	# --- Sorted verdict table. ---
	results.sort_custom(func(a, b): return float(a.saved) > float(b.saved))
	ctx.log("===== savings, largest first =====")
	for r_v in results:
		var r: Dictionary = r_v
		if float(r.saved) > 2.0:
			ctx.log("  %-46s saved=%+8.2fms -> avg %.2fms" % [r.key, r.saved, r.avg])
	ctx.log("baseline %.2fms | half-res saves %.2fms | cull-all saves %.2fms" % [
			base.avg, base.avg - half.avg, base.avg - culled.avg])
	var data := {
		"baseline_ms": base.avg,
		"baseline_frames": base.n,
		"half_resolution_ms": half.avg,
		"cull_all_ms": culled.avg,
		"groups": results,
	}
	return ProbeVerdict.passed("baseline %.2f ms over %d groups" % [base.avg, results.size()], data)


func _measure_ms(ctx: ProbeContext, duration_ms: int) -> Dictionary:
	var samples: Array[float] = []
	var deadline := Time.get_ticks_msec() + duration_ms
	# One throwaway frame so a just-restored group's first tick doesn't pollute.
	await ctx.tree.process_frame
	var last := Time.get_ticks_usec()
	while Time.get_ticks_msec() < deadline and not ctx.cancelled:
		await ctx.tree.process_frame
		var now := Time.get_ticks_usec()
		samples.append(float(now - last) / 1000.0)
		last = now
	var n := samples.size()
	if n == 0:
		return {avg = 0.0, n = 0}
	var sum := 0.0
	for v in samples:
		sum += v
	return {avg = sum / n, n = n}
