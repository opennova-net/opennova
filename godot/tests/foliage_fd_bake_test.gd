extends GutTest

# The ":fd" bake binding (NovaFoliageDispatcher.bake_fd_image) - the witnessed
# flat-0x808080 + 3x3-smoothed-alpha bake of the foliage diffuse
# [orig: Foliage_LoadDefAssets @ 0x601260 tail; docs/foliage/foliage-re.md,
# D-FOLIAGE-5]. Expected alpha bytes are the same hand-computed 4x4 vector the
# libs ctest (tests/foliage/foliage_fd_bake_test.cpp) pins.

const ALPHA_IN: Array[int] = [
	16, 32, 48, 64,
	80, 96, 112, 128,
	144, 160, 176, 192,
	208, 224, 240, 255,
]
# (4*center + N/S/E/W + 2*corners) >> 4 with pow2 wraparound.
const ALPHA_EXPECTED: Array[int] = [
	115, 112, 127, 123,
	100, 96, 112, 108,
	163, 160, 175, 171,
	147, 144, 159, 155,
]


func _make_image(width: int, height: int, alphas: Array[int]) -> Image:
	var image := Image.create(width, height, false, Image.FORMAT_RGBA8)
	for i in range(width * height):
		var x := i % width
		var y := i / width
		# Arbitrary non-0x80 RGB so the gray fold is observable.
		image.set_pixel(x, y, Color8(7 + i * 13, (201 - i * 5) % 256, (i * 31) % 256, alphas[i]))
	return image


func test_bake_matches_the_hand_vector_and_folds_gray() -> void:
	var image := _make_image(4, 4, ALPHA_IN)
	assert_true(NovaFoliageDispatcher.bake_fd_image(image), "A pow2 RGBA8 image bakes.")

	var data := image.get_data()
	assert_eq(data.size(), 4 * 4 * 4)
	for i in range(16):
		assert_eq(int(data[i * 4 + 0]), 0x80, "R folds to exactly 0x80 (pixel %d)" % i)
		assert_eq(int(data[i * 4 + 1]), 0x80, "G folds to exactly 0x80 (pixel %d)" % i)
		assert_eq(int(data[i * 4 + 2]), 0x80, "B folds to exactly 0x80 (pixel %d)" % i)
		assert_eq(int(data[i * 4 + 3]), ALPHA_EXPECTED[i],
			"Smoothed alpha matches the hand vector (pixel %d)" % i)


func test_non_pow2_is_rejected_untouched() -> void:
	var image := Image.create(3, 2, false, Image.FORMAT_RGBA8)
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
