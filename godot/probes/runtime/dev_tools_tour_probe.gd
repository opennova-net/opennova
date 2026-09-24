extends GameProbe

## dev_tools_tour: the F3 workspace on the live process. Opens the tools,
## opens every inspection window in turn and captures it (a window that
## fails to draw its record shows in the capture), reads the Game window's
## session readout, selects the nearest entity and turns every Game-view
## overlay layer on, checking the layers that must draw (the selection marker
## on an on-screen target, the AI labels when a brain stands in view, the ray
## capture always) drew this frame and capturing the Game view, then captures
## the Wireframe viewport view through the viewport_debug_draw row. Needs a
## window and a mission.

const SETTLE_FRAMES := 8
const OVERLAY_FRAMES := 90
const SKIP_WINDOWS := ["Game", "ImGui demo"]
# The AI labels' reach (ai_overlay.cpp kLabelRange, mission units = metres).
const AI_LABEL_RANGE := 150.0
const WIREFRAME := 4  # Viewport.DEBUG_DRAW_WIREFRAME

var _failures: Array[String] = []


func run(ctx: ProbeContext) -> ProbeVerdict:
	if not await ctx.wait_for_local_player():
		return ProbeVerdict.failed("no local player (mission load stalled or cancelled)")
	var dev_tools := ctx.dev_tools()
	var sim := ctx.sim()
	var adapter := ctx.adapter()
	var camera := ctx.camera()
	if dev_tools == null or sim == null or adapter == null or camera == null:
		return ProbeVerdict.failed("the shell seams are unavailable")
	if not dev_tools.is_available():
		return ProbeVerdict.failed("the dev tools need the imgui-godot addon and a window")
	var controls: DebugControlTable = adapter.get_debug_controls()
	var tools_were_open := dev_tools.is_open()
	var overlays := dev_tools.overlay_names()
	ctx.defer_restore(func() -> void:
		if not is_instance_valid(dev_tools):
			return
		for name in overlays:
			dev_tools.set_overlay_enabled(name, name == "Entities/Selection")
		for title in dev_tools.window_titles():
			if not title in ["Game", "Stats"]:
				dev_tools.set_window_open(title, false)
		dev_tools.select_entity(-1)
		if controls != null:
			controls.set_value(&"viewport_debug_draw", 0, true)
		dev_tools.set_open(tools_were_open))

	dev_tools.set_open(true)
	await ctx.wait_frames(SETTLE_FRAMES)
	var readout := dev_tools.game_status_text()
	ctx.log("game readout: %s" % readout)
	_check(readout.contains("tick"), "the Game window's readout names the logic tick")

	# Every inspection window, one at a time.
	for title in dev_tools.window_titles():
		if title in SKIP_WINDOWS:
			continue
		_check(dev_tools.set_window_open(title, true), "window '%s' opens by title" % title)
		await ctx.wait_frames(SETTLE_FRAMES)
		await ctx.capture_png("window_%s" % title.to_lower().replace(" ", "_"), ctx.tree.root)
		dev_tools.set_window_open(title, false)
		await ctx.wait_frames(2)

	# The overlays over the nearest on-screen entity (on the Game view's own
	# SubViewport, the image the overlays draw over).
	var in_view := _rows_in_view(sim, camera, camera.get_viewport().get_visible_rect())
	var target: EntityRow = null if in_view.is_empty() else in_view[0]
	if target != null:
		dev_tools.select_entity(target.get_wire_handle())
		ctx.log("selected '%s' (handle %d)" % [target.get_item_name(), target.get_wire_handle()])
	for name in overlays:
		_check(dev_tools.set_overlay_enabled(name, true), "overlay '%s' toggles by name" % name)
	await ctx.wait_frames(OVERLAY_FRAMES)
	# The selection docks the Entities windows beside the Game view, so what
	# is in view is read after the layout settles.
	var brain_in_view := false
	for row in _rows_in_view(sim, camera, camera.get_viewport().get_visible_rect()):
		var near := row.get_world_position().distance_to(camera.global_position) <= AI_LABEL_RANGE
		if row.get_ai_index() >= 0 and near:
			brain_in_view = true
			ctx.log("brain in view: '%s'" % row.get_item_name())
			break
	var draws := {}
	for name in overlays:
		var last: Vector3i = dev_tools.overlay_last_draw(name)
		draws[name] = [last.x, last.y, last.z]
		ctx.log("overlay %s: %d lines, %d labels, %d dropped" % [name, last.x, last.y, last.z])
	var must_draw: Array[String] = ["Collision/Rays"]
	if target != null:
		must_draw.append("Entities/Selection")
	if brain_in_view:
		must_draw.append("AI/Labels")
	for name in must_draw:
		var last: Array = draws.get(name, [-1, -1, -1])
		_check(int(last[0]) + int(last[1]) > 0, "overlay '%s' drew this frame" % name)
	await ctx.capture_png("overlays", ctx.tree.root)
	for name in overlays:
		dev_tools.set_overlay_enabled(name, false)

	# The Wireframe view (the viewport_debug_draw row MCP and the Render
	# window share).
	if controls != null:
		_check(controls.set_value(&"viewport_debug_draw", WIREFRAME, true) == OK,
				"the viewport_debug_draw row takes Wireframe")
		await ctx.wait_frames(SETTLE_FRAMES)
		await ctx.capture_png("wireframe", ctx.tree.root)
		controls.set_value(&"viewport_debug_draw", 0, true)
	ctx.progress({"overlays": draws})
	return _verdict(draws)


## The live presented entities with a def whose origin projects inside the
## viewport, nearest the camera first (the local player left out).
func _rows_in_view(sim: Simulation, camera: Camera3D, rect: Rect2) -> Array[EntityRow]:
	var eye := camera.global_position
	var found: Array = []
	for row_v in sim.entity_directory():
		var row := row_v as EntityRow
		if row == null or row.get_item_name().is_empty() or not row.is_alive():
			continue
		if row.get_net_id() <= 0 and row.get_ai_index() >= 0:
			continue  # the local player (net_id 0 with a brain)
		var point: Vector3 = row.get_world_position() + Vector3(0.0, 1.0, 0.0)
		if camera.is_position_behind(point) or not rect.has_point(camera.unproject_position(point)):
			continue
		found.append([row.get_world_position().distance_squared_to(eye), row])
	found.sort_custom(func(a: Array, b: Array) -> bool: return a[0] < b[0])
	var out: Array[EntityRow] = []
	for entry in found:
		out.append(entry[1])
	return out


func _check(condition: bool, message: String) -> void:
	if not condition:
		_failures.append(message)


func _verdict(draws: Dictionary) -> ProbeVerdict:
	var data := {"failures": _failures.duplicate(), "overlays": draws}
	if _failures.is_empty():
		return ProbeVerdict.passed("every window opened and the overlays drew", data)
	return ProbeVerdict.failed("%d dev-tools check(s) failed" % _failures.size(), data)
