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
	# The display labels below are the no-table fallback ("Ctrl - "): a shell
	# booted earlier in the run may have installed keyhelp.bin as the engine's
	# process-wide "Keys" table (which renders "Ctrl-"), so pin the fallback.
	RtxtStringFile.clear_key_strings()


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
			{"seat1": PackedInt32Array([49, 0, 0, 0, 0, 0, 0, 0])})
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
	assert_eq(seat1.size(), 8, "the seat1 record persists")
	assert_eq(seat1[2], 17, "the persisted seat1 carries the Ctrl modifier")
	var fresh := ControlsModel.new()
	assert_true(ControlsBindings.load_saved_records(fresh), "the stamped file round-trips")


# The by-code binding read (the HUDLS key label's record 200 + category): the
# row the start-up re-lay puts at record `code` formats exactly like its token,
# and a code no row carries is the zero record's empty string (engine controls
# action_for_code carries the witness).
func test_display_text_by_action_code() -> void:
	var model := ControlsModel.new()
	assert_eq(model.display_text_for_action_code(201), model.display_text_for_token("Knife"),
			"record 201 is the Knife row's binding")
	assert_eq(model.display_text_for_action_code(211), model.display_text_for_token("magazine"),
			"record 211 is the magazine row's binding")
	assert_ne(model.display_text_for_action_code(201), "", "the Knife row is bound by default")
	assert_eq(model.display_text_for_action_code(200), "", "no row dispatches 200")


# The live records' revision moves on every binding write and never on a read:
# the HUD formats the HUDLS key labels again only when it moved (engine
# controls BindingSet::revision).
func test_binding_revision_moves_on_writes_only() -> void:
	var model := ControlsModel.new()
	var r := model.get_binding_revision()
	model.display_text_for_action_code(201)
	model.get_rows(ControlsModel.DEVICE_KEYBOARD)
	assert_eq(model.get_binding_revision(), r, "reads leave the revision")
	var bound := -1
	for i in model.get_rows(ControlsModel.DEVICE_KEYBOARD).size():
		var action := model.action_index_for_row(i)
		if action >= 0 and model.control_text(action, ControlsModel.DEVICE_KEYBOARD) != "":
			bound = action
			break
	assert_gte(bound, 0, "a bound row exists")
	model.clear_binding(bound, ControlsModel.DEVICE_KEYBOARD)
	assert_ne(model.get_binding_revision(), r, "a clear moves it")
	r = model.get_binding_revision()
	assert_true(model.assign_godot_key(bound, KEY_K))
	assert_ne(model.get_binding_revision(), r, "an assignment moves it")
	r = model.get_binding_revision()
	model.load_blob(model.save_blob())
	assert_ne(model.get_binding_revision(), r, "a persistence load moves it")
	r = model.get_binding_revision()
	model.restore_defaults()
	assert_ne(model.get_binding_revision(), r, "the defaults move it")
