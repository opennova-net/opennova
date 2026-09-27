extends GutTest

# Raster pins for the skinned effects' vertex program (skin.gdshaderinc),
# drawn through the production wrappers on a real RenderingDevice (pending
# under the headless dummy renderer). Every retail skinned vertex shader
# blends the position over four palette entries, but only SkBasic and SkGlass
# blend the normal (skinnormal = true); the lit bump effects light the
# UNDEFORMED normal in the vertex's FIRST entry's frame and measure their
# point lights from the vertex that entry carries rigidly.
# [orig: _BaseInc.fx CalcSkinWorldPosAndNormal; _vsSkDfT.fx vsTanSkinDot3Dir;
#  _vsSkDfT.fx vsTanSkinDot3PointPS; SkBasic.fx vsSkinBasic;
#  CRenderBatchQueue_FlushBatches @ 0x5DA4F6..0x5DA5CE, @ 0x5DA950..0x5DA9A1]

const GLOBALS := [
	"opennova_light_block_dir", "opennova_light_block_dir_color",
	"opennova_light_block_hemi_sky", "opennova_light_block_hemi_ground",
	"opennova_light_block_gain", "opennova_thermal_view",
	"opennova_fog_enabled", "opennova_water_active",
	"opennova_environment_cube_ready",
]
const EYE := Vector3(0.0, 0.0, 5.0)
const BASE := 0.25
const HEMI := 0.1


func before_each() -> void:
	_global("opennova_thermal_view", false)
	_global("opennova_fog_enabled", false)
	_global("opennova_water_active", false)
	_global("opennova_environment_cube_ready", false)
	_global("opennova_light_block_gain", Vector3.ONE)
	# The light travels -X: the vector toward it is +X.
	_global("opennova_light_block_dir", Vector3(-1.0, 0.0, 0.0))
	_global("opennova_light_block_dir_color", Vector3.ONE)
	_global("opennova_light_block_hemi_sky", Vector3.ONE * HEMI)
	_global("opennova_light_block_hemi_ground", Vector3.ONE * HEMI)


func after_each() -> void:
	ShaderGlobals.restore_defaults(GLOBALS)


func _global(name: String, value: Variant) -> void:
	RenderingServer.global_shader_parameter_set(name, value)


func _rd_available() -> bool:
	return RenderingServer.get_rendering_device() != null


func _view() -> SubViewport:
	var viewport := SubViewport.new()
	viewport.size = Vector2i(64, 64)
	viewport.own_world_3d = true
	viewport.render_target_update_mode = SubViewport.UPDATE_ALWAYS
	add_child_autofree(viewport)
	var environment_resource := Environment.new()
	environment_resource.background_mode = Environment.BG_COLOR
	environment_resource.background_color = Color.BLACK
	environment_resource.ambient_light_source = Environment.AMBIENT_SOURCE_DISABLED
	environment_resource.glow_enabled = false
	var environment := WorldEnvironment.new()
	environment.environment = environment_resource
	viewport.add_child(environment)
	var camera := Camera3D.new()
	camera.projection = Camera3D.PROJECTION_ORTHOGONAL
	camera.size = 2.0
	camera.position = EYE
	camera.current = true
	viewport.add_child(camera)
	viewport.add_child(DisplayDecode.new())
	return viewport


func _solid(color: Color) -> ImageTexture:
	var image := Image.create(1, 1, true, Image.FORMAT_RGBA8)
	image.fill(color)
	return ImageTexture.create_from_image(image)


# The palette as ObjectModel publishes it: one row per bone, three RGBAF
# texels holding the rows of the bind-to-skeleton 3x4 matrix.
func _palette(matrices: Array[Transform3D]) -> ImageTexture:
	var floats := PackedFloat32Array()
	for matrix in matrices:
		for row in 3:
			floats.append(matrix.basis[0][row])
			floats.append(matrix.basis[1][row])
			floats.append(matrix.basis[2][row])
			floats.append(matrix.origin[row])
	var image := Image.create_from_data(3, matrices.size(), false, Image.FORMAT_RGBAF,
			floats.to_byte_array())
	return ImageTexture.create_from_image(image)


# A 2x2 quad facing the camera (+Z), every vertex riding `bones` at
# 0.5 / 0.5 (index byte 3 takes 1 - (w0 + w1 + w2) = 0). `fallbacks`, when
# given, is every vertex's light fallback chain (the CUSTOM0 bytes
# prepare_model_mesh packs: the parts before its first entry, 255 for none).
func _skinned_quad(material: ShaderMaterial, bones := PackedInt32Array([0, 1, 0, 0]),
		fallbacks := PackedByteArray()) -> MeshInstance3D:
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3(-1, 1, 0), Vector3(1, 1, 0), Vector3(1, -1, 0), Vector3(-1, -1, 0)])
	arrays[Mesh.ARRAY_NORMAL] = PackedVector3Array([
		Vector3.BACK, Vector3.BACK, Vector3.BACK, Vector3.BACK])
	arrays[Mesh.ARRAY_TEX_UV] = PackedVector2Array([
		Vector2(0, 0), Vector2(1, 0), Vector2(1, 1), Vector2(0, 1)])
	arrays[Mesh.ARRAY_TEX_UV2] = arrays[Mesh.ARRAY_TEX_UV]
	arrays[Mesh.ARRAY_TANGENT] = PackedFloat32Array([
		1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1, 1, 0, 0, 1])
	var vertex_bones := PackedInt32Array()
	var vertex_fallbacks := PackedByteArray()
	for _vertex in 4:
		vertex_bones.append_array(bones)
		vertex_fallbacks.append_array(fallbacks)
	arrays[Mesh.ARRAY_BONES] = vertex_bones
	arrays[Mesh.ARRAY_WEIGHTS] = PackedFloat32Array([
		0.5, 0.5, 0, 0, 0.5, 0.5, 0, 0, 0.5, 0.5, 0, 0, 0.5, 0.5, 0, 0])
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2, 0, 2, 3])
	var format := 0
	if not fallbacks.is_empty():
		arrays[Mesh.ARRAY_CUSTOM0] = vertex_fallbacks
		format = Mesh.ARRAY_CUSTOM_RGBA8_UNORM << Mesh.ARRAY_FORMAT_CUSTOM0_SHIFT
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays, [], {}, format)
	var instance := MeshInstance3D.new()
	instance.mesh = mesh
	instance.material_override = material
	instance.set_instance_shader_parameter("u_entity_light", Vector4(1.0, 0.0, 1.0, 0.0))
	return instance


func _material(wrapper: String, palette: ImageTexture) -> ShaderMaterial:
	var material := ShaderMaterial.new()
	material.shader = load("res://shaders/object/%s.gdshader" % wrapper) as Shader
	material.set_shader_parameter("u_diffuse", _solid(Color(BASE, BASE, BASE, 1.0)))
	material.set_shader_parameter("u_normal_map", _solid(Color(0.5, 0.5, 1.0, 1.0)))
	material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
	material.set_shader_parameter("u_alpha_mod", 1.0)
	if palette != null:
		material.set_shader_parameter("u_skin_palette", palette)
		material.set_shader_parameter("u_skin_palette_bound", true)
	return material


func _centre(viewport: SubViewport) -> Color:
	for _frame in 4:
		await get_tree().process_frame
	RenderingServer.force_draw(true)
	RenderingServer.force_sync()
	return viewport.get_texture().get_image().get_pixel(32, 32)


func _render(wrapper: String, palette: ImageTexture,
		configure := Callable()) -> Color:
	return await _render_material(_material(wrapper, palette), configure)


func _render_material(material: ShaderMaterial, configure := Callable(),
		bones := PackedInt32Array([0, 1, 0, 0]),
		fallbacks := PackedByteArray()) -> Color:
	var viewport := _view()
	var quad := _skinned_quad(material, bones, fallbacks)
	if configure.is_valid():
		configure.call(quad)
	viewport.add_child(quad)
	return await _centre(viewport)


# The material a key selects, configured by the production shader cache.
func _technique_material(key: int, palette: ImageTexture) -> ShaderMaterial:
	var material := ShaderMaterial.new()
	ObjectShaderCache.get_singleton().configure_material_for_key(material, key)
	material.set_shader_parameter("u_diffuse", _solid(Color(BASE, BASE, BASE, 1.0)))
	material.set_shader_parameter("u_detail", _solid(Color(0.5, 0.5, 0.5, 1.0)))
	material.set_shader_parameter("u_normal_map", _solid(Color(0.5, 0.5, 1.0, 1.0)))
	material.set_shader_parameter("u_rgb_mod", Vector3.ONE)
	material.set_shader_parameter("u_alpha_mod", 1.0)
	material.set_shader_parameter("u_skin_palette", palette)
	material.set_shader_parameter("u_skin_palette_bound", true)
	return material


# Bone 0 turns the quad's normal toward the light (+Z -> +X), bone 1 leaves it:
# the first bone's frame lights N.L = 1, the four-weight blend N.L = 0.707.
func _turned_first_bone() -> ImageTexture:
	return _palette([Transform3D(Basis(Vector3.UP, PI / 2.0), Vector3.ZERO),
			Transform3D.IDENTITY])


func test_lit_skinned_bump_effects_light_the_first_bones_frame() -> void:
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var pixel: Color = await _render("dot3_tangent_skinned/opaque_double_sided",
			_turned_first_bone())
	# SkBDiffT2's P0: 2 x Diffuse1 x (Gouraud hemisphere + DirLightColor x N.L)
	# with the flat normal map's N the first bone's.
	assert_almost_eq(pixel.r, BASE * (HEMI + 1.0) * 2.0, 0.02,
			"the first bone's frame lights the undeformed normal: %s" % pixel)
	assert_gt(absf(pixel.r - BASE * (HEMI + sqrt(0.5)) * 2.0), 0.08,
			"not the blended normal Godot's skinning would hand over: %s" % pixel)


func test_skbasic_blends_and_normalizes_the_normal() -> void:
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var pixel: Color = await _render("fixed_skinned/opaque_double_sided",
			_turned_first_bone())
	# vsSkinBasic (skinnormal = true): sat(hemi + N.L of the blended,
	# renormalized normal) per vertex, MODULATE2X.
	assert_almost_eq(pixel.r, BASE * minf(HEMI + sqrt(0.5), 1.0) * 2.0, 0.02,
			"SkBasic lights the blended normal: %s" % pixel)


func test_an_unbound_palette_draws_the_bind_pose() -> void:
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	# No palette (a model without a skeleton): the vertex as the mesh has it,
	# its normal facing away from the light.
	var pixel: Color = await _render("dot3_tangent_skinned/opaque_double_sided", null)
	assert_almost_eq(pixel.r, BASE * HEMI * 2.0, 0.02,
			"the bind vertex, lit by the hemisphere alone: %s" % pixel)


func test_point_lights_start_at_the_vertex_the_first_bone_carries() -> void:
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	_global("opennova_light_block_dir_color", Vector3.ZERO)
	# Bone 0 lifts the quad 1.0 toward the camera, bone 1 leaves it: the
	# blended quad sits at z 0.5, the first bone carries it to z 1.0. A point
	# light in the first bone's plane grazes that plane (N.L 0) and lights the
	# blended one.
	var palette := _palette([Transform3D(Basis.IDENTITY, Vector3(0.0, 0.0, 1.0)),
			Transform3D.IDENTITY])
	var light := func(quad: MeshInstance3D) -> void:
		quad.set_instance_shader_parameter("u_point_light_count", 1.0)
		quad.set_instance_shader_parameter("u_point_light_posr_0",
				Vector4(2.0, 0.0, 1.0, 0.0001))
		quad.set_instance_shader_parameter("u_point_light_color_0",
				Vector4(1.0, 1.0, 1.0, 100.0))
	var pixel: Color = await _render("phong_tangent_diffuse_skinned/opaque_double_sided",
			palette, light)
	assert_almost_eq(pixel.r, BASE * HEMI * 2.0, 0.02,
			"the grazing light adds nothing at the rigid first-bone vertex: %s" % pixel)


# Three palettes over the same vertices (bone 0 then bone 1 at 0.5 / 0.5):
# A turns the first bone toward the light, B keeps that first bone and tilts
# the second toward the ground, C swaps A's bones. A and B share the first
# bone's frame; A and C share the blend.
func _rule_palettes() -> Array[ImageTexture]:
	var toward := Transform3D(Basis(Vector3.UP, PI / 2.0), Vector3.ZERO)
	var tilted := Transform3D(Basis(Vector3.RIGHT, PI / 3.0), Vector3.ZERO)
	return [
		_palette([toward, Transform3D.IDENTITY]),
		_palette([toward, tilted]),
		_palette([Transform3D.IDENTITY, toward]),
	]


func test_every_skinned_technique_lights_the_frame_its_rule_names() -> void:
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	# A sky brighter than the ground makes the hemisphere read the normal's
	# height, the directional light its reach toward +X, and SkGlass's
	# reflection (the hemisphere without a published cube) both.
	_global("opennova_light_block_dir_color", Vector3.ONE * 0.5)
	_global("opennova_light_block_hemi_sky", Vector3.ONE * 0.8)
	_global("opennova_light_block_hemi_ground", Vector3.ZERO)
	var cache := ObjectShaderCache.get_singleton()
	var palettes := _rule_palettes()
	var checked := 0
	for tag in cache.get_known_shader_tags():
		var key: int = cache.classify(tag, ObjectShaderCache.MATERIAL_FLAG_TWO_SIDED, 0, 0, 128)
		var rule: String = cache.skin_normal_for_key(key)
		if rule == "none":
			continue
		var pixels: Array[Color] = []
		for palette in palettes:
			pixels.append(await _render_material(_technique_material(key, palette)))
		var shared: Color = pixels[1] if rule == "first_bone" else pixels[2]
		var apart: Color = pixels[2] if rule == "first_bone" else pixels[1]
		assert_almost_eq(pixels[0].r, shared.r, 0.02,
				"%s (%s): the frame the rule names lights alike: %s" % [tag, rule, pixels])
		assert_gt(absf(pixels[0].r - apart.r), 0.05,
				"%s (%s): the other frame lights apart: %s" % [tag, rule, pixels])
		checked += 1
	assert_eq(checked, 9, "every shipped skinned tag was drawn")


# The first entry collapsed (the right hand's zero-scale row): bone 0 has no
# inverse, so the fill keeps the nearest earlier table entry's. The vertex
# rides bone 0 and bone 2 (identity) at 0.5 / 0.5, a half-size quad at the
# collapsed joint's origin; bone 1, the table entry before bone 0, turns +Z
# toward the light. Without an earlier entry the buffer holds stale stack,
# which the port reads as the identity.
func _collapsed_first_entry() -> ImageTexture:
	return _palette([Transform3D(Basis(Vector3.ZERO, Vector3.ZERO, Vector3.ZERO), Vector3.ZERO),
			Transform3D(Basis(Vector3.UP, PI / 2.0), Vector3.ZERO),
			Transform3D.IDENTITY])


func test_a_collapsed_first_entry_lights_through_the_entry_before_it() -> void:
	if not _rd_available():
		pending("RenderingDevice unavailable under this Godot renderer")
		return
	var bones := PackedInt32Array([0, 2, 0, 0])
	for wrapper in ["dot3_tangent_skinned/opaque_double_sided",
			"phong_object_diffuse_skinned/opaque_double_sided"]:
		var kept: Color = await _render_material(
				_material(wrapper, _collapsed_first_entry()), Callable(), bones,
				PackedByteArray([1, 255, 255, 255]))
		var stale: Color = await _render_material(
				_material(wrapper, _collapsed_first_entry()), Callable(), bones,
				PackedByteArray([255, 255, 255, 255]))
		# The kept inverse turns the undeformed normal toward the light; the
		# collapsed entry's own normal is zero, a level hemisphere.
		assert_almost_eq(kept.r, BASE * (HEMI + 1.0) * 2.0, 0.02,
				"%s: lit through the entry before the collapsed one: %s" % [wrapper, kept])
		assert_almost_eq(stale.r, BASE * HEMI * 2.0, 0.02,
				"%s: no earlier entry, the identity frame: %s" % [wrapper, stale])
