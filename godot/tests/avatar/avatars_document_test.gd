extends GutTest

# AvatarsDocument lifecycle over the committed retail Avatars.def fixture: open is
# clean, an edit via set_model marks dirty, save_as -> reopen persists the edit,
# and create_new yields an empty + clean document.
const AvatarsDocumentScript = preload("res://modtools/avatar/avatars_document.gd")
const AVATARS_FIXTURE := "res://../fixtures/avatars/Avatars.def"


func _fixture_path() -> String:
	return ProjectSettings.globalize_path(AVATARS_FIXTURE)


func test_open_is_clean() -> void:
	var path := _fixture_path()
	if not FileAccess.file_exists(path):
		pending("Avatars.def fixture missing: %s" % path)
		return
	var doc = AvatarsDocumentScript.new()
	assert_eq(doc.open_avatars(path), OK, "open_avatars succeeds")
	assert_true(doc.resource.is_loaded(), "database loaded")
	assert_false(doc.has_unsaved_changes(), "freshly opened document is clean")
	assert_eq(doc.current_path, path, "current_path tracks the opened file")


func test_set_model_marks_dirty() -> void:
	var path := _fixture_path()
	if not FileAccess.file_exists(path):
		pending("Avatars.def fixture missing")
		return
	var doc = AvatarsDocumentScript.new()
	assert_eq(doc.open_avatars(path), OK)
	assert_false(doc.has_unsaved_changes(), "clean before edit")

	var model: Dictionary = doc.resource.get_model()
	var parts: Array = model.get("parts", [])
	assert_gt(parts.size(), 0, "fixture has parts")
	(parts[0] as Dictionary)["graphic"] = "Edited.3di"
	doc.resource.set_model(model)

	assert_true(doc.has_unsaved_changes(), "set_model marks the document dirty")


func test_save_as_persists_edit() -> void:
	var path := _fixture_path()
	if not FileAccess.file_exists(path):
		pending("Avatars.def fixture missing")
		return
	var doc = AvatarsDocumentScript.new()
	assert_eq(doc.open_avatars(path), OK)

	# Edit the first head part's graphic, then save to a temp dir.
	var model: Dictionary = doc.resource.get_model()
	var head_name := ""
	for entry_v in model.get("parts", []):
		var entry: Dictionary = entry_v
		if int(entry.get("kind", -1)) == NovaAvatarDatabase.PART_HEAD:
			entry["graphic"] = "Persisted.3di"
			head_name = String(entry.get("name", ""))
			break
	assert_ne(head_name, "", "found a head part to edit")
	doc.resource.set_model(model)

	var tmp_dir := ProjectSettings.globalize_path("user://avatar_doc_test")
	DirAccess.make_dir_recursive_absolute(tmp_dir)
	assert_eq(doc.save_as(tmp_dir), OK, "save_as to temp dir")
	var saved_path := tmp_dir.path_join("Avatars.def")
	assert_true(FileAccess.file_exists(saved_path), "save_as wrote Avatars.def")
	assert_false(doc.has_unsaved_changes(), "clean after save")

	# Reopen and confirm the edit persisted.
	var doc2 = AvatarsDocumentScript.new()
	assert_eq(doc2.open_avatars(saved_path), OK, "reopen saved file")
	var part: Dictionary = doc2.resource.get_part(NovaAvatarDatabase.PART_HEAD, head_name)
	assert_eq(String(part.get("graphic", "")), "Persisted.3di", "edit survived the round-trip")

	DirAccess.remove_absolute(saved_path)
	DirAccess.remove_absolute(tmp_dir)


func test_create_new_is_empty_and_clean() -> void:
	var doc = AvatarsDocumentScript.new()
	assert_eq(doc.create_new(), OK, "create_new")
	assert_true(doc.resource.is_loaded(), "empty database counts as loaded")
	assert_eq(doc.resource.get_part_count(), 0, "no parts")
	assert_eq(doc.resource.get_nationality_count(), 0, "no nationalities")
	assert_false(doc.has_unsaved_changes(), "new document is clean")
