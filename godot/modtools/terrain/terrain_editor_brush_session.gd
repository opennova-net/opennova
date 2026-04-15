class_name TerrainEditorBrushSession
extends RefCounted

const TerrainEditorBrushes = preload("res://modtools/terrain/terrain_editor_brushes.gd")
const TerrainEditorSlots = preload("res://modtools/terrain/terrain_editor_slots.gd")
const TerrainEditHistory = preload("res://modtools/terrain/terrain_edit_history.gd")
const CDEPConstraint = preload("res://modtools/terrain/terrain_editor_cdep_constraint.gd")
const HM_SIZE := 1024

var current_tool: int = 0
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

var _paint_texture_image: Image = null
var _paint_texture_filename: String = ""

var _clone_source_set: bool = false
var _clone_source_world: Vector3 = Vector3.ZERO
var _clone_source_image: Image = null
var _clone_offset_px: Vector2i = Vector2i.ZERO


func clear_history() -> void:
	_history.clear()


func load_paint_texture(path: String) -> bool:
	var image := TerrainEditorSlots.load_image_from_file(path)
	if image == null:
		return false
	_paint_texture_image = image
	_paint_texture_filename = path.get_file()
	return true


func clear_paint_texture() -> void:
	_paint_texture_image = null
	_paint_texture_filename = ""


func get_paint_texture() -> Image:
	return _paint_texture_image


func get_paint_texture_filename() -> String:
	return _paint_texture_filename


func has_paint_texture() -> bool:
	return _paint_texture_image != null


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


func apply_brush_stroke(delta: float, hover_hit: Vector3, hover_hit_valid: bool, terrain_mesh, heightmap_image: Image, blendmap_image: Image, colormap_image: Image) -> Dictionary:
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

		if current_tool == 3 and not flatten_target_set:
			var hit_source: Vector2 = terrain_mesh.world_to_source_coords(dab_hit.x, dab_hit.z)
			if hit_source.x >= 0.0:
				flatten_target_height = TerrainEditorBrushes.sample_flatten_target(heightmap_image, hit_source.x, hit_source.y)
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

			var effective_tool := current_tool
			if _stroke_invert:
				if effective_tool == 0:
					effective_tool = 1
				elif effective_tool == 1:
					effective_tool = 0

			# Rect of pixels this dab can touch — used to identify which CDEP
			# blocks need re-checking after the brush runs.
			var height_dab_rect := Rect2i(
				center_x - radius,
				center_z - radius,
				radius * 2 + 1,
				radius * 2 + 1
			).intersection(clip_rect)

			match effective_tool:
				0:
					TerrainEditorBrushes.apply_raise_lower(heightmap_image, center_x, center_z, radius, brush_strength * dab_delta * 20.0, brush_hardness, clip_rect)
					CDEPConstraint.clamp_blocks_in_rect(heightmap_image, height_dab_rect)
					result["changed_heightmap"] = true
				1:
					TerrainEditorBrushes.apply_raise_lower(heightmap_image, center_x, center_z, radius, -brush_strength * dab_delta * 20.0, brush_hardness, clip_rect)
					CDEPConstraint.clamp_blocks_in_rect(heightmap_image, height_dab_rect)
					result["changed_heightmap"] = true
				2:
					TerrainEditorBrushes.apply_smooth(heightmap_image, center_x, center_z, radius, brush_strength * dab_delta * 5.0, brush_hardness, clip_rect)
					CDEPConstraint.clamp_blocks_in_rect(heightmap_image, height_dab_rect)
					result["changed_heightmap"] = true
				3:
					if flatten_target_set:
						TerrainEditorBrushes.apply_flatten(heightmap_image, center_x, center_z, radius, flatten_target_height, brush_strength * dab_delta * 5.0, brush_hardness, clip_rect)
						CDEPConstraint.clamp_blocks_in_rect(heightmap_image, height_dab_rect)
						result["changed_heightmap"] = true
				4:
					TerrainEditorBrushes.apply_blend_paint(blendmap_image, paint_detail_channel, center_x, center_z, radius, brush_strength * dab_delta * 3.0, brush_hardness, clip_rect)
					result["changed_blendmap"] = true
				6:
					TerrainEditorBrushes.apply_colormap_paint(colormap_image, paint_color, center_x, center_z, radius, brush_strength * dab_delta * 3.0, brush_hardness, clip_rect, _paint_texture_image)
					result["changed_colormap"] = true
				7:
					if _clone_source_image != null:
						var src_cx := center_x + _clone_offset_px.x
						var src_cy := center_z + _clone_offset_px.y
						TerrainEditorBrushes.apply_colormap_clone(colormap_image, _clone_source_image, src_cx, src_cy, center_x, center_z, radius, brush_strength * dab_delta * 3.0, brush_hardness, clip_rect)
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
		0, 1, 2, 3:
			return TerrainEditHistory.Kind.HEIGHTMAP
		4:
			return TerrainEditHistory.Kind.BLENDMAP
		6, 7:
			return TerrainEditHistory.Kind.COLORMAP
	return -1
