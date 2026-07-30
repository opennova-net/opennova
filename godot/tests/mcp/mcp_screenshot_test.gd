extends GutTest

# McpScreenshot: headless fast-fail (no hang — pins the timeout race) plus the
# pure-CPU encode pipeline with constructed images.


func _gradient(width: int, height: int) -> Image:
	var image := Image.create(width, height, false, Image.FORMAT_RGBA8)
	for y in range(height):
		for x in range(width):
			image.set_pixel(x, y, Color(float(x) / width, float(y) / height, 0.5, 1.0))
	return image


func test_capture_fails_fast_headless() -> void:
	var start := Time.get_ticks_msec()
	var outcome: Dictionary = await McpScreenshot.capture(get_tree().root)
	assert_false(outcome["ok"])
	assert_true(String(outcome["error"]).contains("headless"))
	assert_lt(Time.get_ticks_msec() - start, 2000, "Headless capture returns immediately, never hangs.")


func test_capture_null_viewport_errors() -> void:
	var outcome: Dictionary = await McpScreenshot.capture(null)
	assert_false(outcome["ok"])


func test_capture_honors_a_pre_cancelled_request() -> void:
	var outcome: Dictionary = await McpScreenshot.capture(
			get_tree().root, {}, func() -> bool: return true)
	assert_false(outcome["ok"])
	assert_true(String(outcome["error"]).contains("cancelled"))


func test_encode_webp_with_dimensions() -> void:
	var outcome := McpScreenshot.encode(_gradient(64, 32))
	assert_true(outcome["ok"])
	assert_eq(outcome["mime"], "image/webp")
	assert_eq(outcome["width"], 64)
	assert_eq(outcome["height"], 32)
	assert_gt((outcome["bytes"] as PackedByteArray).size(), 0)


func test_encode_png_format() -> void:
	var outcome := McpScreenshot.encode(_gradient(16, 16), { "format": "png" })
	assert_true(outcome["ok"])
	assert_eq(outcome["mime"], "image/png")
	var bytes: PackedByteArray = outcome["bytes"]
	assert_eq(bytes[1], 0x50, "PNG magic.")


func test_encode_downscales_to_max_dim() -> void:
	var outcome := McpScreenshot.encode(_gradient(400, 200), { "max_dim": 100 })
	assert_true(outcome["ok"])
	assert_eq(outcome["width"], 100)
	assert_eq(outcome["height"], 50, "Aspect preserved.")


func test_encode_crops_region() -> void:
	var outcome := McpScreenshot.encode(_gradient(64, 64), { "region": Rect2i(8, 8, 16, 12) })
	assert_true(outcome["ok"])
	assert_eq(outcome["width"], 16)
	assert_eq(outcome["height"], 12)


func test_encode_rejects_out_of_bounds_region() -> void:
	var outcome := McpScreenshot.encode(_gradient(16, 16), { "region": Rect2i(100, 100, 10, 10) })
	assert_false(outcome["ok"])


func test_encode_does_not_mutate_source() -> void:
	var image := _gradient(300, 300)
	McpScreenshot.encode(image, { "max_dim": 64 })
	assert_eq(image.get_width(), 300, "Caller's image untouched.")


func test_encode_empty_image_errors() -> void:
	var outcome := McpScreenshot.encode(null)
	assert_false(outcome["ok"])


func test_region_for_control_without_window_falls_back_to_full_image() -> void:
	var control: Control = autofree(Control.new())
	var region: Rect2i = McpScreenshot.region_for_control(control, Vector2i(800, 600))
	assert_eq(region, Rect2i(0, 0, 800, 600))
