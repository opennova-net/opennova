extends GutTest

const VegAssetsScript = preload("res://engine/terrain/veg_assets.gd")
const SOURCE_OBJECT := "res://../fixtures/3dp/Bird1/Bird1.3di"
const OVERRIDE_OBJECT := "res://../fixtures/threedi/3di3/House.3di"


func before_each() -> void:
	_cleanup_dir(_fixture_root())
	VegAssetsScript.clear_cache()
	NovaObjectData.reset_network_challenge_model_registry()


func after_each() -> void:
	VegAssetsScript.clear_cache()
	NovaObjectData.reset_network_challenge_model_registry()
	_cleanup_dir(_fixture_root())


func test_list_graphics_preserves_actual_model_path_case() -> void:
	var resource_root := _prepare_veg_fixture("Mveg6.3di")
	var graphics := VegAssetsScript.list_graphics(resource_root, true)
	var mveg6_path := ""
	for entry in graphics:
		if String(entry.basename) == "mveg6":
			mveg6_path = String(entry.model_path)
			break

	assert_eq(
		mveg6_path,
		resource_root.get_root_dir().path_join("Mveg6.3di"),
		"Vegetation asset lookup should retain the real file case for portable object paths."
	)


func test_list_graphics_cache_returns_caller_safe_copy() -> void:
	var resource_root := _prepare_veg_fixture("Mveg6.3di")
	var graphics := VegAssetsScript.list_graphics(resource_root, true)
	assert_gt(graphics.size(), 0, "Vegetation asset lookup should find configured .3di graphics.")

	graphics.clear()
	var cached_again := VegAssetsScript.list_graphics(resource_root)

	assert_gt(cached_again.size(), 0, "Callers should not be able to mutate the shared vegetation graphics cache.")


func test_resolve_slot_meshes_preserves_slots_and_loads_known_graphic() -> void:
	var resource_root := _prepare_veg_fixture("Mveg6.3di")
	var def := NovaTerrainFoliageDef.new()
	def.graphic = "Mveg6.3di"

	var meshes := VegAssetsScript.resolve_slot_meshes(resource_root, [null, def])

	assert_eq(meshes.size(), 2, "Resolver should preserve the foliage slot array shape.")
	assert_null(meshes[0], "Null foliage defs should remain null mesh slots.")
	assert_true(meshes[1] is Mesh, "Resolver should load the mesh for a known .3di graphic.")


func test_foliage_model_is_sticky_excluded_from_network_challenge_snapshot() -> void:
	var resource_root := _prepare_veg_fixture("Mveg6.3di")
	NovaObjectData.reset_network_challenge_model_registry()

	assert_not_null(VegAssetsScript.load_mesh(resource_root, "Mveg6"))
	assert_eq(NovaObjectData.network_challenge_model_count(), 0,
		"foliage geometry loads normally but its model-def row is excluded")

	# Retail marks the shared definition node, so a later ordinary lookup of the
	# same filename does not clear the foliage bit before the snapshot.
	var ordinary := NovaObjectData.new()
	assert_eq(ordinary.open_from_resource_root(resource_root, "MVEG6.3DI"), OK)
	assert_eq(NovaObjectData.network_challenge_model_count(), 0,
		"the foliage marker is sticky on a case-folded shared definition")


func test_cached_foliage_hit_restores_marker_after_mission_registry_reset() -> void:
	var resource_root := _prepare_veg_fixture("Mveg6.3di")
	NovaObjectData.reset_network_challenge_model_registry()

	var first_mesh := VegAssetsScript.load_mesh(resource_root, "Mveg6")
	assert_not_null(first_mesh)
	assert_eq(NovaObjectData.network_challenge_model_count(), 0)

	# Game_StartMission destroys the logical loaded-definition registry, while
	# MainGame keeps the mounted resource root and VegAssets renderer cache alive.
	# The next mission's cache hit must therefore recreate the model-def's sticky
	# foliage mark without reparsing or rebuilding its mesh.
	NovaObjectData.reset_network_challenge_model_registry()
	var cached_mesh := VegAssetsScript.load_mesh(resource_root, "mVEG6.3di")
	assert_same(cached_mesh, first_mesh, "the second mission takes the real mesh-cache hit")

	var ordinary := NovaObjectData.new()
	assert_eq(ordinary.open_from_resource_root(resource_root, "MVEG6.3DI"), OK)
	assert_eq(NovaObjectData.network_challenge_model_count(), 0,
		"a cached foliage definition remains excluded when another path loads it normally")


func test_network_challenge_model_registry_deduplicates_and_resets() -> void:
	var resource_root := _prepare_veg_fixture("Mveg6.3di")
	NovaObjectData.reset_network_challenge_model_registry()
	var first := NovaObjectData.new()
	var duplicate := NovaObjectData.new()

	assert_eq(first.open_from_resource_root(resource_root, "Mveg6.3di"), OK)
	assert_eq(duplicate.open_from_resource_root(resource_root, "mVEG6.3di"), OK)
	assert_eq(NovaObjectData.network_challenge_model_count(), 1,
		"case variants of one mounted .3DI contribute one renderer definition")

	NovaObjectData.reset_network_challenge_model_registry()
	assert_eq(NovaObjectData.network_challenge_model_count(), 0,
		"mission cache destruction starts the next frozen page empty")


func test_resolve_slot_meshes_loads_graphic_resident_only_in_runtime_pff() -> void:
	var root_dir := _fixture_root()
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir), OK)
	var model_bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(SOURCE_OBJECT))
	assert_gt(model_bytes.size(), 0)
	_write_pff(root_dir.path_join('resource.pff'), [{
		'name': 'Mveg6.3di',
		'bytes': model_bytes,
	}])
	var resource_root := NovaResourceRoot.new()
	assert_eq(resource_root.mount_runtime(root_dir), OK)

	var def := NovaTerrainFoliageDef.new()
	def.graphic = 'Mveg6'
	var meshes := VegAssetsScript.resolve_slot_meshes(resource_root, [def])

	assert_true(resource_root.has_file('Mveg6.3di'),
		'The fixture model must be served by the packed runtime VFS.')
	assert_eq(meshes.size(), 1)
	assert_true(meshes[0] is Mesh,
		'Runtime foliage must build geometry when the authored model exists only in a PFF.')


func test_mesh_cache_does_not_alias_base_and_expansion_mounts_of_same_directory() -> void:
	var root_dir := _fixture_root()
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir.path_join('expansion/jox01')), OK)
	var base_bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(SOURCE_OBJECT))
	var expansion_bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(OVERRIDE_OBJECT))
	assert_gt(base_bytes.size(), 0)
	assert_gt(expansion_bytes.size(), 0)
	_write_pff(root_dir.path_join('resource.pff'), [{
		'name': 'Mveg6.3di',
		'bytes': base_bytes,
	}])
	_write_pff(root_dir.path_join('expansion/jox01/jox01.pff'), [{
		'name': 'Mveg6.3di',
		'bytes': expansion_bytes,
	}])
	var base_root := NovaResourceRoot.new()
	var expansion_root := NovaResourceRoot.new()
	assert_eq(base_root.mount_runtime(root_dir), OK)
	assert_eq(expansion_root.mount_runtime(root_dir, 'jox01'), OK)

	var base_mesh: Mesh = VegAssetsScript.load_mesh(base_root, 'Mveg6')
	var expansion_mesh: Mesh = VegAssetsScript.load_mesh(expansion_root, 'Mveg6')

	assert_not_null(base_mesh)
	assert_not_null(expansion_mesh)
	assert_ne(base_mesh, expansion_mesh,
		'Distinct live VFS mounts must never share a foliage mesh cache entry.')
	assert_ne(base_mesh.get_aabb(), expansion_mesh.get_aabb(),
		'The expansion override must produce its own authored geometry.')


func test_installed_dvxi5_foliage_assets_enable_every_authored_slot() -> void:
	var install_dir := OS.get_environment('OPENNOVA_JO_DIR')
	if install_dir.is_empty():
		pass_test('OPENNOVA_JO_DIR is unset; installed-game foliage asset check skipped.')
		return
	var resource_root := NovaResourceRoot.new()
	var expansion := OS.get_environment('JO_EXPANSION')
	assert_eq(resource_root.mount_runtime(install_dir, expansion, false, 'jo'), OK,
		'Installed JO root must mount through the production packed VFS.')
	var terrain := NovaTerrainData.new()
	assert_eq(terrain.load_from_resource_root(resource_root, 'Dvxi5.trn'), OK)
	var defs: Array = terrain.get_foliage_defs()
	assert_gt(defs.size(), 0, 'Dvxi5 must contain authored foliage definitions.')
	var meshes := VegAssetsScript.resolve_slot_meshes(resource_root, defs)
	var textures := VegAssetsScript.resolve_slot_fd_textures(resource_root, defs)
	var dispatcher := NovaFoliageDispatcher.new()
	add_child_autofree(dispatcher)
	dispatcher.configure_slots(defs, meshes, textures)

	var diagnostics: Array = dispatcher.get_slot_diagnostics()
	for slot in range(defs.size()):
		assert_true(meshes[slot] is Mesh,
			'Installed foliage graphic must resolve for authored slot %d.' % slot)
		assert_true(textures[slot] is Texture2D,
			'Installed foliage diffuse must produce :fd texture for slot %d.' % slot)
		assert_eq(String(diagnostics[slot].status), 'enabled',
			'Installed authored foliage slot %d must reach the renderer.' % slot)
	var stats := dispatcher.get_frame_stats()
	assert_eq(int(stats.enabled_slots), defs.size())
	assert_eq(int(stats.disabled_slots), 0)


func test_lod0_aggregation_keeps_every_submesh_surface() -> void:
	var first := ArrayMesh.new()
	_add_triangle_surface(first, 0.0)
	_add_triangle_surface(first, 10.0)
	var second := ArrayMesh.new()
	_add_triangle_surface(second, 20.0)

	var aggregate: ArrayMesh = VegAssetsScript._aggregate_lod0_submeshes([
		{"mesh": first, "material_index": 3},
		{"mesh": second, "material_index": 9},
	])

	assert_not_null(aggregate)
	assert_eq(aggregate.get_surface_count(), 3,
		"Foliage source geometry should retain every surface from every LOD0 submesh.")
	for surface_index in range(3):
		var arrays := aggregate.surface_get_arrays(surface_index)
		var vertices: PackedVector3Array = arrays[Mesh.ARRAY_VERTEX]
		assert_eq(vertices.size(), 3)
		assert_almost_eq(vertices[0].x, float(surface_index * 10), 0.0001,
			"LOD0 surfaces should remain in source order without losing their vertices.")


func test_cache_epoch_is_monotonic_and_bumped_by_mount() -> void:
	var before := NovaResourceRoot.cache_epoch()
	NovaResourceRoot.bump_cache_epoch()
	assert_gt(NovaResourceRoot.cache_epoch(), before, "Explicit bump should advance the epoch.")

	var at_bump := NovaResourceRoot.cache_epoch()
	_prepare_veg_fixture("Mveg6.3di")  # set_root_dir routes through mount_with_mode
	assert_gt(NovaResourceRoot.cache_epoch(), at_bump, "Any root mount should advance the epoch.")


func test_caches_self_clear_when_epoch_moves() -> void:
	var resource_root := _prepare_veg_fixture("Mveg6.3di")
	VegAssetsScript.list_graphics(resource_root, true)
	assert_false(VegAssetsScript._graphics_cache_by_root.is_empty(),
		"Listing should fill the graphics cache.")

	# Any mount/rescan/clear in this process bumps the global epoch; the next
	# cache access self-clears before refilling, so a rescanned resource dir is
	# never served a stale listing or mesh.
	NovaResourceRoot.bump_cache_epoch()
	VegAssetsScript._check_epoch()
	assert_true(VegAssetsScript._graphics_cache_by_root.is_empty(),
		"An epoch move should drop the listing cache on next access.")
	assert_true(VegAssetsScript._mesh_cache.is_empty(),
		"An epoch move should drop the mesh cache on next access.")


func _prepare_veg_fixture(filename: String) -> NovaResourceRoot:
	var root := _fixture_root()
	assert_eq(DirAccess.make_dir_recursive_absolute(root), OK)
	_copy_file(ProjectSettings.globalize_path(SOURCE_OBJECT), root.path_join(filename))
	var resource_root := NovaResourceRoot.new()
	assert_eq(resource_root.set_root_dir(root), OK)
	return resource_root


func _add_triangle_surface(mesh: ArrayMesh, x_offset: float) -> void:
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = PackedVector3Array([
		Vector3(x_offset, 0.0, 0.0),
		Vector3(x_offset + 1.0, 0.0, 0.0),
		Vector3(x_offset, 1.0, 0.0),
	])
	arrays[Mesh.ARRAY_TEX_UV] = PackedVector2Array([
		Vector2(0.0, 0.0), Vector2(1.0, 0.0), Vector2(0.0, 1.0),
	])
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2])
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)


func _fixture_root() -> String:
	return OS.get_cache_dir().path_join("opennova_veg_assets_test")


func _copy_file(src: String, dst: String) -> void:
	var bytes := FileAccess.get_file_as_bytes(src)
	assert_gt(bytes.size(), 0, "Fixture copy source should contain bytes: %s" % src)
	var file := FileAccess.open(dst, FileAccess.WRITE)
	assert_not_null(file, "Fixture copy destination should open: %s" % dst)
	if file == null:
		return
	file.store_buffer(bytes)
	file.close()


func _write_pff(path: String, entries: Array) -> void:
	var file := FileAccess.open(path, FileAccess.WRITE)
	assert_not_null(file, 'PFF fixture should be writable: %s' % path)
	if file == null:
		return
	var header_size := 20
	var entry_size := 36
	var next_payload_offset := header_size + entries.size() * entry_size
	file.store_32(header_size)
	file.store_32(0x33464650)
	file.store_32(entries.size())
	file.store_32(entry_size)
	file.store_32(header_size)
	for entry in entries:
		var bytes: PackedByteArray = entry.bytes
		file.store_32(0)
		file.store_32(next_payload_offset)
		file.store_32(bytes.size())
		file.store_32(0)
		var name_bytes := String(entry.name).to_utf8_buffer()
		for index in range(16):
			file.store_8(name_bytes[index] if index < name_bytes.size() else 0)
		file.store_32(0)
		next_payload_offset += bytes.size()
	for entry in entries:
		file.store_buffer(entry.bytes)
	file.close()


func _cleanup_dir(path: String) -> void:
	if not DirAccess.dir_exists_absolute(path):
		return
	for file in DirAccess.get_files_at(path):
		DirAccess.remove_absolute(path.path_join(file))
	for dir in DirAccess.get_directories_at(path):
		_cleanup_dir(path.path_join(dir))
	DirAccess.remove_absolute(path)
