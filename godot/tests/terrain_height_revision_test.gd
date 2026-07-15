extends GutTest

# The terrain height revision (terrain_editor.get_height_revision): a monotonic
# counter the Mission workspace compares against its load-time capture to detect
# that placed objects may have drifted off the ground. Full heightmap
# replacements (new/open/import all funnel through _set_heightmap_image) bump
# it; stroke and undo/redo bumps ride the same changed_heightmap flag the brush
# session suite covers. Surface-only edits must never bump it.

const EditorMainScene = preload("res://modtools/editor/editor_main.tscn")
func _press_primary(editor: Node) -> void:
	var event := InputEventMouseButton.new()
	event.button_index = MOUSE_BUTTON_LEFT
	event.pressed = true
	editor.handle_viewport_input(event)




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


func test_batch_and_scalar_run_the_single_cpp_sampler() -> void:
	# Scalar and batch both forward to NovaTerrainData's live-surface sampler
	# (one C++ per-point core), so this pins that single path end-to-end through
	# the wrappers, including their NAN/sentinel translation (the mesh's scalar
	# maps NAN to -1e6; terrain_editor's sample_height_world maps it back to
	# NAN). A sloped surface pins the sample LOCATION as well as the height
	# source (a flat fill cannot tell a remap bug from a correct read), and it
	# is written AFTER new_terrain, so this also proves the sampler reads live
	# edits.
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


func test_raycast_world_hits_the_live_surface() -> void:
	# EditorTerrainMesh.raycast_world forwards to the engine's witnessed segment
	# raycast (NovaTerrainData.raycast_terrain, the ENG-3 B1 port [orig:
	# Terrain_RaycastHeightmapLoRes @ 0x60cb80; Terrain_RaycastHeightmapHiRes_0
	# @ 0x60e710]) over the SAME live editable surface the height samplers above
	# read, so a hit must land on the sampled surface and on the cast segment.
	var editor = add_child_autofree(EditorMainScene.instantiate()).get_terrain_editor()
	await get_tree().process_frame
	editor.new_terrain()
	# The sloped surface from the sampler-parity test: a slope pins the hit
	# LOCATION as well as the height substrate (a flat fill cannot tell a
	# remap bug from a correct read).
	var img: Image = editor.terrain_mesh.get_heightmap_image()
	var w := img.get_width()
	var h := img.get_height()
	var floats := PackedFloat32Array()
	floats.resize(w * h)
	for i in floats.size():
		@warning_ignore("integer_division")
		floats[i] = float(i % w) * 0.05 + float(i / w) * 0.025
	img.set_data(w, h, false, Image.FORMAT_RF, floats.to_byte_array())

	# A descending segment from above the active region down through the slope.
	var from := Vector3(-300.0, 200.0, -260.0)
	var to := Vector3(340.0, -40.0, 300.0)
	var hit: Vector3 = editor.terrain_mesh.raycast_world(from, to)
	assert_false(is_nan(hit.x) or is_nan(hit.y) or is_nan(hit.z),
		"the descending segment hits the live surface")
	var surface: float = editor.sample_height_world(hit.x, hit.z)
	assert_almost_eq(hit.y, surface, 1.0 / 256.0 + 0.02,
		"the refined hit sits on the sampled surface (raw16 quantum + refine tolerance)")
	# The hit lies on the cast segment: x and z share one segment parameter,
	# inside [0, 1].
	var tx := (hit.x - from.x) / (to.x - from.x)
	var tz := (hit.z - from.z) / (to.z - from.z)
	assert_almost_eq(tx, tz, 0.001, "hit x/z share one segment parameter")
	assert_between(tx, 0.0, 1.0, "the hit lies between the endpoints")

	# Entirely outside the authored extent -> the all-NAN miss (the engine-side
	# slab clip rejects before the core marches).
	var missed: Vector3 = editor.terrain_mesh.raycast_world(
		Vector3(50000.0, 100.0, 50000.0), Vector3(50100.0, -100.0, 50100.0))
	assert_true(is_nan(missed.x) and is_nan(missed.y) and is_nan(missed.z),
		"a segment outside the authored extent misses")

	# No terrain mounted at all (neither a live image nor a baked CPT) -> the
	# all-NAN miss, never a fake ground plane.
	var bare_hit: Vector3 = NovaTerrainData.new().raycast_terrain(
		Vector3(0.0, 100.0, 0.0), Vector3(0.0, -100.0, 0.0))
	assert_true(is_nan(bare_hit.x) and is_nan(bare_hit.y) and is_nan(bare_hit.z),
		"no terrain data -> the all-NAN miss")


func test_viewport_deactivation_finalizes_live_surface_input_fallbacks() -> void:
	var editor = add_child_autofree(EditorMainScene.instantiate()).get_terrain_editor()
	await get_tree().process_frame
	editor.new_terrain()
	editor.set_viewport_active(true, true)
	var material: ShaderMaterial = editor.terrain_mesh.get_material()

	editor.set_tool(editor.Tool.RAISE)
	assert_true(material.get_shader_parameter("u_has_heightfield_normal"))
	_press_primary(editor)
	assert_true(editor.brush_active)
	assert_false(material.get_shader_parameter("u_has_heightfield_normal"),
		"height sculpt temporarily derives normals from the live heightmap")
	editor.set_viewport_active(false, false)
	assert_false(editor.brush_active)
	assert_true(material.get_shader_parameter("u_has_heightfield_normal"),
		"workspace deactivation restores the cached retail heightfield normal")

	editor.set_viewport_active(true, true)
	editor.set_tool(editor.Tool.PAINT_DETAIL)
	var derived_blend: Texture2D = material.get_shader_parameter("u_blendmap")
	_press_primary(editor)
	assert_true(editor.brush_active)
	var raw_blend: Texture2D = material.get_shader_parameter("u_blendmap")
	assert_ne(raw_blend, derived_blend,
		"detail painting temporarily binds the raw live blendmap")
	editor.set_viewport_active(false, false)
	assert_false(editor.brush_active)
	assert_ne(material.get_shader_parameter("u_blendmap"), raw_blend,
		"workspace deactivation drops the raw live blendmap")
	assert_same(material.get_shader_parameter("u_blendmap"),
		editor.terrain_mesh.get_surface_inputs().get_blend_texture(),
		"workspace deactivation applies the rebuilt normalized retail blendmap")


func test_tool_switch_finalizes_active_cross_kind_strokes() -> void:
	var editor = add_child_autofree(EditorMainScene.instantiate()).get_terrain_editor()
	await get_tree().process_frame
	editor.new_terrain()
	editor.set_viewport_active(true, true)
	var material: ShaderMaterial = editor.terrain_mesh.get_material()
	var normalized_blend: Texture2D = editor.terrain_mesh.get_surface_inputs().get_blend_texture()

	editor.set_tool(editor.Tool.RAISE)
	_press_primary(editor)
	assert_true(editor.brush_active)
	assert_eq(editor.current_tool, editor.Tool.RAISE,
		"the public input seam starts a Raise stroke with the selected tool")
	assert_false(material.get_shader_parameter("u_has_heightfield_normal"),
		"Raise temporarily derives normals from the live heightmap")

	editor.set_tool(editor.Tool.PAINT_DETAIL)
	assert_false(editor.brush_active,
		"Raise -> Paint Detail closes the active height stroke before changing kind")
	assert_eq(editor.current_tool, editor.Tool.PAINT_DETAIL,
		"the public tool state advances after finalizing the Raise stroke")
	assert_true(material.get_shader_parameter("u_has_heightfield_normal"),
		"closing Raise restores the cached retail heightfield normal")
	assert_same(material.get_shader_parameter("u_blendmap"), normalized_blend,
		"closing Raise leaves the normalized retail blendmap bound")

	_press_primary(editor)
	assert_true(editor.brush_active)
	assert_eq(editor.current_tool, editor.Tool.PAINT_DETAIL,
		"the public input seam starts a Paint Detail stroke with the selected tool")
	var raw_blend: Texture2D = material.get_shader_parameter("u_blendmap")
	assert_ne(raw_blend, normalized_blend,
		"Paint Detail temporarily binds the raw live blendmap")

	editor.set_tool(editor.Tool.RAISE)
	assert_false(editor.brush_active,
		"Paint Detail -> Raise closes the active blend stroke before changing kind")
	assert_eq(editor.current_tool, editor.Tool.RAISE,
		"the public tool state advances after finalizing the Paint Detail stroke")
	assert_ne(material.get_shader_parameter("u_blendmap"), raw_blend,
		"closing Paint Detail drops the raw live blendmap")
	assert_same(material.get_shader_parameter("u_blendmap"),
		editor.terrain_mesh.get_surface_inputs().get_blend_texture(),
		"closing Paint Detail binds the rebuilt normalized retail blendmap")
	assert_true(material.get_shader_parameter("u_has_heightfield_normal"),
		"switching back to Raise does not strand the temporary normal fallback")
