extends GutTest

# ControlsBindings' user:// persistence stand-in (D-CTRL-3): the records blob
# applies only under this build's schema stamp. An unstamped file -- one a
# build before the seat rows' Ctrl seeding wrote, pinning seat1 to a bare 1 --
# is dropped for the catalog defaults, and persist() stamps what it writes.

var _had_cfg := false
var _saved_cfg := PackedByteArray()


func before_each() -> void:
	_had_cfg = FileAccess.file_exists(ControlsBindings.CONFIG_PATH)
	_saved_cfg = FileAccess.get_file_as_bytes(ControlsBindings.CONFIG_PATH) \
			if _had_cfg else PackedByteArray()


func after_each() -> void:
	if _had_cfg:
		var file := FileAccess.open(ControlsBindings.CONFIG_PATH, FileAccess.WRITE)
		if file != null:
			file.store_buffer(_saved_cfg)
			file.close()
	elif FileAccess.file_exists(ControlsBindings.CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(ControlsBindings.CONFIG_PATH))


# A cfg holding the pre-seeding seat1 record (bare 1, no modifier).
func _write_stale_seat_blob(stamped: bool) -> void:
	var config := ConfigFile.new()
	config.set_value(ControlsBindings.SECTION, ControlsBindings.KEY,
			{"seat1": PackedInt32Array([49, 0, 0, 0, 0, 0])})
	if stamped:
		config.set_value(ControlsBindings.SECTION, ControlsBindings.SCHEMA_KEY,
				ControlsBindings.SCHEMA)
	assert_eq(config.save(ControlsBindings.CONFIG_PATH), OK, "the fixture cfg writes")


func test_unstamped_blob_is_dropped_for_the_defaults() -> void:
	_write_stale_seat_blob(false)
	var model := ControlsModel.new()
	assert_false(ControlsBindings.load_saved_records(model), "an unstamped blob does not load")
	assert_true(model.display_text_for_token("seat1").begins_with("Ctrl - "),
			"seat1 keeps the seeded Ctrl chord")


func test_stamped_blob_applies() -> void:
	_write_stale_seat_blob(true)
	var model := ControlsModel.new()
	assert_true(ControlsBindings.load_saved_records(model), "a stamped blob loads")
	assert_false(model.display_text_for_token("seat1").begins_with("Ctrl"),
			"the saved bare-digit record replaces the default")


func test_missing_or_corrupt_file_leaves_the_defaults() -> void:
	if FileAccess.file_exists(ControlsBindings.CONFIG_PATH):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(ControlsBindings.CONFIG_PATH))
	var model := ControlsModel.new()
	assert_false(ControlsBindings.load_saved_records(model), "no file, nothing loads")
	var config := ConfigFile.new()
	config.set_value(ControlsBindings.SECTION, ControlsBindings.SCHEMA_KEY, "one")
	config.set_value(ControlsBindings.SECTION, ControlsBindings.KEY, 42)
	assert_eq(config.save(ControlsBindings.CONFIG_PATH), OK)
	assert_false(ControlsBindings.load_saved_records(model),
			"a non-int stamp and a non-Dictionary blob both fall back")


func test_persist_stamps_the_file() -> void:
	ControlsBindings.model().restore_defaults()
	ControlsBindings.persist()
	var config := ConfigFile.new()
	assert_eq(config.load(ControlsBindings.CONFIG_PATH), OK, "persist writes the cfg")
	assert_eq(config.get_value(ControlsBindings.SECTION, ControlsBindings.SCHEMA_KEY, 0),
			ControlsBindings.SCHEMA, "the stamp lands beside the records")
	var blob: Dictionary = config.get_value(ControlsBindings.SECTION, ControlsBindings.KEY, {})
	var seat1: PackedInt32Array = blob.get("seat1", PackedInt32Array())
	assert_eq(seat1.size(), 6, "the seat1 record persists")
	assert_eq(seat1[2], 17, "the persisted seat1 carries the Ctrl modifier")
	var fresh := ControlsModel.new()
	assert_true(ControlsBindings.load_saved_records(fresh), "the stamped file round-trips")
