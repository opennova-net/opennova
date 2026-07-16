extends GutTest

const UserPointDebugView := preload("res://engine/debug/user_point_debug_view.gd")
const UserPointOverlay := preload("res://engine/object/object_user_point_overlay.gd")
const MP5_FIXTURE := "res://../fixtures/threedi/3di3/MP5.3di"


class FakeLiveModel:
	extends Node3D
	var object_data: NovaObjectData
	var skeleton: Skeleton3D

	func get_object_data() -> NovaObjectData:
		return object_data

	func get_skeleton() -> Skeleton3D:
		return skeleton

	func get_render_part_nodes() -> Dictionary:
		return {}

	func get_active_lod() -> int:
		return 0


func test_live_overlay_maps_user_point_through_non_identity_bone_rest_and_pose() -> void:
	var data := _load_mp5()
	var user_point_index := _first_bone_user_point(data)
	assert_gte(user_point_index, 0, "MP5 fixture should carry a bone-owned user point")
	if user_point_index < 0:
		return
	var info: Dictionary = data.get_user_point_info(user_point_index)
	var bone_index := int(info.get("subobject", -1))

	var model := FakeLiveModel.new()
	model.object_data = data
	model.transform = Transform3D(Basis(Vector3.UP, 0.25), Vector3(4.0, 2.0, -3.0))
	var viewmodel_visual := MeshInstance3D.new()
	viewmodel_visual.layers = 1 << 11
	model.add_child(viewmodel_visual)
	var skeleton := Skeleton3D.new()
	skeleton.name = "Skeleton3D"
	for i in range(bone_index + 1):
		skeleton.add_bone("bone_%d" % i)
	var rest := Transform3D(Basis(Vector3.RIGHT, -0.35), Vector3(0.4, 0.2, -0.1))
	skeleton.set_bone_rest(bone_index, rest)
	skeleton.set_bone_pose_position(bone_index, Vector3(0.2, 0.1, -0.7))
	skeleton.set_bone_pose_rotation(bone_index, Quaternion(Vector3.UP, 0.6))
	model.add_child(skeleton)
	model.skeleton = skeleton
	add_child_autofree(model)
	skeleton.force_update_all_bone_transforms()

	var overlay := UserPointOverlay.new()
	add_child_autofree(overlay)
	overlay.set_source_model(model)
	overlay.set_object_data(data)
	overlay.set_points_visible(true)
	overlay.refresh_now()

	var marker := overlay.get_node_or_null("UserPoint_%02d" % user_point_index) as Node3D
	assert_not_null(marker, "the authored point gets a stable marker")
	if marker == null:
		return
	var model_to_world := (skeleton.global_transform
			* skeleton.get_bone_global_pose(bone_index)
			* skeleton.get_bone_global_rest(bone_index).affine_inverse())
	var expected: Vector3 = model_to_world * Vector3(info.get("position", Vector3.ZERO))
	assert_true(marker.global_position.is_equal_approx(expected),
			"the marker uses the same rest-to-live pose composition as muzzle effects")
	assert_false(marker.global_position.is_equal_approx(
			model.global_transform * Vector3(info.get("position", Vector3.ZERO))),
			"the regression would incorrectly leave the point on the model root")
	assert_eq((marker as VisualInstance3D).layers, 1 << 11,
			"the marker renders through the source viewmodel camera layer")
	var label := marker.get_node_or_null("UserPointLabel_%02d" % user_point_index) as Label3D
	assert_not_null(label)
	if label != null:
		assert_eq(label.layers, 1 << 11,
				"the label stays in the same projection pass as its marker")


func test_world_view_tracks_grouped_static_instances_and_live_model_lifecycle() -> void:
	var data := _load_mp5()
	var root := Node3D.new()
	add_child_autofree(root)
	var view := UserPointDebugView.new()
	view.name = "UserPointDebug"
	root.add_child(view)
	var static_a := Transform3D(Basis.IDENTITY, Vector3(2.0, 0.0, 0.0))
	var static_b := Transform3D(Basis(Vector3.UP, 0.5), Vector3(-3.0, 1.0, 4.0))
	view.setup(root, [{
		"graphic": "MP5",
		"object_data": data,
		"transforms": [static_a, static_b],
	}])

	assert_eq(view.get_static_overlay_count(), 2,
			"one overlay is built per static entity, never per submesh")
	assert_eq(view.get_live_overlay_count(), 0)

	var live := FakeLiveModel.new()
	live.object_data = data
	root.add_child(live)
	view.refresh_now()
	assert_eq(view.get_live_overlay_count(), 1,
			"a newly spawned model is discovered without rebuilding static points")

	root.remove_child(live)
	live.queue_free()
	view.refresh_now()
	assert_eq(view.get_live_overlay_count(), 0,
			"a despawned model drops its overlay and stale source reference")
	assert_eq(view.get_static_overlay_count(), 2,
			"live churn leaves retained static sources intact")


func _load_mp5() -> NovaObjectData:
	var data := NovaObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(MP5_FIXTURE)), OK,
			"MP5 fixture should open through NovaObjectData")
	return data


func _first_bone_user_point(data: NovaObjectData) -> int:
	for i in range(data.get_user_point_count()):
		if int(data.get_user_point_info(i).get("subobject", -1)) >= 0:
			return i
	return -1
