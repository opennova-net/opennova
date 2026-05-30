extends Node3D

# Runtime wiring for NovaFoliageDispatcher + NovaTerrainTileOverlay. Both are
# the same GDExtension classes the editor preview uses; the samplers resolve
# to shared C++ methods on NovaTerrainData (sub_5C6770 / sub_5C65E0 analogues)
# so runtime and editor go through one set of engine-correct math.

const VegAssets := preload("res://engine/terrain/veg_assets.gd")
const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")

# Bundled veg roots hold only textures, no .3di; the real vegetation models live
# in the user's external asset directory (the same one the editor browses).
const DEFAULT_VEG_ROOTS := ["res://modtools/assets/models/", "res://game/assets/models/"]

@onready var _terrain: NovaTerrain = $NovaTerrain
@onready var _camera: Camera3D = $Camera3D
@onready var _dispatcher: NovaFoliageDispatcher = $NovaTerrain/FoliageDispatcher
@onready var _tile_overlay: NovaTerrainTileOverlay = $NovaTerrain/TileOverlay

var _terrain_data: NovaTerrainData
var _runtime_assets_configured: bool = false
var _veg_roots_ready: bool = false
var _resource_picker: FileDialog


func _ready() -> void:
	if _terrain == null or _dispatcher == null:
		return
	_terrain_data = _terrain.terrain_data
	if _terrain_data == null:
		return

	if not _terrain_data.terrain_changed.is_connected(_on_terrain_data_changed):
		_terrain_data.terrain_changed.connect(_on_terrain_data_changed)
	_ensure_veg_roots()
	_refresh_runtime_assets()


# Vegetation .3di live in the user's external asset directory (the same one the
# editor's resource browser uses, persisted in NovaResourceDirSettings). Resolve
# it so the runtime renders real foliage instead of BoxMesh placeholders; prompt
# once if it has never been set. Headless/CI runs never block on a dialog.
func _ensure_veg_roots() -> void:
	var dir := ResourceDirSettings.get_resource_dir()
	if not dir.is_empty():
		_apply_veg_roots(dir)
		return
	if DisplayServer.get_name() == "headless":
		_veg_roots_ready = true
		return
	_prompt_resource_dir()


func _apply_veg_roots(dir: String) -> void:
	var roots: Array = [dir]
	roots.append_array(DEFAULT_VEG_ROOTS)
	VegAssets.set_search_roots(roots)
	_veg_roots_ready = true


func _prompt_resource_dir() -> void:
	if _resource_picker != null:
		return
	_resource_picker = FileDialog.new()
	_resource_picker.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	_resource_picker.access = FileDialog.ACCESS_FILESYSTEM
	_resource_picker.use_native_dialog = true
	_resource_picker.title = "Select your OpenNova asset directory (vegetation models)"
	_resource_picker.dir_selected.connect(_on_resource_dir_selected)
	_resource_picker.canceled.connect(_on_resource_dir_canceled)
	add_child(_resource_picker)
	_resource_picker.popup_centered_ratio(0.6)


func _on_resource_dir_selected(dir: String) -> void:
	ResourceDirSettings.set_resource_dir(dir)
	VegAssets.clear_cache()
	_apply_veg_roots(dir)
	_runtime_assets_configured = false
	_cleanup_resource_picker()


func _on_resource_dir_canceled() -> void:
	# No directory chosen — fall back to bundled roots (placeholder foliage).
	_veg_roots_ready = true
	_cleanup_resource_picker()


func _cleanup_resource_picker() -> void:
	if _resource_picker != null:
		_resource_picker.queue_free()
		_resource_picker = null


func _on_terrain_data_changed() -> void:
	_runtime_assets_configured = false


func _refresh_runtime_assets() -> void:
	if _runtime_assets_configured:
		return
	# Wait for the veg search roots (the resource-dir prompt may still be open).
	if not _veg_roots_ready:
		return
	if _terrain_data == null or not _terrain_data.is_loaded():
		return

	# Fast path: dispatcher calls terrain_data's C++ samplers directly, skipping
	# the Callable/Variant round trip. Callables remain set for symmetry / as a
	# fallback if terrain_data ever gets cleared at runtime.
	_dispatcher.terrain_data = _terrain_data
	_dispatcher.height_sampler = Callable(self, "_sample_height_xz")
	_dispatcher.foliage_sampler = Callable(self, "_sample_foliage_index")
	_dispatcher.dispatch_algorithm = NovaFoliageDispatcher.DISPATCH_ALGORITHM_CELL_GRID
	_dispatcher.cell_grid_radius = 8

	var defs: Array = _terrain_data.get_foliage_defs()
	_dispatcher.foliage_defs = defs
	_dispatcher.slot_meshes = VegAssets.resolve_slot_meshes(defs)

	if _tile_overlay != null:
		# Runtime tile parity is handled by NovaTerrain's terrain-composited
		# overlay bake. A scene-assigned TileOverlay remains useful as an
		# authoring override provider, but should not draw separate quads.
		if _terrain.tile_info_override == null and _tile_overlay.tile_info != null:
			_terrain.tile_info_override = _tile_overlay.tile_info
		_tile_overlay.clear()
		_tile_overlay.visible = false

	_runtime_assets_configured = true


# Runtime height sampler. NovaTerrainData.get_height_world_bilinear is the
# engine's Terrain_SampleHeightBilinear @ 0x5C6770 analogue.
func _sample_height_xz(world_x: float, world_z: float) -> float:
	if _terrain_data == null:
		return -1000000.0
	return _terrain_data.get_height_world_bilinear(Vector3(world_x, 0.0, world_z))


# Runtime foliagemap sampler. Delegates to the shared C++ implementation so
# the editor preview and runtime go through identical sector/origin math.
func _sample_foliage_index(world_x: float, world_z: float) -> int:
	if _terrain_data == null:
		return 0
	return _terrain_data.get_foliage_index_world(world_x, world_z)


func _process(_delta: float) -> void:
	if _dispatcher == null or _camera == null:
		return
	_refresh_runtime_assets()
	if not _runtime_assets_configured:
		return
	# Runtime currently uses the camera CELL_GRID coverage algorithm. The
	# IDA-matched ENGINE_CENTERS path remains available for future visible-
	# entity dispatch; this scene needs broad camera-local coverage today.
	_dispatcher.dispatch(_camera.global_position)
