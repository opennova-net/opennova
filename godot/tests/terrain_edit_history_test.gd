extends GutTest

const TerrainEditHistory = preload("res://modtools/terrain/terrain_edit_history.gd")
const TerrainEditorBrushSession = preload("res://modtools/terrain/terrain_editor_brush_session.gd")


func test_rect_accumulation() -> void:
	var history := TerrainEditHistory.new()
	var image := _make_heightmap(4.0)

	history.begin_stroke(TerrainEditHistory.Kind.HEIGHTMAP, image)
	history.expand_rect(Rect2i(4, 4, 2, 2), 16)
	history.expand_rect(Rect2i(7, 7, 3, 1), 16)
	image.set_pixel(4, 4, Color(6.0, 0, 0, 1))
	image.set_pixel(9, 7, Color(8.0, 0, 0, 1))
	history.end_stroke(image)

	var snapshot := history.pop_undo()
	assert_eq(snapshot["rect"], Rect2i(4, 4, 6, 4), "Stroke history should merge dab bounds into one accumulated rect.")
	assert_true(history.can_redo(), "Undoing a stroke should make it available for redo.")


func test_undo_redo_replay() -> void:
	var history := TerrainEditHistory.new()
	var before := _make_heightmap(5.0)
	var current := Image.new()
	current.copy_from(before)

	history.begin_stroke(TerrainEditHistory.Kind.HEIGHTMAP, before)
	history.expand_rect(Rect2i(2, 2, 3, 3), 16)
	current.set_pixel(3, 3, Color(11.0, 0, 0, 1))
	history.end_stroke(current)

	var brush_session := TerrainEditorBrushSession.new()
	var blendmap := _make_color_image(Color(1.0, 0.0, 0.0, 1.0))
	var colormap := _make_color_image(Color(0.0, 0.0, 0.0, 1.0))

	var undo_snapshot := history.pop_undo()
	var undo_result := brush_session.apply_history_snapshot(undo_snapshot, true, current, blendmap, colormap)
	assert_true(bool(undo_result["changed_heightmap"]), "Undo replay should report a heightmap change.")
	assert_almost_eq(current.get_pixel(3, 3).r, 5.0, 0.0001, "Undo replay should restore the pre-stroke height.")

	var redo_snapshot := history.pop_redo()
	var redo_result := brush_session.apply_history_snapshot(redo_snapshot, false, current, blendmap, colormap)
	assert_true(bool(redo_result["changed_heightmap"]), "Redo replay should report a heightmap change.")
	assert_almost_eq(current.get_pixel(3, 3).r, 11.0, 0.0001, "Redo replay should restore the post-stroke height.")


func _make_heightmap(fill_height: float) -> Image:
	var image := Image.create(16, 16, false, Image.FORMAT_RF)
	image.fill(Color(fill_height, 0, 0, 1))
	return image


func _make_color_image(color: Color) -> Image:
	var image := Image.create(16, 16, false, Image.FORMAT_RGBA8)
	image.fill(color)
	return image
