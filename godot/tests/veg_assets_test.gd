extends GutTest

const VegAssetsScript = preload("res://engine/terrain/veg_assets.gd")
const VegAssetsShim = preload("res://modtools/terrain/veg_assets.gd")
const SOURCE_OBJECT := "res://../fixtures/3dp/Bird1/Bird1.3di"


func before_each() -> void:
	_cleanup_dir(_fixture_root())
	VegAssetsScript.set_search_roots([])


func after_each() -> void:
	VegAssetsScript.set_search_roots([])
	_cleanup_dir(_fixture_root())


func test_list_graphics_preserves_actual_model_path_case() -> void:
	var root := _prepare_veg_fixture("Mveg6.3di")
	var graphics := VegAssetsScript.list_graphics(true)
	var mveg6_path := ""
	for entry in graphics:
		if String(entry.basename) == "mveg6":
			mveg6_path = String(entry.model_path)
			break

	assert_eq(
		mveg6_path,
		root.path_join("Mveg6.3di"),
		"Vegetation asset lookup should retain the real file case for portable object paths."
	)


func test_list_graphics_cache_returns_caller_safe_copy() -> void:
	_prepare_veg_fixture("Mveg6.3di")
	var graphics := VegAssetsScript.list_graphics(true)
	assert_gt(graphics.size(), 0, "Vegetation asset lookup should find configured .3di graphics.")

	graphics.clear()
	var cached_again := VegAssetsScript.list_graphics()

	assert_gt(cached_again.size(), 0, "Callers should not be able to mutate the shared vegetation graphics cache.")


func test_resolve_slot_meshes_preserves_slots_and_loads_known_graphic() -> void:
	_prepare_veg_fixture("Mveg6.3di")
	var def := NovaTerrainFoliageDef.new()
	def.graphic = "Mveg6.3di"

	var meshes := VegAssetsScript.resolve_slot_meshes([null, def])

	assert_eq(meshes.size(), 2, "Resolver should preserve the foliage slot array shape.")
	assert_null(meshes[0], "Null foliage defs should remain null mesh slots.")
	assert_true(meshes[1] is Mesh, "Resolver should load the mesh for a known .3di graphic.")


func test_modtools_veg_assets_class_forwards_to_shared_implementation() -> void:
	_prepare_veg_fixture("Mveg6.3di")
	var graphics := VegAssetsShim.list_graphics()

	assert_gt(graphics.size(), 0, "The compatibility VegAssets class should expose shared asset listing.")
	assert_true(
		VegAssetsShim.load_mesh("Mveg6") is Mesh,
		"The compatibility VegAssets class should expose shared mesh loading."
	)


func _prepare_veg_fixture(filename: String) -> String:
	var root := _fixture_root()
	assert_eq(DirAccess.make_dir_recursive_absolute(root), OK)
	_copy_file(ProjectSettings.globalize_path(SOURCE_OBJECT), root.path_join(filename))
	VegAssetsScript.set_search_roots([root])
	return root


func _fixture_root() -> String:
	return ProjectSettings.globalize_path("user://veg_assets_test")


func _copy_file(src: String, dst: String) -> void:
	var bytes := FileAccess.get_file_as_bytes(src)
	assert_gt(bytes.size(), 0, "Fixture copy source should contain bytes: %s" % src)
	var file := FileAccess.open(dst, FileAccess.WRITE)
	assert_not_null(file, "Fixture copy destination should open: %s" % dst)
	if file == null:
		return
	file.store_buffer(bytes)
	file.close()


func _cleanup_dir(path: String) -> void:
	if not DirAccess.dir_exists_absolute(path):
		return
	for file in DirAccess.get_files_at(path):
		DirAccess.remove_absolute(path.path_join(file))
	for dir in DirAccess.get_directories_at(path):
		_cleanup_dir(path.path_join(dir))
	DirAccess.remove_absolute(path)
