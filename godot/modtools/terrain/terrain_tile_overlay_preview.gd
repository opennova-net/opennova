class_name TerrainTileOverlayPreview
extends Node3D

const CELL_WORLD_SIZE := float(NovaTerrainTileInfo.CELL_WORLD_SIZE)
const TILE_PIXELS := float(NovaTerrainTileInfo.ATLAS_TILE_PIXELS)
const SURFACE_OFFSET := 0.08
const GHOST_OFFSET := 0.12
const HOVER_OFFSET := 0.16
const SELECTION_OFFSET := 0.18

var _terrain_mesh: EditorTerrainMesh
var _tileinfo: NovaTerrainTileInfo
var _tilestrip: Texture2D
var _selected_index: int = -1
var _hover_index: int = -1
var _ghost_enabled: bool = false
var _ghost_cell := Vector2i.ZERO
var _ghost_tile_index: int = 0
var _ghost_flags: int = 0
var _dirty: bool = true
var _base_overlay_visible := true
var _authoring_outlines_visible := true

# Base overlay + FLAG_OUTLINE perimeter rendering live inside this C++ node.
# The editor-only authoring layers (ghost/hover/selection/selection-outline)
# stay as GDScript MeshInstance3D children below. Ghost tile UVs also route
# through the same native helper so FLIP_X -> FLIP_Y -> ROTATE_90 ordering only
# exists in one place.
var _tile_overlay: NovaTerrainTileOverlay

var _ghost_instance: MeshInstance3D
var _hover_instance: MeshInstance3D
var _selection_instance: MeshInstance3D
var _selection_outline_instance: MeshInstance3D
var _ghost_material: StandardMaterial3D
var _hover_material: StandardMaterial3D
var _selection_material: StandardMaterial3D
var _selection_outline_material: StandardMaterial3D


func _ready() -> void:
	_tile_overlay = NovaTerrainTileOverlay.new()
	_tile_overlay.name = "TileOverlay"
	_tile_overlay.surface_offset = SURFACE_OFFSET
	# Editor shows the FLAG_OUTLINE perimeter as authoring feedback. The engine's
	# 3D renderer (sub_5C42B0) ignores this bit — runtime leaves it false.
	_tile_overlay.draw_outline_flag = true
	_tile_overlay.visible = _base_overlay_visible or _authoring_outlines_visible
	add_child(_tile_overlay)

	_selection_material = StandardMaterial3D.new()
	_selection_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_selection_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_selection_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	_selection_material.albedo_color = Color(1.0, 0.55, 0.18, 0.74)
	_selection_material.no_depth_test = false
	_selection_material.render_priority = 4

	_ghost_material = StandardMaterial3D.new()
	_ghost_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_ghost_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_ghost_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	_ghost_material.albedo_color = Color(1.0, 1.0, 1.0, 0.62)
	_ghost_material.no_depth_test = false
	_ghost_material.render_priority = 2

	_ghost_instance = MeshInstance3D.new()
	_ghost_instance.name = "GhostMesh"
	_ghost_instance.material_override = _ghost_material
	_ghost_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_ghost_instance)

	_hover_material = StandardMaterial3D.new()
	_hover_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_hover_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_hover_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	_hover_material.albedo_color = Color(0.52, 0.76, 1.0, 0.26)
	_hover_material.no_depth_test = false
	_hover_material.render_priority = 3

	_hover_instance = MeshInstance3D.new()
	_hover_instance.name = "HoverMesh"
	_hover_instance.material_override = _hover_material
	_hover_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_hover_instance)

	_selection_instance = MeshInstance3D.new()
	_selection_instance.name = "SelectionMesh"
	_selection_instance.material_override = _selection_material
	_selection_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_selection_instance)

	_selection_outline_material = StandardMaterial3D.new()
	_selection_outline_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_selection_outline_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_selection_outline_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	_selection_outline_material.albedo_color = Color(1.0, 0.74, 0.3, 1.0)
	_selection_outline_material.no_depth_test = false
	_selection_outline_material.render_priority = 5

	_selection_outline_instance = MeshInstance3D.new()
	_selection_outline_instance.name = "SelectionOutlineMesh"
	_selection_outline_instance.material_override = _selection_outline_material
	_selection_outline_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_selection_outline_instance)


func set_preview_state(
	terrain_mesh: EditorTerrainMesh,
	tileinfo: NovaTerrainTileInfo,
	tilestrip: Texture2D,
	selected_index: int,
	hover_index: int,
	ghost_enabled: bool,
	ghost_cell: Vector2i,
	ghost_tile_index: int,
	ghost_flags: int
) -> void:
	if (
		_terrain_mesh != terrain_mesh
		or _tileinfo != tileinfo
		or _tilestrip != tilestrip
		or _selected_index != selected_index
		or _hover_index != hover_index
		or _ghost_enabled != ghost_enabled
		or _ghost_cell != ghost_cell
		or _ghost_tile_index != ghost_tile_index
		or _ghost_flags != ghost_flags
	):
		_terrain_mesh = terrain_mesh
		_tileinfo = tileinfo
		_tilestrip = tilestrip
		_selected_index = selected_index
		_hover_index = hover_index
		_ghost_enabled = ghost_enabled
		_ghost_cell = ghost_cell
		_ghost_tile_index = ghost_tile_index
		_ghost_flags = ghost_flags
		_dirty = true


func mark_dirty() -> void:
	_dirty = true


## Show the legacy separate base geometry only when terrain material
## composition is unavailable. Authoring ghost/hover/selection children are
## intentionally independent and remain visible in the Terrain workspace.
func set_base_overlay_visible(value: bool) -> void:
	if _base_overlay_visible == value:
		return
	_base_overlay_visible = value
	_dirty = true


func is_base_overlay_visible() -> bool:
	return _base_overlay_visible


## FLAG_OUTLINE is Terrain authoring feedback, independent from the legacy
## textured base geometry. Mission context disables it while material
## composition can still suppress only the duplicate base albedo.
func set_authoring_outlines_visible(value: bool) -> void:
	if _authoring_outlines_visible == value:
		return
	_authoring_outlines_visible = value
	_dirty = true


## User-visible authoring-layer state without exposing owned mesh instances.
func is_outline_visible() -> bool:
	if _tile_overlay == null:
		return false
	var outline_instance := _tile_overlay.get_node_or_null("OutlineMesh") as MeshInstance3D
	return outline_instance != null and outline_instance.visible


func is_selection_visible() -> bool:
	return _selection_instance != null and _selection_instance.visible


func is_ghost_visible() -> bool:
	return _ghost_instance != null and _ghost_instance.visible


func rebuild_if_needed() -> void:
	if not _dirty:
		return
	_rebuild_tile_overlay_node()
	_rebuild_ghost_mesh()
	_rebuild_hover_mesh()
	_rebuild_selection_mesh()
	_rebuild_selection_outline_mesh()
	_dirty = false


# Push current state into the shared C++ overlay node. Base triangle mesh +
# FLAG_OUTLINE perimeter are rendered there; this script only dims the albedo
# when a tile is selected (so the selection highlight reads clearly).
func _rebuild_tile_overlay_node() -> void:
	if _tile_overlay == null:
		return
	_tile_overlay.draw_outline_flag = _authoring_outlines_visible
	_tile_overlay.visible = _base_overlay_visible or _authoring_outlines_visible
	if not _tile_overlay.visible:
		_tile_overlay.clear()
		return
	if _terrain_mesh == null or _tileinfo == null or _tilestrip == null:
		_tile_overlay.clear()
		return
	_tile_overlay.tile_info = _tileinfo
	_tile_overlay.tilestrip = _tilestrip
	_tile_overlay.height_sampler = Callable(_terrain_mesh, "sample_world_height")
	_tile_overlay.rebuild()
	# NovaTerrainTileOverlay owns both native children. Material composition
	# replaces only OverlayMesh; FLAG_OUTLINE remains a separate authoring layer.
	var base_instance := _tile_overlay.get_node_or_null("OverlayMesh") as MeshInstance3D
	if base_instance != null:
		base_instance.visible = _base_overlay_visible and base_instance.mesh != null
	var outline_instance := _tile_overlay.get_node_or_null("OutlineMesh") as MeshInstance3D
	if outline_instance != null:
		outline_instance.visible = _authoring_outlines_visible and outline_instance.mesh != null


static func entry_center_world(entry: NovaTerrainTileEntry, terrain_mesh: EditorTerrainMesh) -> Vector3:
	if entry == null or terrain_mesh == null:
		return Vector3.ZERO
	var center_x := float(entry.get_x_fixed()) / 65536.0 + CELL_WORLD_SIZE * 0.5
	var center_z := -float(entry.get_z_fixed()) / 65536.0 + CELL_WORLD_SIZE * 0.5
	var center_y := terrain_mesh.sample_world_height(center_x, center_z)
	if center_y <= -1000000.0:
		center_y = 0.0
	return Vector3(center_x, center_y, center_z)


func _rebuild_selection_mesh() -> void:
	_selection_instance.visible = false
	_selection_instance.mesh = null
	if _terrain_mesh == null or _tileinfo == null or _selected_index < 0:
		return

	var entry := _tileinfo.get_entry(_selected_index)
	if entry == null:
		return

	var quad := _build_entry_quad(entry, SELECTION_OFFSET)
	var verts := PackedVector3Array()
	for vertex in quad:
		verts.push_back(vertex)
	var indices := PackedInt32Array([0, 1, 2, 1, 3, 2])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_INDEX] = indices

	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	_selection_instance.mesh = mesh
	_selection_instance.visible = true


func _rebuild_selection_outline_mesh() -> void:
	_selection_outline_instance.visible = false
	_selection_outline_instance.mesh = null
	if _terrain_mesh == null or _tileinfo == null or _selected_index < 0:
		return

	var entry := _tileinfo.get_entry(_selected_index)
	if entry == null:
		return

	var quad := _build_entry_quad(entry, SELECTION_OFFSET + 0.03)
	var verts := PackedVector3Array()
	verts.push_back(quad[0]); verts.push_back(quad[1])
	verts.push_back(quad[1]); verts.push_back(quad[3])
	verts.push_back(quad[3]); verts.push_back(quad[2])
	verts.push_back(quad[2]); verts.push_back(quad[0])

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_LINES, arrays)
	_selection_outline_instance.mesh = mesh
	_selection_outline_instance.visible = true


func _rebuild_hover_mesh() -> void:
	_hover_instance.visible = false
	_hover_instance.mesh = null
	if _terrain_mesh == null or _tileinfo == null or _hover_index < 0 or _hover_index == _selected_index:
		return

	var entry := _tileinfo.get_entry(_hover_index)
	if entry == null:
		return

	var quad := _build_entry_quad(entry, HOVER_OFFSET)
	var verts := PackedVector3Array()
	for vertex in quad:
		verts.push_back(vertex)
	var indices := PackedInt32Array([0, 1, 2, 1, 3, 2])
	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_INDEX] = indices

	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	_hover_instance.mesh = mesh
	_hover_instance.visible = true


func _rebuild_ghost_mesh() -> void:
	_ghost_instance.visible = false
	_ghost_instance.mesh = null
	if _terrain_mesh == null or _tilestrip == null or not _ghost_enabled:
		return

	var atlas_width := _tilestrip.get_width()
	var atlas_height := _tilestrip.get_height()
	if atlas_width <= 0 or atlas_height <= 0:
		return

	var tiles_x := atlas_width / int(TILE_PIXELS)
	var tiles_y := atlas_height / int(TILE_PIXELS)
	if tiles_x <= 0 or tiles_y <= 0:
		return

	var ghost_entry := NovaTerrainTileEntry.new()
	ghost_entry.set_cell(_ghost_cell.x, _ghost_cell.y)
	ghost_entry.set_tile_index(_ghost_tile_index)
	ghost_entry.set_flags(_ghost_flags)

	var quad := _build_entry_quad(ghost_entry, GHOST_OFFSET)
	var quad_uvs := _build_entry_uvs(ghost_entry, tiles_x, tiles_y)
	var verts := PackedVector3Array()
	var uvs := PackedVector2Array()
	for vertex in quad:
		verts.push_back(vertex)
	for uv in quad_uvs:
		uvs.push_back(uv)

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = PackedInt32Array([0, 1, 2, 1, 3, 2])

	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	_ghost_material.albedo_texture = _tilestrip
	_ghost_instance.mesh = mesh
	_ghost_instance.visible = true


func _build_entry_quad(entry: NovaTerrainTileEntry, y_offset: float) -> Array:
	var origin_x := float(entry.get_x_fixed()) / 65536.0
	var origin_z := -float(entry.get_z_fixed()) / 65536.0
	var x1 := origin_x + CELL_WORLD_SIZE
	var z1 := origin_z + CELL_WORLD_SIZE

	return [
		Vector3(origin_x, _sample_height(origin_x, origin_z, y_offset), origin_z),
		Vector3(x1, _sample_height(x1, origin_z, y_offset), origin_z),
		Vector3(origin_x, _sample_height(origin_x, z1, y_offset), z1),
		Vector3(x1, _sample_height(x1, z1, y_offset), z1),
	]


func _sample_height(world_x: float, world_z: float, y_offset: float) -> float:
	var height := _terrain_mesh.sample_world_height(world_x, world_z)
	if height <= -1000000.0:
		height = 0.0
	return height + y_offset


func _build_entry_uvs(entry: NovaTerrainTileEntry, tiles_x: int, tiles_y: int) -> Array:
	if _tile_overlay == null:
		return []
	var atlas_width := tiles_x * int(TILE_PIXELS)
	var atlas_height := tiles_y * int(TILE_PIXELS)
	return _tile_overlay.build_entry_uvs(entry, atlas_width, atlas_height)
