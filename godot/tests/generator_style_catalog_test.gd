extends GutTest

## Drift guard: the editor's GeneratorStyleCatalog (friendly names for the NovaLogic
## generator-style byte) must stay in lockstep with the canonical C++ enum
## kControlEntries in libs/threedi/src/threedi_panm.cpp. This is what failed
## silently before (materials mislabeled 50 as "square"; lights showed "Custom 55").

const PartAnimsInspectorScript = preload("res://modtools/object/ui/inspectors/part_anims_inspector.gd")


func _catalog_ids() -> Array:
	var ids := []
	for style in GeneratorStyleCatalog.STYLES:
		ids.append(int(style.get("id", -1)))
	return ids


func _canonical_codes_from_cpp() -> Array:
	var path := ProjectSettings.globalize_path("res://").path_join("../libs/threedi/src/threedi_panm.cpp").simplify_path()
	var file := FileAccess.open(path, FileAccess.READ)
	assert_not_null(file, "Canonical generator-style source should be readable at %s" % path)
	if file == null:
		return []
	var text := file.get_as_text()
	file.close()
	# Scope the scan to the kControlEntries array so unrelated braces never match.
	var start := text.find("kControlEntries")
	assert_true(start >= 0, "kControlEntries table should exist in threedi_panm.cpp.")
	if start < 0:
		return []
	var end := text.find("};", start)
	var block := text.substr(start, end - start) if end > start else text.substr(start)
	var regex := RegEx.new()
	regex.compile("\\{\\s*(\\d+)\\s*,\\s*\\{\\s*\"")
	var codes := []
	for m in regex.search_all(block):
		codes.append(int(m.get_string(1)))
	return codes


func test_catalog_matches_canonical_cpp_table() -> void:
	var canonical := _canonical_codes_from_cpp()
	assert_gt(canonical.size(), 40, "Expected the full canonical generator-style table (~55 codes).")
	var catalog := _catalog_ids()
	for code in canonical:
		assert_true(catalog.has(code), "GeneratorStyleCatalog is missing canonical style code %d." % code)
	for id in catalog:
		assert_true(canonical.has(id), "GeneratorStyleCatalog has style %d absent from the canonical C++ table." % id)


func test_catalog_labels_are_real_names() -> void:
	for style in GeneratorStyleCatalog.STYLES:
		var id := int(style.get("id", -1))
		var label := String(style.get("label", ""))
		assert_false(label.is_empty(), "Style %d should have a label." % id)
		assert_false(label.begins_with("Custom"), "Style %d label should be a real name, got '%s'." % [id, label])


func test_part_anim_motion_modes_are_catalog_subset() -> void:
	var catalog := _catalog_ids()
	for id in PartAnimsInspectorScript.MOTION_MODE_IDS:
		assert_true(catalog.has(int(id)), "Part-anim motion id %d should exist in the catalog." % int(id))
		assert_true(PartAnimsInspectorScript.MOTION_MODE_KEYS.has(int(id)), "Part-anim motion id %d should map to a mode string." % int(id))
	assert_eq(int(PartAnimsInspectorScript.MOTION_MODE_KEYS.size()), int(PartAnimsInspectorScript.MOTION_MODE_IDS.size()), "Motion id list and key map should be the same size.")
