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
	dispatcher.far_slot_mask_sampler = Callable(self, "_slot_zero_mask")
	dispatcher.dispatch(Vector3.ZERO)
	assert_gt(dispatcher.get_cached_cells(), 0, "Precondition: FAR cells are resident.")

	def.match = 2
	preview.set_preview_state(null, null, null, null, [def], -1, null, resource_root)
	await get_tree().process_frame # release FAR nodes detached by reset()

	assert_eq(dispatcher.get_cached_cells(), 0,
		"Changing a match value in place flushes placements baked with the old gate.")


func test_far_preview_reads_the_world_routed_texel() -> void:
	# The FAR preview mask samples the SAME world->source-routed texel the
	# MODEL gate resolves [orig: Foliage_SampleFoliageMapMask @ 0x606620;
	# retail's flat FAR sampler consumes source-space keys @ 0x603f8a].
	var data := NovaTerrainData.new()
	data.set_trn_path(ProjectSettings.globalize_path(
		"res://../fixtures/godot/dvxi5").path_join("Dvxi5.trn"))
	assert_eq(data.load(), OK, "Dvxi5 fixture should load.")
	var preview = add_child_autofree(TerrainFoliagePreviewScript.new())
	await get_tree().process_frame

	# An interior world point whose source coords resolve.
	var world := Vector2(-1, -1)
	for sz in range(64, 8192, 256):
		for sx in range(64, 8192, 256):
			if data.world_to_source_coords(float(sx), float(sz)).x >= 0.0:
				world = Vector2(float(sx), float(sz))
				break
		if world.x >= 0.0:
			break
	assert_gt(world.x, 0.0, "fixture should expose an interior point")

	var map := data.get_foliage_map()
	map.clear(0)
	var source := data.world_to_source_coords_wrapped(world.x, world.y)
	map.set_index(map.map_x_from_heightmap_x(source.x),
		map.map_y_from_heightmap_y(source.y), 7)
	var def := NovaTerrainFoliageDef.new()
	def.match = 7
	preview.set_preview_state(null, null, null, map, [def], -1, data)

	assert_eq(preview.sample_far_slot_mask(world.x, world.y), 1,
		"The FAR preview mask resolves the world-routed texel at the candidate's own (x, z).")
	assert_eq(preview.sample_far_slot_mask(world.x, -world.y), 0,
		"The mirrored z reads a different (unpainted) texel.")


func test_height_stroke_and_undo_invalidate_bake_once_preview() -> void:
	var editor: TerrainEditor = await _make_terrain_editor()
	editor.new_terrain()
	editor.current_tool = TerrainEditorScript.Tool.RAISE
	editor.brush_radius = 8.0
	editor.brush_strength = 1.0
	editor.brush_hardness = 1.0
	var revision_before: int = editor.get_height_revision()
	watch_signals(editor)

	assert_true(editor.apply_brush_stroke_at(
		Vector3(16.0, TerrainEditorScript.DEFAULT_HEIGHT, 16.0), 1.0),
		"The public brush action reports a changed height.")

	assert_gt(editor.get_height_revision(), revision_before, "Precondition: the stroke edits terrain height.")
	assert_signal_emit_count(editor, "foliage_preview_invalidated", 1,
		"A height stroke invalidates FAR meshes baked against the old terrain.")
	assert_true(editor.can_undo(), "Precondition: the height stroke is undoable.")

	editor.undo()
	assert_signal_emit_count(editor, "foliage_preview_invalidated", 2,
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
