extends "res://modtools/terrain/terrain_editor_section.gd"

# Foliage authoring for the Terrain workspace (quality slice W4-6d):
# foliage-def add/remove/field edits with their history bracket, the
# foliage paint stroke + eyedrop, and foliage preview sync. Moved verbatim
# from terrain_editor.gd; ALL state stays on the editor, reached through
# `_te`.

const TerrainEditHistory = preload("res://modtools/terrain/terrain_edit_history.gd")


func add_foliage_def() -> void:
	if _te.is_export_running():
		return
	if _te._document.foliage_defs.size() >= _te.FOLIAGE_DEFS_LIMIT:
		return
	var before_state: Dictionary = _te._document.capture_foliage_editor_history_state()
	if not _te._document.add_foliage_def():
		return
	var after_state: Dictionary = _te._document.capture_foliage_editor_history_state()
	_push_foliage_defs_history(before_state, after_state)
	_te.is_dirty = true
	_te._mark_foliage_preview_dirty()
	_te._mark_ui_state_changed()


func remove_foliage_def(index: int) -> void:
	if _te.is_export_running():
		return
	if index < 0 or index >= _te._document.foliage_defs.size():
		return
	var before_state: Dictionary = _te._document.capture_foliage_editor_history_state()
	if not _te._document.remove_foliage_def(index):
		return
	var after_state: Dictionary = _te._document.capture_foliage_editor_history_state()
	_push_foliage_defs_history(before_state, after_state)
	_te.is_dirty = true
	_te._mark_foliage_preview_dirty()
	_te._mark_ui_state_changed()


func set_foliage_def_field(index: int, field: String, value: Variant) -> void:
	if _te.is_export_running():
		return
	if index < 0 or index >= _te._document.foliage_defs.size():
		return
	var def: NovaTerrainFoliageDef = _te._document.foliage_defs[index]
	if def == null:
		return
	var before_state: Dictionary = _te._document.capture_foliage_editor_history_state()
	match field:
		"graphic":
			if def.graphic == String(value):
				return
			def.graphic = String(value)
		"color_lower":
			if def.color_lower == int(value):
				return
			def.color_lower = int(value)
		"color_upper":
			if def.color_upper == int(value):
				return
			def.color_upper = int(value)
		"shadow":
			if def.shadow == bool(value):
				return
			def.shadow = bool(value)
		"force_on":
			if def.force_on == bool(value):
				return
			def.force_on = bool(value)
		"attrib_flags":
			if def.attrib_flags == int(value):
				return
			def.attrib_flags = int(value)
		_:
			return
	var after_state: Dictionary = _te._document.capture_foliage_editor_history_state()
	_push_foliage_defs_history(before_state, after_state)
	_te.is_dirty = true
	_te._mark_foliage_preview_dirty()
	_te._mark_ui_state_changed()


func _push_foliage_defs_history(before_state: Variant, after_state: Variant) -> void:
	_te._brush_session.push_custom_snapshot(TerrainEditHistory.Kind.FOLIAGE_DEFS, before_state, after_state)


func _sync_foliage_preview() -> void:
	if _te._foliage_preview == null:
		return
	_te._foliage_preview.set_preview_state(
		_te.terrain_mesh,
		_te.camera,
		_te._document.foliage_map,
		_te._document.foliage_defs,
		_te._data,
		_te.get_resource_root(),
		_te.get_effective_tile_info()
	)
	_te._foliage_preview.rebuild_if_needed()


func _apply_foliage_paint_stroke(delta: float) -> bool:
	if _te._document.foliage_map == null or not _te._hover_hit_valid:
		_te._brush_session.reset_stroke_tracking()
		return false

	var target_index := 0
	if not Input.is_key_pressed(KEY_CTRL):
		target_index = _te._document.get_selected_foliage_paint_index()
		if target_index < 0:
			_te._brush_session.reset_stroke_tracking()
			return false

	var start_hit: Vector3 = _te._brush_session.get_stroke_start_hit(_te._hover_hit)
	var end_hit: Vector3 = _te._hover_hit
	var dab_count: int = _te._brush_session.get_stroke_dab_count(start_hit, end_hit)
	var map_width := maxi(_te._document.foliage_map.get_width(), 1)
	var map_height := maxi(_te._document.foliage_map.get_height(), 1)
	var detail_resolution: int = maxi(
		_te._document.foliage_map.get_detail_sample_resolution(), 1)
	var radius_pixels: int = maxi(1, int(round(
		_te.brush_radius * float(detail_resolution) / float(_te.HM_SIZE))))
	var changed := false

	for dab_idx in range(dab_count):
		var t: float = 1.0 if dab_count == 1 else float(dab_idx + 1) / float(dab_count)
		var dab_hit := start_hit.lerp(end_hit, t)
		var center: Vector2i = _te._document.foliage_map.get_detail_map_position_world(
			dab_hit.x, dab_hit.z)
		var center_x := center.x
		var center_y := center.y
		if center_x < 0 or center_y < 0 or center_x >= map_width or center_y >= map_height:
			continue
		changed = _te._document.foliage_map.paint_detail_circle_wrap(
			center_x,
			center_y,
			radius_pixels,
			_te.brush_hardness,
			_te.brush_strength,
			target_index
		) or changed

	_te._brush_session.commit_stroke_hit(end_hit)
	if changed:
		_te._foliage_map_stroke_changed = true
	return changed


func _eyedrop_foliage_at_hover() -> bool:
	if _te._document.foliage_map == null or not _te._hover_hit_valid:
		return false
	var map_position: Vector2i = _te._document.foliage_map.get_detail_map_position_world(
		_te._hover_hit.x, _te._hover_hit.z)
	if map_position.x < 0 or map_position.y < 0:
		return false
	var match_index: int = int(_te._document.foliage_map.get_index(
		map_position.x, map_position.y))
	var def_index: int = _te._document.find_foliage_def_index_by_match(match_index)
	if def_index < 0:
		return false
	_te._document.set_selected_foliage_def_index(def_index)
	_te._mark_foliage_preview_dirty()
	_te._mark_ui_state_changed()
	return true
