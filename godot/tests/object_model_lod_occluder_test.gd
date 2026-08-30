extends GutTest

# Mission ObjectModels retain all authored RLODs and switch visibility in place
# (each model only for its own instances); eligible OOBJ faces become Godot
# occluders without replacing the retail section mask owner, and without the
# model touching its viewport.

const PUMP_3DI := "res://../fixtures/threedi/synth/pump.3di"
const ARMORY_3DI := "res://../fixtures/threedi/synth/armory.3di"


func _data(path: String) -> ObjectData:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(path)), OK,
			"fixture loads")
	return data


func _lod_instances(model: ObjectModel) -> Array[GeometryInstance3D]:
	var result: Array[GeometryInstance3D] = []
	for node in model.find_children("*", "GeometryInstance3D", true, false):
		var instance := node as GeometryInstance3D
		if instance != null and instance.has_meta("_opennova_lod_index"):
			result.append(instance)
	return result


func _visible_lod_count(instances: Array[GeometryInstance3D], lod: int) -> int:
	var count := 0
	for instance in instances:
		if int(instance.get_meta("_opennova_lod_index")) == lod \
				and instance.visible:
			count += 1
	return count


func test_authored_lod_switch_reuses_the_retained_scene() -> void:
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_authored_lod_enabled(true)
	model.set_object_data(_data(PUMP_3DI))
	var lod_count := int(model.get_object_data().get_summary().get("lod_count", 0))
	assert_gt(lod_count, 1, "the pump fixture carries multiple authored RLODs")
	if lod_count <= 1:
		return

	var before := _lod_instances(model)
	var before_ids: Array[int] = []
	for instance in before:
		before_ids.append(instance.get_instance_id())
	var build_serial := model.get_scene_build_serial()
	assert_gt(_visible_lod_count(before, 0), 0, "LOD0 starts visible")
	assert_eq(_visible_lod_count(before, 1), 0, "LOD1 starts retained and hidden")

	model.set_active_lod(1)
	var after := _lod_instances(model)
	var after_ids: Array[int] = []
	for instance in after:
		after_ids.append(instance.get_instance_id())
	assert_eq(model.get_scene_build_serial(), build_serial,
			"an LOD transition never rebuilds the ObjectModel subtree")
	assert_eq(after_ids, before_ids, "every retained geometry instance is reused")
	assert_eq(_visible_lod_count(after, 0), 0, "the finer LOD is hidden")
	assert_gt(_visible_lod_count(after, 1), 0, "the selected coarser LOD is shown")


func test_preview_lod_switch_keeps_the_single_lod_memory_contract() -> void:
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_object_data(_data(PUMP_3DI))
	var lod_count := int(model.get_object_data().get_summary().get("lod_count", 0))
	assert_gt(lod_count, 1)
	if lod_count <= 1:
		return
	var before_serial := model.get_scene_build_serial()
	var before := _lod_instances(model)
	assert_gt(before.size(), 0)
	for instance in before:
		assert_eq(int(instance.get_meta("_opennova_lod_index")), 0,
				"the preview initially builds only its selected level")
	model.set_active_lod(1)
	assert_eq(model.get_scene_build_serial(), before_serial + 1,
			"an explicit preview LOD switch rebuilds its one selected mesh set")
	var after := _lod_instances(model)
	assert_gt(after.size(), 0)
	for instance in after:
		assert_eq(int(instance.get_meta("_opennova_lod_index")), 1,
				"the preview still retains only its selected level")


func test_nested_model_keeps_its_instances_across_the_parent_lod_switch() -> void:
	# A husk graft is built as a child of its intact building and an avatar
	# head under its body: each model owns only its own retained instances, so
	# the parent's level switch must not hide a nested model's geometry.
	var parent := ObjectModel.new()
	add_child_autofree(parent)
	parent.set_authored_lod_enabled(true)
	parent.set_object_data(_data(PUMP_3DI))
	var lod_count := int(parent.get_object_data().get_summary().get("lod_count", 0))
	assert_gt(lod_count, 1, "the parent carries multiple authored RLODs")
	if lod_count <= 1:
		return
	var child := ObjectModel.new()
	parent.add_child(child)
	child.set_authored_lod_enabled(true)
	child.set_object_data(_data(ARMORY_3DI))
	var child_instances := _lod_instances(child)
	assert_gt(child_instances.size(), 0, "the nested model retains tagged instances")
	var child_level := child.get_active_lod()
	var visible_before := _visible_lod_count(child_instances, child_level)
	assert_gt(visible_before, 0, "the nested model's selected level starts visible")

	parent.set_active_lod(1)
	assert_eq(parent.get_active_lod(), 1)
	assert_eq(child.get_active_lod(), child_level,
			"the parent's switch never selects for the nested model")
	assert_eq(_visible_lod_count(child_instances, child_level), visible_before,
			"the nested model's instances keep their visibility across the parent's switch")
	assert_eq(_visible_lod_count(_lod_instances(parent).filter(
			func(instance: GeometryInstance3D) -> bool:
				return not child.is_ancestor_of(instance)), 0), 0,
			"the parent's own finer level is hidden")


func test_closed_authored_records_create_section_owned_occluders() -> void:
	var viewport := get_viewport()
	var viewport_was_enabled := viewport.use_occlusion_culling
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_authored_occluders_enabled(true)
	model.set_object_data(_data(ARMORY_3DI))
	var occluders := model.find_children("AuthoredOccluder_Section*",
			"OccluderInstance3D", true, false)
	assert_gt(occluders.size(), 0,
			"eligible armory OOBJ records become ArrayOccluder3D instances")
	assert_eq(model.get_authored_occluder_count(), occluders.size(),
			"the world reads the retained occluder count to decide on culling")
	assert_eq(viewport.use_occlusion_culling, viewport_was_enabled,
			"a model never flips its viewport's occlusion consumer; the world owns that")
	for node in occluders:
		var instance := node as OccluderInstance3D
		assert_not_null(instance.occluder, "the retained instance owns geometry")

	model.set_section_visibility_mask(0)
	for node in occluders:
		assert_false((node as OccluderInstance3D).visible,
				"the retail section mask also gates its Godot occluder")
	model.set_section_visibility_mask(-1)
	for node in occluders:
		assert_true((node as OccluderInstance3D).visible,
				"clearing the section verdict restores the occluder")
