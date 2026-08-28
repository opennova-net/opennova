extends GutTest

const UserPointDebugView := preload("res://game/debug/user_point_debug_view.gd")
const UserPointOverlay := preload("res://game/object/object_user_point_overlay.gd")
const GUN_FIXTURE := "res://../fixtures/threedi/synth/gun.3di"


func test_live_overlay_maps_user_point_through_the_live_part_frame() -> void:
	var data := _load_gun()
	var user_point_index := _first_bone_user_point(data)
	assert_gte(user_point_index, 0, "gun fixture should carry a part-owned user point")
	if user_point_index < 0:
		return
	var info: Dictionary = data.get_user_point_info(user_point_index)
	var part_index := int(info.get("subobject", -1))

	var model := ObjectModel.new()
	add_child_autofree(model)
	model.set_process(false)
	model.set_object_data(data)
	model.transform = Transform3D(Basis(Vector3.UP, 0.25), Vector3(4.0, 2.0, -3.0))
	_set_visual_layers(model, 1 << 11)
	var part_nodes: Dictionary = model.get_render_part_nodes()
	assert_true(part_nodes.has(part_index),
			"the rigid gun builds a live part node for the point's subobject")
	if not part_nodes.has(part_index):
		return
	# Pose the real part node off its rest — the same write the part-anim
	# applier performs mid-recoil — so the live mapping below cannot pass by
	# coincidence with the naive model-root mapping.
	var part := part_nodes[part_index] as Node3D
	part.transform = part.transform * Transform3D(
			Basis(Vector3.UP, 0.6), Vector3(0.2, 0.1, -0.7))

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
	# The overlay's contract: model-space point moved into the part's rest
	# frame, then out through the part's LIVE global transform.
	var rest: Transform3D = data.evaluate_panm(0, 0, {})[part_index]
	var expected: Vector3 = (part.global_transform * rest.affine_inverse()
			* Vector3(info.get("position", Vector3.ZERO)))
	assert_true(marker.global_position.is_equal_approx(expected),
			"the marker rides the part's live frame, the muzzle-seam composition")
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
	var data := _load_gun()
	var root := Node3D.new()
	add_child_autofree(root)
	var view := UserPointDebugView.new()
	view.name = "UserPointDebug"
	root.add_child(view)
	var static_a := Transform3D(Basis.IDENTITY, Vector3(2.0, 0.0, 0.0))
	var static_b := Transform3D(Basis(Vector3.UP, 0.5), Vector3(-3.0, 1.0, 4.0))
	view.setup(root, [{
		"graphic": "gun",
		"object_data": data,
		"transforms": [static_a, static_b],
	}])

	assert_eq(view.get_static_overlay_count(), 2,
			"one overlay is built per static entity, never per submesh")
	assert_eq(view.get_live_overlay_count(), 0)

	# Discovery is typed: live sources ARE ObjectModels.
	var live := ObjectModel.new()
	root.add_child(live)
	live.set_process(false)
	live.set_object_data(data)
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


func _load_gun() -> ObjectData:
	var data := ObjectData.new()
	assert_eq(data.open_file(ProjectSettings.globalize_path(GUN_FIXTURE)), OK,
			"gun fixture should open through ObjectData")
	return data


func _set_visual_layers(node: Node, layers: int) -> void:
	if node is VisualInstance3D:
		(node as VisualInstance3D).layers = layers
	for child in node.get_children():
		_set_visual_layers(child, layers)


func _first_bone_user_point(data: ObjectData) -> int:
	for i in range(data.get_user_point_count()):
		if int(data.get_user_point_info(i).get("subobject", -1)) >= 0:
			return i
	return -1
