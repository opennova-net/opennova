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


func test_sample_height_world_reads_the_live_editable_surface() -> void:
	# The re-ground sampler must read the surface the revision counter describes:
	# the LIVE editable heightmap (what brushes mutate and placement raycasts
	# ground on), never the baked CPT — which height edits leave stale and which a
	# never-exported project terrain does not have at all.
	var editor = add_child_autofree(TerrainEditorScene.instantiate())
	await get_tree().process_frame
	editor.new_terrain()
	# A fresh terrain's active region is centred on the world origin.
	assert_almost_eq(editor.sample_height_world(0.0, 0.0), editor.DEFAULT_HEIGHT, 0.01,
		"a fresh project terrain (no baked CPT exists yet) samples the live surface")
	assert_true(is_nan(editor.sample_height_world(50000.0, 50000.0)),
		"off the terrain -> NAN (re-ground callers skip, never ground to a bogus height)")

	# Mutate the live heightmap image directly: the seam must see the edit with no
	# export/re-bake in between.
	editor.terrain_mesh.get_heightmap_image().fill(Color(35.0, 0.0, 0.0))
	assert_almost_eq(editor.sample_height_world(0.0, 0.0), 35.0, 0.01,
		"height edits are visible to the re-ground sampler immediately")
