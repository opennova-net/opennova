extends GutTest

# OcclusionDebugView: the F3 "Show portal faces" 3D overlay. Drives it with a
# duck-typed world/sim pair feeding a crafted get_occlusion_portal_debug()
# dictionary (the same shape NovaSimulation emits) through the public refresh
# seam to pin: outlines + section labels draw, plain occluder records stay
# label-free, per-frame visible flips reuse the cached geometry, geometry
# changes rebuild, and a vanished sim clears everything instead of erroring.

const ViewScript := preload("res://engine/debug/occlusion_debug_view.gd")


# Node-based doubles: the view's setup takes the owner's world NODE (a GameWorld
# in production) and duck-types get_sim()/get_occlusion_portal_debug() off it.
class FakeSim:
	extends Node
	var debug: Dictionary = {}
	func get_occlusion_portal_debug(_anchor: Vector3, _range_units: float) -> Dictionary:
		return debug


class FakeWorld:
	extends Node
	var sim: Node = null
	func get_sim():
		return sim


static func _quad_segments(origin: Vector3) -> PackedVector3Array:
	# A unit-quad boundary as 4 (a, b) segment pairs, the layout the sim emits.
	var o := origin
	return PackedVector3Array([
		o, o + Vector3(1, 0, 0),
		o + Vector3(1, 0, 0), o + Vector3(1, 1, 0),
		o + Vector3(1, 1, 0), o + Vector3(0, 1, 0),
		o + Vector3(0, 1, 0), o,
	])


func _payload(visible := true, origin := Vector3.ZERO) -> Dictionary:
	return {
		"buildings": [{
			"bms_id": 42,
			"pos": Vector3.ZERO,
			"visible": visible,
			"records": [
				{ "type": 2, "section_a": 3, "section_b": 0,
					"pos": origin + Vector3(0.5, 0.5, 0.0), "radius": 0.7, "glow": 0.0,
					"segments": _quad_segments(origin) },
				{ "type": 0, "section_a": 0, "section_b": 0,
					"pos": origin + Vector3(2.5, 0.5, 0.0), "radius": 0.7, "glow": 0.0,
					"segments": _quad_segments(origin + Vector3(2.0, 0.0, 0.0)) },
			],
		}],
	}


func _make_world(payload: Dictionary) -> Node:
	var world: FakeWorld = autofree(FakeWorld.new())
	var sim: FakeSim = autofree(FakeSim.new())
	sim.debug = payload
	world.sim = sim
	return world


func _make_view(world: Node) -> Node3D:
	var view: Node3D = ViewScript.new()
	add_child_autofree(view)
	view.setup(world)
	return view


func test_draws_outlines_and_portal_labels() -> void:
	var world := _make_world(_payload())
	var view := _make_view(world)
	view.refresh_now()

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
	var world := _make_world(_payload(true))
	var view := _make_view(world)
	view.refresh_now()
	var label_before := (view.get_node("OcclusionPortalLabels")).get_child(0)

	# The batch flicking a building's per-frame visible flag must NOT rebuild
	# the static geometry (the rebuild key excludes it).
	world.sim.debug = _payload(false)
	view.refresh_now()
	var label_after := (view.get_node("OcclusionPortalLabels")).get_child(0)
	assert_eq(label_before.get_instance_id(), label_after.get_instance_id(),
		"a visible-only change keeps the cached labels (no rebuild)")


func test_geometry_change_rebuilds() -> void:
	var world := _make_world(_payload())
	var view := _make_view(world)
	view.refresh_now()
	var lines := view.get_node("OcclusionPortalLines") as MeshInstance3D
	var initial_bounds := (lines.mesh as ImmediateMesh).get_aabb()

	world.sim.debug = _payload(true, Vector3(5.0, 0.0, 0.0))
	view.refresh_now()
	var moved_bounds := (lines.mesh as ImmediateMesh).get_aabb()
	assert_ne(moved_bounds, initial_bounds, "record geometry changes redraw the outlines")


func test_missing_sim_clears_instead_of_erroring() -> void:
	var world := _make_world(_payload())
	var view := _make_view(world)
	view.refresh_now()

	# The sim goes away (mission unloaded): the next frame clears everything.
	world.sim = null
	view.refresh_now()
	var lines := view.get_node("OcclusionPortalLines") as MeshInstance3D
	assert_eq((lines.mesh as ImmediateMesh).get_surface_count(), 0, "outlines cleared")
	assert_eq((view.get_node("OcclusionPortalLabels")).get_child_count(), 0, "labels cleared")
