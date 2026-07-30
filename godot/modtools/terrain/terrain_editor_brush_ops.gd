extends "res://modtools/terrain/terrain_editor_section.gd"

# Brush machinery for the Terrain workspace (quality slice W4-6d): the
# primary start/end tool mux, brush/surface-paint/clone strokes, terrain
# raycast, undo/redo snapshot application, the custom-history push
# helpers, and the surface-input refresh family. Moved verbatim from
# terrain_editor.gd; ALL state stays on the editor, reached through `_te`.

const TerrainEditorSlots = preload("res://modtools/terrain/terrain_editor_slots.gd")
const TerrainEditHistory = preload("res://modtools/terrain/terrain_edit_history.gd")


func _push_surface_map_history(before_state: Dictionary, after_state: Dictionary) -> void:
	_te._brush_session.push_custom_snapshot(TerrainEditHistory.Kind.SURFACEMAP, before_state, after_state)


func _push_foliage_map_history(before_state: Dictionary, after_state: Dictionary) -> void:
	_te._brush_session.push_custom_snapshot(TerrainEditHistory.Kind.FOLIAGEMAP, before_state, after_state)


func _ensure_surface_inputs_rebuilt() -> void:
	if _te.terrain_mesh == null or _te._data == null or _te._surface_inputs_data == _te._data:
		return
	_te.terrain_mesh.call("rebuild_surface_inputs", _te.get_effective_tile_info(), true)
	_te._surface_inputs_data = _te._data


func _refresh_surface_input_heightfield() -> void:
	if _te.terrain_mesh != null:
		_te.terrain_mesh.call("refresh_surface_inputs_heightfield")


func _refresh_surface_input_blend() -> void:
	if _te.terrain_mesh != null:
		_te.terrain_mesh.call("refresh_surface_inputs_blend")


func _refresh_surface_input_details() -> void:
	if _te.terrain_mesh != null:
		_te.terrain_mesh.call("refresh_surface_inputs_details")


func _refresh_surface_input_tile_overlay() -> void:
	if _te.terrain_mesh != null:
		_te.terrain_mesh.call(
			"refresh_surface_inputs_tile_overlay",
			_te.get_effective_tile_info(),
			true
		)


func _refresh_surface_inputs_for_texture_slot(slot_id: String) -> void:
	if slot_id == "tilestrip":
		_refresh_surface_input_tile_overlay()
	elif slot_id in TerrainEditorSlots.get_detail_slot_ids() \
			or slot_id in TerrainEditorSlots.get_aux_slot_ids():
		_refresh_surface_input_details()


func _apply_surface_paint_stroke(_delta: float) -> bool:
	var surface_map: NovaTerrainSurfaceMap = _te._document.surface_map
	if surface_map == null or not _te._hover_hit_valid:
		_te._brush_session.reset_stroke_tracking()
		return false

	var map_width := surface_map.get_width()
	var map_height := surface_map.get_height()
	if map_width <= 0 or map_height <= 0:
		_te._brush_session.reset_stroke_tracking()
		return false

	var start_hit: Vector3 = _te._brush_session.get_stroke_start_hit(_te._hover_hit)
	var end_hit: Vector3 = _te._hover_hit
	var dab_count: int = _te._brush_session.get_stroke_dab_count(start_hit, end_hit)
	var radius_pixels := maxi(1, int(round(_te.brush_radius * float(map_width) / float(_te.HM_SIZE))))
	var target_index: int = _te._get_surface_paint_index(Input.is_key_pressed(KEY_CTRL))
	var changed := false

	for dab_idx in range(dab_count):
		var t: float = 1.0 if dab_count == 1 else float(dab_idx + 1) / float(dab_count)
		var dab_hit := start_hit.lerp(end_hit, t)
		var source: Vector2 = _te.terrain_mesh.world_to_source_coords(dab_hit.x, dab_hit.z)
		if source.x < 0.0 or source.y < 0.0:
			continue
		var center_x := surface_map.map_x_from_heightmap_x(source.x)
		var center_y := surface_map.map_y_from_heightmap_y(source.y)
		if center_x < 0 or center_y < 0 or center_x >= map_width or center_y >= map_height:
			continue
		changed = surface_map.paint_circle(
			center_x,
			center_y,
			radius_pixels,
			_te.brush_hardness,
			_te.brush_strength,
			target_index
		) or changed

	if changed:
		_te._document.sync_surface_map_to_data(_te._get_material())
		_te._surface_map_stroke_changed = true

	_te._brush_session.commit_stroke_hit(end_hit)
	return changed


func _eyedrop_surface_at_hover() -> bool:
	var surface_map: NovaTerrainSurfaceMap = _te._document.surface_map
	if surface_map == null or not _te._hover_hit_valid:
		return false
	var map_width := surface_map.get_width()
	var map_height := surface_map.get_height()
	if map_width <= 0 or map_height <= 0:
		return false
	var source: Vector2 = _te.terrain_mesh.world_to_source_coords(_te._hover_hit.x, _te._hover_hit.z)
	if source.x < 0.0 or source.y < 0.0:
		return false
	var map_x := surface_map.map_x_from_heightmap_x(source.x)
	var map_y := surface_map.map_y_from_heightmap_y(source.y)
	_te.selected_surface_index = surface_map.get_index(map_x, map_y)
	_te._mark_ui_state_changed()
	return true


func _set_clone_source(world_pos: Vector3) -> void:
	_te._brush_session.set_clone_source(world_pos, _te._colormap_image)
	if _te._clone_source_marker:
		_te._clone_source_marker.position = world_pos
		_te._clone_source_marker.visible = true
	_te._mark_ui_state_changed()


func clear_clone_source() -> void:
	_te._brush_session.clear_clone_source()
	if _te._clone_source_marker:
		_te._clone_source_marker.visible = false
	_te._mark_ui_state_changed()


func has_clone_source() -> bool:
	return _te._brush_session.has_clone_source()


func _on_primary_start() -> void:
	if _te.is_export_running():
		return
	if Input.is_mouse_button_pressed(MOUSE_BUTTON_RIGHT):
		return
	if Input.is_mouse_button_pressed(MOUSE_BUTTON_MIDDLE):
		return
	if _te.current_tool == _te.Tool.EDIT_SECTORS:
		return
	if _te.current_tool == _te.Tool.TILE_STAMP:
		var hover_index: int = _te._tileinfo_ops._tileinfo_entry_index_at_hover()
		if _te._tileinfo_ops.is_tile_edit_mode():
			if hover_index < 0:
				_te.clear_tileinfo_selection()
				_te.get_viewport().set_input_as_handled()
			elif hover_index != _te._document.get_tileinfo_selected_index():
				_te._tileinfo_ops._select_tileinfo_entry_at_hover()
				_te.get_viewport().set_input_as_handled()
			else:
				_te.get_viewport().set_input_as_handled()
			return
		if hover_index < 0:
			_te._tileinfo_ops._stamp_tileinfo_at_hover()
		else:
			_te._tileinfo_ops._select_tileinfo_entry_at_hover()
		return
	if _te.current_tool == _te.Tool.SURFACE_PAINT:
		if not _te._hover_hit_valid or _te._document.surface_map == null:
			return
		if Input.is_key_pressed(KEY_ALT):
			_eyedrop_surface_at_hover()
			return
		_te._surface_map_stroke_before = _te._document.capture_surface_map_history_state()
		_te._surface_map_stroke_changed = false
		_te.brush_active = true
		_te._brush_session.reset_stroke_tracking()
		_apply_surface_paint_stroke(1.0 / 60.0)
		return
	if _te.current_tool == _te.Tool.FOLIAGE_PAINT:
		if not _te._hover_hit_valid or _te._document.foliage_map == null:
			return
		if Input.is_key_pressed(KEY_ALT):
			_te._foliage_ops._eyedrop_foliage_at_hover()
			return
		if not Input.is_key_pressed(KEY_CTRL):
			var selected_def: NovaTerrainFoliageDef = _te._document.get_selected_foliage_def()
			if selected_def == null or selected_def.get_match() < 0:
				return
		_te._foliage_map_stroke_before = _te._document.capture_foliage_map_history_state()
		_te._foliage_map_stroke_changed = false
		_te.brush_active = true
		_te._brush_session.reset_stroke_tracking()
		_te._foliage_ops._apply_foliage_paint_stroke(1.0 / 60.0)
		return
	if _te.current_tool == _te.Tool.CLONE_COLOR:
		if Input.is_key_pressed(KEY_CTRL) and _te._hover_hit_valid:
			_set_clone_source(_te._hover_hit)
			_te._mark_ui_state_changed()
			return
		if not _te._brush_session.has_clone_source() or not _te._hover_hit_valid:
			return
		if not _te._brush_session.prepare_clone_drag(_te._hover_hit, _te.terrain_mesh):
			return
	# fall through to normal brush-active path
	if _te.current_tool == _te.Tool.PAINT_COLORMAP and Input.is_key_pressed(KEY_ALT) and _te._hover_hit_valid:
		var source: Vector2 = _te.terrain_mesh.world_to_source_coords(_te._hover_hit.x, _te._hover_hit.z)
		if source.x >= 0.0:
			_te.paint_color = _te._data.brush_sample_colormap(source.x, source.y)
			_te._mark_ui_state_changed()
		return
	var stroke_kind: int = _te._brush_session.history_kind_for_tool(_te.current_tool)
	var material: ShaderMaterial = _te._get_material()
	if material != null:
		if stroke_kind == TerrainEditHistory.Kind.BLENDMAP and _te._blendmap_tex != null:
			# Keep blend painting live; commit restores the normalized retail map.
			material.set_shader_parameter("u_blendmap", _te._blendmap_tex)
		elif stroke_kind == TerrainEditHistory.Kind.HEIGHTMAP:
			# Let the shader derive normals from the live heightmap during sculpt.
			material.set_shader_parameter("u_has_heightfield_normal", false)
	_te._brush_session.begin_brush_drag(_source_image_for_kind(_te._brush_session.history_kind_for_tool(_te.current_tool)), Input.is_key_pressed(KEY_CTRL))


func _on_primary_end() -> void:
	if _te.current_tool == _te.Tool.SURFACE_PAINT:
		_te.brush_active = false
		_te._brush_session.reset_stroke_tracking()
		if _te._surface_map_stroke_changed:
			var after_state: Dictionary = _te._document.capture_surface_map_history_state()
			_push_surface_map_history(_te._surface_map_stroke_before, after_state)
			_te.is_dirty = true
			_te._mark_ui_state_changed()
		_te._surface_map_stroke_before = {}
		_te._surface_map_stroke_changed = false
		return
	if _te.current_tool == _te.Tool.FOLIAGE_PAINT:
		_te.brush_active = false
		_te._brush_session.reset_stroke_tracking()
		if _te._foliage_map_stroke_changed:
			var after_state: Dictionary = _te._document.capture_foliage_map_history_state()
			_push_foliage_map_history(_te._foliage_map_stroke_before, after_state)
			_te.is_dirty = true
			_te._mark_foliage_preview_dirty()
			_te._mark_ui_state_changed()
		_te._foliage_map_stroke_before = {}
		_te._foliage_map_stroke_changed = false
		return
	var result: Dictionary = _te._brush_session.end_brush_drag(
		_source_image_for_kind(_te._brush_session.get_stroke_kind())
	)
	if not bool(result.get("history_committed", false)):
		_te.terrain_mesh.apply_surface_inputs()
		return
	match int(result.get("history_kind", -1)):
		TerrainEditHistory.Kind.HEIGHTMAP:
			_refresh_surface_input_heightfield()
			_te._mark_foliage_preview_dirty()
		TerrainEditHistory.Kind.BLENDMAP:
			_refresh_surface_input_blend()


# Public: intersect a viewport-space mouse position with the terrain surface. Returns
# a world-space point, or INVALID_HIT on a miss; pair with is_valid_terrain_hit().
# Used by the Mission workspace to drag/place entities onto the ground. Forwards to
# the engine's witnessed segment raycast ([orig: Terrain_RaycastHeightmapLoRes
# @ 0x60cb80; Terrain_RaycastHeightmapHiRes_0 @ 0x60e710], docs/terrain/terrain-re.md
# §Runtime terrain queries) via EditorTerrainMesh.raycast_world; the engine side
# slab-clips the long probe segment to the authored extent.
func raycast_terrain_at(mouse_pos: Vector2) -> Vector3:
	if _te.camera == null or _te.terrain_mesh == null:
		return _te.INVALID_HIT
	var origin: Vector3 = _te.camera.project_ray_origin(mouse_pos)
	var direction: Vector3 = _te.camera.project_ray_normal(mouse_pos)
	var hit: Vector3 = _te.terrain_mesh.raycast_world(origin, origin + direction * 100000.0)
	if is_nan(hit.x) or is_nan(hit.y) or is_nan(hit.z):
		return _te.INVALID_HIT
	return hit


func is_valid_terrain_hit(hit: Vector3) -> bool:
	return hit != _te.INVALID_HIT


func _raycast_terrain() -> Vector3:
	var mouse_pos: Vector2 = _te._viewport_mouse_position if _te._uses_workspace_viewport else _te.get_viewport().get_mouse_position()
	return raycast_terrain_at(mouse_pos)


func _is_valid_hit(hit: Vector3) -> bool:
	return is_finite(hit.x) and is_finite(hit.y) and is_finite(hit.z)


func _apply_brush_stroke(delta: float) -> void:
	if _te.current_tool == _te.Tool.SURFACE_PAINT:
		if _apply_surface_paint_stroke(delta):
			_te.is_dirty = true
		return
	if _te.current_tool == _te.Tool.FOLIAGE_PAINT:
		if _te._foliage_ops._apply_foliage_paint_stroke(delta):
			_te.is_dirty = true
			_te._mark_foliage_preview_dirty()
		return
	var result: Dictionary = _te._brush_session.apply_brush_stroke(delta, _te._hover_hit, _te._hover_hit_valid, _te.terrain_mesh, _te._data)
	if result["changed_heightmap"]:
		_te.terrain_mesh.set_heightmap(_te._heightmap_image)
		_te._height_revision += 1
		_te._mark_tile_overlay_dirty()
	if result["changed_blendmap"]:
		_te._blendmap_tex.update(_te._blendmap_image)
	if result["changed_colormap"]:
		_te._colormap_tex.update(_te._colormap_image)
	if result["changed_heightmap"] or result["changed_blendmap"] or result["changed_colormap"]:
		_te.is_dirty = true


func undo() -> void:
	if _te.is_export_running() or not _te._brush_session.can_undo():
		return
	var snapshot: Dictionary = _te._brush_session.pop_undo()
	_apply_history_snapshot(snapshot, true)


func redo() -> void:
	if _te.is_export_running() or not _te._brush_session.can_redo():
		return
	var snapshot: Dictionary = _te._brush_session.pop_redo()
	_apply_history_snapshot(snapshot, false)


func can_undo() -> bool:
	return _te._brush_session.can_undo()


func can_redo() -> bool:
	return _te._brush_session.can_redo()


func _apply_history_snapshot(snapshot: Dictionary, is_undo: bool) -> void:
	if int(snapshot.get("kind", -1)) == TerrainEditHistory.Kind.SURFACEMAP:
		var surface_state: Dictionary = snapshot.get("before_value", {}) if is_undo else snapshot.get("after_value", {})
		_te._document.restore_surface_map_history_state(_te._get_material(), surface_state)
		_te.is_dirty = true
		_te._mark_ui_state_changed()
		return
	if int(snapshot.get("kind", -1)) == TerrainEditHistory.Kind.TILEINFO:
		var state: Dictionary = snapshot.get("before_value", {}) if is_undo else snapshot.get("after_value", {})
		_te._document.restore_tileinfo_history_state(state)
		_te.is_dirty = true
		_refresh_surface_input_tile_overlay()
		_te._mark_foliage_preview_dirty()
		_te._mark_tile_overlay_dirty()
		_te._mark_ui_state_changed()
		return
	if int(snapshot.get("kind", -1)) == TerrainEditHistory.Kind.FOLIAGEMAP:
		var state: Dictionary = snapshot.get("before_value", {}) if is_undo else snapshot.get("after_value", {})
		_te._document.restore_foliage_map_history_state(state)
		_te.is_dirty = true
		_te._mark_foliage_preview_dirty()
		_te._mark_ui_state_changed()
		return
	if int(snapshot.get("kind", -1)) == TerrainEditHistory.Kind.FOLIAGE_DEFS:
		var defs_state: Variant = snapshot.get("before_value", {}) if is_undo else snapshot.get("after_value", {})
		if defs_state is Dictionary:
			_te._document.restore_foliage_editor_history_state(defs_state)
		else:
			_te._document.restore_foliage_defs_history_state(defs_state)
		_te.is_dirty = true
		_te._mark_foliage_preview_dirty()
		_te._mark_ui_state_changed()
		return
	var result: Dictionary = _te._brush_session.apply_history_snapshot(snapshot, is_undo, _te._heightmap_image, _te._blendmap_image, _te._colormap_image)
	if result["changed_heightmap"]:
		_te.terrain_mesh.set_heightmap(_te._heightmap_image)
		_te._height_revision += 1
		_refresh_surface_input_heightfield()
		_te._mark_foliage_preview_dirty()
		_te._mark_tile_overlay_dirty()
	if result["changed_blendmap"]:
		_te._blendmap_tex.update(_te._blendmap_image)
		_refresh_surface_input_blend()
	if result["changed_colormap"]:
		_te._colormap_tex.update(_te._colormap_image)
	if result["changed_heightmap"] or result["changed_blendmap"] or result["changed_colormap"]:
		_te.is_dirty = true


func _source_image_for_kind(kind: int) -> Image:
	match kind:
		TerrainEditHistory.Kind.HEIGHTMAP:
			return _te._heightmap_image
		TerrainEditHistory.Kind.BLENDMAP:
			return _te._blendmap_image
		TerrainEditHistory.Kind.COLORMAP:
			return _te._colormap_image
	return null
