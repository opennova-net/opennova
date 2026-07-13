extends GutTest


func test_editor_normalization_preserves_shared_match_semantics() -> void:
	var document := TerrainEditorDocument.new()
	var first := NovaTerrainFoliageDef.new()
	first.matches = PackedInt32Array([7, 8, -1, -1])
	var second := NovaTerrainFoliageDef.new()
	second.matches = PackedInt32Array([7, 9, -1, -1])
	document.foliage_defs = [first, second]
	document.foliage_map = NovaTerrainFoliageMap.new()
	document.foliage_map.set_size(1, 1)
	document.foliage_map.set_index(0, 0, 7)

	document.normalize_foliage_state_for_editor()

	assert_has(first.matches, 7,
			"The first definition still accepts the shared authored map byte.")
	assert_has(second.matches, 7,
			"The second definition still accepts the shared authored map byte.")
	assert_eq(document.foliage_map.get_index(0, 0), 7,
			"Normalization does not orphan pixels shared by multiple definitions.")
	assert_eq(document.find_foliage_def_index_by_match(9), 1,
			"The editor eyedropper recognizes secondary authored match values.")
	document.set_selected_foliage_def_index(1)
	assert_eq(document.get_selected_foliage_paint_index(), 7,
			"Painting uses the first usable authored value without canonicalizing the definition.")


func test_foliage_definition_history_preserves_all_four_matches() -> void:
	var document := TerrainEditorDocument.new()
	var def := NovaTerrainFoliageDef.new()
	def.matches = PackedInt32Array([11, 12, 13, 14])
	document.foliage_defs = [def]

	var snapshot := document.capture_foliage_defs_history_state()
	document.restore_foliage_defs_history_state(snapshot)

	assert_eq(document.foliage_defs[0].matches, PackedInt32Array([11, 12, 13, 14]),
			"Undo/redo snapshots retain every retail match byte.")


func test_removing_definition_only_clears_unclaimed_match_pixels() -> void:
	var document := TerrainEditorDocument.new()
	var first := NovaTerrainFoliageDef.new()
	first.matches = PackedInt32Array([7, 8, -1, -1])
	var second := NovaTerrainFoliageDef.new()
	second.matches = PackedInt32Array([7, 9, -1, -1])
	document.foliage_defs = [first, second]
	document.foliage_map = NovaTerrainFoliageMap.new()
	document.foliage_map.set_size(2, 1)
	document.foliage_map.set_index(0, 0, 7)
	document.foliage_map.set_index(1, 0, 8)

	assert_true(document.remove_foliage_def(0))

	assert_eq(document.foliage_map.get_index(0, 0), 7,
			"A match shared by the remaining definition stays painted.")
	assert_eq(document.foliage_map.get_index(1, 0), 0,
			"A match owned only by the removed definition is cleared.")
