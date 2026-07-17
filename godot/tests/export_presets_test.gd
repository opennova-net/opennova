extends GutTest


func test_export_presets_do_not_bundle_resource_root_data() -> void:
	var text := FileAccess.get_file_as_string("res://export_presets.cfg")
	# One empty include_filter per preset: Mod Tools + Runtime, on both Windows
	# and macOS. None of them should bundle original resource-root data.
	assert_eq(text.count("include_filter=\"\""), 4,
		"Mod tools and runtime exports should not bundle original resource-root data.")
	assert_false(text.contains("*.kda") or text.contains("*.fnt"),
		"KDA and FNT files are loaded from the configured resource root, not exported in the app PCK.")


func test_export_presets_exclude_the_other_product() -> void:
	var text := FileAccess.get_file_as_string("res://export_presets.cfg")
	# The two-product split (ADR 0015): each preset excludes the other
	# product's tree, on both Windows and macOS. This is the load-time
	# tripwire behind the engine/editor boundary (ADR 0016) - an exported
	# package that still bundles the other tree would mask a cross-product
	# load instead of failing it.
	assert_eq(text.count("exclude_filter=\"game/*\""), 2,
		"Both Mod Tools presets should exclude the game shell.")
	assert_eq(text.count("exclude_filter=\"modtools/*\""), 2,
		"Both Runtime presets should exclude the editor.")
