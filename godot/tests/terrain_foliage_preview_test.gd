extends GutTest

const TerrainEditorScript = preload("res://modtools/terrain/terrain_editor.gd")
const TerrainFoliagePreviewScript = preload("res://modtools/terrain/terrain_foliage_preview.gd")
const EditorTerrainMeshScript = preload("res://modtools/terrain/editor_terrain_mesh.gd")
const SOURCE_OBJECT := "res://../fixtures/3dp/Bird1/Bird1.3di"


func before_each() -> void:
	_cleanup_dir(_fixture_root())


func after_each() -> void:
	_cleanup_dir(_fixture_root())


func test_in_place_graphic_change_refreshes_preview_assets() -> void:
	var resource_root := _prepare_veg_fixture()
	var preview = add_child_autofree(TerrainFoliagePreviewScript.new())
	await get_tree().process_frame
	var def := NovaTerrainFoliageDef.new()
	def.graphic = "MvegA.3di"

	preview.set_preview_state(null, null, null, null, [def], -1, null, resource_root)
	var dispatcher := preview.get_node("Dispatcher") as NovaFoliageDispatcher
	var first_mesh: Mesh = dispatcher.slot_meshes[0]
	assert_not_null(first_mesh, "The first graphic resolves into the preview dispatcher.")

	def.graphic = "MvegB.3di"
	preview.set_preview_state(null, null, null, null, [def], -1, null, resource_root)
	var second_mesh: Mesh = dispatcher.slot_meshes[0]

	assert_not_null(second_mesh, "The replacement graphic resolves into the preview dispatcher.")
	assert_ne(second_mesh, first_mesh,
		"Changing a definition in place refreshes its mesh despite stable object identity.")


func test_in_place_match_change_flushes_bake_once_preview_cache() -> void:
	var resource_root := _prepare_veg_fixture()
	var preview = add_child_autofree(TerrainFoliagePreviewScript.new())
	await get_tree().process_frame
	var def := NovaTerrainFoliageDef.new()
	def.graphic = "MvegA.3di"
	def.match = 1
	preview.set_preview_state(null, null, null, null, [def], -1, null, resource_root)

	var dispatcher := preview.get_node("Dispatcher") as NovaFoliageDispatcher
	dispatcher.height_sampler = Callable(self, "_flat_height")
	dispatcher.surface_sampler = Callable(self, "_slot_zero_mask")
	dispatcher.dispatch(Vector3.ZERO)
	assert_gt(dispatcher.get_cached_cells(), 0, "Precondition: FAR cells are resident.")

	def.match = 2
	preview.set_preview_state(null, null, null, null, [def], -1, null, resource_root)
	await get_tree().process_frame # release FAR nodes detached by reset()

	assert_eq(dispatcher.get_cached_cells(), 0,
		"Changing a match value in place flushes placements baked with the old gate.")


func test_far_preview_compensates_for_render_key_provenance() -> void:
	var preview = add_child_autofree(TerrainFoliagePreviewScript.new())
	await get_tree().process_frame
	var map := NovaTerrainFoliageMap.new()
	map.set_size(16, 16)
	# 64 and 128 downshift to flat texel (1, 2) for a 16x16 map.
	map.set_index(1, 2, 7)
	var def := NovaTerrainFoliageDef.new()
	def.match = 7
	preview.set_preview_state(null, null, null, map, [def], -1)

	assert_eq(preview._sample_far_mask(64.0, 128.0), 1,
		"Host render keys hand the preview native z, so it reads that flat row directly.")
	assert_eq(preview._sample_far_mask(64.0, -128.0), 0,
		"The uncompensated mirrored row remains empty while D-FOLIAGE-8 is open.")


func test_height_stroke_and_undo_invalidate_bake_once_preview() -> void:
	var editor: TerrainEditor = await _make_terrain_editor()
	editor.new_terrain()
	editor.current_tool = TerrainEditorScript.Tool.RAISE
	editor.brush_radius = 8.0
	editor.brush_strength = 1.0
	editor.brush_hardness = 1.0
	editor._hover_hit = Vector3(16.0, TerrainEditorScript.DEFAULT_HEIGHT, 16.0)
	editor._hover_hit_valid = true
	editor._foliage_preview._pending_flush = false
	var revision_before: int = editor.get_height_revision()

	editor._on_primary_start()
	editor._apply_brush_stroke(1.0)
	editor._on_primary_end()

	assert_gt(editor.get_height_revision(), revision_before, "Precondition: the stroke edits terrain height.")
	assert_true(editor._foliage_preview._pending_flush,
		"A height stroke invalidates FAR meshes baked against the old terrain.")
	assert_true(editor.can_undo(), "Precondition: the height stroke is undoable.")

	editor._foliage_preview._pending_flush = false
	editor.undo()
	assert_true(editor._foliage_preview._pending_flush,
		"Undoing a height edit also invalidates bake-once FAR meshes.")


func _flat_height(_world_x: float, _world_z: float) -> float:
	return 0.0


func _slot_zero_mask(_world_x: float, _native_z: float) -> int:
	return 1


func _make_terrain_editor() -> TerrainEditor:
	var editor = TerrainEditorScript.new()
	var world_root := Node3D.new()
	world_root.name = "TerrainWorldRoot"
	var terrain_mesh = EditorTerrainMeshScript.new()
	terrain_mesh.name = "EditorTerrainMesh"
	var camera := Camera3D.new()
	camera.name = "FlyCamera"
	world_root.add_child(terrain_mesh)
	world_root.add_child(camera)
	editor.add_child(world_root)
	add_child_autofree(editor)
	await get_tree().process_frame
	return editor


func _prepare_veg_fixture() -> NovaResourceRoot:
	var root := _fixture_root()
	assert_eq(DirAccess.make_dir_recursive_absolute(root), OK)
	var bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(SOURCE_OBJECT))
	assert_gt(bytes.size(), 0, "Fixture source contains a usable 3DI.")
	for filename in ["MvegA.3di", "MvegB.3di"]:
		var file := FileAccess.open(root.path_join(filename), FileAccess.WRITE)
		assert_not_null(file, "Fixture destination opens: %s" % filename)
		if file != null:
			file.store_buffer(bytes)
			file.close()
	var resource_root := NovaResourceRoot.new()
	assert_eq(resource_root.set_root_dir(root), OK)
	return resource_root


func _fixture_root() -> String:
	return OS.get_cache_dir().path_join("opennova_terrain_foliage_preview_test")


func _cleanup_dir(path: String) -> void:
	if not DirAccess.dir_exists_absolute(path):
		return
	for filename in DirAccess.get_files_at(path):
		DirAccess.remove_absolute(path.path_join(filename))
	for dirname in DirAccess.get_directories_at(path):
		_cleanup_dir(path.path_join(dirname))
	DirAccess.remove_absolute(path)
