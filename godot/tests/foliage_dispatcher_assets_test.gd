extends GutTest

# The vegetation asset resolver on FoliageDispatcher (the former veg_assets.gd,
# folded into the dispatcher as instance state): the *veg*.3di listing, the
# LOD0 mesh aggregate, the :fd texture bake, the network-challenge foliage
# mark, and the per-root+instance cache keys.

const SOURCE_OBJECT := "res://../fixtures/threedi/synth/bird.3di"
const OVERRIDE_OBJECT := "res://../fixtures/threedi/synth/house.3di"

var _dispatcher: FoliageDispatcher = null


func before_each() -> void:
	TestFs.remove_dir_recursive(_fixture_root())
	_dispatcher = FoliageDispatcher.new()
	add_child_autofree(_dispatcher)
	ObjectData.reset_network_challenge_model_registry()


func after_each() -> void:
	_dispatcher = null
	ObjectData.reset_network_challenge_model_registry()
	TestFs.remove_dir_recursive(_fixture_root())


func test_resolve_slot_meshes_preserves_slots_and_loads_known_graphic() -> void:
	var resource_root := _prepare_veg_fixture("Mveg6.3di")
	var def := TerrainFoliageDef.new()
	def.graphic = "Mveg6.3di"

	var meshes := _dispatcher.resolve_slot_meshes(resource_root, [null, def])

	assert_eq(meshes.size(), 2, "Resolver should preserve the foliage slot array shape.")
	assert_null(meshes[0], "Null foliage defs should remain null mesh slots.")
	assert_true(meshes[1] is Mesh, "Resolver should load the mesh for a known .3di graphic.")


func test_foliage_model_is_sticky_excluded_from_network_challenge_snapshot() -> void:
	var resource_root := _prepare_veg_fixture("Mveg6.3di")
	ObjectData.reset_network_challenge_model_registry()

	assert_not_null(_dispatcher.load_mesh(resource_root, "Mveg6"))
	assert_eq(ObjectData.network_challenge_model_count(), 0,
		"foliage geometry loads normally but its model-def row is excluded")

	# Retail marks the shared definition node, so a later ordinary lookup of the
	# same filename does not clear the foliage bit before the snapshot.
	var ordinary := ObjectData.new()
	assert_eq(ordinary.open_from_resource_root(resource_root, "MVEG6.3DI"), OK)
	assert_eq(ObjectData.network_challenge_model_count(), 0,
		"the foliage marker is sticky on a case-folded shared definition")


func test_cached_foliage_hit_restores_marker_after_mission_registry_reset() -> void:
	var resource_root := _prepare_veg_fixture("Mveg6.3di")
	ObjectData.reset_network_challenge_model_registry()

	var first_mesh := _dispatcher.load_mesh(resource_root, "Mveg6")
	assert_not_null(first_mesh)
	assert_eq(ObjectData.network_challenge_model_count(), 0)

	# Game_StartMission destroys the logical loaded-definition registry, while
	# the world keeps the mounted resource root and the dispatcher's renderer
	# cache alive. The next mission's cache hit must therefore recreate the
	# model-def's sticky foliage mark without reparsing or rebuilding its mesh.
	ObjectData.reset_network_challenge_model_registry()
	var cached_mesh := _dispatcher.load_mesh(resource_root, "mVEG6.3di")
	assert_same(cached_mesh, first_mesh, "the second mission takes the real mesh-cache hit")

	var ordinary := ObjectData.new()
	assert_eq(ordinary.open_from_resource_root(resource_root, "MVEG6.3DI"), OK)
	assert_eq(ObjectData.network_challenge_model_count(), 0,
		"a cached foliage definition remains excluded when another path loads it normally")


func test_network_challenge_model_registry_deduplicates_and_resets() -> void:
	var resource_root := _prepare_veg_fixture("Mveg6.3di")
	ObjectData.reset_network_challenge_model_registry()
	var first := ObjectData.new()
	var duplicate := ObjectData.new()

	assert_eq(first.open_from_resource_root(resource_root, "Mveg6.3di"), OK)
	assert_eq(duplicate.open_from_resource_root(resource_root, "mVEG6.3di"), OK)
	assert_eq(ObjectData.network_challenge_model_count(), 1,
		"case variants of one mounted .3DI contribute one renderer definition")

	ObjectData.reset_network_challenge_model_registry()
	assert_eq(ObjectData.network_challenge_model_count(), 0,
		"mission cache destruction starts the next frozen page empty")


func test_resolve_slot_meshes_loads_graphic_resident_only_in_runtime_pff() -> void:
	var root_dir := _fixture_root()
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir), OK)
	var model_bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(SOURCE_OBJECT))
	assert_gt(model_bytes.size(), 0)
	WorldFixture.write_pff(self, root_dir.path_join('resource.pff'), [{
		'name': 'Mveg6.3di',
		'bytes': model_bytes,
	}])
	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.mount_runtime(root_dir), OK)

	var def := TerrainFoliageDef.new()
	def.graphic = 'Mveg6'
	var meshes := _dispatcher.resolve_slot_meshes(resource_root, [def])

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
	WorldFixture.write_pff(self, root_dir.path_join('resource.pff'), [{
		'name': 'Mveg6.3di',
		'bytes': base_bytes,
	}])
	WorldFixture.write_pff(self, root_dir.path_join('expansion/jox01/jox01.pff'), [{
		'name': 'Mveg6.3di',
		'bytes': expansion_bytes,
	}])
	var base_root := ResourceRoot.new()
	var expansion_root := ResourceRoot.new()
	assert_eq(base_root.mount_runtime(root_dir), OK)
	assert_eq(expansion_root.mount_runtime(root_dir, 'jox01'), OK)

	var base_mesh: Mesh = _dispatcher.load_mesh(base_root, 'Mveg6')
	var expansion_mesh: Mesh = _dispatcher.load_mesh(expansion_root, 'Mveg6')

	assert_not_null(base_mesh)
	assert_not_null(expansion_mesh)
	assert_ne(base_mesh, expansion_mesh,
		'Distinct live VFS mounts must never share a foliage mesh cache entry.')
	assert_ne(base_mesh.get_aabb(), expansion_mesh.get_aabb(),
		'The expansion override must produce its own authored geometry.')


func test_lod0_aggregation_keeps_every_submesh_surface() -> void:
	var first := ArrayMesh.new()
	_add_triangle_surface(first, 0.0)
	_add_triangle_surface(first, 10.0)
	var second := ArrayMesh.new()
	_add_triangle_surface(second, 20.0)

	var aggregate: ArrayMesh = FoliageDispatcher.aggregate_lod0_submeshes([
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
	var before := ResourceRoot.cache_epoch()
	ResourceRoot.bump_cache_epoch()
	assert_gt(ResourceRoot.cache_epoch(), before, "Explicit bump should advance the epoch.")

	var at_bump := ResourceRoot.cache_epoch()
	_prepare_veg_fixture("Mveg6.3di")  # set_root_dir routes through mount_with_mode
	assert_gt(ResourceRoot.cache_epoch(), at_bump, "Any root mount should advance the epoch.")


func test_caches_self_clear_when_epoch_moves() -> void:
	var resource_root := _prepare_veg_fixture("Mveg6.3di")
	assert_not_null(_dispatcher.load_mesh(resource_root, "Mveg6"))
	assert_gt(_dispatcher.asset_cache_entry_count(), 0,
		"Loading should fill the listing and mesh caches.")

	# Any mount/rescan/clear in this process bumps the global epoch; the next
	# cache access self-clears before refilling, so a rescanned resource dir is
	# never served a stale listing or mesh.
	ResourceRoot.bump_cache_epoch()
	# The count is itself a cache reader: it runs the epoch check (the
	# self-clear) and refills nothing.
	assert_eq(_dispatcher.asset_cache_entry_count(), 0,
		"An epoch move should drop every cache (listing and mesh) on next access.")


func test_clear_asset_cache_drops_every_entry() -> void:
	var resource_root := _prepare_veg_fixture("Mveg6.3di")
	assert_not_null(_dispatcher.load_mesh(resource_root, "Mveg6"))
	assert_gt(_dispatcher.asset_cache_entry_count(), 0,
		"Listing and loading fill the dispatcher's asset caches.")

	# The caches are instance state keyed by root + graphic; beside the epoch
	# self-clear above, the shell's exit (MainGame) empties them explicitly
	# and they die with the dispatcher.
	_dispatcher.clear_asset_cache()
	assert_eq(_dispatcher.asset_cache_entry_count(), 0,
		"clear_asset_cache drops every cache (listing, path, mesh and :fd).")


# Every foliage slot a terrain authors resolves end to end: the minted Tmap
# (tests/fixtures/minimal_terrain_gen.cpp) authors two foliage blocks in the
# shape of the retail Dvxi5 this witness first ran on (Dvxi5: mveg5b.3di
# matching 254, mveg5.3di matching 253; Tmap: bush1.3di on 254, bush2.3di on
# 253), each graphic a synthetic model whose diffuse is staged beside it.
func test_terrain_foliage_assets_enable_every_authored_slot() -> void:
	var resource_root := _prepare_terrain_foliage_fixture()
	var terrain := TerrainData.new()
	assert_eq(terrain.load_from_resource_root(resource_root, TestFs.TMAP_TRN), OK)
	var defs: Array = terrain.get_foliage_defs()
	assert_gt(defs.size(), 0, 'Tmap must contain authored foliage definitions.')
	var meshes := _dispatcher.resolve_slot_meshes(resource_root, defs)
	var textures := _dispatcher.resolve_slot_fd_textures(resource_root, defs)
	_dispatcher.configure_slots(defs, meshes, textures)

	var diagnostics: Array = _dispatcher.get_slot_diagnostics()
	for slot in range(defs.size()):
		assert_true(meshes[slot] is Mesh,
			'The foliage graphic must resolve for authored slot %d.' % slot)
		assert_true(textures[slot] is Texture2D,
			'The foliage diffuse must produce an :fd texture for slot %d.' % slot)
		assert_eq(String(diagnostics[slot].status), 'enabled',
			'Authored foliage slot %d must reach the renderer.' % slot)
	var stats := _dispatcher.get_frame_stats()
	assert_eq(int(stats.enabled_slots), defs.size())
	assert_eq(int(stats.disabled_slots), 0)


# The Tmap terrain over the minimal assets it names, its two foliage graphics
# (the bird and the house) and their materials' diffuse textures.
func _prepare_terrain_foliage_fixture() -> ResourceRoot:
	var root := _fixture_root()
	assert_eq(DirAccess.make_dir_recursive_absolute(root), OK)
	for source in [TestFs.TMAP_FIXTURE_DIR, TestFs.BOOT_FIXTURE_DIR]:
		var source_dir := ProjectSettings.globalize_path(source)
		for file_name in DirAccess.get_files_at(source_dir):
			TestFs.copy(self, source_dir.path_join(file_name), root.path_join(file_name))
	TestFs.copy(self, ProjectSettings.globalize_path(SOURCE_OBJECT), root.path_join("bush1.3di"))
	TestFs.copy(self, ProjectSettings.globalize_path(OVERRIDE_OBJECT), root.path_join("bush2.3di"))
	for texture in ["bird.tga", "wall.tga", "roof.tga", "wood.tga"]:
		TestFs.write_bytes(self, root.path_join(texture),
				TestFs.tga_bytes(Vector2i(64, 64), Color(0.25, 0.5, 0.25, 1.0)))
	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.set_root_dir(root), OK)
	return resource_root


func _prepare_veg_fixture(filename: String) -> ResourceRoot:
	var root := _fixture_root()
	assert_eq(DirAccess.make_dir_recursive_absolute(root), OK)
	TestFs.copy(self, ProjectSettings.globalize_path(SOURCE_OBJECT), root.path_join(filename))
	var resource_root := ResourceRoot.new()
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
	return OS.get_cache_dir().path_join("opennova_foliage_dispatcher_assets_test")
