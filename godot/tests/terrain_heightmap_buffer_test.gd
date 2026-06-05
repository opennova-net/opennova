extends GutTest

# NovaTerrainData owns the editable height buffer (3e). These tests pin the
# raw16 <-> FORMAT_RF conversion to the canonical formula the editor's
# save/export rely on, the same one the former GDScript image_to_raw16 used:
#   raw16 (little-endian uint16) = clamp(int(height * 256), 0, 65535)
#   height (FORMAT_RF float)     = raw_u16 / 256.0


func _make_rf_image(values: Array, side: int) -> Image:
	var bytes := PackedByteArray()
	bytes.resize(side * side * 4)
	for i in side * side:
		bytes.encode_float(i * 4, float(values[i]))
	return Image.create_from_data(side, side, false, Image.FORMAT_RF, bytes)


func test_get_depth_raw16_matches_canonical_formula() -> void:
	# Representative heights including the clamp ceiling (255.996 * 256 = 65535).
	var heights := [0.0, 1.0, 255.996, 12.5]
	var data := NovaTerrainData.new()
	data.set_heightmap_image(_make_rf_image(heights, 2))

	var raw := data.get_depth_raw16()
	assert_eq(raw.size(), 2 * 2 * 2, "raw16 should be 2 bytes per cell.")
	for i in heights.size():
		var expected: int = clampi(int(float(heights[i]) * 256.0), 0, 65535)
		var got: int = int(raw[i * 2]) | (int(raw[i * 2 + 1]) << 8)
		assert_eq(got, expected, "Cell %d raw16 should equal clamp(int(height * 256))." % i)


func test_heightmap_image_from_raw16_roundtrips() -> void:
	var data := NovaTerrainData.new()
	# u16 per cell, including values that exercise the high byte. Every u16/256
	# is exactly representable in float32, so the round-trip is byte-exact.
	var values := [0, 256, 4096, 65535]
	var raw := PackedByteArray()
	raw.resize(values.size() * 2)
	for i in values.size():
		raw[i * 2] = values[i] & 0xFF
		raw[i * 2 + 1] = (values[i] >> 8) & 0xFF

	var image := data.heightmap_image_from_raw16(raw)
	assert_not_null(image, "Should build a square FORMAT_RF image from raw16.")
	assert_eq(image.get_format(), Image.FORMAT_RF, "Height image should be single-channel float.")

	data.set_heightmap_image(image)
	var roundtrip := data.get_depth_raw16()
	assert_eq(roundtrip, raw, "raw16 -> image -> raw16 should be byte-identical.")


func test_set_heightmap_image_is_authoritative_for_depth() -> void:
	# get_depth_raw16 must reflect the live image, not a stale CPT/empty buffer.
	var data := NovaTerrainData.new()
	assert_eq(data.get_depth_raw16().size(), 0, "Unloaded data has no depth.")
	data.set_heightmap_image(_make_rf_image([2.0, 2.0, 2.0, 2.0], 2))
	var raw := data.get_depth_raw16()
	assert_eq(raw.size(), 8, "Setting the height image should make depth available.")
	assert_eq(int(raw[0]) | (int(raw[1]) << 8), 512, "height 2.0 -> raw16 512.")


func test_colormap_and_blendmap_images_are_owned_by_data() -> void:
	# Color/blend mirror the height buffer: NovaTerrainData owns the editable
	# Image and shares it by reference, so brush edits stay visible through data.
	var data := NovaTerrainData.new()
	assert_null(data.get_colormap_image(), "Colormap image should start unset.")
	assert_null(data.get_blendmap_image(), "Blendmap image should start unset.")

	var color := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	color.fill(Color(0.25, 0.5, 0.75, 1.0))
	var blend := Image.create(4, 4, false, Image.FORMAT_RGBA8)
	blend.fill(Color(1.0, 0.0, 0.0, 1.0))
	data.set_colormap_image(color)
	data.set_blendmap_image(blend)

	assert_eq(data.get_colormap_image(), color, "data should hold the same colormap Image it was given.")
	assert_eq(data.get_blendmap_image(), blend, "data should hold the same blendmap Image it was given.")

	# An in-place edit (what a brush does) is visible through data.
	color.set_pixel(0, 0, Color(0.0, 1.0, 0.0, 1.0))
	assert_eq(data.get_colormap_image().get_pixel(0, 0), Color(0.0, 1.0, 0.0, 1.0),
		"Edits to the shared colormap Image should be visible through data.")
