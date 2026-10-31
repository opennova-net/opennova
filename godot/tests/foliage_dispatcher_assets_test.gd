extends GutTest

# The vegetation asset resolver on FoliageDispatcher (the former veg_assets.gd,
# folded into the dispatcher as instance state), driven through the load's own
# path (configure_for_terrain over a terrain's definitions) and read through the
# slot diagnostics: the model lookup (the *veg*.3di listing, the VFS, a PFF),
# the :fd texture, the per-root+instance cache keys and the cache epoch. The
# model asset itself (the LOD0 geometry, its bounds, the :fd row, which slots
# draw) is the engine's, pinned by ctest foliage_assets. (The C2S 0x3D page's
# foliage marks are the engine's, inmatch/loaded_model_list.h, pinned by ctest
# npruntime_loaded_model_list.)

const SOURCE_OBJECT := "res://../fixtures/threedi/synth/bird.3di"
const OVERRIDE_OBJECT := "res://../fixtures/threedi/synth/house.3di"

var _dispatcher: FoliageDispatcher = null


func before_each() -> void:
	TestFs.remove_dir_recursive(_fixture_root())
	TestFs.remove_dir_recursive(_terrain_root())
	_dispatcher = FoliageDispatcher.new()
	add_child_autofree(_dispatcher)


func after_each() -> void:
	if _dispatcher != null:
		_dispatcher.terrain_data = null
	_dispatcher = null
	TestFs.remove_dir_recursive(_fixture_root())
	TestFs.remove_dir_recursive(_terrain_root())


# Every slot reports why it draws or not: Tmap authors two definitions (bush1.3di
# on 254, bush2.3di on 253) in the four retail slots, and only bush1's model is
# staged. A definition whose model loaded draws; one whose model is missing and
# an empty slot do not (assets::foliage_slot_status; its invalid-geometry leg is
# the engine ctest's).
func test_slot_diagnostics_explain_every_slot_that_cannot_render() -> void:
	var resource_root := _prepare_terrain_foliage_fixture(false)
	var terrain := _tmap(resource_root)
	_dispatcher.configure_for_terrain(resource_root, terrain, null)

	var diagnostics: Array = _dispatcher.get_slot_diagnostics()
	assert_eq(diagnostics.size(), 4, "one row per retail slot")
	if diagnostics.size() < 4:
		return
	assert_eq(String(diagnostics[0].status), 'enabled')
	assert_true(bool(diagnostics[0].mesh_supplied))
	assert_gt(int(diagnostics[0].source_vertices), 0, "bush1's LOD0 reaches the slot")
	assert_eq(String(diagnostics[1].status), 'missing_mesh')
	assert_eq(String(diagnostics[1].graphic), 'bush2.3di')
	assert_false(bool(diagnostics[1].mesh_supplied))
	assert_eq(String(diagnostics[2].status), 'missing_definition')
	assert_eq(String(diagnostics[3].status), 'missing_definition')

	var stats := _dispatcher.get_frame_stats()
	assert_eq(stats.authored_slots, 2)
	assert_eq(stats.enabled_slots, 1)
	assert_eq(stats.disabled_slots, 1)


func test_definition_graphic_resident_only_in_runtime_pff_resolves() -> void:
	var root_dir := _fixture_root()
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir), OK)
	var model_bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(SOURCE_OBJECT))
	assert_gt(model_bytes.size(), 0)
	WorldFixture.write_pff(self, root_dir.path_join('resource.pff'), [
		{'name': 'bush1.3di', 'bytes': model_bytes},
		{'name': 'bush2.3di', 'bytes': model_bytes},
	])
	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.mount_runtime(root_dir), OK)
	assert_true(resource_root.has_file('bush1.3di'),
		'The fixture model must be served by the packed runtime VFS.')

	_dispatcher.configure_for_terrain(resource_root, _tmap(_prepare_terrain_root()), null)
	var diagnostics: Array = _dispatcher.get_slot_diagnostics()
	for slot in range(2):
		assert_eq(String(diagnostics[slot].status), 'enabled',
			'Runtime foliage must build slot %d when its model exists only in a PFF.' % slot)


func test_model_cache_does_not_alias_base_and_expansion_mounts_of_same_directory() -> void:
	var root_dir := _fixture_root()
	assert_eq(DirAccess.make_dir_recursive_absolute(root_dir.path_join('expansion/jox01')), OK)
	var base_bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(SOURCE_OBJECT))
	var expansion_bytes := FileAccess.get_file_as_bytes(ProjectSettings.globalize_path(OVERRIDE_OBJECT))
	assert_gt(base_bytes.size(), 0)
	assert_gt(expansion_bytes.size(), 0)
	WorldFixture.write_pff(self, root_dir.path_join('resource.pff'), [{
		'name': 'bush1.3di',
		'bytes': base_bytes,
	}])
	WorldFixture.write_pff(self, root_dir.path_join('expansion/jox01/jox01.pff'), [{
		'name': 'bush1.3di',
		'bytes': expansion_bytes,
	}])
	var base_root := ResourceRoot.new()
	var expansion_root := ResourceRoot.new()
	assert_eq(base_root.mount_runtime(root_dir), OK)
	assert_eq(expansion_root.mount_runtime(root_dir, 'jox01'), OK)
	var terrain := _tmap(_prepare_terrain_root())

	_dispatcher.configure_for_terrain(base_root, terrain, null)
	var base_slot: Dictionary = _dispatcher.get_slot_diagnostics()[0]
	_dispatcher.configure_for_terrain(expansion_root, terrain, null)
	var expansion_slot: Dictionary = _dispatcher.get_slot_diagnostics()[0]

	assert_eq(String(base_slot.status), 'enabled')
	assert_eq(String(expansion_slot.status), 'enabled')
	assert_ne(int(base_slot.source_vertices), int(expansion_slot.source_vertices),
		'Distinct live VFS mounts must never share a foliage model cache entry: the ' +
		'expansion override produces its own authored geometry.')


func test_cache_epoch_is_monotonic_and_bumped_by_mount() -> void:
	var before := ResourceRoot.cache_epoch()
	ResourceRoot.bump_cache_epoch()
	assert_gt(ResourceRoot.cache_epoch(), before, "Explicit bump should advance the epoch.")

	var at_bump := ResourceRoot.cache_epoch()
	_prepare_terrain_foliage_fixture(true)  # set_root_dir routes through mount_with_mode
	assert_gt(ResourceRoot.cache_epoch(), at_bump, "Any root mount should advance the epoch.")


func test_caches_self_clear_when_epoch_moves() -> void:
	var resource_root := _prepare_terrain_foliage_fixture(true)
	_dispatcher.configure_for_terrain(resource_root, _tmap(resource_root), null)
	assert_gt(_dispatcher.asset_cache_entry_count(), 0,
		"Configuring should fill the listing, model and :fd caches.")

	# Any mount/rescan/clear in this process bumps the global epoch; the next
	# cache access self-clears before refilling, so a rescanned resource dir is
	# never served a stale listing or model.
	ResourceRoot.bump_cache_epoch()
	# The count is itself a cache reader: it runs the epoch check (the
	# self-clear) and refills nothing.
	assert_eq(_dispatcher.asset_cache_entry_count(), 0,
		"An epoch move should drop every cache (listing and model) on next access.")


func test_clear_asset_cache_drops_every_entry() -> void:
	var resource_root := _prepare_terrain_foliage_fixture(true)
	_dispatcher.configure_for_terrain(resource_root, _tmap(resource_root), null)
	assert_gt(_dispatcher.asset_cache_entry_count(), 0,
		"Configuring fills the dispatcher's asset caches.")

	# The caches are instance state keyed by root + graphic; beside the epoch
	# self-clear above, the shell's exit (MainGame) empties them explicitly
	# and they die with the dispatcher.
	_dispatcher.clear_asset_cache()
	assert_eq(_dispatcher.asset_cache_entry_count(), 0,
		"clear_asset_cache drops every cache (listing, path, model and :fd).")


# Every foliage slot a terrain authors resolves end to end: the minted Tmap
# (tests/fixtures/minimal_terrain_gen.cpp) authors two foliage blocks in the
# shape of the retail Dvxi5 this witness first ran on (Dvxi5: mveg5b.3di
# matching 254, mveg5.3di matching 253; Tmap: bush1.3di on 254, bush2.3di on
# 253), each graphic a synthetic model whose diffuse is staged beside it.
func test_terrain_foliage_assets_enable_every_authored_slot() -> void:
	var resource_root := _prepare_terrain_foliage_fixture(true)
	var terrain := _tmap(resource_root)
	var defs: Array = terrain.get_foliage_defs()
	assert_gt(defs.size(), 0, 'Tmap must contain authored foliage definitions.')
	_dispatcher.configure_for_terrain(resource_root, terrain, null)

	var diagnostics: Array = _dispatcher.get_slot_diagnostics()
	for slot in range(defs.size()):
		assert_true(bool(diagnostics[slot].mesh_supplied),
			'The foliage graphic must resolve for authored slot %d.' % slot)
		assert_true(bool(diagnostics[slot].fd_texture_loaded),
			'The foliage diffuse must produce an :fd texture for slot %d.' % slot)
		assert_eq(String(diagnostics[slot].status), 'enabled',
			'Authored foliage slot %d must reach the renderer.' % slot)
	var stats := _dispatcher.get_frame_stats()
	assert_eq(int(stats.enabled_slots), defs.size())
	assert_eq(int(stats.disabled_slots), 0)


func _tmap(resource_root: ResourceRoot) -> TerrainData:
	var terrain := TerrainData.new()
	assert_eq(terrain.load_from_resource_root(resource_root, TestFs.TMAP_TRN), OK)
	return terrain


# The Tmap terrain over the minimal assets it names, and (with `models`) its
# two foliage graphics (the bird and the house) and their materials' diffuse
# textures; without, only bush1's.
func _prepare_terrain_foliage_fixture(models: bool) -> ResourceRoot:
	var root := _fixture_root()
	_stage_tmap(root)
	TestFs.stage_tmap_foliage(root, -1)
	if not models:
		DirAccess.remove_absolute(root.path_join("bush2.3di"))
	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.set_root_dir(root), OK)
	return resource_root


# The Tmap terrain alone, in a root of its own (its definitions' models are
# looked up through another root).
func _prepare_terrain_root() -> ResourceRoot:
	var root := _terrain_root()
	_stage_tmap(root)
	var resource_root := ResourceRoot.new()
	assert_eq(resource_root.set_root_dir(root), OK)
	return resource_root


func _stage_tmap(root: String) -> void:
	assert_eq(DirAccess.make_dir_recursive_absolute(root), OK)
	for source in [TestFs.TMAP_FIXTURE_DIR, TestFs.BOOT_FIXTURE_DIR]:
		var source_dir := ProjectSettings.globalize_path(source)
		for file_name in DirAccess.get_files_at(source_dir):
			TestFs.copy(self, source_dir.path_join(file_name), root.path_join(file_name))


func _fixture_root() -> String:
	return OS.get_cache_dir().path_join("opennova_foliage_dispatcher_assets_test")


func _terrain_root() -> String:
	return OS.get_cache_dir().path_join("opennova_foliage_dispatcher_assets_terrain")
