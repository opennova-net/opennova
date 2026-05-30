extends SceneTree

# Headless validation of the main_game.tscn runtime pipeline. Loads the scene,
# waits for NovaTerrainData, moves the camera onto a painted foliage point, then
# verifies the runtime foliage dispatcher is using broad camera-grid coverage.
#
# Use: `godot --headless --path godot -s res://tests/runtime_scene_probe.gd`

const INVALID_CELL := Vector2i(-9999, -9999)


func _initialize() -> void:
	call_deferred("_run")


func _run() -> void:
	var packed := load("res://game/main_game.tscn") as PackedScene
	if packed == null:
		push_error("runtime_scene_probe: failed to load main_game.tscn")
		quit(1)
		return

	var scene := packed.instantiate()
	root.add_child(scene)

	var terrain: NovaTerrain = null
	var dispatcher: NovaFoliageDispatcher = null
	var overlay: NovaTerrainTileOverlay = null
	var camera: Camera3D = null
	var data: NovaTerrainData = null

	for _i in range(30):
		await process_frame
		terrain = scene.get_node_or_null("NovaTerrain")
		dispatcher = scene.get_node_or_null("NovaTerrain/FoliageDispatcher")
		overlay = scene.get_node_or_null("NovaTerrain/TileOverlay")
		camera = scene.get_node_or_null("Camera3D")
		data = terrain.terrain_data if terrain != null else null
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
		"dispatcher_algorithm": dispatcher.get_dispatch_algorithm() if dispatcher != null else -1,
		"dispatcher_cell_grid_radius": dispatcher.get_cell_grid_radius() if dispatcher != null else -1,
		"dispatcher_total_instances": dispatcher.get_total_instances() if dispatcher != null else -1,
		"dispatcher_cached_cells": dispatcher.get_cached_cells() if dispatcher != null else -1,
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
	elif dispatcher.get_dispatch_algorithm() != NovaFoliageDispatcher.DISPATCH_ALGORITHM_CELL_GRID:
		failures.append(
			"expected runtime dispatcher algorithm CELL_GRID, got %d"
				% dispatcher.get_dispatch_algorithm()
		)
	elif dispatcher.get_cached_cells() <= 0:
		failures.append(
			"expected dispatcher.cached_cells > 0 after camera-grid dispatch, got %d"
				% dispatcher.get_cached_cells()
		)

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
			if not (child is MultiMeshInstance3D):
				continue
			var name_str := child.name as String
			if not name_str.begins_with("FoliageSlot"):
				continue
			var slot_index := int(name_str.substr("FoliageSlot".length()))
			if slot_index < 0 or slot_index >= defs.size():
				continue
			var def: NovaTerrainFoliageDef = defs[slot_index]
			if def == null:
				continue
			var wants_shadow := (int(def.attrib_flags) & NovaTerrainFoliageDef.ATTRIB_SHADOW) != 0
			var expected := GeometryInstance3D.SHADOW_CASTING_SETTING_ON if wants_shadow \
				else GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
			if child.cast_shadow != expected:
				failures.append(
					"slot %d: SHADOW attrib=%s, expected shadow_setting=%d, got %d"
						% [slot_index, wants_shadow, expected, child.cast_shadow]
				)

	# Foliage ground-color tint. The runtime dispatcher (terrain_data set) tints
	# each instance from the colormap. Pre-fix the editor path left foliage white.
	if dispatcher != null and dispatcher.get_total_instances() > 0:
		if not _has_nonwhite_instance(dispatcher):
			failures.append("runtime foliage instances are all white; colormap tint not applied")

	# Editor-path parity: a dispatcher wired like terrain_foliage_preview.gd
	# (colormap_source set, terrain_data UNSET, Callable samplers) must tint from
	# the colormap too. This is the regression gate for the editor-foliage fix.
	if data != null and data.is_loaded() and camera != null and painted_probe.has("position"):
		var editor_dispatcher := _make_editor_style_dispatcher(data)
		root.add_child(editor_dispatcher)
		for _i in range(6):
			await process_frame
		editor_dispatcher.dispatch(camera.global_position)
		for _i in range(4):
			await process_frame
		var editor_instances := editor_dispatcher.get_total_instances()
		var editor_nonwhite := _has_nonwhite_instance(editor_dispatcher)
		print("editor-style foliage: instances=%d nonwhite=%s" % [editor_instances, editor_nonwhite])
		if editor_instances <= 0:
			failures.append("editor-style dispatcher placed no foliage at painted point")
		elif not editor_nonwhite:
			failures.append("editor-style foliage instances are all white (colormap_source tint missing)")
		editor_dispatcher.queue_free()
		await process_frame

	if failures.is_empty():
		print("runtime_scene_probe: OK")
	else:
		for failure in failures:
			push_error("runtime_scene_probe FAIL: " + failure)

	scene.queue_free()
	await process_frame
	quit(0 if failures.is_empty() else 1)


func _has_nonwhite_instance(dispatcher: NovaFoliageDispatcher) -> bool:
	for child in dispatcher.get_children():
		if not (child is MultiMeshInstance3D):
			continue
		var mm: MultiMesh = (child as MultiMeshInstance3D).multimesh
		if mm == null or not mm.use_colors:
			continue
		for i in range(mm.instance_count):
			var c := mm.get_instance_color(i)
			if absf(c.r - 1.0) > 0.02 or absf(c.g - 1.0) > 0.02 or absf(c.b - 1.0) > 0.02:
				return true
	return false


func _make_editor_style_dispatcher(data: NovaTerrainData) -> NovaFoliageDispatcher:
	# Mirror terrain_foliage_preview.gd: colormap_source set for tint, terrain_data
	# left null, placement driven by Callable samplers over the same data.
	var d := NovaFoliageDispatcher.new()
	d.dispatch_algorithm = NovaFoliageDispatcher.DISPATCH_ALGORITHM_CELL_GRID
	d.cell_grid_radius = 8
	d.foliage_defs = data.get_foliage_defs()
	d.colormap_source = data
	d.height_sampler = func(wx: float, wz: float) -> float:
		return data.get_height_world_bilinear(Vector3(wx, 0.0, wz))
	d.foliage_sampler = func(wx: float, wz: float) -> int:
		return int(data.get_foliage_index_world(wx, wz))
	return d


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
			var world_z := float((origin_y + grid_cell.y) * 512) + local_z
			if int(data.get_foliage_index_world(world_x, world_z)) <= 0:
				continue

			var world_y := data.get_height_world_bilinear(Vector3(world_x, 0.0, world_z))
			return {
				"position": Vector3(world_x, world_y, world_z),
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
