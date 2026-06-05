extends GutTest

const FNT_PATH := "res://../fixtures/fnt/Serpen24.fnt"
const TEMP_FNT_PATH := "user://test_blank_font.fnt"


func after_each() -> void:
	var abs := ProjectSettings.globalize_path(TEMP_FNT_PATH)
	if FileAccess.file_exists(abs):
		DirAccess.remove_absolute(abs)


func test_fnt_loads_as_native_resource_and_font_file_view() -> void:
	var res := ResourceLoader.load(FNT_PATH, "NovaFntResource", ResourceLoader.CACHE_MODE_IGNORE) as NovaFntResource
	assert_not_null(res, "Serpen24.fnt should load as NovaFntResource.")
	if res == null:
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
	var res := NovaFntResource.new()
	var err := res.create_blank(1, -3)
	assert_eq(err, OK, "Blank Nova FNT resource should initialize.")

	res.set_glyph_rect(32, 0, Rect2i(0, 0, 4, 3))
	res.set_pixel_alpha(0, 0, 0, 200)

	var save_err := ResourceSaver.save(res, TEMP_FNT_PATH)
	assert_eq(save_err, OK, "NovaFntResource should save as raw .fnt.")
	if save_err != OK:
		return

	var reloaded := ResourceLoader.load(TEMP_FNT_PATH, "NovaFntResource", ResourceLoader.CACHE_MODE_IGNORE) as NovaFntResource
	assert_not_null(reloaded, "Saved raw .fnt should reload.")
	if reloaded == null:
		return

	assert_eq(reloaded.get_page_count(), 1, "Reloaded blank font keeps page count.")
	assert_eq(reloaded.get_shadow_offset(), -3, "Reloaded blank font keeps shadow offset.")
	assert_eq(reloaded.get_glyph_rect(32), Rect2i(0, 0, 4, 3), "Glyph rect should round-trip.")
	assert_eq(reloaded.get_pixel_alpha(0, 0, 0), 200, "Edited alpha should round-trip.")
