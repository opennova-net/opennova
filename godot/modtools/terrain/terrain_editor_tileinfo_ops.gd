extends "res://modtools/terrain/terrain_editor_section.gd"

# Tile-stamp authoring for the Terrain workspace (quality slice W4-6d):
# stamp/select/edit/delete of tile entries, tileinfo load/new/reset, the
# tile hover helpers + shared tile-overlay preview sync, and the tileinfo
# undo-history bracket. Moved verbatim from terrain_editor.gd; ALL state
# stays on the editor, reached through `_te`.

const TerrainEditorSlots = preload("res://modtools/terrain/terrain_editor_slots.gd")
const TerrainEditHistory = preload("res://modtools/terrain/terrain_edit_history.gd")
const TerrainTileOverlayPreview = preload("res://modtools/terrain/terrain_tile_overlay_preview.gd")


func is_tile_edit_mode() -> bool:
	return _te.current_tool == _te.Tool.TILE_STAMP and _te._tile_interaction_mode == _te.TileInteractionMode.EDIT_SELECTED and _te.has_selected_tileinfo_entry()


func clear_tileinfo_selection() -> void:
	if _te._document.get_tileinfo_selected_index() < 0 and _te._tile_interaction_mode == _te.TileInteractionMode.PLACE:
		return
	_te._document.clear_tileinfo_selection()
	_te._tile_interaction_mode = _te.TileInteractionMode.PLACE
	_te._mark_tile_overlay_dirty()
	_te._mark_ui_state_changed()


func select_tileinfo_entry(index: int, focus_camera: bool = false) -> void:
	if _te._document.get_tileinfo_selected_index() == index and _te._tile_interaction_mode == _te.TileInteractionMode.EDIT_SELECTED:
		if focus_camera:
			focus_selected_tileinfo_entry()
		return
	_te._document.set_tileinfo_selected_index(index, false)
	_te._tile_interaction_mode = _te.TileInteractionMode.EDIT_SELECTED
	_te._mark_tile_overlay_dirty()
	_te._mark_ui_state_changed()
	if focus_camera:
		focus_selected_tileinfo_entry()


func set_tile_stamp_tile_index(value: int) -> void:
	if _te.is_export_running():
		return
	if _te._document.get_tile_stamp_tile_index() == clampi(value, 0, 255):
		return
	_te._document.set_tile_stamp_tile_index(value)
	_te._mark_ui_state_changed()


func set_tile_stamp_flags(value: int) -> void:
	if _te.is_export_running():
		return
	var normalized := value & (
		NovaTerrainTileInfo.FLAG_FLIP_X |
		NovaTerrainTileInfo.FLAG_FLIP_Y |
		NovaTerrainTileInfo.FLAG_ROTATE_90 |
		NovaTerrainTileInfo.FLAG_OUTLINE
	)
	if _te._document.get_tile_stamp_flags() == normalized:
		return
	_te._document.set_tile_stamp_flags(value)
	_te._mark_ui_state_changed()


## Place the current tile stamp at an explicit authored cell. This is the
## programmatic counterpart to the viewport click path and retains the same
## history, selection, surface-composite, foliage, and UI refresh semantics.
func stamp_tileinfo_cell(cell_x: int, cell_z: int) -> bool:
	if _te.is_export_running() or not _te._document.has_tileinfo_resource():
		return false
	var before_state: Dictionary = _te._document.capture_tileinfo_history_state()
	var result: Dictionary = _te._document.stamp_tileinfo_cell(cell_x, cell_z)
	if int(result.get("index", -1)) >= 0:
		_te._tile_interaction_mode = _te.TileInteractionMode.EDIT_SELECTED
	return _finalize_tileinfo_edit(before_state, bool(result.get("changed", false)))


func delete_selected_tileinfo_entry() -> bool:
	if _te.is_export_running():
		return false
	var before_state: Dictionary = _te._document.capture_tileinfo_history_state()
	var changed := _finalize_tileinfo_edit(before_state, _te._document.delete_selected_tileinfo_entry())
	if changed:
		_te._tile_interaction_mode = _te.TileInteractionMode.PLACE
	return changed


func replace_selected_tileinfo_tile_index(value: int) -> bool:
	if _te.is_export_running():
		return false
	var before_state: Dictionary = _te._document.capture_tileinfo_history_state()
	return _finalize_tileinfo_edit(before_state, _te._document.set_selected_tileinfo_tile_index(value))


func set_selected_tileinfo_flags(value: int) -> bool:
	if _te.is_export_running():
		return false
	var before_state: Dictionary = _te._document.capture_tileinfo_history_state()
	return _finalize_tileinfo_edit(before_state, _te._document.set_selected_tileinfo_flags(value))


func rotate_selected_tileinfo_clockwise() -> bool:
	if _te.is_export_running():
		return false
	var before_state: Dictionary = _te._document.capture_tileinfo_history_state()
	return _finalize_tileinfo_edit(before_state, _te._document.rotate_selected_tileinfo_clockwise())


func flip_selected_tileinfo_x() -> bool:
	if _te.is_export_running():
		return false
	var before_state: Dictionary = _te._document.capture_tileinfo_history_state()
	return _finalize_tileinfo_edit(before_state, _te._document.flip_selected_tileinfo_x())


func flip_selected_tileinfo_y() -> bool:
	if _te.is_export_running():
		return false
	var before_state: Dictionary = _te._document.capture_tileinfo_history_state()
	return _finalize_tileinfo_edit(before_state, _te._document.flip_selected_tileinfo_y())


func focus_selected_tileinfo_entry() -> bool:
	var entry: NovaTerrainTileEntry = _te._document.get_tileinfo_entry(_te._document.get_tileinfo_selected_index())
	if entry == null:
		return false
	var center := TerrainTileOverlayPreview.entry_center_world(entry, _te.terrain_mesh)
	center.y += 4.0
	if _te.camera and _te.camera.has_method("frame_bounds"):
		_te.camera.frame_bounds(center, maxf(float(NovaTerrainTileInfo.CELL_WORLD_SIZE) * 8.0, 96.0))
		return true
	return false


func load_tileinfo(path: String) -> void:
	if _te.is_export_running():
		return
	if _te._document.load_tileinfo(path):
		_normalize_loaded_tileinfo_if_needed()
		_te.is_dirty = true
		_te._brush_ops._refresh_surface_input_tile_overlay()
		_te._mark_foliage_preview_dirty()
		_te._mark_tile_overlay_dirty()
		_te._mark_ui_state_changed()


func new_tileinfo() -> void:
	if _te.is_export_running():
		return
	_te._document.new_tileinfo()
	_te.is_dirty = true
	_te._brush_ops._refresh_surface_input_tile_overlay()
	_te._mark_foliage_preview_dirty()
	_te._mark_tile_overlay_dirty()
	_te._mark_ui_state_changed()


func reset_tileinfo() -> void:
	if _te.is_export_running():
		return
	_te._document.reset_tileinfo()
	_te.is_dirty = _te._document.is_dirty
	_te._brush_ops._refresh_surface_input_tile_overlay()
	_te._mark_foliage_preview_dirty()
	_te._mark_tile_overlay_dirty()
	_te._mark_ui_state_changed()


func _tile_cell_from_world(world_x: float, world_z: float) -> Vector2i:
	return Vector2i(
		int(floor(world_x / float(NovaTerrainTileInfo.CELL_WORLD_SIZE))),
		int(floor(world_z / float(NovaTerrainTileInfo.CELL_WORLD_SIZE)))
	)


func _stamp_tileinfo_at_hover() -> bool:
	if not _te._hover_hit_valid:
		return false
	if not _te._document.has_tileinfo_resource():
		_te._notify_status("Load or create a tile layout before placing tiles.")
		return false

	var cell := _tile_cell_from_world(_te._hover_hit.x, _te._hover_hit.z)
	return stamp_tileinfo_cell(cell.x, cell.y)


func _select_tileinfo_entry_at_hover() -> bool:
	if not _te._hover_hit_valid or not _te._document.has_tileinfo_resource():
		return false

	var cell := _tile_cell_from_world(_te._hover_hit.x, _te._hover_hit.z)
	var index: int = _te._document.find_tileinfo_entry_index_at_cell(cell.x, cell.y)
	if index < 0:
		clear_tileinfo_selection()
		return false

	select_tileinfo_entry(index, false)
	return true


func _finalize_tileinfo_edit(before_state: Dictionary, changed: bool) -> bool:
	if not changed:
		return false
	var after_state: Dictionary = _te._document.capture_tileinfo_history_state()
	_push_tileinfo_history(before_state, after_state)
	_te.is_dirty = _te._document.is_dirty
	_te._brush_ops._refresh_surface_input_tile_overlay()
	_te._mark_foliage_preview_dirty()
	_te._mark_tile_overlay_dirty()
	_te._mark_ui_state_changed()
	return true


func _normalize_loaded_tileinfo_if_needed() -> bool:
	var removed_count: int = _te._document.normalize_tileinfo_for_editor()
	if removed_count <= 0:
		return false
	var plural := "y" if removed_count == 1 else "ies"
	_te._notify_status("Flattened %d stacked tile entr%s. Tile mode now supports one tile per cell." % [removed_count, plural])
	_te._mark_tile_overlay_dirty()
	return true


func _push_tileinfo_history(before_state: Dictionary, after_state: Dictionary) -> void:
	_te._brush_session.push_custom_snapshot(TerrainEditHistory.Kind.TILEINFO, before_state, after_state)


func _tileinfo_entry_index_at_hover() -> int:
	if not _te._hover_hit_valid or not _te._document.has_tileinfo_resource():
		return -1
	var cell := _tile_cell_from_world(_te._hover_hit.x, _te._hover_hit.z)
	return _te._document.find_tileinfo_entry_index_at_cell(cell.x, cell.y)


func _tileinfo_ghost_state() -> Dictionary:
	return {
		"enabled": false,
		"cell": Vector2i.ZERO,
		"tile_index": _te._document.get_tile_stamp_tile_index(),
		"flags": _te._document.get_tile_stamp_flags(),
	}


func _sync_tile_overlay_preview() -> void:
	if _te._tile_overlay_preview == null:
		return
	var tilestrip: Texture2D = null
	if _te._data:
		tilestrip = TerrainEditorSlots.get_slot_texture(_te._data, "tilestrip")
	var has_composited_base: bool = _te.terrain_mesh != null \
		and _te.terrain_mesh.has_tile_overlay_texture()
	_te._tile_overlay_preview.set_base_overlay_visible(not has_composited_base)
	_te._tile_overlay_preview.set_authoring_outlines_visible(not _te._mission_preview_context_active)
	var preview_tile_info: NovaTerrainTileInfo = _te.get_effective_tile_info()
	var ghost_state: Dictionary = {"enabled": false} if _te._mission_preview_context_active \
		else _tileinfo_ghost_state()
	var hover_index := -1
	if not _te._mission_preview_context_active and _te.current_tool == _te.Tool.TILE_STAMP \
			and _te._hover_hit_valid and _te._document.has_tileinfo_resource():
		var ghost_cell := _tile_cell_from_world(_te._hover_hit.x, _te._hover_hit.z)
		hover_index = _te._document.find_tileinfo_entry_index_at_cell(ghost_cell.x, ghost_cell.y)
		if hover_index < 0:
			ghost_state = {
				"enabled": true,
				"cell": ghost_cell,
				"tile_index": _te._document.get_tile_stamp_tile_index(),
				"flags": _te._document.get_tile_stamp_flags(),
			}
	_te._tile_overlay_preview.set_preview_state(
		_te.terrain_mesh,
		preview_tile_info,
		tilestrip,
		-1 if _te._mission_preview_context_active else _te._document.get_tileinfo_selected_index(),
		hover_index,
		bool(ghost_state.get("enabled", false)),
		ghost_state.get("cell", Vector2i.ZERO),
		int(ghost_state.get("tile_index", 0)),
		int(ghost_state.get("flags", 0))
	)
	_te._tile_overlay_preview.rebuild_if_needed()
