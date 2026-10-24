extends GutTest

# The water mirrors' own collects. Retail's reflected subscene draws the lists
# its own collect builds (engine inmatch/occlusion_collect.h MirrorViewLists),
# so a model whose mirror verdict differs from what its node shows that mirror
# leaves the mirror by its layers, and a twin draws the mirror's on the bit only
# that mirror camera admits (render/visual_layers.h).

const MODEL_GRAPHIC := "armory"
const MODEL_3DI := "res://../fixtures/threedi/synth/armory.3di"
const WORLD := 1 << 0
const WORLD_NO_MIRROR := 1 << 16
const MIRROR_VIEW := 1 << 24
const INSET_MIRROR_VIEW := 1 << 25
const SCENE_VIEW_NO_REFLECTION := 1 << 26


func after_each() -> void:
	ObjectModel.set_mirror_view_state(0, false, false)
	ObjectModel.set_mirror_view_state(1, false, false)


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


func _surface_layers(model: ObjectModel) -> int:
	for part in model.get_render_part_nodes().values():
		for child in (part as Node3D).get_children():
			if child is GeometryInstance3D:
				return (child as GeometryInstance3D).layers
	return -1


func test_a_node_the_main_view_hides_draws_in_the_mirror_on_a_twin() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var model := _fixture_model(container)
	assert_not_null(model)
	if model == null:
		return
	var main_camera := _view_camera(Vector2i(640, 480), 90.0)
	ObjectModel.set_mirror_view_state(0, true, false)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, null, 0.0)
	# The main collect hides it (outside the main view's frustum); the mirror's
	# square view collects it.
	model.set_occlusion_hidden(true)
	model.set_mirror_occlusion_hidden(0, false)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, null, 0.0)
	assert_false(model.is_visible_in_tree())
	assert_true(model.is_mirror_view_split(0))
	assert_gt(model.get_mirror_view_twin_count(0), 0, "the mirror draws it on a twin")
	assert_eq(model.get_mirror_view_twin_count(1), 0, "the Inset's mirror gave no verdict")
	# The mirror stops rendering: its verdicts release and the twins go.
	ObjectModel.set_mirror_view_state(0, false, false)
	assert_false(model.is_mirror_view_split(0))
	assert_eq(model.get_mirror_view_twin_count(0), 0)


func test_a_node_the_mirror_does_not_collect_leaves_it() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var model := _fixture_model(container)
	assert_not_null(model)
	if model == null:
		return
	var main_camera := _view_camera(Vector2i(640, 480), 90.0)
	# Below the water the mirror admits the node's no-mirror world layer, so the
	# node shows there unless the mirror's collect drops it.
	ObjectModel.set_mirror_view_state(0, true, true)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, null, 0.0)
	assert_ne(_surface_layers(model) & WORLD_NO_MIRROR, 0, "the fixture is a no-mirror model")
	model.set_mirror_occlusion_hidden(0, true)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, null, 0.0)
	assert_true(model.is_visible_in_tree())
	assert_true(model.is_mirror_view_split(0))
	assert_eq(model.get_mirror_view_twin_count(0), 0)
	var layers := _surface_layers(model)
	assert_eq(layers & (WORLD | WORLD_NO_MIRROR | MIRROR_VIEW), 0, "the node leaves the mirror")
	assert_ne(layers & SCENE_VIEW_NO_REFLECTION, 0, "and still draws for the scene views")
	assert_eq(layers & INSET_MIRROR_VIEW, 0, "the Inset's mirror is above the water")
	# The verdicts agree again: back on the world layer.
	model.set_mirror_occlusion_hidden(0, false)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, null, 0.0)
	assert_false(model.is_mirror_view_split(0))
	assert_ne(_surface_layers(model) & WORLD_NO_MIRROR, 0)
