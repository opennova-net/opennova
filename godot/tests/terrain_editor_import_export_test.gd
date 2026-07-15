extends GutTest

const EditorMainScene = preload("res://modtools/editor/editor_main.tscn")
const TerrainEditorSlots = preload("res://modtools/terrain/terrain_editor_slots.gd")

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
	var editor = add_child_autofree(EditorMainScene.instantiate()).get_terrain_editor()

	assert_eq(editor.open_trn(_fixture_path("Dvxi5.trn")), OK, "Dvxi5 fixture should import through the editor.")
	assert_false(editor.is_dirty, "A no-edit import should remain clean before export.")

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


func test_uniformless_authored_slots_export_from_terrain_data() -> void:
	var document := TerrainEditorDocument.new()
	document.data = NovaTerrainData.new()
	document.colormap_image = _solid_image(Color8(1, 2, 3))
	document.blendmap_image = _solid_image(Color8(4, 5, 6))
	document.data.reset_pcx_slot_default("charmap", 2, 2)
	document.data.reset_pcx_slot_default("foliagemap", 2, 2)

	var expected_colors := {
		"detailmapdist": Color8(12, 34, 56),
		"detailmap2": Color8(78, 90, 123),
		"detailmapdist2": Color8(201, 45, 67),
	}
	for slot_id in expected_colors:
		var slot: Dictionary = TerrainEditorSlots.get_slot(String(slot_id))
		assert_eq(String(slot.get("uniform", "")), "", "%s is authored data, not a top-tier shader input." % slot_id)
		var texture := ImageTexture.create_from_image(_solid_image(expected_colors[slot_id]))
		document.data.call(String(slot["setter"]), texture)

	DirAccess.make_dir_recursive_absolute(_output_dir())
	var material := ShaderMaterial.new()
	assert_eq(document.save_texture_assets(material, _output_dir(), "Authored"), OK)

	for slot_id in expected_colors:
		var filename := TerrainEditorSlots.get_export_filename(String(slot_id), "Authored")
		var output_path := _output_dir().path_join(filename)
		assert_true(FileAccess.file_exists(output_path), "%s should survive export without a shader uniform." % filename)
		if not FileAccess.file_exists(output_path):
			continue
		var exported := Image.new()
		assert_eq(exported.load(output_path), OK, "%s should remain a readable TGA." % filename)
		assert_eq(exported.get_size(), Vector2i(2, 2), "%s should preserve the authored image dimensions." % filename)
		assert_eq(exported.get_pixel(0, 0).to_html(false), expected_colors[slot_id].to_html(false),
			"%s should preserve the authored pixel data." % filename)


func test_cptless_project_data_does_not_build_render_terrain() -> void:
	DirAccess.make_dir_recursive_absolute(_output_dir())
	var imported := NovaTerrainData.new()
	imported.set_trn_path(_fixture_path("Dvxi5.trn"))

	assert_eq(imported.load(), OK, "Fixture should load before saving a project-only TRN.")
	imported.set_polydata_filename("")

	var project_path := _output_dir().path_join("Cptless.trn")
	assert_eq(ResourceSaver.save(imported, project_path), OK, "Project-only TRN should save without CPT polydata.")

	var reopened := ResourceLoader.load(project_path, "NovaTerrainData", ResourceLoader.CACHE_MODE_IGNORE) as NovaTerrainData
	assert_not_null(reopened, "Project-only TRN should reopen as NovaTerrainData.")
	if reopened == null:
		return
	assert_true(reopened.is_loaded(), "Project-only TRN should report loaded even without CPT.")
	assert_eq(reopened.get_tile_count(), 0, "Project-only TRN should not expose baked CPT tiles.")

	var terrain: NovaTerrain = add_child_autofree(NovaTerrain.new())
	terrain.set_terrain_data(reopened)
	terrain.build()

	assert_eq(terrain.get_patches_active(), 0, "CPT-less data should not create render patch instances.")


func test_extensionless_tileinfo_filename_resolves_til_sidecar() -> void:
	DirAccess.make_dir_recursive_absolute(_output_dir())

	var source_tileinfo := NovaTerrainTileInfo.new()
	var entry := NovaTerrainTileEntry.new()
	entry.set_cell(2, 3)
	entry.set_tile_index(7)
	source_tileinfo.add_entry(entry)
	assert_eq(ResourceSaver.save(source_tileinfo, _output_dir().path_join("Overlay.til")), OK, "Fixture .til should save before sidecar lookup.")

	var data := NovaTerrainData.new()
	data.set_trn_path(_output_dir().path_join("OverlayMap.trn"))
	data.set_tileinfo_filename("Overlay")

	var loaded := data.get_tileinfo_resource()
	assert_not_null(loaded, "Extensionless tileinfo references should resolve to a .til sidecar.")
	if loaded == null:
		return
	assert_eq(loaded.get_entry_count(), 1, "Resolved tileinfo should load saved entries.")
	assert_eq(loaded.get_entry(0).get_tile_index(), 7, "Resolved tileinfo should preserve entry data.")

	assert_eq(ResourceSaver.save(source_tileinfo, _output_dir().path_join("Overlay.V1.til")), OK, "Dotted sidecar fixture should save.")
	data.set_tileinfo_filename("Overlay.V1")
	loaded = data.get_tileinfo_resource()
	assert_not_null(loaded, "Dotted tileinfo references should append .til without stripping the dotted stem.")
	if loaded != null:
		assert_eq(loaded.get_entry(0).get_tile_index(), 7, "Dotted sidecar lookup should preserve entry data.")


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


func _solid_image(color: Color) -> Image:
	var image := Image.create(2, 2, false, Image.FORMAT_RGBA8)
	image.fill(color)
	return image


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
