extends GutTest


func test_export_presets_do_not_bundle_resource_root_data() -> void:
	var text := FileAccess.get_file_as_string("res://export_presets.cfg")
	# One empty include_filter per preset: Mod Tools + Runtime, on both Windows
	# and macOS. None of them should bundle original resource-root data.
	assert_eq(text.count("include_filter=\"\""), 4,
		"Mod tools and runtime exports should not bundle original resource-root data.")
	assert_false(text.contains("*.kda") or text.contains("*.fnt"),
		"KDA and FNT files are loaded from the configured resource root, not exported in the app PCK.")
