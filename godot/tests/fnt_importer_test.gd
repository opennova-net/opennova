extends GutTest


func test_nova_fnt_files_are_not_claimed_by_bmfont_importer() -> void:
	for filename in DirAccess.get_files_at("res://assets/fonts"):
		if filename.get_extension().to_lower() != "import":
			continue
		if not filename.get_basename().to_lower().ends_with(".fnt"):
			continue
		var import_text := FileAccess.get_file_as_string("res://assets/fonts/".path_join(filename))
		assert_false(import_text.contains("importer=\"font_data_bmfont\""),
			"Nova .fnt files should not be imported by Godot's text BMFont importer.")
