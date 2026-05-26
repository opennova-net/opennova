class_name ObjectPreview
extends Control

const FlyCameraScript = preload("res://engine/fly_camera.gd")

const MATERIAL_FLAG_ALPHA_TEST := 0x01
const MATERIAL_FLAG_ALPHA_INVERT := 0x02
const MATERIAL_FLAG_TWO_SIDED := 0x04

var object_data: NovaObjectData

var _viewport_container: SubViewportContainer
var _viewport: SubViewport
var _root: Node3D
var _mesh_root: Node3D
var _camera: Camera3D
var _grid_material: StandardMaterial3D
var _material_cache: Dictionary = {}
var _material_defs: Dictionary = {}
var _robj_nodes: Dictionary = {}
var _surface_material_indices: PackedInt32Array = PackedInt32Array()
var _surface_materials: Array[ShaderMaterial] = []
var _anim_frames_by_mat: Dictionary = {}
var _ctrl_values: Dictionary = {}
var _anim_time_ms: int = 0
var _active_lod: int = 0
var _is_playing := true
var _wireframe := false
var _has_framed := false


func _ready() -> void:
	mouse_filter = Control.MOUSE_FILTER_STOP
	clip_contents = true
	_build_viewport()
	_rebuild_mesh()


func set_object_data(value: NovaObjectData) -> void:
	if object_data != null and object_data.object_changed.is_connected(_on_object_changed):
		object_data.object_changed.disconnect(_on_object_changed)
	if object_data != value:
		_has_framed = false
	object_data = value
	_active_lod = _clamp_lod_index(_active_lod)
	if object_data != null and not object_data.object_changed.is_connected(_on_object_changed):
		object_data.object_changed.connect(_on_object_changed)
	_rebuild_mesh()


func _build_viewport() -> void:
	_viewport_container = SubViewportContainer.new()
	_viewport_container.set_anchors_preset(Control.PRESET_FULL_RECT)
	_viewport_container.stretch = true
	_viewport_container.mouse_filter = Control.MOUSE_FILTER_STOP
	add_child(_viewport_container)

	_viewport = SubViewport.new()
	_viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	_viewport.transparent_bg = false
	_viewport.handle_input_locally = true
	_viewport_container.add_child(_viewport)

	_root = Node3D.new()
	_viewport.add_child(_root)

	_mesh_root = Node3D.new()
	_root.add_child(_mesh_root)

	_camera = FlyCameraScript.new()
	_camera.current = true
	_camera.fov = 42.0
	_camera.near = 0.02
	_camera.far = 500.0
	_camera.look_at_from_position(Vector3(0.0, 1.5, 6.0), Vector3.ZERO)
	_root.add_child(_camera)

	_grid_material = StandardMaterial3D.new()
	_grid_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_grid_material.vertex_color_use_as_albedo = true
	_grid_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	set_process(true)


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
	_has_framed = false
	_rebuild_mesh()


func get_active_lod() -> int:
	return _active_lod


func is_wireframe() -> bool:
	return _wireframe


func set_wireframe(value: bool) -> void:
	_wireframe = value
	if _viewport != null:
		_viewport.debug_draw = Viewport.DEBUG_DRAW_WIREFRAME if value else Viewport.DEBUG_DRAW_DISABLED


func set_ctrl_value(name: String, value: int) -> void:
	if name.is_empty():
		return
	_ctrl_values[name] = clampi(value, 0, 65535)


func clear_ctrl_value(name: String) -> void:
	_ctrl_values.erase(name)


func clear_ctrl_values() -> void:
	_ctrl_values.clear()


func get_ctrl_values() -> Dictionary:
	return _ctrl_values.duplicate(true)


func _on_object_changed() -> void:
	_rebuild_mesh()


func _process(delta: float) -> void:
	_apply_runtime_state(delta)


func _clamp_lod_index(lod_index: int) -> int:
	if object_data == null or not object_data.has_document():
		return 0
	var summary: Dictionary = object_data.get_summary()
	var lod_count := int(summary.get("lod_count", 1))
	return clampi(lod_index, 0, maxi(lod_count - 1, 0))


func _rebuild_mesh() -> void:
	if _mesh_root == null:
		return
	for child in _mesh_root.get_children():
		child.queue_free()
	_mesh_root.position = Vector3.ZERO
	_robj_nodes.clear()
	_surface_material_indices.clear()
	_surface_materials.clear()
	_anim_frames_by_mat.clear()
	if object_data == null or not object_data.has_document():
		return

	_material_cache.clear()
	_material_defs = _build_material_defs()
	_active_lod = _clamp_lod_index(_active_lod)
	var submeshes: Array = object_data.build_lod_submeshes(_active_lod) if object_data.has_method("build_lod_submeshes") else []
	if submeshes.is_empty():
		submeshes = _legacy_submeshes_from_surfaces(_active_lod)
	for entry in submeshes:
		var submesh: Dictionary = entry
		var mesh := submesh.get("mesh") as ArrayMesh
		if mesh == null:
			continue
		var robj_index := int(submesh.get("robj_index", submesh.get("part_index", 0)))
		var material_index := int(submesh.get("material_index", 0))
		var node := _get_or_create_robj_node(robj_index)
		var instance := MeshInstance3D.new()
		instance.mesh = mesh
		var material := _material_for_index(material_index)
		instance.material_override = material
		node.add_child(instance)
		_surface_material_indices.append(material_index)
		_surface_materials.append(material)
		_collect_anim_frames(material_index)

	_apply_robj_transforms()
	var bounds := _compute_transformed_mesh_bounds()
	var has_bounds := bounds.size != Vector3.ZERO
	if has_bounds:
		_add_grid(bounds)
		_frame_bounds(bounds)


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
	_mesh_root.add_child(node)
	_robj_nodes[robj_index] = node
	return node


func _compute_transformed_mesh_bounds() -> AABB:
	var bounds := AABB()
	var has_bounds := false
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
				var world_aabb: AABB = instance.global_transform * mesh_aabb
				bounds = world_aabb if not has_bounds else bounds.merge(world_aabb)
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


func _apply_robj_transforms() -> void:
	if object_data == null or not object_data.has_method("evaluate_panm") or _robj_nodes.is_empty():
		return
	var transforms: Dictionary = object_data.evaluate_panm(_active_lod, _anim_time_ms, _ctrl_values)
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
	for material in _surface_materials:
		if material == null:
			continue
		material.set_shader_parameter("u_local_light_count", count)
		material.set_shader_parameter("u_local_light_position", position)
		material.set_shader_parameter("u_local_light_color", Vector3(color.r, color.g, color.b))
		material.set_shader_parameter("u_local_light_intensity", best_intensity if best_intensity > 0.0 else 1.0)
		material.set_shader_parameter("u_local_light_atten_start", atten_start)
		material.set_shader_parameter("u_local_light_atten_end", atten_end)


func _material_for_surface(surface: Dictionary) -> ShaderMaterial:
	var material_array_index := int(surface.get("material_array_index", surface.get("material_index", 0)))
	return _material_for_index(material_array_index)


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


func _add_grid(bounds: AABB) -> void:
	var vertices := PackedVector3Array()
	var colors := PackedColorArray()
	var min_x := bounds.position.x
	var max_x := bounds.end.x
	var min_z := bounds.position.z
	var max_z := bounds.end.z
	var half: float = maxf(1.0, maxf(maxf(absf(min_x), absf(max_x)), maxf(absf(min_z), absf(max_z))))
	var step: float = _grid_step_for_extent(half)
	var limit: float = ceilf(half / step + 1.0) * step
	var line_count := int(roundf(limit / step))

	for i in range(-line_count, line_count + 1):
		var p := float(i) * step
		var axis_x := is_zero_approx(p)
		_push_grid_line(vertices, colors, Vector3(p, 0.0, -limit), Vector3(p, 0.0, limit), axis_x)
		_push_grid_line(vertices, colors, Vector3(-limit, 0.0, p), Vector3(limit, 0.0, p), axis_x)

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = vertices
	arrays[Mesh.ARRAY_COLOR] = colors
	var grid_mesh := ArrayMesh.new()
	grid_mesh.add_surface_from_arrays(Mesh.PRIMITIVE_LINES, arrays)
	var grid := MeshInstance3D.new()
	grid.name = "ObjectGrid"
	grid.mesh = grid_mesh
	grid.material_override = _grid_material
	_mesh_root.add_child(grid)


func _push_grid_line(vertices: PackedVector3Array, colors: PackedColorArray, a: Vector3, b: Vector3, axis: bool) -> void:
	var color := Color(0.80, 0.86, 0.90, 0.72) if axis else Color(0.38, 0.43, 0.48, 0.34)
	vertices.push_back(a)
	vertices.push_back(b)
	colors.push_back(color)
	colors.push_back(color)


func _grid_step_for_extent(half_extent: float) -> float:
	if half_extent <= 4.0:
		return 0.5
	if half_extent <= 16.0:
		return 1.0
	if half_extent <= 64.0:
		return 4.0
	return 16.0


func _frame_bounds(bounds: AABB) -> void:
	var center := bounds.get_center()
	var radius := bounds.size.length() * 0.5
	if radius < 1.0:
		radius = 1.0
	_mesh_root.position = -center
	_camera.near = clampf(radius * 0.001, 0.02, 5.0)
	_camera.far = maxf(radius * 12.0, 50.0)
	_camera.set("fly_speed", clampf(radius * 2.5, 1.0, 250.0))
	_camera.set("zoom_speed", clampf(radius * 0.18, 0.05, 20.0))
	_camera.set("pan_sensitivity", clampf(radius * 0.01, 0.01, 1.0))
	if not _has_framed:
		_camera.call("frame_bounds_custom", Vector3.ZERO, radius, 1.5, maxf(radius * 8.0, 6.0), 2.8, -0.18)
		_has_framed = true
