extends GutTest

# OcclusionDebugView: the F3 "Show portal faces" 3D overlay. Drives it with a
# crafted get_occlusion_portal_debug() dictionary (the same shape Simulation
# emits) through the public render seam to pin: outlines + section labels
# draw, plain occluder records stay label-free, per-frame visible flips reuse
# the cached geometry, geometry changes rebuild, and a vanished sim clears
# everything instead of erroring.

const ViewScript := preload("res://game/debug/occlusion_debug_view.gd")


static func _quad_segments(origin: Vector3) -> PackedVector3Array:
	# A unit-quad boundary as 4 (a, b) segment pairs, the layout the sim emits.
	var o := origin
	return PackedVector3Array([
		o, o + Vector3(1, 0, 0),
		o + Vector3(1, 0, 0), o + Vector3(1, 1, 0),
		o + Vector3(1, 1, 0), o + Vector3(0, 1, 0),
		o + Vector3(0, 1, 0), o,
	])


func _record(type: int, section_a: int, pos: Vector3,
		segments: PackedVector3Array) -> OcclusionPortalRecord:
	var record := OcclusionPortalRecord.new()
	record.type = type
	record.section_a = section_a
	record.pos = pos
	record.radius = 0.7
	record.segments = segments
	return record


func _payload(visible := true, origin := Vector3.ZERO) -> OcclusionPortalReport:
	var building := OcclusionPortalBuilding.new()
	building.bms_id = 42
	building.visible = visible
	building.add_record(_record(2, 3, origin + Vector3(0.5, 0.5, 0.0), _quad_segments(origin)))
	building.add_record(_record(0, 0, origin + Vector3(2.5, 0.5, 0.0),
			_quad_segments(origin + Vector3(2.0, 0.0, 0.0))))
	var report := OcclusionPortalReport.new()
	report.add_building(building)
	return report


func _make_view() -> Node3D:
	var view: Node3D = ViewScript.new()
	add_child_autofree(view)
	view.setup(null)
	return view


func test_draws_outlines_and_portal_labels() -> void:
	var view := _make_view()
	view.render_report(_payload())

	var lines := view.get_node("OcclusionPortalLines") as MeshInstance3D
	assert_gt((lines.mesh as ImmediateMesh).get_surface_count(), 0, "portal outlines drawn")
	var labels := view.get_node("OcclusionPortalLabels")
	assert_eq(labels.get_child_count(), 1,
		"the window record gets a label; the plain occluder face stays label-free")
	var label := labels.get_child(0) as Label3D
	assert_eq(label.text, "window s3->ext",
		"the label names the type and the a->b sections (0 = exterior)")
	assert_eq(label.modulate, ViewScript.type_color(2), "label rides the type color")


func test_visible_flip_reuses_cached_geometry() -> void:
	var view := _make_view()
	view.render_report(_payload(true))
	var label_before := (view.get_node("OcclusionPortalLabels")).get_child(0)

	# The batch flicking a building's per-frame visible flag must NOT rebuild
	# the static geometry (the rebuild key excludes it).
	view.render_report(_payload(false))
	var label_after := (view.get_node("OcclusionPortalLabels")).get_child(0)
	assert_eq(label_before.get_instance_id(), label_after.get_instance_id(),
		"a visible-only change keeps the cached labels (no rebuild)")


func test_geometry_change_rebuilds() -> void:
	var view := _make_view()
	view.render_report(_payload())
	var lines := view.get_node("OcclusionPortalLines") as MeshInstance3D
	var initial_bounds := (lines.mesh as ImmediateMesh).get_aabb()

	view.render_report(_payload(true, Vector3(5.0, 0.0, 0.0)))
	var moved_bounds := (lines.mesh as ImmediateMesh).get_aabb()
	assert_ne(moved_bounds, initial_bounds, "record geometry changes redraw the outlines")


func test_missing_sim_clears_instead_of_erroring() -> void:
	var view := _make_view()
	view.render_report(_payload())

	# The sim goes away (mission unloaded): the next frame's real refresh
	# resolves no world/sim and clears everything.
	view.refresh_now()
	var lines := view.get_node("OcclusionPortalLines") as MeshInstance3D
	assert_eq((lines.mesh as ImmediateMesh).get_surface_count(), 0, "outlines cleared")
	assert_eq((view.get_node("OcclusionPortalLabels")).get_child_count(), 0, "labels cleared")
