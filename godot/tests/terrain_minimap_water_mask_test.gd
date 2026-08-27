extends GutTest


func _depthspin_height(data: TerrainData, at: Vector2i) -> int:
	var raw_sum := 0
	for dz in [0, 2]:
		for dx in [0, 2]:
			var source := Vector3(float(at.x * 4 + dx), 0.0,
					float(at.y * 4 + dz))
			raw_sum += int(round(data.get_height(source) * 256.0))
	return raw_sum >> 10


func test_minimap_water_mask_reproduces_retail_depthspin_reduction() -> void:
	var root_dir := TestFs.stage_terrain_root("minimap")
	var root := ResourceRoot.new()
	assert_eq(root.set_root_dir(root_dir), OK)
	var data := TerrainData.new()
	assert_eq(data.load_from_resource_root(root, TestFs.TMAP_TRN), OK)

	# Find two real fixture texels on opposite sides of one integer water
	# plane. The expected height is retail's four raw16 taps followed by >>10,
	# not a bilinear world sample or a hand-registered per-cell carve.
	var wet_at := Vector2i(-1, -1)
	var dry_at := Vector2i(-1, -1)
	var wet_height := 256
	var dry_height := -1
	for z in range(0, 256, 4):
		for x in range(0, 256, 4):
			var at := Vector2i(x, z)
			var height := _depthspin_height(data, at)
			if height > 0 and height < wet_height:
				wet_height = height
				wet_at = at
			if height > dry_height:
				dry_height = height
				dry_at = at
	assert_ne(wet_at, Vector2i(-1, -1))
	assert_gt(dry_height, wet_height)
	if wet_at == Vector2i(-1, -1) or dry_height <= wet_height:
		return

	var mask: ImageTexture = data.build_minimap_water_mask(float(wet_height))
	assert_not_null(mask)
	if mask == null:
		return
	var image := mask.get_image()
	assert_eq(mask.get_size(), Vector2(256, 256))
	assert_eq(image.get_format(), Image.FORMAT_RG8,
			"The shoreline keeps sampled height and water operands in a linear data texture.")
	assert_false(image.has_mipmaps(),
			"The magnified depthspin mask must stay on its authored mip level.")
	var wet_sample := image.get_pixelv(wet_at)
	var dry_sample := image.get_pixelv(dry_at)
	assert_eq(int(round(wet_sample.r * 255.0)), wet_height,
			"Depthspin R retains the reduced integer terrain height.")
	assert_eq(int(round(wet_sample.g * 255.0)), wet_height,
			"Depthspin G carries the integer water plane for the sampled cutoff.")
	assert_gt(dry_sample.r, dry_sample.g,
			"Terrain above the water plane remains available for post-sample rejection.")
	assert_null(data.build_minimap_water_mask(0.0),
			"Env_WaterHeightFixed zero suppresses the shore pass.")
	TestFs.remove_dir_recursive(root_dir)
