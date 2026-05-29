extends GutTest

## Integration coverage for the CDEP raw16 kernel reached THROUGH the
## NovaTerrainData GDExtension binding (the standalone kernel ctest covers the
## math; this locks in the Image get_data -> kernel -> set_data round-trip and
## that get_depth_raw16() observes the clamp on the shared Image).


func _heightmap_with_violation() -> Image:
	# Floor the whole map at raw 28 (0.11328 * 256 ~= 28) so block 0/row 0 has a
	# real, non-zero minimum, then spike one pixel far past the 32767 range.
	var image := Image.create(1024, 1024, false, Image.FORMAT_RF)
	image.fill(Color(0.11328, 0.0, 0.0, 1.0))
	image.set_pixel(1, 0, Color(200.0, 0.0, 0.0, 1.0))  # raw 51200 -> range >> 32767
	return image


func _block0_row0_raw_range(data) -> int:
	var raw: PackedByteArray = data.get_depth_raw16()
	var lo := 65535
	var hi := 0
	for x in range(256):
		var v := raw[x * 2] | (raw[x * 2 + 1] << 8)
		if v < lo:
			lo = v
		if v > hi:
			hi = v
	return hi - lo


func test_cdep_clamp_blocks_in_rect_mutates_shared_image() -> void:
	var data := NovaTerrainData.new()
	var image := _heightmap_with_violation()
	data.set_heightmap_image(image)

	assert_eq(data.cdep_count_violations(), 1, "Block 0 / row 0 should be over-range before clamp.")
	assert_gt(_block0_row0_raw_range(data), 32767, "Pre-clamp raw range exceeds the CDEP limit.")

	var clamped := data.cdep_clamp_blocks_in_rect(Rect2i(0, 0, 256, 1))
	assert_eq(clamped, 1, "Exactly one block should be clamped.")
	assert_eq(data.cdep_count_violations(), 0, "No violations should remain after the clamp.")
	assert_lte(_block0_row0_raw_range(data), 32767, "Post-clamp raw range is within the CDEP limit.")

	# The binding must mutate the SAME Image the caller handed in (in place via
	# set_data), not a private copy, so the editor's shared ref stays current.
	assert_lt(image.get_pixel(1, 0).r, 200.0, "The over-range pixel in the shared image was clamped down.")


func test_cdep_count_and_clamp_all_via_data() -> void:
	var data := NovaTerrainData.new()
	data.set_heightmap_image(_heightmap_with_violation())

	assert_eq(data.cdep_count_violations(), 1, "One over-range block before clamp_all.")
	assert_eq(data.cdep_clamp_all_violations(), 1, "clamp_all should report one block healed.")
	assert_eq(data.cdep_count_violations(), 0, "No violations remain after clamp_all.")


func test_cdep_count_is_zero_on_flat_map() -> void:
	var data := NovaTerrainData.new()
	var image := Image.create(1024, 1024, false, Image.FORMAT_RF)
	image.fill(Color(5.0, 0.0, 0.0, 1.0))
	data.set_heightmap_image(image)
	assert_eq(data.cdep_count_violations(), 0, "A flat heightmap has no CDEP violations.")
	assert_eq(data.cdep_clamp_all_violations(), 0, "Nothing to clamp on a flat heightmap.")
