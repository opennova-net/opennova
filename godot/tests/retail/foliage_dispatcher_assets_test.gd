extends GutTest

# The vegetation asset resolver on FoliageDispatcher (the former veg_assets.gd,
# folded into the dispatcher as instance state): the *veg*.3di listing, the
# LOD0 mesh aggregate, the :fd texture bake, the network-challenge foliage
# mark, and the per-root+instance cache keys.

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


func test_installed_dvxi5_foliage_assets_enable_every_authored_slot() -> void:
	var resource_root := RetailData.mount_install_with('Dvxi5.trn')
	if resource_root == null:
		pending('OPENNOVA_JO_DIR / retail JO PFFs serving Dvxi5.trn are required for the installed foliage check')
		return
	var terrain := TerrainData.new()
	assert_eq(terrain.load_from_resource_root(resource_root, 'Dvxi5.trn'), OK)
	var defs: Array = terrain.get_foliage_defs()
	assert_gt(defs.size(), 0, 'Dvxi5 must contain authored foliage definitions.')
	var meshes := _dispatcher.resolve_slot_meshes(resource_root, defs)
	var textures := _dispatcher.resolve_slot_fd_textures(resource_root, defs)
	_dispatcher.configure_slots(defs, meshes, textures)

	var diagnostics: Array = _dispatcher.get_slot_diagnostics()
	for slot in range(defs.size()):
		assert_true(meshes[slot] is Mesh,
			'Installed foliage graphic must resolve for authored slot %d.' % slot)
		assert_true(textures[slot] is Texture2D,
			'Installed foliage diffuse must produce :fd texture for slot %d.' % slot)
		assert_eq(String(diagnostics[slot].status), 'enabled',
			'Installed authored foliage slot %d must reach the renderer.' % slot)
	var stats := _dispatcher.get_frame_stats()
	assert_eq(int(stats.enabled_slots), defs.size())
	assert_eq(int(stats.disabled_slots), 0)


func _fixture_root() -> String:
	return OS.get_cache_dir().path_join("opennova_foliage_dispatcher_assets_test")
