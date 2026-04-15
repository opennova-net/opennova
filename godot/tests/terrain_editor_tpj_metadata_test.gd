extends GutTest

const TerrainEditorDocument = preload("res://modtools/terrain/terrain_editor_document.gd")
const TerrainEditorSlots = preload("res://modtools/terrain/terrain_editor_slots.gd")
const _terrain_editor_script = preload("res://modtools/terrain/terrain_editor.gd")
const _editor_shader = preload("res://shaders/terrain_editor.gdshader")


func _make_tile_entry(cell_x: int, cell_z: int, tile_index: int, flags: int) -> NovaTerrainTileEntry:
	var entry := NovaTerrainTileEntry.new()
	entry.set_cell(cell_x, cell_z)
	entry.set_tile_index(tile_index)
	entry.set_flags(flags)
	return entry


func _make_foliage_def(
	graphic: String,
	match_index: int,
	color_lower: int = NovaTerrainFoliageDef.COLOR_MATCH_GROUND,
	color_upper: int = NovaTerrainFoliageDef.COLOR_MATCH_GROUND
) -> NovaTerrainFoliageDef:
	var def := NovaTerrainFoliageDef.new()
	def.graphic = graphic
	def.match = match_index
	def.color_lower = color_lower
	def.color_upper = color_upper
	return def


func test_parse_roundtrip() -> void:
	var tpj_path := _temp_path("rt.tpj")
	_write_tpj(tpj_path, [
		'terrainname     "MyMap"',
		'depthmap        "MyMap_depth.raw"',
		'charmap         "mymap_char.pcx"',
		'foliagemap      "mymap_f.pcx"',
		'tilestrip       "mymap_t.pcx"',
		'tileinfo        "mymap_layout"',
		'',
		'foliage',
		'  graphic         "tree01.3di"',
		'  color_lower     1',
		'  color_upper     2',
		'  match           0',
		'end',
		'',
		'foliage',
		'  graphic         "bush.3di"',
		'  color_lower     0',
		'  color_upper     1',
		'  match           1',
		'end',
	])

	var project := _load_tpj(tpj_path)
	_cleanup(tpj_path)

	if project == null:
		fail_test(".tpj resource load returned null for populated file")
		return
	assert_true(project.has_metadata, ".tpj has_metadata flag should be true when metadata present")
	assert_eq(String(project.charmap), "mymap_char.pcx", "charmap round-trip")
	assert_eq(String(project.foliagemap), "mymap_f.pcx", "foliagemap round-trip")
	assert_eq(String(project.tilestrip), "mymap_t.pcx", "tilestrip round-trip")
	assert_eq(String(project.tileinfo), "mymap_layout", "tileinfo round-trip")

	var defs: Array = project.foliage_defs
	if defs.size() != 2:
		fail_test("expected 2 foliage defs, got " + str(defs.size()))
		return
	var first := defs[0] as NovaTerrainFoliageDef
	var second := defs[1] as NovaTerrainFoliageDef
	assert_eq(String(first.graphic), "tree01.3di", "first foliage def graphic")
	assert_eq(int(first.match), 0, "first foliage def match")
	assert_eq(int(second.color_lower), NovaTerrainFoliageDef.COLOR_MATCH_GROUND, "second foliage def color_lower")
	assert_eq(int(second.color_upper), NovaTerrainFoliageDef.COLOR_BLEND_50, "second foliage def color_upper")


func test_legacy_tpj_lacks_metadata_flag() -> void:
	var tpj_path := _temp_path("legacy.tpj")
	_write_tpj(tpj_path, [
		'terrainname     "OldMap"',
		'creator         ""',
		'depthmap        "OldMap_depth.raw"',
		'output          "OldMap"',
	])

	var project := _load_tpj(tpj_path)
	_cleanup(tpj_path)

	if project == null:
		fail_test("legacy .tpj resource load returned null")
		return
	assert_false(project.has_metadata, "legacy .tpj must NOT set has_metadata flag")
	var defs: Array = project.foliage_defs
	assert_eq(defs.size(), 0, "legacy .tpj foliage_defs should default to empty")


func test_resource_saver_roundtrip() -> void:
	var tpj_path := _temp_path("resource_roundtrip.tpj")
	var project := NovaTerrainProject.new()
	project.terrain_name = "SavedMap"
	project.creator = "tester"
	project.project_path = "C:\\SavedMap\\"
	project.depthmap = "SavedMap_depth.raw"
	project.output = "SavedMap"
	project.charmap = "saved_char.pcx"
	project.foliagemap = "saved_f.pcx"
	project.tilestrip = "saved_t.pcx"
	project.tileinfo = "saved_layout"
	project.foliage_defs = [
		_make_foliage_def("tree01.3di", 3, NovaTerrainFoliageDef.COLOR_BLEND_50, NovaTerrainFoliageDef.COLOR_RETAIN_FULL),
	]
	project.has_metadata = true

	var save_err := ResourceSaver.save(project, tpj_path)
	if save_err != OK:
		fail_test("ResourceSaver.save should persist NovaTerrainProject")
		_cleanup(tpj_path)
		return

	var reloaded := _load_tpj(tpj_path)
	_cleanup(tpj_path)
	if reloaded == null:
		fail_test("saved NovaTerrainProject should reload")
		return

	assert_eq(String(reloaded.terrain_name), "SavedMap", "resource round-trip terrain_name")
	assert_eq(String(reloaded.project_path), "C:\\SavedMap\\", "resource round-trip project_path")
	assert_eq(String(reloaded.depthmap), "SavedMap_depth.raw", "resource round-trip depthmap")
	assert_eq(String(reloaded.charmap), "saved_char.pcx", "resource round-trip charmap")
	assert_true(reloaded.has_metadata, "resource round-trip has_metadata")
	assert_eq((reloaded.foliage_defs as Array).size(), 1, "resource round-trip foliage_defs size")


func test_document_prepare_data_for_trn_save_fields() -> void:
	var doc := TerrainEditorDocument.new()
	doc.data = NovaTerrainData.new()
	doc.data.set_terrain_name("Test")
	doc.data.set_sector_count(4)
	doc.data.set_sector_rows(4)
	doc.data.set_sector_grid(_zero_grid())
	# Slot-backed filenames round-trip via texture_files (charmap/foliagemap/tilestrip slots).
	doc.texture_files["charmap"] = "c.pcx"
	doc.texture_files["foliagemap"] = "f.pcx"
	doc.texture_files["tilestrip"] = "t.pcx"
	# tileinfo is not a slot — it's a direct document field.
	doc.tileinfo_filename = "i"
	var doc_defs: Array[NovaTerrainFoliageDef] = [
		_make_foliage_def("tree.3di", 0, NovaTerrainFoliageDef.COLOR_BLEND_50, NovaTerrainFoliageDef.COLOR_RETAIN_FULL),
	]
	doc.foliage_defs = doc_defs

	doc.prepare_data_for_trn_save("Test", "Test.cpt")
	assert_eq(String(doc.data.get_trn_texture_filename("charmap")), "c.pcx", "prepare_data_for_trn_save charmap")
	assert_eq(String(doc.data.get_trn_texture_filename("foliagemap")), "f.pcx", "prepare_data_for_trn_save foliagemap")
	assert_eq(String(doc.data.get_trn_texture_filename("tilestrip")), "t.pcx", "prepare_data_for_trn_save tilestrip")
	assert_eq(String(doc.data.get_trn_texture_filename("colormap")), "Test_c.tga", "prepare_data_for_trn_save colormap")
	assert_eq(String(doc.data.get_trn_texture_filename("detailblendmap")), "Test_d1.tga", "prepare_data_for_trn_save detailblendmap")
	assert_eq(String(doc.data.get_tileinfo_filename()), "i", "prepare_data_for_trn_save tileinfo")
	assert_eq(String(doc.data.get_polydata_filename()), "Test.cpt", "prepare_data_for_trn_save polydata")
	var out_defs: Array = doc.data.get_foliage_defs()
	var out_def: NovaTerrainFoliageDef = null
	if out_defs.size() > 0:
		out_def = out_defs[0] as NovaTerrainFoliageDef
	if out_defs.size() != 1 or out_def == null or String(out_def.graphic) != "tree.3di":
		fail_test("prepare_data_for_trn_save foliage_defs wrong")
		return
	assert_eq(int(out_def.match), 254, "prepare_data_for_trn_save should canonicalize foliage match")

	# Mutating the resource copy must not leak back into the document.
	out_def.graphic = "modified"
	assert_eq(String((doc.foliage_defs[0] as NovaTerrainFoliageDef).graphic), "tree.3di", "prepare_data_for_trn_save must deep-copy foliage_defs")


func test_editor_metadata_summary_uses_slot_filenames() -> void:
	var editor = _terrain_editor_script.new()
	editor._document = TerrainEditorDocument.new()
	editor.texture_files = {
		"charmap": "slot_char.pcx",
		"foliagemap": "slot_foliage.pcx",
		"tilestrip": "slot_tilestrip.tga",
	}
	editor._document.tileinfo_filename = "tileinfo_layout"
	var defs: Array[NovaTerrainFoliageDef] = [_make_foliage_def("tree.3di", 7)]
	editor._document.foliage_defs = defs

	var metadata := editor.get_metadata_summary()
	assert_eq(String(metadata.get("charmap", "")), "slot_char.pcx", "metadata summary charmap")
	assert_eq(String(metadata.get("foliagemap", "")), "slot_foliage.pcx", "metadata summary foliagemap")
	assert_eq(String(metadata.get("tilestrip", "")), "slot_tilestrip.tga", "metadata summary tilestrip")
	assert_eq(String(metadata.get("tileinfo", "")), "tileinfo_layout", "metadata summary tileinfo")

	editor.queue_free()


func test_til_resource_roundtrip() -> void:
	var til_path := _temp_path("roundtrip.til")
	var tileinfo := NovaTerrainTileInfo.new()
	# FLAG_OUTLINE (0x08) is an authored bit — expect it to round-trip alongside FLIP_X and ROTATE_90.
	tileinfo.entries = [_make_tile_entry(16, 32, 7, NovaTerrainTileInfo.FLAG_FLIP_X | NovaTerrainTileInfo.FLAG_ROTATE_90 | NovaTerrainTileInfo.FLAG_OUTLINE)]

	var save_err := ResourceSaver.save(tileinfo, til_path)
	if save_err != OK:
		fail_test("ResourceSaver.save should persist NovaTerrainTileInfo")
		_cleanup(til_path)
		return

	var reloaded := ResourceLoader.load(til_path, "NovaTerrainTileInfo", ResourceLoader.CACHE_MODE_IGNORE) as NovaTerrainTileInfo
	_cleanup(til_path)
	if reloaded == null:
		fail_test("saved NovaTerrainTileInfo should reload")
		return

	var entries: Array = reloaded.get_entries()
	var entry := entries[0] as NovaTerrainTileEntry
	assert_eq(reloaded.get_entry_count(), 1, ".til round-trip entry count")
	assert_not_null(entry, ".til round-trip entry object")
	assert_eq(entry.get_x_fixed(), 16 * NovaTerrainTileInfo.CELL_WORLD_SIZE * 65536, ".til round-trip x_fixed")
	assert_eq(entry.get_z_fixed(), -32 * NovaTerrainTileInfo.CELL_WORLD_SIZE * 65536, ".til round-trip z_fixed")
	assert_eq(entry.get_tile_index(), 7, ".til round-trip tile index")
	assert_eq(entry.get_flags(), NovaTerrainTileInfo.FLAG_FLIP_X | NovaTerrainTileInfo.FLAG_ROTATE_90 | NovaTerrainTileInfo.FLAG_OUTLINE, ".til round-trip flags (incl. FLAG_OUTLINE)")
	assert_false((entry.to_dictionary() as Dictionary).has("reserved"), ".til entry dictionary should not expose reserved padding")
	assert_eq(entry.get_cell_x(), 16, ".til round-trip cell_x")
	assert_eq(entry.get_cell_z(), 32, ".til round-trip cell_z")


func test_document_tileinfo_load_and_save_rules() -> void:
	var source_dir := _temp_dir("tileinfo_source")
	var export_dir := _temp_dir("tileinfo_export")
	_cleanup_dir(source_dir)
	_cleanup_dir(export_dir)
	DirAccess.make_dir_recursive_absolute(source_dir)
	DirAccess.make_dir_recursive_absolute(export_dir)

	var doc := TerrainEditorDocument.new()
	doc.load_tileinfo_from_dir(source_dir)
	var summary := doc.get_tileinfo_summary()
	assert_false(bool(summary.get("loaded", false)), "An empty tileinfo reference should not auto-load a default .til.")
	assert_eq(String(summary.get("state", "")), "", "An empty tileinfo reference should stay empty.")

	var err := doc.save_tileinfo(export_dir, "TerrainA")
	assert_eq(err, OK, "Saving with no tile layout should be a no-op.")
	assert_eq(doc.tileinfo_filename, "", "No tile layout should preserve an empty TPJ/TRN reference.")
	assert_false(FileAccess.file_exists(export_dir.path_join("TerrainA.til")), "No tile layout should not write a .til file.")

	doc.new_tileinfo()
	err = doc.save_tileinfo(export_dir, "TerrainA")
	assert_eq(err, OK, "new blank tileinfo should save")
	assert_eq(doc.tileinfo_filename, "TerrainA", "blank tileinfo should save as <terrain_name>.til")
	assert_true(FileAccess.file_exists(export_dir.path_join("TerrainA.til")), "blank tileinfo should write <terrain_name>.til")

	_cleanup_dir(source_dir)
	_cleanup_dir(export_dir)


func test_document_tileinfo_single_cell_replace_keeps_one_entry() -> void:
	var doc := TerrainEditorDocument.new()
	doc.new_tileinfo()
	doc.set_tile_stamp_tile_index(1)
	doc.stamp_tileinfo_cell(4, 5)
	doc.set_tile_stamp_tile_index(2)

	var result := doc.stamp_tileinfo_cell(4, 5, true)
	var indices := doc.get_tileinfo_entry_indices_at_cell(4, 5)

	assert_true(bool(result.get("changed", false)), "Stamping an occupied cell should still report a replacement.")
	assert_eq(doc.tileinfo_resource.get_entry_count(), 1, "Tile mode should keep only one entry per cell.")
	assert_eq(indices.size(), 1, "The cell should expose a single surviving tile entry.")
	assert_eq(doc.get_tileinfo_selected_index(), 0, "Replacing a tile should keep the replacement selected.")
	assert_eq(doc.get_tileinfo_entry(0).get_tile_index(), 2, "The surviving tile should use the latest stamp tile index.")


func test_document_normalize_tileinfo_for_editor_flattens_stacked_cells() -> void:
	var doc := TerrainEditorDocument.new()
	doc.new_tileinfo()
	doc.tileinfo_resource.set_entries([
		_make_tile_entry(4, 5, 1, 0),
		_make_tile_entry(4, 5, 2, 0),
		_make_tile_entry(6, 7, 3, 0),
	])

	var removed_count := doc.normalize_tileinfo_for_editor()

	assert_eq(removed_count, 1, "Legacy stacked tiles should be flattened to one tile per cell.")
	assert_eq(doc.tileinfo_resource.get_entry_count(), 2, "Only the newest tile per cell should remain after normalization.")
	assert_eq(doc.find_tileinfo_entry_index_at_cell(4, 5), 0, "The surviving stacked cell should still be addressable.")
	assert_eq(doc.get_tileinfo_entry(0).get_tile_index(), 2, "Normalization should keep the newest tile in a stacked cell.")


func test_editor_tile_selected_entry_edit_helpers_update_selected_tile() -> void:
	var editor = _terrain_editor_script.new()
	editor._document = TerrainEditorDocument.new()
	editor._document.new_tileinfo()
	editor._document.set_tile_stamp_tile_index(1)
	editor._document.stamp_tileinfo_cell(2, 3)
	editor.select_tileinfo_entry(0)

	assert_true(editor.has_selected_tileinfo_entry(), "Tile edit helpers should operate on an active selected overlay.")
	assert_true(editor.replace_selected_tileinfo_tile_index(7), "Replacing the selected tile index should report a change.")
	assert_eq(editor.get_selected_tileinfo_entry().get_tile_index(), 7, "Replacing the selected tile index should update the selected overlay.")
	assert_eq(editor.get_tile_stamp_tile_index(), 1, "Selecting and replacing a placed tile should not overwrite the current brush tile.")

	assert_true(editor.rotate_selected_tileinfo_clockwise(), "Rotate should update the selected overlay.")
	assert_eq(editor.get_selected_tileinfo_entry().get_flags(), NovaTerrainTileInfo.FLAG_ROTATE_90, "One rotate should produce the 90-degree authored transform.")
	assert_true(editor.rotate_selected_tileinfo_clockwise(), "A second rotate should continue composing from the current orientation.")
	assert_eq(
		editor.get_selected_tileinfo_entry().get_flags(),
		NovaTerrainTileInfo.FLAG_FLIP_X | NovaTerrainTileInfo.FLAG_FLIP_Y,
		"Two rotates should resolve to the 180-degree authored transform."
	)
	assert_true(editor.rotate_selected_tileinfo_clockwise(), "A third rotate should continue composing from the current orientation.")
	assert_eq(
		editor.get_selected_tileinfo_entry().get_flags(),
		NovaTerrainTileInfo.FLAG_FLIP_X | NovaTerrainTileInfo.FLAG_FLIP_Y | NovaTerrainTileInfo.FLAG_ROTATE_90,
		"Three rotates should resolve to the 270-degree authored transform."
	)
	assert_true(editor.rotate_selected_tileinfo_clockwise(), "A fourth rotate should return to the original orientation.")
	assert_eq(editor.get_selected_tileinfo_entry().get_flags(), 0, "Four rotates should wrap back to the identity transform.")

	assert_true(editor.flip_selected_tileinfo_x(), "Flip X should update the selected overlay.")
	assert_eq(editor.get_selected_tileinfo_entry().get_flags(), NovaTerrainTileInfo.FLAG_FLIP_X, "Flip X should set the authored X-flip bit.")
	assert_eq(editor.get_tile_stamp_flags(), 0, "Selected-tile edits should not rewrite the stored brush transform state.")


func test_document_delete_selected_tileinfo_entry_clears_selection() -> void:
	var doc := TerrainEditorDocument.new()
	doc.new_tileinfo()
	doc.set_tile_stamp_tile_index(3)
	doc.stamp_tileinfo_cell(1, 2)
	doc.set_tileinfo_selected_index(0)

	assert_true(doc.delete_selected_tileinfo_entry(), "Deleting the selected tile should succeed.")
	assert_eq(doc.get_tileinfo_selected_index(), -1, "Deleting a selected tile should return Tile mode to placement with no replacement selection.")


func test_document_load_texture_slot_imports_pcx_indices() -> void:
	var doc := TerrainEditorDocument.new()
	doc.data = NovaTerrainData.new()
	var material := _make_material()
	assert_true(doc.load_texture_slot(material, "charmap", "res://game/assets/terrains/Dvxi5/Dvxi5_m.pcx"), "load_texture_slot should import the charmap fixture")
	assert_true(doc.load_texture_slot(material, "foliagemap", "res://game/assets/terrains/Dvxi5/Dvxi5_f.pcx"), "load_texture_slot should import the foliagemap fixture")

	for slot_id in ["charmap", "foliagemap"]:
		var save_path := _temp_path("%s_slot_roundtrip.pcx" % slot_id)
		var save_err := doc.data.save_pcx_slot(slot_id, save_path)
		if save_err != OK:
			fail_test("load_texture_slot should initialize %s PCX data" % slot_id)
			_cleanup(save_path)
			continue

		var reloaded := NovaTerrainData.new()
		var import_err := reloaded.import_pcx_slot(slot_id, save_path)
		if import_err != OK:
			fail_test("re-import of saved %s PCX should succeed" % slot_id)
			_cleanup(save_path)
			continue

		var source_texture := TerrainEditorSlots.get_slot_texture(doc.data, slot_id)
		var reloaded_texture := TerrainEditorSlots.get_slot_texture(reloaded, slot_id)
		assert_true(_textures_match(source_texture, reloaded_texture), "load_texture_slot must preserve %s PCX payload" % slot_id)

		_cleanup(save_path)


func test_document_reset_pcx_slot_clears_saved_payload() -> void:
	var doc := TerrainEditorDocument.new()
	doc.data = NovaTerrainData.new()
	var material := _make_material()
	var source_path := "res://game/assets/terrains/Dvxi5/Dvxi5_f.pcx"
	if not doc.load_texture_slot(material, "foliagemap", source_path):
		fail_test("load_texture_slot should import foliagemap fixture")
		return

	doc.reset_texture_slot(material, "foliagemap")
	assert_false(doc.texture_files.has("foliagemap"), "reset_texture_slot should clear slot-backed foliagemap filename")

	var save_path := _temp_path("foliagemap_reset_roundtrip.pcx")
	var save_err := doc.data.save_pcx_slot("foliagemap", save_path)
	if save_err != OK:
		fail_test("reset foliagemap should still save a default PCX payload")
		_cleanup(save_path)
		return

	var reloaded := NovaTerrainData.new()
	if reloaded.import_pcx_slot("foliagemap", save_path) != OK:
		fail_test("re-import of reset foliagemap PCX should succeed")
		_cleanup(save_path)
		return

	var expected := NovaTerrainData.new()
	expected.reset_pcx_slot_default("foliagemap", 1024, 1024)
	assert_true(_textures_match(reloaded.get_foliagemap_tex(), expected.get_foliagemap_tex()), "reset_texture_slot must restore the default foliagemap payload")

	_cleanup(save_path)


func test_nova_terrain_data_load_preserves_dvxi5_slot_state() -> void:
	pending("previously orphaned: never called from the old run() — full Dvxi5 PCX round-trip flake under investigation")
	return
	var data := NovaTerrainData.new()
	data.set_trn_path("res://game/assets/terrains/Dvxi5/Dvxi5.trn")
	if data.load() != OK:
		fail_test("NovaTerrainData.load should load the Dvxi5 fixture")
		return

	assert_not_null(data.get_foliagemap_tex(), "NovaTerrainData.load should populate the Dvxi5 foliagemap preview")
	assert_not_null(data.get_tilestrip_tex(), "NovaTerrainData.load should preserve the Dvxi5 tilestrip filename through PCX slot imports")

	var foliage_indices: Dictionary = data.load_foliage_indices()
	assert_false(foliage_indices.is_empty(), "NovaTerrainData.load should preserve trn.foliagemap for load_foliage_indices()")

	var save_path := _temp_path("dvxi5_loaded_foliagemap_roundtrip.pcx")
	if data.save_pcx_slot("foliagemap", save_path) != OK:
		fail_test("loaded Dvxi5 foliagemap should save back out through save_pcx_slot()")
		_cleanup(save_path)
		return

	var expected := NovaTerrainData.new()
	if expected.import_pcx_slot("foliagemap", "res://game/assets/terrains/Dvxi5/Dvxi5_f.pcx") != OK:
		fail_test("fixture import should succeed for Dvxi5_f.pcx")
		_cleanup(save_path)
		return

	var reloaded := NovaTerrainData.new()
	if reloaded.import_pcx_slot("foliagemap", save_path) != OK:
		fail_test("saved Dvxi5 foliage round-trip PCX should re-import")
		_cleanup(save_path)
		return

	assert_true(_textures_match(reloaded.get_foliagemap_tex(), expected.get_foliagemap_tex()), "NovaTerrainData.load must preserve the Dvxi5 foliage slot payload")

	_cleanup(save_path)


func test_document_save_texture_assets_preserves_dvxi5_foliagemap() -> void:
	pending("previously orphaned: never called from the old run() — full save_texture_assets round-trip flake under investigation")
	return
	var doc := TerrainEditorDocument.new()
	doc.data = NovaTerrainData.new()
	doc.data.set_trn_path("res://game/assets/terrains/Dvxi5/Dvxi5.trn")
	var output_dir := _temp_dir("dvxi5_save_project")
	_cleanup_dir(output_dir)
	DirAccess.make_dir_recursive_absolute(output_dir)

	if doc.data.load() != OK:
		fail_test("document save fixture should load the Dvxi5 TRN data")
		return

	var material := _make_material()
	doc.apply_default_visual_state(material, false)
	doc.apply_loaded_textures_from_data(material)

	var err := doc.save_texture_assets(material, output_dir, "Dvxi5")
	if err != OK:
		fail_test("document save path should export texture assets for the Dvxi5 fixture")
		_cleanup_dir(output_dir)
		return

	var expected := NovaTerrainData.new()
	if expected.import_pcx_slot("foliagemap", "res://game/assets/terrains/Dvxi5/Dvxi5_f.pcx") != OK:
		fail_test("fixture import should succeed for Dvxi5_f.pcx")
		_cleanup_dir(output_dir)
		return

	var actual := NovaTerrainData.new()
	var saved_foliage_path := output_dir.path_join("Dvxi5_f.pcx")
	if actual.import_pcx_slot("foliagemap", saved_foliage_path) != OK:
		fail_test("saved Dvxi5_f.pcx should re-import after save_texture_assets()")
		_cleanup_dir(output_dir)
		return

	assert_true(_textures_match(actual.get_foliagemap_tex(), expected.get_foliagemap_tex()), "save_texture_assets() must preserve the imported Dvxi5 foliage map payload")

	_cleanup_dir(output_dir)


func test_editor_add_remove_foliage_def() -> void:
	var editor = _terrain_editor_script.new()
	editor._document = TerrainEditorDocument.new()

	editor.add_foliage_def()
	editor.add_foliage_def()
	assert_eq(editor._document.foliage_defs.size(), 2, "add_foliage_def should append two entries")

	# Default def shape
	var first := editor._document.foliage_defs[0] as NovaTerrainFoliageDef
	var second := editor._document.foliage_defs[1] as NovaTerrainFoliageDef
	if first == null or second == null or String(first.graphic) != "" or int(first.match) != 254 or int(second.match) != 253:
		fail_test("add_foliage_def default values wrong")

	assert_true(editor.is_document_dirty(), "add_foliage_def should mark document dirty")

	# Bound check — hard cap at 4
	editor.add_foliage_def()
	editor.add_foliage_def()
	editor.add_foliage_def()  # This one should be a no-op
	assert_eq(editor._document.foliage_defs.size(), editor.FOLIAGE_DEFS_LIMIT, "add_foliage_def past limit should be no-op")

	editor.remove_foliage_def(1)
	assert_eq(editor._document.foliage_defs.size(), 3, "remove_foliage_def(1) should shrink to 3 entries")

	editor.remove_foliage_def(-1)  # Invalid
	editor.remove_foliage_def(99)  # Out of range
	assert_eq(editor._document.foliage_defs.size(), 3, "remove_foliage_def with invalid index should be no-op")

	editor.queue_free()


func test_editor_remove_foliage_def_remaps_foliage_map() -> void:
	var editor = _terrain_editor_script.new()
	editor._document = TerrainEditorDocument.new()
	editor._document.foliage_map = NovaTerrainFoliageMap.new()
	editor._document.foliage_map.set_size(16, 16)

	editor.add_foliage_def()
	editor.add_foliage_def()
	editor.add_foliage_def()
	editor._document.foliage_map.set_index(1, 1, 254)
	editor._document.foliage_map.set_index(2, 2, 253)
	editor._document.foliage_map.set_index(3, 3, 252)

	editor.remove_foliage_def(1)

	assert_eq(int(editor._document.foliage_map.get_index(1, 1)), 254, "removing slot 1 should preserve slot 0 pixels")
	assert_eq(int(editor._document.foliage_map.get_index(2, 2)), 0, "removing slot 1 should erase its painted pixels")
	assert_eq(int(editor._document.foliage_map.get_index(3, 3)), 253, "removing slot 1 should compact later slot pixels")

	editor.queue_free()


func test_editor_set_foliage_def_field() -> void:
	var editor = _terrain_editor_script.new()
	editor._document = TerrainEditorDocument.new()
	editor.add_foliage_def()

	editor.set_foliage_def_field(0, "graphic", "tree.3di")
	editor.set_foliage_def_field(0, "color_lower", 200)
	editor.set_foliage_def_field(0, "color_upper", 210)
	editor.set_foliage_def_field(0, "match", 0)

	var def := editor._document.foliage_defs[0] as NovaTerrainFoliageDef
	assert_eq(String(def.graphic), "tree.3di", "set_foliage_def_field graphic")
	assert_eq(int(def.color_lower), NovaTerrainFoliageDef.COLOR_RETAIN_FULL, "set_foliage_def_field color_lower")
	assert_eq(int(def.color_upper), NovaTerrainFoliageDef.COLOR_RETAIN_FULL, "set_foliage_def_field color_upper")
	assert_eq(int(def.match), 254, "match should remain editor-managed")

	# Out-of-range index = no-op
	editor.set_foliage_def_field(5, "graphic", "invalid")
	assert_eq(editor._document.foliage_defs.size(), 1, "set_foliage_def_field with bad index should not grow array")

	editor.queue_free()


func _temp_path(name: String) -> String:
	return "user://" + name


func _load_tpj(path: String) -> NovaTerrainProject:
	return ResourceLoader.load(path, "NovaTerrainProject", ResourceLoader.CACHE_MODE_IGNORE) as NovaTerrainProject


func _write_tpj(path: String, lines: Array) -> void:
	var file: FileAccess = FileAccess.open(path, FileAccess.WRITE)
	for line in lines:
		file.store_line(String(line))
	file.close()


func _cleanup(path: String) -> void:
	if FileAccess.file_exists(path):
		DirAccess.remove_absolute(ProjectSettings.globalize_path(path))


func _make_material() -> ShaderMaterial:
	var material := ShaderMaterial.new()
	material.shader = _editor_shader
	return material


func _textures_match(left: Texture2D, right: Texture2D) -> bool:
	if left == null or right == null:
		return left == right
	var left_image := left.get_image()
	var right_image := right.get_image()
	if left_image == null or right_image == null:
		return left_image == right_image
	return (
		left_image.get_width() == right_image.get_width()
		and left_image.get_height() == right_image.get_height()
		and left_image.get_data() == right_image.get_data()
	)


func _zero_grid() -> PackedInt32Array:
	var grid := PackedInt32Array()
	grid.resize(256)
	return grid


func _temp_dir(name: String) -> String:
	return ProjectSettings.globalize_path("user://" + name)


func _cleanup_dir(path: String) -> void:
	var dir := DirAccess.open(path)
	if dir == null:
		return

	dir.list_dir_begin()
	while true:
		var entry := dir.get_next()
		if entry.is_empty():
			break
		if entry == "." or entry == "..":
			continue
		var child_path := path.path_join(entry)
		if dir.current_is_dir():
			_cleanup_dir(child_path)
		else:
			DirAccess.remove_absolute(child_path)
	dir.list_dir_end()
	DirAccess.remove_absolute(path)
