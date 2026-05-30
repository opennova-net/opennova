extends GutTest

const KDA_PATH := "res://assets/credits/nlist.kda"
const FONT_SAVE_PATH := "user://kda_font_preserve_test.kda"


func test_kda_loads_as_cbin_credits_resource() -> void:
	var res = ResourceLoader.load(KDA_PATH)

	assert_not_null(res, "nlist.kda should load through the registered KdaResourceFormatLoader.")
	if res == null:
		return

	assert_true(res.get_class() == "CbinCreditsResource",
		"Loaded resource should be a CbinCreditsResource, got: %s" % res.get_class())

	assert_true(res.get_entry_count() > 0,
		"nlist.kda should contain at least one credits entry.")

	assert_true(res.get_scroll_rate() > 0.0,
		"scroll_rate should be positive, got: %.4f" % res.get_scroll_rate())

	print("PASS: loaded %s with %d entries, scroll_rate=%.2f" % [
		KDA_PATH, res.get_entry_count(), res.get_scroll_rate()
	])


func test_kda_load_save_preserves_unresolved_font_names() -> void:
	var res := ResourceLoader.load(KDA_PATH, "CbinCreditsResource",
		ResourceLoader.CACHE_MODE_IGNORE) as CbinCreditsResource
	assert_not_null(res, "Fixture should load before font preservation test.")
	if res == null:
		return

	var original_font_count := _text_entries_with_font_names(res)
	assert_eq(original_font_count, 231,
		"Stock nlist.kda should expose all text-entry font names even without .fnt assets.")

	var err := ResourceSaver.save(res, FONT_SAVE_PATH)
	assert_eq(err, OK, "Saving loaded KDA should succeed.")
	if err != OK:
		return

	var reloaded := ResourceLoader.load(FONT_SAVE_PATH, "CbinCreditsResource",
		ResourceLoader.CACHE_MODE_IGNORE) as CbinCreditsResource
	assert_not_null(reloaded, "Saved KDA should reload.")
	if reloaded == null:
		return

	assert_eq(_text_entries_with_font_names(reloaded), original_font_count,
		"Saving must not drop unresolved KDA font names.")

	var abs := ProjectSettings.globalize_path(FONT_SAVE_PATH)
	if FileAccess.file_exists(abs):
		DirAccess.remove_absolute(abs)


func _text_entries_with_font_names(resource: CbinCreditsResource) -> int:
	var count := 0
	for i in range(resource.get_entry_count()):
		var entry := resource.get_entry(i)
		if entry is CbinTextEntry and not (entry as CbinTextEntry).get_font_name().is_empty():
			count += 1
	return count
