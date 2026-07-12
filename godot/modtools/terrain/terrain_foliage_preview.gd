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
var _resource_root: NovaResourceRoot
var _surface_map: NovaTerrainSurfaceMap
var _foliage_map: NovaTerrainFoliageMap
var _foliage_defs: Array[NovaTerrainFoliageDef] = []
# Raw input array reference, retained to do element-wise change detection.
# Comparing against a freshly-built typed array each frame triggered false
# positives that flushed the LRU every frame (see plan Step 1).
var _last_raw_foliage_defs: Array = []
var _selected_index: int = -1

var _dispatcher: NovaFoliageDispatcher
var _pending_flush: bool = true


func _ready() -> void:
	# The preview runs the same witnessed retail coverage as the game (the
	# 42u near-cell pool); what you see while painting is what ships.
	_dispatcher = NovaFoliageDispatcher.new()
	_dispatcher.name = "Dispatcher"
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
	surface_map: NovaTerrainSurfaceMap,
	foliage_map: NovaTerrainFoliageMap,
	foliage_defs: Array,
	selected_index: int,
	terrain_data: NovaTerrainData = null,
	resource_root: NovaResourceRoot = null
) -> void:
	var terrain_changed := _terrain_mesh != terrain_mesh
	var data_changed := _terrain_data != terrain_data
	var root_changed := _resource_root != resource_root
	var surface_changed := _surface_map != surface_map
	var map_changed := _foliage_map != foliage_map
	var defs_changed := _defs_changed_raw(foliage_defs) or root_changed
	var sel_changed := _selected_index != selected_index

	_terrain_mesh = terrain_mesh
	_camera = camera
	_terrain_data = terrain_data
	_resource_root = resource_root
	_surface_map = surface_map
	_foliage_map = foliage_map
	_selected_index = selected_index

	if data_changed and _dispatcher != null:
		# Colormap-only source for the FAR fragment pass's terrain-light T1.
		# Placement stays on the live-sculpt Callable samplers below.
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
			_dispatcher.slot_meshes = VegAssets.resolve_slot_meshes(_resource_root, _foliage_defs)
			# The ":fd" bake both tiers bind [orig: Foliage_LoadDefAssets @ 0x601260].
			_dispatcher.slot_fd_textures = VegAssets.resolve_slot_fd_textures(_resource_root, _foliage_defs)
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
		# FAR consumes the foliagemap remapped through the def match values, at
		# the original (x, -z) call boundary [orig: Foliage_SampleFarMapMask
		# @ 0x6066d0 over the load-remapped map @ 0x605AD0/0x5FF4E0].
		_dispatcher.surface_sampler = Callable(self, "_sample_far_mask")
		_pending_flush = true

	if surface_changed or map_changed or sel_changed:
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


# The FAR def-slot mask: the foliagemap pixel remapped through the def match
# values (pixel == match -> bit(def); pixel 0 never matches) [orig:
# Foliage_SampleFarMapMask @ 0x6066d0; remap sub_605AD0 -> sub_5FF4E0].
# native_z is already -candidate_world_z (the witnessed argument boundary);
# the pixel resolves through the editor's shared world->map chain — the same
# resolution the runtime uses on the atlas-normalized map resource.
func _sample_far_mask(world_x: float, native_z: float) -> int:
	var pixel := _sample_foliage_index(world_x, -native_z)
	if pixel == 0:
		return 0
	var mask := 0
	for d in mini(_foliage_defs.size(), 4):
		var def := _foliage_defs[d]
		if def != null and def.get_match() >= 0 and pixel == def.get_match():
			mask |= 1 << d
	return mask


func mark_dirty() -> void:
	# Paint / brush / def edits invalidate the LRU.
	_pending_flush = true


func rebuild_if_needed() -> void:
	if _dispatcher == null or _camera == null:
		return
	if _pending_flush:
		_dispatcher.reset()
		_pending_flush = false

	# Per-frame dispatch is cheap now: the pool bakes each 16u cell once and a
	# steady frame is pure hits plus the per-cell fade/pass refresh (which
	# tracks the camera continuously, like the retail draw).
	_dispatcher.set_model_anchors(_collect_model_anchors())
	_dispatcher.dispatch(_camera.global_position, _camera.global_transform)


# Near-tier anchors. The witnessed driver is per-SECTOR-ENTITY (the
# .trn/.bms-placed world models) [orig: Terrain_RenderSectorEntitiesBySide
# @ 0x5c7d50]; placed world objects are the host equivalent (host mapping).
# The terrain workspace carries no placed-object index yet, so probe for one
# duck-typed. No entities means no model clusters - retail has no
# camera-carpet model dispatch.
func _collect_model_anchors() -> PackedVector3Array:
	if _terrain_mesh == null:
		return PackedVector3Array()
	if _terrain_mesh.has_method("get_placed_object_positions"):
		return _terrain_mesh.get_placed_object_positions()
	return PackedVector3Array()
