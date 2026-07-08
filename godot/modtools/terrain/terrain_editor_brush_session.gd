class_name TerrainEditorBrushSession
extends RefCounted

const TerrainEditHistory = preload("res://modtools/terrain/terrain_edit_history.gd")
const HM_SIZE := NovaTerrainData.ATLAS_SIZE
const BRUSH_RADIUS_MIN := 1.0
const BRUSH_RADIUS_MAX := 128.0
const BRUSH_STRENGTH_MIN := 0.01
const BRUSH_STRENGTH_MAX := 5.0
const BRUSH_HARDNESS_MIN := 0.0
const BRUSH_HARDNESS_MAX := 1.0

enum Tool { RAISE, LOWER, SMOOTH, FLATTEN, PAINT_DETAIL, EDIT_SECTORS, PAINT_COLORMAP, CLONE_COLOR, TILE_STAMP, FOLIAGE_PAINT, SURFACE_PAINT }

var current_tool: int = Tool.RAISE
var brush_radius: float = 16.0
var brush_strength: float = 0.5
var brush_hardness: float = 0.5
var brush_active: bool = false
var flatten_target_height: float = 0.0
var flatten_target_set: bool = false
var paint_detail_channel: int = 0
var paint_color: Color = Color(0.5, 0.5, 0.5, 1.0)

var _stroke_last_hit: Vector3 = Vector3.ZERO
var _stroke_has_last_hit: bool = false
var _history: TerrainEditHistory = TerrainEditHistory.new()
var _stroke_kind: int = -1
var _stroke_invert: bool = false

var _clone_source_set: bool = false
var _clone_source_world: Vector3 = Vector3.ZERO
var _clone_source_image: Image = null
var _clone_offset_px: Vector2i = Vector2i.ZERO


func clear_history() -> void:
	_history.clear()


func reset_stroke_tracking() -> void:
	_stroke_has_last_hit = false


func get_stroke_kind() -> int:
	return _stroke_kind


func get_stroke_start_hit(fallback: Vector3) -> Vector3:
	return _stroke_last_hit if _stroke_has_last_hit else fallback


func get_stroke_dab_count(start_hit: Vector3, end_hit: Vector3) -> int:
	var spacing := maxf(1.0, brush_radius * 0.25)
	var distance := Vector2(end_hit.x - start_hit.x, end_hit.z - start_hit.z).length()
	return 1 if not _stroke_has_last_hit else maxi(1, int(ceil(distance / spacing)))


func commit_stroke_hit(hit: Vector3) -> void:
	_stroke_last_hit = hit
	_stroke_has_last_hit = true


func set_clone_source(world_pos: Vector3, source_image: Image) -> void:
	_clone_source_world = world_pos
	if _clone_source_image == null:
		_clone_source_image = Image.new()
	_clone_source_image.copy_from(source_image)
	_clone_source_set = true


func clear_clone_source() -> void:
	_clone_source_set = false
	_clone_source_image = null


func has_clone_source() -> bool:
	return _clone_source_set


func prepare_clone_drag(hover_hit: Vector3, terrain_mesh) -> bool:
	if not _clone_source_set:
		return false
	var src_atlas: Vector2 = terrain_mesh.world_to_source_coords(_clone_source_world.x, _clone_source_world.z)
	var dst_atlas: Vector2 = terrain_mesh.world_to_source_coords(hover_hit.x, hover_hit.z)
	if src_atlas.x < 0.0 or dst_atlas.x < 0.0:
		return false
	_clone_offset_px = Vector2i(int(round(src_atlas.x - dst_atlas.x)), int(round(src_atlas.y - dst_atlas.y)))
	return true


func begin_brush_drag(source_image: Image, invert: bool) -> void:
	brush_active = true
	flatten_target_set = false
	_stroke_has_last_hit = false
	_stroke_invert = invert
	_stroke_kind = history_kind_for_tool(current_tool)
	if _stroke_kind >= 0 and source_image != null:
		_history.begin_stroke(_stroke_kind, source_image)
	else:
		_stroke_kind = -1


func end_brush_drag(current_image: Image) -> Dictionary:
	var result := {
		"history_committed": false,
		"history_kind": _stroke_kind,
	}
	brush_active = false
	_stroke_has_last_hit = false
	_stroke_invert = false
	if _stroke_kind >= 0 and _history.has_pending():
		if current_image != null:
			_history.end_stroke(current_image)
			result["history_committed"] = true
		else:
			_history.cancel_stroke()
	_stroke_kind = -1
	return result


# `data` is the NovaTerrainData that owns the editable height/colour/blend buffers;
# every tool runs its C++ kernel through it. The brush is skipped when data is null
# (production always passes the loaded data; only some unit tests may omit it).
func apply_brush_stroke(delta: float, hover_hit: Vector3, hover_hit_valid: bool, terrain_mesh, data = null) -> Dictionary:
	var result := {
		"changed_heightmap": false,
		"changed_blendmap": false,
		"changed_colormap": false,
	}
	if not hover_hit_valid:
		_stroke_has_last_hit = false
		return result

	var start_hit: Vector3 = _stroke_last_hit if _stroke_has_last_hit else hover_hit
	var end_hit := hover_hit
	var spacing := maxf(1.0, brush_radius * 0.25)
	var distance := Vector2(end_hit.x - start_hit.x, end_hit.z - start_hit.z).length()
	var dab_count: int = 1 if not _stroke_has_last_hit else maxi(1, int(ceil(distance / spacing)))
	var radius := int(brush_radius)

	for dab_idx in range(dab_count):
		var t: float = 1.0 if dab_count == 1 else float(dab_idx + 1) / float(dab_count)
		var dab_hit := start_hit.lerp(end_hit, t)
		var cells: Array = terrain_mesh.get_cells_overlapping_brush(dab_hit.x, dab_hit.z, brush_radius)
		if cells.is_empty():
			continue
		var dab_delta := delta / float(dab_count)

		if current_tool == Tool.FLATTEN and not flatten_target_set and data != null:
			var hit_source: Vector2 = terrain_mesh.world_to_source_coords(dab_hit.x, dab_hit.z)
			if hit_source.x >= 0.0:
				flatten_target_height = data.brush_sample_flatten_target(hit_source.x, hit_source.y)
				flatten_target_set = true

		var seen_sectors: Dictionary = {}
		for cell in cells:
			var sector_id: int = terrain_mesh.get_sector_cell_value(cell.x, cell.y)
			if sector_id <= 0 or seen_sectors.has(sector_id):
				continue
			seen_sectors[sector_id] = true
			var src: Vector2 = terrain_mesh.world_to_cell_source_coords(dab_hit.x, dab_hit.z, cell.x, cell.y)
			var clip_rect: Rect2i = terrain_mesh.get_cell_atlas_rect(cell.x, cell.y)
			var center_x := int(round(src.x))
			var center_z := int(round(src.y))

			if _stroke_kind >= 0 and _history.has_pending():
				var dab_rect := Rect2i(
					center_x - radius,
					center_z - radius,
					radius * 2 + 1,
					radius * 2 + 1
				).intersection(clip_rect)
				if dab_rect.size.x > 0 and dab_rect.size.y > 0:
					_history.expand_rect(dab_rect, HM_SIZE)

			var effective_tool := _effective_tool_for_stroke(current_tool, _stroke_invert)

			# Rect of pixels this dab can touch — used to identify which CDEP
			# blocks need re-checking after the brush runs.
			var height_dab_rect := Rect2i(
				center_x - radius,
				center_z - radius,
				radius * 2 + 1,
				radius * 2 + 1
			).intersection(clip_rect)

			match effective_tool:
				Tool.RAISE:
					if data != null:
						data.brush_raise_lower(center_x, center_z, radius, brush_strength * dab_delta * 20.0, brush_hardness, clip_rect)
						data.cdep_clamp_blocks_in_rect(height_dab_rect)
						result["changed_heightmap"] = true
				Tool.LOWER:
					if data != null:
						data.brush_raise_lower(center_x, center_z, radius, -brush_strength * dab_delta * 20.0, brush_hardness, clip_rect)
						data.cdep_clamp_blocks_in_rect(height_dab_rect)
						result["changed_heightmap"] = true
				Tool.SMOOTH:
					if data != null:
						data.brush_smooth(center_x, center_z, radius, brush_strength * dab_delta * 5.0, brush_hardness, clip_rect)
						data.cdep_clamp_blocks_in_rect(height_dab_rect)
						result["changed_heightmap"] = true
				Tool.FLATTEN:
					if data != null and flatten_target_set:
						data.brush_flatten(center_x, center_z, radius, flatten_target_height, brush_strength * dab_delta * 5.0, brush_hardness, clip_rect)
						data.cdep_clamp_blocks_in_rect(height_dab_rect)
						result["changed_heightmap"] = true
				Tool.PAINT_DETAIL:
					if data != null:
						data.brush_blend_paint(paint_detail_channel, center_x, center_z, radius, brush_strength * dab_delta * 3.0, brush_hardness, clip_rect)
						result["changed_blendmap"] = true
				Tool.PAINT_COLORMAP:
					if data != null:
						data.brush_colormap_paint(paint_color, center_x, center_z, radius, brush_strength * dab_delta * 3.0, brush_hardness, clip_rect)
						result["changed_colormap"] = true
				Tool.CLONE_COLOR:
					if data != null and _clone_source_image != null:
						var src_cx := center_x + _clone_offset_px.x
						var src_cy := center_z + _clone_offset_px.y
						data.brush_colormap_clone(_clone_source_image, src_cx, src_cy, center_x, center_z, radius, brush_strength * dab_delta * 3.0, brush_hardness, clip_rect)
						result["changed_colormap"] = true

	_stroke_last_hit = end_hit
	_stroke_has_last_hit = true
	return result


func can_undo() -> bool:
	return _history.can_undo()


func can_redo() -> bool:
	return _history.can_redo()


func pop_undo() -> Dictionary:
	return _history.pop_undo()


func pop_redo() -> Dictionary:
	return _history.pop_redo()


func push_custom_snapshot(kind: int, before_value: Variant, after_value: Variant) -> void:
	_history.push_custom_snapshot(kind, before_value, after_value)


func apply_history_snapshot(snapshot: Dictionary, is_undo: bool, heightmap_image: Image, blendmap_image: Image, colormap_image: Image) -> Dictionary:
	var result := {
		"changed_heightmap": false,
		"changed_blendmap": false,
		"changed_colormap": false,
	}
	if snapshot.is_empty():
		return result
	var rect: Rect2i = snapshot["rect"]
	var patch: Image = snapshot["before"] if is_undo else snapshot["after"]
	var kind: int = snapshot["kind"]
	var src_rect := Rect2i(Vector2i.ZERO, rect.size)
	match kind:
		TerrainEditHistory.Kind.HEIGHTMAP:
			heightmap_image.blit_rect(patch, src_rect, rect.position)
			result["changed_heightmap"] = true
		TerrainEditHistory.Kind.BLENDMAP:
			blendmap_image.blit_rect(patch, src_rect, rect.position)
			result["changed_blendmap"] = true
		TerrainEditHistory.Kind.COLORMAP:
			colormap_image.blit_rect(patch, src_rect, rect.position)
			result["changed_colormap"] = true
	return result


func history_kind_for_tool(tool: int) -> int:
	match tool:
		Tool.RAISE, Tool.LOWER, Tool.SMOOTH, Tool.FLATTEN:
			return TerrainEditHistory.Kind.HEIGHTMAP
		Tool.PAINT_DETAIL:
			return TerrainEditHistory.Kind.BLENDMAP
		Tool.PAINT_COLORMAP, Tool.CLONE_COLOR:
			return TerrainEditHistory.Kind.COLORMAP
	return -1


func _effective_tool_for_stroke(tool: int, invert: bool) -> int:
	if not invert:
		return tool
	if tool == Tool.RAISE:
		return Tool.LOWER
	if tool == Tool.LOWER:
		return Tool.RAISE
	return tool
