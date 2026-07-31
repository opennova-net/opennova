class_name StampInspector
extends TerrainInspector

## Tile workflow: load/new/reset a .til layout, pick a brush tile from the atlas,
## set transform flags, and edit the selected placed tile. Atlas icons are cached
## per tilestrip. Code-first; built into the inspector mount by the terrain
## workspace.

const TerrainEditorSlots = preload("res://modtools/terrain/terrain_editor_slots.gd")

var _layout_filename: Label
var _layout_status: Label
var _layout_load: Button
var _layout_new: Button
var _layout_reset: Button
var _hint_label: Label
var _brush_preview: TextureRect
var _brush_summary: Label
var _atlas_list: ItemList
var _atlas_status: Label
var _flags_status: Label
var _flip_x: CheckBox
var _flip_y: CheckBox
var _rotate: CheckBox
var _selection_status: Label
var _selection_preview: TextureRect
var _selection_summary: Label
var _selection_done: Button
var _selection_delete: Button

var _atlas_tile_count: int = 0
var _cached_tilestrip_hash: int = -1
var _tile_icon_cache: Dictionary = {}
var _file_dialog: FileDialogHelper


func set_editor(value: TerrainEditor) -> void:
	_cached_tilestrip_hash = -1
	super.set_editor(value)
	if terrain_editor != null and terrain_editor.current_tool != TerrainEditor.Tool.TILE_STAMP:
		terrain_editor.set_tool(TerrainEditor.Tool.TILE_STAMP)


func build_main(mount: Control) -> void:
	var box := _make_inspector_box(mount)
	_root = box

	_add_section_heading(box, "Tile")
	_layout_status = _add_muted_label(box, "")
	var layout_buttons := HBoxContainer.new()
	layout_buttons.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(layout_buttons)
	_layout_load = _add_button(layout_buttons, "Load .til")
	_layout_new = _add_button(layout_buttons, "New layout")
	_layout_reset = _add_button(layout_buttons, "Reset")
	_layout_filename = _add_muted_label(box, "")
	_hint_label = _add_muted_label(box, "Empty: place. Tile: edit.")

	_add_section_heading(box, "Brush")
	_brush_preview = TextureRect.new()
	_brush_preview.custom_minimum_size = Vector2(44, 44)
	_brush_preview.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	box.add_child(_brush_preview)
	_brush_summary = _add_muted_label(box, "")

	_add_section_heading(box, "Atlas")
	_atlas_status = _add_muted_label(box, "")
	_atlas_list = ItemList.new()
	_atlas_list.name = "AtlasList"
	_atlas_list.custom_minimum_size = Vector2(0, 200)
	_atlas_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_atlas_list.icon_mode = ItemList.ICON_MODE_TOP
	_atlas_list.fixed_icon_size = Vector2i(48, 48)
	_atlas_list.max_columns = 6
	_atlas_list.same_column_width = true
	box.add_child(_atlas_list)

	_add_section_heading(box, "Transform")
	_flags_status = _add_muted_label(box, "")
	var flags_row := HBoxContainer.new()
	flags_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(flags_row)
	_flip_x = _add_check(flags_row, "Flip X")
	_flip_y = _add_check(flags_row, "Flip Y")
	_rotate = _add_check(flags_row, "Rotate 90")

	_add_section_heading(box, "Selection")
	_selection_status = _add_muted_label(box, "")
	_selection_preview = TextureRect.new()
	_selection_preview.custom_minimum_size = Vector2(44, 44)
	_selection_preview.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	box.add_child(_selection_preview)
	_selection_summary = _add_muted_label(box, "")
	var selection_buttons := HBoxContainer.new()
	selection_buttons.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(selection_buttons)
	_selection_done = _add_button(selection_buttons, "Done")
	_selection_delete = _add_button(selection_buttons, "Delete tile")

	_layout_load.pressed.connect(_on_layout_load_pressed)
	_layout_new.pressed.connect(_on_layout_new_pressed)
	_layout_reset.pressed.connect(_on_layout_reset_pressed)
	_atlas_list.item_selected.connect(_on_atlas_selected)
	_flip_x.toggled.connect(_on_flip_x)
	_flip_y.toggled.connect(_on_flip_y)
	_rotate.toggled.connect(_on_rotate)
	_selection_done.pressed.connect(_on_selection_done_pressed)
	_selection_delete.pressed.connect(_on_selection_delete_pressed)

	refresh()


func sync_from_editor() -> void:
	refresh()


func refresh() -> void:
	if not _ui_alive() or terrain_editor == null:
		return
	_syncing = true
	_refresh_layout()
	_refresh_brush()
	_refresh_atlas()
	_refresh_flags()
	_refresh_selection()
	_syncing = false


func _add_button(parent: Control, text: String) -> Button:
	var btn := Button.new()
	btn.text = text
	btn.focus_mode = Control.FOCUS_NONE
	btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(btn)
	return btn


func _add_check(parent: Control, text: String) -> CheckBox:
	var check := CheckBox.new()
	check.text = text
	parent.add_child(check)
	return check


func _refresh_layout() -> void:
	var summary := terrain_editor.get_tileinfo_summary()
	var state := String(summary.get("state", ""))
	var display_name := String(summary.get("display_name", "(none)"))
	var loaded := bool(summary.get("loaded", false))
	var missing := bool(summary.get("missing", false))
	var entry_count := int(summary.get("entry_count", 0))
	_layout_filename.text = display_name
	if state == "new":
		_layout_status.text = "Blank layout ready for tile placement. It will save beside the project or exported terrain."
	elif missing:
		_layout_status.text = "Reference preserved, but the .til file is not currently loaded."
	elif loaded:
		_layout_status.text = "%d tile entr%s loaded." % [entry_count, "y" if entry_count == 1 else "ies"]
	else:
		_layout_status.text = "Load a .til file or create a blank layout before placing tiles."
	_layout_reset.disabled = not loaded and state == "" and String(summary.get("reference", "")).is_empty()


func _refresh_brush() -> void:
	var tilestrip := terrain_editor.get_slot_texture("tilestrip")
	var tile_index := terrain_editor.get_tile_stamp_tile_index()
	var flags := terrain_editor.get_tile_stamp_flags()
	_brush_preview.texture = _build_tile_preview(tilestrip, tile_index)
	_brush_summary.text = _brush_summary_text(tile_index, flags)


func _refresh_atlas() -> void:
	var data: NovaTerrainData = terrain_editor.get_data()
	var tilestrip: Texture2D = TerrainEditorSlots.get_slot_texture(data, "tilestrip") if data else null
	var tiles_x := 0
	var tiles_y := 0
	if tilestrip:
		tiles_x = maxi(1, tilestrip.get_width() / NovaTerrainTileInfo.ATLAS_TILE_PIXELS)
		tiles_y = maxi(1, tilestrip.get_height() / NovaTerrainTileInfo.ATLAS_TILE_PIXELS)
	var tile_count := tiles_x * tiles_y
	var strip_hash: int = int(tilestrip.get_instance_id()) if tilestrip else 0
	var needs_rebuild := strip_hash != _cached_tilestrip_hash or tile_count != _atlas_tile_count
	if needs_rebuild:
		_atlas_list.clear()
		_tile_icon_cache.clear()
		if tilestrip:
			for i in tile_count:
				_atlas_list.add_item("%03d" % i, _build_icon(tilestrip, i, tiles_x), true)
		_cached_tilestrip_hash = strip_hash
		_atlas_tile_count = tile_count
	if tilestrip == null:
		_atlas_status.text = "Load a tile atlas in Properties."
	else:
		var brush_index := terrain_editor.get_tile_stamp_tile_index()
		var active_index := brush_index
		var active_label := "brush"
		var selected := terrain_editor.get_selected_tileinfo_entry()
		if selected != null:
			active_index = selected.get_tile_index()
			active_label = "editing"
		var source_name := terrain_editor.get_slot_filename("tilestrip")
		if source_name.is_empty():
			source_name = "loaded atlas"
		_atlas_status.text = "%d tiles - %s - %s %03d" % [tile_count, source_name, active_label, active_index]
		if _atlas_list.item_count > 0 and active_index >= 0 and active_index < _atlas_list.item_count:
			if not _atlas_list.is_selected(active_index):
				_atlas_list.select(active_index)


func _refresh_flags() -> void:
	var selected := terrain_editor.get_selected_tileinfo_entry()
	var flags := selected.get_flags() if selected != null else terrain_editor.get_tile_stamp_flags()
	_flip_x.set_pressed_no_signal((flags & NovaTerrainTileInfo.FLAG_FLIP_X) != 0)
	_flip_y.set_pressed_no_signal((flags & NovaTerrainTileInfo.FLAG_FLIP_Y) != 0)
	_rotate.set_pressed_no_signal((flags & NovaTerrainTileInfo.FLAG_ROTATE_90) != 0)
	if selected != null:
		_flags_status.text = "Editing transform. Brush stays unchanged."
	else:
		_flags_status.text = "Brush transform."


func _refresh_selection() -> void:
	var selected := terrain_editor.get_selected_tileinfo_entry()
	if selected == null:
		_selection_preview.texture = null
		_hint_label.text = "Empty: place. Tile: edit."
		_selection_status.text = "Placement mode"
		_selection_summary.text = "No tile selected."
		_selection_done.disabled = true
		_selection_delete.disabled = true
		return
	_hint_label.text = "Empty/Esc: back to placement."
	_selection_preview.texture = _build_tile_preview(terrain_editor.get_slot_texture("tilestrip"), selected.get_tile_index())
	_selection_status.text = "Editing tile"
	_selection_summary.text = _selection_summary_text(selected)
	_selection_done.disabled = false
	_selection_delete.disabled = false


func _build_tile_preview(tilestrip: Texture2D, tile_index: int) -> Texture2D:
	if tilestrip == null:
		return null
	var tiles_x := maxi(1, tilestrip.get_width() / NovaTerrainTileInfo.ATLAS_TILE_PIXELS)
	var tiles_y := maxi(1, tilestrip.get_height() / NovaTerrainTileInfo.ATLAS_TILE_PIXELS)
	var tile_count := tiles_x * tiles_y
	if tile_count <= 0:
		return null
	return _build_icon(tilestrip, clampi(tile_index, 0, tile_count - 1), tiles_x)


func _build_icon(tilestrip: Texture2D, tile_index: int, tiles_x: int) -> Texture2D:
	var cache_key := "%d:%d:%d" % [tilestrip.get_instance_id(), tile_index, tiles_x]
	if _tile_icon_cache.has(cache_key):
		return _tile_icon_cache[cache_key]
	var atlas := AtlasTexture.new()
	atlas.atlas = tilestrip
	atlas.region = Rect2(
		(tile_index % tiles_x) * NovaTerrainTileInfo.ATLAS_TILE_PIXELS,
		(tile_index / tiles_x) * NovaTerrainTileInfo.ATLAS_TILE_PIXELS,
		NovaTerrainTileInfo.ATLAS_TILE_PIXELS,
		NovaTerrainTileInfo.ATLAS_TILE_PIXELS
	)
	_tile_icon_cache[cache_key] = atlas
	return atlas


func _brush_summary_text(tile_index: int, flags: int) -> String:
	if terrain_editor.get_slot_texture("tilestrip") == null:
		return "Load a tile atlas in Properties."
	var summary := "Brush %03d." % tile_index
	if terrain_editor.has_tileinfo_resource():
		summary += " Left-click empty terrain to place."
	else:
		summary += " Load or create a layout."
	summary += " %s." % _transform_text(flags)
	return summary


func _selection_summary_text(selected: NovaTerrainTileEntry) -> String:
	var text := "Tile %03d @ %d,%d." % [selected.get_tile_index(), selected.get_cell_x(), selected.get_cell_z()]
	text += " %s." % _transform_text(selected.get_flags())
	return text


func _transform_text(flags: int) -> String:
	var parts: Array[String] = []
	if (flags & NovaTerrainTileInfo.FLAG_FLIP_X) != 0:
		parts.append("Flip X")
	if (flags & NovaTerrainTileInfo.FLAG_FLIP_Y) != 0:
		parts.append("Flip Y")
	if (flags & NovaTerrainTileInfo.FLAG_ROTATE_90) != 0:
		parts.append("Rotate 90")
	if parts.is_empty():
		return "No transforms"
	return ", ".join(parts)


func _on_layout_load_pressed() -> void:
	if terrain_editor == null:
		return
	_open_tileinfo_dialog()


func _on_layout_new_pressed() -> void:
	if terrain_editor:
		terrain_editor.new_tileinfo()


func _on_layout_reset_pressed() -> void:
	if terrain_editor:
		terrain_editor.reset_tileinfo()


func _on_atlas_selected(idx: int) -> void:
	if _syncing or terrain_editor == null:
		return
	if terrain_editor.has_selected_tileinfo_entry():
		terrain_editor.replace_selected_tileinfo_tile_index(idx)
	else:
		terrain_editor.set_tile_stamp_tile_index(idx)


func _on_flip_x(pressed: bool) -> void:
	_set_flag(NovaTerrainTileInfo.FLAG_FLIP_X, pressed)


func _on_flip_y(pressed: bool) -> void:
	_set_flag(NovaTerrainTileInfo.FLAG_FLIP_Y, pressed)


func _on_rotate(pressed: bool) -> void:
	_set_flag(NovaTerrainTileInfo.FLAG_ROTATE_90, pressed)


func _set_flag(bit: int, pressed: bool) -> void:
	if _syncing or terrain_editor == null:
		return
	var selected := terrain_editor.get_selected_tileinfo_entry()
	var flags := selected.get_flags() if selected != null else terrain_editor.get_tile_stamp_flags()
	if pressed:
		flags |= bit
	else:
		flags &= ~bit
	if selected != null:
		terrain_editor.set_selected_tileinfo_flags(flags)
	else:
		terrain_editor.set_tile_stamp_flags(flags)


func _on_selection_done_pressed() -> void:
	if terrain_editor:
		terrain_editor.clear_tileinfo_selection()


func _on_selection_delete_pressed() -> void:
	if terrain_editor:
		terrain_editor.delete_selected_tileinfo_entry()


func _open_tileinfo_dialog() -> void:
	if _file_dialog == null:
		_file_dialog = FileDialogHelper.new(_root)
	var dir := terrain_editor.get_last_open_dir() if terrain_editor != null else ""
	_file_dialog.open("Load tile layout", PackedStringArray(["*.til ; Terrain tile layout"]),
		func(path: String) -> void:
			if terrain_editor:
				terrain_editor.load_tileinfo(path),
		dir)
