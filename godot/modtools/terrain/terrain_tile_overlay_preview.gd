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

var _overlay_instance: MeshInstance3D
var _ghost_instance: MeshInstance3D
var _hover_instance: MeshInstance3D
var _selection_instance: MeshInstance3D
var _selection_outline_instance: MeshInstance3D
var _outline_instance: MeshInstance3D
var _overlay_material: StandardMaterial3D
var _ghost_material: StandardMaterial3D
var _hover_material: StandardMaterial3D
var _selection_material: StandardMaterial3D
var _selection_outline_material: StandardMaterial3D
var _outline_material: StandardMaterial3D


func _ready() -> void:
	_overlay_material = StandardMaterial3D.new()
	_overlay_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_overlay_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_overlay_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	_overlay_material.no_depth_test = false
	_overlay_material.render_priority = 1

	_selection_material = StandardMaterial3D.new()
	_selection_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_selection_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_selection_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	_selection_material.albedo_color = Color(1.0, 0.55, 0.18, 0.74)
	_selection_material.no_depth_test = false
	_selection_material.render_priority = 4

	_overlay_instance = MeshInstance3D.new()
	_overlay_instance.name = "OverlayMesh"
	_overlay_instance.material_override = _overlay_material
	_overlay_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_overlay_instance)

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

	# FLAG_OUTLINE (bit 0x08) - original game emits a secondary LINELIST pass
	# around the tile perimeter. We approximate as 4 edges on the ground quad.
	_outline_material = StandardMaterial3D.new()
	_outline_material.shading_mode = BaseMaterial3D.SHADING_MODE_UNSHADED
	_outline_material.transparency = BaseMaterial3D.TRANSPARENCY_ALPHA
	_outline_material.cull_mode = BaseMaterial3D.CULL_DISABLED
	_outline_material.albedo_color = Color(1.0, 1.0, 1.0, 0.9)
	_outline_material.no_depth_test = false
	_outline_material.render_priority = 3

	_outline_instance = MeshInstance3D.new()
	_outline_instance.name = "OutlineMesh"
	_outline_instance.material_override = _outline_material
	_outline_instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
	add_child(_outline_instance)


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


func rebuild_if_needed() -> void:
	if not _dirty:
		return
	_rebuild_overlay_mesh()
	_rebuild_ghost_mesh()
	_rebuild_hover_mesh()
	_rebuild_selection_mesh()
	_rebuild_selection_outline_mesh()
	_rebuild_outline_mesh()
	_dirty = false


static func entry_center_world(entry: NovaTerrainTileEntry, terrain_mesh: EditorTerrainMesh) -> Vector3:
	if entry == null or terrain_mesh == null:
		return Vector3.ZERO
	var center_x := float(entry.get_x_fixed()) / 65536.0 + CELL_WORLD_SIZE * 0.5
	var center_z := -float(entry.get_z_fixed()) / 65536.0 + CELL_WORLD_SIZE * 0.5
	var center_y := terrain_mesh.sample_world_height(center_x, center_z)
	if center_y <= -1000000.0:
		center_y = 0.0
	return Vector3(center_x, center_y, center_z)


func _rebuild_overlay_mesh() -> void:
	_overlay_instance.visible = false
	_overlay_instance.mesh = null
	if _terrain_mesh == null or _tileinfo == null or _tilestrip == null:
		return
	if _tileinfo.get_entry_count() <= 0:
		return

	var atlas_width := _tilestrip.get_width()
	var atlas_height := _tilestrip.get_height()
	if atlas_width <= 0 or atlas_height <= 0:
		return

	var tiles_x := atlas_width / int(TILE_PIXELS)
	var tiles_y := atlas_height / int(TILE_PIXELS)
	if tiles_x <= 0 or tiles_y <= 0:
		return

	var verts := PackedVector3Array()
	var uvs := PackedVector2Array()
	var indices := PackedInt32Array()
	verts.resize(_tileinfo.get_entry_count() * 4)
	uvs.resize(_tileinfo.get_entry_count() * 4)
	indices.resize(_tileinfo.get_entry_count() * 6)

	var vertex_offset := 0
	var index_offset := 0
	for i in _tileinfo.get_entry_count():
		var entry := _tileinfo.get_entry(i)
		if entry == null:
			continue
		var quad := _build_entry_quad(entry, SURFACE_OFFSET)
		var quad_uvs := _build_entry_uvs(entry, tiles_x, tiles_y)
		for vertex_index in 4:
			verts[vertex_offset + vertex_index] = quad[vertex_index]
			uvs[vertex_offset + vertex_index] = quad_uvs[vertex_index]
		indices[index_offset + 0] = vertex_offset + 0
		indices[index_offset + 1] = vertex_offset + 1
		indices[index_offset + 2] = vertex_offset + 2
		indices[index_offset + 3] = vertex_offset + 1
		indices[index_offset + 4] = vertex_offset + 3
		indices[index_offset + 5] = vertex_offset + 2
		vertex_offset += 4
		index_offset += 6

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices

	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	_overlay_material.albedo_texture = _tilestrip
	_overlay_material.albedo_color = Color(1.0, 1.0, 1.0, 0.34) if _selected_index >= 0 else Color(1.0, 1.0, 1.0, 1.0)
	_overlay_instance.mesh = mesh
	_overlay_instance.visible = true


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


func _rebuild_outline_mesh() -> void:
	_outline_instance.visible = false
	_outline_instance.mesh = null
	if _terrain_mesh == null or _tileinfo == null:
		return
	if _tileinfo.get_entry_count() <= 0:
		return

	var verts := PackedVector3Array()
	for i in _tileinfo.get_entry_count():
		var entry := _tileinfo.get_entry(i)
		if entry == null:
			continue
		if (entry.get_flags() & NovaTerrainTileInfo.FLAG_OUTLINE) == 0:
			continue
		# Quad is [TL, TR, BL, BR]. Walk the perimeter as 4 line segments.
		var quad := _build_entry_quad(entry, SURFACE_OFFSET + 0.02)
		verts.push_back(quad[0]); verts.push_back(quad[1])  # top edge
		verts.push_back(quad[1]); verts.push_back(quad[3])  # right edge
		verts.push_back(quad[3]); verts.push_back(quad[2])  # bottom edge
		verts.push_back(quad[2]); verts.push_back(quad[0])  # left edge

	if verts.is_empty():
		return

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_LINES, arrays)
	_outline_instance.mesh = mesh
	_outline_instance.visible = true


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
	var tile_index := entry.get_tile_index()
	var tile_col := tile_index % tiles_x
	var tile_row := tile_index / tiles_x
	if tile_row < 0 or tile_row >= tiles_y:
		tile_row = 0
		tile_col = 0

	var tile_step_x := 1.0 / float(tiles_x)
	var tile_step_y := 1.0 / float(tiles_y)
	var tile_origin := Vector2(float(tile_col) * tile_step_x, float(tile_row) * tile_step_y)
	var flags := entry.get_flags()
	var local_uvs := [
		_transform_local_uv(Vector2(0.0, 0.0), flags),
		_transform_local_uv(Vector2(1.0, 0.0), flags),
		_transform_local_uv(Vector2(0.0, 1.0), flags),
		_transform_local_uv(Vector2(1.0, 1.0), flags),
	]

	for i in local_uvs.size():
		local_uvs[i] = tile_origin + Vector2(local_uvs[i].x * tile_step_x, local_uvs[i].y * tile_step_y)
	return local_uvs


func _transform_local_uv(value: Vector2, flags: int) -> Vector2:
	# Original game (jodemo.exe sub_607B30 / sub_604700) applies in this order:
	# FLIP_X -> FLIP_Y -> ROTATE_90. Reversing produces different UVs for
	# combined flags. See memory/reference_ida_tiles.md Audit (2026-04-20).
	var uv := value
	if (flags & NovaTerrainTileInfo.FLAG_FLIP_X) != 0:
		uv.x = 1.0 - uv.x
	if (flags & NovaTerrainTileInfo.FLAG_FLIP_Y) != 0:
		uv.y = 1.0 - uv.y
	if (flags & NovaTerrainTileInfo.FLAG_ROTATE_90) != 0:
		uv = Vector2(uv.y, 1.0 - uv.x)
	return uv
