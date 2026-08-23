extends GutTest

# terrain_editor.apply_bulk_edit: the programmatic, undoable entry for edits that are not a
# brush drag (a generated relief, a fill, an imported region).
#
# The interactive brush already drives TerrainEditHistory begin -> expand -> end per dab, but
# only through begin_brush_drag/end_brush_drag, which also flip brush_active and the stroke
# tracking a bulk edit has no business touching -- and expand_rect was not exposed at all.
#
# The load-bearing part is the middle call: TerrainEditHistory.end_stroke falls through to
# cancel_stroke when no rect was ever expanded, so an implementation that forgets it produces
# NO undo entry and reports no error. test_a_bulk_edit_is_one_undo_step is what catches that.

const EditorMainScene = preload("res://modtools/editor/editor_main.tscn")

const Kind := TerrainEditHistory.Kind


func _editor() -> Object:
	var editor = add_child_autofree(EditorMainScene.instantiate()).get_terrain_editor()
	return editor


# Raise a small square of the heightmap by `amount`, returning the rect it touched.
func _raise_block(rect: Rect2i, amount: float) -> Callable:
	return func(image: Image) -> Rect2i:
		for z in range(rect.position.y, rect.end.y):
			for x in range(rect.position.x, rect.end.x):
				var c := image.get_pixel(x, z)
				image.set_pixel(x, z, Color(c.r + amount, c.g, c.b, c.a))
		return rect


func test_a_bulk_edit_changes_the_heightmap_and_bumps_the_revision() -> void:
	var editor := _editor()
	await get_tree().process_frame
	editor.new_terrain()
	var rev: int = editor.get_height_revision()
	var before: float = editor._heightmap_image.get_pixel(300, 300).r

	assert_true(editor.apply_bulk_edit(Kind.HEIGHTMAP, _raise_block(Rect2i(280, 280, 40, 40), 5.0)),
			"the edit reports applied")
	assert_almost_eq(editor._heightmap_image.get_pixel(300, 300).r, before + 5.0, 0.001,
			"the pixel moved")
	assert_gt(editor.get_height_revision(), rev, "a height change bumps the drift counter")
	assert_true(editor.is_dirty, "and marks the project dirty")


func test_a_bulk_edit_is_one_undo_step() -> void:
	# The whole point of the seam. Without the expand_history_rect call this passes nothing to
	# the undo stack and silently reports success.
	var editor := _editor()
	await get_tree().process_frame
	editor.new_terrain()
	var before: float = editor._heightmap_image.get_pixel(300, 300).r

	editor.apply_bulk_edit(Kind.HEIGHTMAP, _raise_block(Rect2i(280, 280, 40, 40), 5.0))
	assert_true(editor.can_undo(), "a bulk edit leaves an undo entry")

	editor.undo()
	assert_almost_eq(editor._heightmap_image.get_pixel(300, 300).r, before, 0.001,
			"one undo restores the whole edit")
	assert_true(editor.can_redo(), "and it can be redone")

	editor.redo()
	assert_almost_eq(editor._heightmap_image.get_pixel(300, 300).r, before + 5.0, 0.001,
			"redo puts it back")


func test_an_edit_that_touches_nothing_leaves_no_undo_entry() -> void:
	var editor := _editor()
	await get_tree().process_frame
	editor.new_terrain()
	assert_false(editor.can_undo(), "a fresh terrain has no history")

	var applied: bool = editor.apply_bulk_edit(Kind.HEIGHTMAP,
			func(_image: Image) -> Rect2i: return Rect2i(0, 0, 0, 0))
	assert_false(applied, "an empty rect is not an edit")
	assert_false(editor.can_undo(), "and leaves the stack alone rather than a no-op step")


func test_the_mutator_sees_the_live_shared_buffer() -> void:
	# TerrainData holds the SAME Ref<Image>; that shared buffer is why the C++ brush kernels and
	# the editor agree. A mutator writing into it must be visible through the data side too.
	var editor := _editor()
	await get_tree().process_frame
	editor.new_terrain()
	editor.apply_bulk_edit(Kind.HEIGHTMAP, _raise_block(Rect2i(280, 280, 40, 40), 7.0))
	assert_eq(editor._data.get_heightmap_image(), editor._heightmap_image,
			"the editor's image IS the data's image, not a copy")


func test_colormap_edits_route_through_the_same_seam() -> void:
	var editor := _editor()
	await get_tree().process_frame
	editor.new_terrain()
	var paint := func(image: Image) -> Rect2i:
		var r := Rect2i(100, 100, 20, 20)
		for z in range(r.position.y, r.end.y):
			for x in range(r.position.x, r.end.x):
				image.set_pixel(x, z, Color(0.2, 0.4, 0.1, 1.0))
		return r
	var rev: int = editor.get_height_revision()
	assert_true(editor.apply_bulk_edit(Kind.COLORMAP, paint), "colour edits apply")
	# The colormap is 8 bits per channel, so compare within a quantisation step.
	var got: Color = editor._colormap_image.get_pixel(105, 105)
	assert_almost_eq(got.r, 0.2, 0.005, "red landed")
	assert_almost_eq(got.g, 0.4, 0.005, "green landed")
	assert_almost_eq(got.b, 0.1, 0.005, "blue landed")
	assert_eq(editor.get_height_revision(), rev,
			"a colour edit is not a height change and must not signal object drift")
	assert_true(editor.can_undo(), "and it is undoable like any other edit")
