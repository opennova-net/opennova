extends GutTest

# HitboxDebugView: the F3 Rounds tab's posed collision view. A crafted native
# payload pins the visual language for live, masked, and unresolved person
# sections without needing a mission or NovaSimulation instance.

const ViewScript := preload("res://engine/debug/hitbox_debug_view.gd")


class FakeSim:
	extends Node
	var debug: Dictionary = {}
	func get_hitbox_debug() -> Dictionary:
		return debug


class FakeWorld:
	extends Node
	var sim: Node = null
	func get_sim():
		return sim


func _make_view(payload: Dictionary) -> Node3D:
	var world: FakeWorld = autofree(FakeWorld.new())
	var sim: FakeSim = autofree(FakeSim.new())
	sim.debug = payload
	world.sim = sim
	var view: Node3D = ViewScript.new()
	add_child_autofree(view)
	view.setup(world)
	return view


func test_person_section_colors_distinguish_retail_roles() -> void:
	assert_eq(ViewScript.organic_section_color(14, false, false),
			ViewScript.ORGANIC_HEAD_COLOR, "head section 14 is unmistakable")
	assert_eq(ViewScript.organic_section_color(13, false, false),
			ViewScript.ORGANIC_HEAD_COLOR, "head section 13 shares the x3 zone")
	assert_eq(ViewScript.organic_section_color(4, false, false),
			ViewScript.ORGANIC_HEAVY_COLOR, "sections 0-4 are x1.25, not head")
	assert_eq(ViewScript.organic_section_color(10, false, false),
			ViewScript.ORGANIC_LIMB_COLOR, "sections 9-12 share the x0.5 zone")
	assert_eq(ViewScript.organic_section_color(15, false, false),
			ViewScript.ORGANIC_LIMB_COLOR, "limb section 15 is grouped with limbs")
	assert_eq(ViewScript.organic_section_color(16, false, false),
			ViewScript.ORGANIC_LIMB_COLOR, "limb section 16 is grouped with limbs")
	assert_eq(ViewScript.organic_section_color(4, true, false),
			ViewScript.ORGANIC_MASKED_COLOR, "masked wins over the live section color")
	assert_eq(ViewScript.organic_section_color(1, false, true),
			ViewScript.ORGANIC_FALLBACK_COLOR, "unresolved torso fallback is amber")


func test_draws_and_labels_posed_masked_and_fallback_sections() -> void:
	var view := _make_view({
		"entities": [],
		"organics": [
			{ "entity_handle": 7, "section": 14, "pos": Vector3(1, 1, 0),
				"radius": 0.35, "authored_radius": 0.22,
				"masked": false, "fallback": false },
			{ "entity_handle": 7, "section": 15, "pos": Vector3(2, 1, 0),
				"radius": 0.25, "authored_radius": 0.20,
				"masked": true, "fallback": false },
			{ "entity_handle": 8, "section": 1, "pos": Vector3(3, 1, 0),
				"radius": 0.60, "authored_radius": 0.60,
				"masked": false, "fallback": true },
		],
	})
	view.refresh_now()

	var organic_lines := view.get_node("HitboxOrganicLines") as MeshInstance3D
	assert_gt((organic_lines.mesh as ImmediateMesh).get_surface_count(), 0,
			"the person sections produce visible wire spheres")
	var label_text := ""
	for i in range(ViewScript.LABEL_NEAREST):
		var label := view.get_node("HitboxLabel%d" % i) as Label3D
		if label.visible:
			label_text += label.text + "\n"
	assert_string_contains(label_text, "bone 14")
	assert_string_contains(label_text, "HEAD")
	assert_string_contains(label_text, "damage x3.00")
	assert_string_contains(label_text, "authored 0.22")
	assert_string_contains(label_text, "bone 15")
	assert_string_contains(label_text, "LIMB")
	assert_string_contains(label_text, "MASKED")
	assert_string_contains(label_text, "FALLBACK")


func test_static_hit_mesh_refreshes_when_only_transformed_triangles_change() -> void:
	var view := _make_view({ "entities": [], "organics": [] })
	var entity := {
		"entity_handle": 7,
		"pos": Vector3.ZERO,
		"husk": false,
		"tris": PackedVector3Array([
			Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 1, 0),
		]),
		"materials": PackedByteArray([0]),
		"flags": PackedInt32Array([0]),
		"bound_radius": 0.0,
		"has_faces": true,
		"face_total": 1,
	}
	view._update([entity], [])
	var lines := view.get_node("HitboxLines") as MeshInstance3D
	var before_center := (lines.mesh as ImmediateMesh).get_aabb().get_center()

	# PANM/current section matrices change world-space triangles without moving
	# the entity itself. The static-mesh signature must still invalidate.
	entity["tris"] = PackedVector3Array([
		Vector3(10, 0, 0), Vector3(11, 0, 0), Vector3(10, 1, 0),
	])
	view._update([entity], [])
	var after_center := (lines.mesh as ImmediateMesh).get_aabb().get_center()
	assert_gt(after_center.x - before_center.x, 9.0,
			"same-origin animated collision triangles rebuild the F3 mesh")
