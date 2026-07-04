class_name EditorWorkstation
extends Control

const TerrainWorkspaceAdapter = preload("res://modtools/editor/terrain_workspace.gd")
const EnvironmentWorkspaceAdapter = preload("res://modtools/editor/environment_workspace.gd")
const ObjectWorkspaceAdapter = preload("res://modtools/object/object_workspace.gd")
const MissionWorkspaceAdapter = preload("res://modtools/editor/mission_workspace.gd")
const FontsWorkspaceAdapter = preload("res://modtools/editor/fonts_workspace.gd")
const CreditsWorkspaceAdapter = preload("res://modtools/editor/credits_workspace.gd")
const StringsWorkspaceAdapter = preload("res://modtools/strings/strings_workspace.gd")
const SoundWorkspaceAdapter = preload("res://modtools/sound/sound_workspace.gd")
const MnuWorkspaceAdapter = preload("res://modtools/mnu/mnu_workspace.gd")
const HudWorkspaceAdapter = preload("res://modtools/hud/hud_workspace.gd")
const MusicWorkspaceAdapter = preload("res://modtools/editor/music_workspace.gd")
const CameraSettingsPanelScene = preload("res://modtools/terrain/ui/camera_settings_panel.tscn")

enum Workspace { TERRAIN, ENVIRONMENT, OBJECT, MISSION, CREDITS, FONTS, STRINGS, MUSIC, SOUND, MNU, HUD }

# Workspaces are declared as WorkspaceDef rows in _workspace_defs(); the rail
# shows the non-popup ones in order. The enum above stays only as the stable id
# constants those rows (and tests/tools via get_workspace_adapter) key on — the
# shell itself never branches on a specific workspace. Document-action ids are
# ShellActionBar.Action.

# Item metadata sentinel for the "Clear list" entry in the recent-directories
# dropdown; real entries carry their path, the disabled placeholder carries "".
const _RECENT_CLEAR_META := "::clear::"

@onready var _nav_back_button: Button = %NavBackButton
@onready var _nav_forward_button: Button = %NavForwardButton
@onready var _context_workspace_label: Label = %ContextWorkspaceLabel
@onready var _context_doc_label: Label = %ContextDocLabel
@onready var _top_bar: PanelContainer = %TopBar
@onready var _body_row: SplitContainer = %BodyRow
@onready var _center_right_split: SplitContainer = %CenterRightSplit
@onready var _left_lane: PanelContainer = %LeftLane
@onready var _workspace_bar: PanelContainer = %WorkspaceBar
@onready var _workspace_rail: BoxContainer = %WorkspaceRail
@onready var _workspace_actions_host: BoxContainer = %WorkspaceActionsHost
@onready var _modes_label: Label = %ModesLabel
@onready var _mode_rail: VBoxContainer = %ModeRail
@onready var _inspector_host: Control = %InspectorHost
@onready var _viewport_lane: Control = %ViewportLane
@onready var _viewport_host: Control = %ViewportHost
@onready var _document_tab_strip: PanelContainer = %DocumentTabStrip
@onready var _document_tab_row: HBoxContainer = %DocumentTabRow
@onready var _camera_toggle_button: Button = %CameraToggleButton
@onready var _camera_popup: PopoverPanel = %CameraPopup
@onready var _camera_popup_close: Button = %CameraPopupClose
@onready var _camera_popup_detach: Button = %CameraPopupDetach
@onready var _camera_popup_content: Control = %CameraPopupContent
@onready var _camera_settings_host: Control = %CameraSettingsHost
@onready var _environment_toggle_button: Button = %EnvironmentToggleButton
@onready var _environment_popup: PopoverPanel = %EnvironmentPopup
@onready var _environment_popup_title: Label = %EnvironmentPopupTitle
@onready var _environment_popup_close: Button = %EnvironmentPopupClose
@onready var _environment_popup_detach: Button = %EnvironmentPopupDetach
@onready var _environment_popup_content: Control = %EnvironmentPopupContent
@onready var _environment_actions_host: VBoxContainer = %EnvironmentActionsHost
@onready var _environment_inspector_host: Control = %EnvironmentInspectorHost
@onready var _settings_toggle_button: Button = %SettingsToggleButton
@onready var _settings_popup: PopoverPanel = %SettingsPopup
@onready var _settings_popup_close: Button = %SettingsPopupClose
@onready var _settings_resource_dir_edit: LineEdit = %SettingsResourceDirEdit
@onready var _settings_browse_resource_dir_button: Button = %SettingsBrowseResourceDirButton
@onready var _settings_apply_resource_dir_button: Button = %SettingsApplyResourceDirButton
@onready var _settings_recent_row: HBoxContainer = %SettingsRecentRow
@onready var _settings_recent_option: OptionButton = %SettingsRecentOption
@onready var _settings_expansion_row: HBoxContainer = %SettingsExpansionRow
@onready var _settings_expansion_option: OptionButton = %SettingsExpansionOption
@onready var _settings_view_section: VBoxContainer = %SettingsViewSection
@onready var _settings_grid_toggle: CheckBox = %SettingsGridToggle
@onready var _settings_axes_toggle: CheckBox = %SettingsAxesToggle
@onready var _settings_mcp_toggle: CheckBox = %SettingsMcpToggle
@onready var _settings_mcp_port_edit: LineEdit = %SettingsMcpPortEdit
@onready var _settings_mcp_status_label: Label = %SettingsMcpStatusLabel
@onready var _settings_pff_tool_button: Button = %SettingsPffToolButton
@onready var _asset_dock: Control = %AssetDock
@onready var _right_split: SplitContainer = %RightSplit
@onready var _browser_toggle_button: Button = %BrowserToggleButton
@onready var _browser_pane_host: PanelContainer = %ResourceBrowserPaneHost
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
var _active_workspace_id: int = Workspace.MISSION
var _workspaces: Dictionary = {}
var _workspace_defs_cache: Array = []
# Popup workspaces (WorkspaceDef.popup) live outside _workspaces: id -> adapter.
var _popup_workspaces: Dictionary = {}
var _workspace_buttons: Dictionary = {}
# Shell sub-controllers (editor/shell/): each owns one shell surface over
# injected nodes + Callables, in the EditorResourceLibrary style.
var _top_action_bar := ShellActionBar.new()
var _environment_action_bar := ShellActionBar.new()
var _document_tabs := ShellDocumentTabs.new()
var _status := ShellStatusBar.new()
var _tile_gizmo_overlay := TileGizmoOverlay.new()
var _asset_dock_workspace_id: int = -1
var _camera_settings_panel: Control
var _resource_library := EditorResourceLibrary.new()
# 3D-preview guide visibility, shared across guide-capable workspaces and pushed
# to the active one. Loaded from / saved to the editor-state config.
var _view_grid_visible: bool = true
var _view_axes_visible: bool = true
var _mounted_workspace_id: int = -1
var _current_workflow_id: int = -1
var _workflow_buttons: Dictionary = {}
var _inspector_workspace_id: int = -1
var _save_export := ShellSaveExportFlow.new()
var _export_progress := ShellExportProgress.new()
# App-close guard: one prompt covering every workspace with unsaved work,
# separate from the flow module's per-action UnsavedChangesDialog.
var _close_guard_dialog: ConfirmationDialog
var _resource_browser := EditorResourceBrowser.new()
# The persistent Resource Browser pane (lazy: built on first show).
var _browser_pane: ResourceBrowserPane
var _pff_tool := EditorPffTool.new()
# Detachable panels (B6): the camera/environment popovers can pop their
# content into floating windows. State machines live in the hosts; the cached
# restore dicts make persisted "open floating" decisions without re-reading
# the config per toggle (the save handlers keep them current).
var _camera_panel_host: DetachablePanelHost
var _environment_panel_host: DetachablePanelHost
var _panel_restore: Dictionary = {}
# Browser-style Back/Forward over departure snapshots (see EditorNavHistory).
# History records only at the user navigation entry points
# (_on_workspace_pressed, open_in_workspace); direct set_active_workspace /
# open_file calls (tools, tests, the restore path itself) bypass it.
var _nav_history := EditorNavHistory.new()


func _ready() -> void:
	_ensure_workspaces()
	_ensure_resource_index()
	_load_resource_state()
	_resource_browser.setup(
		self,
		_resource_library,
		_save_export.open_file_dialog,
		func() -> void: _set_settings_popup_visible(true),
		_current_resource_path_for_browser,
		func() -> void: _scan_resource_root(false)
	)
	_pff_tool.setup(
		self,
		_save_export.open_files_dialog,
		_save_export.open_dir_dialog,
		show_status_message,
		_on_pff_extracted
	)
	var active_workspace_supplier := func() -> EditorWorkspace: return _get_active_workspace()
	_save_export.setup(
		self,
		active_workspace_supplier,
		show_status_message,
		func(workspace: EditorWorkspace, on_pick: Callable) -> void:
			_open_resource_browser(workspace, on_pick)
	)
	_export_progress.setup(
		self,
		_progress_backdrop,
		_progress_panel,
		_progress_title_label,
		_progress_message_label,
		_progress_bar,
		_progress_counts_label,
		active_workspace_supplier,
		show_status_message,
		func() -> void: _refresh_shell_state()
	)
	_top_action_bar.setup(
		_workspace_actions_host,
		Callable(self, "_on_workspace_action_pressed"),
		func() -> bool: return _any_workspace_busy(),
		active_workspace_supplier,
		func() -> Theme: return theme
	)
	_environment_action_bar.setup(
		_environment_actions_host,
		Callable(self, "_on_environment_action_pressed"),
		func() -> bool: return _any_workspace_busy(),
		func() -> EditorWorkspace: return _popup_workspace(),
		func() -> Theme: return theme,
		"Environment",
		32.0
	)
	_document_tabs.setup(
		_document_tab_strip,
		_document_tab_row,
		active_workspace_supplier,
		func() -> StyleBoxFlat: return _make_workspace_active_stylebox(),
		prompt_unsaved_for,
		save_then,
		sync_from_editor_state
	)
	_status.setup(
		_status_tool_label,
		_status_context_label,
		_status_camera_label,
		_status_fps_label,
		_context_workspace_label,
		_context_doc_label,
		active_workspace_supplier
	)
	_tile_gizmo_overlay.setup(_tile_gizmo, _tile_gizmo_label, _viewport_lane, active_workspace_supplier)
	_tile_gizmo_overlay.wire_buttons(
		_tile_gizmo_done, _tile_gizmo_rotate, _tile_gizmo_flip_x, _tile_gizmo_flip_y, _tile_gizmo_delete)
	_build_workspace_rail()
	_wire_nav_buttons()
	_wire_camera_popup()
	_wire_environment_popup()
	_wire_settings_popup()
	_wire_splits()
	_wire_browser_pane()
	_apply_window_min_size()
	get_tree().set_auto_accept_quit(false)
	_context_doc_label.clip_text = true
	_status_context_label.clip_text = true
	_status_camera_label.clip_text = true
	_status_fps_label.clip_text = true
	set_process(true)
	if not _resource_library.get_root_dir().is_empty():
		_scan_resource_root(false)
	_mount_active_workspace_viewport()
	_apply_view_guides_to_active_workspace()
	_refresh_workspace_surface()
	sync_from_editor_state()
	# Split offsets land after the first container sort so clamp sees real sizes.
	_apply_split_layout.call_deferred()


func _exit_tree() -> void:
	# Quit-while-floating remembers the preference + rect for the next session.
	if _camera_panel_host != null:
		_camera_panel_host.save_now()
	if _environment_panel_host != null:
		_environment_panel_host.save_now()
	for workspace in _workspaces.values():
		(workspace as EditorWorkspace).release_viewport()
	_clear_viewport_host()
	_mounted_workspace_id = -1


# The shell owns the app-close guard: a single prompt covering every workspace
# with unsaved work. _ready calls set_auto_accept_quit(false) so WM_CLOSE lands
# here; the embedded terrain editor's own handler defers to this (it no-ops when
# it has a workstation), so the user never sees two stacked dialogs. GUT and the
# screenshot tool quit through get_tree().quit(), which bypasses this entirely.
func _notification(what: int) -> void:
	if what == NOTIFICATION_WM_CLOSE_REQUEST:
		_handle_close_request()


## Public close entry (camera Escape, tools): runs the same unified close guard
## as the OS window close button.
func request_close() -> void:
	_handle_close_request()


func _handle_close_request() -> void:
	var dirty := PackedStringArray()
	for workspace in _workspaces.values():
		var ws := workspace as EditorWorkspace
		if ws != null and ws.has_unsaved_changes():
			dirty.append(ws.get_workspace_label())
	for workspace in _popup_workspaces.values():
		var ws := workspace as EditorWorkspace
		if ws != null and ws.has_unsaved_changes():
			dirty.append(ws.get_workspace_label())
	if dirty.is_empty():
		get_tree().quit()
		return
	_ensure_close_guard_dialog()
	_close_guard_dialog.dialog_text = "Unsaved changes in: %s.\n\nQuit without saving?" % ", ".join(dirty)
	_close_guard_dialog.popup_centered()


func _ensure_close_guard_dialog() -> void:
	if _close_guard_dialog != null and is_instance_valid(_close_guard_dialog):
		return
	_close_guard_dialog = ConfirmationDialog.new()
	_close_guard_dialog.name = "CloseGuardDialog"
	_close_guard_dialog.title = "Quit OpenNova Editor"
	_close_guard_dialog.exclusive = true
	if theme != null:
		_close_guard_dialog.theme = theme
	add_child(_close_guard_dialog)
	_close_guard_dialog.get_ok_button().text = "Quit without saving"
	_close_guard_dialog.get_cancel_button().text = "Keep editing"
	# OK quits; Cancel/Escape dismisses and leaves the editor open.
	_close_guard_dialog.confirmed.connect(func() -> void: get_tree().quit())


func set_editor(value: Node) -> void:
	editor = value
	_ensure_workspaces()
	for workspace in _workspaces.values():
		(workspace as EditorWorkspace).bind_to_editor(value)
	for workspace in _popup_workspaces.values():
		(workspace as EditorWorkspace).bind_to_editor(value)
	_reset_environment_popup_content()
	# A floating environment window must not sit empty until its next toggle.
	if _environment_panel_host != null and _environment_panel_host.is_floating():
		_ensure_environment_popup_content()
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
	_refresh_shell_state()
	_status.refresh()
	_tile_gizmo_overlay.refresh()
	_export_progress.sync()
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
	_export_progress.sync()
	_status.refresh()
	_tile_gizmo_overlay.refresh()


func _workspace_defs() -> Array:
	# Categories group the nav list; array order is the within-category order and
	# the order categories first appear (World, Interface, Audio, Atmosphere).
	return [
		WorkspaceDef.make(Workspace.MISSION, MissionWorkspaceAdapter, false, &"World", &"mission"),
		WorkspaceDef.make(Workspace.TERRAIN, TerrainWorkspaceAdapter, false, &"World", &"terrain"),
		WorkspaceDef.make(Workspace.OBJECT, ObjectWorkspaceAdapter, false, &"World", &"object"),
		WorkspaceDef.make(Workspace.FONTS, FontsWorkspaceAdapter, false, &"Interface", &"fonts"),
		WorkspaceDef.make(Workspace.CREDITS, CreditsWorkspaceAdapter, false, &"Interface", &"credits"),
		WorkspaceDef.make(Workspace.STRINGS, StringsWorkspaceAdapter, false, &"Interface", &"strings"),
		WorkspaceDef.make(Workspace.MNU, MnuWorkspaceAdapter, false, &"Interface", &"menu"),
		WorkspaceDef.make(Workspace.HUD, HudWorkspaceAdapter, false, &"Interface", &"hud"),
		WorkspaceDef.make(Workspace.MUSIC, MusicWorkspaceAdapter, false, &"Audio", &"music"),
		WorkspaceDef.make(Workspace.SOUND, SoundWorkspaceAdapter, false, &"Atmosphere", &"sound"),
		WorkspaceDef.make(Workspace.ENVIRONMENT, EnvironmentWorkspaceAdapter, true, &"Atmosphere", &"environment"),
	]


func _ensure_workspaces() -> void:
	if _workspace_defs_cache.is_empty():
		_workspace_defs_cache = _workspace_defs()
	for def_v in _workspace_defs_cache:
		var def := def_v as WorkspaceDef
		if def.popup:
			if not _popup_workspaces.has(def.id):
				var popup_workspace: EditorWorkspace = def.adapter_script.new()
				popup_workspace.set_editor_shell(self)
				_popup_workspaces[def.id] = popup_workspace
		elif not _workspaces.has(def.id):
			var workspace: EditorWorkspace = def.adapter_script.new()
			workspace.set_editor_shell(self)
			# The tab strip is signal-driven: rebuilt only from documents_changed
			# (and workspace switches), never from the per-frame shell poll.
			workspace.documents_changed.connect(_on_workspace_documents_changed.bind(def.id))
			_workspaces[def.id] = workspace


# Build the vertical workspace dock bar at the far left of the body: one toggle
# button per non-popup workspace, grouped by category with separators between
# groups. Nine entries fit the shell height with room to spare, so the bar never
# scrolls or overflows. Popup workspaces (Environment) stay on their top-bar
# toggle button, not the bar.
func _build_workspace_rail() -> void:
	var active_style := _make_workspace_active_stylebox()
	# Bucket defs by category, preserving array order within each bucket and the
	# order categories first appear in the registry.
	var buckets: Dictionary = {}
	var category_order: Array = []
	for def_v in _workspace_defs_cache:
		var def := def_v as WorkspaceDef
		if def.popup:
			continue
		if not buckets.has(def.category):
			buckets[def.category] = []
			category_order.append(def.category)
		(buckets[def.category] as Array).append(def)
	for category in category_order:
		if _workspace_rail.get_child_count() > 0:
			var separator := HSeparator.new()
			separator.custom_minimum_size = Vector2(40, 8)
			_workspace_rail.add_child(separator)
		for def_v in buckets[category]:
			_workspace_rail.add_child(_make_workspace_bar_button(def_v as WorkspaceDef, active_style))
	_refresh_workspace_buttons()


# A workspace dock button: an empty-text toggle Button carrying the active
# stylebox, with a mouse-ignoring VBox{icon, label} child anchored over it so the
# icon sits above the label. Button stays text-less; the child Label is what the
# user (and the tests) read.
func _make_workspace_bar_button(def: WorkspaceDef, active_style: StyleBoxFlat) -> Button:
	var workspace: EditorWorkspace = _get_workspace(def.id)
	var label_text := workspace.get_workspace_label() if workspace != null else "Workspace"
	var tooltip := workspace.get_workspace_tooltip() if workspace != null else ""
	var hotkey := _workspace_hotkey_number(def.id)
	var btn := Button.new()
	btn.toggle_mode = true
	btn.focus_mode = Control.FOCUS_NONE
	btn.custom_minimum_size = Vector2(0, 52)
	btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	btn.tooltip_text = ("Ctrl+%d  %s" % [hotkey, tooltip]) if hotkey > 0 else tooltip
	# Faint accent fill so the active row reads as filled, not just outlined.
	btn.add_theme_stylebox_override("pressed", active_style)
	btn.add_theme_stylebox_override("hover_pressed", active_style)
	btn.pressed.connect(_on_workspace_pressed.bind(def.id))
	_workspace_buttons[def.id] = btn

	# Button is not a Container, so the child lays out by anchors: fill the button.
	var box := VBoxContainer.new()
	box.name = "BarButtonBox"
	box.mouse_filter = Control.MOUSE_FILTER_IGNORE
	box.alignment = BoxContainer.ALIGNMENT_CENTER
	box.add_theme_constant_override("separation", 3)
	box.set_anchors_and_offsets_preset(Control.PRESET_FULL_RECT)
	btn.add_child(box)

	var icon := TextureRect.new()
	icon.name = "BarButtonIcon"
	icon.texture = EditorIconLibrary.resolve(def.icon_id)
	icon.custom_minimum_size = Vector2(16, 16)
	icon.expand_mode = TextureRect.EXPAND_KEEP_SIZE
	icon.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
	icon.size_flags_horizontal = Control.SIZE_SHRINK_CENTER
	icon.mouse_filter = Control.MOUSE_FILTER_IGNORE
	box.add_child(icon)

	var label := Label.new()
	label.name = "BarButtonLabel"
	label.text = label_text
	label.theme_type_variation = &"Muted"
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.clip_text = true
	label.mouse_filter = Control.MOUSE_FILTER_IGNORE
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(label)
	return btn


# 1-based position of a workspace among the non-popup bar entries (its Ctrl+N
# digit), or -1 if it has none. Only Ctrl+1..9 are wired, so a 10th-or-later
# entry gets no digit. _workspace_id_at_bar_index is the inverse.
func _workspace_hotkey_number(workspace_id: int) -> int:
	var index := 0
	for def_v in _workspace_defs_cache:
		var def := def_v as WorkspaceDef
		if def.popup:
			continue
		index += 1
		if def.id == workspace_id:
			return index if index <= 9 else -1
	return -1


func _workspace_id_at_bar_index(index: int) -> int:
	var i := 0
	for def_v in _workspace_defs_cache:
		var def := def_v as WorkspaceDef
		if def.popup:
			continue
		if i == index:
			return def.id
		i += 1
	return -1


func _wire_nav_buttons() -> void:
	if _nav_back_button != null:
		_nav_back_button.icon = EditorIconLibrary.resolve(&"nav_back")
		if _nav_back_button.icon == null:
			_nav_back_button.text = "<"
		if not _nav_back_button.pressed.is_connected(go_back):
			_nav_back_button.pressed.connect(go_back)
	if _nav_forward_button != null:
		_nav_forward_button.icon = EditorIconLibrary.resolve(&"nav_forward")
		if _nav_forward_button.icon == null:
			_nav_forward_button.text = ">"
		if not _nav_forward_button.pressed.is_connected(go_forward):
			_nav_forward_button.pressed.connect(go_forward)
	_refresh_nav_buttons()


# Local-only active-row emphasis (no editor_theme.tres change). The themed pressed
# background is darker than normal, so in a tall uniform list a hovered inactive
# row can read heavier than the active one; a faint accent fill fixes that.
func _make_workspace_active_stylebox() -> StyleBoxFlat:
	var sb := StyleBoxFlat.new()
	sb.bg_color = Color(0.8392, 0.5529, 0.2902, 0.22)
	sb.border_color = Color(0.8392, 0.5529, 0.2902, 0.9)
	sb.set_border_width_all(1)
	sb.set_corner_radius_all(3)
	sb.content_margin_left = 10.0
	sb.content_margin_right = 10.0
	sb.content_margin_top = 6.0
	sb.content_margin_bottom = 6.0
	return sb



func _rebuild_workspace_actions(workspace: EditorWorkspace) -> void:
	_top_action_bar.rebuild(workspace)
	_refresh_workspace_actions_state()


func _refresh_workspace_actions_state() -> void:
	if _workspace_actions_host == null:
		return
	_top_action_bar.refresh_state(_get_active_workspace())


func _on_workspace_pressed(workspace_id: int) -> void:
	var from := _location_snapshot()
	set_active_workspace(workspace_id)
	_record_nav_departure(from)


# Direct calls bypass the Back/Forward history (tools, tests, the restore path
# itself); user navigation records through _on_workspace_pressed and
# open_in_workspace.
func set_active_workspace(workspace_id: int) -> void:
	var def := _def_for_id(workspace_id)
	if def != null and def.popup:
		# Popup workspaces overlay the active view instead of replacing it. One
		# popover surface exists today; a second popup workspace would need the
		# surface generalized alongside this routing.
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
	_apply_view_guides_to_active_workspace()
	_refresh_workspace_surface()
	sync_from_editor_state()


func get_active_workspace_id() -> int:
	return _active_workspace_id


# Generic cross-workspace jump: open `path` in the workspace that declares `kind`
# (EditorWorkspace.get_open_resource_kind), then forward `focus` to its
# focus_reference hook. Capability-driven so the shell never grows per-type jump
# methods; the font/strings/menu jumps below are forwarders over this, and link
# widgets call it directly. A path equal to the workspace's current document skips
# the reopen, so focus-only jumps cannot drop unsaved edits.
func open_in_workspace(kind: String, path: String, focus: Dictionary = {}) -> Error:
	_ensure_workspaces()
	var workspace_id := _workspace_id_for_resource_kind(kind)
	if workspace_id == -1:
		return ERR_UNAVAILABLE
	var workspace := _workspace_for_id(workspace_id)
	var clean_path := path.strip_edges()
	if clean_path.is_empty():
		return ERR_INVALID_PARAMETER
	var from := _location_snapshot()
	if clean_path != String(workspace.get_current_resource_path()):
		var err: Error = workspace.open_file(clean_path)
		if err != OK:
			show_status_message("Could not open %s." % clean_path.get_file(), 5.0)
			return err
	if _active_workspace_id != workspace_id:
		set_active_workspace(workspace_id)
	else:
		_refresh_workspace_surface()
		sync_from_editor_state()
	# A focus miss below still navigated, so the departure records either way.
	_record_nav_departure(from)
	if not focus.is_empty():
		var focus_err: Error = workspace.focus_reference(focus)
		if focus_err != OK:
			var parts := PackedStringArray()
			for value in focus.values():
				parts.append(str(value))
			show_status_message("Opened %s; not found: %s" % [clean_path.get_file(), ", ".join(parts)], 5.0)
			return focus_err
	return OK


# The registry id of the workspace declaring `kind` as one of its open-resource
# kinds (-1 when no workspace does). Covers the popup workspace too, so jumps
# can target Environment. A workspace can claim multiple kinds via
# get_open_resource_kinds() (Menus claims both "menu" and "menu_style" since the
# stylesheet merged in).
func _workspace_id_for_resource_kind(kind: String) -> int:
	if kind.is_empty():
		return -1
	for def_v in _workspace_defs_cache:
		var def := def_v as WorkspaceDef
		var workspace := _workspace_for_id(def.id)
		if workspace == null:
			continue
		for claimed in workspace.get_open_resource_kinds():
			if String(claimed) == kind:
				return def.id
	return -1


# _get_workspace covers the main-rail workspaces; popup workspaces live outside
# _workspaces (see _ensure_workspaces), so jump targets resolve through this.
func _workspace_for_id(workspace_id: int) -> EditorWorkspace:
	var workspace := _get_workspace(workspace_id)
	if workspace != null:
		return workspace
	return _popup_workspaces.get(workspace_id)


## The workspace adapter registered for `workspace_id`, main rail or popup.
## The one accessor for tools/tests/MCP - shell internals may re-shape freely.
func get_workspace_adapter(workspace_id: int) -> EditorWorkspace:
	_ensure_workspaces()
	return _workspace_for_id(workspace_id)


# Cross-jump used by the Credits and Menus workspaces' font references. Fonts open
# by NAME, so resolution stays on the Fonts workspace (resolve_font_file); the open
# itself rides open_in_workspace.
func open_font_workspace(font_name: String) -> Error:
	_ensure_workspaces()
	var workspace := _get_workspace(Workspace.FONTS)
	if workspace == null:
		return ERR_UNAVAILABLE
	var clean_name := font_name.strip_edges()
	if clean_name.is_empty():
		return ERR_INVALID_PARAMETER
	var path := String(workspace.call("resolve_font_file", clean_name))
	if path.is_empty():
		show_status_message("Font not found: %s" % clean_name, 5.0)
		return ERR_DOES_NOT_EXIST
	var err := open_in_workspace("font", path)
	if err != OK:
		return err
	show_status_message("Opened font %s." % clean_name, 3.0)
	return OK


# Cross-jump used by the Menus workspace's "Edit in Strings": open the menu's resolved
# text table in the Strings workspace and focus the given key. table_path is an
# absolute path (already resolved by the caller).
func open_strings_workspace(table_path: String, key: String) -> Error:
	var err := open_in_workspace("strings", table_path, {"key": key})
	if err != OK:
		return err
	show_status_message("Editing string %s." % (key if not key.is_empty() else table_path.get_file()), 3.0)
	return OK


# Cross-jump used by the Menus workspace's cross-file ACTION rows. Resolve a
# .mnu name/path against the configured resource root, open it in Menus, then
# focus the target screen when supplied.
func open_menu_workspace(file: String, screen: String = "") -> Error:
	var path := _resolve_menu_action_path(file)
	if path.is_empty():
		show_status_message("Menu not found: %s" % file.strip_edges(), 5.0)
		return ERR_FILE_NOT_FOUND
	var target := screen.strip_edges()
	var err := open_in_workspace("menu", path, {"screen": target} if not target.is_empty() else {})
	if err != OK:
		return err
	show_status_message("Opened menu %s%s." % [
		path.get_file(),
		" -> %s" % target if not target.is_empty() else "",
	], 3.0)
	return OK


# --- Global navigation history (Back/Forward) --------------------------------

# Where the user is right now, as a history entry: the active workspace and its
# open document ("" when none). Captured lazily at departure, so everything
# beyond the path (selection, tabs, camera) keeps living in the persistent
# workspace instance itself.
func _location_snapshot() -> Dictionary:
	var workspace := _get_active_workspace()
	var path := String(workspace.get_current_resource_path()) if workspace != null else ""
	return {"workspace_id": _active_workspace_id, "path": path}


# Record `from` if the navigation that just ran actually moved the user.
# Same-location jumps (focus-only, failed opens, the Environment popup) leave
# the location unchanged and record nothing.
func _record_nav_departure(from: Dictionary) -> void:
	if EditorNavHistory.same(from, _location_snapshot()):
		return
	_nav_history.record(from)
	_refresh_nav_buttons()


func go_back() -> void:
	if _any_workspace_busy() or not _nav_history.can_go_back():
		return
	var current := _location_snapshot()
	var entry := _nav_history.peek_back()
	if _navigate_to(entry) == OK:
		_nav_history.commit_back(current)
	else:
		_nav_history.drop_back()
		show_status_message("Could not open %s." % String(entry.get("path", "")).get_file(), 5.0)
	_refresh_nav_buttons()


func go_forward() -> void:
	if _any_workspace_busy() or not _nav_history.can_go_forward():
		return
	var current := _location_snapshot()
	var entry := _nav_history.peek_forward()
	if _navigate_to(entry) == OK:
		_nav_history.commit_forward(current)
	else:
		_nav_history.drop_forward()
		show_status_message("Could not open %s." % String(entry.get("path", "")).get_file(), 5.0)
	_refresh_nav_buttons()


# Restore a history entry: reopen its document if it changed, then switch
# workspaces. Resolves through _get_workspace (never _workspace_for_id) so a
# corrupt entry cannot route to the Environment popup. Mirrors
# open_in_workspace's same-path skip, so returning to a still-open document
# keeps selection, tabs, and undo intact — and a failed reopen returns before
# any workspace switch, leaving the user where they were.
func _navigate_to(entry: Dictionary) -> Error:
	var workspace_id := int(entry.get("workspace_id", -1))
	var workspace := _get_workspace(workspace_id)
	if workspace == null:
		return ERR_UNAVAILABLE
	var entry_path := String(entry.get("path", ""))
	if not entry_path.is_empty() and entry_path != String(workspace.get_current_resource_path()):
		var err: Error = workspace.open_file(entry_path)
		if err != OK:
			return err
	if _active_workspace_id != workspace_id:
		set_active_workspace(workspace_id)
	else:
		_refresh_workspace_surface()
		sync_from_editor_state()
	return OK


func _refresh_nav_buttons() -> void:
	var busy := _any_workspace_busy()
	if _nav_back_button != null:
		_nav_back_button.disabled = busy or not _nav_history.can_go_back()
		_nav_back_button.tooltip_text = _nav_tooltip("Back", _nav_history.peek_back())
	if _nav_forward_button != null:
		_nav_forward_button.disabled = busy or not _nav_history.can_go_forward()
		_nav_forward_button.tooltip_text = _nav_tooltip("Forward", _nav_history.peek_forward())


func _nav_tooltip(verb: String, entry: Dictionary) -> String:
	if entry.is_empty():
		return verb
	var workspace := _get_workspace(int(entry.get("workspace_id", -1)))
	var label := String(workspace.get_workspace_label()) if workspace != null else ""
	if label.is_empty():
		return verb
	var file := String(entry.get("path", "")).get_file()
	if file.is_empty():
		return "%s to %s" % [verb, label]
	return "%s to %s (%s)" % [verb, label, file]


# Re-index the resource folder (the browser pane consumes this as an injected
# callable; workspaces that CREATE files under the root call it so the lazy
# VFS name index picks them up without a remount).
func rescan_resource_root() -> Error:
	return _scan_resource_root(false)


func _resolve_menu_action_path(file: String) -> String:
	var clean := file.strip_edges().replace("\\", "/")
	if clean.is_empty():
		return ""
	var candidates := _menu_action_path_candidates(clean)
	for candidate_value in candidates:
		var candidate := String(candidate_value)
		if FileAccess.file_exists(candidate):
			return candidate
	var root := _resource_library.get_root_dir()
	if not root.is_empty():
		for candidate_value in candidates:
			var candidate := String(candidate_value)
			var direct := root.path_join(candidate)
			if FileAccess.file_exists(direct):
				return direct
			var basename := root.path_join(candidate.get_file())
			if FileAccess.file_exists(basename):
				return basename
	var index := _resource_library.get_index()
	if _resource_library.get_root_dir().is_empty():
		return ""
	if index.get_root_dir().is_empty():
		_scan_resource_root(false)
	for entry_value in index.get_resource_files("menu"):
		var entry := entry_value as Dictionary
		var rel := String(entry.get("relative_path", "")).replace("\\", "/")
		var logical := String(entry.get("logical_name", "")).replace("\\", "/")
		for candidate_value in candidates:
			var candidate := String(candidate_value)
			var want := candidate.to_lower()
			var want_file := candidate.get_file().to_lower()
			if rel.to_lower() == want or rel.get_file().to_lower() == want_file \
					or logical.to_lower() == want or logical.get_file().to_lower() == want_file:
				var path := String(entry.get("path", ""))
				if not path.is_empty():
					return path
	return ""


func _menu_action_path_candidates(clean: String) -> Array:
	var candidates := [clean]
	var file_name := clean.get_file()
	if file_name.is_empty() or file_name.begins_with("jo_"):
		return candidates
	var jo_name := "jo_%s" % file_name
	var dir := clean.get_base_dir()
	if not dir.is_empty():
		var jo_relative := dir.path_join(jo_name)
		if not candidates.has(jo_relative):
			candidates.append(jo_relative)
	if not candidates.has(jo_name):
		candidates.append(jo_name)
	return candidates


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
	for workspace in _popup_workspaces.values():
		if (workspace as EditorWorkspace).is_busy():
			return true
	return false


func _refresh_workspace_buttons() -> void:
	var busy := _any_workspace_busy()
	for workspace_id in _workspace_buttons:
		var btn: Button = _workspace_buttons[workspace_id]
		btn.set_pressed_no_signal(workspace_id == _active_workspace_id)
		btn.disabled = busy
	_refresh_nav_buttons()


func _refresh_workspace_surface() -> void:
	var workspace := _get_active_workspace()
	var workflows: Array = workspace.get_workflows() if workspace != null else []
	var has_workflows: bool = not workflows.is_empty()
	_rebuild_workspace_actions(workspace)
	_modes_label.visible = has_workflows
	_mode_rail.visible = has_workflows
	# Whole left lane (picker + inspector host) is opt-out: a workspace that lives
	# entirely in the viewport (e.g. Music's unified screen) hides it to reclaim
	# the width. BodyRow is an HSplitContainer, so the viewport takes the space.
	_left_lane.visible = workspace == null or workspace.uses_left_lane()
	_sync_asset_dock_for_workspace(workspace)
	_rebuild_workflow_rail(workflows)
	if has_workflows:
		_inspector_workspace_id = -1
		_current_workflow_id = -1
		_refresh_workflow_from_workspace()
	else:
		_show_workspace_inspector(workspace)
	_document_tabs.rebuild()
	_refresh_workspace_buttons()


# --- Document tabs (multi-document workspaces) -----------------------------
# One strip above the viewport, hidden unless the active workspace opts into
# the document-tab tier. Rebuilds are signal-driven: documents_changed for the
# active workspace, plus the workspace-switch surface refresh.

func _on_workspace_documents_changed(workspace_id: int) -> void:
	if workspace_id == _active_workspace_id:
		_document_tabs.rebuild()


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
		_sync_right_split_visibility()
		return
	_asset_dock.visible = true
	workspace.set_asset_dock(_asset_dock)
	_asset_dock_workspace_id = _active_workspace_id
	_sync_right_split_visibility()


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
	# Fallback for a workspace with no inspector content: a clean call-to-action
	# (icon + guidance + its own New/Open + quick-open) instead of a bare line.
	var message := "This workspace is not available yet."
	var icon_id := &""
	if workspace != null:
		var context := workspace.get_status_context()
		message = context if not context.is_empty() else "Nothing is open here yet."
		var def := _def_for_id(_active_workspace_id)
		if def != null:
			icon_id = def.icon_id
	_build_empty_state_panel(_inspector_host, message, icon_id)


# A centered call-to-action for an empty inspector: the workspace icon, a one-line
# message, and the workspace's own New/Open actions plus quick-open. The buttons
# route through the same handlers as the top-bar toolbar, so behavior is identical.
func _build_empty_state_panel(host: Control, message: String, icon_id: StringName) -> void:
	var panel := VBoxContainer.new()
	panel.name = "EmptyStatePanel"
	panel.alignment = BoxContainer.ALIGNMENT_CENTER
	panel.add_theme_constant_override("separation", 14)
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	host.add_child(panel)

	var icon_tex := EditorIconLibrary.resolve(icon_id)
	if icon_tex != null:
		var icon := TextureRect.new()
		icon.name = "EmptyStateIcon"
		icon.texture = icon_tex
		icon.custom_minimum_size = Vector2(32, 32)
		icon.expand_mode = TextureRect.EXPAND_KEEP_SIZE
		icon.stretch_mode = TextureRect.STRETCH_KEEP_ASPECT_CENTERED
		icon.size_flags_horizontal = Control.SIZE_SHRINK_CENTER
		panel.add_child(icon)

	var label := Label.new()
	label.name = "EmptyStateMessage"
	label.text = message
	label.theme_type_variation = &"Muted"
	label.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	panel.add_child(label)

	var actions := HBoxContainer.new()
	actions.name = "EmptyStateActions"
	actions.alignment = BoxContainer.ALIGNMENT_CENTER
	actions.add_theme_constant_override("separation", 8)
	panel.add_child(actions)
	var workspace := _get_active_workspace()
	if workspace != null:
		for action_def in ShellActionBar.action_defs_for(workspace):
			var action_id := int(action_def["id"])
			if action_id != ShellActionBar.Action.NEW and action_id != ShellActionBar.Action.OPEN:
				continue
			if not bool(action_def["visible"]):
				continue
			var btn := Button.new()
			btn.name = "EmptyState" + ShellActionBar.button_name_for(action_id)
			btn.text = String(action_def["label"])
			btn.icon = EditorIconLibrary.resolve(ShellActionBar.icon_id_for(action_id))
			btn.focus_mode = Control.FOCUS_NONE
			btn.custom_minimum_size = Vector2(0, 34)
			btn.pressed.connect(_on_workspace_action_pressed.bind(action_id))
			actions.add_child(btn)
	var browse := Button.new()
	browse.name = "EmptyStateBrowseButton"
	browse.text = "Browse Resources"
	browse.icon = EditorIconLibrary.resolve(&"browser")
	browse.focus_mode = Control.FOCUS_NONE
	browse.custom_minimum_size = Vector2(0, 34)
	browse.pressed.connect(focus_browser_pane)
	actions.add_child(browse)


func _def_for_id(workspace_id: int) -> WorkspaceDef:
	for def_v in _workspace_defs_cache:
		var def := def_v as WorkspaceDef
		if def.id == workspace_id:
			return def
	return null


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


# The Resource Browser pane is a DOCK, not a popover: it never joins the
# camera/environment/settings mutual exclusion or the Escape handler, and its
# visibility + split width persist across sessions.
func _wire_browser_pane() -> void:
	if _browser_toggle_button != null:
		_browser_toggle_button.icon = EditorIconLibrary.resolve(&"browser")
		_browser_toggle_button.toggled.connect(_set_browser_pane_visible)
	if _right_split != null and not _right_split.drag_ended.is_connected(_save_browser_state):
		_right_split.drag_ended.connect(_save_browser_state)
	# Startup restore is a read-only apply (mirrors _apply_split_layout): test
	# instantiations must never persist a layout they did not change.
	var state := _resource_library.load_browser_state()
	if _right_split != null and bool(state["has_split"]):
		_right_split.split_offset = int(state["split"])
	if bool(state["visible"]) and _browser_pane_host != null:
		_ensure_browser_pane()
		_browser_pane_host.visible = true
		if _browser_toggle_button != null:
			_browser_toggle_button.set_pressed_no_signal(true)
		# Refresh now only when no root is configured (nothing will scan later);
		# with a root, _ready's scan fills the pane through _refresh_browser_pane
		# - an eager refresh here would lazy-scan and double the startup index walk.
		if _resource_library.get_root_dir().is_empty():
			_browser_pane.refresh()
	_sync_right_split_visibility()


func _ensure_browser_pane() -> void:
	if _browser_pane != null and is_instance_valid(_browser_pane):
		return
	_browser_pane = ResourceBrowserPane.new()
	_browser_pane.name = "ResourceBrowserPane"
	# Capabilities only - the pane's double-click rides the same cross-jump
	# spine as the link widgets (open_in_workspace), never a private open path.
	_browser_pane.setup(
		func() -> RefCounted: return _resource_library.get_index(),
		func() -> String: return _resource_library.get_root_dir(),
		func() -> void: _scan_resource_root(false),
		_current_resource_path_for_browser,
		func(kind: String, path: String) -> void: open_in_workspace(kind, path)
	)
	_browser_pane_host.add_child(_browser_pane)


func _set_browser_pane_visible(active: bool) -> void:
	if _browser_pane_host == null:
		return
	if active:
		_ensure_browser_pane()
	_browser_pane_host.visible = active
	if _browser_toggle_button != null:
		_browser_toggle_button.set_pressed_no_signal(active)
	_sync_right_split_visibility()
	if active and _browser_pane != null:
		_browser_pane.refresh()
	_save_browser_state()


# Quick-open: reveal the resource browser pane and focus its search box (Ctrl+P,
# and the empty-state "Browse resources" button). The table's focus_search()
# defers the grab, so calling it right after the pane is shown is safe.
func focus_browser_pane() -> void:
	_set_browser_pane_visible(true)
	if _browser_pane != null and is_instance_valid(_browser_pane):
		_browser_pane.table.focus_search()


# With both children hidden, RightSplit itself hides so dockless workspaces
# keep the pre-pane behavior: no live divider, and the persisted right offset
# stays inert (CenterRightSplit sees one visible child).
func _sync_right_split_visibility() -> void:
	if _right_split == null:
		return
	_right_split.visible = (_asset_dock != null and _asset_dock.visible) \
			or (_browser_pane_host != null and _browser_pane_host.visible)


func _save_browser_state() -> void:
	if _browser_pane_host == null or _right_split == null:
		return
	_resource_library.save_browser_state(_browser_pane_host.visible, _right_split.split_offset)


# Keep a visible pane truthful after the root changes or a rescan.
func _refresh_browser_pane() -> void:
	if _browser_pane != null and is_instance_valid(_browser_pane) \
			and _browser_pane_host != null and _browser_pane_host.visible:
		_browser_pane.refresh()


func _wire_camera_popup() -> void:
	if _camera_popup != null:
		_camera_popup.visible = false
		_camera_popup.apply_anchor(320.0)
		_camera_popup.bind_close(_camera_popup_close)
		_camera_popup.bind_detach(_camera_popup_detach)
		if not _camera_popup.close_requested.is_connected(_on_camera_popup_close_pressed):
			_camera_popup.close_requested.connect(_on_camera_popup_close_pressed)
		if not _camera_popup.detach_requested.is_connected(_detach_camera_panel):
			_camera_popup.detach_requested.connect(_detach_camera_panel)
	if _camera_panel_host == null and _camera_popup_content != null:
		_camera_panel_host = DetachablePanelHost.new(&"camera", "Camera", Vector2i(344, 320))
		_camera_panel_host.setup(_camera_popup_content, self, _save_camera_panel_state)
		_camera_panel_host.floating_changed.connect(_on_camera_floating_changed)
		# Read-only startup apply: remember the preference, never spawn windows
		# at launch (the floating preference applies on the next open).
		_panel_restore["camera"] = _resource_library.load_panel_state("camera")
	if _camera_toggle_button != null and not _camera_toggle_button.toggled.is_connected(_on_camera_toggle_toggled):
		_camera_toggle_button.icon = EditorIconLibrary.resolve(&"camera")
		_camera_toggle_button.toggled.connect(_on_camera_toggle_toggled)
	_refresh_camera_popup_state()


func _wire_environment_popup() -> void:
	if _environment_popup != null:
		_environment_popup.visible = false
		_environment_popup.apply_anchor(400.0)
		_environment_popup.bind_close(_environment_popup_close)
		_environment_popup.bind_detach(_environment_popup_detach)
		if not _environment_popup.close_requested.is_connected(_on_environment_popup_close_pressed):
			_environment_popup.close_requested.connect(_on_environment_popup_close_pressed)
		if not _environment_popup.detach_requested.is_connected(_detach_environment_panel):
			_environment_popup.detach_requested.connect(_detach_environment_panel)
	if _environment_panel_host == null and _environment_popup_content != null:
		_environment_panel_host = DetachablePanelHost.new(&"environment", "Environment", Vector2i(424, 480))
		_environment_panel_host.setup(_environment_popup_content, self, _save_environment_panel_state)
		_environment_panel_host.floating_changed.connect(_on_environment_floating_changed)
		_panel_restore["environment"] = _resource_library.load_panel_state("environment")
	if _environment_toggle_button != null and not _environment_toggle_button.toggled.is_connected(_on_environment_toggle_toggled):
		_environment_toggle_button.icon = EditorIconLibrary.resolve(&"environment")
		_environment_toggle_button.toggled.connect(_on_environment_toggle_toggled)
	_refresh_environment_popup_state()


# --- Detachable panels (B6) ---

func _save_camera_panel_state(docked: bool, rect: Rect2i) -> void:
	_panel_restore["camera"] = {"docked": docked, "has_rect": true, "rect": rect}
	_resource_library.save_panel_state("camera", docked, rect)


func _save_environment_panel_state(docked: bool, rect: Rect2i) -> void:
	_panel_restore["environment"] = {"docked": docked, "has_rect": true, "rect": rect}
	_resource_library.save_panel_state("environment", docked, rect)


func _panel_restore_for(panel_id: String) -> Dictionary:
	return _panel_restore.get(panel_id, {"docked": true, "has_rect": false, "rect": Rect2i()})


# The window opens where it was last seen (clamped to a visible screen at
# apply time); first detach derives a rect from where the popover sits.
func _panel_detach_rect(panel_id: String, popover: PopoverPanel) -> Rect2i:
	var restore := _panel_restore_for(panel_id)
	if bool(restore.get("has_rect", false)):
		return restore.get("rect", Rect2i()) as Rect2i
	return DetachablePanelHost.screen_rect_for(popover)


func _detach_camera_panel() -> void:
	if _camera_panel_host == null or _camera_panel_host.is_floating():
		return
	# Content must exist before it floats (the popover may never have opened).
	_ensure_camera_popup_content()
	_camera_panel_host.detach(_panel_detach_rect("camera", _camera_popup))
	if _camera_popup != null:
		_camera_popup.close()
	_refresh_camera_popup_state()


func _detach_environment_panel() -> void:
	if _environment_panel_host == null or _environment_panel_host.is_floating():
		return
	_ensure_environment_popup_content()
	_environment_panel_host.detach(_panel_detach_rect("environment", _environment_popup))
	if _environment_popup != null:
		_environment_popup.close()
	_refresh_environment_popup_state()


func _on_camera_floating_changed(floating: bool) -> void:
	if _camera_toggle_button != null:
		_camera_toggle_button.set_pressed_no_signal(floating)


func _on_environment_floating_changed(floating: bool) -> void:
	if _environment_toggle_button != null:
		_environment_toggle_button.set_pressed_no_signal(floating)


func _wire_settings_popup() -> void:
	if _settings_popup != null:
		_settings_popup.visible = false
		_settings_popup.apply_anchor(420.0)
		_settings_popup.bind_close(_settings_popup_close)
		if not _settings_popup.close_requested.is_connected(_on_settings_popup_close_pressed):
			_settings_popup.close_requested.connect(_on_settings_popup_close_pressed)
	if _settings_toggle_button != null and not _settings_toggle_button.toggled.is_connected(_on_settings_toggle_toggled):
		_settings_toggle_button.icon = EditorIconLibrary.resolve(&"settings")
		_settings_toggle_button.toggled.connect(_on_settings_toggle_toggled)
	if _settings_browse_resource_dir_button != null and not _settings_browse_resource_dir_button.pressed.is_connected(_on_settings_browse_resource_dir_pressed):
		_settings_browse_resource_dir_button.pressed.connect(_on_settings_browse_resource_dir_pressed)
	if _settings_apply_resource_dir_button != null and not _settings_apply_resource_dir_button.pressed.is_connected(_on_settings_apply_resource_dir_pressed):
		_settings_apply_resource_dir_button.pressed.connect(_on_settings_apply_resource_dir_pressed)
	if _settings_resource_dir_edit != null and not _settings_resource_dir_edit.text_submitted.is_connected(_on_settings_resource_dir_submitted):
		_settings_resource_dir_edit.text_submitted.connect(_on_settings_resource_dir_submitted)
	if _settings_recent_option != null and not _settings_recent_option.item_selected.is_connected(_on_settings_recent_selected):
		_settings_recent_option.item_selected.connect(_on_settings_recent_selected)
	if _settings_grid_toggle != null and not _settings_grid_toggle.toggled.is_connected(_on_settings_grid_toggled):
		_settings_grid_toggle.toggled.connect(_on_settings_grid_toggled)
	if _settings_axes_toggle != null and not _settings_axes_toggle.toggled.is_connected(_on_settings_axes_toggled):
		_settings_axes_toggle.toggled.connect(_on_settings_axes_toggled)
	if _settings_pff_tool_button != null and not _settings_pff_tool_button.pressed.is_connected(_on_settings_pff_tool_pressed):
		_settings_pff_tool_button.pressed.connect(_on_settings_pff_tool_pressed)
	if _settings_mcp_toggle != null and not _settings_mcp_toggle.toggled.is_connected(_on_settings_mcp_toggled):
		_settings_mcp_toggle.toggled.connect(_on_settings_mcp_toggled)
	if _settings_mcp_port_edit != null and not _settings_mcp_port_edit.text_submitted.is_connected(_on_settings_mcp_port_submitted):
		_settings_mcp_port_edit.text_submitted.connect(_on_settings_mcp_port_submitted)
	_sync_settings_popup_state()


func _on_settings_pff_tool_pressed() -> void:
	# Close the settings popover so the modal archive tool isn't competing with it,
	# then open the tool seeded at the configured resource directory.
	_set_settings_popup_visible(false)
	_pff_tool.open(_preferred_resource_root_dir())


func _on_pff_extracted(dir: String) -> void:
	# The quick-open index is only built at startup/root-change, so files extracted
	# into the configured resource root would stay invisible until a restart. Rescan
	# for the user. Exact-root match only: the loose scan is top-level-only by design,
	# so a subfolder extraction would not be picked up by a rescan anyway.
	var root := _resource_library.get_root_dir()
	if root.is_empty():
		return
	if _resource_library.canonical_key(dir) != _resource_library.canonical_key(root):
		return
	_scan_resource_root(false)


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
		# Ctrl+1..9 jump to the Nth workspace dock button; Ctrl+P opens quick-open.
		# Routed through _on_workspace_pressed so the jump records Back/Forward
		# history exactly like a click. An editor that consumes Ctrl+digit itself
		# (none today) would shadow this, since descendants see input first.
		elif key.pressed and not key.is_echo() and key.ctrl_pressed \
				and not key.alt_pressed and not key.shift_pressed:
			if key.keycode >= KEY_1 and key.keycode <= KEY_9:
				var workspace_id := _workspace_id_at_bar_index(key.keycode - KEY_1)
				if workspace_id >= 0:
					_on_workspace_pressed(workspace_id)
					get_viewport().set_input_as_handled()
			elif key.keycode == KEY_P:
				focus_browser_pane()
				get_viewport().set_input_as_handled()
		# Global Back/Forward. A workspace-local navigation that uses the same
		# gestures wins while its view is on screen (Music's canvas nav,
		# live_mode.gd) — descendants see unhandled input first.
		elif key.pressed and not key.is_echo() and key.alt_pressed:
			if key.keycode == KEY_LEFT:
				go_back()
				get_viewport().set_input_as_handled()
			elif key.keycode == KEY_RIGHT:
				go_forward()
				get_viewport().set_input_as_handled()
	elif event is InputEventMouseButton:
		var mouse := event as InputEventMouseButton
		if mouse.pressed:
			if mouse.button_index == MOUSE_BUTTON_XBUTTON1:
				go_back()
				get_viewport().set_input_as_handled()
			elif mouse.button_index == MOUSE_BUTTON_XBUTTON2:
				go_forward()
				get_viewport().set_input_as_handled()


func _on_camera_toggle_toggled(pressed: bool) -> void:
	_set_camera_popup_visible(pressed)


func _on_camera_popup_close_pressed() -> void:
	_set_camera_popup_visible(false)


func _set_camera_popup_visible(active: bool) -> void:
	if _camera_popup == null:
		return
	# A floating panel is not a popover: the toggle raises its window, and the
	# siblings' mutual-exclusion calls (active=false) must leave it alone.
	if _camera_panel_host != null and _camera_panel_host.is_floating():
		if active:
			_ensure_camera_popup_content()
			_camera_panel_host.focus_window()
		if _camera_toggle_button != null:
			_camera_toggle_button.set_pressed_no_signal(true)
		return
	if active and _get_editor_camera() == null:
		active = false
	# The remembered floating preference applies on open, never at launch.
	if active and _camera_panel_host != null \
			and not bool(_panel_restore_for("camera").get("docked", true)):
		_detach_camera_panel()
		return
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
	var floating := _camera_panel_host != null and _camera_panel_host.is_floating()
	# A floating camera panel whose camera disappeared re-docks (mirrors the
	# docked popover's force-close below). persist=false: a transient
	# camera-null (editor rebind) must not overwrite the user's floating
	# preference - the next open with a camera floats again.
	if floating and not has_camera:
		_camera_panel_host.redock(false)
		floating = false
	if _camera_toggle_button != null:
		_camera_toggle_button.disabled = not has_camera
		if not has_camera:
			_camera_toggle_button.set_pressed_no_signal(false)
		elif floating:
			_camera_toggle_button.set_pressed_no_signal(true)
	if _camera_popup != null and _camera_popup.visible and not has_camera:
		_camera_popup.visible = false
	if (_camera_popup != null and _camera_popup.visible) or floating:
		_ensure_camera_popup_content()
		if _camera_settings_panel != null and _camera_settings_panel.has_method("sync_from_editor_state"):
			_camera_settings_panel.sync_from_editor_state()


# The active workspace's camera via its capability hook; null in workspaces
# without a 3D view (the camera popup then reports no camera instead of
# silently editing a hidden terrain camera).
func get_editor_camera() -> Camera3D:
	var workspace := _get_active_workspace()
	if workspace != null:
		return workspace.get_viewport_camera()
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
	# A floating panel is not a popover: the toggle raises its window, and the
	# siblings' mutual-exclusion calls (active=false) must leave it alone.
	if _environment_panel_host != null and _environment_panel_host.is_floating():
		if active:
			_ensure_environment_popup_content()
			_environment_panel_host.focus_window()
		if _environment_toggle_button != null:
			_environment_toggle_button.set_pressed_no_signal(true)
		return
	# The remembered floating preference applies on open, never at launch.
	if active and _environment_panel_host != null \
			and not bool(_panel_restore_for("environment").get("docked", true)):
		_detach_environment_panel()
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
	_environment_action_bar.rebuild(null)
	if _environment_actions_host != null:
		for child in _environment_actions_host.get_children():
			_environment_actions_host.remove_child(child)
			child.free()
	if _environment_inspector_host != null:
		for child in _environment_inspector_host.get_children():
			_environment_inspector_host.remove_child(child)
			child.free()


# The workspace behind the single popup surface (%EnvironmentPopup). One popup
# workspace exists today; a second requires generalizing the surface too.
func _popup_workspace() -> EditorWorkspace:
	for workspace in _popup_workspaces.values():
		return workspace
	return null


func _ensure_environment_popup_content() -> void:
	var popup_workspace := _popup_workspace()
	if popup_workspace == null:
		return
	if _environment_actions_host != null and _environment_action_bar.buttons().is_empty():
		_environment_action_bar.rebuild(popup_workspace)
	if _environment_inspector_host != null and _environment_inspector_host.get_child_count() == 0:
		popup_workspace.build_inspector(_environment_inspector_host)


func _refresh_environment_popup_state() -> void:
	var popup_workspace := _popup_workspace()
	var title := popup_workspace.get_project_title() if popup_workspace != null else "Environment"
	if _environment_popup_title != null:
		_environment_popup_title.text = title
	if _environment_panel_host != null and _environment_panel_host.is_floating():
		# The dirty "*" reaches the floating window through its OS title.
		_environment_panel_host.set_window_title("Environment — %s" % title)
	_environment_action_bar.refresh_state(_popup_workspace())


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
	_populate_expansion_options()
	_populate_recent_dirs()
	if _settings_grid_toggle != null:
		_settings_grid_toggle.set_pressed_no_signal(_view_grid_visible)
	if _settings_axes_toggle != null:
		_settings_axes_toggle.set_pressed_no_signal(_view_axes_visible)
	# The View section only applies to workspaces with a 3D guide overlay; hide it
	# for the rest so the popup stays relevant to the active workspace.
	if _settings_view_section != null:
		var workspace := _get_active_workspace()
		_settings_view_section.visible = workspace != null and workspace.shows_view_guides()
	_sync_settings_mcp_state()


# Reflect the MCP service's live state into the Settings popup (toggle, port,
# status line). The service lives on the TerrainEditor root; runtime builds
# have none and the controls simply show Stopped/disabled.
func _sync_settings_mcp_state() -> void:
	var service := _mcp_service()
	if _settings_mcp_toggle != null:
		_settings_mcp_toggle.set_pressed_no_signal(service != null and service.is_running())
		_settings_mcp_toggle.disabled = service == null
	if _settings_mcp_port_edit != null and not _settings_mcp_port_edit.has_focus():
		_settings_mcp_port_edit.text = str(McpSettings.get_port())
	if _settings_mcp_status_label != null:
		_settings_mcp_status_label.text = service.get_status_text() if service != null else "Unavailable in this build"


func _mcp_service() -> Node:
	return editor.get("mcp_service") if editor != null else null


func _on_settings_mcp_toggled(pressed: bool) -> void:
	var service := _mcp_service()
	if service != null:
		service.set_enabled(pressed)
	_sync_settings_mcp_state()


func _on_settings_mcp_port_submitted(text: String) -> void:
	var service := _mcp_service()
	if not text.is_valid_int():
		show_status_message("MCP port must be a number (1024-65535).", 5.0)
		_sync_settings_mcp_state()
		return
	var port := clampi(text.to_int(), 1024, 65535)
	if service != null:
		service.apply_port(port)
	else:
		McpSettings.set_port(port)
	_sync_settings_mcp_state()


func _on_settings_browse_resource_dir_pressed() -> void:
	var on_pick := func(path: String) -> void:
		if _settings_resource_dir_edit != null:
			_settings_resource_dir_edit.text = path
		_apply_resource_settings(true)
	_save_export.open_dir_dialog("Select resource directory", on_pick, _preferred_resource_root_dir())


func _on_settings_apply_resource_dir_pressed() -> void:
	_apply_resource_settings(true)


func _on_settings_resource_dir_submitted(_text: String) -> void:
	_apply_resource_settings(true)


# The editor authors loose files only; PFF expansions are a runtime concern (mounted
# via the `/exp` launch flag), so the settings popup no longer offers an expansion picker.
# The row is hidden here in case the scene still carries it.
func _populate_expansion_options() -> void:
	if _settings_expansion_row != null:
		_settings_expansion_row.visible = false


# Fill the "Recent directories…" dropdown from the shared recent-dirs list. Index 0
# is a disabled placeholder so the control reads as an action menu (its face never
# shows a picked path); the currently active root is excluded; a "Clear list" item
# trails the entries. The whole row hides when there is nothing else to switch to.
func _populate_recent_dirs() -> void:
	if _settings_recent_option == null:
		return
	_settings_recent_option.clear()
	_settings_recent_option.add_item("Recent directories…")
	_settings_recent_option.set_item_disabled(0, true)
	_settings_recent_option.set_item_metadata(0, "")
	var current_key := _resource_library.canonical_key(_resource_library.get_root_dir())
	var count := 0
	for path in _resource_library.get_recent_dirs():
		if _resource_library.canonical_key(path) == current_key:
			continue
		var display := path.replace("\\", "/").rstrip("/").get_file()
		if display.is_empty():
			display = path
		var idx := _settings_recent_option.item_count
		_settings_recent_option.add_item(display)
		_settings_recent_option.set_item_metadata(idx, path)
		_settings_recent_option.set_item_tooltip(idx, path)
		count += 1
	if count > 0:
		_settings_recent_option.add_separator()
		var clear_idx := _settings_recent_option.item_count
		_settings_recent_option.add_item("Clear list")
		_settings_recent_option.set_item_metadata(clear_idx, _RECENT_CLEAR_META)
	_settings_recent_option.select(0)
	if _settings_recent_row != null:
		_settings_recent_row.visible = count > 0


# Apply a directory chosen from the recent-directories dropdown (mirrors the Browse
# on_pick: fill the path field, then apply + refresh). The control is reset to its
# placeholder so it never displays a selection and the same entry can be re-picked.
func _on_settings_recent_selected(index: int) -> void:
	if _settings_recent_option == null:
		return
	var path := String(_settings_recent_option.get_item_metadata(index))
	_settings_recent_option.select(0)
	if path.is_empty():
		return
	if path == _RECENT_CLEAR_META:
		_resource_library.clear_recent_dirs()
		_populate_recent_dirs()
		return
	if _settings_resource_dir_edit != null:
		_settings_resource_dir_edit.text = path
	_apply_resource_settings(true)


func _on_settings_grid_toggled(pressed: bool) -> void:
	if _view_grid_visible == pressed:
		return
	_view_grid_visible = pressed
	_resource_library.save_view_state(_view_grid_visible, _view_axes_visible)
	_apply_view_guides_to_active_workspace()


func _on_settings_axes_toggled(pressed: bool) -> void:
	if _view_axes_visible == pressed:
		return
	_view_axes_visible = pressed
	_resource_library.save_view_state(_view_grid_visible, _view_axes_visible)
	_apply_view_guides_to_active_workspace()


# Push the persisted guide visibility onto the active workspace (no-op for ones
# without a 3D guide overlay). Called when the choice changes and when a
# guide-capable workspace becomes active.
func _apply_view_guides_to_active_workspace() -> void:
	var workspace := _get_active_workspace()
	if workspace == null or not workspace.shows_view_guides():
		return
	workspace.set_grid_visible(_view_grid_visible)
	workspace.set_axes_visible(_view_axes_visible)


func _apply_resource_settings(scan: bool, persist: bool = true) -> Error:
	var path := _resource_library.get_root_dir()
	if _settings_resource_dir_edit != null:
		path = _settings_resource_dir_edit.text
	return _set_resource_root_dir(path, persist, scan)


func _ensure_resource_index() -> void:
	_resource_library.ensure_index()


func get_resource_index() -> RefCounted:
	return _resource_library.get_index()


func get_resource_root_dir() -> String:
	return _resource_library.get_root_dir()


func get_resource_root() -> NovaResourceRoot:
	return _resource_library.get_resource_root()


func set_resource_root_dir(path: String) -> void:
	_set_resource_root_dir(path, true, true)


# Thin forwarders over EditorResourceLibrary (editor/resource_library.gd): the
# helper owns the index + root-dir state + persistence; the shell applies status
# and settings-popup sync side effects.
func _set_resource_root_dir(path: String, persist: bool, scan: bool) -> Error:
	var result := _resource_library.set_root_dir(path, persist, scan)
	_show_resource_status(result)
	_sync_settings_popup_state()
	_refresh_browser_pane()
	return int(result["err"])


func _scan_resource_root(show_message: bool) -> Error:
	if _resource_library.get_root_dir().is_empty():
		_resource_library.clear_index()
		return OK
	var result := _resource_library.scan_root()
	if show_message:
		_show_resource_status(result)
	_sync_settings_popup_state()
	_refresh_browser_pane()
	return int(result["err"])


func _show_resource_status(result: Dictionary) -> void:
	var status := String(result["status"])
	if status.is_empty():
		return
	show_status_message(status, 4.0 if int(result["err"]) == OK else 6.0)


func _load_resource_state() -> void:
	var state := _resource_library.load_state()
	var view := _resource_library.load_view_state()
	_view_grid_visible = bool(view["grid"])
	_view_axes_visible = bool(view["axes"])
	_resource_library.set_root_dir(String(state["root_dir"]), false, false)


func _save_resource_state() -> void:
	_resource_library.save_state()


func _preferred_resource_root_dir() -> String:
	return _resource_library.get_root_dir()


func _on_workspace_action_pressed(action_id: int) -> void:
	_save_export.run_workspace_action(_get_active_workspace(), action_id)


func _on_environment_action_pressed(action_id: int) -> void:
	_save_export.run_workspace_action(_popup_workspace(), action_id)
	_refresh_environment_popup_state()


func _open_resource_browser(workspace: EditorWorkspace, on_pick: Callable) -> void:
	_resource_browser.open(workspace, on_pick)


## Open the in-app resource picker over an explicit file list (kind-independent), for resource
## types the C++ index does not register (e.g. .adm). Public so inspectors can offer a scoped
## picker via the workspace's editor_shell. `on_pick` receives the chosen entry's name.
func open_file_picker(title: String, files: PackedStringArray, on_pick: Callable) -> void:
	_resource_browser.open_files(title, files, on_pick)


## Open the indexed resource picker over every resource of `kind`. The browse
## affordance behind link widgets (ResourceRefWidget.services_from_shell);
## `on_pick` receives the chosen resource's path.
func open_kind_picker(kind: String, title: String, on_pick: Callable) -> void:
	_resource_browser.open_kind(kind, title, on_pick)


## The shared reference index over the resource root — link widgets resolve
## their validity badges through this.
func get_reference_index() -> NovaReferenceIndex:
	return _resource_library.get_reference_index()


# Resolves the active workspace's current resource path for the browser's
# "(open)" marker; an open popup workspace retargets it for the kinds it
# claims. Stays on the shell (reads popup/workspace state) and is injected
# into EditorResourceBrowser as a capability callable.
func _current_resource_path_for_browser(kind: String) -> String:
	var workspace := _get_active_workspace()
	# The popup panel counts as open whether docked OR floating - the popover
	# hides while the content floats, but its document is still the one the
	# "(open)" marker should follow.
	var popup_open := (_environment_popup != null and _environment_popup.visible) \
			or (_environment_panel_host != null and _environment_panel_host.is_floating())
	var popup_workspace := _popup_workspace()
	if popup_open and popup_workspace != null \
			and kind in popup_workspace.get_open_resource_kinds():
		workspace = popup_workspace
	if workspace != null:
		return workspace.get_current_resource_path()
	return ""


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
	_top_bar.modulate = color
	_workspace_bar.modulate = color
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


## Transient status message in the status bar's tool cell (the shell's public
## notification surface for workspaces, tools, and MCP).
func show_status_message(text: String, duration: float = 4.0) -> void:
	_status.show_status_message(text, duration)


## Saves the workspace's current document, then runs on_done (Save As fallback
## for path-less documents). Public seam for workspaces + the tab strip.
func save_then(workspace: EditorWorkspace, on_done: Callable, failure_message := "Save failed.") -> void:
	_save_export.save_then(workspace, on_done, failure_message)


## Pops the shared unsaved-changes dialog with caller-supplied outcomes. Pair
## with save_then() so path-less documents route through Save As.
func prompt_unsaved_for(on_save: Callable, on_discard: Callable, on_cancel := Callable()) -> void:
	_save_export.prompt_unsaved_for(on_save, on_discard, on_cancel)


func prompt_cdep_violations(count: int, on_fix_callback: Callable) -> void:
	_save_export.prompt_cdep_violations(count, on_fix_callback)


## Export lifecycle, called by exporting workspaces' domain editors.
func on_export_started(dir_path: String) -> void:
	_export_progress.on_export_started(dir_path)


func on_export_completed(err: Error, message: String) -> void:
	_export_progress.on_export_completed(err, message)

