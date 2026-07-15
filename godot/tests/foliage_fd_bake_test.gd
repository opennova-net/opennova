extends GutTest

# The ":fd" binding builds the recovered dual-source retail mip chain:
# wrapped 3x3 alpha on the source, original RGB at mip zero, then exact floor
# 2x2 downsampling with progressive 0x808080 RGB blending.
# [orig: Foliage_LoadDefAssets @ 0x601260;
# GTexture_CreateFromPixelDataWithAlphaBlend].

const ALPHA_EXPECTED: Array[int] = [
	71, 56, 87, 118, 149, 180, 179, 149,
	79, 81, 112, 143, 174, 173, 172, 77,
	84, 118, 149, 180, 179, 178, 81, 82,
	121, 155, 186, 185, 184, 87, 86, 87,
	158, 160, 175, 158, 93, 92, 91, 124,
	163, 165, 100, 83, 98, 97, 128, 161,
	168, 74, 73, 72, 103, 134, 165, 166,
	97, 66, 65, 96, 127, 158, 189, 175,
]
const LEVEL_4X4_EXPECTED: Array[int] = [
	89, 87, 92, 71, 103, 90, 110, 115,
	117, 92, 127, 169, 131, 94, 144, 144,
	94, 109, 101, 119, 108, 111, 118, 175,
	122, 114, 135, 157, 137, 116, 152, 84,
	99, 131, 109, 161, 113, 133, 126, 129,
	128, 135, 143, 95, 142, 138, 161, 126,
	104, 153, 117, 101, 119, 155, 134, 76,
	133, 157, 152, 130, 147, 159, 169, 173,
]
const LEVEL_2X2_EXPECTED: Array[int] = [
	98, 99, 105, 120, 126, 104, 139, 138,
	108, 143, 121, 116, 137, 147, 156, 131,
]
const LEVEL_1X1_EXPECTED: Array[int] = [117, 123, 130, 126]


func _make_image() -> Image:
	var image := Image.create(8, 8, false, Image.FORMAT_RGBA8)
	for y in range(8):
		for x in range(8):
			image.set_pixel(x, y, Color8(
				(11 + x * 19 + y * 7) & 0xff,
				(5 + x * 3 + y * 29) & 0xff,
				(17 + x * 23 + y * 11) & 0xff,
				(13 + x * 31 + y * 37) & 0xff))
	return image


func _assert_level(data: PackedByteArray, offset: int,
		expected: Array[int], label: String) -> void:
	for i in range(expected.size()):
		assert_eq(int(data[offset + i]), expected[i],
			"%s byte %d matches the literal vector" % [label, i])


func test_bake_builds_the_literal_retail_mip_chain() -> void:
	var image := _make_image()
	assert_true(NovaFoliageDispatcher.bake_fd_image(image),
		"An 8x8 power-of-two RGBA8 image builds.")
	assert_true(image.has_mipmaps(), "The adapter returns a mipmapped Image.")
	assert_eq(image.get_mipmap_count(), 3,
		"Godot sees the complete 4x4/2x2/1x1 lower-level chain.")

	var data := image.get_data()
	assert_eq(data.size(), 8 * 8 * 4 + 4 * 4 * 4 + 2 * 2 * 4 + 4,
		"The packed payload contains exact 8x8/4x4/2x2/1x1 dimensions.")
	for y in range(8):
		for x in range(8):
			var pixel := y * 8 + x
			var offset := pixel * 4
			assert_eq(int(data[offset + 0]), (11 + x * 19 + y * 7) & 0xff,
				"Mip-zero R survives unchanged (pixel %d)." % pixel)
			assert_eq(int(data[offset + 1]), (5 + x * 3 + y * 29) & 0xff,
				"Mip-zero G survives unchanged (pixel %d)." % pixel)
			assert_eq(int(data[offset + 2]), (17 + x * 23 + y * 11) & 0xff,
				"Mip-zero B survives unchanged (pixel %d)." % pixel)
			assert_eq(int(data[offset + 3]), ALPHA_EXPECTED[pixel],
				"Mip-zero alpha uses the wrapped kernel (pixel %d)." % pixel)

	var offset_4x4 := 8 * 8 * 4
	var offset_2x2 := offset_4x4 + 4 * 4 * 4
	var offset_1x1 := offset_2x2 + 2 * 2 * 4
	_assert_level(data, offset_4x4, LEVEL_4X4_EXPECTED,
		"4x4 weight-160 retail level")
	_assert_level(data, offset_2x2, LEVEL_2X2_EXPECTED,
		"2x2 floor-box terminal level")
	_assert_level(data, offset_1x1, LEVEL_1X1_EXPECTED,
		"1x1 floor-box terminal level")


func test_non_pow2_is_rejected_untouched() -> void:
	var image := Image.create(3, 4, false, Image.FORMAT_RGBA8)
	image.set_pixel(0, 0, Color8(10, 20, 30, 40))
	var before := image.get_data()
	assert_false(NovaFoliageDispatcher.bake_fd_image(image), "Non-pow2 width is rejected.")
	assert_eq(image.get_data(), before, "A rejected image is left untouched.")


func test_non_rgba8_is_rejected() -> void:
	var image := Image.create(4, 4, false, Image.FORMAT_RGB8)
	assert_false(NovaFoliageDispatcher.bake_fd_image(image),
		"Only RGBA8 input is accepted (callers convert first).")


func test_null_image_is_rejected() -> void:
	assert_false(NovaFoliageDispatcher.bake_fd_image(null), "Null image is rejected.")
