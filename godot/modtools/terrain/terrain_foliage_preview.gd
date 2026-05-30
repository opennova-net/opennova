class_name TerrainFoliagePreview
extends Node3D

# Thin editor shell around NovaFoliageDispatcher (GDExtension, C++ hot path).
# All placement / LRU / MultiMesh work happens in the dispatcher child node;
# this script just wires editor-specific inputs (camera position, foliagemap,
# defs, live-sculpt heightmap sampler) and forwards paint/def edits as LRU
# flushes.
#
# Same dispatcher class is used by the runtime scene (main_game.tscn); only
# the samplers differ (runtime reads from NovaTerrainData directly).

const INVALID_HEIGHT := -1000000.0

const VegAssets := preload("res://engine/terrain/veg_assets.gd")

var _terrain_mesh: EditorTerrainMesh
var _camera: Camera3D
var _terrain_data: NovaTerrainData
var _foliage_map: NovaTerrainFoliageMap
var _foliage_defs: Array[NovaTerrainFoliageDef] = []
# Raw input array reference, retained to do element-wise change detection.
# Comparing against a freshly-built typed array each frame triggered false
# positives that flushed the LRU every frame (see plan Step 1).
var _last_raw_foliage_defs: Array = []
var _selected_index: int = -1

var _dispatcher: NovaFoliageDispatcher
var _last_camera_cell_key: String = ""
var _pending_flush: bool = true


func _ready() -> void:
	_dispatcher = NovaFoliageDispatcher.new()
	_dispatcher.name = "Dispatcher"
	_dispatcher.dispatch_algorithm = NovaFoliageDispatcher.DISPATCH_ALGORITHM_CELL_GRID
	_dispatcher.cell_grid_radius = 8
	add_child(_dispatcher)


func _defs_changed_raw(raw: Array) -> bool:
	if raw.size() != _last_raw_foliage_defs.size():
		return true
	for i in range(raw.size()):
		if raw[i] != _last_raw_foliage_defs[i]:
			return true
	return false


func set_preview_state(
	terrain_mesh: EditorTerrainMesh,
	camera: Camera3D,
	foliage_map: NovaTerrainFoliageMap,
	foliage_defs: Array,
	selected_index: int,
	terrain_data: NovaTerrainData = null
) -> void:
	var terrain_changed := _terrain_mesh != terrain_mesh
	var data_changed := _terrain_data != terrain_data
	var map_changed := _foliage_map != foliage_map
	var defs_changed := _defs_changed_raw(foliage_defs)
	var sel_changed := _selected_index != selected_index

	_terrain_mesh = terrain_mesh
	_camera = camera
	_terrain_data = terrain_data
	_foliage_map = foliage_map
	_selected_index = selected_index

	if data_changed and _dispatcher != null:
		# Colormap-only source: the dispatcher tints each foliage instance from the
		# colormap (sub_5C5FE0 analogue) while placement keeps using the live-sculpt
		# Callable samplers below. Without this the editor renders foliage white.
		_dispatcher.colormap_source = _terrain_data
		_pending_flush = true

	if defs_changed:
		var typed_defs: Array[NovaTerrainFoliageDef] = []
		for value in foliage_defs:
			if value is NovaTerrainFoliageDef:
				typed_defs.append(value)
		_foliage_defs = typed_defs
		_last_raw_foliage_defs = foliage_defs.duplicate()  # snapshot refs
		if _dispatcher != null:
			_dispatcher.foliage_defs = _foliage_defs
			_dispatcher.slot_meshes = VegAssets.resolve_slot_meshes(_foliage_defs)
		_pending_flush = true

	if _dispatcher == null:
		return

	if terrain_changed and _terrain_mesh != null:
		# Height sampler → live-sculpt-aware path (sub_5C6770 analogue).
		_dispatcher.height_sampler = Callable(self, "_sample_height")
		# Foliage sampler → world→source_coords→map_pixel→index chain.
		# Consumer applies polytrn_origin + sector_grid shift (editor has both
		# via EditorTerrainMesh).
		_dispatcher.foliage_sampler = Callable(self, "_sample_foliage_index")
		_pending_flush = true

	if map_changed or sel_changed:
		_pending_flush = true


# Height sampler bound to EditorTerrainMesh. Returns world Y in world units,
# or a large negative sentinel when the world point is outside any active sector.
func _sample_height(world_x: float, world_z: float) -> float:
	if _terrain_mesh == null:
		return INVALID_HEIGHT
	return _terrain_mesh.sample_world_height(world_x, world_z)


# Foliagemap sampler: world coords → heightmap-atlas coords → foliage map pixel
# → palette index. Honors Dvxi5-style polytrn_origin via world_to_source_coords.
func _sample_foliage_index(world_x: float, world_z: float) -> int:
	if _terrain_mesh == null or _foliage_map == null:
		return 0
	var source := _terrain_mesh.world_to_source_coords(world_x, world_z)
	if source.x < 0.0:
		return 0
	var w := _foliage_map.get_width()
	var h := _foliage_map.get_height()
	if w <= 0 or h <= 0:
		return 0
	var map_x := _foliage_map.map_x_from_heightmap_x(source.x)
	var map_y := _foliage_map.map_y_from_heightmap_y(source.y)
	if map_x < 0 or map_x >= w or map_y < 0 or map_y >= h:
		return 0
	return int(_foliage_map.get_index(map_x, map_y))


func mark_dirty() -> void:
	# Paint / brush / def edits invalidate the LRU.
	_pending_flush = true


func rebuild_if_needed() -> void:
	if _dispatcher == null or _camera == null:
		return
	if _pending_flush:
		_dispatcher.reset()
		_pending_flush = false
		_last_camera_cell_key = ""  # force dispatch on next frame

	# Only re-dispatch when the camera crosses a 16u cell boundary. Within a
	# cell, the LRU output is unchanged, so calling dispatch() would be pure
	# cache hits + a no-op MultiMesh check.
	var base_x := int(floor(_camera.global_position.x / 16.0)) * 16
	var base_z := int(floor(_camera.global_position.z / 16.0)) * 16
	var key := "%d,%d" % [base_x, base_z]
	if key == _last_camera_cell_key:
		return
	_last_camera_cell_key = key
	_dispatcher.dispatch(_camera.global_position)
