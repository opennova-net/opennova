class_name EditorWorkstation
extends Control

const TerrainWorkspaceAdapter = preload("res://modtools/editor/terrain_workspace.gd")
const EnvironmentWorkspaceAdapter = preload("res://modtools/editor/environment_workspace.gd")
const PlaceholderWorkspaceAdapter = preload("res://modtools/editor/placeholder_workspace.gd")

enum Workspace { TERRAIN, ENVIRONMENT, MISSION }

const WORKSPACE_LABELS := {
	Workspace.TERRAIN: "Terrain",
	Workspace.ENVIRONMENT: "Environment",
	Workspace.MISSION: "Mission",
}

enum PromptKind { NONE, UNSAVED, EXPORT, CDEP }

signal workflow_changed(workflow_id: int)

enum FileMenuItem { NEW, OPEN, SEP1, SAVE, SAVE_AS, SEP2, EXPORT }

@onready var _file_menu: MenuButton = %FileMenu
@onready var _project_label: Label = %ProjectLabel
@onready var _save_button: Button = %SaveButton
@onready var _export_button: Button = %ExportButton
@onready var _undo_button: Button = %UndoButton
@onready var _redo_button: Button = %RedoButton
@onready var _left_lane: PanelContainer = %LeftLane
@onready var _workspace_rail: HBoxContainer = %WorkspaceRail
@onready var _modes_label: Label = %ModesLabel
@onready var _mode_rail: VBoxContainer = %ModeRail
@onready var _inspector_host: Control = %InspectorHost
@onready var _viewport_lane: Control = %ViewportLane
@onready var _asset_dock: Control = %AssetDock
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

var editor: Node
var _active_workspace_id: int = Workspace.TERRAIN
var _workspaces: Dictionary = {}
var _workspace_buttons: Dictionary = {}
var _current_workflow_id: int = -1
var _workflow_buttons: Dictionary = {}
var _placeholder_workspace_id: int = -1
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
	_ensure_workspaces()
	_build_workspace_rail()
	_wire_top_bar()
	_wire_prompts()
	_wire_tile_gizmo()
	_project_label.clip_text = true
	_status_context_label.clip_text = true
	_status_camera_label.clip_text = true
	_status_fps_label.clip_text = true
	set_process(true)
	_refresh_workspace_surface()
	sync_from_editor_state()


func set_editor(value: Node) -> void:
	editor = value
	_ensure_workspaces()
	var terrain_workspace = _workspaces.get(Workspace.TERRAIN)
	if terrain_workspace != null and terrain_workspace.has_method("set_terrain_editor"):
		terrain_workspace.set_terrain_editor(value)
	var environment_workspace = _workspaces.get(Workspace.ENVIRONMENT)
	if environment_workspace != null and environment_workspace.has_method("set_environment_editor") and value != null and value.has_method("get_environment_editor"):
		environment_workspace.set_environment_editor(value.get_environment_editor())
	_refresh_workspace_surface()
	for child in _inspector_host.get_children():
		if child.has_method("set_editor"):
			child.set_editor(value)
	sync_from_editor_state()


func sync_from_editor_state() -> void:
	_ensure_workspaces()
	_refresh_workspace_buttons()
	_refresh_workflow_from_workspace()
	_refresh_project_label()
	_refresh_top_bar_state()
	_refresh_status()
	_refresh_tile_gizmo()
	_sync_export_progress()
	var workspace = _get_active_workspace()
	if workspace != null:
		workspace.sync_asset_dock()


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


func _ensure_workspaces() -> void:
	if _workspaces.is_empty():
		_workspaces[Workspace.TERRAIN] = TerrainWorkspaceAdapter.new(editor)
		var environment_editor: Variant = editor.get_environment_editor() if editor != null and editor.has_method("get_environment_editor") else null
		_workspaces[Workspace.ENVIRONMENT] = EnvironmentWorkspaceAdapter.new(environment_editor)
		_workspaces[Workspace.MISSION] = PlaceholderWorkspaceAdapter.new(
			"mission",
			"Mission",
			"Mission entity editing is planned; mission formats are not implemented yet."
		)
	for workspace in _workspaces.values():
		if workspace != null and workspace.has_method("set_editor_shell"):
			workspace.set_editor_shell(self)


func _build_workspace_rail() -> void:
	for workspace_id in Workspace.values():
		var btn := Button.new()
		btn.text = WORKSPACE_LABELS[workspace_id]
		btn.toggle_mode = true
		btn.focus_mode = Control.FOCUS_NONE
		btn.custom_minimum_size = Vector2(0, 36)
		btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		btn.tooltip_text = _workspace_tooltip(workspace_id)
		btn.pressed.connect(_on_workspace_pressed.bind(workspace_id))
		_workspace_rail.add_child(btn)
		_workspace_buttons[workspace_id] = btn
	_refresh_workspace_buttons()


func _workspace_tooltip(workspace_id: int) -> String:
	match workspace_id:
		Workspace.TERRAIN:
			return "Edit terrain sculpting, paint, foliage, tiles, and layout."
		Workspace.ENVIRONMENT:
			return "Edit .env weather, lighting, atmosphere, and time of day."
		Workspace.MISSION:
			return "Reserved for mission entities, objectives, and triggers."
		_:
			return ""


func _on_workspace_pressed(workspace_id: int) -> void:
	set_active_workspace(workspace_id)


func set_active_workspace(workspace_id: int) -> void:
	if not _workspaces.has(workspace_id) or workspace_id == _active_workspace_id:
		_refresh_workspace_buttons()
		return
	var current_workspace = _get_active_workspace()
	if current_workspace != null and current_workspace.has_method("deactivate"):
		current_workspace.deactivate()
	_active_workspace_id = workspace_id
	var next_workspace = _get_active_workspace()
	if next_workspace != null and next_workspace.has_method("activate"):
		next_workspace.activate()
	_refresh_workspace_surface()
	sync_from_editor_state()


func get_active_workspace_id() -> int:
	return _active_workspace_id


func _get_active_workspace() -> Variant:
	return _workspaces.get(_active_workspace_id)


func _is_terrain_workspace_active() -> bool:
	return _active_workspace_id == Workspace.TERRAIN


func _any_workspace_busy() -> bool:
	for workspace in _workspaces.values():
		if workspace != null and workspace.has_method("is_busy") and workspace.is_busy():
			return true
	return false


func _refresh_workspace_buttons() -> void:
	var busy := _any_workspace_busy()
	for workspace_id in _workspace_buttons:
		var btn: Button = _workspace_buttons[workspace_id]
		btn.set_pressed_no_signal(workspace_id == _active_workspace_id)
		btn.disabled = busy


func _refresh_workspace_surface() -> void:
	var workspace = _get_active_workspace()
	var workflows: Array = workspace.get_workflows() if workspace != null else []
	var has_workflows: bool = not workflows.is_empty()
	_modes_label.visible = has_workflows
	_mode_rail.visible = has_workflows
	_asset_dock.visible = workspace != null and workspace.uses_asset_dock()
	if workspace != null:
		workspace.set_asset_dock(_asset_dock if _asset_dock.visible else null)
	_rebuild_workflow_rail(workflows)
	if has_workflows:
		_placeholder_workspace_id = -1
		_current_workflow_id = -1
		_refresh_workflow_from_workspace()
	else:
		_show_workspace_inspector(workspace)
	_refresh_workspace_buttons()


func _show_workspace_inspector(workspace: Variant) -> void:
	if _placeholder_workspace_id == _active_workspace_id:
		return
	_placeholder_workspace_id = _active_workspace_id
	for child in _inspector_host.get_children():
		child.queue_free()
	if workspace != null and workspace.has_method("build_inspector"):
		workspace.build_inspector(_inspector_host)
		if _inspector_host.get_child_count() > 0:
			return
	var label := Label.new()
	var workspace_label := "Workspace"
	var context := "This workspace is not implemented yet."
	if workspace != null:
		workspace_label = workspace.get_workspace_label()
		context = workspace.get_status_context()
	label.text = "%s workspace\n%s" % [workspace_label, context]
	label.theme_type_variation = &"Muted"
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	label.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_inspector_host.add_child(label)


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
	var workspace = _get_active_workspace()
	if workspace == null:
		return
	var open_trn := func(path: String) -> void:
		var err: Error = workspace.open_file(path)
		if err != OK:
			show_status_message("Open failed (error %d)" % err, 6.0)
	var save_project_as := func(dir_path: String) -> void:
		var err: Error = workspace.save_as(dir_path)
		if err != OK:
			show_status_message("Save failed (error %d)" % err, 6.0)
	match id:
		FileMenuItem.NEW:
			if workspace.can_new():
				workspace.new_current()
		FileMenuItem.OPEN:
			if not workspace.can_open():
				return
			_open_file_dialog(
				workspace.get_open_dialog_title(),
				workspace.get_open_dialog_filters(),
				open_trn,
				workspace.get_open_dialog_dir()
			)
		FileMenuItem.SAVE:
			_on_save_pressed()
		FileMenuItem.SAVE_AS:
			_open_dir_dialog(
				workspace.get_save_dialog_title(),
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
	var workspace = _get_active_workspace()
	if workspace != null:
		var dir: String = workspace.get_save_dialog_dir()
		if not dir.is_empty():
			return dir
	if editor == null:
		return ""
	if editor.has_current_project_dir():
		return editor.get_current_project_dir()
	return editor.get_last_save_dir()


func _preferred_export_dir() -> String:
	var workspace = _get_active_workspace()
	if workspace != null:
		var dir: String = workspace.get_export_dialog_dir()
		if not dir.is_empty():
			return dir
	if editor == null:
		return ""
	if not editor.get_last_export_dir().is_empty():
		return editor.get_last_export_dir()
	if editor.has_current_project_dir():
		return editor.get_current_project_dir()
	return editor.get_last_save_dir()


func _on_undo_pressed() -> void:
	var workspace = _get_active_workspace()
	if workspace != null:
		workspace.undo()


func _on_redo_pressed() -> void:
	var workspace = _get_active_workspace()
	if workspace != null:
		workspace.redo()


func _on_save_pressed() -> void:
	var workspace = _get_active_workspace()
	if workspace == null:
		return
	var err: Error = workspace.save_current()
	if err == ERR_INVALID_PARAMETER:
		var save_project_as := func(dir_path: String) -> void:
			workspace.save_as(dir_path)
		_open_dir_dialog(
			"Choose where to save your project",
			save_project_as,
			_preferred_save_dir()
		)
	elif err != OK:
		show_status_message("%s save is not available." % workspace.get_workspace_label(), 4.0)


func _on_export_pressed() -> void:
	var workspace = _get_active_workspace()
	if workspace == null or not workspace.can_export():
		return
	var choose_export_dir := func(dir_path: String) -> void:
		if _is_terrain_workspace_active():
			_show_export_flavor_dialog(dir_path)
		else:
			var err: Error = workspace.begin_export(dir_path, 0)
			if err == OK:
				show_status_message("%s exported." % workspace.get_workspace_label(), 4.0)
			else:
				show_status_message("Export failed (error %d)" % err, 6.0)
	_open_dir_dialog(
		workspace.get_export_dialog_title(),
		choose_export_dir,
		_preferred_export_dir()
	)


func _refresh_top_bar_state() -> void:
	var workspace = _get_active_workspace()
	var busy := _any_workspace_busy()
	_file_menu.disabled = workspace == null or busy or not _workspace_has_file_actions(workspace)
	_sync_file_menu_labels(workspace)
	_sync_file_menu_state(workspace, busy)
	for button in _workflow_buttons.values():
		var workflow_button := button as Button
		if workflow_button:
			workflow_button.disabled = busy

	if workspace == null:
		_save_button.disabled = true
		_export_button.disabled = true
		_undo_button.disabled = true
		_redo_button.disabled = true
		_apply_busy_modulation(false)
		return

	_save_button.disabled = busy or not workspace.can_save()
	_export_button.disabled = busy or not workspace.can_export()
	_undo_button.disabled = busy or not workspace.can_undo()
	_redo_button.disabled = busy or not workspace.can_redo()
	_apply_busy_modulation(busy)


func _workspace_has_file_actions(workspace: Variant) -> bool:
	if workspace == null:
		return false
	return workspace.can_new() or workspace.can_open() or workspace.can_save() or workspace.can_save_as() or workspace.can_export()


func _sync_file_menu_labels(workspace: Variant) -> void:
	if _file_menu == null:
		return
	var popup := _file_menu.get_popup()
	if popup == null or popup.get_item_count() < 7:
		return
	var open_label := "Open..."
	if workspace != null:
		match workspace.get_workspace_id():
			"terrain":
				open_label = "Open .trn..."
			"environment":
				open_label = "Open .env..."
	popup.set_item_text(popup.get_item_index(FileMenuItem.OPEN), open_label)


func _sync_file_menu_state(workspace: Variant, busy: bool) -> void:
	if _file_menu == null:
		return
	var popup := _file_menu.get_popup()
	if popup == null or popup.get_item_count() < 7:
		return
	_set_file_menu_item_disabled(popup, FileMenuItem.NEW, busy or workspace == null or not workspace.can_new())
	_set_file_menu_item_disabled(popup, FileMenuItem.OPEN, busy or workspace == null or not workspace.can_open())
	_set_file_menu_item_disabled(popup, FileMenuItem.SAVE, busy or workspace == null or not workspace.can_save())
	_set_file_menu_item_disabled(popup, FileMenuItem.SAVE_AS, busy or workspace == null or not workspace.can_save_as())
	_set_file_menu_item_disabled(popup, FileMenuItem.EXPORT, busy or workspace == null or not workspace.can_export())


func _set_file_menu_item_disabled(popup: PopupMenu, id: int, disabled: bool) -> void:
	var index := popup.get_item_index(id)
	if index >= 0:
		popup.set_item_disabled(index, disabled)


func _apply_busy_modulation(active: bool) -> void:
	var alpha := 0.55 if active else 1.0
	var color := Color(1.0, 1.0, 1.0, alpha)
	_left_lane.modulate = color
	_asset_dock.modulate = color
	_status_bar.modulate = color


func _rebuild_workflow_rail(workflows: Array) -> void:
	for child in _mode_rail.get_children():
		child.queue_free()
	_workflow_buttons.clear()
	for workflow in workflows:
		var workflow_id: int = int(workflow.get("id", -1))
		var btn := Button.new()
		btn.text = String(workflow.get("label", "Workflow"))
		btn.toggle_mode = true
		btn.focus_mode = Control.FOCUS_NONE
		btn.custom_minimum_size = Vector2(0, 44)
		btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		btn.tooltip_text = String(workflow.get("tooltip", ""))
		btn.pressed.connect(_on_workflow_pressed.bind(workflow_id))
		_mode_rail.add_child(btn)
		_workflow_buttons[workflow_id] = btn


func _refresh_workflow_from_workspace() -> void:
	var workspace = _get_active_workspace()
	if workspace == null or workspace.get_workflows().is_empty():
		return
	_set_workflow(workspace.get_active_workflow_id(), false)


func _on_workflow_pressed(workflow_id: int) -> void:
	_set_workflow(workflow_id, true)


func _set_workflow(workflow_id: int, activate: bool) -> void:
	var workspace = _get_active_workspace()
	if workspace == null or not _workflow_buttons.has(workflow_id):
		return
	if activate:
		workspace.activate_workflow(workflow_id)
	if workflow_id == _current_workflow_id:
		return
	_current_workflow_id = workflow_id
	for id in _workflow_buttons:
		var button: Button = _workflow_buttons[id]
		button.set_pressed_no_signal(id == workflow_id)
	_swap_workflow_inspector(workspace, workflow_id)
	workflow_changed.emit(workflow_id)


func _swap_workflow_inspector(workspace: Variant, workflow_id: int) -> void:
	for child in _inspector_host.get_children():
		child.queue_free()
	if workspace != null:
		workspace.build_workflow_inspector(workflow_id, _inspector_host)
	if _inspector_host.get_child_count() > 0:
		return
	var placeholder := Label.new()
	placeholder.text = "Workflow inspector coming soon"
	placeholder.theme_type_variation = &"Muted"
	placeholder.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	placeholder.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	placeholder.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	placeholder.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_inspector_host.add_child(placeholder)


func _refresh_project_label() -> void:
	var workspace = _get_active_workspace()
	if workspace == null:
		_project_label.text = "OpenNova Terrain Editor"
		return
	_project_label.text = workspace.get_project_title()


func show_status_message(text: String, duration: float = 4.0) -> void:
	_message_text = text
	_message_until = Time.get_ticks_msec() / 1000.0 + duration


func _refresh_status() -> void:
	var workspace = _get_active_workspace()
	if workspace == null:
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
		_status_tool_label.text = workspace.get_status_tool()
		_status_tool_label.theme_type_variation = &""

	_status_context_label.text = workspace.get_status_context()
	if _is_terrain_workspace_active() and editor and editor.camera:
		var pos: Vector3 = editor.camera.global_position
		_status_camera_label.text = "%.0f, %.0f, %.0f" % [pos.x, pos.y, pos.z]
	else:
		_status_camera_label.text = ""
	_status_fps_label.text = "%d fps" % Engine.get_frames_per_second()
	_refresh_project_label()


func _refresh_tile_gizmo() -> void:
	if not is_node_ready() or _tile_gizmo == null or _viewport_lane == null or _tile_gizmo_label == null:
		return
	_tile_gizmo.visible = false
	if not _is_terrain_workspace_active() or editor == null or _current_workflow_id != TerrainWorkspaceAdapter.Workflow.STAMP or not editor.has_selected_tileinfo_entry():
		return

	var camera: Camera3D = editor.get_editor_camera()
	if camera == null:
		return

	var entry: Variant = editor.get_selected_tileinfo_entry()
	if entry == null:
		return

	var anchor_world: Vector3 = editor.get_selected_tileinfo_world_center() + Vector3(0.0, 2.0, 0.0)
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
		var screen_pos: Vector2 = camera.unproject_position(anchor_world)
		target = Vector2(
			screen_pos.x - gizmo_size.x * 0.5,
			screen_pos.y - gizmo_size.y - 24.0
		)
	target.x = clampf(target.x, lane_rect.position.x + 12.0, lane_end.x - gizmo_size.x - 12.0)
	target.y = clampf(target.y, lane_rect.position.y + 12.0, lane_end.y - gizmo_size.y - 12.0)
	_tile_gizmo.global_position = target
	_tile_gizmo.visible = true


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
	_select_export_flavor(TerrainWorkspaceAdapter.ExportFlavor.DFX_JO)
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
	var workspace = _get_active_workspace()
	if workspace == null:
		return
	var flavor := _current_export_flavor()
	var err: Error = workspace.begin_export(_pending_export_dir, flavor)
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
	_prompt_format_bhd.button_pressed = flavor == TerrainWorkspaceAdapter.ExportFlavor.BHD
	_prompt_format_cdep.button_pressed = flavor == TerrainWorkspaceAdapter.ExportFlavor.DFX_JO


func _select_export_flavor(flavor: int) -> void:
	_prompt_format_bhd.button_pressed = flavor == TerrainWorkspaceAdapter.ExportFlavor.BHD
	_prompt_format_cdep.button_pressed = flavor == TerrainWorkspaceAdapter.ExportFlavor.DFX_JO


func _current_export_flavor() -> int:
	return TerrainWorkspaceAdapter.ExportFlavor.BHD if _prompt_format_bhd.button_pressed else TerrainWorkspaceAdapter.ExportFlavor.DFX_JO


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
	var workspace = _get_active_workspace()
	if workspace == null:
		_set_export_ui_active(false)
		return

	var export_running: bool = workspace.is_busy()
	if export_running != _export_ui_active:
		_set_export_ui_active(export_running)
	if not export_running:
		return

	var phase: String = workspace.get_export_progress_phase()
	var message: String = workspace.get_export_progress_message()
	var current: int = workspace.get_export_progress_current()
	var total: int = workspace.get_export_progress_total()
	var ratio: float = clampf(workspace.get_export_progress_ratio(), 0.0, 1.0)

	_progress_title_label.text = workspace.get_export_progress_title()
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
