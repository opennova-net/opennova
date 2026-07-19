extends GutTest

# HitboxDebugView: the F3 Rounds tab's posed collision view. A crafted native
# payload pins the visual language for live, masked, and unresolved person
# sections without needing a mission or NovaSimulation instance.

const ViewScript := preload("res://engine/debug/hitbox_debug_view.gd")


class FakeSim:
	extends Node
	var debug: Dictionary = {}
	var copy_payload_each_read := false
	var read_count := 0
	func get_hitbox_debug() -> Dictionary:
		read_count += 1
		return debug.duplicate(true) if copy_payload_each_read else debug


class FakeWorld:
	extends Node
	var sim: Node = null
	func get_sim():
		return sim


func _make_view(payload: Dictionary, copy_payload_each_read := false) -> Node3D:
	var world: FakeWorld = autofree(FakeWorld.new())
	var sim: FakeSim = autofree(FakeSim.new())
	sim.debug = payload
	sim.copy_payload_each_read = copy_payload_each_read
	world.sim = sim
	var view: Node3D = ViewScript.new()
	add_child_autofree(view)
	view.setup(world)
	return view


func _make_actor_budget_organic_payload() -> Dictionary:
	var organics: Array = []
	for actor_index in range(96):
		for section in range(19):
			var radius := 0.10 + float(section) * 0.01
			organics.append({
				'entity_handle': actor_index + 1,
				'section': section,
				'pos': Vector3(float(actor_index) * 2.0, float(section) * 0.25,
						float(actor_index) * -0.5),
				'radius': radius,
				'authored_radius': radius * 0.75,
				'masked': actor_index == 95 and section == 18,
				'fallback': actor_index == 94 and section == 1,
			})
	return { 'entities': [], 'organics': organics }


func _organic_geometry_resource(view: Node3D) -> Resource:
	var organic_node := view.get_node("HitboxOrganicLines")
	if organic_node is MultiMeshInstance3D:
		return (organic_node as MultiMeshInstance3D).multimesh
	if organic_node is MeshInstance3D:
		return (organic_node as MeshInstance3D).mesh
	return null


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

	var organic_geometry := _organic_geometry_resource(view)
	assert_not_null(organic_geometry,
			"the person sections own a geometry resource")
	if organic_geometry is MultiMesh:
		assert_gt((organic_geometry as MultiMesh).instance_count, 0,
				"the person sections produce visible wire-sphere instances")
	elif organic_geometry is Mesh:
		assert_gt((organic_geometry as Mesh).get_surface_count(), 0,
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


func test_world_labels_use_compact_debug_font_scale() -> void:
	var view := _make_view({
		"entities": [],
		"organics": [
			{ "entity_handle": 7, "section": 14, "pos": Vector3(1, 1, 0),
				"radius": 0.35, "authored_radius": 0.22,
				"masked": false, "fallback": false },
		],
	})
	view.refresh_now()

	var label := view.get_node("HitboxLabel0") as Label3D
	assert_true(label.visible)
	assert_eq(label.font_size, 8, "compact world-debug font")
	assert_eq(label.outline_size, 2,
			"compact labels keep a proportional two-pixel outline")
	assert_lte(float(label.font_size) * label.pixel_size, 0.04,
			"one glyph is at most 0.04 world units")
	assert_lte((float(label.font_size) + 2.0 * float(label.outline_size)) * label.pixel_size,
			0.06, "glyph plus outline stays within the compact world-size budget")


func test_actor_budget_uses_one_packed_colored_organic_multimesh() -> void:
	var payload := _make_actor_budget_organic_payload()
	var view := _make_view(payload, true)
	var organic_node := view.get_node("HitboxOrganicLines")
	assert_true(organic_node is MultiMeshInstance3D,
			"the 96-actor posed-hitbox budget renders through one MultiMesh")
	if not (organic_node is MultiMeshInstance3D):
		return
	view.refresh_now()

	var organic_lines := organic_node as MultiMeshInstance3D
	var batch := organic_lines.multimesh
	assert_not_null(batch, "the organic instance owns a MultiMesh resource")
	if batch == null:
		return
	assert_eq(batch.transform_format, MultiMesh.TRANSFORM_3D)
	assert_true(batch.use_colors, "instance colors carry section roles")
	assert_false(batch.use_custom_data, "the packed row is transform plus color only")
	assert_eq(batch.instance_count, 1824)
	var packed := batch.buffer
	assert_eq(packed.size(), 29184, "1824 instances use sixteen floats each")

	assert_true(batch.mesh is ArrayMesh, "all instances share one unit wire sphere")
	if not (batch.mesh is ArrayMesh):
		return
	var unit_mesh := batch.mesh as ArrayMesh
	assert_eq(unit_mesh.get_surface_count(), 1)
	if unit_mesh.get_surface_count() != 1:
		return
	assert_eq(unit_mesh.surface_get_primitive_type(0), Mesh.PRIMITIVE_LINES)
	var arrays := unit_mesh.surface_get_arrays(0)
	var vertices := arrays[Mesh.ARRAY_VERTEX] as PackedVector3Array
	assert_eq(vertices.size(), 120, "three twenty-segment circles have 120 endpoints")
	var unit_bounds := unit_mesh.get_aabb()
	for axis in range(3):
		assert_almost_eq(unit_bounds.position[axis], -1.0, 0.0001)
		assert_almost_eq(unit_bounds.size[axis], 2.0, 0.0001)

	var head_index := 37 * 19 + 14
	var expected_head_row := PackedFloat32Array([
		0.24, 0.0, 0.0, 74.0,
		0.0, 0.24, 0.0, 3.5,
		0.0, 0.0, 0.24, -18.5,
		1.0, 0.25, 0.85, 1.0,
	])
	var head_offset := head_index * 16
	for component in range(16):
		assert_almost_eq(packed[head_offset + component], expected_head_row[component],
				0.0001, "packed transform/color component %d" % component)
	var masked_color := PackedFloat32Array([0.45, 0.08, 0.08, 0.8])
	var masked_color_offset := 1823 * 16 + 12
	for component in range(4):
		assert_almost_eq(packed[masked_color_offset + component], masked_color[component],
				0.0001, "packed masked color component %d" % component)
	var fallback_color := PackedFloat32Array([1.0, 0.65, 0.15, 0.95])
	var fallback_color_offset := (94 * 19 + 1) * 16 + 12
	for component in range(4):
		assert_almost_eq(packed[fallback_color_offset + component], fallback_color[component],
				0.0001, "packed fallback color component %d" % component)

	var first_multimesh := batch
	var first_unit_mesh := unit_mesh
	var mutation_count := [0]
	batch.changed.connect(func() -> void: mutation_count[0] += 1)
	for refresh_index in range(3):
		view.refresh_now()
	assert_same(organic_lines.multimesh, first_multimesh)
	assert_same(first_multimesh.mesh, first_unit_mesh)
	assert_eq(mutation_count[0], 0,
			"equivalent snapshots perform zero MultiMesh mutations")

	var organics := payload["organics"] as Array
	var head_row := organics[head_index] as Dictionary
	head_row["pos"] = Vector3(75.0, 4.0, -17.5)
	view.refresh_now()
	assert_same(organic_lines.multimesh, first_multimesh,
			"changed poses retain the MultiMesh resource")
	assert_same(first_multimesh.mesh, first_unit_mesh,
			"changed poses retain the shared unit sphere")
	assert_gt(mutation_count[0], 0, "changed poses mutate the packed instance data")
	assert_eq(first_multimesh.instance_count, 1824)
	assert_eq(first_multimesh.buffer.size(), 29184)
	var moved_packed := first_multimesh.buffer
	for radius_offset in [0, 5, 10]:
		assert_almost_eq(moved_packed[head_offset + radius_offset], 0.24, 0.0001)
	var moved_origin := PackedFloat32Array([75.0, 4.0, -17.5])
	var origin_offsets := [3, 7, 11]
	for component in range(3):
		assert_almost_eq(moved_packed[head_offset + origin_offsets[component]],
				moved_origin[component], 0.0001)


func test_refresh_cadence_is_six_hz_at_common_frame_rates() -> void:
	for fps in [60, 144, 240]:
		var world: FakeWorld = autofree(FakeWorld.new())
		var sim: FakeSim = autofree(FakeSim.new())
		sim.debug = { "entities": [], "organics": [] }
		world.sim = sim
		var view: Node3D = ViewScript.new()
		add_child_autofree(view)
		view.setup(world)
		view.set_process(false)
		assert_eq(sim.read_count, 0,
				"setup does not fetch before the first scheduled cadence at %d FPS" % fps)
		for frame_index in range(fps):
			view.advance_refresh(1.0 / float(fps))
		assert_eq(sim.read_count, 6,
				"one simulated second performs six native reads at %d FPS" % fps)


func test_refresh_cadence_discards_missed_intervals_without_frame_bursts() -> void:
	var world: FakeWorld = autofree(FakeWorld.new())
	var sim: FakeSim = autofree(FakeSim.new())
	sim.debug = { "entities": [], "organics": [] }
	world.sim = sim
	var view: Node3D = ViewScript.new()
	add_child_autofree(view)
	view.setup(world)
	view.set_process(false)

	view.advance_refresh(2.0)
	assert_eq(sim.read_count, 1, "a two-second hitch performs at most one native read")
	for zero_frame in range(4):
		view.advance_refresh(0.0)
	assert_eq(sim.read_count, 1, "zero-delta frames do not replay missed intervals")
	for frame_index in range(9):
		view.advance_refresh(1.0 / 60.0)
	assert_eq(sim.read_count, 1,
			"nine fresh 60 FPS frames remain below the next cadence boundary")
	view.advance_refresh(1.0 / 60.0)
	assert_eq(sim.read_count, 2,
			"the tenth fresh 60 FPS frame reaches exactly one new cadence boundary")

	for fps in [4, 2]:
		var low_world: FakeWorld = autofree(FakeWorld.new())
		var low_sim: FakeSim = autofree(FakeSim.new())
		low_sim.debug = { "entities": [], "organics": [] }
		low_world.sim = low_sim
		var low_view: Node3D = ViewScript.new()
		add_child_autofree(low_view)
		low_view.setup(low_world)
		low_view.set_process(false)
		for frame_index in range(fps):
			var before_frame_reads := low_sim.read_count
			low_view.advance_refresh(1.0 / float(fps))
			assert_lte(low_sim.read_count - before_frame_reads, 1,
					"%d FPS never bursts more than one native read in a frame" % fps)
		assert_eq(low_sim.read_count, fps,
				"%d FPS performs one bounded read per available frame" % fps)


func test_cadence_equivalent_payload_does_not_rebuild_posed_geometry() -> void:
	var view := _make_view({
		"entities": [],
		"organics": [
			{ "entity_handle": 7, "section": 14, "pos": Vector3(1, 1, 0),
				"radius": 0.35, "authored_radius": 0.22,
				"masked": false, "fallback": false },
		],
	}, true)
	view.refresh_now()

	var first_geometry := _organic_geometry_resource(view)
	assert_not_null(first_geometry)
	if first_geometry == null:
		return
	var change_count := [0]
	first_geometry.changed.connect(func() -> void: change_count[0] += 1)
	var first_label := view.get_node("HitboxLabel0") as Label3D
	var visibility_change_count := [0]
	first_label.visibility_changed.connect(
			func() -> void: visibility_change_count[0] += 1)

	# NovaSimulation publishes fresh Dictionary/Array containers at the debug
	# cadence even when the posed collision values are unchanged. Equivalent
	# snapshots must not force the same wire geometry through RenderingServer.
	for refresh_index in range(8):
		view.refresh_now()

	assert_same(_organic_geometry_resource(view), first_geometry,
			"equivalent snapshots retain the posed-hitbox mesh resource")
	assert_eq(change_count[0], 0,
			"equivalent snapshots perform zero posed-hitbox mesh mutations")
	assert_eq(visibility_change_count[0], 0,
			"equivalent snapshots perform zero label visibility mutations")


func test_changed_organic_draw_inputs_invalidate_geometry_and_labels_update() -> void:
	var payload := {
		"entities": [],
		"organics": [
			{ "entity_handle": 7, "section": 5, "pos": Vector3(1, 1, 0),
				"radius": 0.35, "authored_radius": 0.22,
				"masked": false, "fallback": false },
		],
	}
	var organic := payload["organics"][0] as Dictionary
	var view := _make_view(payload, true)
	view.refresh_now()

	var organic_geometry := _organic_geometry_resource(view)
	assert_not_null(organic_geometry)
	if organic_geometry == null:
		return
	var change_count := [0]
	organic_geometry.changed.connect(func() -> void: change_count[0] += 1)
	var label := view.get_node("HitboxLabel0") as Label3D

	var before_count: int = int(change_count[0])
	organic["pos"] = Vector3(3, 1, 0)
	view.refresh_now()
	assert_gt(change_count[0], before_count, "position invalidates posed geometry")
	assert_eq(label.position.x, 3.0, "position updates the matching label")

	before_count = change_count[0]
	organic["radius"] = 0.55
	view.refresh_now()
	assert_gt(change_count[0], before_count, "radius invalidates posed geometry")
	assert_string_contains(label.text, "r 0.55")

	before_count = change_count[0]
	organic["section"] = 14
	view.refresh_now()
	assert_gt(change_count[0], before_count, "section color invalidates posed geometry")
	assert_string_contains(label.text, "HEAD")

	before_count = change_count[0]
	organic["masked"] = true
	view.refresh_now()
	assert_gt(change_count[0], before_count, "masked color invalidates posed geometry")
	assert_string_contains(label.text, "MASKED")

	before_count = change_count[0]
	organic["masked"] = false
	view.refresh_now()
	assert_gt(change_count[0], before_count, "clearing masked invalidates posed geometry")

	before_count = change_count[0]
	organic["fallback"] = true
	view.refresh_now()
	assert_gt(change_count[0], before_count, "fallback color invalidates posed geometry")
	assert_string_contains(label.text, "FALLBACK")

	before_count = change_count[0]
	organic["fallback"] = false
	view.refresh_now()
	assert_gt(change_count[0], before_count, "clearing fallback invalidates posed geometry")
	before_count = change_count[0]
	organic["authored_radius"] = 0.44
	view.refresh_now()
	assert_eq(change_count[0], before_count,
			"label-only authored radius does not invalidate posed geometry")
	assert_string_contains(label.text, "authored 0.44")


func test_static_hit_mesh_refreshes_when_only_transformed_triangles_change() -> void:
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
	var payload := { "entities": [entity], "organics": [] }
	var view := _make_view(payload)
	view.refresh_now()
	var lines := view.get_node("HitboxLines") as MeshInstance3D
	var before_center := (lines.mesh as ImmediateMesh).get_aabb().get_center()

	# PANM/current section matrices change world-space triangles without moving
	# the entity itself. The static-mesh signature must still invalidate.
	entity["tris"] = PackedVector3Array([
		Vector3(10, 0, 0), Vector3(11, 0, 0), Vector3(10, 1, 0),
	])
	view.refresh_now()
	var after_center := (lines.mesh as ImmediateMesh).get_aabb().get_center()
	assert_gt(after_center.x - before_center.x, 9.0,
			"same-origin animated collision triangles rebuild the F3 mesh")
