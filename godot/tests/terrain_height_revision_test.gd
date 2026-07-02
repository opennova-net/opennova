extends GutTest

# The terrain height revision (terrain_editor.get_height_revision): a monotonic
# counter the Mission workspace compares against its load-time capture to detect
# that placed objects may have drifted off the ground. Full heightmap
# replacements (new/open/import all funnel through _set_heightmap_image) bump
# it; stroke and undo/redo bumps ride the same changed_heightmap flag the brush
# session suite covers. Surface-only edits must never bump it.

const EditorMainScene = preload("res://modtools/editor/editor_main.tscn")


func test_new_terrain_bumps_and_revision_is_monotonic() -> void:
	var editor = add_child_autofree(EditorMainScene.instantiate()).get_terrain_editor()
	await get_tree().process_frame
	var rev0: int = editor.get_height_revision()
	editor.new_terrain()
	var rev1: int = editor.get_height_revision()
	assert_gt(rev1, rev0, "a fresh heightmap is a height change")
	editor.new_terrain()
	assert_gt(editor.get_height_revision(), rev1, "the counter only moves forward")


func test_surface_only_history_restore_does_not_bump() -> void:
	var editor = add_child_autofree(EditorMainScene.instantiate()).get_terrain_editor()
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
	var editor = add_child_autofree(EditorMainScene.instantiate()).get_terrain_editor()
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


func test_batch_sampler_matches_the_scalar_live_surface_sampler() -> void:
	# The batch sampler (one C++ call — the re-ground request builder's fast
	# path) and the scalar sampler must be the same surface read: same live
	# image, same editor-mode remap, same edge-clamped bilinear. A sloped surface
	# pins the sample LOCATION as well as the height source (a flat fill cannot
	# tell a remap bug from a correct read), and it is written AFTER new_terrain,
	# so parity here also proves the batch path reads live edits.
	var editor = add_child_autofree(EditorMainScene.instantiate()).get_terrain_editor()
	await get_tree().process_frame
	editor.new_terrain()
	var img: Image = editor.terrain_mesh.get_heightmap_image()
	var w := img.get_width()
	var h := img.get_height()
	var floats := PackedFloat32Array()
	floats.resize(w * h)
	for i in floats.size():
		@warning_ignore("integer_division")
		floats[i] = float(i % w) * 0.05 + float(i / w) * 0.025
	img.set_data(w, h, false, Image.FORMAT_RF, floats.to_byte_array())

	# A grid across (and past) the active region, plus a far off-mesh point: both
	# samplers must agree row by row, NAN included.
	var points := PackedVector2Array()
	for gx in range(-4, 5):
		for gz in range(-4, 5):
			points.append(Vector2(float(gx) * 211.5, float(gz) * 173.25))
	points.append(Vector2(50000.0, 50000.0))
	var batch: PackedFloat32Array = editor.sample_heights_world(points)
	assert_eq(batch.size(), points.size(), "one height per input point")
	var nan_seen := false
	for i in points.size():
		var scalar: float = editor.sample_height_world(points[i].x, points[i].y)
		if is_nan(scalar):
			nan_seen = true
			assert_true(is_nan(batch[i]), "off-mesh point %d is NAN in both samplers" % i)
		else:
			assert_almost_eq(batch[i], scalar, 0.0001, "batch/scalar parity at point %d" % i)
	assert_true(nan_seen, "the grid includes at least one off-mesh row (the NAN branch is exercised)")
