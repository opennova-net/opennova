extends Node3D

# Runtime wiring for NovaFoliageDispatcher + NovaTerrainTileOverlay. Both are
# the same GDExtension classes the editor preview uses; the samplers resolve
# to shared C++ methods on NovaTerrainData (sub_5C6770 / sub_5C65E0 analogues)
# so runtime and editor go through one set of engine-correct math.

const VegAssets := preload("res://engine/terrain/veg_assets.gd")

@onready var _terrain: NovaTerrain = $NovaTerrain
@onready var _camera: Camera3D = $Camera3D
@onready var _dispatcher: NovaFoliageDispatcher = $NovaTerrain/FoliageDispatcher
@onready var _tile_overlay: NovaTerrainTileOverlay = $NovaTerrain/TileOverlay

var _terrain_data: NovaTerrainData
var _runtime_assets_configured: bool = false


func _ready() -> void:
	if _terrain == null or _dispatcher == null:
		return
	_terrain_data = _terrain.terrain_data
	if _terrain_data == null:
		return

	if not _terrain_data.terrain_changed.is_connected(_on_terrain_data_changed):
		_terrain_data.terrain_changed.connect(_on_terrain_data_changed)
	_refresh_runtime_assets()


func _on_terrain_data_changed() -> void:
	_runtime_assets_configured = false


func _refresh_runtime_assets() -> void:
	if _runtime_assets_configured:
		return
	if _terrain_data == null or not _terrain_data.is_loaded():
		return

	# Fast path: dispatcher calls terrain_data's C++ samplers directly, skipping
	# the Callable/Variant round trip. Callables remain set for symmetry / as a
	# fallback if terrain_data ever gets cleared at runtime.
	_dispatcher.terrain_data = _terrain_data
	_dispatcher.height_sampler = Callable(self, "_sample_height_xz")
	_dispatcher.foliage_sampler = Callable(self, "_sample_foliage_index")

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
	# Engine increments its global frame counter per frame and re-dispatches
	# every visible entity every frame — the 8-frame stagger gate inside
	# the shared dispatcher is what throttles actual re-bakes. Mirroring that
	# cadence here so `((frame + 2*slot) & 7) == 0` stays aligned.
	#
	# We dispatch around the camera (single-camera port) and deliberately
	# omit the view transform: the 38.0 near-Z reject was designed for
	# per-entity dispatch where `centre` is a distant entity, not the camera
	# itself. Passing a real view_xform here would give view_local.z ≈ 0 and
	# skip every frame. The identity-transform default is the correct no-op
	# until per-entity dispatch (spec §4.5) lands.
	_dispatcher.dispatch(_camera.global_position)
