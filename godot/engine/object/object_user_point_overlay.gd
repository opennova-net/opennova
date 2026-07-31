class_name ObjectUserPointOverlay
extends Node3D

# Reusable 3D overlay for NovaObjectData userpoints. With a source NovaObjectModel,
# points are resolved through that model's live skeleton / part nodes so skeletal and
# PANM movement are visible.
# Without a source model, the overlay is just model-local and can be transformed as a
# whole by a mission entity transform.

const MARKER_RADIUS := 0.16
const LABEL_OFFSET := Vector3(0.0, 0.55, 0.0)
const LABEL_FONT_SIZE := 8
const LABEL_OUTLINE_SIZE := 8
const LABEL_PIXEL_SIZE := 0.025
const MARKER_COLOR := Color(0.2, 0.9, 1.0, 0.95)

var _object_data: NovaObjectData
var _source_model: Node3D
var _entity_transform := Transform3D.IDENTITY
var _markers: Array = []
var _marker_mesh: SphereMesh
var _marker_material: StandardMaterial3D


func _ready() -> void:
	set_process(_source_model != null)


func set_object_data(data: NovaObjectData) -> void:
	if _object_data == data:
		return
	_object_data = data
	_rebuild()


func set_source_model(model: Node3D) -> void:
	if _source_model == model:
		return
	_source_model = model
	set_process(_source_model != null)
	_sync_root_transform()
	_rebuild()


func set_entity_transform(xform: Transform3D) -> void:
	_entity_transform = xform
	_sync_root_transform()
	_update_live_marker_positions()


func set_points_visible(points_visible: bool) -> void:
	visible = points_visible and has_user_points()


func has_user_points() -> bool:
	return get_user_point_count() > 0


func get_user_point_count() -> int:
	if _object_data == null:
		return 0
	return _object_data.get_user_point_count()


func refresh_points() -> void:
	_rebuild()


## Resolve the current source pose immediately. Debug owners and tests use this
## instead of reaching through the process callback.
func refresh_now() -> void:
	_update_live_marker_positions()


func _process(_delta: float) -> void:
	refresh_now()


func _rebuild() -> void:
	for child in get_children():
		remove_child(child)
		child.queue_free()
	_markers.clear()
	_sync_root_transform()
	if not has_user_points():
		visible = false
		return
	_ensure_marker_resources()
	for i in range(get_user_point_count()):
		var info: Dictionary = _object_data.get_user_point_info(i)
		if info.is_empty():
			continue
		var model_pos: Vector3 = info.get("position", Vector3.ZERO)
		var subobject := int(info.get("subobject", -1))
		var marker := _make_marker(i, _label_text(info, i))
		add_child(marker)
		_markers.append({
			"node": marker,
			"model_position": model_pos,
			"subobject": subobject,
			"part_local": _part_local_position(model_pos, subobject),
		})
		if _source_model == null:
			marker.position = model_pos
	_update_live_marker_positions()


func _make_marker(index: int, label_text: String) -> MeshInstance3D:
	var marker := MeshInstance3D.new()
	marker.name = "UserPoint_%02d" % index
	marker.mesh = _marker_mesh
	marker.material_override = _marker_material
	marker.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	var label := Label3D.new()
	label.name = "UserPointLabel_%02d" % index
	label.text = label_text
	label.billboard = BaseMaterial3D.BILLBOARD_ENABLED
	label.fixed_size = false
	label.pixel_size = LABEL_PIXEL_SIZE
	label.no_depth_test = true
	label.font_size = LABEL_FONT_SIZE
	label.outline_size = LABEL_OUTLINE_SIZE
	label.modulate = Color.WHITE
	label.outline_modulate = Color(0.0, 0.0, 0.0, 0.9)
	label.position = LABEL_OFFSET
	marker.add_child(label)
	return marker


func _label_text(info: Dictionary, index: int) -> String:
	var text := String(info.get("name", "")).strip_edges()
	return text if not text.is_empty() else "userpoint_%02d" % index


func _ensure_marker_resources() -> void:
	if _marker_mesh == null:
		_marker_mesh = SphereMesh.new()
		_marker_mesh.radius = MARKER_RADIUS
		_marker_mesh.height = MARKER_RADIUS * 2.0
		_marker_mesh.radial_segments = 12
		_marker_mesh.rings = 6
	if _marker_material == null:
		_marker_material = StandardMaterial3D.new()
		_marker_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
		_marker_material.albedo_color = MARKER_COLOR
		_marker_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
		_marker_material.no_depth_test = true


func _sync_root_transform() -> void:
	transform = Transform3D.IDENTITY if _source_model != null else _entity_transform


func _part_local_position(model_position: Vector3, subobject: int) -> Vector3:
	if _object_data == null or subobject < 0:
		return model_position
	var rest: Variant = _rest_part_transform(subobject)
	if rest == null:
		return model_position
	return (rest as Transform3D).affine_inverse() * model_position


func _rest_part_transform(subobject: int):
	var lod := 0
	if _source_model != null and _source_model.has_method("get_active_lod"):
		lod = int(_source_model.get_active_lod())
	var transforms: Dictionary = _object_data.evaluate_panm(lod, 0, {})
	if transforms.has(subobject):
		return transforms[subobject]
	if transforms.has(str(subobject)):
		return transforms[str(subobject)]
	return null


func _update_live_marker_positions() -> void:
	if _source_model == null or not is_instance_valid(_source_model) or _markers.is_empty():
		return
	var part_nodes: Dictionary = {}
	if _source_model.has_method("get_render_part_nodes"):
		part_nodes = _source_model.get_render_part_nodes()
	var skeleton: Skeleton3D = null
	if _source_model.has_method("get_skeleton"):
		skeleton = _source_model.call("get_skeleton") as Skeleton3D
	var visual_layers := _source_visual_layers()
	for entry in _markers:
		var marker := entry.get("node") as Node3D
		if marker == null or not is_instance_valid(marker):
			continue
		_sync_marker_layers(marker, visual_layers)
		var subobject := int(entry.get("subobject", -1))
		var global_pos: Vector3
		if skeleton != null and subobject >= 0 and subobject < skeleton.get_bone_count():
			# Fake-skinned rigid parts carry authored model-space user points by the
			# same subobject/bone index. Move model space into the bone's rest frame,
			# then through its live pose -- exactly the action-particle attachment
			# transform used by LocalPlayerPresenter.
			var model_to_world := (skeleton.global_transform
					* skeleton.get_bone_global_pose(subobject)
					* skeleton.get_bone_global_rest(subobject).affine_inverse())
			global_pos = model_to_world * (entry.get("model_position", Vector3.ZERO) as Vector3)
		elif subobject >= 0 and part_nodes.has(subobject):
			var part := part_nodes[subobject] as Node3D
			global_pos = part.global_transform * (entry.get("part_local", Vector3.ZERO) as Vector3)
		else:
			global_pos = _source_model.global_transform * (entry.get("model_position", Vector3.ZERO) as Vector3)
		marker.global_position = global_pos


# First-person arms/guns render through the dedicated render-FOV camera on a
# non-world cull layer. Their debug marker and label must ride the same mask or
# the world camera projects the point differently and it appears off the muzzle.
func _source_visual_layers() -> int:
	var layers := _collect_visual_layers(_source_model)
	return layers if layers != 0 else 1


func _collect_visual_layers(node: Node) -> int:
	if node == null or not is_instance_valid(node):
		return 0
	var layers := int((node as VisualInstance3D).layers) if node is VisualInstance3D else 0
	for child in node.get_children():
		layers |= _collect_visual_layers(child)
	return layers


func _sync_marker_layers(marker: Node3D, layers: int) -> void:
	if marker is VisualInstance3D:
		(marker as VisualInstance3D).layers = layers
	for child in marker.get_children():
		if child is VisualInstance3D:
			(child as VisualInstance3D).layers = layers
