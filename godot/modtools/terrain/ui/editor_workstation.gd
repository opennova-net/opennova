class_name TerrainEditorWorkstation
extends Control

enum Mode { SCULPT, PAINT, SCATTER, STAMP, LAYOUT }

const MODE_LABELS := {
	Mode.SCULPT: "Sculpt",
	Mode.PAINT: "Paint",
	Mode.SCATTER: "Foliage",
	Mode.STAMP: "Tile",
	Mode.LAYOUT: "Layout",
}

const DETAIL_LABELS := ["Detail A", "Detail B", "Detail C"]

enum PromptKind { NONE, UNSAVED, EXPORT, CDEP }

signal mode_changed(mode: int)

const INSPECTOR_SCENES := {
	Mode.SCULPT: preload("res://modtools/terrain/ui/inspectors/sculpt_inspector.tscn"),
	Mode.PAINT: preload("res://modtools/terrain/ui/inspectors/paint_inspector.tscn"),
	Mode.SCATTER: preload("res://modtools/terrain/ui/inspectors/scatter_inspector.tscn"),
	Mode.STAMP: preload("res://modtools/terrain/ui/inspectors/stamp_inspector.tscn"),
	Mode.LAYOUT: preload("res://modtools/terrain/ui/inspectors/layout_inspector.tscn"),
}

enum FileMenuItem { NEW, OPEN, SEP1, SAVE, SAVE_AS, SEP2, EXPORT }

@onready var _file_menu: MenuButton = %FileMenu
@onready var _project_label: Label = %ProjectLabel
@onready var _save_button: Button = %SaveButton
@onready var _export_button: Button = %ExportButton
@onready var _undo_button: Button = %UndoButton
@onready var _redo_button: Button = %RedoButton
@onready var _left_lane: PanelContainer = %LeftLane
@onready var _mode_rail: VBoxContainer = %ModeRail
@onready var _inspector_host: Control = %InspectorHost
@onready var _viewport_lane: Control = %ViewportLane
@onready var _asset_dock: TerrainEditorAssetDock = %AssetDock
@onready var _status_bar: PanelContainer = %StatusBar
@onready var _status_tool_label: Label = %StatusToolLabel
@onready var _status_context_label: Label = %StatusContextLabel
@onready var _status_camera_label: Label = %StatusCameraLabel
@onready var _status_fps_label: Label = %StatusFpsLabel
@onready var _status_sep2: VSeparator = %StatusSep2
@onready var _tile_gizmo: PanelContainer = %TileGizmo
@onready var _tile_gizmo_label: Label = %TileGizmoLabel
@onready var _tile_gizmo_done: Button = %TileGizmoDone
@onready var _tile_gizmo_rotate: Button = %TileGizmoRotate
@onready var _tile_gizmo_flip_x: Button = %TileGizmoFlipX
@onready var _tile_gizmo_flip_y: Button = %TileGizmoFlipY
@onready var _tile_gizmo_delete: Button = %TileGizmoDelete
@onready var _progress_backdrop: ColorRect = %ProgressBackdrop
@onready var _progress_panel: PanelContainer = %ProgressPanel
@onready var _progress_title_label: Label = %ProgressTitleLabel
@onready var _progress_message_label: Label = %ProgressMessageLabel
@onready var _progress_bar: ProgressBar = %ProgressBar
@onready var _progress_counts_label: Label = %ProgressCountsLabel
@onready var _prompt_backdrop: ColorRect = %PromptBackdrop
@onready var _prompt_host: CenterContainer = %PromptHost
@onready var _prompt_card: PanelContainer = %PromptCard
@onready var _prompt_eyebrow: Label = %PromptEyebrow
@onready var _prompt_lead: Label = %PromptLead
@onready var _prompt_body: Label = %PromptBody
@onready var _prompt_info_panel: PanelContainer = %PromptInfoPanel
@onready var _prompt_info_label: Label = %PromptInfoLabel
@onready var _prompt_format_section: VBoxContainer = %PromptFormatSection
@onready var _prompt_format_bhd: CheckButton = %PromptFormatBHD
@onready var _prompt_format_cdep: CheckButton = %PromptFormatCDEP
@onready var _prompt_secondary_button: Button = %PromptSecondaryButton
@onready var _prompt_tertiary_button: Button = %PromptTertiaryButton
@onready var _prompt_primary_button: Button = %PromptPrimaryButton

var editor: TerrainEditor
var _current_mode: int = -1
var _mode_buttons: Dictionary = {}
var _message_text: String = ""
var _message_until: float = 0.0
var _export_ui_active: bool = false
var _pending_export_dir: String = ""
var _overlay_tween: Tween
var _prompt_kind: int = PromptKind.NONE
var _prompt_primary_action: Callable = Callable()
var _prompt_secondary_action: Callable = Callable()
var _prompt_tertiary_action: Callable = Callable()


func _ready() -> void:
	_build_mode_rail()
	_wire_top_bar()
	_wire_prompts()
	_wire_tile_gizmo()
	_project_label.clip_text = true
	_status_context_label.clip_text = true
	_status_camera_label.clip_text = true
	_status_fps_label.clip_text = true
	set_process(true)
	set_mode(Mode.SCULPT)
	sync_from_editor_state()


func set_editor(value: TerrainEditor) -> void:
	editor = value
	_refresh_mode_from_tool()
	if _asset_dock and _asset_dock.has_method("set_editor"):
		_asset_dock.set_editor(value)
	for child in _inspector_host.get_children():
		if child.has_method("set_editor"):
			child.set_editor(value)
	sync_from_editor_state()


func sync_from_editor_state() -> void:
	_refresh_mode_from_tool()
	_refresh_project_label()
	_refresh_top_bar_state()
	_refresh_status()
	_refresh_tile_gizmo()
	_sync_export_progress()
	if _asset_dock and _asset_dock.has_method("sync_from_editor_state"):
		_asset_dock.sync_from_editor_state()


func _process(_delta: float) -> void:
	_refresh_top_bar_state()
	_sync_export_progress()
	_refresh_status()
	_refresh_tile_gizmo()


func _wire_top_bar() -> void:
	_undo_button.pressed.connect(_on_undo_pressed)
	_redo_button.pressed.connect(_on_redo_pressed)
	_save_button.pressed.connect(_on_save_pressed)
	_export_button.pressed.connect(_on_export_pressed)
	_build_file_menu()


func _wire_prompts() -> void:
	_prompt_backdrop.visible = false
	_prompt_host.visible = false
	_prompt_card.visible = false
	if not _prompt_format_bhd.toggled.is_connected(_on_prompt_format_bhd_toggled):
		_prompt_format_bhd.toggled.connect(_on_prompt_format_bhd_toggled)
	if not _prompt_format_cdep.toggled.is_connected(_on_prompt_format_cdep_toggled):
		_prompt_format_cdep.toggled.connect(_on_prompt_format_cdep_toggled)
	if not _prompt_secondary_button.pressed.is_connected(_on_prompt_secondary_pressed):
		_prompt_secondary_button.pressed.connect(_on_prompt_secondary_pressed)
	if not _prompt_tertiary_button.pressed.is_connected(_on_prompt_tertiary_pressed):
		_prompt_tertiary_button.pressed.connect(_on_prompt_tertiary_pressed)
	if not _prompt_primary_button.pressed.is_connected(_on_prompt_primary_pressed):
		_prompt_primary_button.pressed.connect(_on_prompt_primary_pressed)
	_reset_prompt_content()


func _wire_tile_gizmo() -> void:
	_tile_gizmo_done.pressed.connect(_on_tile_gizmo_done_pressed)
	_tile_gizmo_rotate.pressed.connect(_on_tile_gizmo_rotate_pressed)
	_tile_gizmo_flip_x.pressed.connect(_on_tile_gizmo_flip_x_pressed)
	_tile_gizmo_flip_y.pressed.connect(_on_tile_gizmo_flip_y_pressed)
	_tile_gizmo_delete.pressed.connect(_on_tile_gizmo_delete_pressed)


func _build_file_menu() -> void:
	var popup := _file_menu.get_popup()
	popup.clear()
	popup.add_item("New", FileMenuItem.NEW)
	popup.add_item("Open .trn...", FileMenuItem.OPEN)
	popup.add_separator()
	popup.add_item("Save", FileMenuItem.SAVE)
	popup.add_item("Save as...", FileMenuItem.SAVE_AS)
	popup.add_separator()
	popup.add_item("Export...", FileMenuItem.EXPORT)
	if not popup.id_pressed.is_connected(_on_file_menu_selected):
		popup.id_pressed.connect(_on_file_menu_selected)


func _on_file_menu_selected(id: int) -> void:
	if editor == null:
		return
	var open_trn := func(path: String) -> void:
		editor.request_open_trn(path)
	var save_project_as := func(dir_path: String) -> void:
		editor.save_project(dir_path)
	match id:
		FileMenuItem.NEW:
			editor.request_new_terrain()
		FileMenuItem.OPEN:
			_open_file_dialog(
				"Open .trn",
				PackedStringArray(["*.trn ; Terrain"]),
				open_trn,
				editor.get_last_open_dir()
			)
		FileMenuItem.SAVE:
			_on_save_pressed()
		FileMenuItem.SAVE_AS:
			_open_dir_dialog(
				"Choose where to save your project",
				save_project_as,
				_preferred_save_dir()
			)
		FileMenuItem.EXPORT:
			_on_export_pressed()


func _open_file_dialog(title: String, filters: PackedStringArray, on_pick: Callable, current_dir: String = "") -> void:
	var dialog := FileDialog.new()
	dialog.use_native_dialog = true
	dialog.access = FileDialog.ACCESS_FILESYSTEM
	dialog.file_mode = FileDialog.FILE_MODE_OPEN_FILE
	dialog.title = title
	dialog.filters = filters
	dialog.min_size = Vector2i(760, 520)
	if not current_dir.is_empty():
		dialog.current_dir = current_dir
	add_child(dialog)
	dialog.file_selected.connect(func(path: String) -> void:
		on_pick.call(path)
		dialog.queue_free()
	)
	dialog.canceled.connect(func() -> void:
		dialog.queue_free()
	)
	dialog.popup_centered()


func _open_dir_dialog(title: String, on_pick: Callable, current_dir: String = "") -> void:
	var dialog := FileDialog.new()
	dialog.use_native_dialog = true
	dialog.access = FileDialog.ACCESS_FILESYSTEM
	dialog.file_mode = FileDialog.FILE_MODE_OPEN_DIR
	dialog.title = title
	dialog.min_size = Vector2i(760, 520)
	if not current_dir.is_empty():
		dialog.current_dir = current_dir
	add_child(dialog)
	dialog.dir_selected.connect(func(path: String) -> void:
		on_pick.call(path)
		dialog.queue_free()
	)
	dialog.canceled.connect(func() -> void:
		dialog.queue_free()
	)
	dialog.popup_centered()


func _preferred_save_dir() -> String:
	if editor == null:
		return ""
	if editor.has_current_project_dir():
		return editor.get_current_project_dir()
	return editor.get_last_save_dir()


func _preferred_export_dir() -> String:
	if editor == null:
		return ""
	if not editor.get_last_export_dir().is_empty():
		return editor.get_last_export_dir()
	if editor.has_current_project_dir():
		return editor.get_current_project_dir()
	return editor.get_last_save_dir()


func _on_undo_pressed() -> void:
	if editor:
		editor.undo()


func _on_redo_pressed() -> void:
	if editor:
		editor.redo()


func _on_save_pressed() -> void:
	if editor == null:
		return
	var err := editor.save_project_to_current_dir()
	if err == ERR_INVALID_PARAMETER:
		var save_project_as := func(dir_path: String) -> void:
			editor.save_project(dir_path)
		_open_dir_dialog(
			"Choose where to save your project",
			save_project_as,
			_preferred_save_dir()
		)


func _on_export_pressed() -> void:
	if editor == null:
		return
	var choose_flavor := func(dir_path: String) -> void:
		_show_export_flavor_dialog(dir_path)
	_open_dir_dialog(
		"Choose where to export",
		choose_flavor,
		_preferred_export_dir()
	)


func _refresh_top_bar_state() -> void:
	var busy := editor != null and editor.is_export_running()
	_file_menu.disabled = editor == null or busy
	for button in _mode_buttons.values():
		var mode_button := button as Button
		if mode_button:
			mode_button.disabled = busy

	if editor == null:
		_save_button.disabled = true
		_export_button.disabled = true
		_undo_button.disabled = true
		_redo_button.disabled = true
		_apply_busy_modulation(false)
		return

	_save_button.disabled = busy or not editor.is_dirty
	_export_button.disabled = busy
	_undo_button.disabled = busy or not editor.can_undo()
	_redo_button.disabled = busy or not editor.can_redo()
	_apply_busy_modulation(busy)


func _apply_busy_modulation(active: bool) -> void:
	var alpha := 0.55 if active else 1.0
	var color := Color(1.0, 1.0, 1.0, alpha)
	_left_lane.modulate = color
	_asset_dock.modulate = color
	_status_bar.modulate = color


func _refresh_mode_from_tool() -> void:
	if editor == null:
		return
	var mode := _mode_for_tool(editor.current_tool)
	if mode != _current_mode:
		set_mode(mode)


func set_mode(mode: int) -> void:
	if mode == _current_mode:
		return
	_current_mode = mode
	for m in _mode_buttons:
		var button: Button = _mode_buttons[m]
		button.set_pressed_no_signal(m == mode)
	_swap_inspector(mode)
	mode_changed.emit(mode)


func _swap_inspector(mode: int) -> void:
	for child in _inspector_host.get_children():
		child.queue_free()
	if INSPECTOR_SCENES.has(mode):
		var inspector: Node = INSPECTOR_SCENES[mode].instantiate()
		_inspector_host.add_child(inspector)
		if editor and inspector.has_method("set_editor"):
			inspector.set_editor(editor)
	else:
		var placeholder := Label.new()
		placeholder.text = "%s inspector coming soon" % MODE_LABELS[mode]
		placeholder.theme_type_variation = &"Muted"
		placeholder.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
		placeholder.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
		placeholder.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		placeholder.size_flags_vertical = Control.SIZE_EXPAND_FILL
		_inspector_host.add_child(placeholder)


func _mode_for_tool(tool: int) -> int:
	match tool:
		TerrainEditor.Tool.RAISE, TerrainEditor.Tool.LOWER, TerrainEditor.Tool.SMOOTH, TerrainEditor.Tool.FLATTEN:
			return Mode.SCULPT
		TerrainEditor.Tool.PAINT_DETAIL, TerrainEditor.Tool.PAINT_COLORMAP, TerrainEditor.Tool.CLONE_COLOR, TerrainEditor.Tool.SURFACE_PAINT:
			return Mode.PAINT
		TerrainEditor.Tool.FOLIAGE_PAINT:
			return Mode.SCATTER
		TerrainEditor.Tool.TILE_STAMP:
			return Mode.STAMP
		_:
			return Mode.LAYOUT


func _build_mode_rail() -> void:
	for mode in Mode.values():
		var btn := Button.new()
		btn.text = MODE_LABELS[mode]
		btn.toggle_mode = true
		btn.focus_mode = Control.FOCUS_NONE
		btn.custom_minimum_size = Vector2(0, 44)
		btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		btn.tooltip_text = _mode_tooltip(mode)
		btn.pressed.connect(_on_mode_pressed.bind(mode))
		_mode_rail.add_child(btn)
		_mode_buttons[mode] = btn


func _mode_tooltip(mode: int) -> String:
	match mode:
		Mode.SCULPT:
			return "Raise, lower, smooth, and flatten the terrain."
		Mode.PAINT:
			return "Paint detail layers, color, clone, and surface types."
		Mode.SCATTER:
			return "Manage and paint foliage placement."
		Mode.STAMP:
			return "Place and edit tiles."
		Mode.LAYOUT:
			return "Edit sectors, map size, origin, and water."
		_:
			return ""


func _on_mode_pressed(mode: int) -> void:
	if mode == Mode.LAYOUT and editor:
		editor.set_tool(TerrainEditor.Tool.EDIT_SECTORS)
	set_mode(mode)


func _refresh_project_label() -> void:
	if editor == null:
		_project_label.text = "OpenNova Terrain Editor"
		return
	var name := editor.get_terrain_name_value()
	if name.is_empty():
		name = "untitled"
	var dirty := "*" if editor.is_dirty else ""
	_project_label.text = "%s%s" % [name, dirty]


func show_status_message(text: String, duration: float = 4.0) -> void:
	_message_text = text
	_message_until = Time.get_ticks_msec() / 1000.0 + duration


func _refresh_status() -> void:
	if editor == null:
		_status_tool_label.text = ""
		_status_context_label.text = ""
		_status_camera_label.text = ""
		_status_fps_label.text = ""
		return

	var now := Time.get_ticks_msec() / 1000.0
	if _message_text != "" and now < _message_until:
		_status_tool_label.text = _message_text
		_status_tool_label.theme_type_variation = &"Warn"
	else:
		_status_tool_label.text = _tool_name(editor.current_tool)
		_status_tool_label.theme_type_variation = &""

	_status_context_label.text = _context_summary()
	if editor.camera:
		var pos := editor.camera.global_position
		_status_camera_label.text = "%.0f, %.0f, %.0f" % [pos.x, pos.y, pos.z]
	else:
		_status_camera_label.text = ""
	_status_fps_label.text = "%d fps" % Engine.get_frames_per_second()
	_refresh_project_label()


func _refresh_tile_gizmo() -> void:
	if not is_node_ready() or _tile_gizmo == null or _viewport_lane == null or _tile_gizmo_label == null:
		return
	_tile_gizmo.visible = false
	if editor == null or _current_mode != Mode.STAMP or not editor.has_selected_tileinfo_entry():
		return

	var camera := editor.get_editor_camera()
	if camera == null:
		return

	var entry := editor.get_selected_tileinfo_entry()
	if entry == null:
		return

	var anchor_world := editor.get_selected_tileinfo_world_center() + Vector3(0.0, 2.0, 0.0)
	var lane_rect := _viewport_lane.get_global_rect()
	var lane_end := lane_rect.position + lane_rect.size
	_tile_gizmo_label.text = "Editing tile %03d @ (%d, %d)" % [
		entry.get_tile_index(),
		entry.get_cell_x(),
		entry.get_cell_z(),
	]

	var gizmo_size := _tile_gizmo.get_combined_minimum_size()
	_tile_gizmo.size = gizmo_size
	var target := Vector2(
		lane_rect.position.x + (lane_rect.size.x - gizmo_size.x) * 0.5,
		lane_rect.position.y + 18.0
	)
	if not camera.is_position_behind(anchor_world):
		var screen_pos := camera.unproject_position(anchor_world)
		target = Vector2(
			screen_pos.x - gizmo_size.x * 0.5,
			screen_pos.y - gizmo_size.y - 24.0
		)
	target.x = clampf(target.x, lane_rect.position.x + 12.0, lane_end.x - gizmo_size.x - 12.0)
	target.y = clampf(target.y, lane_rect.position.y + 12.0, lane_end.y - gizmo_size.y - 12.0)
	_tile_gizmo.global_position = target
	_tile_gizmo.visible = true


func _context_summary() -> String:
	if editor == null:
		return ""
	match editor.current_tool:
		TerrainEditor.Tool.PAINT_DETAIL:
			var channel := clampi(editor.get_paint_detail_channel(), 0, DETAIL_LABELS.size() - 1)
			return DETAIL_LABELS[channel]
		TerrainEditor.Tool.PAINT_COLORMAP:
			return "#" + editor.get_paint_color().to_html(false).to_upper()
		TerrainEditor.Tool.CLONE_COLOR:
			return "Clone source ready" if editor.has_clone_source() else "Clone source not set"
		TerrainEditor.Tool.SURFACE_PAINT:
			return "%s (%d)" % [editor.get_selected_surface_label(), editor.get_selected_surface_index()]
		TerrainEditor.Tool.FOLIAGE_PAINT:
			var def := editor.get_selected_foliage_def()
			if def == null:
				return "No foliage type selected"
			var label := String(def.graphic).strip_edges()
			if label.is_empty():
				label = "unnamed foliage type"
			return label
		TerrainEditor.Tool.TILE_STAMP:
			var selected := editor.get_tileinfo_selected_index()
			if selected >= 0:
				var entry := editor.get_tileinfo_entry(selected)
				if entry:
					return "Editing tile %03d at cell (%d, %d)" % [entry.get_tile_index(), entry.get_cell_x(), entry.get_cell_z()]
			return "Brush tile %03d" % editor.get_tile_stamp_tile_index()
		TerrainEditor.Tool.EDIT_SECTORS:
			return "%d x %d sector grid" % [editor.get_sector_count(), editor.get_sector_rows()]
		_:
			return "R %d  S %.2f  H %.2f" % [roundi(editor.brush_radius), editor.brush_strength, editor.brush_hardness]


func _tool_name(tool: int) -> String:
	match tool:
		TerrainEditor.Tool.RAISE: return "Raise"
		TerrainEditor.Tool.LOWER: return "Lower"
		TerrainEditor.Tool.SMOOTH: return "Smooth"
		TerrainEditor.Tool.FLATTEN: return "Flatten"
		TerrainEditor.Tool.PAINT_DETAIL: return "Paint detail"
		TerrainEditor.Tool.PAINT_COLORMAP: return "Paint color"
		TerrainEditor.Tool.SURFACE_PAINT: return "Paint surface"
		TerrainEditor.Tool.CLONE_COLOR: return "Clone color"
		TerrainEditor.Tool.FOLIAGE_PAINT: return "Foliage"
		TerrainEditor.Tool.TILE_STAMP: return "Tile"
		TerrainEditor.Tool.EDIT_SECTORS: return "Edit sectors"
		_:
			return ""


func _on_tile_gizmo_rotate_pressed() -> void:
	if editor:
		editor.rotate_selected_tileinfo_clockwise()


func _on_tile_gizmo_done_pressed() -> void:
	if editor:
		editor.clear_tileinfo_selection()


func _on_tile_gizmo_flip_x_pressed() -> void:
	if editor:
		editor.flip_selected_tileinfo_x()


func _on_tile_gizmo_flip_y_pressed() -> void:
	if editor:
		editor.flip_selected_tileinfo_y()


func _on_tile_gizmo_delete_pressed() -> void:
	if editor:
		editor.delete_selected_tileinfo_entry()


func prompt_unsaved_changes(_action_name: String) -> void:
	_set_prompt_state(
		PromptKind.UNSAVED,
		"",
		"Save changes?",
		"",
		"",
		false
	)
	_set_prompt_buttons("Cancel", "Discard", "Save")
	_prompt_secondary_action = Callable(self, "_on_prompt_keep_editing")
	_prompt_tertiary_action = Callable(self, "_on_prompt_discard_changes")
	_prompt_primary_action = Callable(self, "_on_prompt_save_changes")
	_set_prompt_visible(true)
	show_status_message("Save or discard your changes to continue.", 6.0)


func prompt_save_directory_for_pending_action(_action_name: String) -> void:
	var save_pending_as := func(dir_path: String) -> void:
		if editor:
			editor.confirm_pending_action_save_as(dir_path)
	_open_dir_dialog(
		"Select project save directory",
		save_pending_as,
		_preferred_save_dir()
	)


func prompt_cdep_violations(count: int, on_fix_callback: Callable) -> void:
	var plural := "" if count == 1 else "s"
	_set_prompt_state(
		PromptKind.CDEP,
		"",
		"Flatten before export?",
		"%d area%s exceed the JO/DFX limit." % [count, plural],
		"BHD exports are unaffected.",
		false
	)
	_set_prompt_buttons("Leave as-is", "", "Flatten automatically")
	_prompt_secondary_action = Callable()
	_prompt_primary_action = Callable(func() -> void:
		on_fix_callback.call()
	)
	_set_prompt_visible(true)
	show_status_message("%d area%s too steep for Joint Operations / DFX export." % [count, plural], 6.0)


func _on_prompt_save_changes() -> void:
	if editor:
		editor.confirm_pending_action_save()


func _on_prompt_discard_changes() -> void:
	if editor:
		editor.confirm_pending_action_discard()


func _on_prompt_keep_editing() -> void:
	if editor:
		editor.cancel_pending_action()


func _on_prompt_leave_as_is() -> void:
	pass


func _show_export_flavor_dialog(dir_path: String) -> void:
	_pending_export_dir = dir_path
	_select_export_flavor(TerrainEditor.ExportFlavor.DFX_JO)
	_sync_export_prompt_copy()
	_set_prompt_state(
		PromptKind.EXPORT,
		"",
		"Export",
		"",
		"",
		true
	)
	_set_prompt_buttons("Cancel", "", "Export")
	_prompt_secondary_action = Callable(self, "_on_prompt_cancel")
	_prompt_primary_action = Callable(self, "_on_prompt_export_confirmed")
	_set_prompt_visible(true)


func _on_prompt_export_confirmed() -> void:
	if editor == null or _pending_export_dir.is_empty():
		return
	var flavor := _current_export_flavor()
	var err := editor.begin_export_terrain(_pending_export_dir, flavor)
	_pending_export_dir = ""
	_set_prompt_visible(false)
	if err != OK:
		show_status_message("Export failed (error %d)" % err, 6.0)


func _on_prompt_cancel() -> void:
	_pending_export_dir = ""
	_set_prompt_visible(false)


func _on_prompt_primary_pressed() -> void:
	if _prompt_primary_action.is_valid():
		_prompt_primary_action.call()
	_set_prompt_visible(false)


func _on_prompt_secondary_pressed() -> void:
	if _prompt_secondary_action.is_valid():
		_prompt_secondary_action.call()
	_set_prompt_visible(false)


func _on_prompt_tertiary_pressed() -> void:
	if _prompt_tertiary_action.is_valid():
		_prompt_tertiary_action.call()
	_set_prompt_visible(false)


func _on_prompt_format_bhd_toggled(pressed: bool) -> void:
	if not pressed:
		return
	_sync_export_prompt_copy()


func _on_prompt_format_cdep_toggled(pressed: bool) -> void:
	if not pressed:
		return
	_sync_export_prompt_copy()


func _sync_export_prompt_copy() -> void:
	var flavor := _current_export_flavor()
	_prompt_lead.text = "Export"
	_prompt_body.text = ""
	_prompt_info_label.text = ""
	_prompt_format_section.visible = true
	_prompt_info_panel.visible = false
	_prompt_format_bhd.button_pressed = flavor == TerrainEditor.ExportFlavor.BHD
	_prompt_format_cdep.button_pressed = flavor == TerrainEditor.ExportFlavor.DFX_JO


func _select_export_flavor(flavor: int) -> void:
	_prompt_format_bhd.button_pressed = flavor == TerrainEditor.ExportFlavor.BHD
	_prompt_format_cdep.button_pressed = flavor == TerrainEditor.ExportFlavor.DFX_JO


func _current_export_flavor() -> int:
	return TerrainEditor.ExportFlavor.BHD if _prompt_format_bhd.button_pressed else TerrainEditor.ExportFlavor.DFX_JO


func _set_prompt_state(kind: int, eyebrow_text: String, lead_text: String, body_text: String, info_text: String, show_format_section: bool) -> void:
	_prompt_kind = kind
	_prompt_eyebrow.text = eyebrow_text
	_prompt_eyebrow.visible = not eyebrow_text.strip_edges().is_empty()
	_prompt_lead.text = lead_text
	_prompt_body.text = body_text
	_prompt_body.visible = not body_text.strip_edges().is_empty()
	_prompt_info_label.text = info_text
	_prompt_info_panel.visible = not info_text.strip_edges().is_empty()
	_prompt_format_section.visible = show_format_section
	_prompt_card.visible = true


func _set_prompt_buttons(secondary_text: String, tertiary_text: String, primary_text: String) -> void:
	_prompt_secondary_button.text = secondary_text
	_prompt_secondary_button.visible = not secondary_text.is_empty()
	_prompt_tertiary_button.text = tertiary_text
	_prompt_tertiary_button.visible = not tertiary_text.is_empty()
	_prompt_primary_button.text = primary_text
	_prompt_primary_button.visible = not primary_text.is_empty()


func _set_prompt_visible(active: bool) -> void:
	_prompt_backdrop.visible = active
	_prompt_host.visible = active
	_prompt_card.visible = active
	if active:
		_prompt_primary_button.grab_focus()
	else:
		_prompt_kind = PromptKind.NONE
		_prompt_primary_action = Callable()
		_prompt_secondary_action = Callable()
		_prompt_tertiary_action = Callable()
		_reset_prompt_content()


func _reset_prompt_content() -> void:
	_prompt_eyebrow.text = "Prompt"
	_prompt_eyebrow.visible = false
	_prompt_lead.text = "Prompt title"
	_prompt_body.text = "Prompt body"
	_prompt_body.visible = false
	_prompt_info_label.text = ""
	_prompt_info_panel.visible = false
	_prompt_format_section.visible = false
	_prompt_secondary_button.visible = false
	_prompt_tertiary_button.visible = false
	_prompt_primary_button.visible = false


func on_export_started(_dir_path: String) -> void:
	show_status_message("Exporting terrain...", 30.0)
	_set_export_ui_active(true)
	_sync_export_progress()


func on_export_completed(err: Error, message: String) -> void:
	_set_export_ui_active(false)
	if not message.is_empty():
		show_status_message(message, 6.0)
	else:
		show_status_message("Export failed (error %d)" % err, 6.0)
	_refresh_top_bar_state()


func _sync_export_progress() -> void:
	if editor == null:
		_set_export_ui_active(false)
		return

	var export_running := editor.is_export_running()
	if export_running != _export_ui_active:
		_set_export_ui_active(export_running)
	if not export_running:
		return

	var phase := editor.get_export_progress_phase()
	var message := editor.get_export_progress_message()
	var current := editor.get_export_progress_current()
	var total := editor.get_export_progress_total()
	var ratio := clampf(editor.get_export_progress_ratio(), 0.0, 1.0)

	_progress_title_label.text = "Exporting terrain..."
	if not phase.is_empty():
		_progress_message_label.text = phase.capitalize() + ": " + message
	else:
		_progress_message_label.text = message
	_progress_bar.value = ratio * 100.0
	if total > 0:
		_progress_counts_label.text = "%d / %d" % [current, total]
	else:
		_progress_counts_label.text = ""


func _set_export_ui_active(active: bool) -> void:
	if _export_ui_active == active and _progress_backdrop.visible == active:
		return
	_export_ui_active = active
	if _overlay_tween:
		_overlay_tween.kill()
	_overlay_tween = create_tween()
	if active:
		_progress_backdrop.visible = true
		_progress_panel.visible = true
		_progress_backdrop.modulate = Color(1.0, 1.0, 1.0, 0.0)
		_progress_panel.modulate = Color(1.0, 1.0, 1.0, 0.0)
		_overlay_tween.tween_property(_progress_backdrop, "modulate", Color(1.0, 1.0, 1.0, 1.0), 0.16).set_trans(Tween.TRANS_SINE).set_ease(Tween.EASE_OUT)
		_overlay_tween.parallel().tween_property(_progress_panel, "modulate", Color(1.0, 1.0, 1.0, 1.0), 0.16).set_trans(Tween.TRANS_SINE).set_ease(Tween.EASE_OUT)
	else:
		_overlay_tween.tween_property(_progress_backdrop, "modulate", Color(1.0, 1.0, 1.0, 0.0), 0.14).set_trans(Tween.TRANS_SINE).set_ease(Tween.EASE_IN)
		_overlay_tween.parallel().tween_property(_progress_panel, "modulate", Color(1.0, 1.0, 1.0, 0.0), 0.14).set_trans(Tween.TRANS_SINE).set_ease(Tween.EASE_IN)
		_overlay_tween.finished.connect(func() -> void:
			if not _export_ui_active:
				_progress_backdrop.visible = false
				_progress_panel.visible = false
		, CONNECT_ONE_SHOT)
