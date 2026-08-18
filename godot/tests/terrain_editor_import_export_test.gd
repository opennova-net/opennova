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
	document.data = TerrainData.new()
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
	var imported := TerrainData.new()
	imported.set_trn_path(_fixture_path("Dvxi5.trn"))

	assert_eq(imported.load(), OK, "Fixture should load before saving a project-only TRN.")
	imported.set_polydata_filename("")

	var project_path := _output_dir().path_join("Cptless.trn")
	assert_eq(imported.save_to_path(project_path), OK, "Project-only TRN should save without CPT polydata.")

	var reopened := TerrainData.new()
	reopened.set_trn_path(project_path)
	assert_eq(reopened.load(), OK, "Project-only TRN should reopen as TerrainData.")
	assert_true(reopened.is_loaded(), "Project-only TRN should report loaded even without CPT.")
	assert_eq(reopened.get_tile_count(), 0, "Project-only TRN should not expose baked CPT tiles.")

	var terrain: Terrain = add_child_autofree(Terrain.new())
	terrain.set_terrain_data(reopened)
	terrain.build()

	assert_eq(terrain.get_patches_active(), 0, "CPT-less data should not create render patch instances.")


# A BHD-era terrain authors ONE detail texture (`polytrn_detailmap`) and no
# `polytrn_detailmap_c1..c3`/`detailmapdist`, and ships `terrain_name ""`.
# Opening it must promote that single detail into Detail A/B/C + the far
# target (so a JO/DFX export bakes the detail instead of the gray placeholder)
# and name the terrain after the file stem; an authored splat set is untouched.
func test_legacy_single_detail_trn_promotes_into_splat_slots() -> void:
	var legacy_dir := _output_dir().path_join("legacy")
	DirAccess.make_dir_recursive_absolute(legacy_dir)
	for filename in ["Dvxi5_c.tga", "Dvxi5_dm.tga", "Dvxi5_m.pcx", "Dvxi5_f.pcx", "TRNTILE10.TGA"]:
		assert_eq(DirAccess.copy_absolute(_fixture_path(filename), legacy_dir.path_join(filename)), OK,
			"%s should copy next to the legacy .trn." % filename)
	var legacy_trn := FileAccess.open(legacy_dir.path_join("Oldmap.trn"), FileAccess.WRITE)
	legacy_trn.store_string("\n".join([
		'terrain_name     ""',
		"water_height      0",
		"polytrn_colormap    Dvxi5_c.tga",
		"polytrn_detailmap    Dvxi5_dm.tga",
		"polytrn_tilestrip        trntile10.tga",
		"polytrn_charmap      Dvxi5_m.pcx",
		"polytrn_foliagemap   Dvxi5_f.pcx",
		"polytrn_detaildensity         128",
		"polytrn_sectorcount	        8",
		"polytrn_wrapx	        1",
		"polytrn_wrapy	        1",
		"polytrn_origin			-4	-4",
		"",
	]))
	legacy_trn.close()

	var editor = add_child_autofree(EditorMainScene.instantiate()).get_terrain_editor()
	assert_eq(editor.open_trn(legacy_dir.path_join("Oldmap.trn")), OK, "Legacy single-detail .trn should open through the editor.")
	assert_true(editor.is_dirty, "Promotion changes the document relative to disk.")
	assert_eq(editor.get_terrain_name_value(), "Oldmap", "An empty terrain_name is seeded from the .trn file stem.")

	var detail := Image.new()
	assert_eq(detail.load(_fixture_path("Dvxi5_dm.tga")), OK)
	var expected := detail.get_pixel(3, 5).to_html(false)
	var placeholder := TerrainEditorSlots.FAR_DETAIL_PLACEHOLDER_COLOR.to_html(false)
	assert_ne(expected, placeholder, "Fixture detail must be distinguishable from the export placeholder.")
	for slot_id in ["detail_c1", "detail_c2", "detail_c3", "detailmapdist"]:
		var texture: Texture2D = TerrainEditorSlots.get_slot_texture(editor._data, slot_id)
		assert_not_null(texture, "%s should be seeded from the single detail texture." % slot_id)
		if texture == null:
			continue
		assert_eq(texture.get_image().get_pixel(3, 5).to_html(false), expected,
			"%s should carry the single detail's pixels." % slot_id)

	# The preview must bind an all-A blend map on TerrainData (not only the
	# material): a null u_blendmap samples Godot's white default and sums all
	# three layers into a x3 blow-out on the x4 splat stage.
	var bound_blend: Texture2D = editor._get_material().get_shader_parameter("u_blendmap")
	assert_not_null(bound_blend, "A blend-map-less terrain still binds a blend map after the surface-inputs rebuild.")
	if bound_blend != null:
		assert_eq(bound_blend.get_image().get_pixel(3, 5).to_html(false), Color(1, 0, 0).to_html(false),
			"The default blend map selects layer A everywhere (retail's no-blend-map outcome).")
	assert_not_null(editor._data.get_detailblendmap(), "TerrainData carries the default blend map so rebuilds keep it.")

	# The exported splat set bakes that detail, not the gray placeholder.
	var out_dir := _output_dir().path_join("legacy_out")
	DirAccess.make_dir_recursive_absolute(out_dir)
	assert_eq(editor._document.save_texture_assets(editor._get_material(), out_dir, "Oldmap"), OK)
	for filename in ["Oldmap_dc1.tga", "Oldmap_dc2.tga", "Oldmap_dc3.tga", "Oldmap_dmd.tga"]:
		var exported := Image.new()
		assert_eq(exported.load(out_dir.path_join(filename)), OK, "%s should export." % filename)
		assert_eq(exported.get_pixel(3, 5).to_html(false), expected, "%s should bake the promoted detail." % filename)
	assert_true(FileAccess.file_exists(out_dir.path_join("Dvxi5_dm.tga")),
		"The authored detail stays exported under its own name as the JO coefficient source.")

	# An authored splat set (Dvxi5 itself) is never overwritten by the promotion.
	assert_eq(editor.open_trn(_fixture_path("Dvxi5.trn")), OK)
	assert_false(editor.is_dirty, "A fully authored JO terrain must not be promoted or renamed.")
	var c2 := Image.new()
	assert_eq(c2.load(_fixture_path("Dvxi5_dc2.tga")), OK)
	assert_eq(TerrainEditorSlots.get_slot_texture(editor._data, "detail_c2").get_image().get_pixel(3, 5).to_html(false),
		c2.get_pixel(3, 5).to_html(false), "Authored Detail B stays the authored texture.")

	_cleanup_dir(out_dir)
	_cleanup_dir(legacy_dir)


func test_extensionless_tileinfo_filename_resolves_til_sidecar() -> void:
	DirAccess.make_dir_recursive_absolute(_output_dir())

	var source_tileinfo := TerrainTileInfo.new()
	var entry := TerrainTileEntry.new()
	entry.set_cell(2, 3)
	entry.set_tile_index(7)
	source_tileinfo.add_entry(entry)
	assert_eq(source_tileinfo.save_to_path(_output_dir().path_join("Overlay.til")), OK, "Fixture .til should save before sidecar lookup.")

	var data := TerrainData.new()
	data.set_trn_path(_output_dir().path_join("OverlayMap.trn"))
	data.set_tileinfo_filename("Overlay")

	var loaded := data.get_tileinfo_resource()
	assert_not_null(loaded, "Extensionless tileinfo references should resolve to a .til sidecar.")
	if loaded == null:
		return
	assert_eq(loaded.get_entry_count(), 1, "Resolved tileinfo should load saved entries.")
	assert_eq(loaded.get_entry(0).get_tile_index(), 7, "Resolved tileinfo should preserve entry data.")

	assert_eq(source_tileinfo.save_to_path(_output_dir().path_join("Overlay.V1.til")), OK, "Dotted sidecar fixture should save.")
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
