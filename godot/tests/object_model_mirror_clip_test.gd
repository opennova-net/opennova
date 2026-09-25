extends GutTest

# The water mirror's per-draw CLIP arming as ObjectModel keeps it
# (runtime/environment/water_mirror.h water_mirror_clip_armed): the first
# entity wave arms while z - boundRadius sits below the water height
# (retail Terrain_RenderSectorEntities @ 0x5c7c1a..0x5c7c2e), the building
# pass while the model's floor sits below wh - 0.25 (retail
# Terrain_RenderSectorModels @ 0x5c5e57..0x5c5e75), a BySide person never.
# The verdict rides u_entity_light.w's bit 2 on every surface instance.

const MODEL_3DI := "res://../fixtures/threedi/synth/crate.3di"


func after_each() -> void:
	ObjectShaderCache.get_singleton().clear_water_plane()


func _model() -> ObjectModel:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(MODEL_3DI)), OK)
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_object_data(data)
	return model


func _clip_bit(model: ObjectModel) -> float:
	var meshes: Array[MeshInstance3D] = []
	_meshes(model, meshes)
	assert_gt(meshes.size(), 0)
	var lane: Vector4 = meshes[0].get_instance_shader_parameter("u_entity_light")
	return lane.w


func _meshes(node: Node, out: Array[MeshInstance3D]) -> void:
	if node is MeshInstance3D:
		out.append(node as MeshInstance3D)
	for child in node.get_children():
		_meshes(child, out)


func test_an_entity_arms_while_its_bound_sphere_reaches_below_the_water() -> void:
	var cache := ObjectShaderCache.get_singleton()
	cache.set_water_plane(7.0, true)
	var model := _model()
	model.set_shadow_bound_radii(1.0, 1.0)
	model.global_position = Vector3(0.0, 7.5, 0.0)
	# Godot flushes the transform notifications at the end of the frame.
	await get_tree().process_frame
	assert_true(model.is_water_mirror_clip_armed(), "z - r = 6.5 sits below 7")
	assert_eq(_clip_bit(model), 2.0, "the verdict rides the lane's bit 2")
	model.global_position = Vector3(0.0, 8.0, 0.0)
	await get_tree().process_frame
	assert_false(model.is_water_mirror_clip_armed(), "z - r = 7 is not below 7")
	assert_eq(_clip_bit(model), 0.0)
	cache.set_water_plane(20.0, true)
	assert_true(model.is_water_mirror_clip_armed(), "a higher plane re-tests every model")
	model.set_thermal_entity_wave(true)
	assert_false(model.is_water_mirror_clip_armed(), "a BySide person never arms")
	assert_eq(_clip_bit(model), 1.0, "the person bit alone")
	model.set_thermal_entity_wave(false)
	model.set_water_mirror_clip_wave_id(ObjectModel.WATER_MIRROR_CLIP_NONE)
	assert_false(model.is_water_mirror_clip_armed(), "a draw outside the sector walks never arms")
	cache.clear_water_plane()


func test_a_building_arms_while_its_floor_sits_a_quarter_below_the_water() -> void:
	var cache := ObjectShaderCache.get_singleton()
	var model := _model()
	model.set_water_mirror_clip_wave_id(ObjectModel.WATER_MIRROR_CLIP_SECTOR_MODEL)
	model.global_position = Vector3.ZERO
	await get_tree().process_frame
	# The floor is the CMDL header z-lo; find the plane heights around it by
	# the test itself: a plane far above arms, one far below does not.
	cache.set_water_plane(100.0, true)
	assert_true(model.is_water_mirror_clip_armed(), "a building deep under the plane arms")
	cache.set_water_plane(-100.0, true)
	assert_false(model.is_water_mirror_clip_armed(), "a building far above the plane does not")
	cache.clear_water_plane()
	assert_false(model.is_water_mirror_clip_armed(), "no plane, no mirror pass")
