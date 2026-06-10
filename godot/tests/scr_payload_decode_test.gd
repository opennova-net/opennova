extends GutTest

const ITEMS_PATH := "res://../fixtures/def/items.def"
const FNT_PATH := "res://../fixtures/fnt/Serpen24.fnt"
const SCR_KEY_DEFAULT := 0xabee_face

var _temp_paths: Array[String] = []


func after_each() -> void:
	for path in _temp_paths:
		var abs := ProjectSettings.globalize_path(path)
		if FileAccess.file_exists(abs):
			DirAccess.remove_absolute(abs)
	_temp_paths.clear()


func _u32(value: int) -> int:
	return value & 0xffff_ffff


func _rol32(value: int, shift: int) -> int:
	value = _u32(value)
	return _u32((value << shift) | (value >> (32 - shift)))


func _xor_scr_keystream(bytes: PackedByteArray, key: int) -> PackedByteArray:
	var out := bytes.duplicate()
	for i in range(out.size()):
		key = _u32(_rol32(_u32(key + _rol32(key, 11)), 4) ^ 1)
		out[i] = int(out[i]) ^ (key & 0xff)
	return out


func _scr_wrap_default_key(plain: PackedByteArray) -> PackedByteArray:
	var payload := _xor_scr_keystream(plain, SCR_KEY_DEFAULT)
	var out := PackedByteArray()
	out.resize(4 + payload.size())
	out[0] = 83 # S
	out[1] = 67 # C
	out[2] = 82 # R
	out[3] = 0
	for i in range(payload.size()):
		out[4 + i] = payload[payload.size() - 1 - i]
	return out


func _write_scr_wrapped_fixture(source_path: String, target_stem: String, target_ext: String) -> String:
	var plain := FileAccess.get_file_as_bytes(source_path)
	assert_false(plain.is_empty(), "fixture should be readable: %s" % source_path)
	var target := "user://%s_%d.%s" % [target_stem, Time.get_ticks_usec(), target_ext]
	var file := FileAccess.open(target, FileAccess.WRITE)
	assert_not_null(file, "temp target should open for write: %s" % target)
	if file == null:
		return ""
	file.store_buffer(_scr_wrap_default_key(plain))
	file.close()
	_temp_paths.append(target)
	return ProjectSettings.globalize_path(target)


func test_scr_wrapped_loose_items_def_loads_invisibly() -> void:
	var path := _write_scr_wrapped_fixture(ITEMS_PATH, "scr_items", "def")
	assert_false(path.is_empty(), "encrypted loose items.def path should be available")
	if path.is_empty():
		return

	var db := NovaItemDatabase.new()
	assert_eq(db.load(path), OK, "NovaItemDatabase.load should auto-decode SCR-wrapped loose items.def")
	assert_true(db.is_loaded(), "decoded item database should report loaded")
	assert_gt(db.get_count(), 0, "decoded item database should expose entries")


func test_scr_wrapped_loose_fnt_resource_loads_invisibly() -> void:
	var path := _write_scr_wrapped_fixture(FNT_PATH, "scr_font", "fnt")
	assert_false(path.is_empty(), "encrypted loose FNT path should be available")
	if path.is_empty():
		return

	var font := ResourceLoader.load(path, "NovaFntResource", ResourceLoader.CACHE_MODE_IGNORE) as NovaFntResource
	assert_not_null(font, "ResourceLoader should auto-decode SCR-wrapped loose .fnt")
	if font == null:
		return
	assert_eq(font.get_glyph_count(), 224, "decoded FNT should parse normally")
