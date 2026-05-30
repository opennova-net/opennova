extends Node3D

# Runtime wiring for NovaFoliageDispatcher + NovaTerrainTileOverlay. Both are
# the same GDExtension classes the editor preview uses; the samplers resolve
# to shared C++ methods on NovaTerrainData (sub_5C6770 / sub_5C65E0 analogues)
# so runtime and editor go through one set of engine-correct math.
#
# Assets (terrain + vegetation) are NOT shipped in the .pck — bundling .tga
# imports them to compressed .ctex, which degrades the terrain vs the editor's
# raw load. Instead the runtime loads them from the user's external asset
# directory (the same one the editor's resource browser uses), raw and
# uncompressed. A repo-local bundled copy is used as a dev/CI fallback only.

const VegAssets := preload("res://engine/terrain/veg_assets.gd")
const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")

# Bundled veg roots hold only textures, no .3di; the real vegetation models live
# in the user's external asset directory.
const DEFAULT_VEG_ROOTS := ["res://modtools/assets/models/", "res://game/assets/models/"]

# The terrain this runtime loads, by name, from the chosen resource directory.
const TERRAIN_FILE := "Dvxi5.trn"
# Dev/CI fallback — present in the repo but excluded from the runtime export.
const BUNDLED_TRN := "res://game/assets/terrains/Dvxi5/Dvxi5.trn"

@onready var _terrain: NovaTerrain = $NovaTerrain
@onready var _camera: Camera3D = $Camera3D
@onready var _dispatcher: NovaFoliageDispatcher = $NovaTerrain/FoliageDispatcher
@onready var _tile_overlay: NovaTerrainTileOverlay = $NovaTerrain/TileOverlay
@onready var _water: Node = get_node_or_null("NovaWater")

var _terrain_data: NovaTerrainData
var _runtime_assets_configured: bool = false
var _veg_roots_ready: bool = false
var _resource_picker: FileDialog


func _ready() -> void:
	if _terrain == null or _dispatcher == null:
		return
	_resolve_and_load()


# Resolve the asset directory (persisted in NovaResourceDirSettings, shared with
# the editor) and load the terrain + vegetation from it. Prompt for the directory
# when it is unset and no bundled fallback exists (the shipped game). Headless/CI
# runs never block on a dialog and rely on the repo-local fallback.
func _resolve_and_load() -> void:
	var dir := ResourceDirSettings.get_resource_dir()
	var trn := _resolve_trn_path(dir)
	if not trn.is_empty():
		_apply_veg_roots(dir)
		_load_terrain(trn)
		return
	if DisplayServer.get_name() == "headless":
		push_warning("main_game: no terrain source (resource dir unset, no bundled copy)")
		return
	_prompt_resource_dir()


func _resolve_trn_path(dir: String) -> String:
	if not dir.is_empty():
		var external := dir.path_join(TERRAIN_FILE)
		if FileAccess.file_exists(external):
			return external
	if FileAccess.file_exists(BUNDLED_TRN):
		return BUNDLED_TRN
	return ""


func _apply_veg_roots(dir: String) -> void:
	var roots: Array = []
	if not dir.is_empty():
		roots.append(dir)
	roots.append_array(DEFAULT_VEG_ROOTS)
	VegAssets.set_search_roots(roots)
	_veg_roots_ready = true


# Load the terrain from `trn_path`. For an external (absolute) path the textures
# decode raw/uncompressed; the bundled res:// fallback resolves the same source
# in-editor and the imported .ctex in exports.
func _load_terrain(trn_path: String) -> void:
	var data := NovaTerrainData.new()
	data.set_trn_path(trn_path)
	if data.load() != OK:
		push_error("main_game: failed to load terrain '%s'" % trn_path)
		return
	_terrain_data = data
	if not data.terrain_changed.is_connected(_on_terrain_data_changed):
		data.terrain_changed.connect(_on_terrain_data_changed)
	_terrain.terrain_data = data
	_terrain.build()
	if _water != null:
		_water.set("terrain_data", data)
	_runtime_assets_configured = false
	_refresh_runtime_assets()


func _prompt_resource_dir() -> void:
	if _resource_picker != null:
		return
	_resource_picker = FileDialog.new()
	_resource_picker.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	_resource_picker.access = FileDialog.ACCESS_FILESYSTEM
	_resource_picker.use_native_dialog = true
	_resource_picker.title = "Select your OpenNova asset directory (contains %s)" % TERRAIN_FILE
	_resource_picker.dir_selected.connect(_on_resource_dir_selected)
	_resource_picker.canceled.connect(_on_resource_dir_canceled)
	add_child(_resource_picker)
	_resource_picker.popup_centered_ratio(0.6)


func _on_resource_dir_selected(dir: String) -> void:
	_cleanup_resource_picker()
	var trn := _resolve_trn_path(dir)
	if trn.is_empty():
		push_error("main_game: '%s' does not contain %s" % [dir, TERRAIN_FILE])
		_prompt_resource_dir()
		return
	ResourceDirSettings.set_resource_dir(dir)
	VegAssets.clear_cache()
	_apply_veg_roots(dir)
	_load_terrain(trn)


func _on_resource_dir_canceled() -> void:
	_cleanup_resource_picker()
	# A directory is required; fall back to a bundled copy when one exists (dev),
	# otherwise re-prompt.
	var trn := _resolve_trn_path("")
	if not trn.is_empty():
		_apply_veg_roots("")
		_load_terrain(trn)
	else:
		_prompt_resource_dir()


func _cleanup_resource_picker() -> void:
	if _resource_picker != null:
		_resource_picker.queue_free()
		_resource_picker = null


func _on_terrain_data_changed() -> void:
	_runtime_assets_configured = false


func _refresh_runtime_assets() -> void:
	if _runtime_assets_configured:
		return
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
