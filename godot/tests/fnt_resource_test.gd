extends GutTest

const FNT_PATH := "res://../fixtures/fnt/Serpen24.fnt"
const TEMP_FNT_PATH := "user://test_blank_font.fnt"


func after_each() -> void:
	var abs := ProjectSettings.globalize_path(TEMP_FNT_PATH)
	if FileAccess.file_exists(abs):
		DirAccess.remove_absolute(abs)


func test_fnt_loads_as_native_resource_and_font_file_view() -> void:
	var bytes := FileAccess.get_file_as_bytes(FNT_PATH)
	assert_gt(bytes.size(), 0, "Serpen24.fnt fixture bytes should be readable.")
	var res := FntResource.new()
	var err := res.load_from_bytes(bytes)
	assert_eq(err, OK, "Serpen24.fnt should load as FntResource.")
	if err != OK:
		return

	assert_eq(res.get_page_count(), 1, "Serpen24 should be a one-page Nova FNT fixture.")
	assert_eq(res.get_glyph_count(), 224, "Nova FNT exposes fixed glyph slots 32..255.")
	assert_eq(res.get_first_char(), 32, "First glyph code should be ASCII 32.")

	var page := res.get_page_image(0)
	assert_not_null(page, "Loaded FNT should expose page images for authoring.")
	if page != null:
		assert_eq(page.get_width(), 256, "FNT page width is fixed.")
		assert_eq(page.get_height(), 256, "FNT page height is fixed.")

	var font := res.to_font_file()
	assert_not_null(font, "Native FNT resource should generate a Godot FontFile view.")
	if font != null:
		assert_gt(font.get_fixed_size(), 0, "Generated FontFile should have a fixed size.")


func test_blank_fnt_saves_and_reloads_alpha_and_glyph_rect() -> void:
	var res := FntResource.new()
	var err := res.create_blank(1, -3)
	assert_eq(err, OK, "Blank Nova FNT resource should initialize.")

	res.set_glyph_rect(32, 0, Rect2i(0, 0, 4, 3))
	res.set_pixel_alpha(0, 0, 0, 200)

	var out_bytes := res.to_bytes()
	assert_gt(out_bytes.size(), 0, "FntResource should serialize to raw .fnt bytes.")
	var file := FileAccess.open(TEMP_FNT_PATH, FileAccess.WRITE)
	assert_not_null(file, "Temp .fnt path should be writable.")
	if file == null:
		return
	file.store_buffer(out_bytes)
	file.close()

	var reload_bytes := FileAccess.get_file_as_bytes(TEMP_FNT_PATH)
	var reloaded := FntResource.new()
	var reload_err := reloaded.load_from_bytes(reload_bytes)
	assert_eq(reload_err, OK, "Saved raw .fnt should reload.")
	if reload_err != OK:
		return

	assert_eq(reloaded.get_page_count(), 1, "Reloaded blank font keeps page count.")
	assert_eq(reloaded.get_glyph_spacing(), -3, "Reloaded blank font keeps glyph spacing.")
	assert_eq(reloaded.get_glyph_rect(32), Rect2i(0, 0, 4, 3), "Glyph rect should round-trip.")
	assert_eq(reloaded.get_pixel_alpha(0, 0, 0), 200, "Edited alpha should round-trip.")


func test_pack_shelf_binding_packs_deterministically() -> void:
	# The shelf packer is native (engine/formats/fnt fnt_pack_shelf, ENG-4); this pins
	# the binding contract — (w,h) pairs in, (page,x,y,w,h) quintuples out —
	# and the format constants the fonts rasterizer aliases.
	assert_eq(FntResource.FIRST_CHAR, 32)
	assert_eq(FntResource.GLYPH_COUNT, 224)
	assert_eq(FntResource.TEXTURE_WIDTH, 256)
	assert_eq(FntResource.TEXTURE_HEIGHT, 256)
	assert_eq(FntResource.MAX_PAGES, 16)
	assert_eq(FntResource.PACK_PAD, 1)

	var sizes := PackedInt32Array([100, 20, 100, 30, 60, 10, 200, 40, 0, 0, 30, 50])
	var rects := FntResource.pack_shelf(sizes)
	assert_eq(rects.size(), 6 * 5, "One quintuple per input pair.")
	if rects.size() != 6 * 5:
		return
	# The hand-walked reference layout (mirrors tests/fnt/fnt_pack_test.c).
	assert_eq(Rect2i(rects[1], rects[2], rects[3], rects[4]), Rect2i(1, 1, 100, 20))
	assert_eq(Rect2i(rects[6], rects[7], rects[8], rects[9]), Rect2i(102, 1, 100, 30))
	assert_eq(Rect2i(rects[11], rects[12], rects[13], rects[14]), Rect2i(1, 32, 60, 10))
	assert_eq(Rect2i(rects[16], rects[17], rects[18], rects[19]), Rect2i(1, 43, 200, 40))
	assert_eq(rects[23], 0, "Empty cell packs as a zero rect.")
	assert_eq(Rect2i(rects[26], rects[27], rects[28], rects[29]), Rect2i(202, 43, 30, 50))
	for i in range(6):
		assert_eq(rects[i * 5], 0, "This layout stays on page 0.")

	var overflow := PackedInt32Array()
	for i in range(FntResource.MAX_PAGES + 1):
		overflow.append(254)
		overflow.append(254)
	assert_eq(FntResource.pack_shelf(overflow).size(), 0, "Page overflow returns empty.")
