extends GutTest

const TerrainEditorBrushes = preload("res://modtools/terrain/terrain_editor_brushes.gd")
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
	var blendmap := _make_color_image(Color(1.0, 0.0, 0.0, 1.0))
	var colormap := _make_color_image(Color(0.0, 0.0, 0.0, 1.0))

	session.current_tool = TerrainEditorBrushSession.Tool.RAISE
	session.brush_radius = 4.0
	session.brush_strength = 1.0
	session.brush_hardness = 1.0
	session.begin_brush_drag(heightmap, true)
	var raise_result := session.apply_brush_stroke(1.0, Vector3(32, 0, 32), true, mesh, heightmap, blendmap, colormap)
	session.end_brush_drag(heightmap)
	assert_true(bool(raise_result["changed_heightmap"]), "Ctrl+Raise should edit the heightmap.")
	assert_lt(heightmap.get_pixel(32, 32).r, 10.0, "Ctrl+Raise should invert into lowering terrain.")

	heightmap = _make_heightmap(10.0)
	session.current_tool = TerrainEditorBrushSession.Tool.LOWER
	session.begin_brush_drag(heightmap, true)
	var lower_result := session.apply_brush_stroke(1.0, Vector3(32, 0, 32), true, mesh, heightmap, blendmap, colormap)
	session.end_brush_drag(heightmap)
	assert_true(bool(lower_result["changed_heightmap"]), "Ctrl+Lower should edit the heightmap.")
	assert_gt(heightmap.get_pixel(32, 32).r, 10.0, "Ctrl+Lower should invert into raising terrain.")


func test_stamp_paint_uses_stamp_texture() -> void:
	var image := _make_color_image(Color(0.2, 0.2, 0.2, 1.0))
	var stamp := Image.create(1, 1, false, Image.FORMAT_RGBA8)
	stamp.fill(Color(0.1, 0.8, 0.2, 1.0))

	TerrainEditorBrushes.apply_colormap_paint(
		image,
		Color(0.9, 0.1, 0.1, 1.0),
		16,
		16,
		3,
		1.0,
		1.0,
		Rect2i(0, 0, 64, 64),
		stamp
	)

	var painted := image.get_pixel(16, 16)
	assert_gt(painted.g, painted.r, "Stamp paint should use the loaded stamp colors over the flat picker color.")


func test_clone_paint_uses_source_color() -> void:
	var mesh := DummyTerrainMesh.new()
	var session := TerrainEditorBrushSession.new()
	var heightmap := _make_heightmap(0.0)
	var blendmap := _make_color_image(Color(1.0, 0.0, 0.0, 1.0))
	var source := _make_color_image(Color(0.0, 0.0, 0.0, 1.0))
	var dest := _make_color_image(Color(0.0, 0.0, 0.0, 1.0))
	source.set_pixel(10, 10, Color(0.25, 0.7, 0.4, 1.0))

	session.current_tool = TerrainEditor.Tool.CLONE_COLOR
	session.brush_radius = 1.0
	session.brush_strength = 1.0
	session.brush_hardness = 1.0
	session.set_clone_source(Vector3(10, 0, 10), source)
	assert_true(session.prepare_clone_drag(Vector3(20, 0, 20), mesh), "Clone paint should prepare a valid drag offset.")
	session.begin_brush_drag(dest, false)
	var result := session.apply_brush_stroke(1.0, Vector3(20, 0, 20), true, mesh, heightmap, blendmap, dest)
	session.end_brush_drag(dest)

	assert_true(bool(result["changed_colormap"]), "Clone paint should edit the colormap.")
	assert_true(dest.get_pixel(20, 20).is_equal_approx(source.get_pixel(10, 10)), "Clone paint should copy the sampled source color.")


func test_blend_paint_stays_normalized() -> void:
	var image := _make_color_image(Color(1.0, 0.0, 0.0, 1.0))
	TerrainEditorBrushes.apply_blend_paint(image, 1, 16, 16, 4, 1.0, 1.0, Rect2i(0, 0, 64, 64))
	var painted := image.get_pixel(16, 16)
	var total := painted.r + painted.g + painted.b
	assert_almost_eq(total, 1.0, 0.005, "Blend painting should keep channel weights normalized within RGBA8 precision.")
	assert_true(painted.g > 0.0 and painted.g < 1.0, "Blend painting should add weight to the selected channel.")


func test_hardness_changes_edge_falloff() -> void:
	var soft := _make_color_image(Color(0.0, 0.0, 0.0, 1.0))
	var hard := _make_color_image(Color(0.0, 0.0, 0.0, 1.0))
	var clip_rect := Rect2i(0, 0, 64, 64)

	TerrainEditorBrushes.apply_colormap_paint(soft, Color(1.0, 1.0, 1.0, 1.0), 16, 16, 4, 1.0, 0.0, clip_rect)
	TerrainEditorBrushes.apply_colormap_paint(hard, Color(1.0, 1.0, 1.0, 1.0), 16, 16, 4, 1.0, 1.0, clip_rect)

	assert_lt(soft.get_pixel(20, 16).r, 0.05, "Soft brushes should taper to nearly zero at the edge.")
	assert_gt(hard.get_pixel(20, 16).r, 0.95, "Hard brushes should stay full strength across the brush radius.")


func _make_heightmap(fill_height: float) -> Image:
	var image := Image.create(1024, 1024, false, Image.FORMAT_RF)
	image.fill(Color(fill_height, 0, 0, 1))
	return image


func _make_color_image(color: Color) -> Image:
	var image := Image.create(64, 64, false, Image.FORMAT_RGBA8)
	image.fill(color)
	return image
