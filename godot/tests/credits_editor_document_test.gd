extends GutTest

const CreditsEditorDocument = preload("res://modtools/credits/credits_editor_document.gd")
const KDA_PATH := "res://../fixtures/cbin/nlist.reference.kda"
const TEMP_DIR := "user://test_credits_doc"


func before_each() -> void:
	DirAccess.make_dir_recursive_absolute(
		ProjectSettings.globalize_path(TEMP_DIR)
	)


func after_each() -> void:
	_cleanup_dir(TEMP_DIR)


func _cleanup_dir(dir_path: String) -> void:
	var abs := ProjectSettings.globalize_path(dir_path)
	var da := DirAccess.open(abs)
	if da == null:
		return
	da.list_dir_begin()
	var fname := da.get_next()
	while fname != "":
		if not da.current_is_dir():
			da.remove(fname)
		fname = da.get_next()
	da.list_dir_end()
	DirAccess.remove_absolute(abs)


func test_fresh_document_is_clean() -> void:
	var doc = autofree(CreditsEditorDocument.new())

	assert_not_null(doc.resource, "Fresh document should have a non-null resource.")
	assert_false(doc.is_dirty, "Fresh document should start clean (is_dirty == false).")
	assert_eq(doc.current_path, "", "Fresh document current_path should be empty.")


func test_resource_changed_marks_dirty() -> void:
	var doc = autofree(CreditsEditorDocument.new())

	# Use an array so the lambda captures a reference, not a copy.
	var state_changed_count := [0]
	doc.state_changed.connect(func() -> void: state_changed_count[0] += 1)

	var entry := CbinTextEntry.new()
	entry.set_text("test entry")
	doc.resource.add_entry(entry)

	assert_true(doc.is_dirty,
		"Adding an entry to the resource should mark the document dirty.")
	assert_true(state_changed_count[0] > 0,
		"state_changed should have fired at least once after mutation.")


func test_create_new_replaces_resource_and_clears_dirty() -> void:
	var doc = autofree(CreditsEditorDocument.new())

	var open_err = doc.open_kda(KDA_PATH)
	assert_eq(open_err, OK, "Opening fixture should succeed.")
	if open_err != OK:
		return

	var old_resource = doc.resource

	var entry := CbinTextEntry.new()
	entry.set_text("dirty")
	doc.resource.add_entry(entry)
	assert_true(doc.is_dirty, "Document should be dirty before create_new.")

	var loaded_count := [0]
	doc.resource_loaded.connect(func(_r) -> void: loaded_count[0] += 1)

	var err = doc.create_new()
	assert_eq(err, OK, "create_new should return OK.")
	assert_true(doc.resource != old_resource,
		"create_new should replace the resource with a new one.")
	assert_false(doc.is_dirty, "Document should be clean after create_new.")
	assert_eq(doc.current_path, "", "current_path should be empty after create_new.")
	assert_true(loaded_count[0] > 0, "resource_loaded signal should fire after create_new.")


func test_open_kda_loads_fixture() -> void:
	var doc = autofree(CreditsEditorDocument.new())

	var err = doc.open_kda(KDA_PATH)
	assert_eq(err, OK, "open_kda should return OK for the fixture.")
	if err != OK:
		return

	assert_not_null(doc.resource, "resource should be non-null after open_kda.")
	assert_true(doc.resource.get_entry_count() > 0,
		"Loaded fixture should have at least one entry.")
	assert_eq(doc.current_path, KDA_PATH,
		"current_path should be set to the opened file path.")
	assert_false(doc.is_dirty, "Document should be clean immediately after open_kda.")


func test_save_as_round_trip() -> void:
	var doc = autofree(CreditsEditorDocument.new())

	var open_err = doc.open_kda(KDA_PATH)
	assert_eq(open_err, OK, "Fixture should open before save-as round-trip.")
	if open_err != OK:
		return

	var expected_count = doc.resource.get_entry_count()

	var extra := CbinTextEntry.new()
	extra.set_text("round-trip sentinel")
	doc.resource.add_entry(extra)
	expected_count += 1

	var save_err = doc.save_as(TEMP_DIR)
	assert_eq(save_err, OK, "save_as should succeed.")
	if save_err != OK:
		return

	var saved_path = doc.current_path
	assert_false(saved_path.is_empty(), "current_path should be updated after save_as.")

	var doc2 = autofree(CreditsEditorDocument.new())
	var reload_err = doc2.open_kda(saved_path)
	assert_eq(reload_err, OK, "Saved .kda should reload successfully.")
	if reload_err != OK:
		return

	assert_eq(doc2.resource.get_entry_count(), expected_count,
		"Reloaded resource should have the same entry count as what was saved.")
