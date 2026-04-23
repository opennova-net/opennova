extends GutTest

const TerrainEditorScene = preload("res://modtools/terrain/terrain_editor.tscn")

const DVXI5_FIXTURE_RES_DIR := "res://../fixtures/godot/dvxi5"
const OUTPUT_DIR_NAME := "terrain_editor_dvxi5_export_parity"
const EXPECTED_FILES := [
	"Dvxi5.trn",
	"Dvxi5.cpt",
	"Dvxi5_c.tga",
	"Dvxi5_d1.tga",
	"Dvxi5_dc1.tga",
	"Dvxi5_dc2.tga",
	"Dvxi5_dc3.tga",
	"Dvxi5_dm.tga",
	"Dvxi5_dm2.tga",
	"Dvxi5_dmd.tga",
	"Dvxi5_m.pcx",
]
const IGNORED_OUTPUT_FILES := [
	"Dvxi5_f.pcx",
	"TRNTILE10.TGA",
]
const COMPARED_EXTENSIONS := ["cpt", "pcx", "tga", "til", "trn"]


func before_each() -> void:
	_cleanup_dir(_output_dir())


func after_each() -> void:
	_cleanup_dir(_output_dir())


func test_dvxi5_import_then_export_matches_fixture_bytes() -> void:
	var editor = add_child_autofree(TerrainEditorScene.instantiate())

	assert_eq(editor.open_trn(_fixture_path("Dvxi5.trn")), OK, "Dvxi5 fixture should import through the editor.")
	assert_false(editor.is_document_dirty(), "A no-edit import should remain clean before export.")

	var err: Error = editor.export_terrain(_output_dir(), TerrainEditor.ExportFlavor.DFX_JO)
	assert_eq(err, OK, "Dvxi5 fixture should export through the real editor pipeline.")

	for filename in EXPECTED_FILES:
		var source_path := _resolve_fixture_path(String(filename))
		var output_path := _resolve_output_path(String(filename))
		assert_false(source_path.is_empty(), "Fixture file should exist: " + String(filename))
		assert_false(output_path.is_empty(), "Exported file should exist: " + String(filename))
		if source_path.is_empty() or output_path.is_empty():
			continue
		_assert_files_equal(source_path, output_path)

	var unexpected := _unexpected_output_files()
	assert_eq(unexpected, [], "Export should not create extra terrain payload files.")


func _assert_files_equal(expected_path: String, actual_path: String) -> void:
	var expected := FileAccess.get_file_as_bytes(expected_path)
	var actual := FileAccess.get_file_as_bytes(actual_path)
	var label := "%s should match fixture bytes" % expected_path.get_file()
	if expected.size() != actual.size():
		fail_test("%s: size %d != %d" % [label, actual.size(), expected.size()])
		return
	for i in expected.size():
		if actual[i] != expected[i]:
			fail_test("%s: first byte mismatch at offset %d (got %d expected %d)" % [label, i, actual[i], expected[i]])
			return
	pass_test(label)


func _resolve_fixture_path(filename: String) -> String:
	return _resolve_case_insensitive(_fixture_dir(), filename)


func _resolve_output_path(filename: String) -> String:
	return _resolve_case_insensitive(_output_dir(), filename)


func _resolve_case_insensitive(dir_path: String, filename: String) -> String:
	var direct := dir_path.path_join(filename)
	if FileAccess.file_exists(direct):
		return direct
	var wanted := filename.to_lower()
	for existing in DirAccess.get_files_at(dir_path):
		if String(existing).to_lower() == wanted:
			return dir_path.path_join(String(existing))
	return ""


func _unexpected_output_files() -> Array[String]:
	var expected := {}
	for filename in EXPECTED_FILES:
		expected[String(filename).to_lower()] = true
	for filename in IGNORED_OUTPUT_FILES:
		expected[String(filename).to_lower()] = true

	var unexpected: Array[String] = []
	for filename in DirAccess.get_files_at(_output_dir()):
		var ext := String(filename).get_extension().to_lower()
		if ext not in COMPARED_EXTENSIONS:
			continue
		if not expected.has(String(filename).to_lower()):
			unexpected.append(String(filename))
	unexpected.sort()
	return unexpected


func _output_dir() -> String:
	return OS.get_user_data_dir().path_join(OUTPUT_DIR_NAME)


func _fixture_dir() -> String:
	return ProjectSettings.globalize_path(DVXI5_FIXTURE_RES_DIR)


func _fixture_path(filename: String) -> String:
	return _fixture_dir().path_join(filename)


func _cleanup_dir(path: String) -> void:
	if not DirAccess.dir_exists_absolute(path):
		return
	for file in DirAccess.get_files_at(path):
		DirAccess.remove_absolute(path.path_join(file))
	DirAccess.remove_absolute(path)
