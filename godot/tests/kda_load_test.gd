extends GutTest

const KDA_PATH := "res://../fixtures/cbin/nlist.reference.kda"
const CREDITS_IMAGE_FIXTURE := "res://../fixtures/cbin/cr1.png"
const FONT_FIXTURE := "res://../fixtures/fnt/Serpen24.fnt"
const FONT_SAVE_PATH := "user://kda_font_preserve_test.kda"

var _temp_roots: Array[String] = []


func after_each() -> void:
	for root in _temp_roots:
		_remove_dir_recursive(root)
	_temp_roots.clear()


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


func test_kda_resolves_fonts_and_images_from_same_filesystem_root() -> void:
	var root := OS.get_cache_dir().path_join("opennova_kda_root_%d" % Time.get_ticks_usec())
	_temp_roots.append(root)
	assert_eq(DirAccess.make_dir_recursive_absolute(root), OK)
	_copy_fixture(KDA_PATH, root.path_join("nlist.kda"))
	_copy_fixture(CREDITS_IMAGE_FIXTURE, root.path_join("cr1.png"))
	_copy_fixture(FONT_FIXTURE, root.path_join("Serpen24.fnt"))

	var res := ResourceLoader.load(root.path_join("nlist.kda"), "CbinCreditsResource",
		ResourceLoader.CACHE_MODE_IGNORE) as CbinCreditsResource
	assert_not_null(res, "KDA should load from a filesystem resource root.")
	if res == null:
		return

	var resolved_image := false
	var resolved_font := false
	for i in range(res.get_entry_count()):
		var entry := res.get_entry(i)
		if entry is CbinImageEntry and (entry as CbinImageEntry).get_texture_name().to_lower() == "cr1.png":
			resolved_image = (entry as CbinImageEntry).get_texture() != null
		elif entry is CbinTextEntry and (entry as CbinTextEntry).get_font_name() == "Serpen24":
			resolved_font = (entry as CbinTextEntry).get_font() != null
		if resolved_image and resolved_font:
			break

	assert_true(resolved_image, "KDA image references should resolve beside the opened .kda.")
	assert_true(resolved_font, "KDA font names should resolve beside the opened .kda.")


func _text_entries_with_font_names(resource: CbinCreditsResource) -> int:
	var count := 0
	for i in range(resource.get_entry_count()):
		var entry := resource.get_entry(i)
		if entry is CbinTextEntry and not (entry as CbinTextEntry).get_font_name().is_empty():
			count += 1
	return count


func _copy_fixture(src_path: String, dst_path: String) -> void:
	var src := FileAccess.open(ProjectSettings.globalize_path(src_path), FileAccess.READ)
	assert_not_null(src, "Fixture should be readable: %s" % src_path)
	if src == null:
		return
	var dst := FileAccess.open(dst_path, FileAccess.WRITE)
	assert_not_null(dst, "Fixture destination should be writable: %s" % dst_path)
	if dst != null:
		dst.store_buffer(src.get_buffer(src.get_length()))
		dst.close()
	src.close()


func _remove_dir_recursive(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return
	dir.list_dir_begin()
	var entry := dir.get_next()
	while not entry.is_empty():
		var child := path.path_join(entry)
		if dir.current_is_dir():
			_remove_dir_recursive(child)
		else:
			DirAccess.remove_absolute(child)
		entry = dir.get_next()
	dir.list_dir_end()
	DirAccess.remove_absolute(path)
