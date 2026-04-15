class_name StampInspector
extends MarginContainer

const TerrainEditorSlots = preload("res://modtools/terrain/terrain_editor_slots.gd")

@onready var _layout_filename: Label = %LayoutFilename
@onready var _layout_status: Label = %LayoutStatus
@onready var _layout_load: Button = %LayoutLoadButton
@onready var _layout_new: Button = %LayoutNewButton
@onready var _layout_reset: Button = %LayoutResetButton
@onready var _hint_label: Label = %HintLabel
@onready var _brush_preview: TextureRect = %BrushPreview
@onready var _brush_summary: Label = %BrushSummary
@onready var _atlas_list: ItemList = %AtlasList
@onready var _atlas_status: Label = %AtlasStatus
@onready var _flags_status: Label = %FlagsStatus
@onready var _flip_x: CheckBox = %FlipXToggle
@onready var _flip_y: CheckBox = %FlipYToggle
@onready var _rotate: CheckBox = %RotateToggle
@onready var _selection_status: Label = %SelectionStatus
@onready var _selection_preview: TextureRect = %SelectionPreview
@onready var _selection_summary: Label = %SelectionSummary
@onready var _selection_done: Button = %SelectionDoneButton
@onready var _selection_delete: Button = %SelectionDeleteButton

var editor: TerrainEditor
var _syncing: bool = false
var _atlas_tile_count: int = 0
var _cached_tilestrip_hash: int = -1
var _file_dialog: FileDialog


func _ready() -> void:
	_layout_load.pressed.connect(_on_layout_load_pressed)
	_layout_new.pressed.connect(_on_layout_new_pressed)
	_layout_reset.pressed.connect(_on_layout_reset_pressed)

	_atlas_list.icon_mode = ItemList.ICON_MODE_TOP
	_atlas_list.fixed_icon_size = Vector2i(48, 48)
	_atlas_list.max_columns = 6
	_atlas_list.same_column_width = true
	_atlas_list.item_selected.connect(_on_atlas_selected)

	_flip_x.toggled.connect(_on_flip_x)
	_flip_y.toggled.connect(_on_flip_y)
	_rotate.toggled.connect(_on_rotate)
	_selection_done.pressed.connect(_on_selection_done_pressed)
	_selection_delete.pressed.connect(_on_selection_delete_pressed)
	_brush_preview.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	_selection_preview.texture_filter = CanvasItem.TEXTURE_FILTER_NEAREST
	_hint_label.text = "Empty: place. Tile: edit."

	set_process(true)


func set_editor(value: TerrainEditor) -> void:
	editor = value
	if editor and editor.current_tool != TerrainEditor.Tool.TILE_STAMP:
		editor.set_tool(TerrainEditor.Tool.TILE_STAMP)
	_cached_tilestrip_hash = -1
	_sync_from_editor()


func _process(_delta: float) -> void:
	_sync_from_editor()


func sync_from_editor() -> void:
	_sync_from_editor()


func _sync_from_editor() -> void:
	if editor == null:
		return
	_syncing = true
	_refresh_layout()
	_refresh_brush()
	_refresh_atlas()
	_refresh_flags()
	_refresh_selection()
	_syncing = false


func _refresh_layout() -> void:
	var summary := editor.get_tileinfo_summary()
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
	var tilestrip := editor.get_slot_texture("tilestrip")
	var tile_index := editor.get_tile_stamp_tile_index()
	var flags := editor.get_tile_stamp_flags()
	_brush_preview.texture = _build_tile_preview(tilestrip, tile_index)
	_brush_summary.text = _brush_summary_text(tile_index, flags)


func _refresh_atlas() -> void:
	var data: NovaTerrainData = editor.get_data()
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
		if tilestrip:
			for i in tile_count:
				_atlas_list.add_item("%03d" % i, _build_icon(tilestrip, i, tiles_x), true)
		_cached_tilestrip_hash = strip_hash
		_atlas_tile_count = tile_count

	if tilestrip == null:
		_atlas_status.text = "Load a tile atlas in Properties."
	else:
		var brush_index := editor.get_tile_stamp_tile_index()
		var active_index := brush_index
		var active_label := "brush"
		var selected := editor.get_selected_tileinfo_entry()
		if selected != null:
			active_index = selected.get_tile_index()
			active_label = "editing"
		var source_name := editor.get_slot_filename("tilestrip")
		if source_name.is_empty():
			source_name = "loaded atlas"
		_atlas_status.text = "%d tiles - %s - %s %03d" % [tile_count, source_name, active_label, active_index]
		if _atlas_list.item_count > 0 and active_index >= 0 and active_index < _atlas_list.item_count:
			if not _atlas_list.is_selected(active_index):
				_atlas_list.select(active_index)


func _refresh_flags() -> void:
	var selected := editor.get_selected_tileinfo_entry()
	var flags := selected.get_flags() if selected != null else editor.get_tile_stamp_flags()
	_flip_x.set_pressed_no_signal((flags & NovaTerrainTileInfo.FLAG_FLIP_X) != 0)
	_flip_y.set_pressed_no_signal((flags & NovaTerrainTileInfo.FLAG_FLIP_Y) != 0)
	_rotate.set_pressed_no_signal((flags & NovaTerrainTileInfo.FLAG_ROTATE_90) != 0)
	if selected != null:
		_flags_status.text = "Editing transform. Brush stays unchanged."
	else:
		_flags_status.text = "Brush transform."


func _refresh_selection() -> void:
	var selected := editor.get_selected_tileinfo_entry()
	if selected == null:
		_selection_preview.texture = null
		_hint_label.text = "Empty: place. Tile: edit."
		_selection_status.text = "Placement mode"
		_selection_summary.text = "No tile selected."
		_selection_done.disabled = true
		_selection_delete.disabled = true
		return

	_hint_label.text = "Empty/Esc: back to placement."
	_selection_preview.texture = _build_tile_preview(editor.get_slot_texture("tilestrip"), selected.get_tile_index())
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
	var atlas := AtlasTexture.new()
	atlas.atlas = tilestrip
	atlas.region = Rect2(
		(tile_index % tiles_x) * NovaTerrainTileInfo.ATLAS_TILE_PIXELS,
		(tile_index / tiles_x) * NovaTerrainTileInfo.ATLAS_TILE_PIXELS,
		NovaTerrainTileInfo.ATLAS_TILE_PIXELS,
		NovaTerrainTileInfo.ATLAS_TILE_PIXELS
	)
	return atlas


func _brush_summary_text(tile_index: int, flags: int) -> String:
	if editor.get_slot_texture("tilestrip") == null:
		return "Load a tile atlas in Properties."
	var summary := "Brush %03d." % tile_index
	if editor.has_tileinfo_resource():
		summary += " Left-click empty terrain to place."
	else:
		summary += " Load or create a layout."
	summary += " %s." % _transform_text(flags)
	return summary


func _selection_summary_text(selected: NovaTerrainTileEntry) -> String:
	var text := "Tile %03d @ %d,%d." % [
		selected.get_tile_index(),
		selected.get_cell_x(),
		selected.get_cell_z(),
	]
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
	if editor == null:
		return
	_open_tileinfo_dialog()


func _on_layout_new_pressed() -> void:
	if editor:
		editor.new_tileinfo()


func _on_layout_reset_pressed() -> void:
	if editor:
		editor.reset_tileinfo()


func _on_atlas_selected(idx: int) -> void:
	if _syncing or editor == null:
		return
	if editor.has_selected_tileinfo_entry():
		editor.replace_selected_tileinfo_tile_index(idx)
	else:
		editor.set_tile_stamp_tile_index(idx)


func _on_flip_x(pressed: bool) -> void:
	_set_flag(NovaTerrainTileInfo.FLAG_FLIP_X, pressed)


func _on_flip_y(pressed: bool) -> void:
	_set_flag(NovaTerrainTileInfo.FLAG_FLIP_Y, pressed)


func _on_rotate(pressed: bool) -> void:
	_set_flag(NovaTerrainTileInfo.FLAG_ROTATE_90, pressed)


func _set_flag(bit: int, pressed: bool) -> void:
	if _syncing or editor == null:
		return
	var selected := editor.get_selected_tileinfo_entry()
	var flags := selected.get_flags() if selected != null else editor.get_tile_stamp_flags()
	if pressed:
		flags |= bit
	else:
		flags &= ~bit
	if selected != null:
		editor.set_selected_tileinfo_flags(flags)
	else:
		editor.set_tile_stamp_flags(flags)


func _on_selection_done_pressed() -> void:
	if editor:
		editor.clear_tileinfo_selection()


func _on_selection_delete_pressed() -> void:
	if editor:
		editor.delete_selected_tileinfo_entry()


func _open_tileinfo_dialog() -> void:
	if _file_dialog == null:
		_file_dialog = FileDialog.new()
		_file_dialog.use_native_dialog = true
		_file_dialog.access = FileDialog.ACCESS_FILESYSTEM
		_file_dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
		_file_dialog.min_size = Vector2i(760, 520)
		add_child(_file_dialog)

	_file_dialog.title = "Load tile layout"
	_file_dialog.filters = PackedStringArray(["*.til ; Terrain tile layout"])
	if editor and not editor.get_last_open_dir().is_empty():
		_file_dialog.current_dir = editor.get_last_open_dir()
	for sig in _file_dialog.file_selected.get_connections():
		_file_dialog.file_selected.disconnect(sig.callable)
	_file_dialog.file_selected.connect(func(path: String) -> void:
		if editor:
			editor.load_tileinfo(path)
	, CONNECT_ONE_SHOT)
	_file_dialog.popup_centered()
