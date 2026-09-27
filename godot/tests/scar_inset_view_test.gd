extends GutTest

# The scar pass per view: retail's weapon Inset scene core compiles and draws
# the scar caches inside its own collect (engine world/occlusion.h
# OcclusionView carries the witness), so while the Inset renders the main
# list's world mesh draws for the main view only, the Inset list's on the
# Inset bit, and an entity ring of an owner whose views differ takes an Inset
# twin the owner poses (ObjectModel::attach_inset_section_twin).

const MODEL_GRAPHIC := "armory"
const MODEL_3DI := "res://../fixtures/threedi/synth/armory.3di"
const OWNER_A := 0x1004
const WORLD := 1 << 0
const MAIN_VIEW := 1 << 20
const INSET_VIEW := 1 << 21


func _fixture_model(parent: Node) -> ObjectModel:
	var placer := MissionObjectPlacer.new()
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(MODEL_3DI)), OK)
	placer.register_object_data(MODEL_GRAPHIC, data)
	return placer.build_model_from_graphic(MODEL_GRAPHIC, "", parent, "")


func _quad(draw: ScarDrawList, centre: Vector3) -> void:
	var corners := [
		centre + Vector3(-0.25, 0, -0.25), centre + Vector3(0.25, 0, -0.25),
		centre + Vector3(-0.25, 0, 0.25), centre + Vector3(0.25, 0, 0.25),
	]
	var vertices := draw.vertices
	var uvs := draw.uvs
	var colors := draw.colors
	for k in [0, 1, 2, 1, 3, 2]:
		vertices.append(corners[k])
		uvs.append(Vector2.ZERO)
		colors.append(Color(0.5, 0.5, 0.5, 1.0))
	draw.vertices = vertices
	draw.uvs = uvs
	draw.colors = colors


static func _append_i32(values: PackedInt32Array, value: int) -> PackedInt32Array:
	values.append(value)
	return values


func _batch(draw: ScarDrawList, owner: int, section: int, entity_local: bool, quads: int) -> void:
	var first := draw.vertices.size()
	for q in range(quads):
		_quad(draw, Vector3(q, 0, 0))
	draw.batch_owner = _append_i32(draw.batch_owner, owner)
	draw.batch_texture = _append_i32(draw.batch_texture, 0)
	draw.batch_section = _append_i32(draw.batch_section, section)
	draw.batch_flags = _append_i32(draw.batch_flags,
			ScarDrawList.FLAG_ENTITY_LOCAL if entity_local else 0)
	draw.batch_first = _append_i32(draw.batch_first, first)
	draw.batch_count = _append_i32(draw.batch_count, quads * 6)
	draw.batch_bms_id = _append_i32(draw.batch_bms_id, 0)
	var origins := draw.batch_spawn_origin
	origins.append(Simulation.SPAWN_ORIGIN_NONE)
	draw.batch_spawn_origin = origins


func _draw_list() -> ScarDrawList:
	var draw := ScarDrawList.new()
	var names := PackedStringArray()
	names.resize(32)
	draw.strip_names = names
	var words := PackedInt32Array()
	words.resize(32)
	words.fill(0x120651)
	draw.strip_mode_words = words
	return draw


func _view_camera(size: Vector2i, fov: float) -> Camera3D:
	var view := SubViewport.new()
	view.size = size
	add_child_autofree(view)
	var camera := Camera3D.new()
	camera.keep_aspect = Camera3D.KEEP_WIDTH
	camera.fov = fov
	view.add_child(camera)
	camera.global_transform = Transform3D(Basis.IDENTITY, Vector3(0, 0, 60))
	return camera


func test_world_batches_draw_per_view_while_the_inset_renders() -> void:
	var presenter := ScarPresenter.new()
	add_child_autofree(presenter)
	var main := _draw_list()
	_batch(main, 0xFFFF, 0, false, 2)
	presenter.present(main, {})
	var world := presenter.get_node_or_null("ScarWorld") as MeshInstance3D
	assert_not_null(world)
	if world == null:
		return
	assert_eq(world.layers, WORLD, "no Inset: the one world mesh on the world layer")
	var inset := _draw_list()
	_batch(inset, 0xFFFF, 0, false, 1)
	presenter.set_inset_view(true, Vector3.ZERO)
	presenter.present_inset(inset, {})
	assert_eq(world.layers, MAIN_VIEW, "the main list draws for the main view alone")
	var inset_world := presenter.get_node_or_null("ScarWorldInset") as MeshInstance3D
	assert_not_null(inset_world)
	if inset_world != null:
		assert_eq(inset_world.layers, INSET_VIEW)
		assert_true(inset_world.visible)
		assert_eq(inset_world.mesh.surface_get_array_len(0), 6, "the Inset list's own batch")
	assert_eq(presenter.get_inset_world_surface_count(), 1)
	presenter.set_inset_view(false, Vector3.ZERO)
	assert_eq(world.layers, WORLD, "the Inset closes: the world layer again")
	if inset_world != null:
		assert_false(inset_world.visible)


func test_a_split_owners_entity_ring_takes_an_inset_twin() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var model := _fixture_model(container)
	assert_not_null(model)
	if model == null:
		return
	var parts: Dictionary = model.get_render_part_nodes()
	assert_gt(parts.size(), 0)
	if parts.is_empty():
		return
	var section := int(parts.keys()[0])
	var main_camera := _view_camera(Vector2i(640, 480), 90.0)
	var inset_camera := _view_camera(Vector2i(256, 256), 10.0)
	# The Inset view opens and its collect draws only section 0 of the owner
	# where the main view draws every section: the views differ.
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, inset_camera, 256.0)
	model.set_inset_occlusion_section_mask(1 << section, 0)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, inset_camera, 256.0)
	assert_true(model.is_view_split())
	var presenter := ScarPresenter.new()
	add_child_autofree(presenter)
	var main := _draw_list()
	_batch(main, OWNER_A, section, true, 1)
	presenter.present(main, {OWNER_A: model})
	var ring: MeshInstance3D = null
	var part: Node3D = parts[section]
	for child in part.get_children():
		if String(child.name).begins_with("Scars_"):
			ring = child
	assert_not_null(ring, "the main list's ring rides the section node")
	if ring != null:
		# The armory is a no-mirror model: its node takes MAIN_VIEW_NO_MIRROR.
		assert_eq(ring.layers & (WORLD | (1 << 16)), 0,
				"a split owner's node ring leaves the world layers")
		assert_ne(ring.layers & (MAIN_VIEW | (1 << 22)), 0, "and draws for the main view")
	presenter.set_inset_view(true, Vector3.ZERO)
	presenter.present_inset(main, {OWNER_A: model})
	assert_eq(presenter.get_inset_entity_twin_count(), 1, "the Inset draws it on a twin")
	# The owner's views converge: no twin.
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, null, 0.0)
	assert_false(model.is_view_split())
	presenter.present_inset(main, {OWNER_A: model})
	assert_eq(presenter.get_inset_entity_twin_count(), 0)
	presenter.set_inset_view(false, Vector3.ZERO)
