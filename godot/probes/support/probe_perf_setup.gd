class_name ProbePerfSetup
extends RefCounted

## The measurement preconditions the perf probes share, every one undone
## through the context when the run finishes: an uncapped frame rate (vsync
## off, no fps cap), the shell's frame-span instrumentation, and the render
## time split on the shell viewport.


## Vsync off and Engine.max_fps 0 for the run.
static func uncap_frame_rate(ctx: ProbeContext) -> void:
	var previous_vsync := DisplayServer.window_get_vsync_mode()
	var previous_fps := Engine.max_fps
	ctx.defer_restore(func() -> void:
		DisplayServer.window_set_vsync_mode(previous_vsync)
		Engine.max_fps = previous_fps)
	DisplayServer.window_set_vsync_mode(DisplayServer.VSYNC_DISABLED)
	Engine.max_fps = 0


## The shell's and the world's frame-span probes on (their clock reads are
## opt-in so a retail frame is never perturbed); off again at finish.
static func enable_spans(ctx: ProbeContext) -> void:
	var shell := ctx.game()
	if shell != null:
		ctx.defer_restore(func() -> void:
			if is_instance_valid(shell):
				shell.set_perf_probe_enabled(false))
		shell.set_perf_probe_enabled(true)
	var world := ctx.world()
	if world != null:
		ctx.defer_restore(func() -> void:
			if is_instance_valid(world):
				world.set_perf_probe_enabled(false))
		world.set_perf_probe_enabled(true)


## RS main-thread CPU vs GPU per frame on the shell viewport. Returns the
## viewport RID (invalid when the shell has none).
static func measure_render_time(ctx: ProbeContext) -> RID:
	var viewport := ctx.viewport()
	if viewport == null:
		return RID()
	var rid := viewport.get_viewport_rid()
	ctx.defer_restore(func() -> void:
		if rid.is_valid():
			RenderingServer.viewport_set_measure_render_time(rid, false))
	RenderingServer.viewport_set_measure_render_time(rid, true)
	return rid


## Census of the render-relevant node classes under the tree root.
static func node_census(tree: SceneTree) -> Dictionary:
	var counts := {}
	var stack: Array = [tree.root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		for cls in ["MeshInstance3D", "MultiMeshInstance3D", "GPUParticles3D",
				"CPUParticles3D", "Camera3D", "AnimationPlayer", "AudioStreamPlayer3D",
				"Skeleton3D"]:
			if n.is_class(cls):
				counts[cls] = int(counts.get(cls, 0)) + 1
		for ch in n.get_children():
			stack.push_back(ch)
	return counts


## Every node of one class under the root.
static func nodes_of_class(tree: SceneTree, cls: String) -> Array:
	var out: Array = []
	var stack: Array = [tree.root]
	while not stack.is_empty():
		var n: Node = stack.pop_back()
		for ch in n.get_children():
			stack.push_back(ch)
		if n.is_class(cls):
			out.append(n)
	return out
