extends GutTest

## Drift guard: the editor's GeneratorStyleCatalog (friendly names for the NovaLogic
## generator-style byte) must stay in lockstep with the canonical C++ enum
## kControlEntries in libs/threedi/src/threedi_panm.cpp. This is what failed
## silently before (materials mislabeled 50 as "square"; lights showed "Custom 55").

const PartAnimsInspectorScript = preload("res://modtools/object/ui/inspectors/part_anims_inspector.gd")


func _catalog_ids() -> Array:
	var ids := []
	for id in GeneratorStyleCatalog.style_ids():
		ids.append(int(id))
	return ids


func _label(consumer: String, id: int) -> String:
	var info := GeneratorStyleCatalog.style_info(consumer, id)
	return info.label if info != null else ""


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
	for id in GeneratorStyleCatalog.style_ids():
		var info := GeneratorStyleCatalog.style_info(GeneratorStyleCatalog.CONSUMER_UV, id)
		assert_not_null(info, "Style %d should have typed metadata." % id)
		var label := info.label if info != null else ""
		assert_false(label.is_empty(), "Style %d should have a label." % id)
		assert_false(label.begins_with("Custom"), "Style %d label should be a real name, got '%s'." % [id, label])


func test_control_value_read_dispatch_is_consumer_specific() -> void:
	var expected := {
		GeneratorStyleCatalog.CONSUMER_UV: [113, 114, 115, 116, 117],
		GeneratorStyleCatalog.CONSUMER_RGB: [113, 114],
		GeneratorStyleCatalog.CONSUMER_ALPHA: [113],
		GeneratorStyleCatalog.CONSUMER_LIGHT: [113, 114],
		GeneratorStyleCatalog.CONSUMER_PANM: [113],
	}
	for consumer in expected:
		for id in range(113, 118):
			assert_eq(
					GeneratorStyleCatalog.reads_control_value(consumer, id),
					(expected[consumer] as Array).has(id),
					"%s style %d should match the witnessed retail dispatch." % [
						consumer, id,
					])


func test_loader_parameter_remains_a_ctrl_reference_for_every_high_style() -> void:
	for id in range(0, 256):
		assert_eq(
				GeneratorStyleCatalog.parameter_is_ctrl_reference(id),
				id > 0x70,
				"Style %d should follow the loader's >0x70 parameter fixup." % id)


func test_control_style_labels_follow_each_consumer() -> void:
	assert_eq(_label(GeneratorStyleCatalog.CONSUMER_UV, 114),
			"Add (control register)")
	assert_eq(_label(GeneratorStyleCatalog.CONSUMER_UV, 117),
			"Rotate (control register)")

	assert_eq(_label(GeneratorStyleCatalog.CONSUMER_RGB, 114),
			"Add (control register)")
	assert_eq(_label(GeneratorStyleCatalog.CONSUMER_RGB, 115),
			"Wave: triangle")
	assert_eq(_label(GeneratorStyleCatalog.CONSUMER_LIGHT, 117),
			"Wave: inverse saw")

	assert_eq(_label(GeneratorStyleCatalog.CONSUMER_ALPHA, 113),
			"Set (control register)")
	assert_eq(_label(GeneratorStyleCatalog.CONSUMER_ALPHA, 114),
			"Wave: sine")
	assert_eq(_label(GeneratorStyleCatalog.CONSUMER_PANM, 116),
			"Wave: saw")


func test_consumer_options_carry_dispatch_metadata() -> void:
	for consumer in GeneratorStyleCatalog.CONSUMERS:
		var options := GeneratorStyleCatalog.options_for_consumer(consumer)
		assert_eq(options.size(), GeneratorStyleCatalog.style_count())
		for option in options:
			var id := option.id
			assert_eq(
					option.reads_control_value,
					GeneratorStyleCatalog.reads_control_value(consumer, id))
			assert_eq(
					option.parameter_is_ctrl_reference,
					GeneratorStyleCatalog.parameter_is_ctrl_reference(id))


func test_typed_catalog_options_populate_inspector_dropdown() -> void:
	var dropdown: OptionButton = add_child_autofree(OptionButton.new())
	InspectorForms.populate_id_option(
			dropdown,
			GeneratorStyleCatalog.options_for_ids(
					GeneratorStyleCatalog.CONSUMER_ALPHA,
					[
						GeneratorStyleCatalog.STYLE_CONTROL_SET,
						GeneratorStyleCatalog.STYLE_CONTROL_ADD,
					]),
			GeneratorStyleCatalog.STYLE_CONTROL_ADD)

	assert_eq(dropdown.get_item_count(), 2)
	assert_eq(dropdown.get_item_id(0), GeneratorStyleCatalog.STYLE_CONTROL_SET)
	assert_eq(dropdown.get_item_text(0), "Set (control register)")
	assert_eq(dropdown.get_item_id(1), GeneratorStyleCatalog.STYLE_CONTROL_ADD)
	assert_eq(dropdown.get_item_text(1), "Wave: sine")
	assert_eq(dropdown.get_selected_id(), GeneratorStyleCatalog.STYLE_CONTROL_ADD)


func test_part_anim_motion_modes_are_catalog_subset() -> void:
	var catalog := _catalog_ids()
	for id in PartAnimsInspectorScript.MOTION_MODE_IDS:
		assert_true(catalog.has(int(id)), "Part-anim motion id %d should exist in the catalog." % int(id))
		assert_true(PartAnimsInspectorScript.MOTION_MODE_KEYS.has(int(id)), "Part-anim motion id %d should map to a mode string." % int(id))
	assert_eq(int(PartAnimsInspectorScript.MOTION_MODE_KEYS.size()), int(PartAnimsInspectorScript.MOTION_MODE_IDS.size()), "Motion id list and key map should be the same size.")
	assert_false(PartAnimsInspectorScript.MOTION_MODE_IDS.has(114),
			"Retail PANM treats code 114 as sine wave lookup, not an add-control-register authoring mode.")
