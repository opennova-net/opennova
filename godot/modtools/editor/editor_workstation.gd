class_name EditorWorkstation
extends Control

const TerrainWorkspaceAdapter = preload("res://modtools/editor/terrain_workspace.gd")
const EnvironmentWorkspaceAdapter = preload("res://modtools/editor/environment_workspace.gd")
const MissionWorkspaceAdapter = preload("res://modtools/editor/mission_workspace.gd")
const CameraSettingsPanelScene = preload("res://modtools/terrain/ui/camera_settings_panel.tscn")

enum Workspace { TERRAIN, ENVIRONMENT, MISSION }

const STATE_CONFIG_PATH := "user://terrain_editor_state.cfg"
const RESOURCE_STATE_SECTION := "resources"
const RESOURCE_DIR_KEY := "resource_dir"
const RESOURCE_RECURSIVE_KEY := "resource_recursive"

const WORKSPACE_LABELS := {
	Workspace.TERRAIN: "Terrain",
	Workspace.ENVIRONMENT: "Environment",
	Workspace.MISSION: "Mission",
}

const SELECTABLE_WORKSPACES := [Workspace.TERRAIN, Workspace.MISSION]

enum PromptKind { NONE, UNSAVED, EXPORT, CDEP }

signal workflow_changed(workflow_id: int)

enum WorkspaceAction { NEW, OPEN, SAVE, SAVE_AS, EXPORT }

@onready var _project_label: Label = %ProjectLabel
@onready var _left_lane: PanelContainer = %LeftLane
@onready var _workspace_rail: HBoxContainer = %WorkspaceRail
@onready var _workspace_actions_host: VBoxContainer = %WorkspaceActionsHost
@onready var _modes_label: Label = %ModesLabel
@onready var _mode_rail: VBoxContainer = %ModeRail
@onready var _inspector_host: Control = %InspectorHost
@onready var _viewport_lane: Control = %ViewportLane
@onready var _viewport_host: Control = %ViewportHost
@onready var _camera_toggle_button: Button = %CameraToggleButton
@onready var _camera_popup: PanelContainer = %CameraPopup
@onready var _camera_popup_close: Button = %CameraPopupClose
@onready var _camera_settings_host: Control = %CameraSettingsHost
@onready var _environment_toggle_button: Button = %EnvironmentToggleButton
@onready var _environment_popup: PanelContainer = %EnvironmentPopup
@onready var _environment_popup_title: Label = %EnvironmentPopupTitle
@onready var _environment_popup_close: Button = %EnvironmentPopupClose
@onready var _environment_actions_host: VBoxContainer = %EnvironmentActionsHost
@onready var _environment_inspector_host: Control = %EnvironmentInspectorHost
@onready var _settings_toggle_button: Button = %SettingsToggleButton
@onready var _settings_popup: PanelContainer = %SettingsPopup
@onready var _settings_popup_close: Button = %SettingsPopupClose
@onready var _settings_resource_dir_edit: LineEdit = %SettingsResourceDirEdit
@onready var _settings_browse_resource_dir_button: Button = %SettingsBrowseResourceDirButton
@onready var _settings_apply_resource_dir_button: Button = %SettingsApplyResourceDirButton
@onready var _settings_recursive_toggle: CheckBox = %SettingsRecursiveToggle
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
var _environment_workspace
var _workspace_buttons: Dictionary = {}
var _workspace_action_buttons: Dictionary = {}
var _environment_action_buttons: Dictionary = {}
var _camera_settings_panel: Control
var _resource_index: RefCounted
var _resource_root_dir: String = ""
var _resource_recursive: bool = true
var _mounted_workspace_id: int = -1
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
var _resource_browser_dialog: ConfirmationDialog
var _resource_browser_directory_label: Label
var _resource_browser_settings_button: Button
var _resource_browser_search: LineEdit
var _resource_browser_hint: Label
var _resource_browser_list: ItemList
var _resource_browser_browse_button: Button
var _resource_browser_open_button: Button
var _resource_browser_kind: String = ""
var _resource_browser_title: String = ""
var _resource_browser_filters: PackedStringArray = PackedStringArray()
var _resource_browser_current_dir: String = ""
var _resource_browser_entries: Array = []
var _resource_browser_visible_entries: Array = []
var _resource_browser_open_action: Callable = Callable()


func _ready() -> void:
	_ensure_workspaces()
	_ensure_resource_index()
	_load_resource_state()
	_build_workspace_rail()
	_wire_camera_popup()
	_wire_environment_popup()
	_wire_settings_popup()
	_wire_prompts()
	_wire_tile_gizmo()
	_project_label.clip_text = true
	_status_context_label.clip_text = true
	_status_camera_label.clip_text = true
	_status_fps_label.clip_text = true
	set_process(true)
	if not _resource_root_dir.is_empty():
		_scan_resource_root(false)
	_mount_active_workspace_viewport()
	_refresh_workspace_surface()
	sync_from_editor_state()


func _exit_tree() -> void:
	for workspace in _workspaces.values():
		if workspace != null and workspace.has_method("release_viewport"):
			workspace.release_viewport()
	_clear_viewport_host()
	_mounted_workspace_id = -1


func set_editor(value: Node) -> void:
	editor = value
	_ensure_workspaces()
	var terrain_workspace = _workspaces.get(Workspace.TERRAIN)
	if terrain_workspace != null and terrain_workspace.has_method("set_terrain_editor"):
		terrain_workspace.set_terrain_editor(value)
	var mission_workspace = _workspaces.get(Workspace.MISSION)
	if mission_workspace != null and mission_workspace.has_method("set_terrain_editor"):
		mission_workspace.set_terrain_editor(value)
	if _environment_workspace != null and _environment_workspace.has_method("set_environment_editor") and value != null and value.has_method("get_environment_editor"):
		_environment_workspace.set_environment_editor(value.get_environment_editor())
	_reset_environment_popup_content()
	_sync_camera_popup_editor()
	_remount_active_workspace_viewport()
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
	_refresh_shell_state()
	_refresh_status()
	_refresh_tile_gizmo()
	_sync_export_progress()
	_refresh_camera_popup_state()
	_refresh_environment_popup_state()
	var workspace = _get_active_workspace()
	if workspace != null:
		workspace.sync_asset_dock()


func _process(_delta: float) -> void:
	_refresh_shell_state()
	_sync_export_progress()
	_refresh_status()
	_refresh_tile_gizmo()


func _ensure_workspaces() -> void:
	if _workspaces.is_empty():
		_workspaces[Workspace.TERRAIN] = TerrainWorkspaceAdapter.new(editor)
		_workspaces[Workspace.MISSION] = MissionWorkspaceAdapter.new(editor)
	if _environment_workspace == null:
		var environment_editor: Variant = editor.get_environment_editor() if editor != null and editor.has_method("get_environment_editor") else null
		_environment_workspace = EnvironmentWorkspaceAdapter.new(environment_editor)
	for workspace in _workspaces.values():
		if workspace != null and workspace.has_method("set_editor_shell"):
			workspace.set_editor_shell(self)
	if _environment_workspace != null and _environment_workspace.has_method("set_editor_shell"):
		_environment_workspace.set_editor_shell(self)


func _build_workspace_rail() -> void:
	for workspace_id in SELECTABLE_WORKSPACES:
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


func _rebuild_workspace_actions(workspace: Variant) -> void:
	for child in _workspace_actions_host.get_children():
		_workspace_actions_host.remove_child(child)
		child.free()
	_workspace_action_buttons.clear()

	if workspace == null:
		_workspace_actions_host.visible = false
		return

	var action_defs := [
		{"id": WorkspaceAction.NEW, "visible": workspace.has_new_action(), "label": workspace.get_new_action_label()},
		{"id": WorkspaceAction.OPEN, "visible": workspace.has_open_action(), "label": workspace.get_open_action_label()},
		{"id": WorkspaceAction.SAVE, "visible": workspace.has_save_action(), "label": workspace.get_save_action_label()},
		{"id": WorkspaceAction.SAVE_AS, "visible": workspace.has_save_as_action(), "label": workspace.get_save_as_action_label()},
		{"id": WorkspaceAction.EXPORT, "visible": workspace.has_export_action(), "label": workspace.get_export_action_label()},
	]
	for action_def in action_defs:
		if not bool(action_def["visible"]):
			continue
		var btn := Button.new()
		var action_id := int(action_def["id"])
		btn.name = _workspace_action_button_name(action_id)
		btn.text = String(action_def["label"])
		btn.focus_mode = Control.FOCUS_NONE
		btn.custom_minimum_size = Vector2(0, 34)
		btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		btn.pressed.connect(_on_workspace_action_pressed.bind(action_id))
		_workspace_actions_host.add_child(btn)
		_workspace_action_buttons[action_id] = btn

	_workspace_actions_host.visible = not _workspace_action_buttons.is_empty()
	_refresh_workspace_actions_state()


func _workspace_action_button_name(action_id: int) -> String:
	match action_id:
		WorkspaceAction.NEW:
			return "NewActionButton"
		WorkspaceAction.OPEN:
			return "OpenActionButton"
		WorkspaceAction.SAVE:
			return "SaveActionButton"
		WorkspaceAction.SAVE_AS:
			return "SaveAsActionButton"
		WorkspaceAction.EXPORT:
			return "ExportActionButton"
		_:
			return "WorkspaceActionButton"


func _refresh_workspace_actions_state() -> void:
	if _workspace_actions_host == null:
		return
	var workspace = _get_active_workspace()
	var busy := _any_workspace_busy()
	for action_id in _workspace_action_buttons:
		var btn := _workspace_action_buttons[action_id] as Button
		if btn == null:
			continue
		match int(action_id):
			WorkspaceAction.NEW:
				btn.disabled = busy or workspace == null or not workspace.can_new()
			WorkspaceAction.OPEN:
				btn.disabled = busy or workspace == null or not workspace.can_open()
			WorkspaceAction.SAVE:
				btn.disabled = busy or workspace == null or not workspace.can_save()
			WorkspaceAction.SAVE_AS:
				btn.disabled = busy or workspace == null or not workspace.can_save_as()
			WorkspaceAction.EXPORT:
				btn.disabled = busy or workspace == null or not workspace.can_export()


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
	if workspace_id == Workspace.ENVIRONMENT:
		_set_environment_popup_visible(true)
		_refresh_workspace_buttons()
		return
	if not _workspaces.has(workspace_id) or workspace_id == _active_workspace_id:
		_refresh_workspace_buttons()
		return
	var current_workspace = _get_active_workspace()
	if current_workspace != null and current_workspace.has_method("deactivate"):
		current_workspace.deactivate()
	_unmount_workspace_viewport(_active_workspace_id, current_workspace)
	_active_workspace_id = workspace_id
	var next_workspace = _get_active_workspace()
	_mount_active_workspace_viewport()
	if next_workspace != null and next_workspace.has_method("activate"):
		next_workspace.activate()
	_refresh_workspace_surface()
	sync_from_editor_state()


func get_active_workspace_id() -> int:
	return _active_workspace_id


func _get_active_workspace() -> Variant:
	return _workspaces.get(_active_workspace_id)


func _mount_active_workspace_viewport() -> void:
	if _viewport_host == null:
		return
	_clear_viewport_host()
	var workspace = _get_active_workspace()
	if workspace == null:
		_mounted_workspace_id = -1
		return
	if workspace.has_method("mount_viewport"):
		workspace.mount_viewport(_viewport_host)
	_mounted_workspace_id = _active_workspace_id


func _unmount_workspace_viewport(workspace_id: int, workspace: Variant) -> void:
	if _viewport_host == null:
		return
	if workspace != null and workspace.has_method("unmount_viewport"):
		workspace.unmount_viewport(_viewport_host)
	_clear_viewport_host()
	if _mounted_workspace_id == workspace_id:
		_mounted_workspace_id = -1


func _remount_active_workspace_viewport() -> void:
	var workspace = _get_active_workspace()
	_unmount_workspace_viewport(_active_workspace_id, workspace)
	_mount_active_workspace_viewport()


func _clear_viewport_host() -> void:
	if _viewport_host == null:
		return
	for child in _viewport_host.get_children():
		_viewport_host.remove_child(child)


func _is_terrain_workspace_active() -> bool:
	return _active_workspace_id == Workspace.TERRAIN


func _any_workspace_busy() -> bool:
	for workspace in _workspaces.values():
		if workspace != null and workspace.has_method("is_busy") and workspace.is_busy():
			return true
	if _environment_workspace != null and _environment_workspace.has_method("is_busy") and _environment_workspace.is_busy():
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
	_rebuild_workspace_actions(workspace)
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


func _wire_camera_popup() -> void:
	if _camera_popup != null:
		_camera_popup.visible = false
	if _camera_toggle_button != null and not _camera_toggle_button.toggled.is_connected(_on_camera_toggle_toggled):
		_camera_toggle_button.icon = _build_camera_icon()
		_camera_toggle_button.toggled.connect(_on_camera_toggle_toggled)
	if _camera_popup_close != null and not _camera_popup_close.pressed.is_connected(_on_camera_popup_close_pressed):
		_camera_popup_close.pressed.connect(_on_camera_popup_close_pressed)
	_refresh_camera_popup_state()


func _wire_environment_popup() -> void:
	if _environment_popup != null:
		_environment_popup.visible = false
	if _environment_toggle_button != null and not _environment_toggle_button.toggled.is_connected(_on_environment_toggle_toggled):
		_environment_toggle_button.icon = _build_sun_icon()
		_environment_toggle_button.toggled.connect(_on_environment_toggle_toggled)
	if _environment_popup_close != null and not _environment_popup_close.pressed.is_connected(_on_environment_popup_close_pressed):
		_environment_popup_close.pressed.connect(_on_environment_popup_close_pressed)
	_refresh_environment_popup_state()


func _wire_settings_popup() -> void:
	if _settings_popup != null:
		_settings_popup.visible = false
	if _settings_toggle_button != null and not _settings_toggle_button.toggled.is_connected(_on_settings_toggle_toggled):
		_settings_toggle_button.icon = _build_settings_icon()
		_settings_toggle_button.toggled.connect(_on_settings_toggle_toggled)
	if _settings_popup_close != null and not _settings_popup_close.pressed.is_connected(_on_settings_popup_close_pressed):
		_settings_popup_close.pressed.connect(_on_settings_popup_close_pressed)
	if _settings_browse_resource_dir_button != null and not _settings_browse_resource_dir_button.pressed.is_connected(_on_settings_browse_resource_dir_pressed):
		_settings_browse_resource_dir_button.pressed.connect(_on_settings_browse_resource_dir_pressed)
	if _settings_apply_resource_dir_button != null and not _settings_apply_resource_dir_button.pressed.is_connected(_on_settings_apply_resource_dir_pressed):
		_settings_apply_resource_dir_button.pressed.connect(_on_settings_apply_resource_dir_pressed)
	if _settings_resource_dir_edit != null and not _settings_resource_dir_edit.text_submitted.is_connected(_on_settings_resource_dir_submitted):
		_settings_resource_dir_edit.text_submitted.connect(_on_settings_resource_dir_submitted)
	if _settings_recursive_toggle != null and not _settings_recursive_toggle.toggled.is_connected(_on_settings_recursive_toggled):
		_settings_recursive_toggle.toggled.connect(_on_settings_recursive_toggled)
	_sync_settings_popup_state()


func _build_camera_icon() -> Texture2D:
	var image := Image.create(20, 20, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.0, 0.0, 0.0, 0.0))
	var color := Color(0.8941, 0.8941, 0.9059, 1.0)
	var center := Vector2(9.5, 10.5)
	for y in 20:
		for x in 20:
			var body := x >= 4 and x <= 14 and y >= 7 and y <= 14
			var top := x >= 6 and x <= 11 and y >= 5 and y <= 7
			var side := x >= 15 and x <= 17 and y >= 8 and y <= 12
			var lens := Vector2(float(x), float(y)).distance_to(center)
			if body or top or side or lens <= 2.2:
				image.set_pixel(x, y, color)
	return ImageTexture.create_from_image(image)


func _build_sun_icon() -> Texture2D:
	var image := Image.create(20, 20, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.0, 0.0, 0.0, 0.0))
	var color := Color(0.8941, 0.8941, 0.9059, 1.0)
	var center := Vector2(9.5, 9.5)
	for y in 20:
		for x in 20:
			var p := Vector2(float(x), float(y))
			var d := p.distance_to(center)
			var cardinal_ray := (absf(p.x - center.x) < 0.75 and (p.y < 4.0 or p.y > 15.0)) or (absf(p.y - center.y) < 0.75 and (p.x < 4.0 or p.x > 15.0))
			var diag_a := absf((p.x - center.x) - (p.y - center.y)) < 0.75 and d > 6.2 and d < 9.5
			var diag_b := absf((p.x - center.x) + (p.y - center.y)) < 0.75 and d > 6.2 and d < 9.5
			if d <= 3.0 or (d >= 4.15 and d <= 5.1) or cardinal_ray or diag_a or diag_b:
				image.set_pixel(x, y, color)
	return ImageTexture.create_from_image(image)


func _build_settings_icon() -> Texture2D:
	var image := Image.create(20, 20, false, Image.FORMAT_RGBA8)
	image.fill(Color(0.0, 0.0, 0.0, 0.0))
	var color := Color(0.8941, 0.8941, 0.9059, 1.0)
	var center := Vector2(9.5, 9.5)
	for y in 20:
		for x in 20:
			var p := Vector2(float(x), float(y))
			var d := p.distance_to(center)
			var ring := d >= 4.2 and d <= 6.0
			var hub := d <= 2.0
			var tooth_horizontal := y >= 8 and y <= 11 and (x <= 4 or x >= 15)
			var tooth_vertical := x >= 8 and x <= 11 and (y <= 4 or y >= 15)
			var diag_a := absf((p.x - center.x) - (p.y - center.y)) < 1.0 and d >= 6.0 and d <= 8.4
			var diag_b := absf((p.x - center.x) + (p.y - center.y)) < 1.0 and d >= 6.0 and d <= 8.4
			if hub or ring or tooth_horizontal or tooth_vertical or diag_a or diag_b:
				image.set_pixel(x, y, color)
	return ImageTexture.create_from_image(image)


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey:
		var key := event as InputEventKey
		if key.pressed and not key.is_echo() and key.keycode == KEY_ESCAPE:
			if _camera_popup != null and _camera_popup.visible:
				_set_camera_popup_visible(false)
				get_viewport().set_input_as_handled()
				return
			if _environment_popup != null and _environment_popup.visible:
				_set_environment_popup_visible(false)
				get_viewport().set_input_as_handled()
				return
			if _settings_popup != null and _settings_popup.visible:
				_set_settings_popup_visible(false)
				get_viewport().set_input_as_handled()


func _on_camera_toggle_toggled(pressed: bool) -> void:
	_set_camera_popup_visible(pressed)


func _on_camera_popup_close_pressed() -> void:
	_set_camera_popup_visible(false)


func _set_camera_popup_visible(active: bool) -> void:
	if _camera_popup == null:
		return
	if active and _get_editor_camera() == null:
		active = false
	if active:
		_set_environment_popup_visible(false)
		_set_settings_popup_visible(false)
	_camera_popup.visible = active
	if _camera_toggle_button != null:
		_camera_toggle_button.set_pressed_no_signal(active)
	if active:
		_ensure_camera_popup_content()
	_refresh_camera_popup_state()


func _ensure_camera_popup_content() -> void:
	if _camera_settings_host == null:
		return
	if _camera_settings_panel == null:
		_camera_settings_panel = CameraSettingsPanelScene.instantiate() as Control
		_camera_settings_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_camera_settings_host.add_child(_camera_settings_panel)
	_sync_camera_popup_editor()


func _sync_camera_popup_editor() -> void:
	if _camera_settings_panel != null and _camera_settings_panel.has_method("set_editor"):
		_camera_settings_panel.set_editor(editor)


func _refresh_camera_popup_state() -> void:
	var has_camera := _get_editor_camera() != null
	if _camera_toggle_button != null:
		_camera_toggle_button.disabled = not has_camera
		if not has_camera:
			_camera_toggle_button.set_pressed_no_signal(false)
	if _camera_popup != null and _camera_popup.visible and not has_camera:
		_camera_popup.visible = false
	if _camera_popup != null and _camera_popup.visible:
		_ensure_camera_popup_content()
		if _camera_settings_panel != null and _camera_settings_panel.has_method("sync_from_editor_state"):
			_camera_settings_panel.sync_from_editor_state()


func _get_editor_camera() -> Camera3D:
	if editor == null:
		return null
	return editor.get("camera") as Camera3D


func _on_environment_toggle_toggled(pressed: bool) -> void:
	_set_environment_popup_visible(pressed)


func _on_environment_popup_close_pressed() -> void:
	_set_environment_popup_visible(false)


func _set_environment_popup_visible(active: bool) -> void:
	if _environment_popup == null:
		return
	if active:
		_set_camera_popup_visible(false)
		_set_settings_popup_visible(false)
	_environment_popup.visible = active
	if _environment_toggle_button != null:
		_environment_toggle_button.set_pressed_no_signal(active)
	if active:
		_ensure_environment_popup_content()
	_refresh_environment_popup_state()


func _reset_environment_popup_content() -> void:
	_environment_action_buttons.clear()
	if _environment_actions_host != null:
		for child in _environment_actions_host.get_children():
			_environment_actions_host.remove_child(child)
			child.free()
	if _environment_inspector_host != null:
		for child in _environment_inspector_host.get_children():
			_environment_inspector_host.remove_child(child)
			child.free()


func _ensure_environment_popup_content() -> void:
	if _environment_workspace == null:
		return
	if _environment_actions_host != null and _environment_action_buttons.is_empty():
		var action_defs := [
			{"id": WorkspaceAction.NEW, "visible": _environment_workspace.has_new_action(), "label": _environment_workspace.get_new_action_label()},
			{"id": WorkspaceAction.OPEN, "visible": _environment_workspace.has_open_action(), "label": _environment_workspace.get_open_action_label()},
			{"id": WorkspaceAction.SAVE, "visible": _environment_workspace.has_save_action(), "label": _environment_workspace.get_save_action_label()},
			{"id": WorkspaceAction.SAVE_AS, "visible": _environment_workspace.has_save_as_action(), "label": _environment_workspace.get_save_as_action_label()},
			{"id": WorkspaceAction.EXPORT, "visible": _environment_workspace.has_export_action(), "label": _environment_workspace.get_export_action_label()},
		]
		for action_def in action_defs:
			if not bool(action_def["visible"]):
				continue
			var btn := Button.new()
			var action_id := int(action_def["id"])
			btn.name = "Environment" + _workspace_action_button_name(action_id)
			btn.text = String(action_def["label"])
			btn.focus_mode = Control.FOCUS_NONE
			btn.custom_minimum_size = Vector2(0, 32)
			btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
			btn.pressed.connect(_on_environment_action_pressed.bind(action_id))
			_environment_actions_host.add_child(btn)
			_environment_action_buttons[action_id] = btn
	if _environment_inspector_host != null and _environment_inspector_host.get_child_count() == 0:
		_environment_workspace.build_inspector(_environment_inspector_host)


func _refresh_environment_popup_state() -> void:
	if _environment_popup_title != null:
		_environment_popup_title.text = _environment_workspace.get_project_title() if _environment_workspace != null else "Environment"
	var busy := _any_workspace_busy()
	for action_id in _environment_action_buttons:
		var btn := _environment_action_buttons[action_id] as Button
		if btn == null:
			continue
		match int(action_id):
			WorkspaceAction.NEW:
				btn.disabled = busy or _environment_workspace == null or not _environment_workspace.can_new()
			WorkspaceAction.OPEN:
				btn.disabled = busy or _environment_workspace == null or not _environment_workspace.can_open()
			WorkspaceAction.SAVE:
				btn.disabled = busy or _environment_workspace == null or not _environment_workspace.can_save()
			WorkspaceAction.SAVE_AS:
				btn.disabled = busy or _environment_workspace == null or not _environment_workspace.can_save_as()
			WorkspaceAction.EXPORT:
				btn.disabled = busy or _environment_workspace == null or not _environment_workspace.can_export()


func _on_settings_toggle_toggled(pressed: bool) -> void:
	_set_settings_popup_visible(pressed)


func _on_settings_popup_close_pressed() -> void:
	_set_settings_popup_visible(false)


func _set_settings_popup_visible(active: bool) -> void:
	if _settings_popup == null:
		return
	if active:
		_set_camera_popup_visible(false)
		_set_environment_popup_visible(false)
	_sync_settings_popup_state()
	_settings_popup.visible = active
	if _settings_toggle_button != null:
		_settings_toggle_button.set_pressed_no_signal(active)


func _sync_settings_popup_state() -> void:
	if _settings_resource_dir_edit != null:
		_settings_resource_dir_edit.text = _resource_root_dir
	if _settings_recursive_toggle != null:
		_settings_recursive_toggle.set_pressed_no_signal(_resource_recursive)


func _on_settings_browse_resource_dir_pressed() -> void:
	var on_pick := func(path: String) -> void:
		if _settings_resource_dir_edit != null:
			_settings_resource_dir_edit.text = path
		_apply_resource_settings(true)
	_open_dir_dialog("Select resource directory", on_pick, _preferred_resource_root_dir())


func _on_settings_apply_resource_dir_pressed() -> void:
	_apply_resource_settings(true)


func _on_settings_resource_dir_submitted(_text: String) -> void:
	_apply_resource_settings(true)


func _on_settings_recursive_toggled(pressed: bool) -> void:
	if _resource_recursive == pressed:
		return
	_resource_recursive = pressed
	_save_resource_state()
	if not _resource_root_dir.is_empty():
		_scan_resource_root(true)


func _apply_resource_settings(scan: bool, persist: bool = true) -> Error:
	var path := _resource_root_dir
	if _settings_resource_dir_edit != null:
		path = _settings_resource_dir_edit.text
	if _settings_recursive_toggle != null:
		_resource_recursive = _settings_recursive_toggle.button_pressed
	return _set_resource_root_dir(path, persist, scan)


func _ensure_resource_index() -> void:
	if _resource_index == null:
		_resource_index = NovaResourceIndex.new()


func get_resource_index() -> RefCounted:
	_ensure_resource_index()
	return _resource_index


func get_resource_root_dir() -> String:
	return _resource_root_dir


func is_resource_recursive() -> bool:
	return _resource_recursive


func set_resource_root_dir(path: String) -> void:
	_set_resource_root_dir(path, true, true)


func _set_resource_root_dir(path: String, persist: bool, scan: bool) -> Error:
	_ensure_resource_index()
	var previous := _resource_root_dir
	_resource_root_dir = path.strip_edges()
	if persist:
		_save_resource_state()
	if _resource_root_dir.is_empty():
		_resource_index.clear()
		_sync_settings_popup_state()
		return OK
	if scan:
		return _scan_resource_root(true)
	if previous != _resource_root_dir:
		_resource_index.clear()
	_sync_settings_popup_state()
	return OK


func _scan_resource_root(show_message: bool) -> Error:
	_ensure_resource_index()
	if _resource_root_dir.is_empty():
		_resource_index.clear()
		return OK
	var err: Error = _resource_index.scan(_resource_root_dir, _resource_recursive)
	if err == OK:
		if show_message:
			show_status_message("Resource directory indexed.", 4.0)
	else:
		var detail := ""
		if _resource_index.has_method("get_last_error"):
			detail = String(_resource_index.get_last_error())
		if show_message:
			show_status_message("Resource scan failed." if detail.is_empty() else detail, 6.0)
	_sync_settings_popup_state()
	return err


func _load_resource_state() -> void:
	var config := ConfigFile.new()
	if config.load(STATE_CONFIG_PATH) != OK:
		return
	_resource_root_dir = String(config.get_value(RESOURCE_STATE_SECTION, RESOURCE_DIR_KEY, ""))
	_resource_recursive = bool(config.get_value(RESOURCE_STATE_SECTION, RESOURCE_RECURSIVE_KEY, true))


func _save_resource_state() -> void:
	var config := ConfigFile.new()
	config.load(STATE_CONFIG_PATH)
	config.set_value(RESOURCE_STATE_SECTION, RESOURCE_DIR_KEY, _resource_root_dir)
	config.set_value(RESOURCE_STATE_SECTION, RESOURCE_RECURSIVE_KEY, _resource_recursive)
	config.save(STATE_CONFIG_PATH)


func _preferred_resource_root_dir() -> String:
	if not _resource_root_dir.is_empty():
		return _resource_root_dir
	if editor != null:
		if editor.has_current_project_dir():
			return editor.get_current_project_dir()
		if not editor.get_last_open_dir().is_empty():
			return editor.get_last_open_dir()
	return ""


func _on_workspace_action_pressed(action_id: int) -> void:
	_run_workspace_action(_get_active_workspace(), action_id)


func _on_environment_action_pressed(action_id: int) -> void:
	_run_workspace_action(_environment_workspace, action_id)
	_refresh_environment_popup_state()


func _run_workspace_action(workspace: Variant, action_id: int) -> void:
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
	match action_id:
		WorkspaceAction.NEW:
			if workspace.can_new():
				workspace.new_current()
		WorkspaceAction.OPEN:
			if not workspace.can_open():
				return
			_open_resource_browser(workspace, open_trn)
		WorkspaceAction.SAVE:
			_on_save_pressed(workspace)
		WorkspaceAction.SAVE_AS:
			_open_dir_dialog(
				workspace.get_save_dialog_title(),
				save_project_as,
				_preferred_save_dir(workspace)
			)
		WorkspaceAction.EXPORT:
			_on_export_pressed(workspace)


func _open_resource_browser(workspace: Variant, on_pick: Callable) -> void:
	if workspace == null:
		return
	var kind := String(workspace.get_open_resource_kind()).strip_edges()
	if kind.is_empty():
		_open_file_dialog(
			workspace.get_open_dialog_title(),
			workspace.get_open_dialog_filters(),
			on_pick,
			workspace.get_open_dialog_dir()
		)
		return
	_ensure_resource_browser_dialog()
	_resource_browser_kind = kind
	_resource_browser_title = workspace.get_open_dialog_title()
	_resource_browser_filters = workspace.get_open_dialog_filters()
	_resource_browser_current_dir = workspace.get_open_dialog_dir()
	_resource_browser_open_action = on_pick
	_resource_browser_search.text = ""
	_resource_browser_dialog.title = _resource_browser_title
	_refresh_resource_browser_entries()
	_refresh_resource_browser()
	_resource_browser_dialog.popup_centered(Vector2i(760, 520))


func _ensure_resource_browser_dialog() -> void:
	if _resource_browser_dialog != null and is_instance_valid(_resource_browser_dialog):
		return
	_resource_browser_dialog = ConfirmationDialog.new()
	_resource_browser_dialog.name = "ResourceBrowserDialog"
	_resource_browser_dialog.min_size = Vector2i(760, 520)
	_resource_browser_dialog.exclusive = true
	add_child(_resource_browser_dialog)

	var margin := MarginContainer.new()
	margin.name = "ResourceBrowserMargin"
	margin.add_theme_constant_override("margin_left", 14)
	margin.add_theme_constant_override("margin_top", 12)
	margin.add_theme_constant_override("margin_right", 14)
	margin.add_theme_constant_override("margin_bottom", 12)
	_resource_browser_dialog.add_child(margin)

	var box := VBoxContainer.new()
	box.name = "ResourceBrowserBox"
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_theme_constant_override("separation", 10)
	margin.add_child(box)

	var header := HBoxContainer.new()
	header.name = "ResourceBrowserHeader"
	header.add_theme_constant_override("separation", 8)
	box.add_child(header)

	_resource_browser_directory_label = Label.new()
	_resource_browser_directory_label.name = "ResourceBrowserDirectoryLabel"
	_resource_browser_directory_label.theme_type_variation = &"Muted"
	_resource_browser_directory_label.clip_text = true
	_resource_browser_directory_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(_resource_browser_directory_label)

	_resource_browser_settings_button = Button.new()
	_resource_browser_settings_button.name = "ResourceBrowserSettingsButton"
	_resource_browser_settings_button.text = "Settings"
	_resource_browser_settings_button.focus_mode = Control.FOCUS_NONE
	_resource_browser_settings_button.pressed.connect(_on_resource_browser_settings_pressed)
	header.add_child(_resource_browser_settings_button)

	_resource_browser_search = LineEdit.new()
	_resource_browser_search.name = "ResourceBrowserSearch"
	_resource_browser_search.placeholder_text = "Search resources"
	_resource_browser_search.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_resource_browser_search.text_changed.connect(func(_text: String) -> void:
		_refresh_resource_browser()
	)
	box.add_child(_resource_browser_search)

	_resource_browser_hint = Label.new()
	_resource_browser_hint.name = "ResourceBrowserHint"
	_resource_browser_hint.theme_type_variation = &"Muted"
	_resource_browser_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_resource_browser_hint)

	_resource_browser_list = ItemList.new()
	_resource_browser_list.name = "ResourceBrowserList"
	_resource_browser_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_resource_browser_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_resource_browser_list.item_selected.connect(_on_resource_browser_item_selected)
	_resource_browser_list.item_activated.connect(_on_resource_browser_item_activated)
	box.add_child(_resource_browser_list)

	_resource_browser_browse_button = _resource_browser_dialog.add_button("Browse Files...", false, "browse")
	_resource_browser_browse_button.name = "ResourceBrowserBrowseFilesButton"
	_resource_browser_dialog.custom_action.connect(_on_resource_browser_custom_action)
	_resource_browser_dialog.confirmed.connect(_on_resource_browser_confirmed)
	_resource_browser_open_button = _resource_browser_dialog.get_ok_button()
	_resource_browser_open_button.text = "Open"
	_resource_browser_dialog.get_cancel_button().text = "Cancel"


func _refresh_resource_browser_entries() -> void:
	_resource_browser_entries = []
	_ensure_resource_index()
	if _resource_root_dir.is_empty():
		_resource_index.clear()
		return
	if _resource_index.get_root_dir().is_empty():
		_scan_resource_root(false)
	if not _resource_index.get_root_dir().is_empty():
		_resource_browser_entries = _resource_index.get_resource_files(_resource_browser_kind)


func _refresh_resource_browser() -> void:
	if _resource_browser_list == null:
		return
	_resource_browser_list.clear()
	_resource_browser_visible_entries = []
	var search := _resource_browser_search.text.strip_edges().to_lower() if _resource_browser_search != null else ""
	for entry_value in _resource_browser_entries:
		var entry := entry_value as Dictionary
		var display_name := String(entry.get("display_name", ""))
		var relative_path := String(entry.get("relative_path", ""))
		var haystack := ("%s %s" % [display_name, relative_path]).to_lower()
		if not search.is_empty() and not haystack.contains(search):
			continue
		_resource_browser_visible_entries.append(entry)
		var text := "%s  %s" % [display_name, relative_path]
		var current_path := _current_resource_path_for_browser()
		if not current_path.is_empty() and _same_filesystem_path(String(entry.get("path", "")), current_path):
			text += "  (open)"
		var index := _resource_browser_list.add_item(text)
		_resource_browser_list.set_item_metadata(index, entry)

	var has_root := not _resource_root_dir.strip_edges().is_empty()
	var has_entries := not _resource_browser_entries.is_empty()
	var has_visible := not _resource_browser_visible_entries.is_empty()
	if _resource_browser_directory_label != null:
		_resource_browser_directory_label.text = _resource_root_dir if has_root else "No resource directory selected"
	if _resource_browser_hint != null:
		_resource_browser_hint.visible = not has_visible
		if not has_root:
			_resource_browser_hint.text = "No resource directory selected."
		elif not has_entries:
			_resource_browser_hint.text = "No %s resources found in %s." % [_resource_browser_kind_label(), _resource_root_dir]
		else:
			_resource_browser_hint.text = "No matching resources."
	if _resource_browser_settings_button != null:
		_resource_browser_settings_button.visible = not has_root or not has_entries
	if _resource_browser_open_button != null:
		_resource_browser_open_button.disabled = true


func _resource_browser_kind_label() -> String:
	match _resource_browser_kind:
		"terrain":
			return "terrain"
		"environment":
			return "environment"
		"mission":
			return "mission"
		"model":
			return "model"
		_:
			return "resource"


func _current_resource_path_for_browser() -> String:
	var workspace = _get_active_workspace()
	if _environment_popup != null and _environment_popup.visible and _resource_browser_kind == "environment":
		workspace = _environment_workspace
	if workspace != null and workspace.has_method("get_current_resource_path"):
		return workspace.get_current_resource_path()
	return ""


func _on_resource_browser_item_selected(_index: int) -> void:
	if _resource_browser_open_button != null:
		_resource_browser_open_button.disabled = false


func _on_resource_browser_item_activated(index: int) -> void:
	_resource_browser_list.select(index)
	_open_selected_resource_browser_entry()


func _on_resource_browser_confirmed() -> void:
	_open_selected_resource_browser_entry()


func _open_selected_resource_browser_entry() -> void:
	if _resource_browser_list == null:
		return
	var selected := _resource_browser_list.get_selected_items()
	if selected.size() == 0:
		return
	var entry := _resource_browser_list.get_item_metadata(selected[0]) as Dictionary
	var path := String(entry.get("path", ""))
	if path.is_empty():
		return
	if _resource_browser_open_action.is_valid():
		_resource_browser_open_action.call(path)
	_resource_browser_dialog.hide()


func _on_resource_browser_custom_action(action: StringName) -> void:
	if action == &"browse":
		_on_resource_browser_browse_files_pressed()


func _on_resource_browser_browse_files_pressed() -> void:
	if _resource_browser_dialog != null:
		_resource_browser_dialog.hide()
	_open_file_dialog(
		_resource_browser_title,
		_resource_browser_filters,
		_resource_browser_open_action,
		_resource_browser_current_dir
	)


func _on_resource_browser_settings_pressed() -> void:
	if _resource_browser_dialog != null:
		_resource_browser_dialog.hide()
	_set_settings_popup_visible(true)


func _same_filesystem_path(a: String, b: String) -> bool:
	if a.is_empty() or b.is_empty():
		return false
	var left := _globalized_path(a).replace("\\", "/").to_lower()
	var right := _globalized_path(b).replace("\\", "/").to_lower()
	return left == right


func _globalized_path(path: String) -> String:
	if path.begins_with("res://") or path.begins_with("user://"):
		return ProjectSettings.globalize_path(path)
	return path


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


func _preferred_save_dir(workspace: Variant = null) -> String:
	if workspace == null:
		workspace = _get_active_workspace()
	if workspace != null:
		var dir: String = workspace.get_save_dialog_dir()
		if not dir.is_empty():
			return dir
	if editor == null:
		return ""
	if editor.has_current_project_dir():
		return editor.get_current_project_dir()
	return editor.get_last_save_dir()


func _preferred_export_dir(workspace: Variant = null) -> String:
	if workspace == null:
		workspace = _get_active_workspace()
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


func _on_save_pressed(workspace: Variant = null) -> void:
	if workspace == null:
		workspace = _get_active_workspace()
	if workspace == null:
		return
	var err: Error = workspace.save_current()
	if err == ERR_INVALID_PARAMETER:
		var save_project_as := func(dir_path: String) -> void:
			workspace.save_as(dir_path)
		_open_dir_dialog(
			workspace.get_save_dialog_title(),
			save_project_as,
			_preferred_save_dir(workspace)
		)
	elif err != OK:
		show_status_message("%s save is not available." % workspace.get_workspace_label(), 4.0)


func _on_export_pressed(workspace: Variant = null) -> void:
	if workspace == null:
		workspace = _get_active_workspace()
	if workspace == null or not workspace.can_export():
		return
	var choose_export_dir := func(dir_path: String) -> void:
		if workspace == _get_active_workspace() and _is_terrain_workspace_active():
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
		_preferred_export_dir(workspace)
	)


func _refresh_shell_state() -> void:
	var busy := _any_workspace_busy()
	_refresh_workspace_actions_state()
	_refresh_environment_popup_state()
	for button in _workflow_buttons.values():
		var workflow_button := button as Button
		if workflow_button:
			workflow_button.disabled = busy
	_apply_busy_modulation(busy)


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
	if (_is_terrain_workspace_active() or _active_workspace_id == Workspace.MISSION) and editor and editor.camera:
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
	_refresh_shell_state()


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
