extends GutTest

# The weapon Inset pass's own draws. Retail runs the scene core again for the
# Inset camera (engine renderer/scene_overlay.h kInsetOverlayOrder carries the
# witness), so every draw it makes selects its lights there, and a part model
# drawn inside a body's submit follows the body into that view. A part the
# Inset draws through a twin while the main view hides it has no main-pass
# selection to mirror: the Inset pass writes the twin's own.

const MODEL_GRAPHIC := "armory"
const MODEL_3DI := "res://../fixtures/threedi/synth/armory.3di"


func _fixture_model(parent: Node) -> ObjectModel:
	var placer := MissionObjectPlacer.new()
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(MODEL_3DI)), OK)
	placer.register_object_data(MODEL_GRAPHIC, data)
	return placer.build_model_from_graphic(MODEL_GRAPHIC, "", parent, "")


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


func _part_surface(model: ObjectModel, robj_index: int) -> GeometryInstance3D:
	var part := model.get_render_part_nodes().get(robj_index) as Node3D
	if part == null:
		return null
	for child in part.get_children():
		if child is GeometryInstance3D:
			return child as GeometryInstance3D
	return null


func test_a_twin_the_main_view_hides_takes_its_own_selection() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var model := _fixture_model(container)
	assert_not_null(model)
	if model == null:
		return
	var keys: Array = model.get_render_part_nodes().keys()
	keys.sort()
	assert_gt(keys.size(), 1)
	if keys.size() < 2:
		return
	var hidden := int(keys[keys.size() - 1])
	var main_camera := _view_camera(Vector2i(640, 480), 90.0)
	var inset_camera := _view_camera(Vector2i(256, 256), 10.0)
	# The main collect draws every section but one; the Inset's draws that one.
	model.set_occlusion_section_mask(0x7FFFFFFF & ~(1 << hidden), 0)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, inset_camera, 256.0)
	model.set_inset_occlusion_section_mask(1 << hidden, 0)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, inset_camera, 256.0)
	assert_true(model.is_view_split())
	assert_true(model.is_inset_view_drawn())
	assert_eq(model.get_inset_view_section_mask(), 1 << hidden)
	var twins := model.get_view_twin_count()
	assert_gt(twins, 0)
	var scene := LightScene.new()
	assert_gt(scene.spawn_model_light(ModelLightSpawn.make(Vector3(0.0, 1.0, 0.0), 8.0)), 0)
	var models: Array[Node3D] = [model]
	var owners := PackedInt64Array([model.get_instance_id()])
	# The main pass: a building's rows over the ROBJs its node draws.
	scene.render_model_frame(models, owners, PackedInt64Array([0]), PackedInt32Array([0]),
			PackedByteArray([1]), Vector3.ONE, 0, null)
	var hidden_surface := _part_surface(model, hidden)
	assert_not_null(hidden_surface)
	if hidden_surface != null:
		assert_null(hidden_surface.get_instance_shader_parameter("u_point_light_count"),
				"the main view never selected for the part it hides")
	ObjectModel.sync_view_twins()
	for i in range(twins):
		assert_null(model.get_view_twin_shader_parameter(i, "u_point_light_count"),
				"a twin mirrors the node until the Inset pass selects")
	assert_eq(scene.render_inset_model_frame(models, owners, PackedInt64Array([0]),
			PackedInt32Array([0]), PackedByteArray([1]), Vector3.ONE, 0, null,
			PackedVector3Array(), PackedInt32Array()), 1)
	# The node's mirror runs after the pass and leaves the twins' own.
	ObjectModel.sync_view_twins()
	for i in range(twins):
		assert_eq(float(model.get_view_twin_shader_parameter(i, "u_point_light_count")), 1.0,
				"the Inset pass lit the twin")
	if hidden_surface != null:
		assert_null(hidden_surface.get_instance_shader_parameter("u_point_light_count"),
				"and left the node alone")
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, null, 0.0)
	assert_eq(model.get_view_twin_count(), 0)


func test_a_part_under_a_body_the_main_view_hides_follows_the_body() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var body := _fixture_model(container)
	assert_not_null(body)
	if body == null:
		return
	var part := _fixture_model(body)
	assert_not_null(part)
	if part == null:
		return
	var main_camera := _view_camera(Vector2i(640, 480), 90.0)
	var inset_camera := _view_camera(Vector2i(256, 256), 10.0)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, inset_camera, 256.0)
	# The main collect hides the body (its node, so the part under it too); the
	# Inset collect draws it.
	body.set_occlusion_hidden(true)
	body.set_inset_occlusion_hidden(false)
	part.set_inset_occlusion_hidden(false)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, inset_camera, 256.0)
	assert_false(body.is_visible_in_tree())
	assert_true(body.is_inset_view_drawn())
	assert_gt(body.get_view_twin_count(), 0)
	assert_true(part.is_view_split(), "the part draws in the Inset with the body")
	assert_true(part.is_inset_view_drawn())
	assert_gt(part.get_view_twin_count(), 0)
	# The Inset hides the body too: the part goes with it.
	body.set_inset_occlusion_hidden(true)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, inset_camera, 256.0)
	assert_false(body.is_inset_view_drawn())
	assert_false(part.is_inset_view_drawn())
	assert_eq(part.get_view_twin_count(), 0)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, null, 0.0)
