class_name EditorWorkstation
extends Control

const TerrainWorkspaceAdapter = preload("res://modtools/terrain/terrain_workspace.gd")
const EnvironmentWorkspaceAdapter = preload("res://modtools/environment/environment_workspace.gd")
const ObjectWorkspaceAdapter = preload("res://modtools/object/object_workspace.gd")
const MissionWorkspaceAdapter = preload("res://modtools/mission/mission_workspace.gd")
const FontsWorkspaceAdapter = preload("res://modtools/fonts/fonts_workspace.gd")
const CreditsWorkspaceAdapter = preload("res://modtools/credits/credits_workspace.gd")
const StringsWorkspaceAdapter = preload("res://modtools/strings/strings_workspace.gd")
const SoundWorkspaceAdapter = preload("res://modtools/sound/sound_workspace.gd")
const MnuWorkspaceAdapter = preload("res://modtools/mnu/mnu_workspace.gd")
const HudWorkspaceAdapter = preload("res://modtools/hud/hud_workspace.gd")
const MusicWorkspaceAdapter = preload("res://modtools/music/music_workspace.gd")
const AvatarsWorkspaceAdapter = preload("res://modtools/avatar/avatars_workspace.gd")
const ParticleWorkspaceAdapter = preload("res://modtools/particle/particle_workspace.gd")

enum Workspace { TERRAIN, ENVIRONMENT, OBJECT, MISSION, CREDITS, FONTS, STRINGS, MUSIC, SOUND, MNU, HUD, AVATARS, PARTICLE }

# Workspaces are declared as WorkspaceDef rows in _workspace_defs(); the rail
# shows the non-popup ones in order. The enum above stays only as the stable id
# constants those rows (and tests/tools via get_workspace_adapter) key on — the
# shell itself never branches on a specific workspace. Document-action ids are
# ShellActionBar.Action.

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
@onready var _workspace_actions_mount: BoxContainer = %WorkspaceActionsMount
@onready var _modes_label: Label = %ModesLabel
@onready var _mode_rail: VBoxContainer = %ModeRail
@onready var _inspector_mount: Control = %InspectorMount
@onready var _viewport_lane: Control = %ViewportLane
@onready var _viewport_mount: Control = %ViewportMount
@onready var _document_tab_strip: PanelContainer = %DocumentTabStrip
@onready var _document_tab_row: HBoxContainer = %DocumentTabRow
@onready var _camera_toggle_button: Button = %CameraToggleButton
@onready var _camera_popup: PopoverPanel = %CameraPopup
@onready var _camera_popup_close: Button = %CameraPopupClose
@onready var _camera_popup_detach: Button = %CameraPopupDetach
@onready var _camera_popup_content: Control = %CameraPopupContent
@onready var _camera_settings_mount: Control = %CameraSettingsMount
@onready var _environment_toggle_button: Button = %EnvironmentToggleButton
@onready var _environment_popup: PopoverPanel = %EnvironmentPopup
@onready var _environment_popup_title: Label = %EnvironmentPopupTitle
@onready var _environment_popup_close: Button = %EnvironmentPopupClose
@onready var _environment_popup_detach: Button = %EnvironmentPopupDetach
@onready var _environment_popup_content: Control = %EnvironmentPopupContent
@onready var _environment_actions_mount: VBoxContainer = %EnvironmentActionsMount
@onready var _environment_inspector_mount: Control = %EnvironmentInspectorMount
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
@onready var _play_in_game_button: Button = %PlayInGameButton
@onready var _play_current_mission_button: Button = %PlayCurrentMissionButton
@onready var _stop_game_button: Button = %StopGameButton
@onready var _browser_pane_mount: PanelContainer = %ResourceBrowserPaneMount
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
var _resource_library := EditorResourceLibrary.new()
var _mounted_workspace_id: int = -1
var _current_workflow_id: int = -1
var _workflow_buttons: Dictionary = {}
var _inspector_workspace_id: int = -1
var _save_export := ShellSaveExportFlow.new()
var _export_progress := ShellExportProgress.new()
# Managed standalone game launcher (F5/F6/F8, runtime + /d loose override).
var _game_launch := ShellGameLaunch.new()
# App-close guard: one prompt covering every workspace with unsaved work,
# separate from the flow module's per-action UnsavedChangesDialog.
var _close_guard_dialog: ConfirmationDialog
var _resource_browser := EditorResourceBrowser.new()
var _pff_tool := EditorPffTool.new()
# A8 shell modules: persisted layout + browser dock, the popover trio with the
# detachable panels, and the settings popover content (editor/shell/).
var _layout := ShellLayoutPersistence.new()
var _popovers := ShellPopoverDock.new()
var _settings_panel := ShellSettingsPanel.new()
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
		func() -> void: _popovers.set_settings_visible(true),
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
		_workspace_actions_mount,
		Callable(self, "_on_workspace_action_pressed"),
		func() -> bool: return _any_workspace_busy(),
		active_workspace_supplier,
		func() -> Theme: return theme
	)
	_environment_action_bar.setup(
		_environment_actions_mount,
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
	_game_launch.setup(
		_play_in_game_button,
		func() -> String: return _resource_library.get_root_dir(),
		func() -> String: return NovaResourceDirSettings.get_expansion(),
		func() -> String: return NovaResourceDirSettings.get_game(),
		func(path: String, args: PackedStringArray) -> int: return OS.create_process(path, args),
		func(path: String) -> bool: return FileAccess.file_exists(path),
		get_unsaved_workspace_labels,
		show_status_message,
		func() -> Dictionary:
			var mission_workspace := _get_workspace(Workspace.MISSION)
			if mission_workspace == null:
				return {}
			return {
				"path": mission_workspace.get_current_resource_path(),
			},
		func(pid: int) -> bool: return OS.is_process_running(pid),
		func(pid: int) -> int: return OS.kill(pid),
		func() -> int: return Time.get_ticks_msec(),
		_play_current_mission_button,
		_stop_game_button
	)
	_tile_gizmo_overlay.setup(_tile_gizmo, _tile_gizmo_label, _viewport_lane, active_workspace_supplier)
	_tile_gizmo_overlay.wire_buttons(
		_tile_gizmo_done, _tile_gizmo_rotate, _tile_gizmo_flip_x, _tile_gizmo_flip_y, _tile_gizmo_delete)
	_build_workspace_rail()
	_wire_nav_buttons()
	_popovers.setup(
		self,
		_resource_library,
		_environment_action_bar,
		_get_editor_camera,
		_popup_workspace,
		# Cross-module seams route through shell lambdas (Callables that capture
		# this Node) - a module-method Callable would refcount-cycle the two
		# RefCounted modules together.
		func() -> void: _settings_panel.sync_popup_state()
	)
	_popovers.bind_camera_nodes(
		_camera_toggle_button, _camera_popup, _camera_popup_close,
		_camera_popup_detach, _camera_popup_content, _camera_settings_mount)
	_popovers.bind_environment_nodes(
		_environment_toggle_button, _environment_popup, _environment_popup_title,
		_environment_popup_close, _environment_popup_detach,
		_environment_popup_content, _environment_actions_mount,
		_environment_inspector_mount)
	_popovers.bind_settings_nodes(_settings_toggle_button, _settings_popup, _settings_popup_close)
	_settings_panel.setup(
		_resource_library,
		_get_active_workspace,
		_set_resource_root_dir,
		show_status_message,
		_save_export.open_dir_dialog,
		_preferred_resource_root_dir,
		_pff_tool.open,
		_mcp_service,
		func(active: bool) -> void: _popovers.set_settings_visible(active)
	)
	_settings_panel.bind_nodes(
		_settings_resource_dir_edit, _settings_browse_resource_dir_button,
		_settings_apply_resource_dir_button, _settings_recent_row,
		_settings_recent_option, _settings_expansion_row, _settings_view_section,
		_settings_grid_toggle, _settings_axes_toggle, _settings_mcp_toggle,
		_settings_mcp_port_edit, _settings_mcp_status_label, _settings_pff_tool_button)
	_settings_panel.load_view_state()
	_layout.setup(
		self,
		_resource_library,
		_body_row,
		_center_right_split,
		_right_split,
		_browser_toggle_button,
		_browser_pane_mount,
		_asset_dock,
		func() -> void: _scan_resource_root(false),
		_current_resource_path_for_browser,
		func(kind: String, path: String) -> void: open_in_workspace(kind, path)
	)
	_popovers.wire_camera()
	_popovers.wire_environment()
	_popovers.wire_settings()
	_settings_panel.wire()
	_layout.wire_splits()
	_layout.wire_browser_pane()
	_layout.apply_window_min_size()
	get_tree().set_auto_accept_quit(false)
	_context_doc_label.clip_text = true
	_status_context_label.clip_text = true
	_status_camera_label.clip_text = true
	_status_fps_label.clip_text = true
	set_process(true)
	if not _resource_library.get_root_dir().is_empty():
		_scan_resource_root(false)
	_mount_active_workspace_viewport()
	# The initial workspace ID is already selected, so routing it back through
	# set_active_workspace() would hit that method's same-ID early return. Give
	# startup the same mount-then-activate lifecycle as every later switch.
	var initial_workspace := _get_active_workspace()
	if initial_workspace != null:
		initial_workspace.activate()
	_settings_panel.apply_view_guides_to_active()
	_refresh_workspace_surface()
	sync_from_editor_state()
	# Split offsets land after the first container sort so clamp sees real sizes.
	_layout.apply_split_layout.call_deferred()


func _exit_tree() -> void:
	_game_launch.shutdown()
	_popovers.save_floating_states()
	for workspace in _workspaces.values():
		(workspace as EditorWorkspace).release_viewport()
	_clear_viewport_mount()
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


## Stable shell-wide dirty summary shared by run warnings and the close guard.
## F5/F6 only read it; saving remains an explicit editor action.
func get_unsaved_workspace_labels() -> PackedStringArray:
	_ensure_workspaces()
	var dirty := PackedStringArray()
	for def_v in _workspace_defs_cache:
		var def := def_v as WorkspaceDef
		var workspace := _workspace_for_id(def.id)
		if workspace != null and workspace.has_unsaved_changes():
			dirty.append(workspace.get_workspace_label())
	return dirty


func _handle_close_request() -> void:
	var dirty := get_unsaved_workspace_labels()
	if dirty.is_empty():
		_quit_after_game_shutdown()
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
	# OK first proves the editor-owned child is gone; Cancel/Escape dismisses.
	_close_guard_dialog.confirmed.connect(_quit_after_game_shutdown)


func _quit_after_game_shutdown() -> void:
	if not _shutdown_game_for_close():
		return
	get_tree().quit()


func _shutdown_game_for_close() -> bool:
	if _game_launch.shutdown():
		return true
	var session: Variant = _game_launch.get_session() \
			if _game_launch.has_method("get_session") else null
	var reason := String(session.get_last_error()) \
			if session != null and session.has_method("get_last_error") else ""
	if reason.is_empty():
		reason = "Could not stop the running game; the editor remains open."
	show_status_message(reason, 0.0, &"error")
	return false


func set_editor(value: Node) -> void:
	editor = value
	_ensure_workspaces()
	for workspace in _workspaces.values():
		(workspace as EditorWorkspace).bind_to_editor(value)
	for workspace in _popup_workspaces.values():
		(workspace as EditorWorkspace).bind_to_editor(value)
	_popovers.reset_environment_content()
	# A floating environment window must not sit empty until its next toggle.
	var env_mount := _popovers.environment_panel_mount()
	if env_mount != null and env_mount.is_floating():
		_popovers.ensure_environment_content()
	_popovers.sync_camera_editor()
	_remount_active_workspace_viewport()
	_refresh_workspace_surface()
	for child in _inspector_mount.get_children():
		if child.has_method("set_editor"):
			child.set_editor(value)
	sync_from_editor_state()


## The single standalone game session consumed by toolbar, shortcuts and MCP.
## Callers observe/control it through its public JSON-safe surface.
func get_game_run_session() -> ShellGameSession:
	return _game_launch.get_session()


func run_game(mode: String = "game") -> bool:
	var normalized := mode.strip_edges().to_lower()
	if normalized != "game" and normalized != "mission":
		show_status_message("Unknown run mode '%s'." % mode, 0.0, &"error")
		return false
	return _game_launch.run_current_mission() \
			if normalized == "mission" else _game_launch.launch()


func stop_game() -> bool:
	return _game_launch.stop()


func sync_from_editor_state() -> void:
	_ensure_workspaces()
	_refresh_workspace_buttons()
	_refresh_workflow_from_workspace()
	_refresh_shell_state()
	_status.refresh()
	_tile_gizmo_overlay.refresh()
	_export_progress.sync()
	_popovers.refresh_camera_state()
	_popovers.refresh_environment_state()
	_game_launch.refresh()
	var workspace := _get_active_workspace()
	if workspace != null:
		workspace.sync_asset_dock()
	# Re-apply the persisted dock widths so re-showing the right dock on a
	# workspace switch restores its dragged size instead of the scene default.
	_layout.apply_split_layout()


func _process(_delta: float) -> void:
	_game_launch.poll()
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
		WorkspaceDef.make(Workspace.AVATARS, AvatarsWorkspaceAdapter, false, &"World", &"avatar"),
		WorkspaceDef.make(Workspace.FONTS, FontsWorkspaceAdapter, false, &"Interface", &"fonts"),
		WorkspaceDef.make(Workspace.CREDITS, CreditsWorkspaceAdapter, false, &"Interface", &"credits"),
		WorkspaceDef.make(Workspace.STRINGS, StringsWorkspaceAdapter, false, &"Interface", &"strings"),
		WorkspaceDef.make(Workspace.MNU, MnuWorkspaceAdapter, false, &"Interface", &"menu"),
		WorkspaceDef.make(Workspace.HUD, HudWorkspaceAdapter, false, &"Interface", &"hud"),
		WorkspaceDef.make(Workspace.MUSIC, MusicWorkspaceAdapter, false, &"Audio", &"music"),
		WorkspaceDef.make(Workspace.PARTICLE, ParticleWorkspaceAdapter, false, &"Atmosphere", &"particle"),
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


# Local-only active-row emphasis (a stylebox no themed control carries). The themed
# pressed background is darker than normal, so in a tall uniform list a hovered
# inactive row can read heavier than the active one; a faint accent fill fixes that.
func _make_workspace_active_stylebox() -> StyleBoxFlat:
	var sb := StyleBoxFlat.new()
	var accent := get_theme_color(&"accent", &"EditorPalette")
	sb.bg_color = Color(accent, 0.22)
	sb.border_color = Color(accent, 0.9)
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
	if _workspace_actions_mount == null:
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
		_popovers.set_environment_visible(true)
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
	_settings_panel.apply_view_guides_to_active()
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
func open_in_workspace(kind: String, path: String, focus: FocusPayload = null) -> Error:
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
			show_status_message("Could not open %s." % clean_path.get_file(), 0.0, &"error")
			return err
	if _active_workspace_id != workspace_id:
		set_active_workspace(workspace_id)
	else:
		_refresh_workspace_surface()
		sync_from_editor_state()
	# A focus miss below still navigated, so the departure records either way.
	_record_nav_departure(from)
	if focus != null and not focus.is_empty():
		var focus_err: Error = workspace.focus_reference(focus)
		if focus_err != OK:
			show_status_message("Opened %s; not found: %s" % [clean_path.get_file(), focus.describe()], 0.0, &"warn")
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
		show_status_message("Font not found: %s" % clean_name, 0.0, &"error")
		return ERR_DOES_NOT_EXIST
	var err := open_in_workspace("font", path)
	if err != OK:
		return err
	show_status_message("Opened font %s." % clean_name, 0.0, &"success")
	return OK


# Cross-jump used by the Menus workspace's "Edit in Strings": open the menu's resolved
# text table in the Strings workspace and focus the given key. table_path is an
# absolute path (already resolved by the caller).
func open_strings_workspace(table_path: String, key: String) -> Error:
	var err := open_in_workspace("strings", table_path, FocusPayload.for_key(key))
	if err != OK:
		return err
	show_status_message("Editing string %s." % (key if not key.is_empty() else table_path.get_file()), 3.0, &"info")
	return OK


# Cross-jump used by the Menus workspace's cross-file ACTION rows. Resolve a
# .mnu name/path against the configured resource root, open it in Menus, then
# focus the target screen when supplied.
func open_menu_workspace(file: String, screen: String = "") -> Error:
	var path := _resolve_menu_action_path(file)
	if path.is_empty():
		show_status_message("Menu not found: %s" % file.strip_edges(), 0.0, &"error")
		return ERR_FILE_NOT_FOUND
	var target := screen.strip_edges()
	var err := open_in_workspace("menu", path, FocusPayload.for_screen(target) if not target.is_empty() else null)
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
func _location_snapshot() -> EditorNavLocation:
	var workspace := _get_active_workspace()
	var path := String(workspace.get_current_resource_path()) if workspace != null else ""
	return EditorNavLocation.make(_active_workspace_id, path)


# Record `from` if the navigation that just ran actually moved the user.
# Same-location jumps (focus-only, failed opens, the Environment popup) leave
# the location unchanged and record nothing.
func _record_nav_departure(from: EditorNavLocation) -> void:
	if EditorNavLocation.same(from, _location_snapshot()):
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
		show_status_message("Could not open %s." % entry.path.get_file(), 0.0, &"error")
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
		show_status_message("Could not open %s." % entry.path.get_file(), 0.0, &"error")
	_refresh_nav_buttons()


# Restore a history entry: reopen its document if it changed, then switch
# workspaces. Resolves through _get_workspace (never _workspace_for_id) so a
# corrupt entry cannot route to the Environment popup. Mirrors
# open_in_workspace's same-path skip, so returning to a still-open document
# keeps selection, tabs, and undo intact — and a failed reopen returns before
# any workspace switch, leaving the user where they were.
func _navigate_to(entry: EditorNavLocation) -> Error:
	var workspace_id := entry.workspace_id
	var workspace := _get_workspace(workspace_id)
	if workspace == null:
		return ERR_UNAVAILABLE
	var entry_path := entry.path
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


func _nav_tooltip(verb: String, entry: EditorNavLocation) -> String:
	if entry == null:
		return verb
	var workspace := _get_workspace(entry.workspace_id)
	var label := String(workspace.get_workspace_label()) if workspace != null else ""
	if label.is_empty():
		return verb
	var file := entry.path.get_file()
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
	if _viewport_mount == null:
		return
	_clear_viewport_mount()
	var workspace := _get_active_workspace()
	if workspace == null:
		_mounted_workspace_id = -1
		return
	workspace.mount_viewport(_viewport_mount)
	_mounted_workspace_id = _active_workspace_id


func _unmount_workspace_viewport(workspace_id: int, workspace: EditorWorkspace) -> void:
	if _viewport_mount == null:
		return
	if workspace != null:
		workspace.unmount_viewport(_viewport_mount)
	_clear_viewport_mount()
	if _mounted_workspace_id == workspace_id:
		_mounted_workspace_id = -1


func _remount_active_workspace_viewport() -> void:
	var workspace := _get_active_workspace()
	_unmount_workspace_viewport(_active_workspace_id, workspace)
	_mount_active_workspace_viewport()


func _clear_viewport_mount() -> void:
	if _viewport_mount == null:
		return
	for child in _viewport_mount.get_children():
		_viewport_mount.remove_child(child)


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
	# Whole left lane (picker + inspector mount) is opt-out: a workspace that lives
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
		_layout.sync_right_split_visibility()
		return
	_asset_dock.visible = true
	workspace.set_asset_dock(_asset_dock)
	_asset_dock_workspace_id = _active_workspace_id
	_layout.sync_right_split_visibility()


func _clear_asset_dock_children() -> void:
	for child in _asset_dock.get_children():
		_asset_dock.remove_child(child)
		child.free()


func _show_workspace_inspector(workspace: EditorWorkspace) -> void:
	if _inspector_workspace_id == _active_workspace_id:
		return
	_inspector_workspace_id = _active_workspace_id
	for child in _inspector_mount.get_children():
		child.queue_free()
	if workspace != null:
		workspace.build_inspector(_inspector_mount)
		if _inspector_mount.get_child_count() > 0:
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
	_build_empty_state_panel(_inspector_mount, message, icon_id)


# A centered call-to-action for an empty inspector: the workspace icon, a one-line
# message, and the workspace's own New/Open actions plus quick-open. The buttons
# route through the same handlers as the top-bar toolbar, so behavior is identical.
func _build_empty_state_panel(mount: Control, message: String, icon_id: StringName) -> void:
	var panel := VBoxContainer.new()
	panel.name = "EmptyStatePanel"
	panel.alignment = BoxContainer.ALIGNMENT_CENTER
	panel.add_theme_constant_override("separation", 14)
	panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	mount.add_child(panel)

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


# Quick-open: reveal the resource browser pane and focus its search box (Ctrl+P,
# the empty-state "Browse resources" button, and external callers). The pane
# itself lives in ShellLayoutPersistence.
func focus_browser_pane() -> void:
	_layout.focus_browser_pane()


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


# Global document shortcuts (B6). _shortcut_input fires after _gui_input — a
# focused text field keeps its native Ctrl+Z — and before _unhandled_input,
# so viewport routers never see claimed keys. ONE focus guard, for undo/redo
# only: Ctrl+S is deliberately unguarded (the save flow flushes pending
# edits, which covers half-typed buffers).
func _shortcut_input(event: InputEvent) -> void:
	if not (event is InputEventKey):
		return
	var key := event as InputEventKey
	if not key.pressed or key.is_echo():
		return
	if not key.ctrl_pressed and not key.alt_pressed:
		match key.keycode:
			KEY_F5:
				run_game("game")
				get_viewport().set_input_as_handled()
				return
			KEY_F6:
				run_game("mission")
				get_viewport().set_input_as_handled()
				return
			KEY_F8:
				stop_game()
				get_viewport().set_input_as_handled()
				return
	if not key.ctrl_pressed or key.alt_pressed:
		return
	var workspace := _get_active_workspace()
	match key.keycode:
		KEY_S:
			if workspace == null:
				return
			if key.shift_pressed:
				if _save_export.flush_workspace_or_status(workspace):
					_save_export.open_save_as_dialog(workspace)
			else:
				_save_export.on_save_pressed(workspace)
			sync_from_editor_state()
			get_viewport().set_input_as_handled()
		KEY_Z, KEY_Y:
			if workspace == null or _shortcut_focus_blocks_undo():
				return
			if key.keycode == KEY_Y or key.shift_pressed:
				if workspace.can_redo():
					workspace.redo()
			elif workspace.can_undo():
				workspace.undo()
			sync_from_editor_state()
			get_viewport().set_input_as_handled()


func _shortcut_focus_blocks_undo() -> bool:
	var focus := get_viewport().gui_get_focus_owner()
	return focus is LineEdit or focus is TextEdit or focus is SpinBox


func _unhandled_input(event: InputEvent) -> void:
	if event is InputEventKey:
		var key := event as InputEventKey
		if key.pressed and not key.is_echo() and key.keycode == KEY_ESCAPE:
			# The native confirm/export dialogs own their own Escape handling
			# (they emit canceled and hide), so only the corner popovers need it
			# routed here.
			if _popovers.handle_escape():
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


func show_environment_dialog() -> void:
	_popovers.set_environment_visible(true)


# The workspace behind the single popup surface (%EnvironmentPopup). One popup
# workspace exists today; a second requires generalizing the surface too.
func _popup_workspace() -> EditorWorkspace:
	for workspace in _popup_workspaces.values():
		return workspace
	return null


func _mcp_service() -> Node:
	return editor.get("mcp_service") if editor != null else null


func _ensure_resource_index() -> void:
	_resource_library.ensure_index()


func get_resource_index() -> RefCounted:
	return _resource_library.get_index()


func get_resource_root_dir() -> String:
	return _resource_library.get_root_dir()


func get_resource_root() -> NovaResourceRoot:
	return _resource_library.get_resource_root()


## Public root-dir entry (ADR 0018 — probes/tests come through here, not the
## private forwarder). persist=false leaves the user's saved root untouched
## (the headless/windowed probes mount retail dirs transiently).
func set_resource_root_dir(path: String, persist: bool = true, scan: bool = true) -> void:
	_set_resource_root_dir(path, persist, scan)


# Thin forwarders over EditorResourceLibrary (editor/resource_library.gd): the
# helper owns the index + root-dir state + persistence; the shell applies status
# and settings-popup sync side effects.
func _set_resource_root_dir(path: String, persist: bool, scan: bool) -> Error:
	var result := _resource_library.set_root_dir(path, persist, scan)
	_show_resource_status(result)
	_settings_panel.sync_popup_state()
	_layout.refresh_browser_pane()
	_game_launch.refresh()
	return int(result["err"])


func _scan_resource_root(show_message: bool) -> Error:
	if _resource_library.get_root_dir().is_empty():
		_resource_library.clear_index()
		return OK
	var result := _resource_library.scan_root()
	if show_message:
		_show_resource_status(result)
	_settings_panel.sync_popup_state()
	_layout.refresh_browser_pane()
	_game_launch.refresh()
	return int(result["err"])


func _show_resource_status(result: Dictionary) -> void:
	var status := String(result["status"])
	if status.is_empty():
		return
	show_status_message(status, 0.0, &"info" if int(result["err"]) == OK else &"error")


func _load_resource_state() -> void:
	# View-guide state loads in _ready once the settings module is bound.
	var state := _resource_library.load_state()
	_resource_library.set_root_dir(String(state["root_dir"]), false, false)


func _save_resource_state() -> void:
	_resource_library.save_state()


func _preferred_resource_root_dir() -> String:
	return _resource_library.get_root_dir()


func _on_workspace_action_pressed(action_id: int) -> void:
	_save_export.run_workspace_action(_get_active_workspace(), action_id)


func _on_environment_action_pressed(action_id: int) -> void:
	_save_export.run_workspace_action(_popup_workspace(), action_id)
	_popovers.refresh_environment_state()


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
	var popup_open := _popovers.environment_open()
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
	_popovers.refresh_environment_state()
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
	for child in _inspector_mount.get_children():
		child.queue_free()
	if workspace != null:
		workspace.build_workflow_inspector(workflow_id, _inspector_mount)
	if _inspector_mount.get_child_count() > 0:
		return
	var placeholder := Label.new()
	placeholder.text = "Workflow inspector coming soon"
	placeholder.theme_type_variation = &"Muted"
	placeholder.horizontal_alignment = HORIZONTAL_ALIGNMENT_CENTER
	placeholder.vertical_alignment = VERTICAL_ALIGNMENT_CENTER
	placeholder.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	placeholder.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_inspector_mount.add_child(placeholder)


## Transient status message in the status bar's tool cell (the shell's public
## notification surface for workspaces, tools, and MCP).
func show_status_message(text: String, duration: float = 0.0, severity: StringName = &"info") -> void:
	_status.show_status_message(text, duration, severity)


## Re-sync the workflow rail + inspector after a workspace changed its active
## workflow programmatically (e.g. the particle blueprint selecting a node of a
## different kind). Public seam for EditorWorkspace._sync_shell_workflow().
func sync_workflow_from_workspace() -> void:
	_refresh_workflow_from_workspace()


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
