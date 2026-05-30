class_name NovaWorld
extends Node3D

# Loads a playable world (terrain + environment + vegetation + foliage) from ONE
# user-chosen resource directory and wires it onto the engine nodes it contains
# (NovaTerrain, NovaEnvironment, NovaWater). The data core is shared engine code
# (NovaTerrainData, EnvFile, NovaFoliageDispatcher, VegAssets); this node is just
# the orchestration both the runtime (game/main_game.tscn) and a future editor
# "Play" mode go through — one loader, one resource dir, no fallbacks.

const VegAssets := preload("res://engine/terrain/veg_assets.gd")
const ResourceDirSettings := preload("res://engine/resource_index/resource_dir_settings.gd")

signal world_loaded()
signal load_failed(reason: String)

# The terrain + environment loaded, by name, from the resource directory.
@export var terrain_file: String = "Dvxi5.trn"
@export var env_file: String = "full_00.env"

@onready var _terrain: NovaTerrain = $NovaTerrain
@onready var _env: Node = get_node_or_null("NovaEnvironment")
@onready var _water: Node = get_node_or_null("NovaWater")

var _dispatcher: NovaFoliageDispatcher
var _tile_overlay: NovaTerrainTileOverlay
var _terrain_data: NovaTerrainData
var _loaded: bool = false


func _ready() -> void:
	if _terrain != null:
		_dispatcher = _terrain.get_node_or_null("FoliageDispatcher") as NovaFoliageDispatcher
		_tile_overlay = _terrain.get_node_or_null("TileOverlay") as NovaTerrainTileOverlay


## Load the world from `dir`, or from the persisted resource directory when empty.
## Returns OK, or ERR_FILE_NOT_FOUND when the directory is unset/missing the
## terrain (the caller decides whether to prompt). No fallbacks: the chosen
## directory is the only place looked.
func load_world(dir: String = "") -> int:
	if dir.is_empty():
		dir = ResourceDirSettings.get_resource_dir()
	if dir.is_empty():
		load_failed.emit("no resource directory set")
		return ERR_FILE_NOT_FOUND
	var trn := dir.path_join(terrain_file)
	if not FileAccess.file_exists(trn):
		load_failed.emit("%s not found in %s" % [terrain_file, dir])
		return ERR_FILE_NOT_FOUND

	VegAssets.set_search_roots([dir])
	_load_environment(dir)
	if not _load_terrain(trn):
		load_failed.emit("failed to load %s" % trn)
		return ERR_CANT_OPEN

	_loaded = true
	world_loaded.emit()
	return OK


func _load_environment(dir: String) -> void:
	if _env == null:
		return
	var env_path := dir.path_join(env_file)
	if not FileAccess.file_exists(env_path):
		push_warning("NovaWorld: environment '%s' not found" % env_path)
		return
	var env := EnvFile.new()
	env.set_source_path(env_path)
	if env.load() != OK:
		push_warning("NovaWorld: failed to load environment '%s'" % env_path)
		return
	# NovaEnvironment's setter reloads + pushes shader globals on assignment.
	_env.environment_data = env


func _load_terrain(trn_path: String) -> bool:
	var data := NovaTerrainData.new()
	data.set_trn_path(trn_path)
	if data.load() != OK:
		return false
	_terrain_data = data
	_terrain.terrain_data = data
	_terrain.build()
	if _water != null:
		_water.set("terrain_data", data)
	_configure_foliage()
	return true


# Runtime foliage: feed the dispatcher NovaTerrainData directly (C++ fast path —
# no Callable round trip). Same block the editor preview shares minus the
# live-sculpt samplers.
func _configure_foliage() -> void:
	if _dispatcher == null or _terrain_data == null:
		return
	_dispatcher.terrain_data = _terrain_data
	_dispatcher.dispatch_algorithm = NovaFoliageDispatcher.DISPATCH_ALGORITHM_CELL_GRID
	_dispatcher.cell_grid_radius = 8
	var defs: Array = _terrain_data.get_foliage_defs()
	_dispatcher.foliage_defs = defs
	_dispatcher.slot_meshes = VegAssets.resolve_slot_meshes(defs)
	if _tile_overlay != null:
		# NovaTerrain composites the tile overlay into its own material; the scene
		# TileOverlay node is only an authoring override provider here.
		if _terrain.tile_info_override == null and _tile_overlay.tile_info != null:
			_terrain.tile_info_override = _tile_overlay.tile_info
		_tile_overlay.clear()
		_tile_overlay.visible = false


func get_terrain_data() -> NovaTerrainData:
	return _terrain_data


func is_loaded() -> bool:
	return _loaded


## Drive per-frame foliage coverage around the viewer.
func tick(camera_pos: Vector3) -> void:
	if _loaded and _dispatcher != null:
		_dispatcher.dispatch(camera_pos)
