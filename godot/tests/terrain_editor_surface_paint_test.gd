extends GutTest

const TerrainEditorSurfacePaint = preload("res://modtools/terrain/terrain_editor_surface_paint.gd")


func test_surface_paint_circle_overwrites_indices() -> void:
	var indices := PackedByteArray()
	indices.resize(16 * 16)
	indices.fill(TerrainEditorSurfacePaint.DEFAULT_SURFACE_INDEX)
	var palette := PackedByteArray()
	palette.resize(256 * 3)

	var surface_map := NovaTerrainSurfaceMap.new()
	surface_map.load_from_dictionary({
		"width": 16,
		"height": 16,
		"indices": indices,
		"palette": palette,
	})

	var changed := surface_map.paint_circle(8, 8, 3, 1.0, 1.0, 5)
	assert_true(changed, "Surface paint should report when it writes a new index.")
	assert_eq(surface_map.get_index(8, 8), 5, "Surface paint should write the selected index at the brush center.")
	assert_eq(surface_map.get_index(0, 0), TerrainEditorSurfacePaint.DEFAULT_SURFACE_INDEX, "Surface paint should not touch pixels outside the brush.")


func test_nova_terrain_data_charmap_state_roundtrips() -> void:
	var data := NovaTerrainData.new()
	data.reset_pcx_slot_default("charmap", 8, 8)
	var state: Dictionary = data.get_pcx_slot_state("charmap")
	assert_eq(int(state.get("width", 0)), 8, "Charmap state should preserve width.")
	assert_eq(int(state.get("height", 0)), 8, "Charmap state should preserve height.")

	var indices: PackedByteArray = state.get("indices", PackedByteArray())
	assert_eq(int(indices[0]), TerrainEditorSurfacePaint.DEFAULT_SURFACE_INDEX, "New charmap slots should default to the Dirt surface index.")

	indices[9] = 5
	state["indices"] = indices
	data.set_pcx_slot_state("charmap", state)

	var roundtrip: Dictionary = data.get_pcx_slot_state("charmap")
	var roundtrip_indices: PackedByteArray = roundtrip.get("indices", PackedByteArray())
	assert_eq(int(roundtrip_indices[9]), 5, "set_pcx_slot_state should round-trip edited charmap indices.")
	assert_not_null(data.get_charmap_tex(), "Editing charmap slot state should rebuild the preview texture.")


func test_surface_type_labels_match_charmap_legend() -> void:
	assert_eq(TerrainEditorSurfacePaint.get_surface_label(1), "Dirt", "Charmap index 1 should map to Dirt.")
	assert_eq(TerrainEditorSurfacePaint.get_surface_label(3), "Snow", "Charmap index 3 should map to Snow.")
	assert_eq(TerrainEditorSurfacePaint.get_surface_label(10), "Ice", "Charmap index 10 should map to Ice.")
	assert_eq(TerrainEditorSurfacePaint.get_surface_label(12), "Rock/Stone", "Charmap index 12 should map to Rock/Stone.")
