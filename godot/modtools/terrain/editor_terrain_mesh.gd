class_name EditorTerrainMesh
extends Node3D

const ATLAS_SIZE := NovaTerrainData.ATLAS_SIZE
const SECTOR_SIZE := float(NovaTerrainData.SECTOR_SIZE)
# Verts per sector edge: one per heightmap texel plus the shared far-edge vert.
const PATCH_VERTS := NovaTerrainData.SECTOR_SIZE + 1

var _sector_instances: Array[MeshInstance3D] = []
var _material: ShaderMaterial
var _sector_mesh: ArrayMesh
var _heightmap_tex: ImageTexture
var _heightmap_image: Image

var _sector_count: int = 1
var _sector_rows: int = 1
var _origin_x: int = 0
var _origin_y: int = 0
var _sector_grid := PackedInt32Array()
var _bounds := AABB()
# Source-of-truth for world->atlas coordinate transforms. The math lives in C++
# (NovaTerrainData / libs/terrain_query/coords.h); this node forwards to it so the
# editor brush paths and the runtime samplers share one implementation.
var _data: NovaTerrainData = null
var _surface_inputs: NovaTerrainSurfaceInputs = NovaTerrainSurfaceInputs.new()


func _ready() -> void:
	_build_resources()
	set_sector_layout(8, 8, PackedInt32Array(), -4, -4)


func _build_resources() -> void:
	var shader := preload("res://shaders/terrain_editor.gdshader")
	_material = ShaderMaterial.new()
	_material.shader = shader
	_material.set_shader_parameter("u_heightmap_size", float(ATLAS_SIZE))

	_heightmap_image = Image.create(ATLAS_SIZE, ATLAS_SIZE, false, Image.FORMAT_RF)
	_heightmap_tex = ImageTexture.create_from_image(_heightmap_image)
	_material.set_shader_parameter("u_heightmap", _heightmap_tex)

	_sector_mesh = _create_sector_mesh()


func _create_sector_mesh() -> ArrayMesh:
	var verts := PackedVector3Array()
	var uvs := PackedVector2Array()
	var indices := PackedInt32Array()

	verts.resize(PATCH_VERTS * PATCH_VERTS)
	uvs.resize(PATCH_VERTS * PATCH_VERTS)

	for vz in PATCH_VERTS:
		for vx in PATCH_VERTS:
			var idx := vz * PATCH_VERTS + vx
			var tx := float(vx) / float(PATCH_VERTS - 1)
			var tz := float(vz) / float(PATCH_VERTS - 1)
			verts[idx] = Vector3(tx * SECTOR_SIZE, 0.0, tz * SECTOR_SIZE)
			uvs[idx] = Vector2(tx, tz)

	var quad_count := (PATCH_VERTS - 1) * (PATCH_VERTS - 1)
	indices.resize(quad_count * 6)
	var ii := 0
	for qz in PATCH_VERTS - 1:
		for qx in PATCH_VERTS - 1:
			var tl := qz * PATCH_VERTS + qx
			var tr := tl + 1
			var bl := tl + PATCH_VERTS
			var br := bl + 1
			indices[ii] = tl
			ii += 1
			indices[ii] = bl
			ii += 1
			indices[ii] = tr
			ii += 1
			indices[ii] = tr
			ii += 1
			indices[ii] = bl
			ii += 1
			indices[ii] = br
			ii += 1

	var arrays := []
	arrays.resize(Mesh.ARRAY_MAX)
	arrays[Mesh.ARRAY_VERTEX] = verts
	arrays[Mesh.ARRAY_TEX_UV] = uvs
	arrays[Mesh.ARRAY_INDEX] = indices

	var mesh := ArrayMesh.new()
	mesh.add_surface_from_arrays(Mesh.PRIMITIVE_TRIANGLES, arrays)
	return mesh


func set_sector_layout(sector_count: int, sector_rows: int, sector_grid: PackedInt32Array, origin_x: int, origin_y: int) -> void:
	_sector_count = clampi(sector_count, 1, 16)
	_sector_rows = clampi(sector_rows, 1, 16)
	_origin_x = origin_x
	_origin_y = origin_y
	_sector_grid = PackedInt32Array()
	_sector_grid.resize(256)
	for idx in 256:
		_sector_grid[idx] = sector_grid[idx] if idx < sector_grid.size() else 0
	_ensure_sector_instances(_sector_count * _sector_rows)
	_update_sector_instances()


func set_terrain_data(data: NovaTerrainData) -> void:
	_data = data
	_surface_inputs.set_terrain_data(data)


func _ensure_sector_instances(total: int) -> void:
	while _sector_instances.size() < total:
		var instance := MeshInstance3D.new()
		instance.mesh = _sector_mesh
		instance.material_override = _material
		instance.cast_shadow = GeometryInstance3D.SHADOW_CASTING_SETTING_OFF
		# Verts live at y=0 but the shader displaces Y from the heightmap (0..256).
		# Give the culler a tall AABB so sectors aren't frustum-culled when the
		# displaced geometry is on-screen but the flat mesh AABB isn't.
		instance.custom_aabb = AABB(
			Vector3(0.0, -32.0, 0.0),
			Vector3(SECTOR_SIZE, 320.0, SECTOR_SIZE)
		)
		add_child(instance)
		_sector_instances.append(instance)

	while _sector_instances.size() > total:
		var instance: MeshInstance3D = _sector_instances.pop_back()
		instance.queue_free()


func _update_sector_instances() -> void:
	var total := _sector_count * _sector_rows
	var min_x := float(_origin_x) * SECTOR_SIZE
	var min_z := float(_origin_y) * SECTOR_SIZE
	_bounds = AABB(Vector3(min_x, -16.0, min_z), Vector3(float(_sector_count) * SECTOR_SIZE, 272.0, float(_sector_rows) * SECTOR_SIZE))

	for idx in total:
		var row := idx / _sector_count
		var col := idx % _sector_count
		var instance: MeshInstance3D = _sector_instances[idx]
		var sector_id := get_sector_cell_value(row, col)
		instance.visible = sector_id > 0
		instance.position = Vector3(
			float(_origin_x + col) * SECTOR_SIZE,
			0.0,
			float(_origin_y + row) * SECTOR_SIZE
		)
		instance.set_instance_shader_parameter("u_instance_sector_id", float(sector_id))


func set_heightmap(image: Image) -> void:
	_heightmap_image = image
	_heightmap_tex.update(image)


func get_heightmap_image() -> Image:
	return _heightmap_image


func get_material() -> ShaderMaterial:
	return _material


## Shared retail-faithful terrain inputs used by both the runtime and ONED.
## The object is stable for this mesh lifetime so foliage preview consumers
## can retain it while transaction-boundary refreshes replace its textures.
func get_surface_inputs() -> NovaTerrainSurfaceInputs:
	return _surface_inputs


## Rebuild every derived input after a terrain document load or Mission context
## change, then bind the effective inputs to the existing editor material.
func rebuild_surface_inputs(
	tile_info: NovaTerrainTileInfo = null,
	tile_overlay_enabled: bool = true
) -> bool:
	if _data == null:
		return false
	var rebuilt := _surface_inputs.rebuild(_data, tile_info, tile_overlay_enabled)
	var applied := apply_surface_inputs()
	return rebuilt and applied


## Rebind cached inputs without allocating replacements. This is useful after
## a shader/material context change and deliberately leaves u_heightmap alone.
func apply_surface_inputs() -> bool:
	if _material == null:
		return false
	return _surface_inputs.apply_to_material(_material)


## Refresh only the normalized blend allocation after a committed blend stroke.
func refresh_surface_inputs_blend() -> bool:
	_surface_inputs.rebuild_blend()
	return apply_surface_inputs()


## Refresh only the live heightfield normal after a committed height stroke.
func refresh_surface_inputs_heightfield() -> bool:
	_surface_inputs.rebuild_heightfield()
	return apply_surface_inputs()


## Refresh coefficient and paired retail mip inputs after authored detail maps
## change. Height/blend editing must not pay this allocation cost.
func refresh_surface_inputs_details() -> bool:
	_surface_inputs.rebuild_detail_textures()
	return apply_surface_inputs()


## Rebuild only the baked tile composite for the effective Terrain/Mission TIL.
## A disabled or empty overlay is a valid refresh and clears the material flag.
func refresh_surface_inputs_tile_overlay(
	tile_info: NovaTerrainTileInfo = null,
	enabled: bool = true
) -> bool:
	_surface_inputs.set_tile_info_override(tile_info)
	_surface_inputs.set_tile_overlay_enabled(enabled)
	_surface_inputs.rebuild_tile_overlay()
	return apply_surface_inputs()


func get_heightfield_normal_texture() -> Texture2D:
	return _surface_inputs.get_heightfield_normal_texture()


func has_heightfield_normal_texture() -> bool:
	return _surface_inputs.has_heightfield_normal()


func get_tile_overlay_texture() -> Texture2D:
	return _surface_inputs.get_tile_overlay_texture()


func has_tile_overlay_texture() -> bool:
	return _surface_inputs.has_tile_overlay()


func get_world_bounds() -> AABB:
	return _bounds


func get_sector_count() -> int:
	return _sector_count


func get_sector_rows() -> int:
	return _sector_rows


func get_sector_cell_value(row: int, col: int) -> int:
	if row < 0 or row >= _sector_rows or col < 0 or col >= _sector_count:
		return 0
	var idx := row * NovaTerrainData.SECTOR_GRID_DIM + col
	return clampi(_sector_grid[idx], 0, NovaTerrainData.SECTOR_ID_MAX)


func world_to_sector_cell(world_x: float, world_z: float) -> Vector2i:
	# Forwards to the shared C++ transform (extent-guarded floor(world/512)-origin
	# cell lookup, no grid-value check). Pre-load (no data) returns the (-1,-1)
	# sentinel, like the world_to_source_coords forwarder below.
	if _data == null:
		return Vector2i(-1, -1)
	return _data.world_to_sector_cell(world_x, world_z)


func world_to_source_coords(world_x: float, world_z: float) -> Vector2:
	# Forwards to the shared C++ kernel (editor-mode: bounds-reject, sector-id
	# clamp, local clamp). Pre-load (no data) returns the (-1,-1) sentinel, which
	# also matched the old all-empty placeholder grid.
	if _data == null:
		return Vector2(-1.0, -1.0)
	return _data.world_to_source_coords(world_x, world_z)


func get_sector_origin_world(row: int, col: int) -> Vector3:
	return Vector3(
		float(_origin_x + col) * SECTOR_SIZE,
		0.0,
		float(_origin_y + row) * SECTOR_SIZE
	)


func sample_world_height(world_x: float, world_z: float) -> float:
	# Scalar and batch now run the same C++ live-surface sampler; -1e6 is this
	# wrapper's legacy off-mesh sentinel (the batch variant keeps NAN).
	if _data == null:
		return -1000000.0
	var height := _data.sample_height_world_live(world_x, world_z)
	return height if not is_nan(height) else -1000000.0


# Batch variant of sample_world_height: one C++ call for the whole point set
# instead of one crossing per point. Scalar and batch run the same
# NovaTerrainData per-point sampler core over the SAME live editable image
# (the document hands one Image to both this mesh and the NovaTerrainData;
# pinned by terrain_height_revision_test). Off-mesh / no-data points are NAN,
# not the scalar path's -1e6 sentinel.
func sample_world_heights(points: PackedVector2Array) -> PackedFloat32Array:
	if _data == null:
		var out := PackedFloat32Array()
		out.resize(points.size())
		out.fill(NAN)
		return out
	return _data.sample_heights_world_live(points)


# Segment raycast against the live terrain surface. Forwards to the engine's
# witnessed raycast (NovaTerrainData.raycast_terrain, the ENG-3 B1 port
# [orig: Terrain_RaycastHeightmapLoRes @ 0x60cb80; Terrain_RaycastHeightmapHiRes_0
# @ 0x60e710]); returns the refined world-space hit or the all-NAN miss.
# Pre-load (no data) misses, like the sampler forwarders above.
func raycast_world(from: Vector3, to: Vector3) -> Vector3:
	if _data == null:
		return Vector3(NAN, NAN, NAN)
	return _data.raycast_terrain(from, to)


func world_to_cell_source_coords(world_x: float, world_z: float, row: int, col: int) -> Vector2:
	# Explicit-cell, unclamped-local variant. Forwards to the C++ kernel.
	if _data == null:
		return Vector2(-1e9, -1e9)
	return _data.world_to_cell_source_coords(world_x, world_z, row, col)


func get_cell_atlas_rect(row: int, col: int) -> Rect2i:
	if _data == null:
		return Rect2i(0, 0, 0, 0)
	return _data.get_cell_atlas_rect(row, col)


func get_cells_overlapping_brush(world_x: float, world_z: float, radius_world: float) -> Array:
	var result: Array = []
	if _sector_count <= 0 or _sector_rows <= 0:
		return result
	var bbox_min_col := int(floor((world_x - radius_world) / SECTOR_SIZE)) - _origin_x
	var bbox_max_col := int(floor((world_x + radius_world) / SECTOR_SIZE)) - _origin_x
	var bbox_min_row := int(floor((world_z - radius_world) / SECTOR_SIZE)) - _origin_y
	var bbox_max_row := int(floor((world_z + radius_world) / SECTOR_SIZE)) - _origin_y
	var row_start := maxi(0, bbox_min_row)
	var row_end := mini(_sector_rows - 1, bbox_max_row)
	var col_start := maxi(0, bbox_min_col)
	var col_end := mini(_sector_count - 1, bbox_max_col)
	for row in range(row_start, row_end + 1):
		for col in range(col_start, col_end + 1):
			if get_sector_cell_value(row, col) > 0:
				result.append(Vector2i(row, col))
	return result
