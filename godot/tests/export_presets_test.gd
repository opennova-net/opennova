extends GutTest


func test_export_presets_include_kda_and_fnt_files() -> void:
	var text := FileAccess.get_file_as_string("res://export_presets.cfg")
	assert_string_contains(text, "include_filter=\"*.kda,*.KDA,*.fnt,*.FNT\"",
		"Godot export presets should explicitly include raw credits and Nova font assets.")
	assert_eq(text.count("include_filter=\"*.kda,*.KDA,*.fnt,*.FNT\""), 2,
		"Both mod tools and runtime exports should include .kda and .fnt files.")
