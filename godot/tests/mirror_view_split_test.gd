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
const MAIN_VIEW_NO_MIRROR := 1 << 22
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


func _all_layers(model: ObjectModel) -> int:
	var layers := 0
	for node in model.find_children("*", "MeshInstance3D", true, false):
		layers |= (node as MeshInstance3D).layers
	return layers


# Above the water both mirrors keep the flag-0x400 population alone (vehicles by
# item type, records authored Reflective): a model that carries the flag is on
# both mirror cameras, a person's model (no person carries it) on neither; below
# the water the collect runs unfiltered and both are on.
# [orig: Terrain_CollectVisibleEntitiesForReflection @0x5c90a0 - the filter word
# @0x5c9109..0x5c911f; Entity_InitFromModel @0x40e20a]
func test_above_the_water_the_mirrors_draw_the_flagged_model_alone() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var person := _fixture_model(container)
	assert_not_null(person)
	var vehicle := ObjectModel.new()
	vehicle.set_authored_lod_enabled(true)
	vehicle.set_mirror_reflected(true)
	container.add_child(vehicle)
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(MODEL_3DI)), OK)
	vehicle.set_object_data(data)
	if person == null:
		return
	assert_ne(_all_layers(vehicle), 0, "the flagged model carries visual instances")
	var mirrors: Array[int] = [Water.REFLECTION_CULL_MASK | MIRROR_VIEW,
			Water.REFLECTION_CULL_MASK | INSET_MIRROR_VIEW]
	for m in mirrors.size():
		var above := mirrors[m]
		var below := above | WORLD_NO_MIRROR | MAIN_VIEW_NO_MIRROR
		assert_eq(_all_layers(person) & above, 0,
				"mirror %d: a person's model is off it above the water" % m)
		assert_ne(_all_layers(vehicle) & above, 0,
				"mirror %d: the flagged model is on it above the water" % m)
		assert_ne(_all_layers(person) & below, 0,
				"mirror %d: below the water the person's model is on it" % m)
		assert_ne(_all_layers(vehicle) & below, 0,
				"mirror %d: and the flagged model stays on it" % m)


# A person's model in every presentation state: the world entity (a remote
# person, an AI soldier, a corpse), the drawn local person (the third-person
# body, its head and gun, its overlays, the first-person canopy, whose submit
# comes ahead of the camera-tracked gate), the hidden first-person body and the
# view model. Above the water none is on either mirror camera (no person carries
# the Reflective bit); below the water the collect runs unfiltered, so the two
# drawn states are on both and the two the camera never draws are on neither.
# [orig: Terrain_CollectVisibleEntitiesForReflection @0x5c90a0 - the filter word
# @0x5c9109..0x5c911f; BoneCallback_org0_World @0x4e3940 - the canopy submit
# @0x4e3aa5, the camera-tracked gate @0x4e3ab3..0x4e3aca]
func test_a_person_in_every_presentation_state_is_off_the_mirrors_above_the_water() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var person := _fixture_model(container)
	assert_not_null(person)
	if person == null:
		return
	var mirrors: Array[int] = [Water.REFLECTION_CULL_MASK | MIRROR_VIEW,
			Water.REFLECTION_CULL_MASK | INSET_MIRROR_VIEW]
	var states := {
		ObjectModel.PRESENTATION_LAYER_WORLD: true,
		ObjectModel.PRESENTATION_LAYER_LOCAL_BODY: true,
		ObjectModel.PRESENTATION_LAYER_LOCAL_BODY_HIDDEN: false,
		ObjectModel.PRESENTATION_LAYER_VIEWMODEL: false,
	}
	for state in states.keys():
		person.set_presentation_layer(state)
		var layers := _all_layers(person)
		assert_ne(layers, 0, "state %d: the model carries visual instances" % state)
		for m in mirrors.size():
			var above := mirrors[m]
			var below := above | WORLD_NO_MIRROR | MAIN_VIEW_NO_MIRROR
			assert_eq(layers & above, 0,
					"state %d: mirror %d leaves the person above the water" % [state, m])
			if bool(states[state]):
				assert_ne(layers & below, 0,
						"state %d: mirror %d draws the drawn person below the water" % [state, m])
			else:
				assert_eq(layers & below, 0,
						"state %d: mirror %d never draws a person the camera does not" % [state, m])


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


# Retail's held-weapon 2 px gate projects through the subscene's own
# projection: in a water mirror the mirrored block's, the reflected eye over the
# mirror target, not the mirror collect's eye (engine renderer/object_lod.h
# held_weapon_projection_culled). A gun the main view draws and the mirror's
# drawn view sees under 2 px leaves that mirror alone.
func test_the_held_weapon_gate_runs_through_the_mirrors_drawn_view() -> void:
	var weapon := ObjectModel.new()
	add_child_autofree(weapon)
	weapon.set_authored_lod_enabled(true)
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(MODEL_3DI)), OK)
	weapon.set_object_data(data)
	weapon.set_attachment_pixel_cull(true)
	weapon.position = Vector3(0.0, 0.0, 55.0)
	var main_camera := _view_camera(Vector2i(640, 480), 90.0)
	ObjectModel.set_mirror_view_state(0, true, true)
	var far_eye := Transform3D(Basis.IDENTITY, Vector3(0.0, 0.0, 200055.0))
	ObjectModel.update_authored_lods_for_mirror_view(main_camera, 640.0, far_eye)
	assert_false(weapon.is_camera_pixel_culled(), "the main view draws it")
	assert_true(weapon.is_mirror_pixel_culled(0), "the mirror's drawn view sees it under 2 px")
	assert_true(weapon.is_mirror_view_split(0))
	assert_eq(weapon.get_mirror_view_twin_count(0), 0)
	var layers := 0
	for node in weapon.find_children("*", "MeshInstance3D", true, false):
		layers |= (node as MeshInstance3D).layers
	assert_eq(layers & (WORLD | WORLD_NO_MIRROR | MIRROR_VIEW), 0, "the gun leaves the mirror")
	assert_ne(layers & SCENE_VIEW_NO_REFLECTION, 0, "and still draws for the scene views")
	# The mirror's drawn view comes near: its gate draws the gun again.
	ObjectModel.update_authored_lods_for_mirror_view(main_camera, 640.0,
			main_camera.global_transform)
	assert_false(weapon.is_mirror_pixel_culled(0))
	assert_false(weapon.is_mirror_view_split(0))


# Retail's reflected subscene walks only its collect's lists, which the collect
# empties each time it runs (Terrain_CollectVisibleEntitiesForReflection,
# Water_RenderReflectedWorldScene). A model the mirror's own collect drops
# leaves the views' walk; its node's own edges still move it off the mirror, and
# the mirror listing it again brings it back into the walk.
func test_a_model_the_mirror_drops_leaves_the_walk() -> void:
	var container := Node3D.new()
	add_child_autofree(container)
	var model := _fixture_model(container)
	assert_not_null(model)
	if model == null:
		return
	var main_camera := _view_camera(Vector2i(640, 480), 90.0)
	ObjectModel.set_mirror_view_state(0, true, true)
	model.set_mirror_occlusion_hidden(0, false)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, null, 0.0)
	assert_eq(ObjectModel.get_view_marked_count(), 1, "the mirror lists it: walked")
	model.set_mirror_occlusion_hidden(0, true)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, null, 0.0)
	assert_true(model.is_mirror_view_split(0), "the node leaves the mirror")
	assert_eq(ObjectModel.get_view_marked_count(), 0, "dropped: out of the walk")
	# The main view hides the node: nothing left to keep off the mirror.
	model.set_occlusion_hidden(true)
	assert_false(model.is_mirror_view_split(0))
	# Shown again, its own edge takes it off the mirror again, with no walk.
	model.set_occlusion_hidden(false)
	assert_true(model.is_mirror_view_split(0))
	assert_eq(_surface_layers(model) & (WORLD | WORLD_NO_MIRROR | MIRROR_VIEW), 0)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, null, 0.0)
	assert_eq(ObjectModel.get_view_marked_count(), 0)
	# The mirror lists it again: walked, and back on the world layer.
	model.set_mirror_occlusion_hidden(0, false)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, null, 0.0)
	assert_eq(ObjectModel.get_view_marked_count(), 1)
	assert_false(model.is_mirror_view_split(0))
	assert_ne(_surface_layers(model) & WORLD_NO_MIRROR, 0)


# A part draws inside its owner's submit (the avatar head inside the body's,
# the held gun inside the person's bone callback): a part with no verdict of
# its own leaves a mirror whose collect dropped its owner, and leaves the walk
# with it.
func test_a_part_goes_with_an_owner_the_mirror_drops() -> void:
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
	ObjectModel.set_mirror_view_state(0, true, true)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, null, 0.0)
	body.set_mirror_occlusion_hidden(0, true)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, null, 0.0)
	assert_true(body.is_visible_in_tree())
	assert_true(body.is_mirror_view_split(0), "the body leaves the mirror")
	assert_true(part.is_mirror_view_split(0), "and its part with it")
	assert_eq(_surface_layers(part) & (WORLD | WORLD_NO_MIRROR | MIRROR_VIEW), 0)
	assert_eq(ObjectModel.get_view_marked_count(), 0, "neither is in the mirror's lists")
	body.set_mirror_occlusion_hidden(0, false)
	ObjectModel.update_authored_lods_for_views(main_camera, 640.0, null, 0.0)
	assert_false(body.is_mirror_view_split(0))
	assert_false(part.is_mirror_view_split(0))
	assert_eq(ObjectModel.get_view_marked_count(), 2, "both walked while the mirror lists the body")
