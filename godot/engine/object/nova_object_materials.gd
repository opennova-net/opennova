extends RefCounted

# NovaObjectModel's material factory + classification and environment-
# lighting application (quality slice W4-6c, the merged W4-6a/6b verbatim-
# motion shape): _create_material and its texture/shadow-receiver helpers,
# the dynamic-material classifier, the EnvLightValues typed record, and
# the static env-value derivation the mission placer's static batches also
# consume (via the owner's static delegates). ALL state and constants stay
# on the composing NovaObjectModel, reached through `_m`; the statics use
# no instance state and read the owner's DEFAULT_* lighting registers via
# the NovaObjectModel class name. The owner is a Node3D -- manually
# managed, not refcounted -- so this plain back-reference cannot cycle.

var _m


func _init(model) -> void:
	_m = model


func _build_material_defs() -> Dictionary:
	var result := {}
	for material in _m.object_data.get_materials():
		var material_index := int(material.get("material_index", material.get("index", 0)))
		result[material_index] = material
		var array_index := int(material.get("index", material_index))
		if not result.has(array_index):
			result[array_index] = material
	return result


static func material_supports_projected_shadow_receiver(
		blend_mode: int, material_flags: int) -> bool:
	# The simple attenuation next-pass has no access to the source material's
	# alpha coverage or two-sided raster state. Applying it to those surfaces
	# would darken transparent cards/polygons or miss their back faces. Keep the
	# approximation on coverage-complete one-sided opaque surfaces only; exact
	# alpha-aware projection remains part of the retail tile-compositor work.
	return blend_mode == NovaObjectShaderCache.BLEND_OPAQUE \
			and (material_flags & (
				NovaObjectShaderCache.MATERIAL_FLAG_ALPHA_TEST
				| NovaObjectShaderCache.MATERIAL_FLAG_TWO_SIDED)) == 0


func _create_material(index: int, material_def: Dictionary) -> ShaderMaterial:
	var material := ShaderMaterial.new()
	var material_index := int(material_def.get("index", index))
	var info: Dictionary = _m.object_data.get_material_info(material_index) if _m.object_data != null and material_index >= 0 and material_index < _m.object_data.get_material_count() else {}
	var shader_tag := String(info.get("shader_tag", material_def.get("shader", "FF_ST_OP")))
	if shader_tag.is_empty():
		shader_tag = "FF_ST_OP"
	var material_flags := 0
	if bool(info.get("alpha_test_enabled", (int(material_def.get("flags", 0)) & NovaObjectShaderCache.MATERIAL_FLAG_ALPHA_TEST) != 0)):
		material_flags |= NovaObjectShaderCache.MATERIAL_FLAG_ALPHA_TEST
	if bool(info.get("alpha_invert", (int(material_def.get("flags", 0)) & NovaObjectShaderCache.MATERIAL_FLAG_ALPHA_INVERT) != 0)):
		material_flags |= NovaObjectShaderCache.MATERIAL_FLAG_ALPHA_INVERT
	if bool(info.get("two_sided", (int(material_def.get("flags", 0)) & NovaObjectShaderCache.MATERIAL_FLAG_TWO_SIDED) != 0)):
		material_flags |= NovaObjectShaderCache.MATERIAL_FLAG_TWO_SIDED
	var emissive_type := 2 if bool(info.get("emissive", false)) else int(material_def.get("emissive_type", 0))
	var is_glass_flag := 1 if bool(info.get("is_glass", material_def.get("is_glass", false))) else 0
	var alpha_test_byte := int(info.get("alpha_test", roundi(float(material_def.get("alpha_threshold", 0.0)) * 255.0)))
	var shader_cache := NovaObjectShaderCache.get_singleton()

	# Textures resolve before the shader key: the detail stage only survives
	# classification when the secondary texture actually resolved (below).
	var diffuse := _load_texture_for_slot(material_def, 1)
	var detail := _load_texture_for_slot(material_def, 2)
	var normal := _load_texture_for_slot(material_def, 3)
	if normal == null:
		normal = _load_texture_for_slot(material_def, 4)
	if diffuse == null and detail != null:
		diffuse = detail
		detail = null

	var key := shader_cache.classify(shader_tag, material_flags, emissive_type, is_glass_flag, alpha_test_byte)
	if detail == null:
		# Retail runs the _MT second stage only with its texture bound — a
		# NULL-texture stage is dropped. An unresolved secondary therefore
		# composes the no-detail shader: identical output to no stage at all,
		# never the Modulate2x stage over a placeholder
		# (render-material-re.md §FF technique tables).
		key &= ~NovaObjectShaderCache.CAP_DETAIL
	material.shader = shader_cache.get_shader_for_key(key)
	var blend_mode := shader_cache.blend_for_key(key)
	if blend_mode != NovaObjectShaderCache.BLEND_OPAQUE:
		# Water-side rung applied by refresh_render_order() once placed.
		_m._alpha_materials.append(material)

	if diffuse != null:
		material.set_shader_parameter("u_diffuse", diffuse)
	else:
		material.set_shader_parameter("u_diffuse", _solid_colour_texture(_hash_color_for_index(index)))
	if detail != null:
		# The _MT secondary map, same resolver path as the diffuse (slot 2 =
		# the material record's second texture — OED's SECONDARY slot).
		material.set_shader_parameter("u_detail", detail)
	if normal != null:
		material.set_shader_parameter("u_normal_map", normal)
	else:
		material.set_shader_parameter("u_normal_map", _solid_colour_texture(Color(0.5, 0.5, 1.0, 1.0)))
	if (material_flags & NovaObjectShaderCache.MATERIAL_FLAG_ALPHA_TEST) != 0:
		## The ref byte feeds the compare exactly; the shader keeps a > ref
		## (invert: a <= ref), so no epsilon fudge is needed for ref 0.
		## [orig: CGfxDevice_SetAlphaTestRef @ 0x6770a0]
		material.set_shader_parameter("u_alpha_test_threshold", float(alpha_test_byte) / 255.0)
		material.set_shader_parameter("u_alpha_test_invert", 1.0 if (material_flags & NovaObjectShaderCache.MATERIAL_FLAG_ALPHA_INVERT) != 0 else 0.0)
	else:
		material.set_shader_parameter("u_alpha_test_threshold", 0.0)
		material.set_shader_parameter("u_alpha_test_invert", 0.0)
	var reflect: Color = info.get("reflect_color", Color(0.7, 0.8, 0.9, 0.35))
	material.set_shader_parameter("u_reflect_color", reflect)
	# The PANM evaluator supplies the complete two-row affine transform, including
	# the coupled off-diagonal terms retail's style 113..117 paths can produce.
	material.set_shader_parameter("u_uv_transform_u", Vector3(1.0, 0.0, 0.0))
	material.set_shader_parameter("u_uv_transform_v", Vector3(0.0, 1.0, 0.0))
	material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
	material.set_shader_parameter("u_alpha_mod", 1.0)
	material.set_shader_parameter("u_emissive", 1.0 if bool(info.get("emissive", false)) else 0.0)
	material.set_shader_parameter("u_local_light_count", 0)
	material.set_shader_parameter("u_local_light_position", Vector3.ZERO)
	material.set_shader_parameter("u_local_light_color", Vector3.ONE)
	material.set_shader_parameter("u_local_light_intensity", 1.0)
	material.set_shader_parameter("u_local_light_atten_start", 0.0)
	material.set_shader_parameter("u_local_light_atten_end", 5.0)
	if material_supports_projected_shadow_receiver(
			blend_mode, material_flags):
		material.next_pass = _get_shadow_receiver_material()
	_apply_default_environment_to_material(material)
	return material


func _get_shadow_receiver_material() -> ShaderMaterial:
	if _m._shadow_receiver_material == null:
		_m._shadow_receiver_material = ShaderMaterial.new()
		_m._shadow_receiver_material.shader = _m.SUN_SHADOW_CATCHER_SHADER
	return _m._shadow_receiver_material


func _load_texture_for_slot(material_def: Dictionary, slot: int) -> Texture2D:
	if _m.object_data == null or material_def.is_empty():
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
			var loaded: Texture2D = _m.object_data.load_material_texture(material_index, i)
			if loaded != null:
				return loaded
	return null


func _collect_anim_frames(material_index: int) -> void:
	if _m._anim_frames_by_mat.has(material_index) or _m.object_data == null:
		return
	var frame_names: PackedStringArray = _m.object_data.get_material_anim_frames(material_index, 1)
	if frame_names.size() <= 1:
		return
	var frames: Array = []
	for frame_name in frame_names:
		frames.append(_load_texture_name(frame_name))
	_m._anim_frames_by_mat[material_index] = frames


func _load_texture_name(texture_name: String) -> Texture2D:
	if _m.object_data == null or texture_name.is_empty():
		return null
	return _m.object_data.load_texture_name(texture_name)


func _hash_color_for_index(idx: int) -> Color:
	var h := IndexHue.hue_for_index(idx)
	return Color.from_hsv(h, 0.35, 0.85)


func _solid_colour_texture(color: Color) -> ImageTexture:
	var image := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	image.set_pixel(0, 0, color)
	return ImageTexture.create_from_image(image)


func _apply_default_environment_to_material(material: ShaderMaterial) -> void:
	apply_environment_values(material, environment_values_from(null))


# A surface material needs per-frame UV/RGB/alpha evaluation only if one of its generators
# animates. The classifier is conservative: any non-zero generator style counts as dynamic --
# it can only over-evaluate, never freeze an animation (a fully-static material's eval is
# the identity that _create_material already set).
func _material_runtime_is_dynamic(material_index: int) -> bool:
	if _m.object_data == null:
		return true
	var info: Dictionary = _m.object_data.get_material_info(material_index)
	if info.is_empty():
		return true
	return int(info.get("uv_u_style", 0)) != 0 \
		or int(info.get("uv_v_style", 0)) != 0 \
		or int(info.get("rgb_gen_style", 0)) != 0 \
		or int(info.get("alpha_gen_style", 0)) != 0


# Partition the surface materials into those that change at runtime (UV/RGB/alpha generators
# or a multi-frame texture animation) and the static remainder. Only the dynamic slots are
# visited per frame; static slots keep the identity values written at material creation.
func _classify_materials() -> void:
	_m._material_needs_eval.clear()
	var dynamic_slots := PackedInt32Array()
	var kind_cache: Dictionary = {}
	for i in range(_m._surface_materials.size()):
		var material_index := int(_m._surface_material_indices[i])
		var needs_eval: bool
		if kind_cache.has(material_index):
			needs_eval = bool(kind_cache[material_index])
		else:
			needs_eval = _material_runtime_is_dynamic(material_index)
			kind_cache[material_index] = needs_eval
		_m._material_needs_eval.append(needs_eval)
		var frames: Array = _m._anim_frames_by_mat.get(material_index, [])
		if needs_eval or frames.size() > 1:
			dynamic_slots.append(i)
	_m._dynamic_material_slots = dynamic_slots


# ADR 0017 typed record: the env-derived lighting/fog values the object
# shaders consume — computed once per env change and stamped onto many
# materials (live model surfaces AND the mission placer's static batches, so
# batched world objects relight from the SAME values/skip logic as live
# models; retail relights every entity from the current lighting block each
# frame [orig: setup_entity_lighting_and_shader_constants @ 0x5d98a0]).
# Fields are always assigned by environment_values_from().
class EnvLightValues:
	extends RefCounted
	var hemi_sky: Vector3
	var dir: Vector3
	var dir_color: Vector3
	var hemi_ground: Vector3
	var ceiling: Vector3
	var floor: Vector3
	var gain: Vector3
	var fog_enabled: bool
	var fog_color: Vector3
	var fog_start: float
	var fog_end: float
	var fog_type: int

	# True when `other` carries the same lighting/fog the shaders consume.
	# Colours compare with is_equal_approx (the weather smoother quantises to
	# 8-bit, so real changes are >= 1/255, far above epsilon); a null other
	# (first push after rebuild) is never equal, forcing the initial push.
	func equals(other: EnvLightValues) -> bool:
		if other == null:
			return false
		return hemi_sky.is_equal_approx(other.hemi_sky) \
			and dir.is_equal_approx(other.dir) \
			and dir_color.is_equal_approx(other.dir_color) \
			and hemi_ground.is_equal_approx(other.hemi_ground) \
			and ceiling.is_equal_approx(other.ceiling) \
			and floor.is_equal_approx(other.floor) \
			and gain.is_equal_approx(other.gain) \
			and fog_enabled == other.fog_enabled \
			and fog_color.is_equal_approx(other.fog_color) \
			and is_equal_approx(fog_start, other.fog_start) \
			and is_equal_approx(fog_end, other.fog_end) \
			and fog_type == other.fog_type


static func environment_values_from(env_node: Node) -> EnvLightValues:
	var v := EnvLightValues.new()
	if env_node == null or not env_node.has_method("is_loaded") or not env_node.call("is_loaded"):
		v.hemi_sky = NovaObjectModel.DEFAULT_HEMI_SKY_COLOR
		v.dir = NovaObjectModel.DEFAULT_DIR_LIGHT_DIR
		v.dir_color = NovaObjectModel.DEFAULT_DIR_LIGHT_COLOR
		v.hemi_ground = NovaObjectModel.DEFAULT_HEMI_GROUND_COLOR
		v.ceiling = NovaObjectModel.DEFAULT_HEMI_SKY_COLOR
		v.floor = NovaObjectModel.DEFAULT_HEMI_GROUND_COLOR
		v.gain = NovaObjectModel.DEFAULT_COLOR_SRC_GAIN
		v.fog_enabled = false
		v.fog_color = NovaObjectModel.DEFAULT_FOG_COLOR
		v.fog_start = NovaObjectModel.DEFAULT_FOG_START
		v.fog_end = NovaObjectModel.DEFAULT_FOG_END
		v.fog_type = NovaObjectModel.DEFAULT_FOG_TYPE
		return v
	# Select sun or moon before the object-material normalization seam
	# [orig: Environment_GetLightDirectionFloat @ 0x57d870].
	var light_dir: Vector3 = env_node.call("get_light_direction")
	if light_dir.length() <= 0.001:
		light_dir = -NovaObjectModel.DEFAULT_DIR_LIGHT_DIR
	var gain: Vector3 = NovaObjectModel.DEFAULT_COLOR_SRC_GAIN
	if env_node.has_method("get_color_src_gain"):
		gain = env_node.call("get_color_src_gain")
	v.hemi_sky = env_node.call("get_sky_ambient")
	v.dir = -light_dir.normalized()
	v.dir_color = env_node.call("get_sun_light")
	v.hemi_ground = env_node.call("get_fill_light")
	v.ceiling = env_node.call("get_ceiling_color")
	v.floor = env_node.call("get_floor_color")
	v.gain = gain
	v.fog_enabled = true
	v.fog_color = env_node.call("get_fog_color")
	v.fog_start = env_node.call("get_fog_start")
	v.fog_end = env_node.call("get_fog_level")
	v.fog_type = env_node.call("get_fog_type")
	return v


static func entity_lighting_values(world_values: EnvLightValues, effect_scale: float,
		interior_lerp: bool, interior_daylight: float) -> EnvLightValues:
	if world_values == null:
		return null
	var v := EnvLightValues.new()
	v.dir = world_values.dir
	v.dir_color = world_values.dir_color * clampf(effect_scale, 0.0, 1.0)
	v.hemi_sky = world_values.hemi_sky
	v.hemi_ground = world_values.hemi_ground
	v.ceiling = world_values.ceiling
	v.floor = world_values.floor
	v.gain = world_values.gain
	v.fog_enabled = world_values.fog_enabled
	v.fog_color = world_values.fog_color
	v.fog_start = world_values.fog_start
	v.fog_end = world_values.fog_end
	v.fog_type = world_values.fog_type
	if interior_lerp:
		var transfer := clampf(interior_daylight, 0.0, 1.0)
		v.dir_color *= transfer
		v.hemi_ground = world_values.floor.lerp(world_values.hemi_ground, transfer)
		v.hemi_sky = world_values.ceiling.lerp(world_values.hemi_sky, transfer)
	return v


static func apply_environment_values(material: ShaderMaterial, values: EnvLightValues) -> void:
	if material == null or values == null:
		return
	material.set_shader_parameter("u_hemi_sky_color", values.hemi_sky)
	material.set_shader_parameter("u_dir_light_dir", values.dir)
	material.set_shader_parameter("u_dir_light_color", values.dir_color)
	material.set_shader_parameter("u_hemi_ground_color", values.hemi_ground)
	material.set_shader_parameter("u_color_src_global_gain", values.gain)
	material.set_shader_parameter("u_fog_enabled", values.fog_enabled)
	material.set_shader_parameter("u_fog_color", values.fog_color)
	material.set_shader_parameter("u_fog_start", values.fog_start)
	material.set_shader_parameter("u_fog_end", values.fog_end)
	material.set_shader_parameter("u_fog_type", values.fog_type)


func _apply_environment_to_materials() -> void:
	# The environment is shared and changes slowly (time-of-day) or not at all. NovaWeather
	# re-stamps it every frame, but the smoothed colours quantise to identical bytes once
	# settled, so the 9 values these materials consume are byte-stable in steady state. Skip
	# the 9 cross-language reads + 9-uniform-per-material push when nothing changed since the
	# last push: a NovaEnvironment generation makes the steady-state check a single int
	# compare; the value cache is the fallback for env nodes without one. Either way the skip
	# only ever omits re-pushing identical uniforms (retained mode -> invisible), so the
	# rendered lighting/fog is byte-identical to pushing every frame.
	var gen := -1
	if _m._env_has_generation:
		gen = int(_m._environment_node.get_env_generation())
		var have_all_cached: bool = _m._last_env_values != null \
				and (not _m._interior_section_lighting
					or _m._last_section_env_values != null)
		if gen == _m._last_env_gen and have_all_cached:
			return
	var world_values := environment_values_from(_m._environment_node)
	# A portal building is not an ordinary entity submission: its exterior
	# shell always keeps effectScale 1, and only ROBJ 1+ takes its own ItemDef
	# transfer. Make that invariant authoritative here so a generic entity
	# update cannot accidentally dim the shell.
	var values := entity_lighting_values(
			world_values,
			1.0 if _m._interior_section_lighting else _m._lighting_effect_scale,
			false if _m._interior_section_lighting else _m._interior_lerp,
			0.0 if _m._interior_section_lighting else _m._interior_daylight)
	var section_values: EnvLightValues = null
	if _m._interior_section_lighting:
		section_values = entity_lighting_values(
				world_values,
				1.0,
				true,
				_m._interior_section_daylight)
	var entity_unchanged := values.equals(_m._last_env_values)
	var section_unchanged: bool = not _m._interior_section_lighting \
			or section_values.equals(_m._last_section_env_values)
	if entity_unchanged and section_unchanged:
		_m._last_env_gen = gen
		return
	_m._last_env_values = values
	_m._last_section_env_values = section_values
	_m._last_env_gen = gen
	for i in range(_m._surface_materials.size()):
		var material: ShaderMaterial = _m._surface_materials[i]
		if material == null:
			continue
		var context: int = int(_m._surface_lighting_contexts[i]) \
				if i < _m._surface_lighting_contexts.size() \
				else _m.LIGHTING_CONTEXT_ENTITY
		apply_environment_values(
				material,
				section_values
					if context == _m.LIGHTING_CONTEXT_INTERIOR_SECTION
					else values)


# The witnessed block mapping: dir_color <- the light block (sun/moon),
# hemi_sky <- the sky block, hemi_ground <- the ground block, gain <- the
# modulator /64 (the iris exposure reaching self-lit surfaces)
# [orig: CTerrainRenderer_BuildLightingShaderConstants @ 0x5c8090;
#  ColorSrcGlobalGain bind @ 0x58e05d].
func _environment_values() -> EnvLightValues:
	return entity_lighting_values(
			environment_values_from(_m._environment_node),
			_m._lighting_effect_scale,
			_m._interior_lerp,
			_m._interior_daylight)
