extends GutTest

# The terrain height revision (terrain_editor.get_height_revision): a monotonic
# counter the Mission workspace compares against its load-time capture to detect
# that placed objects may have drifted off the ground. Full heightmap
# replacements (new/open/import all funnel through _set_heightmap_image) bump
# it; stroke and undo/redo bumps ride the same changed_heightmap flag the brush
# session suite covers. Surface-only edits must never bump it.

const TerrainEditorScene = preload("res://modtools/terrain/terrain_editor.tscn")


func test_new_terrain_bumps_and_revision_is_monotonic() -> void:
	var editor = add_child_autofree(TerrainEditorScene.instantiate())
	await get_tree().process_frame
	var rev0: int = editor.get_height_revision()
	editor.new_terrain()
	var rev1: int = editor.get_height_revision()
	assert_gt(rev1, rev0, "a fresh heightmap is a height change")
	editor.new_terrain()
	assert_gt(editor.get_height_revision(), rev1, "the counter only moves forward")


func test_surface_only_history_restore_does_not_bump() -> void:
	var editor = add_child_autofree(TerrainEditorScene.instantiate())
	await get_tree().process_frame
	editor.new_terrain()
	var rev: int = editor.get_height_revision()
	# A SURFACEMAP snapshot restore touches paint state, never heights.
	editor._apply_history_snapshot({
		"kind": TerrainEditHistory.Kind.SURFACEMAP,
		"before_value": {},
		"after_value": {},
	}, true)
	assert_eq(editor.get_height_revision(), rev, "surface-only edits never signal height drift")
