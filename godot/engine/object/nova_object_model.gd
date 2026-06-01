class_name NovaObjectModel
extends Node3D

signal bounds_changed(bounds: AABB)

const MATERIAL_FLAG_ALPHA_TEST := 0x01
const MATERIAL_FLAG_ALPHA_INVERT := 0x02
const MATERIAL_FLAG_TWO_SIDED := 0x04
const OED_UPDATE_NONE := 0
const OED_UPDATE_MTRL := 1
const OED_UPDATE_LGHT := 2
const OED_UPDATE_PANM := 4
const OED_UPDATE_ALL := OED_UPDATE_MTRL | OED_UPDATE_LGHT | OED_UPDATE_PANM

const DEFAULT_AMBIENT_COLOR := Vector3(0.35, 0.36, 0.40)
const DEFAULT_DIR_LIGHT_DIR := Vector3(-0.4082, -0.8165, -0.4082)
const DEFAULT_DIR_LIGHT_COLOR := Vector3(0.85, 0.82, 0.75)
const DEFAULT_FILL_LIGHT_COLOR := Vector3(0.18, 0.20, 0.25)
const DEFAULT_FOG_COLOR := Vector3(0.5, 0.6, 0.8)
const DEFAULT_FOG_START := 0.0
const DEFAULT_FOG_END := 1024.0
const DEFAULT_FOG_TYPE := 0

var object_data: NovaObjectData

var _material_cache: Dictionary = {}
var _material_defs: Dictionary = {}
var _robj_nodes: Dictionary = {}
var _skeleton_node: Skeleton3D
var _skeleton_skin: Skin
var _skinned_mesh_instances: Array[MeshInstance3D] = []
var _surface_material_indices: PackedInt32Array = PackedInt32Array()
var _surface_materials: Array[ShaderMaterial] = []
var _anim_frames_by_mat: Dictionary = {}
var _ctrl_values: Dictionary = {}
var _anim_time_ms: int = 0
var _active_lod: int = 0
var _is_playing := true
var _model_bounds := AABB()
var _environment_node: Node


func _ready() -> void:
	set_process(true)
	if object_data != null:
		rebuild()


func set_object_data(value: NovaObjectData) -> void:
	if object_data != null and object_data.object_changed.is_connected(_on_object_changed):
		object_data.object_changed.disconnect(_on_object_changed)
	object_data = value
	_active_lod = _clamp_lod_index(_active_lod)
	if object_data != null and not object_data.object_changed.is_connected(_on_object_changed):
		object_data.object_changed.connect(_on_object_changed, CONNECT_DEFERRED)
	rebuild()


func get_object_data() -> NovaObjectData:
	return object_data


func set_environment_node(value: Node) -> void:
	_environment_node = value
	_apply_environment_to_materials()


func get_model_bounds() -> AABB:
	return _model_bounds


func get_render_part_nodes() -> Dictionary:
	return _robj_nodes


func get_surface_material_indices() -> PackedInt32Array:
	return _surface_material_indices


func get_surface_materials() -> Array:
	return _surface_materials


func get_material_defs() -> Dictionary:
	return _material_defs


func is_playing() -> bool:
	return _is_playing


func set_playing(value: bool) -> void:
	_is_playing = value


func reset_animation_time() -> void:
	_anim_time_ms = 0
	_apply_runtime_state(0.0)


func get_animation_time_ms() -> int:
	return _anim_time_ms


func set_active_lod(lod_index: int) -> void:
	var next_lod := _clamp_lod_index(lod_index)
	if _active_lod == next_lod:
		return
	_active_lod = next_lod
	rebuild()


func get_active_lod() -> int:
	return _active_lod


func set_ctrl_value(name: String, value: int) -> void:
	if name.is_empty():
		return
	_ctrl_values[name] = clampi(value, 0, 65535)
	_apply_runtime_state(0.0)


func clear_ctrl_value(name: String) -> void:
	_ctrl_values.erase(name)
	_apply_runtime_state(0.0)


func clear_ctrl_values() -> void:
	_ctrl_values.clear()
	_apply_runtime_state(0.0)


func get_ctrl_values() -> Dictionary:
	return _ctrl_values.duplicate(true)


func rebuild() -> void:
	for child in get_children():
		remove_child(child)
		child.queue_free()
	_robj_nodes.clear()
	_skeleton_node = null
	_skeleton_skin = null
	_skinned_mesh_instances.clear()
	_surface_material_indices.clear()
	_surface_materials.clear()
	_anim_frames_by_mat.clear()
	_material_cache.clear()
	_material_defs.clear()
	if object_data == null or not object_data.has_document():
		_set_model_bounds(AABB())
		return

	_material_defs = _build_material_defs()
	_active_lod = _clamp_lod_index(_active_lod)
	var submeshes: Array = object_data.build_lod_submeshes(_active_lod) if object_data.has_method("build_lod_submeshes") else []
	if submeshes.is_empty():
		submeshes = _legacy_submeshes_from_surfaces(_active_lod)
	var has_skinned_meshes := false
	for entry in submeshes:
		if entry is Dictionary and bool((entry as Dictionary).get("is_skinned", false)):
			has_skinned_meshes = true
			break
	if has_skinned_meshes:
		_skeleton_node = _build_skeleton(_active_lod)
		if _skeleton_node != null:
			_skeleton_skin = _skeleton_node.create_skin_from_rest_transforms()
	for entry in submeshes:
		var submesh: Dictionary = entry
		var mesh := submesh.get("mesh") as ArrayMesh
		if mesh == null:
			continue
		var robj_index := int(submesh.get("robj_index", submesh.get("part_index", 0)))
		var material_index := int(submesh.get("material_index", 0))
		var instance := MeshInstance3D.new()
		instance.mesh = mesh
		var material := _material_for_index(material_index)
		instance.material_override = material
		if bool(submesh.get("is_skinned", false)) and _skeleton_node != null and _skeleton_skin != null:
			add_child(instance)
			instance.skeleton = instance.get_path_to(_skeleton_node)
			instance.skin = _skeleton_skin
			_skinned_mesh_instances.append(instance)
		else:
			var node := _get_or_create_robj_node(robj_index)
			node.add_child(instance)
		_surface_material_indices.append(material_index)
		_surface_materials.append(material)
		_collect_anim_frames(material_index)

	_apply_robj_transforms()
	_apply_runtime_state(0.0)
	_set_model_bounds(_compute_transformed_mesh_bounds())


func _on_object_changed() -> void:
	var update_mask := _last_object_update_mask()
	if update_mask == OED_UPDATE_PANM or update_mask == OED_UPDATE_LGHT or update_mask == (OED_UPDATE_PANM | OED_UPDATE_LGHT):
		_apply_runtime_state(0.0)
		return
	rebuild()


func _last_object_update_mask() -> int:
	if object_data != null and object_data.has_method("get_last_oed_update_mask"):
		return int(object_data.get_last_oed_update_mask()) & OED_UPDATE_ALL
	return OED_UPDATE_ALL


func _process(delta: float) -> void:
	_apply_runtime_state(delta)


func _clamp_lod_index(lod_index: int) -> int:
	if object_data == null or not object_data.has_document():
		return 0
	var summary: Dictionary = object_data.get_summary()
	var lod_count := int(summary.get("lod_count", 1))
	return clampi(lod_index, 0, maxi(lod_count - 1, 0))


func _build_material_defs() -> Dictionary:
	var result := {}
	for material in object_data.get_materials():
		var material_index := int(material.get("material_index", material.get("index", 0)))
		result[material_index] = material
		var array_index := int(material.get("index", material_index))
		if not result.has(array_index):
			result[array_index] = material
	return result


func _legacy_submeshes_from_surfaces(lod_index: int) -> Array:
	var result := []
	if object_data == null:
		return result
	for surface in object_data.get_lod_surfaces(lod_index):
		var mesh := ArrayMesh.new()
		var arrays := []
		arrays.resize(Mesh.ARRAY_MAX)
		arrays[Mesh.ARRAY_VERTEX] = surface.get("vertices", PackedVector3Array())
		arrays[Mesh.ARRAY_NORMAL] = surface.get("normals", PackedVector3Array())
		arrays[Mesh.ARRAY_TEX_UV] = surface.get("uvs", PackedVector2Array())
		arrays[Mesh.ARRAY_TEX_UV2] = surface.get("uvs2", PackedVector2Array())
		var tangents: PackedFloat32Array = surface.get("tangents", PackedFloat32Array())
		if tangents.size() == arrays[Mesh.ARRAY_VERTEX].size() * 4:
			arrays[Mesh.ARRAY_TANGENT] = tangents
		arrays[Mesh.ARRAY_INDEX] = surface.get("indices", PackedInt32Array())
		if arrays[Mesh.ARRAY_VERTEX].is_empty():
			continue
		mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
		result.append({
			"robj_index": int(surface.get("part_index", 0)),
			"material_index": int(surface.get("material_array_index", surface.get("material_index", 0))),
			"mesh": mesh,
		})
	return result


func _get_or_create_robj_node(robj_index: int) -> Node3D:
	if _robj_nodes.has(robj_index):
		return _robj_nodes[robj_index]
	var node := Node3D.new()
	node.name = "Robj_%d" % robj_index
	add_child(node)
	_robj_nodes[robj_index] = node
	return node


func _build_skeleton(lod_index: int) -> Skeleton3D:
	if object_data == null or not object_data.has_method("get_render_parts"):
		return null
	var parts: Array = object_data.get_render_parts(lod_index)
	if parts.is_empty():
		return null
	var skeleton := Skeleton3D.new()
	skeleton.name = "LwSkeleton"
	add_child(skeleton)
	for i in range(parts.size()):
		var part: Dictionary = parts[i]
		var part_index := int(part.get("index", i))
		skeleton.add_bone("Part_%d" % part_index)
	for i in range(parts.size()):
		var part: Dictionary = parts[i]
		var parent_index := int(part.get("parent_index", -1))
		if parent_index >= 0 and parent_index < parts.size() and parent_index != i:
			skeleton.set_bone_parent(i, parent_index)
	for i in range(parts.size()):
		var part: Dictionary = parts[i]
		var rel: Vector3 = part.get("rel", Vector3.ZERO)
		skeleton.set_bone_rest(i, Transform3D(Basis(), rel))
	skeleton.reset_bone_poses()
	skeleton.force_update_all_bone_transforms()
	return skeleton


func _compute_transformed_mesh_bounds() -> AABB:
	var bounds := AABB()
	var has_bounds := false
	var model_inverse := global_transform.affine_inverse()
	for robj_node in _robj_nodes.values():
		var node := robj_node as Node3D
		if node == null:
			continue
		for child in node.get_children():
			if child is MeshInstance3D:
				var instance := child as MeshInstance3D
				if instance.mesh == null:
					continue
				var mesh_aabb := instance.mesh.get_aabb()
				if mesh_aabb.size == Vector3.ZERO:
					continue
				var local_aabb: AABB = model_inverse * (instance.global_transform * mesh_aabb)
				bounds = local_aabb if not has_bounds else bounds.merge(local_aabb)
				has_bounds = true
	for instance in _skinned_mesh_instances:
		if instance == null or instance.mesh == null:
			continue
		var mesh_aabb := instance.mesh.get_aabb()
		if mesh_aabb.size == Vector3.ZERO:
			continue
		var local_aabb: AABB = model_inverse * (instance.global_transform * mesh_aabb)
		bounds = local_aabb if not has_bounds else bounds.merge(local_aabb)
		has_bounds = true
	return bounds


func _apply_runtime_state(delta: float) -> void:
	if object_data == null or not object_data.has_document():
		return
	if _is_playing:
		_anim_time_ms = (_anim_time_ms + int(delta * 1000.0)) & 0x7fffffff
	for i in range(_surface_materials.size()):
		var material := _surface_materials[i]
		if material == null:
			continue
		var material_index := int(_surface_material_indices[i])
		if object_data.has_method("eval_material_runtime"):
			var runtime: Dictionary = object_data.eval_material_runtime(material_index, _anim_time_ms, _ctrl_values)
			if not runtime.is_empty():
				material.set_shader_parameter("u_uv_offset", runtime.get("uv_offset", Vector2.ZERO))
				material.set_shader_parameter("u_uv_scale", runtime.get("uv_scale", Vector2.ONE))
				material.set_shader_parameter("u_uv_rotation", runtime.get("uv_rotation", 0.0))
				var rgb: Vector3 = runtime.get("rgb_mod", Vector3.ONE)
				material.set_shader_parameter("u_rgb_mod", rgb)
				material.set_shader_parameter("u_alpha_mod", runtime.get("alpha_mod", 1.0))
		var frames: Array = _anim_frames_by_mat.get(material_index, [])
		if frames.size() > 1 and object_data.has_method("compute_anim_frame"):
			var frame_index := int(object_data.compute_anim_frame(material_index, _anim_time_ms, _ctrl_values))
			if frame_index >= 0 and frame_index < frames.size() and frames[frame_index] is Texture2D:
				material.set_shader_parameter("u_diffuse", frames[frame_index])
	_apply_robj_transforms()
	_apply_lights()
	_apply_environment_to_materials()
	_set_model_bounds(_compute_transformed_mesh_bounds())


func _apply_robj_transforms() -> void:
	if object_data == null or not object_data.has_method("evaluate_panm"):
		return
	if _robj_nodes.is_empty() and _skeleton_node == null:
		return
	var transforms: Dictionary = object_data.evaluate_panm(_active_lod, _anim_time_ms, _ctrl_values)
	if _skeleton_node != null:
		for bone_index in range(_skeleton_node.get_bone_count()):
			if transforms.has(bone_index):
				_skeleton_node.set_bone_global_pose(bone_index, transforms[bone_index])
			else:
				_skeleton_node.reset_bone_pose(bone_index)
		_skeleton_node.force_update_all_bone_transforms()
	for key in transforms.keys():
		var robj_index := int(key)
		if _robj_nodes.has(robj_index):
			var node := _robj_nodes[robj_index] as Node3D
			node.transform = transforms[key]


func _apply_lights() -> void:
	if object_data == null or not object_data.has_method("evaluate_lights"):
		return
	var lights: Array = object_data.evaluate_lights(_anim_time_ms, _ctrl_values)
	var dominant := {}
	var best_intensity := -1.0
	for light in lights:
		var info: Dictionary = light
		var intensity := float(info.get("intensity", 1.0))
		if intensity > best_intensity:
			best_intensity = intensity
			dominant = info
	var count := 0 if dominant.is_empty() else 1
	var position: Vector3 = dominant.get("position", Vector3.ZERO)
	var color: Color = dominant.get("color", Color.WHITE)
	var atten_start := float(dominant.get("atten_start", 0.0))
	var atten_end := float(dominant.get("atten_end", 5.0))
	var subobject := int(dominant.get("subobject", -1))
	if subobject >= 0 and _robj_nodes.has(subobject):
		var node := _robj_nodes[subobject] as Node3D
		position = node.global_transform * position
	elif subobject >= 0 and _skeleton_node != null and subobject < _skeleton_node.get_bone_count():
		position = _skeleton_node.get_bone_global_pose(subobject) * position
	for material in _surface_materials:
		if material == null:
			continue
		material.set_shader_parameter("u_local_light_count", count)
		material.set_shader_parameter("u_local_light_position", position)
		material.set_shader_parameter("u_local_light_color", Vector3(color.r, color.g, color.b))
		material.set_shader_parameter("u_local_light_intensity", best_intensity if best_intensity > 0.0 else 1.0)
		material.set_shader_parameter("u_local_light_atten_start", atten_start)
		material.set_shader_parameter("u_local_light_atten_end", atten_end)


func _material_for_index(material_array_index: int) -> ShaderMaterial:
	if _material_cache.has(material_array_index):
		return _material_cache[material_array_index]
	var material_def: Dictionary = _material_defs.get(material_array_index, {})
	var material := _create_material(material_array_index, material_def)
	_material_cache[material_array_index] = material
	return material


func _create_material(index: int, material_def: Dictionary) -> ShaderMaterial:
	var material := ShaderMaterial.new()
	var material_index := int(material_def.get("index", index))
	var info := object_data.get_material_info(material_index) if object_data != null and material_index >= 0 and material_index < object_data.get_material_count() else {}
	var shader_tag := String(info.get("shader_tag", material_def.get("shader", "FF_ST_OP")))
	if shader_tag.is_empty():
		shader_tag = "FF_ST_OP"
	var material_flags := 0
	if bool(info.get("alpha_test_enabled", (int(material_def.get("flags", 0)) & MATERIAL_FLAG_ALPHA_TEST) != 0)):
		material_flags |= MATERIAL_FLAG_ALPHA_TEST
	if bool(info.get("alpha_invert", (int(material_def.get("flags", 0)) & MATERIAL_FLAG_ALPHA_INVERT) != 0)):
		material_flags |= MATERIAL_FLAG_ALPHA_INVERT
	if bool(info.get("two_sided", (int(material_def.get("flags", 0)) & MATERIAL_FLAG_TWO_SIDED) != 0)):
		material_flags |= MATERIAL_FLAG_TWO_SIDED
	var emissive_type := 2 if bool(info.get("emissive", false)) else int(material_def.get("emissive_type", 0))
	var is_glass_flag := 1 if bool(info.get("is_glass", material_def.get("is_glass", false))) else 0
	var alpha_test_byte := int(info.get("alpha_test", roundi(float(material_def.get("alpha_threshold", 0.0)) * 255.0)))
	var shader_cache := NovaObjectShaderCache.get_singleton()
	var key := shader_cache.classify(shader_tag, material_flags, emissive_type, is_glass_flag, alpha_test_byte)
	material.shader = shader_cache.get_shader_for_key(key)

	var diffuse := _load_texture_for_slot(material_def, 1)
	var detail := _load_texture_for_slot(material_def, 2)
	var normal := _load_texture_for_slot(material_def, 3)
	if normal == null:
		normal = _load_texture_for_slot(material_def, 4)
	if diffuse == null and detail != null:
		diffuse = detail
		detail = null
	if diffuse != null:
		material.set_shader_parameter("u_diffuse", diffuse)
	else:
		material.set_shader_parameter("u_diffuse", _solid_colour_texture(_hash_color_for_index(index)))
	if detail != null:
		material.set_shader_parameter("u_detail", detail)
	else:
		material.set_shader_parameter("u_detail", _solid_colour_texture(Color.WHITE))
	if normal != null:
		material.set_shader_parameter("u_normal_map", normal)
	else:
		material.set_shader_parameter("u_normal_map", _solid_colour_texture(Color(0.5, 0.5, 1.0, 1.0)))
	if (material_flags & MATERIAL_FLAG_ALPHA_TEST) != 0:
		material.set_shader_parameter("u_alpha_test_threshold", maxf(0.001, float(alpha_test_byte) / 255.0))
		material.set_shader_parameter("u_alpha_test_invert", 1.0 if (material_flags & MATERIAL_FLAG_ALPHA_INVERT) != 0 else 0.0)
	else:
		material.set_shader_parameter("u_alpha_test_threshold", 0.0)
		material.set_shader_parameter("u_alpha_test_invert", 0.0)
	var reflect: Color = info.get("reflect_color", Color(0.7, 0.8, 0.9, 0.35))
	material.set_shader_parameter("u_reflect_color", reflect)
	material.set_shader_parameter("u_uv_offset", Vector2.ZERO)
	material.set_shader_parameter("u_uv_scale", Vector2.ONE)
	material.set_shader_parameter("u_uv_rotation", 0.0)
	material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
	material.set_shader_parameter("u_alpha_mod", 1.0)
	material.set_shader_parameter("u_emissive", 1.0 if bool(info.get("emissive", false)) else 0.0)
	material.set_shader_parameter("u_local_light_count", 0)
	material.set_shader_parameter("u_local_light_position", Vector3.ZERO)
	material.set_shader_parameter("u_local_light_color", Vector3.ONE)
	material.set_shader_parameter("u_local_light_intensity", 1.0)
	material.set_shader_parameter("u_local_light_atten_start", 0.0)
	material.set_shader_parameter("u_local_light_atten_end", 5.0)
	_apply_default_environment_to_material(material)
	return material


func _load_texture_for_slot(material_def: Dictionary, slot: int) -> Texture2D:
	if object_data == null or material_def.is_empty():
		return null
	var textures: Array = material_def.get("textures", [])
	if textures.is_empty():
		return null

	var material_index := int(material_def.get("index", -1))
	if material_index < 0:
		return null

	for i in range(textures.size()):
		var texture: Dictionary = textures[i]
		if int(texture.get("slot", 0)) == slot:
			var loaded: Texture2D = object_data.load_material_texture(material_index, i)
			if loaded != null:
				return loaded
	return null


func _collect_anim_frames(material_index: int) -> void:
	if _anim_frames_by_mat.has(material_index) or object_data == null or not object_data.has_method("get_material_anim_frames"):
		return
	var frame_names: PackedStringArray = object_data.get_material_anim_frames(material_index, 1)
	if frame_names.size() <= 1:
		return
	var frames: Array = []
	for frame_name in frame_names:
		frames.append(_load_texture_name(frame_name))
	_anim_frames_by_mat[material_index] = frames


func _load_texture_name(texture_name: String) -> Texture2D:
	if object_data == null or texture_name.is_empty():
		return null
	return object_data.load_texture_name(texture_name)


func _hash_color_for_index(idx: int) -> Color:
	var h := fposmod(float(idx) * 0.61803398, 1.0)
	return Color.from_hsv(h, 0.35, 0.85)


func _solid_colour_texture(color: Color) -> ImageTexture:
	var image := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	image.set_pixel(0, 0, color)
	return ImageTexture.create_from_image(image)


func _apply_default_environment_to_material(material: ShaderMaterial) -> void:
	material.set_shader_parameter("u_ambient_color", DEFAULT_AMBIENT_COLOR)
	material.set_shader_parameter("u_dir_light_dir", DEFAULT_DIR_LIGHT_DIR)
	material.set_shader_parameter("u_dir_light_color", DEFAULT_DIR_LIGHT_COLOR)
	material.set_shader_parameter("u_fill_light_color", DEFAULT_FILL_LIGHT_COLOR)
	material.set_shader_parameter("u_fog_enabled", false)
	material.set_shader_parameter("u_fog_color", DEFAULT_FOG_COLOR)
	material.set_shader_parameter("u_fog_start", DEFAULT_FOG_START)
	material.set_shader_parameter("u_fog_end", DEFAULT_FOG_END)
	material.set_shader_parameter("u_fog_type", DEFAULT_FOG_TYPE)


func _apply_environment_to_materials() -> void:
	var values := _environment_values()
	for material in _surface_materials:
		if material == null:
			continue
		material.set_shader_parameter("u_ambient_color", values.get("ambient", DEFAULT_AMBIENT_COLOR))
		material.set_shader_parameter("u_dir_light_dir", values.get("dir", DEFAULT_DIR_LIGHT_DIR))
		material.set_shader_parameter("u_dir_light_color", values.get("dir_color", DEFAULT_DIR_LIGHT_COLOR))
		material.set_shader_parameter("u_fill_light_color", values.get("fill", DEFAULT_FILL_LIGHT_COLOR))
		material.set_shader_parameter("u_fog_enabled", bool(values.get("fog_enabled", false)))
		material.set_shader_parameter("u_fog_color", values.get("fog_color", DEFAULT_FOG_COLOR))
		material.set_shader_parameter("u_fog_start", float(values.get("fog_start", DEFAULT_FOG_START)))
		material.set_shader_parameter("u_fog_end", float(values.get("fog_end", DEFAULT_FOG_END)))
		material.set_shader_parameter("u_fog_type", int(values.get("fog_type", DEFAULT_FOG_TYPE)))


func _environment_values() -> Dictionary:
	if _environment_node == null or not _environment_node.has_method("is_loaded") or not _environment_node.call("is_loaded"):
		return {
			"ambient": DEFAULT_AMBIENT_COLOR,
			"dir": DEFAULT_DIR_LIGHT_DIR,
			"dir_color": DEFAULT_DIR_LIGHT_COLOR,
			"fill": DEFAULT_FILL_LIGHT_COLOR,
			"fog_enabled": false,
			"fog_color": DEFAULT_FOG_COLOR,
			"fog_start": DEFAULT_FOG_START,
			"fog_end": DEFAULT_FOG_END,
			"fog_type": DEFAULT_FOG_TYPE,
		}
	var sun_dir: Vector3 = _environment_node.call("get_sun_direction")
	if sun_dir.length() <= 0.001:
		sun_dir = -DEFAULT_DIR_LIGHT_DIR
	return {
		"ambient": _environment_node.call("get_sky_ambient"),
		"dir": -sun_dir.normalized(),
		"dir_color": _environment_node.call("get_sun_light"),
		"fill": _environment_node.call("get_fill_light"),
		"fog_enabled": true,
		"fog_color": _environment_node.call("get_fog_color"),
		"fog_start": _environment_node.call("get_fog_start"),
		"fog_end": _environment_node.call("get_fog_level"),
		"fog_type": _environment_node.call("get_fog_type"),
	}


func _set_model_bounds(bounds: AABB) -> void:
	if _aabb_equal_approx(_model_bounds, bounds):
		return
	_model_bounds = bounds
	bounds_changed.emit(_model_bounds)


func _aabb_equal_approx(a: AABB, b: AABB) -> bool:
	return a.position.is_equal_approx(b.position) and a.size.is_equal_approx(b.size)
