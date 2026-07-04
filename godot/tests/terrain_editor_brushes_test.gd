extends GutTest

## Brush-session behaviour, now driven through the C++ kernels on NovaTerrainData
## (terrain_editor_brushes.gd was removed in A4). NovaTerrainData owns the editable
## buffers; set_*_image hands it the SAME Image the test asserts on, so the in-place
## set_data round-trip is observed here. Byte-parity vs the old GDScript is locked by
## tests/terrain/brush_color_test.cpp (and was gated against real Godot during A4).

const TerrainEditorBrushSession = preload("res://modtools/terrain/terrain_editor_brush_session.gd")


class DummyTerrainMesh:
	extends RefCounted

	var clip_rect := Rect2i(0, 0, 64, 64)

	func get_cells_overlapping_brush(_world_x: float, _world_z: float, _radius: float) -> Array:
		return [Vector2i.ZERO]

	func world_to_source_coords(world_x: float, world_z: float) -> Vector2:
		return Vector2(world_x, world_z)

	func get_sector_cell_value(_row: int, _col: int) -> int:
		return 1

	func world_to_cell_source_coords(world_x: float, world_z: float, _row: int, _col: int) -> Vector2:
		return Vector2(world_x, world_z)

	func get_cell_atlas_rect(_row: int, _col: int) -> Rect2i:
		return clip_rect


func test_raise_lower_invert() -> void:
	var mesh := DummyTerrainMesh.new()
	var session := TerrainEditorBrushSession.new()
	var heightmap := _make_heightmap(10.0)
	var data := NovaTerrainData.new()
	data.set_heightmap_image(heightmap)

	session.current_tool = TerrainEditorBrushSession.Tool.RAISE
	session.brush_radius = 4.0
	session.brush_strength = 1.0
	session.brush_hardness = 1.0
	session.begin_brush_drag(heightmap, true)
	var raise_result := session.apply_brush_stroke(1.0, Vector3(32, 0, 32), true, mesh, data)
	session.end_brush_drag(heightmap)
	assert_true(bool(raise_result["changed_heightmap"]), "Ctrl+Raise should edit the heightmap.")
	assert_lt(heightmap.get_pixel(32, 32).r, 10.0, "Ctrl+Raise should invert into lowering terrain.")

	heightmap = _make_heightmap(10.0)
	data.set_heightmap_image(heightmap)
	session.current_tool = TerrainEditorBrushSession.Tool.LOWER
	session.begin_brush_drag(heightmap, true)
	var lower_result := session.apply_brush_stroke(1.0, Vector3(32, 0, 32), true, mesh, data)
	session.end_brush_drag(heightmap)
	assert_true(bool(lower_result["changed_heightmap"]), "Ctrl+Lower should edit the heightmap.")
	assert_gt(heightmap.get_pixel(32, 32).r, 10.0, "Ctrl+Lower should invert into raising terrain.")


func test_clone_paint_uses_source_color() -> void:
	var mesh := DummyTerrainMesh.new()
	var session := TerrainEditorBrushSession.new()
	var source := _make_color_image(Color(0.0, 0.0, 0.0, 1.0))
	var dest := _make_color_image(Color(0.0, 0.0, 0.0, 1.0))
	source.set_pixel(10, 10, Color(0.25, 0.7, 0.4, 1.0))
	var data := NovaTerrainData.new()
	data.set_colormap_image(dest)

	session.current_tool = TerrainEditorBrushSession.Tool.CLONE_COLOR
	session.brush_radius = 1.0
	session.brush_strength = 1.0
	session.brush_hardness = 1.0
	session.set_clone_source(Vector3(10, 0, 10), source)
	assert_true(session.prepare_clone_drag(Vector3(20, 0, 20), mesh), "Clone paint should prepare a valid drag offset.")
	session.begin_brush_drag(dest, false)
	var result := session.apply_brush_stroke(1.0, Vector3(20, 0, 20), true, mesh, data)
	session.end_brush_drag(dest)

	assert_true(bool(result["changed_colormap"]), "Clone paint should edit the colormap.")
	assert_true(dest.get_pixel(20, 20).is_equal_approx(source.get_pixel(10, 10)), "Clone paint should copy the sampled source color.")


func test_blend_paint_stays_normalized() -> void:
	var image := _make_color_image(Color(1.0, 0.0, 0.0, 1.0))
	var data := NovaTerrainData.new()
	data.set_blendmap_image(image)
	data.brush_blend_paint(1, 16, 16, 4, 1.0, 1.0, Rect2i(0, 0, 64, 64))
	var painted := image.get_pixel(16, 16)
	var total := painted.r + painted.g + painted.b
	assert_almost_eq(total, 1.0, 0.005, "Blend painting should keep channel weights normalized within RGBA8 precision.")
	assert_true(painted.g > 0.0 and painted.g < 1.0, "Blend painting should add weight to the selected channel.")


func test_hardness_changes_edge_falloff() -> void:
	var clip_rect := Rect2i(0, 0, 64, 64)
	var soft := _make_color_image(Color(0.0, 0.0, 0.0, 1.0))
	var hard := _make_color_image(Color(0.0, 0.0, 0.0, 1.0))
	var soft_data := NovaTerrainData.new()
	soft_data.set_colormap_image(soft)
	var hard_data := NovaTerrainData.new()
	hard_data.set_colormap_image(hard)

	soft_data.brush_colormap_paint(Color(1.0, 1.0, 1.0, 1.0), 16, 16, 4, 1.0, 0.0, clip_rect)
	hard_data.brush_colormap_paint(Color(1.0, 1.0, 1.0, 1.0), 16, 16, 4, 1.0, 1.0, clip_rect)

	assert_lt(soft.get_pixel(20, 16).r, 0.05, "Soft brushes should taper to nearly zero at the edge.")
	assert_gt(hard.get_pixel(20, 16).r, 0.95, "Hard brushes should stay full strength across the brush radius.")


func test_brush_writeback_preserves_mipmaps() -> void:
	# Loaded colormap/blendmap carry mipmaps (normalize_image -> generate_mipmaps),
	# and the editor builds ImageTexture from them then calls update(), which
	# rejects a mismatched mipmap flag. The C++ write-back must keep the flag so
	# live paint keeps reaching the GPU past the first dab.
	var colormap := _make_color_image(Color(0.2, 0.4, 0.6, 1.0))
	colormap.generate_mipmaps()
	assert_true(colormap.has_mipmaps(), "precondition: colormap is mipmapped")
	var data := NovaTerrainData.new()
	data.set_colormap_image(colormap)
	data.brush_colormap_paint(Color(1.0, 1.0, 1.0, 1.0), 16, 16, 4, 1.0, 1.0, Rect2i(0, 0, 64, 64))
	assert_true(colormap.has_mipmaps(), "colormap keeps mipmaps after paint (ImageTexture.update stays valid)")
	assert_eq(colormap.get_pixel(16, 16), Color(1.0, 1.0, 1.0, 1.0), "mip0 painted to the target")

	var blendmap := _make_color_image(Color(1.0, 0.0, 0.0, 1.0))
	blendmap.generate_mipmaps()
	data.set_blendmap_image(blendmap)
	data.brush_blend_paint(1, 16, 16, 4, 1.0, 1.0, Rect2i(0, 0, 64, 64))
	assert_true(blendmap.has_mipmaps(), "blendmap keeps mipmaps after blend paint")

	# Same write-back path for the FORMAT_RF heightmap.
	var heightmap := Image.create(64, 64, true, Image.FORMAT_RF)
	heightmap.fill(Color(10.0, 0.0, 0.0, 1.0))
	assert_true(heightmap.has_mipmaps(), "precondition: heightmap is mipmapped")
	var hdata := NovaTerrainData.new()
	hdata.set_heightmap_image(heightmap)
	hdata.brush_raise_lower(16, 16, 4, 5.0, 1.0, Rect2i(0, 0, 64, 64))
	assert_true(heightmap.has_mipmaps(), "heightmap keeps mipmaps after raise")


func _make_heightmap(fill_height: float) -> Image:
	var image := Image.create(1024, 1024, false, Image.FORMAT_RF)
	image.fill(Color(fill_height, 0, 0, 1))
	return image


func _make_color_image(color: Color) -> Image:
	var image := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	image.fill(color)
	return image
