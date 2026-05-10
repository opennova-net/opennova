class_name EditorWorkstation
extends Control

const TerrainWorkspaceAdapter = preload("res://modtools/editor/terrain_workspace.gd")
const EnvironmentWorkspaceAdapter = preload("res://modtools/editor/environment_workspace.gd")
const ObjectWorkspaceAdapter = preload("res://modtools/object/object_workspace.gd")
const MissionWorkspaceAdapter = preload("res://modtools/editor/mission_workspace.gd")
const FontsWorkspaceAdapter = preload("res://modtools/editor/fonts_workspace.gd")
const CreditsWorkspaceAdapter = preload("res://modtools/editor/credits_workspace.gd")
const CameraSettingsPanelScene = preload("res://modtools/terrain/ui/camera_settings_panel.tscn")
const VegAssets = preload("res://engine/terrain/veg_assets.gd")

enum Workspace { TERRAIN, ENVIRONMENT, OBJECT, MISSION, CREDITS, FONTS }

# Workspaces are declared as WorkspaceDef rows in _workspace_defs(); the rail
# shows the non-popup ones in order. The enum below stays only as stable id
# constants and for the two genuine per-workspace branches (env popup, tile gizmo).


enum WorkspaceAction { NEW, OPEN, SAVE, SAVE_AS, EXPORT }

@onready var _project_label: Label = %ProjectLabel
@onready var _body_row: SplitContainer = %BodyRow
@onready var _center_right_split: SplitContainer = %CenterRightSplit
@onready var _left_lane: PanelContainer = %LeftLane
@onready var _workspace_rail: HBoxContainer = %WorkspaceRail
@onready var _workspace_actions_host: VBoxContainer = %WorkspaceActionsHost
@onready var _modes_label: Label = %ModesLabel
@onready var _mode_rail: VBoxContainer = %ModeRail
@onready var _inspector_host: Control = %InspectorHost
@onready var _viewport_lane: Control = %ViewportLane
@onready var _viewport_host: Control = %ViewportHost
@onready var _camera_toggle_button: Button = %CameraToggleButton
@onready var _camera_popup: PopoverPanel = %CameraPopup
@onready var _camera_popup_close: Button = %CameraPopupClose
@onready var _camera_settings_host: Control = %CameraSettingsHost
@onready var _environment_toggle_button: Button = %EnvironmentToggleButton
@onready var _environment_popup: PopoverPanel = %EnvironmentPopup
@onready var _environment_popup_title: Label = %EnvironmentPopupTitle
@onready var _environment_popup_close: Button = %EnvironmentPopupClose
@onready var _environment_actions_host: VBoxContainer = %EnvironmentActionsHost
@onready var _environment_inspector_host: Control = %EnvironmentInspectorHost
@onready var _settings_toggle_button: Button = %SettingsToggleButton
@onready var _settings_popup: PopoverPanel = %SettingsPopup
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

var editor: Node
var _active_workspace_id: int = Workspace.TERRAIN
var _workspaces: Dictionary = {}
var _workspace_defs_cache: Array = []
var _environment_workspace: EnvironmentEditorWorkspace
var _workspace_buttons: Dictionary = {}
var _workspace_action_buttons: Dictionary = {}
var _environment_action_buttons: Dictionary = {}
var _asset_dock_workspace_id: int = -1
var _camera_settings_panel: Control
var _resource_library := EditorResourceLibrary.new()
var _resource_recursive: bool = true
var _mounted_workspace_id: int = -1
var _current_workflow_id: int = -1
var _workflow_buttons: Dictionary = {}
var _inspector_workspace_id: int = -1
var _message_text: String = ""
var _message_until: float = 0.0
var _export_ui_active: bool = false
var _pending_export_dir: String = ""
var _overlay_tween: Tween
var _unsaved_dialog: ConfirmationDialog
var _cdep_dialog: ConfirmationDialog
var _export_dialog: ExportFlavorDialog
var _cdep_fix_callback: Callable = Callable()
var _resource_browser := EditorResourceBrowser.new()
var _file_dialogs: FileDialogHelper


func _ready() -> void:
	_ensure_workspaces()
	_ensure_resource_index()
	_load_resource_state()
	_resource_browser.setup(
		self,
		_resource_library,
		_open_file_dialog,
		func() -> void: _set_settings_popup_visible(true),
		_current_resource_path_for_browser,
		func() -> void: _scan_resource_root(false)
	)
	_build_workspace_rail()
	_wire_camera_popup()
	_wire_environment_popup()
	_wire_settings_popup()
	_wire_tile_gizmo()
	_wire_splits()
	_apply_window_min_size()
	_project_label.clip_text = true
	_status_context_label.clip_text = true
	_status_camera_label.clip_text = true
	_status_fps_label.clip_text = true
	set_process(true)
	if not _resource_library.get_root_dir().is_empty():
		_scan_resource_root(false)
	_mount_active_workspace_viewport()
	_refresh_workspace_surface()
	sync_from_editor_state()
	# Split offsets land after the first container sort so clamp sees real sizes.
	_apply_split_layout.call_deferred()


func _exit_tree() -> void:
	for workspace in _workspaces.values():
		(workspace as EditorWorkspace).release_viewport()
	_clear_viewport_host()
	_mounted_workspace_id = -1


func set_editor(value: Node) -> void:
	editor = value
	_ensure_workspaces()
	for workspace in _workspaces.values():
		(workspace as EditorWorkspace).bind_to_editor(value)
	if _environment_workspace != null:
		_environment_workspace.bind_to_editor(value)
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
	var workspace := _get_active_workspace()
	if workspace != null:
		workspace.sync_asset_dock()
	# Re-apply the persisted dock widths so re-showing the right dock on a
	# workspace switch restores its dragged size instead of the scene default.
	_apply_split_layout()


func _process(_delta: float) -> void:
	_refresh_shell_state()
	_sync_export_progress()
	_refresh_status()
	_refresh_tile_gizmo()


func _workspace_defs() -> Array:
	return [
		WorkspaceDef.make(Workspace.TERRAIN, TerrainWorkspaceAdapter),
		WorkspaceDef.make(Workspace.OBJECT, ObjectWorkspaceAdapter),
		WorkspaceDef.make(Workspace.MISSION, MissionWorkspaceAdapter),
		WorkspaceDef.make(Workspace.FONTS, FontsWorkspaceAdapter),
		WorkspaceDef.make(Workspace.CREDITS, CreditsWorkspaceAdapter),
		WorkspaceDef.make(Workspace.ENVIRONMENT, EnvironmentWorkspaceAdapter, true),
	]


func _ensure_workspaces() -> void:
	if _workspace_defs_cache.is_empty():
		_workspace_defs_cache = _workspace_defs()
	for def_v in _workspace_defs_cache:
		var def := def_v as WorkspaceDef
		if def.popup:
			if _environment_workspace == null:
				_environment_workspace = def.adapter_script.new()
				_environment_workspace.set_editor_shell(self)
		elif not _workspaces.has(def.id):
			var workspace: EditorWorkspace = def.adapter_script.new()
			workspace.set_editor_shell(self)
			_workspaces[def.id] = workspace


func _build_workspace_rail() -> void:
	for def_v in _workspace_defs_cache:
		var def := def_v as WorkspaceDef
		if def.popup:
			continue
		var workspace := _get_workspace(def.id)
		var btn := Button.new()
		btn.text = workspace.get_workspace_label() if workspace != null else "Workspace"
		btn.toggle_mode = true
		btn.focus_mode = Control.FOCUS_NONE
		btn.custom_minimum_size = Vector2(0, 36)
		btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		btn.tooltip_text = workspace.get_workspace_tooltip() if workspace != null else ""
		btn.pressed.connect(_on_workspace_pressed.bind(def.id))
		_workspace_rail.add_child(btn)
		_workspace_buttons[def.id] = btn
	_refresh_workspace_buttons()


func _action_defs_for_workspace(workspace: EditorWorkspace) -> Array:
	var action_defs := [
		{"id": WorkspaceAction.NEW, "visible": workspace.has_new_action(), "label": workspace.get_new_action_label()},
		{"id": WorkspaceAction.OPEN, "visible": workspace.has_open_action(), "label": workspace.get_open_action_label()},
		{"id": WorkspaceAction.SAVE, "visible": workspace.has_save_action(), "label": workspace.get_save_action_label()},
		{"id": WorkspaceAction.SAVE_AS, "visible": workspace.has_save_as_action(), "label": workspace.get_save_as_action_label()},
		{"id": WorkspaceAction.EXPORT, "visible": workspace.has_export_action(), "label": workspace.get_export_action_label()},
	]
	return action_defs


func _rebuild_action_buttons(
	workspace: EditorWorkspace,
	host: VBoxContainer,
	buttons: Dictionary,
	on_pressed: Callable,
	name_prefix: String = "",
	min_height: float = 34.0
) -> void:
	for child in host.get_children():
		host.remove_child(child)
		child.free()
	buttons.clear()

	if workspace == null:
		host.visible = false
		return

	var action_defs := _action_defs_for_workspace(workspace)
	for action_def in action_defs:
		if not bool(action_def["visible"]):
			continue
		var btn := Button.new()
		var action_id := int(action_def["id"])
		btn.name = name_prefix + _workspace_action_button_name(action_id)
		btn.text = String(action_def["label"])
		btn.focus_mode = Control.FOCUS_NONE
		btn.custom_minimum_size = Vector2(0, min_height)
		btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		btn.pressed.connect(on_pressed.bind(action_id))
		host.add_child(btn)
		buttons[action_id] = btn

	host.visible = not buttons.is_empty()
	_refresh_action_buttons_state(workspace, buttons)


func _rebuild_workspace_actions(workspace: EditorWorkspace) -> void:
	_rebuild_action_buttons(workspace, _workspace_actions_host, _workspace_action_buttons, Callable(self, "_on_workspace_action_pressed"))
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
	var workspace := _get_active_workspace()
	_refresh_action_buttons_state(workspace, _workspace_action_buttons)


func _refresh_action_buttons_state(workspace: EditorWorkspace, buttons: Dictionary) -> void:
	var busy := _any_workspace_busy()
	for action_id in buttons:
		var btn := buttons[action_id] as Button
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
	var current_workspace := _get_active_workspace()
	if current_workspace != null:
		current_workspace.deactivate()
		current_workspace.set_asset_dock(null)
	_unmount_workspace_viewport(_active_workspace_id, current_workspace)
	_active_workspace_id = workspace_id
	var next_workspace := _get_active_workspace()
	_mount_active_workspace_viewport()
	if next_workspace != null:
		next_workspace.activate()
	_refresh_workspace_surface()
	sync_from_editor_state()


func get_active_workspace_id() -> int:
	return _active_workspace_id


func open_font_workspace(font_name: String) -> Error:
	_ensure_workspaces()
	var workspace := _get_workspace(Workspace.FONTS)
	if workspace == null:
		return ERR_UNAVAILABLE
	var clean_name := font_name.strip_edges()
	if clean_name.is_empty():
		return ERR_INVALID_PARAMETER
	var run_open := func() -> Error:
		var err: Error = int(workspace.call("open_font_name", clean_name))
		if err != OK:
			show_status_message("Font not found: %s" % clean_name, 5.0)
			return err
		if _active_workspace_id != Workspace.FONTS:
			set_active_workspace(Workspace.FONTS)
		else:
			_refresh_workspace_surface()
			sync_from_editor_state()
		show_status_message("Opened font %s." % clean_name, 3.0)
		return OK
	if workspace.has_unsaved_changes():
		_prompt_unsaved_workspace_action(workspace, "open font", func() -> void:
			run_open.call()
		)
		return OK
	return run_open.call()


func _get_workspace(workspace_id: int) -> EditorWorkspace:
	return _workspaces.get(workspace_id) as EditorWorkspace


func _get_active_workspace() -> EditorWorkspace:
	return _get_workspace(_active_workspace_id)


func _mount_active_workspace_viewport() -> void:
	if _viewport_host == null:
		return
	_clear_viewport_host()
	var workspace := _get_active_workspace()
	if workspace == null:
		_mounted_workspace_id = -1
		return
	workspace.mount_viewport(_viewport_host)
	_mounted_workspace_id = _active_workspace_id


func _unmount_workspace_viewport(workspace_id: int, workspace: EditorWorkspace) -> void:
	if _viewport_host == null:
		return
	if workspace != null:
		workspace.unmount_viewport(_viewport_host)
	_clear_viewport_host()
	if _mounted_workspace_id == workspace_id:
		_mounted_workspace_id = -1


func _remount_active_workspace_viewport() -> void:
	var workspace := _get_active_workspace()
	_unmount_workspace_viewport(_active_workspace_id, workspace)
	_mount_active_workspace_viewport()


func _clear_viewport_host() -> void:
	if _viewport_host == null:
		return
	for child in _viewport_host.get_children():
		_viewport_host.remove_child(child)


func _any_workspace_busy() -> bool:
	for workspace in _workspaces.values():
		if (workspace as EditorWorkspace).is_busy():
			return true
	if _environment_workspace != null and _environment_workspace.is_busy():
		return true
	return false


func _refresh_workspace_buttons() -> void:
	var busy := _any_workspace_busy()
	for workspace_id in _workspace_buttons:
		var btn: Button = _workspace_buttons[workspace_id]
		btn.set_pressed_no_signal(workspace_id == _active_workspace_id)
		btn.disabled = busy


func _refresh_workspace_surface() -> void:
	var workspace := _get_active_workspace()
	var workflows: Array = workspace.get_workflows() if workspace != null else []
	var has_workflows: bool = not workflows.is_empty()
	_rebuild_workspace_actions(workspace)
	_modes_label.visible = has_workflows
	_mode_rail.visible = has_workflows
	_sync_asset_dock_for_workspace(workspace)
	_rebuild_workflow_rail(workflows)
	if has_workflows:
		_inspector_workspace_id = -1
		_current_workflow_id = -1
		_refresh_workflow_from_workspace()
	else:
		_show_workspace_inspector(workspace)
	_refresh_workspace_buttons()


func _sync_asset_dock_for_workspace(workspace: EditorWorkspace) -> void:
	if _asset_dock_workspace_id != -1 and _asset_dock_workspace_id != _active_workspace_id:
		var old_workspace := _get_workspace(_asset_dock_workspace_id)
		if old_workspace != null:
			old_workspace.set_asset_dock(null)
		_clear_asset_dock_children()
		_asset_dock_workspace_id = -1
	var show_dock := workspace != null and workspace.uses_asset_dock()
	if not show_dock:
		if _asset_dock_workspace_id == _active_workspace_id and workspace != null:
			workspace.set_asset_dock(null)
		_clear_asset_dock_children()
		_asset_dock.visible = false
		_asset_dock_workspace_id = -1
		return
	_asset_dock.visible = true
	workspace.set_asset_dock(_asset_dock)
	_asset_dock_workspace_id = _active_workspace_id


func _clear_asset_dock_children() -> void:
	for child in _asset_dock.get_children():
		_asset_dock.remove_child(child)
		child.free()


func _show_workspace_inspector(workspace: EditorWorkspace) -> void:
	if _inspector_workspace_id == _active_workspace_id:
		return
	_inspector_workspace_id = _active_workspace_id
	for child in _inspector_host.get_children():
		child.queue_free()
	if workspace != null:
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


func _wire_tile_gizmo() -> void:
	_tile_gizmo_done.pressed.connect(_on_tile_gizmo_done_pressed)
	_tile_gizmo_rotate.pressed.connect(_on_tile_gizmo_rotate_pressed)
	_tile_gizmo_flip_x.pressed.connect(_on_tile_gizmo_flip_x_pressed)
	_tile_gizmo_flip_y.pressed.connect(_on_tile_gizmo_flip_y_pressed)
	_tile_gizmo_delete.pressed.connect(_on_tile_gizmo_delete_pressed)


func _wire_splits() -> void:
	if _body_row != null and not _body_row.drag_ended.is_connected(_save_split_layout):
		_body_row.drag_ended.connect(_save_split_layout)
	if _center_right_split != null and not _center_right_split.drag_ended.is_connected(_save_split_layout):
		_center_right_split.drag_ended.connect(_save_split_layout)


func _apply_window_min_size() -> void:
	# The editor needs a usable floor; Godot has no project setting for this, so
	# the window minimum is set at runtime. Only the editor shell does this, so
	# the runtime game window is unaffected.
	var window := get_window()
	if window != null:
		window.min_size = Vector2i(1024, 640)


# Read-only: applies the persisted split offsets without ever writing the config,
# so test instantiations never persist a layout. A side is only applied when it
# was actually stored (has_left/has_right), leaving the scene default otherwise.
# We set split_offset directly and let the container's resort keep children
# within their minimum sizes; calling clamp_split_offset() explicitly throws
# before the first sort (and when the dock is hidden), so it is avoided.
func _apply_split_layout() -> void:
	if _body_row == null or _center_right_split == null:
		return
	var state := _resource_library.load_layout_state()
	if bool(state["has_left"]):
		_body_row.split_offset = int(state["left"])
	if bool(state["has_right"]):
		_center_right_split.split_offset = int(state["right"])


func _save_split_layout() -> void:
	if _body_row == null or _center_right_split == null:
		return
	_resource_library.save_layout_state(_body_row.split_offset, _center_right_split.split_offset)


func _wire_camera_popup() -> void:
	if _camera_popup != null:
		_camera_popup.visible = false
		_camera_popup.apply_anchor(320.0)
		_camera_popup.bind_close(_camera_popup_close)
		if not _camera_popup.close_requested.is_connected(_on_camera_popup_close_pressed):
			_camera_popup.close_requested.connect(_on_camera_popup_close_pressed)
	if _camera_toggle_button != null and not _camera_toggle_button.toggled.is_connected(_on_camera_toggle_toggled):
		_camera_toggle_button.icon = _build_camera_icon()
		_camera_toggle_button.toggled.connect(_on_camera_toggle_toggled)
	_refresh_camera_popup_state()


func _wire_environment_popup() -> void:
	if _environment_popup != null:
		_environment_popup.visible = false
		_environment_popup.apply_anchor(400.0)
		_environment_popup.bind_close(_environment_popup_close)
		if not _environment_popup.close_requested.is_connected(_on_environment_popup_close_pressed):
			_environment_popup.close_requested.connect(_on_environment_popup_close_pressed)
	if _environment_toggle_button != null and not _environment_toggle_button.toggled.is_connected(_on_environment_toggle_toggled):
		_environment_toggle_button.icon = _build_sun_icon()
		_environment_toggle_button.toggled.connect(_on_environment_toggle_toggled)
	_refresh_environment_popup_state()


func _wire_settings_popup() -> void:
	if _settings_popup != null:
		_settings_popup.visible = false
		_settings_popup.apply_anchor(420.0)
		_settings_popup.bind_close(_settings_popup_close)
		if not _settings_popup.close_requested.is_connected(_on_settings_popup_close_pressed):
			_settings_popup.close_requested.connect(_on_settings_popup_close_pressed)
	if _settings_toggle_button != null and not _settings_toggle_button.toggled.is_connected(_on_settings_toggle_toggled):
		_settings_toggle_button.icon = _build_settings_icon()
		_settings_toggle_button.toggled.connect(_on_settings_toggle_toggled)
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
			# The native confirm/export dialogs own their own Escape handling
			# (they emit canceled and hide), so only the corner popovers need it
			# routed here.
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
		_camera_settings_panel.set_editor(self)


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


func get_editor_camera() -> Camera3D:
	var workspace := _get_active_workspace()
	if workspace != null:
		var workspace_camera := workspace.get_viewport_camera()
		if workspace_camera != null:
			return workspace_camera
	if editor != null and editor.has_method("get_editor_camera"):
		return editor.get_editor_camera()
	if editor != null:
		return editor.get("camera") as Camera3D
	return null


func _get_editor_camera() -> Camera3D:
	return get_editor_camera()


func _on_environment_toggle_toggled(pressed: bool) -> void:
	_set_environment_popup_visible(pressed)


func _on_environment_popup_close_pressed() -> void:
	_set_environment_popup_visible(false)


func show_environment_dialog() -> void:
	_set_environment_popup_visible(true)


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
		_rebuild_action_buttons(
			_environment_workspace,
			_environment_actions_host,
			_environment_action_buttons,
			Callable(self, "_on_environment_action_pressed"),
			"Environment",
			32.0
		)
	if _environment_inspector_host != null and _environment_inspector_host.get_child_count() == 0:
		_environment_workspace.build_inspector(_environment_inspector_host)


func _refresh_environment_popup_state() -> void:
	if _environment_popup_title != null:
		_environment_popup_title.text = _environment_workspace.get_project_title() if _environment_workspace != null else "Environment"
	_refresh_action_buttons_state(_environment_workspace, _environment_action_buttons)


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
		_settings_resource_dir_edit.text = _resource_library.get_root_dir()
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
	if not _resource_library.get_root_dir().is_empty():
		_scan_resource_root(true)


func _apply_resource_settings(scan: bool, persist: bool = true) -> Error:
	var path := _resource_library.get_root_dir()
	if _settings_resource_dir_edit != null:
		path = _settings_resource_dir_edit.text
	if _settings_recursive_toggle != null:
		_resource_recursive = _settings_recursive_toggle.button_pressed
	return _set_resource_root_dir(path, persist, scan)


func _ensure_resource_index() -> void:
	_resource_library.ensure_index()


func get_resource_index() -> RefCounted:
	return _resource_library.get_index()


func get_resource_root_dir() -> String:
	return _resource_library.get_root_dir()


func is_resource_recursive() -> bool:
	return _resource_recursive


func set_resource_root_dir(path: String) -> void:
	_set_resource_root_dir(path, true, true)


# Thin forwarders over EditorResourceLibrary (editor/resource_library.gd): the
# helper owns the index + root-dir state + persistence; the shell keeps the
# recursive flag and applies the UI side effects the helper surfaces (VegAssets
# search roots, status message, settings-popup sync).
func _set_resource_root_dir(path: String, persist: bool, scan: bool) -> Error:
	var result := _resource_library.set_root_dir(path, _resource_recursive, persist, scan)
	VegAssets.set_search_roots(result["search_roots"])
	_show_resource_status(result)
	_sync_settings_popup_state()
	return int(result["err"])


func _scan_resource_root(show_message: bool) -> Error:
	if _resource_library.get_root_dir().is_empty():
		_resource_library.clear_index()
		return OK
	var result := _resource_library.scan_root(_resource_recursive)
	if show_message:
		_show_resource_status(result)
	_sync_settings_popup_state()
	return int(result["err"])


func _show_resource_status(result: Dictionary) -> void:
	var status := String(result["status"])
	if status.is_empty():
		return
	show_status_message(status, 4.0 if int(result["err"]) == OK else 6.0)


func _load_resource_state() -> void:
	_resource_recursive = bool(_resource_library.load_state()["recursive"])


func _save_resource_state() -> void:
	_resource_library.save_state(_resource_recursive)


func _preferred_resource_root_dir() -> String:
	var root := _resource_library.get_root_dir()
	if not root.is_empty():
		return root
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


func _run_workspace_action(workspace: EditorWorkspace, action_id: int) -> void:
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
				var run_new := func() -> void:
					workspace.new_current()
				if _flush_workspace_or_status(workspace) and not _prompt_unsaved_workspace_action(workspace, "new", run_new):
					run_new.call()
		WorkspaceAction.OPEN:
			if not workspace.can_open():
				return
			var run_open := func() -> void:
				_open_resource_browser(workspace, open_trn)
			if _flush_workspace_or_status(workspace) and not _prompt_unsaved_workspace_action(workspace, "open", run_open):
				run_open.call()
		WorkspaceAction.SAVE:
			_on_save_pressed(workspace)
		WorkspaceAction.SAVE_AS:
			if _flush_workspace_or_status(workspace):
				_open_dir_dialog(
					workspace.get_save_dialog_title(),
					save_project_as,
					_preferred_save_dir(workspace)
				)
		WorkspaceAction.EXPORT:
			_on_export_pressed(workspace)


func _flush_workspace_or_status(workspace: EditorWorkspace) -> bool:
	if workspace == null:
		return false
	var err := workspace.flush_pending_edits()
	if err != OK:
		show_status_message("Resolve source parse errors before continuing.", 6.0)
		return false
	return true


func _prompt_unsaved_workspace_action(workspace: EditorWorkspace, action_name: String, action: Callable) -> bool:
	if workspace == null or not workspace.has_unsaved_changes():
		return false
	_set_prompt_state(
		PromptKind.UNSAVED,
		"",
		"Save changes?",
		"",
		"",
		false
	)
	_set_prompt_buttons("Cancel", "Discard", "Save")
	_prompt_secondary_action = Callable()
	_prompt_tertiary_action = func() -> void:
		if action.is_valid():
			action.call()
	_prompt_primary_action = func() -> void:
		if not _flush_workspace_or_status(workspace):
			return
		if workspace.can_save():
			var err := workspace.save_current()
			if err != OK:
				show_status_message("Save failed (error %d)" % err, 6.0)
				return
			if action.is_valid():
				action.call()
		elif workspace.can_save_as():
			var save_then_run := func(dir_path: String) -> void:
				var err := workspace.save_as(dir_path)
				if err != OK:
					show_status_message("Save failed (error %d)" % err, 6.0)
					return
				if action.is_valid():
					action.call()
			_open_dir_dialog(workspace.get_save_dialog_title(), save_then_run, _preferred_save_dir(workspace))
		else:
			show_status_message("%s save is not available." % workspace.get_workspace_label(), 4.0)
	_set_prompt_visible(true)
	show_status_message("Save or discard your changes to continue.", 6.0)
	return true


func _open_resource_browser(workspace: EditorWorkspace, on_pick: Callable) -> void:
	_resource_browser.open(workspace, on_pick)


# Resolves the active workspace's current resource path for the browser's
# "(open)" marker; the environment popup retargets it to the environment
# workspace. Stays on the shell (reads popup/workspace state) and is injected
# into EditorResourceBrowser as a capability callable.
func _current_resource_path_for_browser(kind: String) -> String:
	var workspace := _get_active_workspace()
	if _environment_popup != null and _environment_popup.visible and kind == "environment":
		workspace = _environment_workspace
	if workspace != null:
		return workspace.get_current_resource_path()
	return ""


# File and directory pickers share one cached native dialog (FileDialogHelper)
# instead of building and freeing a new FileDialog per open.
func _ensure_file_dialogs() -> FileDialogHelper:
	if _file_dialogs == null:
		_file_dialogs = FileDialogHelper.new(self)
	return _file_dialogs


func _open_file_dialog(title: String, filters: PackedStringArray, on_pick: Callable, current_dir: String = "") -> void:
	_ensure_file_dialogs().open(title, filters, on_pick, current_dir)


func _open_dir_dialog(title: String, on_pick: Callable, current_dir: String = "") -> void:
	_ensure_file_dialogs().open_dir(title, on_pick, current_dir)


func _preferred_save_dir(workspace: EditorWorkspace = null) -> String:
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


func _preferred_export_dir(workspace: EditorWorkspace = null) -> String:
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


func _on_save_pressed(workspace: EditorWorkspace = null) -> void:
	if workspace == null:
		workspace = _get_active_workspace()
	if workspace == null:
		return
	var err: Error = workspace.save_current()
	if err == ERR_PARSE_ERROR:
		show_status_message("Resolve source parse errors before saving.", 6.0)
		return
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


func _on_export_pressed(workspace: EditorWorkspace = null) -> void:
	if workspace == null:
		workspace = _get_active_workspace()
	if workspace == null or not workspace.can_export():
		return
	var choose_export_dir := func(dir_path: String) -> void:
		if not workspace.get_export_flavors().is_empty():
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
	for entry in workflows:
		var def := entry as InspectorDef
		if def == null:
			continue
		var workflow_id: int = def.id
		var btn := Button.new()
		btn.text = def.label if not def.label.is_empty() else "Workflow"
		btn.toggle_mode = true
		btn.focus_mode = Control.FOCUS_NONE
		btn.custom_minimum_size = Vector2(0, 44)
		btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		btn.tooltip_text = def.tooltip
		btn.pressed.connect(_on_workflow_pressed.bind(workflow_id))
		_mode_rail.add_child(btn)
		_workflow_buttons[workflow_id] = btn


func _refresh_workflow_from_workspace() -> void:
	var workspace := _get_active_workspace()
	if workspace == null or workspace.get_workflows().is_empty():
		return
	_set_workflow(workspace.get_active_workflow_id(), false)


func _on_workflow_pressed(workflow_id: int) -> void:
	_set_workflow(workflow_id, true)


func _set_workflow(workflow_id: int, activate: bool) -> void:
	var workspace := _get_active_workspace()
	if workspace == null or not _workflow_buttons.has(workflow_id):
		return
	if activate:
		workspace.activate_workflow(workflow_id)
	_sync_asset_dock_for_workspace(workspace)
	if workflow_id == _current_workflow_id:
		return
	_current_workflow_id = workflow_id
	for id in _workflow_buttons:
		var button: Button = _workflow_buttons[id]
		button.set_pressed_no_signal(id == workflow_id)
	_swap_workflow_inspector(workspace, workflow_id)


func _swap_workflow_inspector(workspace: EditorWorkspace, workflow_id: int) -> void:
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
	var workspace := _get_active_workspace()
	if workspace == null:
		_project_label.text = "OpenNova Terrain Editor"
		return
	_project_label.text = workspace.get_project_title()


func show_status_message(text: String, duration: float = 4.0) -> void:
	_message_text = text
	_message_until = Time.get_ticks_msec() / 1000.0 + duration


func _refresh_status() -> void:
	var workspace := _get_active_workspace()
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
	if workspace != null and workspace.shows_camera_status() and editor and editor.camera:
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
	var active_workspace := _get_active_workspace()
	if active_workspace == null or not active_workspace.shows_tile_gizmo() or editor == null or _current_workflow_id != TerrainWorkspaceAdapter.Workflow.STAMP or not editor.has_selected_tileinfo_entry():
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
	_ensure_unsaved_dialog()
	_unsaved_dialog.popup_centered()
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
	_cdep_fix_callback = on_fix_callback
	_ensure_cdep_dialog()
	_cdep_dialog.dialog_text = "%d area%s exceed the JO/DFX limit.\nBHD exports are unaffected." % [count, plural]
	_cdep_dialog.popup_centered()
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


func _show_export_flavor_dialog(dir_path: String) -> void:
	_pending_export_dir = dir_path
	_ensure_export_dialog()
	_export_dialog.select_flavor(ExportFlavorDialog.FLAVOR_DFX_JO)
	_export_dialog.popup_centered()


func _on_prompt_export_confirmed() -> void:
	if editor == null or _pending_export_dir.is_empty():
		return
	var workspace := _get_active_workspace()
	if workspace == null:
		return
	var flavor: int = _export_dialog.get_flavor() if _export_dialog != null else ExportFlavorDialog.FLAVOR_DFX_JO
	var err: Error = workspace.begin_export(_pending_export_dir, flavor)
	_pending_export_dir = ""
	if err != OK:
		show_status_message("Export failed (error %d)" % err, 6.0)


func _on_prompt_cancel() -> void:
	_pending_export_dir = ""


# The three confirms below are themed native dialogs (the editor theme styles
# ConfirmationDialog/AcceptDialog/Window). Each is created once as a child of the
# shell and given the shell theme explicitly, because an embedded Window does not
# resolve the in-tree theme through the Control parent chain (same pattern as the
# resource browser).
func _ensure_unsaved_dialog() -> void:
	if _unsaved_dialog != null and is_instance_valid(_unsaved_dialog):
		return
	_unsaved_dialog = ConfirmationDialog.new()
	_unsaved_dialog.name = "UnsavedChangesDialog"
	_unsaved_dialog.title = "Unsaved changes"
	_unsaved_dialog.dialog_text = "Save changes?"
	_unsaved_dialog.exclusive = true
	if theme != null:
		_unsaved_dialog.theme = theme
	add_child(_unsaved_dialog)
	_unsaved_dialog.get_ok_button().text = "Save"
	_unsaved_dialog.get_cancel_button().text = "Cancel"
	_unsaved_dialog.add_button("Discard", false, "discard")
	# OK confirms (Save), Cancel/Escape keeps editing, the custom Discard button
	# discards. Custom-action buttons do not auto-hide, so the handler hides it.
	_unsaved_dialog.confirmed.connect(_on_prompt_save_changes)
	_unsaved_dialog.canceled.connect(_on_prompt_keep_editing)
	_unsaved_dialog.custom_action.connect(_on_unsaved_custom_action)


func _on_unsaved_custom_action(action: StringName) -> void:
	if action == &"discard":
		if _unsaved_dialog != null:
			_unsaved_dialog.hide()
		_on_prompt_discard_changes()


func _ensure_cdep_dialog() -> void:
	if _cdep_dialog != null and is_instance_valid(_cdep_dialog):
		return
	_cdep_dialog = ConfirmationDialog.new()
	_cdep_dialog.name = "CdepFlattenDialog"
	_cdep_dialog.title = "Flatten before export?"
	_cdep_dialog.exclusive = true
	if theme != null:
		_cdep_dialog.theme = theme
	add_child(_cdep_dialog)
	_cdep_dialog.get_ok_button().text = "Flatten automatically"
	_cdep_dialog.get_cancel_button().text = "Leave as-is"
	_cdep_dialog.confirmed.connect(_on_cdep_flatten_confirmed)


func _on_cdep_flatten_confirmed() -> void:
	if _cdep_fix_callback.is_valid():
		_cdep_fix_callback.call()


func _ensure_export_dialog() -> void:
	if _export_dialog != null and is_instance_valid(_export_dialog):
		return
	_export_dialog = ExportFlavorDialog.new()
	_export_dialog.name = "ExportFlavorDialog"
	_export_dialog.exclusive = true
	if theme != null:
		_export_dialog.theme = theme
	add_child(_export_dialog)
	_export_dialog.confirmed.connect(_on_prompt_export_confirmed)
	_export_dialog.canceled.connect(_on_prompt_cancel)


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
	var workspace := _get_active_workspace()
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
