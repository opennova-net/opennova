extends GutTest

const VegAssetsScript = preload("res://engine/terrain/veg_assets.gd")
const VegAssetsShim = preload("res://modtools/terrain/veg_assets.gd")


func test_list_graphics_preserves_actual_scene_path_case() -> void:
	var graphics := VegAssetsScript.list_graphics(true)
	var mveg6_path := ""
	for entry in graphics:
		if String(entry.basename) == "mveg6":
			mveg6_path = String(entry.scene_path)
			break

	assert_eq(
		mveg6_path,
		"res://modtools/assets/models/Mveg6.glb",
		"Vegetation asset lookup should retain the real file case for portable ResourceLoader paths."
	)


func test_list_graphics_cache_returns_caller_safe_copy() -> void:
	var graphics := VegAssetsScript.list_graphics(true)
	assert_gt(graphics.size(), 0, "Vegetation asset lookup should find shipped graphics.")

	graphics.clear()
	var cached_again := VegAssetsScript.list_graphics()

	assert_gt(cached_again.size(), 0, "Callers should not be able to mutate the shared vegetation graphics cache.")


func test_resolve_slot_meshes_preserves_slots_and_loads_known_graphic() -> void:
	var def := NovaTerrainFoliageDef.new()
	def.graphic = "Mveg6.3di"

	var meshes := VegAssetsScript.resolve_slot_meshes([null, def])

	assert_eq(meshes.size(), 2, "Resolver should preserve the foliage slot array shape.")
	assert_null(meshes[0], "Null foliage defs should remain null mesh slots.")
	assert_true(meshes[1] is Mesh, "Resolver should load the mesh for a known graphic regardless of source extension.")


func test_modtools_veg_assets_class_forwards_to_shared_implementation() -> void:
	var graphics := VegAssetsShim.list_graphics()

	assert_gt(graphics.size(), 0, "The compatibility VegAssets class should expose shared asset listing.")
	assert_true(
		VegAssetsShim.load_mesh("Mveg6") is Mesh,
		"The compatibility VegAssets class should expose shared mesh loading."
	)
