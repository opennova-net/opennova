extends GutTest

var _directory := ""


func before_each() -> void:
	_directory = ProjectSettings.globalize_path("user://file_transaction_%d" % Time.get_ticks_usec())
	assert_eq(DirAccess.make_dir_recursive_absolute(_directory), OK)


func after_each() -> void:
	_remove_tree(_directory)


func _remove_tree(directory: String) -> void:
	for filename in DirAccess.get_files_at(directory):
		DirAccess.remove_absolute(directory.path_join(filename))
	for child in DirAccess.get_directories_at(directory):
		_remove_tree(directory.path_join(child))
	DirAccess.remove_absolute(directory)


func _write(path: String, payload: String) -> Error:
	var file := FileAccess.open(path, FileAccess.WRITE)
	if file == null:
		return FileAccess.get_open_error()
	file.store_string(payload)
	file.close()
	return OK


func test_a_path_in_an_artifact_name_cannot_overwrite_the_original_during_staging() -> void:
	var output := _directory.path_join("output")
	assert_eq(DirAccess.make_dir_absolute(output), OK)
	var original := output.path_join("outside.tga")
	assert_eq(_write(original, "original"), OK)
	var writers := {"../outside.tga": _write.bind("replacement")}
	var error := FileTransaction.write(output, writers, func() -> String: return "", true)
	assert_false(error.is_empty(), "artifact names must be filenames, never paths")
	assert_eq(FileAccess.get_file_as_string(original), "original")
	assert_false(FileAccess.file_exists(_directory.path_join("outside.tga")))
	assert_eq(DirAccess.get_directories_at(output).size(), 0, "no staging folder is left behind")


func test_case_colliding_artifacts_are_refused_before_any_write() -> void:
	var original := _directory.path_join("texture.tga")
	assert_eq(_write(original, "original"), OK)
	var writers := {"texture.tga": _write.bind("first"), "TEXTURE.TGA": _write.bind("second")}
	var error := FileTransaction.write(_directory, writers, func() -> String: return "", true)
	assert_false(error.is_empty(), "native filenames are case insensitive")
	assert_eq(FileAccess.get_file_as_string(original), "original")
	assert_eq(DirAccess.get_directories_at(_directory).size(), 0)
