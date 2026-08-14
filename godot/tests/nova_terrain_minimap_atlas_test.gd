extends GutTest


func test_minimap_atlas_composites_authored_tiles_then_water() -> void:
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(ProjectSettings.globalize_path(
			"res://../fixtures/godot/dvxi5")), OK)
	var data := TerrainData.new()
	assert_eq(data.load_from_resource_root(root, "Dvxi5.trn"), OK)
	data.set_sector_count(16)
	data.set_sector_rows(16)
	data.set_origin_x(0)
	data.set_origin_y(0)
	var sectors := PackedInt32Array()
	sectors.resize(16 * 16)
	sectors.fill(1)
	data.set_sector_grid(sectors)

	var overlay_image := Image.create(1024, 1024, false, Image.FORMAT_RGBA8)
	var authored_color := Color8(7, 201, 33, 255)
	overlay_image.fill(authored_color)
	var overlay := ImageTexture.create_from_image(overlay_image)

	# Retail's depth-tested water samples are registered 4.25/3.25 world units
	# past each 2-wu atlas texel center (an effective +5.25/+4.25 from its
	# integer texel origin). Find a fixture point where that offset crosses a
	# height contour, then make the cutoff split the shifted and unshifted
	# samples. The old center-only sampler must leave this pixel authored.
	var sample_offset := Vector2(4.25, 3.25)
	var wet_at := Vector2i(-1, -1)
	var dry_at := Vector2i(-1, -1)
	var wet_base_height := 0.0
	var wet_shifted_height := 0.0
	var best_drop := -INF
	var highest_shifted := -INF
	for ty in range(0, 256, 2):
		for tx in range(0, 256, 2):
			var center := Vector3((float(tx) + 0.5) * 2.0, 0.0,
					(float(ty) + 0.5) * 2.0)
			var base_height := data.get_height_world_bilinear(center)
			var shifted_height := data.get_height_world_bilinear(Vector3(
					center.x + sample_offset.x, 0.0,
					center.z + sample_offset.y))
			var drop := base_height - shifted_height
			if drop > best_drop:
				best_drop = drop
				wet_at = Vector2i(tx, ty)
				wet_base_height = base_height
				wet_shifted_height = shifted_height
			if shifted_height > highest_shifted:
				highest_shifted = shifted_height
				dry_at = Vector2i(tx, ty)
	assert_gt(best_drop, 0.25,
			"The terrain fixture needs a contour that witnesses the sample offset.")
	if best_drop <= 0.25:
		return
	var cutoff := (wet_base_height + wet_shifted_height) * 0.5
	var atlas: ImageTexture = data.build_minimap_tile_atlas(cutoff - 1.0, overlay)
	assert_not_null(atlas)
	if atlas == null:
		return
	assert_eq(atlas.get_size(), Vector2(4096, 4096))
	assert_eq(atlas.get_image().get_pixelv(wet_at), Color8(15, 46, 71, 255),
			"Water must use the registered retail height sample, not the texel center.")
	assert_gt(highest_shifted, cutoff)
	assert_eq(atlas.get_image().get_pixelv(dry_at), authored_color,
			"An opaque .til pixel must replace the base colormap exactly.")

	# A plane above the complete fixture floods this valid cell. Water is the
	# final tile-cache layer, so it must replace even an opaque authored pixel.
	atlas = data.build_minimap_tile_atlas(10000.0, overlay)
	assert_not_null(atlas)
	if atlas == null:
		return
	assert_eq(atlas.get_image().get_pixel(0, 0), Color8(15, 46, 71, 255),
			"The terrain-height water mask must compose after authored .til art.")
