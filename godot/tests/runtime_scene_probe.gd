extends Node

const VegAssets := preload("res://engine/terrain/veg_assets.gd")

# Headless validation of the main_game.tscn runtime pipeline. Loads the scene,
# points the shared GameWorld at an explicit resource dir (a synthesized one-root
# fixture by default, or a loose dir passed via `-- <dir>`), waits for NovaTerrainData,
# moves the camera onto a foliage-painted cell, then verifies that NovaTerrain
# hands exact native 16-unit detail cells through the complete GameWorld ->
# VegAssets -> dispatcher path and produces resident foliage geometry.
#
# Use: `godot --headless --path godot res://tests/runtime_scene_probe.tscn -- <dir>`

const FOLIAGE_MODEL_FIXTURE := "res://../fixtures/3dp/CmpFireN/CmpFireN.3di"


func _ready() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	var uses_default_fixture := args.is_empty()
	var dir := args[0] if not uses_default_fixture else _default_runtime_resource_root()

	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		push_error("runtime_scene_probe: failed to load main_game.tscn")
		get_tree().quit(1)
		return

	var scene := packed.instantiate()
	get_tree().root.add_child(scene)
	# main_game._ready already ran load_world() (no-op headless without a config
	# dir). The probe fixture is a flat authored/extracted root, so inject the
	# explicit loose-root seam instead of the production PFF-only mount path.
	var world: GameWorld = scene.get_node_or_null("World")
	var loose_root: NovaResourceRoot = null
	var load_result := ERR_UNAVAILABLE
	if world != null:
		loose_root = NovaResourceRoot.new()
		load_result = loose_root.set_root_dir(dir)
		if load_result == OK:
			world.set_resource_root(loose_root)
			load_result = world.load_world()
		# MainGame remains in its menu state when a probe calls GameWorld
		# directly. Make the loaded world visible so NovaTerrain runs its native
		# detail-cell collection just as it does after the menu handoff.
		world.visible = true

	var terrain: NovaTerrain = null
	var dispatcher: NovaFoliageDispatcher = null
	var overlay: NovaTerrainTileOverlay = null
	var camera: Camera3D = null
	var data: NovaTerrainData = null

	for _i in range(30):
		await get_tree().process_frame
		terrain = scene.get_node_or_null("World/NovaTerrain")
		dispatcher = scene.get_node_or_null("World/NovaTerrain/FoliageDispatcher")
		overlay = scene.get_node_or_null("World/NovaTerrain/TileOverlay")
		camera = scene.get_node_or_null("Camera3D")
		data = world.get_terrain_data() if world != null else null
		if data != null and data.is_loaded():
			break

	var foliage_probe := {}
	if data != null and data.is_loaded():
		foliage_probe = _find_foliage_world_point(data)
		if camera != null and foliage_probe.has("position"):
			var probe_pos: Vector3 = foliage_probe["position"]
			camera.global_position = probe_pos + Vector3(0.0, 2.2, 4.0)
			camera.look_at(probe_pos + Vector3(0.0, 0.5, -12.0), Vector3.UP)

	for _i in range(10):
		await get_tree().process_frame

	# Terrain texture resolution. These are null in a broken export (raw .tga
	# sources stripped from the PCK, decoded via FileAccess); after the
	# ResourceLoader fix they resolve to the imported CompressedTexture2D. Running
	# this probe inside the exported build is the real gate for the texture fix.
	var colormap_ok := data != null and data.get_colormap() != null
	var detail_ok := (
		data != null
		and data.get_detailmap_c1() != null
		and data.get_detailmap_c2() != null
		and data.get_detailmap_c3() != null
		and data.get_detailblendmap() != null
	)

	var foliage_stats := dispatcher.get_frame_stats() if dispatcher != null else {}
	var diagnostics := {
		"load_result": load_result,
		"terrain_node": terrain != null,
		"terrain_data": data != null,
		"terrain_loaded": data != null and data.is_loaded(),
		"colormap_resolved": colormap_ok,
		"detail_maps_resolved": detail_ok,
		"tilestrip_resolved": data != null and data.get_tilestrip_tex() != null,
		"foliage_defs": data.get_foliage_defs().size() if data != null else -1,
		"foliage_probe_found": foliage_probe.has("position"),
		"foliage_probe": foliage_probe,
		"dispatcher_total_instances": dispatcher.get_total_instances() if dispatcher != null else -1,
		"dispatcher_frame_stats": foliage_stats,
		"overlay_tile_info": overlay != null and overlay.tile_info != null,
		"overlay_tilestrip": overlay != null and overlay.tilestrip != null,
		"overlay_entries_rendered": overlay.get_entry_count_rendered() if overlay != null else -1,
		"overlay_entry_count": overlay.tile_info.get_entry_count() if overlay != null and overlay.tile_info != null else -1,
		"tilestrip_size": Vector2i(
			overlay.tilestrip.get_width(),
			overlay.tilestrip.get_height()
		) if overlay != null and overlay.tilestrip != null else Vector2i.ZERO,
	}
	print("runtime_scene_probe diagnostics: ", diagnostics)

	var failures: Array[String] = []

	if dispatcher == null:
		failures.append("expected NovaTerrain/FoliageDispatcher to exist")
	else:
		if not bool(foliage_stats.get("native_detail_source", false)):
			failures.append("expected runtime foliage to consume NovaTerrain's native detail-cell vector")
		if int(foliage_stats.get("detail_cells", 0)) <= 0:
			failures.append("expected at least one exact 16-unit detail cell near the camera")
		if int(foliage_stats.get("runtime_detail_intents", 0)) <= 0:
			failures.append("expected foliage-painted detail cells to emit foliage intents")
		if int(foliage_stats.get("detail_vertices", 0)) <= 0:
			failures.append("expected emitted foliage intents to expand into vertices")
		if int(foliage_stats.get("render_batches", 0)) <= 0:
			failures.append("expected expanded foliage geometry to submit render batches")
		if dispatcher.get_total_instances() <= 0:
			failures.append("expected at least one resident foliage instance")
	if terrain == null:
		failures.append("expected NovaTerrain to exist")

	if data != null and data.is_loaded():
		if not colormap_ok:
			failures.append("expected terrain colormap to resolve (null => export-stripped .tga)")
		if not detail_ok:
			failures.append("expected terrain detail/blend maps to resolve (null => export-stripped .tga)")

	if data == null or not data.is_loaded():
		failures.append("expected NovaTerrainData to be loaded")
	elif not foliage_probe.has("position"):
		failures.append("expected to find a Dvxi5 foliagemap cell with a matching foliage def")

	if failures.is_empty():
		print("runtime_scene_probe: OK")
	else:
		for failure in failures:
			push_error("runtime_scene_probe FAIL: " + failure)

	var exit_code := 0 if failures.is_empty() else 1
	if dispatcher != null:
		dispatcher.reset()
	if world != null:
		world.unload()
	data = null
	scene.queue_free()
	await get_tree().process_frame
	await get_tree().process_frame
	VegAssets.clear_cache()
	if loose_root != null:
		loose_root.clear()
	loose_root = null
	dispatcher = null
	world = null
	scene = null
	if uses_default_fixture:
		_cleanup_dir(dir)
	await get_tree().process_frame
	get_tree().quit(exit_code)


func _default_runtime_resource_root() -> String:
	# NovaResourceRoot deliberately rejects user:// as an authoring mount. Stage
	# the combined terrain+environment fixture under the OS temp directory.
	var root := OS.get_temp_dir().path_join("opennova_runtime_scene_probe_resource_root")
	DirAccess.make_dir_recursive_absolute(root)
	_copy_dir_files(ProjectSettings.globalize_path("res://../fixtures/godot/dvxi5"), root)
	_copy_file(
		ProjectSettings.globalize_path("res://../fixtures/env/full_00.env"),
		root.path_join("full_00.env")
	)
	# Dvxi5 names mveg5/mveg5b. The committed object is intentionally copied
	# under both authored names: this probe validates the runtime asset and
	# geometry path, not the visual identity of the retail vegetation model.
	var model_source := ProjectSettings.globalize_path(FOLIAGE_MODEL_FIXTURE)
	_copy_file(model_source, root.path_join("mveg5.3di"))
	_copy_file(model_source, root.path_join("mveg5b.3di"))
	return root


func _copy_dir_files(src_dir: String, dst_dir: String) -> void:
	var dir := DirAccess.open(src_dir)
	if dir == null:
		push_error("runtime_scene_probe: cannot open fixture dir " + src_dir)
		return
	dir.list_dir_begin()
	var filename := dir.get_next()
	while not filename.is_empty():
		if not dir.current_is_dir():
			_copy_file(src_dir.path_join(filename), dst_dir.path_join(filename))
		filename = dir.get_next()
	dir.list_dir_end()


func _copy_file(src_path: String, dst_path: String) -> void:
	var src := FileAccess.open(src_path, FileAccess.READ)
	if src == null:
		push_error("runtime_scene_probe: cannot read fixture file " + src_path)
		return
	var dst := FileAccess.open(dst_path, FileAccess.WRITE)
	if dst == null:
		src.close()
		push_error("runtime_scene_probe: cannot write fixture file " + dst_path)
		return
	dst.store_buffer(src.get_buffer(src.get_length()))
	dst.close()
	src.close()


func _cleanup_dir(path: String) -> void:
	if not DirAccess.dir_exists_absolute(path):
		return
	for filename in DirAccess.get_files_at(path):
		DirAccess.remove_absolute(path.path_join(filename))
	for directory in DirAccess.get_directories_at(path):
		_cleanup_dir(path.path_join(directory))
	DirAccess.remove_absolute(path)


func _find_foliage_world_point(data: NovaTerrainData) -> Dictionary:
	var defs: Array = data.get_foliage_defs()
	var grid := data.get_sector_grid()
	var origin_x := data.get_origin_x()
	var origin_y := data.get_origin_y()
	var best := {}
	var best_score := 0
	var best_gradient := -1.0
	var foliage_histogram := {}

	# Score regular samples within each native 16-unit cell. A fully matching
	# cell is a deterministic camera target without reproducing the foliage
	# runtime's private PRNG candidate positions in test code.
	for grid_y in range(16):
		for grid_x in range(16):
			var grid_index := grid_y * 16 + grid_x
			var sector_id := int(grid[grid_index]) if grid_index < grid.size() else 0
			if sector_id <= 0:
				continue
			var sector_x := float((origin_x + grid_x) * 512)
			var sector_z := float((origin_y + grid_y) * 512)
			for local_z in range(0, 512, 16):
				for local_x in range(0, 512, 16):
					var score := 0
					var painted := 0
					for offset_z in [2.0, 8.0, 14.0]:
						for offset_x in [2.0, 8.0, 14.0]:
							var sampled := int(data.get_detail_foliage_index_world(
								sector_x + float(local_x) + offset_x,
								sector_z + float(local_z) + offset_z
							))
							foliage_histogram[sampled] = int(foliage_histogram.get(sampled, 0)) + 1
							if _has_matching_foliage_def(defs, sampled):
								score += 1
								painted = sampled
					if score <= 0 or score < best_score:
						continue
					var world_x := sector_x + float(local_x) + 8.0
					var world_z := sector_z + float(local_z) + 8.0
					var center := Vector3(world_x, 0.0, world_z)
					var world_y: float = data.get_height_world_bilinear(center)
					var hx: float = data.get_height_world_bilinear(center + Vector3(8.0, 0.0, 0.0))
					var hz: float = data.get_height_world_bilinear(center + Vector3(0.0, 0.0, 8.0))
					var gradient: float = abs(hx - world_y) + abs(hz - world_y)
					if score == best_score and gradient <= best_gradient:
						continue
					best_score = score
					best_gradient = gradient
					best = {
						"position": Vector3(world_x, world_y, world_z),
						"painted": painted,
						"painted_samples": score,
						"gradient": gradient,
						"cell": Vector2i(int(sector_x) + local_x, int(sector_z) + local_z),
						"sector_id": sector_id,
						"grid_cell": Vector2i(grid_x, grid_y),
					}

	if best.is_empty():
		print("runtime_scene_probe: no foliage-def map match; sampled indices=", foliage_histogram)
	return best


func _has_matching_foliage_def(defs: Array, painted: int) -> bool:
	for value in defs:
		if not (value is NovaTerrainFoliageDef):
			continue
		var def := value as NovaTerrainFoliageDef
		if int(def.match) == painted:
			return true
	return false
