extends SceneTree

# Headless validation of the main_game.tscn runtime pipeline. Loads the scene,
# points the shared GameWorld at an explicit resource dir (a synthesized one-root
# fixture by default, or a dir passed via `-- <dir>`), waits for NovaTerrainData,
# moves the camera onto a painted foliage point, then verifies the foliage
# dispatcher is using the coverage-safe runtime path. There is no runtime fallback;
# the probe chooses the directory itself.
#
# Use: `godot --headless --path godot -s res://tests/runtime_scene_probe.gd -- <dir>`

const INVALID_CELL := Vector2i(-9999, -9999)


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var args := OS.get_cmdline_user_args()
	var dir := args[0] if args.size() >= 1 else _default_runtime_resource_root()

	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		push_error("runtime_scene_probe: failed to load main_game.tscn")
		quit(1)
		return

	var scene := packed.instantiate()
	root.add_child(scene)
	# main_game._ready already ran load_world() (no-op headless without a config
	# dir); drive the shared world loader directly at our chosen dir, no persist.
	var world: GameWorld = scene.get_node_or_null("World")
	if world != null:
		world.load_world(dir)

	var terrain: NovaTerrain = null
	var dispatcher: NovaFoliageDispatcher = null
	var overlay: NovaTerrainTileOverlay = null
	var camera: Camera3D = null
	var data: NovaTerrainData = null

	for _i in range(30):
		await process_frame
		terrain = scene.get_node_or_null("World/NovaTerrain")
		dispatcher = scene.get_node_or_null("World/NovaTerrain/FoliageDispatcher")
		overlay = scene.get_node_or_null("World/NovaTerrain/TileOverlay")
		camera = scene.get_node_or_null("Camera3D")
		data = world.get_terrain_data() if world != null else null
		if data != null and data.is_loaded():
			break

	var painted_probe := {}
	if data != null and data.is_loaded():
		painted_probe = _find_painted_world_point(data)
		if camera != null and painted_probe.has("position"):
			var probe_pos: Vector3 = painted_probe["position"]
			camera.global_position = Vector3(probe_pos.x, probe_pos.y + 20.0, probe_pos.z)

	for _i in range(10):
		await process_frame

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
	var far_cell_node_count := 0
	var model_draw_node_count := 0
	if dispatcher != null:
		for child in dispatcher.get_children():
			var child_name := String(child.name)
			if child_name.begins_with("FarCell"):
				far_cell_node_count += 1
			elif child_name.begins_with("FoliageModelDraw"):
				model_draw_node_count += 1

	var diagnostics := {
		"terrain_node": terrain != null,
		"terrain_data": data != null,
		"terrain_loaded": data != null and data.is_loaded(),
		"colormap_resolved": colormap_ok,
		"detail_maps_resolved": detail_ok,
		"tilestrip_resolved": data != null and data.get_tilestrip_tex() != null,
		"foliage_defs": data.get_foliage_defs().size() if data != null else -1,
		"painted_probe_found": painted_probe.has("position"),
		"painted_probe": painted_probe,
		"dispatcher_total_instances": dispatcher.get_total_instances() if dispatcher != null else -1,
		"dispatcher_cached_cells": dispatcher.get_cached_cells() if dispatcher != null else -1,
		"dispatcher_stats": dispatcher.get_dispatch_stats() if dispatcher != null else {},
		"far_cell_nodes": far_cell_node_count,
		"model_draw_nodes": model_draw_node_count,
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
	elif dispatcher.get_cached_cells() <= 0:
		failures.append(
			"expected dispatcher.cached_cells > 0 after the 42u near-cell dispatch, got %d"
				% dispatcher.get_cached_cells()
		)
	if terrain == null:
		failures.append("expected NovaTerrain to exist")

	if data != null and data.is_loaded():
		if not colormap_ok:
			failures.append("expected terrain colormap to resolve (null => export-stripped .tga)")
		if not detail_ok:
			failures.append("expected terrain detail/blend maps to resolve (null => export-stripped .tga)")

	if data == null or not data.is_loaded():
		failures.append("expected NovaTerrainData to be loaded")
	elif not painted_probe.has("position"):
		failures.append("expected to find a painted Dvxi5 foliage point with a matching foliage def")
	elif dispatcher != null and dispatcher.get_total_instances() <= 0:
		failures.append(
			"expected dispatcher.total_instances > 0 after moving camera to painted foliage, got %d"
				% dispatcher.get_total_instances()
		)

	if dispatcher != null and data != null:
		var defs: Array = data.get_foliage_defs()
		for child in dispatcher.get_children():
			var name_str := String(child.name)
			if name_str.begins_with("FarCell"):
				if not (child is MeshInstance3D):
					failures.append("%s: expected a MeshInstance3D FAR pool node" % name_str)
					continue
				var key_separator := name_str.find("_", "FarCell".length())
				var slot_text := ""
				if key_separator > "FarCell".length():
					slot_text = name_str.substr(
						"FarCell".length(), key_separator - "FarCell".length()
					)
				if not slot_text.is_valid_int():
					failures.append("%s: expected FarCell<slot>_<key> naming" % name_str)
					continue
				var slot_index := int(slot_text)
				if slot_index < 0 or slot_index >= defs.size():
					failures.append("%s: foliage slot %d is outside the def table" % [name_str, slot_index])
					continue
				var def: NovaTerrainFoliageDef = defs[slot_index]
				if def == null:
					failures.append("%s: foliage slot %d has no def" % [name_str, slot_index])
					continue
				var far_node := child as MeshInstance3D
				var wants_shadow := (int(def.attrib_flags) & NovaTerrainFoliageDef.ATTRIB_SHADOW) != 0
				var expected := GeometryInstance3D.SHADOW_CASTING_SETTING_ON if wants_shadow \
					else GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
				if far_node.cast_shadow != expected:
					failures.append(
						"%s: SHADOW attrib=%s, expected shadow_setting=%d, got %d"
							% [name_str, wants_shadow, expected, far_node.cast_shadow]
					)
				var far_material := far_node.material_override as ShaderMaterial
				if far_material == null or far_material.shader == null \
						or not far_material.shader.resource_path.ends_with("foliage_far.gdshader"):
					failures.append("%s: expected the FAR foliage shader material" % name_str)
			elif name_str.begins_with("FoliageModelDraw"):
				if not (child is MultiMeshInstance3D):
					failures.append("%s: expected a MultiMeshInstance3D MODEL pool node" % name_str)
					continue
				var model_node := child as MultiMeshInstance3D
				if model_node.multimesh == null:
					failures.append("%s: MODEL pool node has no MultiMesh" % name_str)
				var model_material := model_node.material_override as ShaderMaterial
				if model_material == null or model_material.shader == null \
						or not model_material.shader.resource_path.ends_with("foliage_model.gdshader"):
					failures.append("%s: expected the MODEL foliage shader material" % name_str)

	# FAR vertex COLOR is the source-height wind weight. Lighting comes from
	# the distinct FAR shader's terrain-light texture/c6 inputs; treating the
	# instance channel as a CPU terrain tint was a false-positive probe.

	if failures.is_empty():
		print("runtime_scene_probe: OK")
	else:
		for failure in failures:
			push_error("runtime_scene_probe FAIL: " + failure)

	scene.queue_free()
	await process_frame
	quit(0 if failures.is_empty() else 1)


func _default_runtime_resource_root() -> String:
	var root := ProjectSettings.globalize_path("user://runtime_scene_probe_resource_root")
	DirAccess.make_dir_recursive_absolute(root)
	_copy_dir_files(ProjectSettings.globalize_path("res://../fixtures/godot/dvxi5"), root)
	_copy_file(
		ProjectSettings.globalize_path("res://../fixtures/env/full_00.env"),
		root.path_join("full_00.env")
	)
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


func _find_painted_world_point(data: NovaTerrainData) -> Dictionary:
	var foliage_map: NovaTerrainFoliageMap = data.get_foliage_map()
	if foliage_map == null:
		return {}

	var defs: Array = data.get_foliage_defs()
	var grid := data.get_sector_grid()
	var origin_x := data.get_origin_x()
	var origin_y := data.get_origin_y()
	var width := foliage_map.get_width()
	var height := foliage_map.get_height()

	for map_y in range(height):
		for map_x in range(width):
			var painted := int(foliage_map.get_index(map_x, map_y))
			if painted <= 0 or not _has_matching_foliage_def(defs, painted):
				continue

			var sector_id := int(foliage_map.get_sector_id_at(map_x, map_y))
			var grid_cell := _find_sector_grid_cell(grid, sector_id)
			if grid_cell == INVALID_CELL:
				continue

			var hm_pos := foliage_map.get_heightmap_position(map_x, map_y)
			var quadrant_x := 512.0 if (sector_id == 3 or sector_id == 4) else 0.0
			var quadrant_z := 512.0 if (sector_id == 2 or sector_id == 4) else 0.0
			var local_x := hm_pos.x - quadrant_x
			var local_z := hm_pos.y - quadrant_z
			if local_x < 0.0 or local_x >= 512.0 or local_z < 0.0 or local_z >= 512.0:
				continue

			var world_x := float((origin_x + grid_cell.x) * 512) + local_x
			var source_z := float((origin_y + grid_cell.y) * 512) + local_z
			# The game gates foliage where the witnessed read (-z into the wrap
			# kernel [orig: Foliage_SampleFarMapMask @ 0x6066d0]) resolves this
			# texel — the RENDER-space z is the negated source-space z. The gate
			# cross-check below is the runtime read at that render point
			# (get_foliage_index_world(x, -render_z) == kernel(source_z)).
			var render_z := -source_z
			if int(data.get_foliage_index_world(world_x, -render_z)) <= 0:
				continue

			var world_y := data.get_height_world_bilinear(Vector3(world_x, 0.0, render_z))
			return {
				"position": Vector3(world_x, world_y, render_z),
				"painted": painted,
				"map": Vector2i(map_x, map_y),
				"sector_id": sector_id,
				"grid_cell": grid_cell,
			}

	return {}


func _has_matching_foliage_def(defs: Array, painted: int) -> bool:
	for value in defs:
		if not (value is NovaTerrainFoliageDef):
			continue
		var def := value as NovaTerrainFoliageDef
		if int(def.match) == painted:
			return true
	return false


func _find_sector_grid_cell(grid: PackedInt32Array, sector_id: int) -> Vector2i:
	if sector_id <= 0:
		return INVALID_CELL
	for gy in range(16):
		for gx in range(16):
			var index := gy * 16 + gx
			if index < grid.size() and int(grid[index]) == sector_id:
				return Vector2i(gx, gy)
	return INVALID_CELL
