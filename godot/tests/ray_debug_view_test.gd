extends GutTest

# RayDebugView renders Simulation.get_ray_debug()'s stride-12 float channel:
# one segment per recorded ray (already mask/TTL-filtered by the binding),
# rebuilt only when the report tick moves, cleared on an empty report.

const ViewScript := preload("res://game/debug/ray_debug_view.gd")


func _make_view() -> Node3D:
	var view: Node3D = ViewScript.new()
	add_child_autofree(view)
	view.setup(null)  # report-driven: no world; _build_view still runs
	view.set_process(false)  # keep the simless _process from clearing reports
	return view


func _report(tick: int, events: PackedFloat32Array) -> RayDebugReport:
	var report := RayDebugReport.new()
	report.tick = tick
	report.events = events
	report.mask = 0x7FFF
	report.ttl = 93
	report.recording = true
	return report


func _event(category: int, age: float, result: int,
		start: Vector3, end: Vector3, hit: Vector3) -> PackedFloat32Array:
	return PackedFloat32Array([category, age, result,
			start.x, start.y, start.z, end.x, end.y, end.z, hit.x, hit.y, hit.z])


func test_events_draw_and_count() -> void:
	var view := _make_view()
	var events := _event(1, 0.0, 1,
			Vector3.ZERO, Vector3(10, 0, 0), Vector3(5, 0, 0))
	events.append_array(_event(4, 30.0, 2,
			Vector3(1, 1, 1), Vector3(1, 1, 20), Vector3(1, 1, 20)))
	view.render_report(_report(100, events))
	assert_eq(view.get_debug_drawable_count(), 2, "two stride-12 events drawn")
	assert_eq((view.get_node("RayDebugLines") as MeshInstance3D)
			.mesh.get_surface_count(), 1, "the segments emit one line surface")


func test_unchanged_tick_skips_the_rebuild() -> void:
	var view := _make_view()
	var events := _event(2, 1.0, 0, Vector3.ZERO, Vector3.ONE, Vector3.ONE)
	view.render_report(_report(7, events))
	assert_eq(view.get_debug_drawable_count(), 1)
	# The same tick + payload size is the change signature: a second identical
	# report leaves the mesh alone (the count sticks, no clear/re-emit churn).
	view.render_report(_report(7, events))
	assert_eq(view.get_debug_drawable_count(), 1)


func test_empty_report_clears_the_mesh() -> void:
	var view := _make_view()
	view.render_report(_report(9, _event(14, 0.0, 1,
			Vector3.ZERO, Vector3(2, 0, 0), Vector3(1, 0, 0))))
	assert_eq(view.get_debug_drawable_count(), 1)
	view.render_report(_report(10, PackedFloat32Array()))
	assert_eq(view.get_debug_drawable_count(), 0, "an empty report clears")
	assert_eq((view.get_node("RayDebugLines") as MeshInstance3D)
			.mesh.get_surface_count(), 0, "no surfaces after the clear")


func test_out_of_range_category_still_draws() -> void:
	var view := _make_view()
	# A category the color table doesn't know (future enum growth) must not
	# crash the view; it draws in the fallback color.
	view.render_report(_report(11, _event(99, 0.0, 0,
			Vector3.ZERO, Vector3(3, 0, 0), Vector3(3, 0, 0))))
	assert_eq(view.get_debug_drawable_count(), 1)
