class_name ShellSettingsPanel
extends RefCounted

## The settings popover's CONTENT: the resource-directory controls, the
## recent-directories dropdown, the View (grid/axes) guide toggles and their
## persisted state, the MCP service controls, and the PFF archive tool button.
## Visibility (the popover joins the camera/environment mutual exclusion)
## belongs to ShellPopoverDock.

# Item metadata sentinel for the "Clear list" entry in the recent-directories
# dropdown; real entries carry their path, the disabled placeholder carries "".
const _RECENT_CLEAR_META := "::clear::"

var _resource_library: EditorResourceLibrary
# func() -> EditorWorkspace: the active workspace (view-guide targeting).
var _get_active_workspace: Callable
# func(path, persist, scan) -> Error: the shell's root-dir apply chain
# (status toast + settings sync + browser refresh side effects live there).
var _set_resource_root_dir: Callable
# func(text, duration): the shell's status toast.
var _show_status: Callable
# func(title, on_pick, current_dir): the flow module's directory picker.
var _open_dir_dialog: Callable
# func() -> String: seed directory for pickers.
var _preferred_resource_root_dir: Callable
# func(dir) -> void: open the PFF archive tool.
var _open_pff_tool: Callable
# func() -> Node: the MCP service on the editor root (null in runtime builds).
var _mcp_service: Callable
# func(active) -> void: the dock's settings visibility (PFF button closes it).
var _set_settings_visible: Callable

var _settings_resource_dir_edit: LineEdit
var _settings_browse_resource_dir_button: Button
var _settings_apply_resource_dir_button: Button
var _settings_recent_row: HBoxContainer
var _settings_recent_option: OptionButton
var _settings_expansion_row: HBoxContainer
var _settings_view_section: VBoxContainer
var _settings_grid_toggle: CheckBox
var _settings_axes_toggle: CheckBox
var _settings_mcp_toggle: CheckBox
var _settings_mcp_port_edit: LineEdit
var _settings_mcp_status_label: Label
var _settings_pff_tool_button: Button

# 3D-preview guide visibility, shared across guide-capable workspaces and pushed
# to the active one. Loaded from / saved to the editor-state config.
var _view_grid_visible: bool = true
var _view_axes_visible: bool = true


func setup(
	resource_library: EditorResourceLibrary,
	get_active_workspace: Callable,
	set_resource_root_dir: Callable,
	show_status: Callable,
	open_dir_dialog: Callable,
	preferred_resource_root_dir: Callable,
	open_pff_tool: Callable,
	mcp_service: Callable,
	set_settings_visible: Callable
) -> void:
	_resource_library = resource_library
	_get_active_workspace = get_active_workspace
	_set_resource_root_dir = set_resource_root_dir
	_show_status = show_status
	_open_dir_dialog = open_dir_dialog
	_preferred_resource_root_dir = preferred_resource_root_dir
	_open_pff_tool = open_pff_tool
	_mcp_service = mcp_service
	_set_settings_visible = set_settings_visible


func bind_nodes(
	resource_dir_edit: LineEdit, browse_button: Button, apply_button: Button,
	recent_row: HBoxContainer, recent_option: OptionButton,
	expansion_row: HBoxContainer, view_section: VBoxContainer,
	grid_toggle: CheckBox, axes_toggle: CheckBox,
	mcp_toggle: CheckBox, mcp_port_edit: LineEdit, mcp_status_label: Label,
	pff_tool_button: Button
) -> void:
	_settings_resource_dir_edit = resource_dir_edit
	_settings_browse_resource_dir_button = browse_button
	_settings_apply_resource_dir_button = apply_button
	_settings_recent_row = recent_row
	_settings_recent_option = recent_option
	_settings_expansion_row = expansion_row
	_settings_view_section = view_section
	_settings_grid_toggle = grid_toggle
	_settings_axes_toggle = axes_toggle
	_settings_mcp_toggle = mcp_toggle
	_settings_mcp_port_edit = mcp_port_edit
	_settings_mcp_status_label = mcp_status_label
	_settings_pff_tool_button = pff_tool_button


func wire() -> void:
	if _settings_browse_resource_dir_button != null and not _settings_browse_resource_dir_button.pressed.is_connected(_on_browse_resource_dir_pressed):
		_settings_browse_resource_dir_button.pressed.connect(_on_browse_resource_dir_pressed)
	if _settings_apply_resource_dir_button != null and not _settings_apply_resource_dir_button.pressed.is_connected(_on_apply_resource_dir_pressed):
		_settings_apply_resource_dir_button.pressed.connect(_on_apply_resource_dir_pressed)
	if _settings_resource_dir_edit != null and not _settings_resource_dir_edit.text_submitted.is_connected(_on_resource_dir_submitted):
		_settings_resource_dir_edit.text_submitted.connect(_on_resource_dir_submitted)
	if _settings_recent_option != null and not _settings_recent_option.item_selected.is_connected(_on_recent_selected):
		_settings_recent_option.item_selected.connect(_on_recent_selected)
	if _settings_grid_toggle != null and not _settings_grid_toggle.toggled.is_connected(_on_grid_toggled):
		_settings_grid_toggle.toggled.connect(_on_grid_toggled)
	if _settings_axes_toggle != null and not _settings_axes_toggle.toggled.is_connected(_on_axes_toggled):
		_settings_axes_toggle.toggled.connect(_on_axes_toggled)
	if _settings_pff_tool_button != null and not _settings_pff_tool_button.pressed.is_connected(_on_pff_tool_pressed):
		_settings_pff_tool_button.pressed.connect(_on_pff_tool_pressed)
	if _settings_mcp_toggle != null and not _settings_mcp_toggle.toggled.is_connected(_on_mcp_toggled):
		_settings_mcp_toggle.toggled.connect(_on_mcp_toggled)
	if _settings_mcp_port_edit != null and not _settings_mcp_port_edit.text_submitted.is_connected(_on_mcp_port_submitted):
		_settings_mcp_port_edit.text_submitted.connect(_on_mcp_port_submitted)
	sync_popup_state()


# --- Popup content state --------------------------------------------------------

func sync_popup_state() -> void:
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
		var workspace := _get_active_workspace.call() as EditorWorkspace
		_settings_view_section.visible = workspace != null and workspace.shows_view_guides()
	sync_mcp_state()


# Reflect the MCP service's live state into the Settings popup (toggle, port,
# status line). The service lives on the editor root; runtime builds have none
# and the controls simply show Stopped/disabled.
func sync_mcp_state() -> void:
	var service := _mcp_service.call() as Node
	if service != null and service.has_signal("status_changed") \
			and not service.is_connected(
					"status_changed", Callable(self, "sync_mcp_state")):
		service.connect("status_changed", Callable(self, "sync_mcp_state"))
	if _settings_mcp_toggle != null:
		_settings_mcp_toggle.set_pressed_no_signal(service != null and service.is_running())
		_settings_mcp_toggle.disabled = service == null
	if _settings_mcp_port_edit != null and not _settings_mcp_port_edit.has_focus():
		_settings_mcp_port_edit.text = str(McpSettings.get_port())
	if _settings_mcp_status_label != null:
		_settings_mcp_status_label.text = service.get_status_text() if service != null else "Unavailable in this build"


func _on_mcp_toggled(pressed: bool) -> void:
	var service := _mcp_service.call() as Node
	if service != null:
		service.set_enabled(pressed)
	sync_mcp_state()


func _on_mcp_port_submitted(text: String) -> void:
	var service := _mcp_service.call() as Node
	if not text.is_valid_int():
		_show_status.call("MCP port must be a number (1024-65535).", 5.0)
		sync_mcp_state()
		return
	var port := clampi(text.to_int(), 1024, 65535)
	if service != null:
		service.apply_port(port)
	else:
		McpSettings.set_port(port)
	sync_mcp_state()


func _on_pff_tool_pressed() -> void:
	# Close the settings popover so the modal archive tool isn't competing with it,
	# then open the tool seeded at the configured resource directory.
	_set_settings_visible.call(false)
	_open_pff_tool.call(_preferred_resource_root_dir.call())


# --- Resource directory ----------------------------------------------------------

func _on_browse_resource_dir_pressed() -> void:
	var dir_edit := _settings_resource_dir_edit
	var on_pick := func(path: String) -> void:
		if dir_edit != null:
			dir_edit.text = path
		apply_resource_settings(true)
	_open_dir_dialog.call("Select resource directory", on_pick, _preferred_resource_root_dir.call())


func _on_apply_resource_dir_pressed() -> void:
	apply_resource_settings(true)


func _on_resource_dir_submitted(_text: String) -> void:
	apply_resource_settings(true)


func apply_resource_settings(scan: bool, persist: bool = true) -> Error:
	var path := _resource_library.get_root_dir()
	if _settings_resource_dir_edit != null:
		path = _settings_resource_dir_edit.text
	return _set_resource_root_dir.call(path, persist, scan)


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
func _on_recent_selected(index: int) -> void:
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
	apply_resource_settings(true)


# --- View guides ------------------------------------------------------------------

func load_view_state() -> void:
	var view := _resource_library.load_view_state()
	_view_grid_visible = bool(view["grid"])
	_view_axes_visible = bool(view["axes"])


func _on_grid_toggled(pressed: bool) -> void:
	if _view_grid_visible == pressed:
		return
	_view_grid_visible = pressed
	_resource_library.save_view_state(_view_grid_visible, _view_axes_visible)
	apply_view_guides_to_active()


func _on_axes_toggled(pressed: bool) -> void:
	if _view_axes_visible == pressed:
		return
	_view_axes_visible = pressed
	_resource_library.save_view_state(_view_grid_visible, _view_axes_visible)
	apply_view_guides_to_active()


# Push the persisted guide visibility onto the active workspace (no-op for ones
# without a 3D guide overlay). Called when the choice changes and when a
# guide-capable workspace becomes active.
func apply_view_guides_to_active() -> void:
	var workspace := _get_active_workspace.call() as EditorWorkspace
	if workspace == null or not workspace.shows_view_guides():
		return
	workspace.set_grid_visible(_view_grid_visible)
	workspace.set_axes_visible(_view_axes_visible)
