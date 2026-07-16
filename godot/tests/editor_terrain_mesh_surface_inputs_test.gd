extends GutTest


func test_editor_mesh_owns_and_applies_shared_surface_inputs() -> void:
	var mesh: EditorTerrainMesh = add_child_autofree(EditorTerrainMesh.new())
	var inputs: NovaTerrainSurfaceInputs = mesh.get_surface_inputs()
	var heightmap_texture: Texture2D = mesh.get_material().get_shader_parameter("u_heightmap")
	var data := _make_surface_data()

	assert_not_null(inputs)
	assert_same(mesh.get_surface_inputs(), inputs, "The editor mesh must retain one cache object.")
	mesh.set_terrain_data(data)
	assert_same(inputs.get_terrain_data(), data)
	assert_true(mesh.rebuild_surface_inputs())

	var material := mesh.get_material()
	assert_same(material.get_shader_parameter("u_blendmap"), inputs.get_blend_texture())
	assert_same(material.get_shader_parameter("u_detail_c1"), inputs.get_detail_c1_texture())
	assert_same(
		material.get_shader_parameter("u_heightfield_normal"),
		inputs.get_heightfield_normal_texture()
	)
	assert_true(bool(material.get_shader_parameter("u_has_heightfield_normal")))
	assert_same(
		material.get_shader_parameter("u_heightmap"),
		heightmap_texture,
		"Applying retail surface inputs must preserve the live editor heightmap binding."
	)
	assert_true(mesh.has_heightfield_normal_texture())
	assert_same(mesh.get_heightfield_normal_texture(), inputs.get_heightfield_normal_texture())


func test_partial_refreshes_rebuild_and_reapply_the_changed_input_family() -> void:
	var mesh: EditorTerrainMesh = add_child_autofree(EditorTerrainMesh.new())
	var data := _make_surface_data()
	mesh.set_terrain_data(data)
	assert_true(mesh.rebuild_surface_inputs())
	var inputs: NovaTerrainSurfaceInputs = mesh.get_surface_inputs()
	var material := mesh.get_material()

	var first_normal: Texture2D = inputs.get_heightfield_normal_texture()
	data.get_heightmap_image().set_pixel(1, 0, Color(12.0, 0.0, 0.0, 1.0))
	assert_true(mesh.refresh_surface_inputs_heightfield())
	assert_ne(inputs.get_heightfield_normal_texture(), first_normal)
	assert_same(
		material.get_shader_parameter("u_heightfield_normal"),
		inputs.get_heightfield_normal_texture()
	)

	var first_blend: Texture2D = inputs.get_normalized_blend_texture()
	data.set_blendmap_image(_solid_image(Color8(32, 128, 96, 255), 4))
	assert_true(mesh.refresh_surface_inputs_blend())
	assert_ne(inputs.get_normalized_blend_texture(), first_blend)
	assert_same(material.get_shader_parameter("u_blendmap"), inputs.get_blend_texture())

	var first_detail2: Texture2D = inputs.get_detail2_texture()
	data.set_detailmap2(_solid_texture(Color8(25, 55, 85, 255), 4))
	assert_true(mesh.refresh_surface_inputs_details())
	assert_ne(inputs.get_detail2_texture(), first_detail2)
	assert_same(
		material.get_shader_parameter("u_detail2"),
		inputs.get_detail2_texture()
	)


func test_tile_overlay_refresh_exposes_foliage_inputs_and_clears_material_state() -> void:
	var mesh: EditorTerrainMesh = add_child_autofree(EditorTerrainMesh.new())
	var data := _make_surface_data()
	data.set_tilestrip_tex(_solid_texture(Color8(12, 90, 34, 255), 64))
	mesh.set_terrain_data(data)
	assert_true(mesh.rebuild_surface_inputs(_one_tile_info(), true))

	assert_true(mesh.has_tile_overlay_texture())
	assert_same(
		mesh.get_tile_overlay_texture(),
		mesh.get_surface_inputs().get_tile_overlay_texture()
	)
	assert_same(
		mesh.get_material().get_shader_parameter("u_tile_overlay"),
		mesh.get_tile_overlay_texture()
	)

	assert_true(mesh.refresh_surface_inputs_tile_overlay(null, false))
	assert_false(mesh.has_tile_overlay_texture())
	assert_false(bool(mesh.get_material().get_shader_parameter("u_has_tile_overlay")))
	assert_null(mesh.get_tile_overlay_texture())


func _make_surface_data() -> NovaTerrainData:
	var data := NovaTerrainData.new()
	data.set_colormap(_solid_texture(Color8(90, 110, 70, 255), 4))
	data.set_detailmap(_solid_texture(Color8(80, 100, 140, 255), 4))
	data.set_detailmap_c1(_solid_texture(Color8(130, 80, 40, 255), 4))
	data.set_detailmap_c2(_solid_texture(Color8(40, 130, 80, 255), 4))
	data.set_detailmap_c3(_solid_texture(Color8(80, 40, 130, 255), 4))
	data.set_detailmapdist(_solid_texture(Color8(70, 75, 80, 255), 4))
	data.set_detailmap2(_solid_texture(Color8(120, 120, 130, 255), 4))
	data.set_detailmapdist2(_solid_texture(Color8(100, 100, 100, 255), 4))
	var blend := _solid_image(Color8(128, 64, 64, 200), 4)
	data.set_blendmap_image(blend)
	data.set_detailblendmap(ImageTexture.create_from_image(blend))
	data.set_heightmap_image(_solid_height_image(4, 2.0))
	data.set_detail_density(73)
	return data


func _one_tile_info() -> NovaTerrainTileInfo:
	var tile_info := NovaTerrainTileInfo.new()
	var entry := NovaTerrainTileEntry.new()
	entry.set_cell(0, 0)
	entry.set_tile_index(0)
	tile_info.add_entry(entry)
	return tile_info


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
