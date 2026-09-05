extends GutTest

# Mission ObjectModels cache every authored RLOD as mesh/material rows and
# retain ONE MeshInstance3D per surface slot, swapping the active level's rows
# onto those slots in place (each model only for its own slots; a level-bound
# visual another owner parented under the model follows the same switch);
# eligible OOBJ faces become Godot occluders without replacing the retail
# section mask owner, and without the model touching its viewport.

const PUMP_3DI := "res://../fixtures/threedi/synth/pump.3di"
const ARMORY_3DI := "res://../fixtures/threedi/synth/armory.3di"


func _data(path: String) -> ObjectData:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(path)), OK,
			"fixture loads")
	return data


## Every MeshInstance3D the model itself retains (a nested ObjectModel's
## instances excluded).
func _own_instances(model: ObjectModel) -> Array[MeshInstance3D]:
	var result: Array[MeshInstance3D] = []
	for node in model.find_children("*", "MeshInstance3D", true, false):
		var instance := node as MeshInstance3D
		if instance == null:
			continue
		var owner := instance.get_parent()
		while owner != null and owner != model and not (owner is ObjectModel):
			owner = owner.get_parent()
		if owner == model:
			result.append(instance)
	return result


## The retained surface slots drawn right now: visible instances that are not
## an auxiliary postmultiply pair.
func _visible_slot_count(instances: Array[MeshInstance3D]) -> int:
	var count := 0
	for instance in instances:
		if instance.visible and instance.mesh != null \
				and not (instance is PostMultiplyDraw):
			count += 1
	return count


func _ids(instances: Array[MeshInstance3D]) -> Array[int]:
	var ids: Array[int] = []
	for instance in instances:
		ids.append(instance.get_instance_id())
	ids.sort()
	return ids


func test_authored_lod_switch_swaps_the_level_onto_the_retained_slots() -> void:
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_authored_lod_enabled(true)
	model.set_object_data(_data(PUMP_3DI))
	var lod_count := model.get_object_data().get_lod_count()
	assert_gt(lod_count, 1, "the pump fixture carries multiple authored RLODs")
	if lod_count <= 1:
		return

	var before := _own_instances(model)
	var build_serial := model.get_scene_build_serial()
	var widest := 0
	for lod in range(lod_count):
		widest = maxi(widest, model.get_level_surface_count(lod))
	assert_gt(model.get_level_surface_count(0), 0, "LOD0 carries surfaces")
	assert_gt(model.get_level_surface_count(1), 0, "LOD1 carries surfaces")
	assert_eq(model.get_surface_slot_count(), widest,
			"one slot per surface of the widest level, never one per retained level")
	assert_eq(before.size(), model.get_retained_surface_instance_count(),
			"the tree holds exactly the retained slots plus their auxiliary pairs")
	assert_eq(_visible_slot_count(before), model.get_level_surface_count(0),
			"LOD0's surfaces are the ones drawn")

	model.set_active_lod(1)
	var after := _own_instances(model)
	assert_eq(model.get_scene_build_serial(), build_serial,
			"an LOD transition never rebuilds the ObjectModel subtree")
	assert_eq(_ids(after), _ids(before), "every retained surface instance is reused")
	assert_eq(model.get_surface_slot_count(), widest, "no slot appears or disappears")
	assert_eq(_visible_slot_count(after), model.get_level_surface_count(1),
			"the selected coarser level's surfaces are the ones drawn")
	# Coming back keeps the same nodes and serial too.
	model.set_active_lod(0)
	assert_eq(model.get_scene_build_serial(), build_serial)
	assert_eq(_ids(_own_instances(model)), _ids(before))
	assert_eq(_visible_slot_count(_own_instances(model)),
			model.get_level_surface_count(0))


func test_preview_lod_switch_keeps_the_single_lod_memory_contract() -> void:
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_object_data(_data(PUMP_3DI))
	var lod_count := model.get_object_data().get_lod_count()
	assert_gt(lod_count, 1)
	if lod_count <= 1:
		return
	var before_serial := model.get_scene_build_serial()
	assert_gt(model.get_level_surface_count(0), 0,
			"the preview builds its selected level")
	assert_eq(model.get_level_surface_count(1), 0,
			"the preview initially builds only its selected level")
	assert_eq(model.get_surface_slot_count(), model.get_level_surface_count(0))
	model.set_active_lod(1)
	assert_eq(model.get_scene_build_serial(), before_serial + 1,
			"an explicit preview LOD switch rebuilds its one selected mesh set")
	assert_gt(model.get_level_surface_count(1), 0)
	assert_eq(model.get_level_surface_count(0), 0,
			"the preview still retains only its selected level")
	assert_eq(model.get_surface_slot_count(), model.get_level_surface_count(1))
	# Rebuilding queues the previous preview subtree for deletion.
	await get_tree().process_frame


func test_nested_model_keeps_its_instances_across_the_parent_lod_switch() -> void:
	# A husk graft is built as a child of its intact building and an avatar
	# head under its body: each model owns only its own slots, so the
	# parent's level switch must not touch a nested model's geometry.
	var parent := ObjectModel.new()
	add_child_autofree(parent)
	parent.set_authored_lod_enabled(true)
	parent.set_object_data(_data(PUMP_3DI))
	var lod_count := parent.get_object_data().get_lod_count()
	assert_gt(lod_count, 1, "the parent carries multiple authored RLODs")
	if lod_count <= 1:
		return
	var child := ObjectModel.new()
	parent.add_child(child)
	child.set_authored_lod_enabled(true)
	child.set_object_data(_data(ARMORY_3DI))
	var child_instances := _own_instances(child)
	assert_gt(child_instances.size(), 0, "the nested model retains its own slots")
	var child_level := child.get_active_lod()
	var visible_before := _visible_slot_count(child_instances)
	assert_gt(visible_before, 0, "the nested model's selected level starts visible")
	var child_serial := child.get_scene_build_serial()
	var child_parents: Array[Node] = []
	for instance in child_instances:
		child_parents.append(instance.get_parent())

	parent.set_active_lod(1)
	assert_eq(parent.get_active_lod(), 1)
	assert_eq(child.get_active_lod(), child_level,
			"the parent's switch never selects for the nested model")
	assert_eq(child.get_scene_build_serial(), child_serial)
	assert_eq(_visible_slot_count(child_instances), visible_before,
			"the nested model's instances keep their visibility across the parent's switch")
	for index in range(child_instances.size()):
		assert_same(child_instances[index].get_parent(), child_parents[index],
				"the parent's swap never reparents a nested model's slot")
	assert_eq(_visible_slot_count(_own_instances(parent)),
			parent.get_level_surface_count(1),
			"the parent draws its own coarser level only")


func test_level_bound_visual_follows_the_owner_switch() -> void:
	# The placer parents shadow-only siblings under an individual model and
	# binds each to the level it was harvested from; the owner's swap shows
	# exactly the bound level's siblings beside its own slots.
	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_authored_lod_enabled(true)
	model.set_object_data(_data(PUMP_3DI))
	var lod_count := model.get_object_data().get_lod_count()
	assert_gt(lod_count, 1)
	if lod_count <= 1:
		return
	var fine := MeshInstance3D.new()
	var coarse := MeshInstance3D.new()
	model.add_child(fine)
	model.add_child(coarse)
	model.add_level_bound_visual(0, fine)
	model.add_level_bound_visual(1, coarse)
	assert_true(fine.visible, "a visual bound to the active level shows")
	assert_false(coarse.visible, "a visual bound to another level hides")
	model.set_active_lod(1)
	assert_false(fine.visible)
	assert_true(coarse.visible)
	model.set_active_lod(0)
	assert_true(fine.visible)
	assert_false(coarse.visible)


func test_attachment_takes_its_owner_level_clamped_to_its_own_count() -> void:
	# An attached model (the third-person held weapon, the NVG/binocular
	# items, a mounted child) never walks its own thresholds: retail's bone
	# callback indexes every overlay model with the parent's selected level
	# clamped to the overlay's own LOD count. The frame walk applies that
	# after the owners' selections; the camera below looks away from both
	# models, so the owner keeps the level set here rather than reselecting.
	var owner := ObjectModel.new()
	add_child_autofree(owner)
	owner.set_authored_lod_enabled(true)
	owner.set_object_data(_data(PUMP_3DI))
	var lod_count := owner.get_object_data().get_lod_count()
	assert_gt(lod_count, 1, "the owner carries multiple authored RLODs")
	if lod_count <= 1:
		return
	var attachment := ObjectModel.new()
	add_child_autofree(attachment)
	attachment.set_authored_lod_enabled(true)
	attachment.set_object_data(_data(PUMP_3DI))
	attachment.set_authored_lod_owner(owner)
	assert_eq(attachment.get_authored_lod_owner(), owner)
	owner.position = Vector3(0.0, 0.0, 50.0)
	attachment.position = Vector3(0.0, 0.0, 50.0)
	var away := Transform3D(Basis.IDENTITY, Vector3.ZERO)

	owner.set_active_lod(1)
	ObjectModel.update_authored_lods(away, 60.0, 640.0, 480.0)
	assert_eq(owner.get_active_lod(), 1, "an owner outside the frustum keeps its level")
	assert_eq(attachment.get_active_lod(), 1,
			"the attachment draws at its owner's level")
	owner.set_active_lod(0)
	ObjectModel.update_authored_lods(away, 60.0, 640.0, 480.0)
	assert_eq(attachment.get_active_lod(), 0, "and follows it back down")

	# A level past the attachment's own count clamps to its last level.
	owner.set_active_lod(lod_count - 1)
	var single := ObjectModel.new()
	add_child_autofree(single)
	single.set_authored_lod_enabled(true)
	single.set_object_data(_data(ARMORY_3DI))
	single.set_authored_lod_owner(owner)
	single.position = Vector3(0.0, 0.0, 50.0)
	ObjectModel.update_authored_lods(away, 60.0, 640.0, 480.0)
	assert_eq(single.get_active_lod(),
			mini(lod_count - 1, single.get_object_data().get_lod_count() - 1),
			"the owner's level clamps to the attachment's own count")

	# A freed owner reads as level 0.
	owner.set_active_lod(1)
	ObjectModel.update_authored_lods(away, 60.0, 640.0, 480.0)
	assert_eq(attachment.get_active_lod(), 1)
	owner.free()
	assert_null(attachment.get_authored_lod_owner())
	ObjectModel.update_authored_lods(away, 60.0, 640.0, 480.0)
	assert_eq(attachment.get_active_lod(), 0, "no owner selects the finest level")


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


func test_exact_owner_lod_skips_a_missing_marker_level() -> void:
	var source := ObjectModel.new()
	add_child_autofree(source)
	source.set_authored_lod_enabled(true)
	source.set_object_data(_data(PUMP_3DI))
	source.position = Vector3(0, 0, 50)
	var marker := ObjectModel.new()
	add_child_autofree(marker)
	marker.set_authored_lod_enabled(true)
	marker.set_object_data(_data("res://../fixtures/threedi/synth/crate.3di"))
	marker.set_authored_lod_owner(source, true)
	source.set_active_lod(1)
	ObjectModel.update_authored_lods(Transform3D.IDENTITY, 60, 640, 480)
	assert_eq(marker.get_active_lod(), 1, "callback LOD is not clamped")
	assert_eq(_visible_slot_count(_own_instances(marker)), 0)
	source.set_active_lod(0)
	ObjectModel.update_authored_lods(Transform3D.IDENTITY, 60, 640, 480)
	assert_gt(_visible_slot_count(_own_instances(marker)), 0)

func test_hidden_source_geometry_survives_lod_switches() -> void:
	var source := ObjectModel.new()
	add_child_autofree(source)
	source.set_authored_lod_enabled(true)
	source.set_object_data(_data(PUMP_3DI))
	source.set_geometry_visible(false)
	source.set_active_lod(1)
	assert_true(source.is_present_visible(), "source remains available to visibility and LOD")
	assert_eq(_visible_slot_count(_own_instances(source)), 0)
	source.set_active_lod(0)
	assert_eq(_visible_slot_count(_own_instances(source)), 0)

func test_rigid_marker_parts_ignore_live_panm_and_rest_offsets() -> void:
	var marker := ObjectModel.new()
	add_child_autofree(marker)
	marker.set_authored_lod_enabled(true)
	marker.set_object_data(_data(PUMP_3DI))
	marker.set_rigid_parts(true)
	for level in [0, 1, 0]:
		marker.set_active_lod(level)
		ObjectModel.advance_awake_frame(0.125)
		for part in marker.get_render_part_nodes().values():
			assert_true((part as Node3D).transform.is_equal_approx(Transform3D.IDENTITY))
