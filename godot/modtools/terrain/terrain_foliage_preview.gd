class_name TerrainFoliagePreview
extends Node3D

const PREVIEW_SECTOR_RADIUS := 1
const SURFACE_OFFSET := 0.05
const INVALID_CELL := Vector2i(-9999, -9999)
const INVALID_HEIGHT := -1000000.0

var _terrain_mesh: EditorTerrainMesh
var _camera: Camera3D
var _foliage_map: NovaTerrainFoliageMap
var _foliage_defs: Array[NovaTerrainFoliageDef] = []
var _selected_index: int = -1
var _dirty: bool = true
var _camera_sector_cell := INVALID_CELL
var _focus_sector_cell := INVALID_CELL
var _mesh_cache: Dictionary = {}
var _fallback_mesh: Mesh
var _preview_total_instances: int = 0
var _preview_counts: Dictionary = {}
var _preview_center_cell := INVALID_CELL


func _ready() -> void:
	var marker := BoxMesh.new()
	marker.size = Vector3(2.0, 6.0, 2.0)
	_fallback_mesh = marker


func set_preview_state(
	terrain_mesh: EditorTerrainMesh,
	camera: Camera3D,
	foliage_map: NovaTerrainFoliageMap,
	foliage_defs: Array,
	selected_index: int,
	focus_sector_cell: Vector2i = INVALID_CELL
) -> void:
	var typed_defs: Array[NovaTerrainFoliageDef] = []
	for value in foliage_defs:
		if value is NovaTerrainFoliageDef:
			typed_defs.append(value)

	var next_camera_cell := INVALID_CELL
	if terrain_mesh != null and camera != null:
		next_camera_cell = terrain_mesh.world_to_sector_cell(camera.global_position.x, camera.global_position.z)

	if (
		_terrain_mesh != terrain_mesh
		or _camera != camera
		or _foliage_map != foliage_map
		or _foliage_defs != typed_defs
		or _selected_index != selected_index
		or _camera_sector_cell != next_camera_cell
		or _focus_sector_cell != focus_sector_cell
	):
		_terrain_mesh = terrain_mesh
		_camera = camera
		_foliage_map = foliage_map
		_foliage_defs = typed_defs
		_selected_index = selected_index
		_camera_sector_cell = next_camera_cell
		_focus_sector_cell = focus_sector_cell
		_dirty = true


func get_preview_summary() -> Dictionary:
	return {
		"total_instances": _preview_total_instances,
		"selected_instances": int(_preview_counts.get(_selected_index, 0)),
		"center_cell": _preview_center_cell,
	}


func mark_dirty() -> void:
	_dirty = true


func rebuild_if_needed() -> void:
	if not _dirty:
		return
	_rebuild()
	_dirty = false


func _rebuild() -> void:
	for child in get_children():
		child.queue_free()

	_preview_total_instances = 0
	_preview_counts.clear()
	_preview_center_cell = _resolve_preview_center_cell()

	if _terrain_mesh == null or _foliage_map == null:
		return
	if _foliage_map.get_width() <= 0 or _foliage_map.get_height() <= 0:
		return
	if _preview_center_cell.x < 0:
		return

	var defs_by_match := _build_defs_by_match()
	if defs_by_match.is_empty():
		return

	var transforms_by_def: Dictionary = {}
	for def_index in range(_foliage_defs.size()):
		transforms_by_def[def_index] = []

	for cell in _iter_preview_sector_cells():
		_append_cell_transforms(cell, defs_by_match, transforms_by_def)

	for def_index in range(_foliage_defs.size()):
		var foliage_def := _foliage_defs[def_index]
		if foliage_def == null:
			continue
		var transforms: Array = transforms_by_def.get(def_index, [])
		_preview_counts[def_index] = transforms.size()
		_preview_total_instances += transforms.size()
		if transforms.is_empty():
			continue

		var mesh := _resolve_mesh_for_graphic(foliage_def.graphic)
		if mesh == null:
			mesh = _fallback_mesh
		var instance := MultiMeshInstance3D.new()
		instance.name = "FoliageDef%d" % def_index
		instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_ON if foliage_def.shadow else GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		instance.multimesh = MultiMesh.new()
		instance.multimesh.transform_format = MultiMesh.TRANSFORM_3D
		instance.multimesh.mesh = mesh
		instance.multimesh.instance_count = transforms.size()
		for transform_index in range(transforms.size()):
			instance.multimesh.set_instance_transform(transform_index, transforms[transform_index])
		add_child(instance)


func _resolve_preview_center_cell() -> Vector2i:
	if _terrain_mesh == null:
		return INVALID_CELL
	if _camera_sector_cell.x >= 0 and _terrain_mesh.get_sector_cell_value(_camera_sector_cell.x, _camera_sector_cell.y) > 0:
		return _camera_sector_cell
	if _focus_sector_cell.x >= 0 and _terrain_mesh.get_sector_cell_value(_focus_sector_cell.x, _focus_sector_cell.y) > 0:
		return _focus_sector_cell
	return INVALID_CELL


func _build_defs_by_match() -> Dictionary:
	var defs_by_match: Dictionary = {}
	for def_index in range(_foliage_defs.size()):
		var foliage_def := _foliage_defs[def_index]
		if foliage_def == null or foliage_def.match <= 0:
			continue
		var match_index := int(foliage_def.match)
		if not defs_by_match.has(match_index):
			defs_by_match[match_index] = []
		var indices: Array = defs_by_match[match_index]
		indices.append(def_index)
		defs_by_match[match_index] = indices
	return defs_by_match


func _iter_preview_sector_cells() -> Array:
	var cells: Array = []
	if _terrain_mesh == null:
		return cells
	if _preview_center_cell.x < 0:
		return cells

	var row_start := maxi(0, _preview_center_cell.x - PREVIEW_SECTOR_RADIUS)
	var row_end := mini(_terrain_mesh.get_sector_rows() - 1, _preview_center_cell.x + PREVIEW_SECTOR_RADIUS)
	var col_start := maxi(0, _preview_center_cell.y - PREVIEW_SECTOR_RADIUS)
	var col_end := mini(_terrain_mesh.get_sector_count() - 1, _preview_center_cell.y + PREVIEW_SECTOR_RADIUS)
	for row in range(row_start, row_end + 1):
		for col in range(col_start, col_end + 1):
			if _terrain_mesh.get_sector_cell_value(row, col) > 0:
				cells.append(Vector2i(row, col))
	return cells


func _append_cell_transforms(cell: Vector2i, defs_by_match: Dictionary, transforms_by_def: Dictionary) -> void:
	if _terrain_mesh == null or _foliage_map == null:
		return
	var atlas_rect := _terrain_mesh.get_cell_atlas_rect(cell.x, cell.y)
	if atlas_rect.size.x <= 0 or atlas_rect.size.y <= 0:
		return
	var map_rect := _map_rect_for_source_rect(atlas_rect)
	if map_rect.size.x <= 0 or map_rect.size.y <= 0:
		return

	var world_origin := _terrain_mesh.get_sector_origin_world(cell.x, cell.y)
	for map_y in range(map_rect.position.y, map_rect.position.y + map_rect.size.y):
		for map_x in range(map_rect.position.x, map_rect.position.x + map_rect.size.x):
			var painted_index := int(_foliage_map.get_index(map_x, map_y))
			if painted_index == 0 or not defs_by_match.has(painted_index):
				continue

			var hm_position := _foliage_map.get_heightmap_position(map_x, map_y)
			if not _source_rect_contains_point(atlas_rect, hm_position):
				continue

			var world_x := world_origin.x + (hm_position.x - float(atlas_rect.position.x))
			var world_z := world_origin.z + (hm_position.y - float(atlas_rect.position.y))
			var world_y := _terrain_mesh.sample_world_height(world_x, world_z)
			if world_y <= INVALID_HEIGHT:
				continue

			var transform := Transform3D(Basis.IDENTITY, Vector3(world_x, world_y + SURFACE_OFFSET, world_z))
			var def_indices: Array = defs_by_match[painted_index]
			for def_index in def_indices:
				var transforms: Array = transforms_by_def.get(int(def_index), [])
				transforms.append(transform)
				transforms_by_def[int(def_index)] = transforms


func _map_rect_for_source_rect(source_rect: Rect2i) -> Rect2i:
	if _foliage_map == null or _foliage_map.get_width() <= 0 or _foliage_map.get_height() <= 0:
		return Rect2i()
	var source_x0 := float(source_rect.position.x)
	var source_y0 := float(source_rect.position.y)
	var source_x1 := float(source_rect.position.x + source_rect.size.x) - 0.001
	var source_y1 := float(source_rect.position.y + source_rect.size.y) - 0.001
	var map_x0 := clampi(_foliage_map.map_x_from_heightmap_x(source_x0), 0, _foliage_map.get_width() - 1)
	var map_y0 := clampi(_foliage_map.map_y_from_heightmap_y(source_y0), 0, _foliage_map.get_height() - 1)
	var map_x1 := clampi(_foliage_map.map_x_from_heightmap_x(source_x1), 0, _foliage_map.get_width() - 1)
	var map_y1 := clampi(_foliage_map.map_y_from_heightmap_y(source_y1), 0, _foliage_map.get_height() - 1)
	return Rect2i(map_x0, map_y0, map_x1 - map_x0 + 1, map_y1 - map_y0 + 1)


func _source_rect_contains_point(source_rect: Rect2i, point: Vector2) -> bool:
	return (
		point.x >= float(source_rect.position.x)
		and point.y >= float(source_rect.position.y)
		and point.x < float(source_rect.position.x + source_rect.size.x)
		and point.y < float(source_rect.position.y + source_rect.size.y)
	)


func _resolve_mesh_for_graphic(graphic: String) -> Mesh:
	var basename: String = graphic.get_file().get_basename().to_lower()
	if basename.is_empty():
		return _fallback_mesh
	if _mesh_cache.has(basename):
		return _mesh_cache[basename] as Mesh
	var mesh: Mesh = VegAssets.load_mesh(graphic)
	if mesh == null:
		mesh = _fallback_mesh
	_mesh_cache[basename] = mesh
	return mesh
