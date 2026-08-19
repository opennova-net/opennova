extends GutTest


func test_surface_inputs_are_registered_for_runtime_and_oned() -> void:
	assert_true(ClassDB.class_exists("TerrainSurfaceInputs"),
		"Runtime terrain preprocessing must be available to ONED through a registered shared object.")


func test_cpu_source_image_replacements_invalidate_live_terrain_inputs() -> void:
	var data := TerrainData.new()
	var changed_count := [0]
	data.terrain_changed.connect(func() -> void: changed_count[0] += 1)
	var revisions := [data.get_change_revision()]

	data.set_heightmap_image(_solid_height_image(4, 2.0))
	revisions.push_back(data.get_change_revision())
	data.set_colormap_image(_solid_image(Color8(90, 110, 70, 255), 4))
	revisions.push_back(data.get_change_revision())
	data.set_blendmap_image(_solid_image(Color8(128, 64, 64, 200), 4))
	revisions.push_back(data.get_change_revision())

	assert_eq(changed_count[0], 3,
		"Every CPU source replacement must invalidate derived inputs and resident tile pages.")
	assert_gt(revisions[1], revisions[0])
	assert_gt(revisions[2], revisions[1])
	assert_gt(revisions[3], revisions[2])
	assert_ne(revisions[3], 0,
		"zero remains the never-observed sentinel across terrain revision wrap")


func test_full_rebuild_produces_retail_surface_inputs_from_live_data() -> void:
	var data := _make_surface_data()
	var inputs := TerrainSurfaceInputs.new()

	assert_true(inputs.rebuild(data))
	assert_true(inputs.has_normalized_blend())
	assert_true(inputs.has_detail_coefficient())
	assert_true(inputs.has_detail2())
	assert_true(inputs.has_heightfield_normal())
	for layer in 3:
		assert_true(inputs.has_paired_detail(layer))

	var blend_bytes := inputs.get_normalized_blend_texture().get_image().get_data()
	assert_eq(int(blend_bytes[0]), 127, "Retail integer normalization should truncate red to 127.")
	assert_eq(int(blend_bytes[1]), 63, "Retail integer normalization should truncate green to 63.")
	assert_eq(int(blend_bytes[2]), 63, "Retail integer normalization should truncate blue to 63.")

	var material := ShaderMaterial.new()
	material.shader = _surface_shader()
	assert_true(inputs.apply_to_material(material))
	assert_same(material.get_shader_parameter("u_blendmap"), inputs.get_blend_texture())
	assert_same(material.get_shader_parameter("u_detail_c1"), inputs.get_detail_c1_texture())
	assert_same(material.get_shader_parameter("u_colormap"), inputs.get_colormap_texture())
	assert_same(material.get_shader_parameter("u_detail2"), inputs.get_detail2_texture())
	assert_same(material.get_shader_parameter("u_heightfield_normal"), inputs.get_heightfield_normal_texture())
	assert_true(bool(material.get_shader_parameter("u_has_detail2")))
	assert_true(bool(material.get_shader_parameter("u_has_heightfield_normal")))
	assert_eq(float(material.get_shader_parameter("u_detail_density")), 73.0)
	assert_eq(float(material.get_shader_parameter("u_detail2_density")), 9.0)


func test_diagnostics_report_live_texture_dimensions_mips_and_densities() -> void:
	var inputs := TerrainSurfaceInputs.new()
	assert_true(inputs.rebuild(_make_surface_data()))

	var diagnostics: Dictionary = inputs.get_diagnostics()
	assert_eq(int(diagnostics["detail_density"]), 73)
	assert_eq(int(diagnostics["detail2_density"]), 9)
	var textures := diagnostics["textures"] as Dictionary
	for name in ["colormap", "blendmap", "detail_c1", "detail_c2", "detail_c3", "detail2"]:
		var texture := textures[name] as Dictionary
		assert_true(bool(texture["available"]), "%s is available." % name)
		assert_eq(texture["size"], Vector2i(4, 4), "%s reports its live dimensions." % name)
		assert_eq(int(texture["mipmap_count"]), 2,
			"%s reports the two lower 2x2/1x1 mips." % name)
		assert_eq(int(texture["level_count"]), 3,
			"%s reports base plus every lower mip level." % name)

	var heightfield := textures["heightfield_normal"] as Dictionary
	assert_eq(heightfield["size"], Vector2i(4, 4))
	assert_eq(int(heightfield["mipmap_count"]), 0)
	assert_eq(int(heightfield["level_count"]), 1)


func test_partial_rebuilds_replace_only_the_changed_allocation_family() -> void:
	var data := _make_surface_data()
	var inputs := TerrainSurfaceInputs.new()
	assert_true(inputs.rebuild(data))

	var first_height: Texture2D = inputs.get_heightfield_normal_texture()
	var first_blend: Texture2D = inputs.get_normalized_blend_texture()
	var first_coefficient: Texture2D = inputs.get_detail_coefficient_texture()
	var first_c1: Texture2D = inputs.get_paired_detail_texture(0)
	var first_height_bytes := first_height.get_image().get_data()

	# Mutate the same live FORMAT_RF image ONED brushes own. A height-only refresh
	# must consume get_depth_raw16(), replace only the normal texture, and leave
	# normalized blend/coefficient/paired-mip allocations untouched.
	data.get_heightmap_image().set_pixel(1, 0, Color(16.0, 0.0, 0.0, 1.0))
	assert_true(inputs.rebuild_heightfield())
	assert_ne(inputs.get_heightfield_normal_texture().get_image().get_data(), first_height_bytes,
		"Height normal refresh must observe the live edited height image.")
	assert_same(inputs.get_normalized_blend_texture(), first_blend)
	assert_same(inputs.get_detail_coefficient_texture(), first_coefficient)
	assert_same(inputs.get_paired_detail_texture(0), first_c1)

	var replacement_blend := _solid_image(Color8(32, 128, 96, 255), 4)
	data.set_blendmap_image(replacement_blend)
	assert_true(inputs.rebuild_blend())
	assert_ne(inputs.get_normalized_blend_texture(), first_blend)
	assert_same(inputs.get_detail_coefficient_texture(), first_coefficient)
	assert_same(inputs.get_paired_detail_texture(0), first_c1)


func test_null_terrain_reapply_clears_every_material_input() -> void:
	var inputs := TerrainSurfaceInputs.new()
	assert_true(inputs.rebuild(_make_surface_data()))
	var material := ShaderMaterial.new()
	material.shader = _surface_shader()
	assert_true(inputs.apply_to_material(material))

	# Tile overlay is independently optional, so seed it explicitly to prove the
	# detach path restores the old runtime clear contract for this sampler too.
	var sentinel := _solid_texture(Color8(20, 40, 60, 255), 2)
	material.set_shader_parameter("u_tile_overlay", sentinel)
	material.set_shader_parameter("u_has_tile_overlay", true)

	inputs.set_terrain_data(null)
	assert_true(inputs.apply_to_material(material),
		"Applying an empty input set must clear a retained material, not leave stale terrain state.")
	for uniform_name in [
		"u_colormap", "u_detailmap", "u_blendmap",
		"u_detail_c1", "u_detail_c2", "u_detail_c3",
		"u_detail2", "u_heightfield_normal", "u_tile_overlay",
	]:
		assert_null(material.get_shader_parameter(uniform_name),
			"Detached terrain must clear %s." % uniform_name)
	assert_false(bool(material.get_shader_parameter("u_has_detail2")))
	assert_false(bool(material.get_shader_parameter("u_has_heightfield_normal")))
	assert_false(bool(material.get_shader_parameter("u_has_tile_overlay")))
	assert_eq(float(material.get_shader_parameter("u_detail_density")), 0.0)


func test_tile_overlay_composite_is_shared_and_independently_refreshable() -> void:
	var data := TerrainData.new()
	data.set_tilestrip_tex(_solid_texture(Color8(12, 90, 34, 255), 64))
	var tile_info := TerrainTileInfo.new()
	var entry := TerrainTileEntry.new()
	entry.set_cell(0, 0)
	entry.set_tile_index(0)
	tile_info.add_entry(entry)

	var inputs := TerrainSurfaceInputs.new()
	assert_true(inputs.rebuild(data, tile_info, true))
	assert_true(inputs.has_tile_overlay())
	assert_eq(inputs.get_tile_overlay_texture().get_size(), Vector2(1024, 1024))
	var overlay_bytes := inputs.get_tile_overlay_texture().get_image().get_data()
	assert_true(255 in overlay_bytes, "The authored tile must contribute opaque pixels to the composite.")
	var diagnostics: Dictionary = inputs.get_diagnostics()
	assert_true(bool(diagnostics["tile_overlay_available"]))
	assert_true(bool(diagnostics["tile_info_available"]))
	assert_eq(int(diagnostics["tile_entry_count"]), 1)
	assert_true(bool(diagnostics["tilestrip_available"]))
	var overlay_texture := (diagnostics["textures"] as Dictionary)["tile_overlay"] as Dictionary
	assert_eq(overlay_texture["size"], Vector2i(1024, 1024))
	assert_eq(int(overlay_texture["mipmap_count"]), 0,
		"The current composed overlay has no mip chain.")

	inputs.set_tile_overlay_enabled(false)
	assert_false(inputs.rebuild_tile_overlay())
	assert_false(inputs.has_tile_overlay())
	diagnostics = inputs.get_diagnostics()
	assert_false(bool(diagnostics["tile_overlay_enabled"]))
	assert_false(bool(diagnostics["tile_overlay_available"]))
	assert_true(bool(diagnostics["tile_info_available"]),
		"Disabling composition must not hide the parsed .til source.")


func test_nova_terrain_delegates_surface_input_ownership() -> void:
	var terrain: Terrain = add_child_autofree(Terrain.new())
	var data := TerrainData.new()
	terrain.set_terrain_data(data)

	assert_not_null(terrain.get_surface_inputs())
	assert_same(terrain.get_surface_inputs().get_terrain_data(), data)


func _make_surface_data() -> TerrainData:
	var data := TerrainData.new()
	data.set_colormap(_solid_texture(Color8(90, 110, 70, 255), 4))
	data.set_detailmap(_solid_texture(Color8(80, 100, 140, 255), 4))
	data.set_detailmap_c1(_solid_texture(Color8(130, 80, 40, 255), 4))
	data.set_detailmap_c2(_solid_texture(Color8(40, 130, 80, 255), 4))
	data.set_detailmap_c3(_solid_texture(Color8(80, 40, 130, 255), 4))
	data.set_detailmapdist(_solid_texture(Color8(70, 75, 80, 255), 4))
	data.set_detailmap2(_solid_texture(Color8(120, 120, 130, 255), 4))
	data.set_detailmapdist2(_solid_texture(Color8(100, 100, 100, 255), 4))
	data.set_detail_density2(9)
	var blend := _solid_image(Color8(128, 64, 64, 200), 4)
	data.set_blendmap_image(blend)
	data.set_detailblendmap(ImageTexture.create_from_image(blend))
	data.set_heightmap_image(_solid_height_image(4, 2.0))
	data.set_detail_density(73)
	return data


func _solid_image(color: Color, side: int) -> Image:
	var image := Image.create(side, side, false, Image.FORMAT_RGBA8)
	image.fill(color)
	return image


func _solid_texture(color: Color, side: int) -> Texture2D:
	return ImageTexture.create_from_image(_solid_image(color, side))


func _solid_height_image(side: int, height: float) -> Image:
	var bytes := PackedByteArray()
	bytes.resize(side * side * 4)
	for index in side * side:
		bytes.encode_float(index * 4, height)
	return Image.create_from_data(side, side, false, Image.FORMAT_RF, bytes)


func _surface_shader() -> Shader:
	var shader := Shader.new()
	shader.code = """
shader_type spatial;
uniform sampler2D u_colormap;
uniform sampler2D u_detailmap;
uniform sampler2D u_blendmap;
uniform sampler2D u_detail_c1;
uniform sampler2D u_detail_c2;
uniform sampler2D u_detail_c3;
uniform sampler2D u_detail2;
uniform bool u_has_detail2 = false;
uniform float u_detail2_density = 8.0;
uniform sampler2D u_heightfield_normal;
uniform bool u_has_heightfield_normal = false;
uniform sampler2D u_tile_overlay;
uniform bool u_has_tile_overlay = false;
uniform float u_detail_density = 0.0;
void fragment() { ALBEDO = vec3(0.0); }
"""
	return shader
