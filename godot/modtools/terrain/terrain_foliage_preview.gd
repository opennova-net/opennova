class_name TerrainFoliagePreview
extends Node3D

# Editor wiring for the same fresh native foliage runtime used in play. DETAIL
# samples the live authored map with retail's flat wrapped lookup; MODEL uses
# the routed terrain lookup. Detail also samples the live sculpted height field.
# The silhouette tier stays empty here: retail
# generates it only around crouched/prone infantry (MoveOrder stance bits),
# which an editor preview does not have [orig:
# Terrain_RenderSectorEntitiesBySide @ 0x5c7dc2/0x5c7ded (MoveOrder & 0x300)].

const INVALID_HEIGHT := -1000000.0
const VegAssets := preload("res://engine/terrain/veg_assets.gd")

## Narrow mount snapshot of the editor-only surface inputs injected into the
## native foliage dispatcher. The dispatcher's wider frame telemetry remains
## an implementation detail of this module.
class SurfaceInputDiagnostics:
	extends RefCounted
	var overrides_active: bool = false

var _terrain_mesh: EditorTerrainMesh
var _camera: Camera3D
var _terrain_data: NovaTerrainData
var _resource_root: NovaResourceRoot
var _foliage_map: NovaTerrainFoliageMap
var _foliage_defs: Array[NovaTerrainFoliageDef] = []
var _tile_info: NovaTerrainTileInfo
var _last_raw_foliage_defs: Array = []
var _dispatcher: NovaFoliageDispatcher
var _pending_reset := true
var _configuration_dirty := true
var _surface_override_applied := false
var _last_heightfield_normal: Texture2D
var _last_tile_overlay: Texture2D
var _last_tile_overlay_tint := Vector3(INF, INF, INF)


func _ready() -> void:
	_dispatcher = NovaFoliageDispatcher.new()
	_dispatcher.name = "Dispatcher"
	add_child(_dispatcher)
	_apply_dispatcher_sources()


func _defs_changed(raw: Array) -> bool:
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
	terrain_data: NovaTerrainData = null,
	resource_root: NovaResourceRoot = null,
	tile_info: NovaTerrainTileInfo = null
) -> void:
	var mesh_changed := _terrain_mesh != terrain_mesh
	var data_changed := _terrain_data != terrain_data
	var maps_changed := _foliage_map != foliage_map
	var root_changed := _resource_root != resource_root
	var defs_changed := _defs_changed(foliage_defs) or root_changed
	var tile_info_changed := _tile_info != tile_info

	_terrain_mesh = terrain_mesh
	_camera = camera
	_terrain_data = terrain_data
	_resource_root = resource_root
	_foliage_map = foliage_map
	_tile_info = tile_info

	if defs_changed:
		var typed_defs: Array[NovaTerrainFoliageDef] = []
		for value in foliage_defs:
			if value is NovaTerrainFoliageDef:
				typed_defs.append(value)
		_foliage_defs = typed_defs
		_last_raw_foliage_defs = foliage_defs.duplicate()
		_configuration_dirty = true

	if _dispatcher == null:
		return
	_apply_dispatcher_sources()

	if mesh_changed or data_changed or maps_changed or defs_changed \
			or tile_info_changed:
		_pending_reset = true


func _apply_dispatcher_sources() -> void:
	if _dispatcher == null:
		return
	_dispatcher.colormap_source = _terrain_data
	_dispatcher.height_sampler = Callable(self, "_sample_height")
	_dispatcher.detail_foliage_sampler = Callable(self, "_sample_detail_foliage_index")
	_dispatcher.foliage_sampler = Callable(self, "_sample_foliage_index")
	_dispatcher.tile_info = _tile_info
	_sync_surface_input_overrides()


func _editor_tile_overlay_tint() -> Vector3:
	if _terrain_mesh == null:
		return Vector3.ONE
	var material: ShaderMaterial = _terrain_mesh.get_material()
	if material == null:
		return Vector3.ONE
	var value: Variant = material.get_shader_parameter("u_tile_overlay_tint")
	return value if value is Vector3 else Vector3.ONE


## A standalone editor dispatcher is parented by TerrainFoliagePreview rather
## than NovaTerrain, so it cannot discover runtime-derived surface textures by
## parent cast. Inject EditorTerrainMesh's shared surface inputs explicitly.
func _sync_surface_input_overrides() -> void:
	if _dispatcher == null:
		return
	var have_editor_inputs := _terrain_mesh != null
	if not have_editor_inputs:
		if _surface_override_applied:
			_dispatcher.call("clear_surface_input_overrides")
		_surface_override_applied = false
		_last_heightfield_normal = null
		_last_tile_overlay = null
		_last_tile_overlay_tint = Vector3(INF, INF, INF)
		return

	var heightfield_normal: Texture2D = _terrain_mesh.call("get_heightfield_normal_texture")
	var tile_overlay: Texture2D = _terrain_mesh.call("get_tile_overlay_texture")
	var tile_tint := _editor_tile_overlay_tint()
	if _surface_override_applied \
			and _last_heightfield_normal == heightfield_normal \
			and _last_tile_overlay == tile_overlay \
			and _last_tile_overlay_tint.is_equal_approx(tile_tint):
		return
	_dispatcher.call(
		"set_surface_input_overrides",
		heightfield_normal,
		tile_overlay,
		tile_tint
	)
	_surface_override_applied = true
	_last_heightfield_normal = heightfield_normal
	_last_tile_overlay = tile_overlay
	_last_tile_overlay_tint = tile_tint


func _sample_height(world_x: float, world_z: float) -> float:
	if _terrain_mesh == null:
		return INVALID_HEIGHT
	return _terrain_mesh.sample_world_height(world_x, world_z)


func _sample_detail_foliage_index(world_x: float, world_z: float) -> int:
	if _foliage_map == null:
		return 0
	return int(_foliage_map.sample_detail_index_world(world_x, world_z))


func _sample_foliage_index(world_x: float, world_z: float) -> int:
	if _terrain_mesh == null or _foliage_map == null:
		return 0
	var source := _terrain_mesh.world_to_source_coords(world_x, world_z)
	if source.x < 0.0:
		return 0
	var map_x := _foliage_map.map_x_from_heightmap_x(source.x)
	var map_y := _foliage_map.map_y_from_heightmap_y(source.y)
	if map_x < 0 or map_x >= _foliage_map.get_width():
		return 0
	if map_y < 0 or map_y >= _foliage_map.get_height():
		return 0
	return int(_foliage_map.get_index(map_x, map_y))


func mark_dirty() -> void:
	_pending_reset = true
	# Definition resources are edited in place, so identity comparison cannot
	# detect match/attrib/graphic changes. Refresh the copied native slot state.
	_configuration_dirty = true


func _configure_slots() -> void:
	_dispatcher.configure_slots(
		_foliage_defs,
		VegAssets.resolve_slot_meshes(_resource_root, _foliage_defs),
		VegAssets.resolve_slot_fd_textures(_resource_root, _foliage_defs)
	)


func rebuild_if_needed() -> void:
	if _dispatcher == null or _camera == null:
		return
	if _configuration_dirty:
		_configure_slots()
		_configuration_dirty = false
		_pending_reset = true
	if _pending_reset:
		_dispatcher.reset()
		_pending_reset = false

	# Fade, alpha-pass selection, and wind are camera/frame dependent, so the
	# preview is rendered every frame even while the camera remains in one cell.
	_dispatcher.silhouette_anchors = PackedVector3Array()
	_dispatcher.render_preview(_camera.global_transform)


## Public mount diagnostic for ONED/runtime parity probes.
func get_surface_input_diagnostics() -> SurfaceInputDiagnostics:
	var diagnostics := SurfaceInputDiagnostics.new()
	if _dispatcher == null:
		return diagnostics
	var telemetry: Variant = _dispatcher.call("get_frame_stats")
	if telemetry is Dictionary:
		diagnostics.overrides_active = bool(telemetry.get("surface_input_overrides", false))
	return diagnostics


## Effective Mission blocker resource, exposed for mount diagnostics and tests.
func get_tile_info() -> NovaTerrainTileInfo:
	return _tile_info
