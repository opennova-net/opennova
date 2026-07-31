class_name ShellLayoutPersistence
extends RefCounted

## The shell's persisted layout: split offsets, the window minimum size, and
## the Resource Browser pane (a DOCK, not a popover: it never joins the
## camera/environment/settings mutual exclusion or the Escape handler, and its
## visibility + split width persist across sessions). Startup applies are
## read-only so test instantiations never persist a layout they did not change.

var _shell: Control
var _resource_library: EditorResourceLibrary
var _body_row: SplitContainer
var _center_right_split: SplitContainer
var _right_split: SplitContainer
var _browser_toggle_button: Button
var _browser_pane_mount: PanelContainer
var _asset_dock: Control
# func() -> void: rescan of the configured resource root (no status toast).
var _scan_resource_root: Callable
# func(kind) -> String: the active document path for the "(open)" marker.
var _current_resource_path: Callable
# func(kind, path) -> void: the shared cross-jump spine.
var _open_in_workspace: Callable

# The persistent Resource Browser pane (lazy: built on first show).
var _browser_pane: ResourceBrowserPane


func setup(
	shell: Control,
	resource_library: EditorResourceLibrary,
	body_row: SplitContainer,
	center_right_split: SplitContainer,
	right_split: SplitContainer,
	browser_toggle_button: Button,
	browser_pane_mount: PanelContainer,
	asset_dock: Control,
	scan_resource_root: Callable,
	current_resource_path: Callable,
	open_in_workspace: Callable
) -> void:
	_shell = shell
	_resource_library = resource_library
	_body_row = body_row
	_center_right_split = center_right_split
	_right_split = right_split
	_browser_toggle_button = browser_toggle_button
	_browser_pane_mount = browser_pane_mount
	_asset_dock = asset_dock
	_scan_resource_root = scan_resource_root
	_current_resource_path = current_resource_path
	_open_in_workspace = open_in_workspace


# --- Splits + window floor -------------------------------------------------

func wire_splits() -> void:
	if _body_row != null and not _body_row.drag_ended.is_connected(save_split_layout):
		_body_row.drag_ended.connect(save_split_layout)
	if _center_right_split != null and not _center_right_split.drag_ended.is_connected(save_split_layout):
		_center_right_split.drag_ended.connect(save_split_layout)


func apply_window_min_size() -> void:
	# The editor needs a usable floor; Godot has no project setting for this, so
	# the window minimum is set at runtime. Only the editor shell does this, so
	# the runtime game window is unaffected.
	var window := _shell.get_window()
	if window != null:
		window.min_size = Vector2i(1024, 640)


# Read-only: applies the persisted split offsets without ever writing the config,
# so test instantiations never persist a layout. A side is only applied when it
# was actually stored (has_left/has_right), leaving the scene default otherwise.
# We set split_offset directly and let the container's resort keep children
# within their minimum sizes; calling clamp_split_offset() explicitly throws
# before the first sort (and when the dock is hidden), so it is avoided.
func apply_split_layout() -> void:
	if _body_row == null or _center_right_split == null:
		return
	var state := _resource_library.load_layout_state()
	if bool(state["has_left"]):
		_body_row.split_offset = int(state["left"])
	if bool(state["has_right"]):
		_center_right_split.split_offset = int(state["right"])


func save_split_layout() -> void:
	if _body_row == null or _center_right_split == null:
		return
	_resource_library.save_layout_state(_body_row.split_offset, _center_right_split.split_offset)


# --- Resource Browser pane ---------------------------------------------------

func wire_browser_pane() -> void:
	if _browser_toggle_button != null:
		_browser_toggle_button.icon = EditorIconLibrary.resolve(&"browser")
		if not _browser_toggle_button.toggled.is_connected(set_browser_pane_visible):
			_browser_toggle_button.toggled.connect(set_browser_pane_visible)
	if _right_split != null and not _right_split.drag_ended.is_connected(_save_browser_state):
		_right_split.drag_ended.connect(_save_browser_state)
	# Startup restore is a read-only apply (mirrors apply_split_layout): test
	# instantiations must never persist a layout they did not change.
	var state := _resource_library.load_browser_state()
	if _right_split != null and bool(state["has_split"]):
		_right_split.split_offset = int(state["split"])
	if bool(state["visible"]) and _browser_pane_mount != null:
		_ensure_browser_pane()
		_browser_pane_mount.visible = true
		if _browser_toggle_button != null:
			_browser_toggle_button.set_pressed_no_signal(true)
		# Refresh now only when no root is configured (nothing will scan later);
		# with a root, _ready's scan fills the pane through refresh_browser_pane
		# - an eager refresh here would lazy-scan and double the startup index walk.
		if _resource_library.get_root_dir().is_empty():
			_browser_pane.refresh()
	sync_right_split_visibility()


func browser_pane() -> ResourceBrowserPane:
	return _browser_pane


func _ensure_browser_pane() -> void:
	if _browser_pane != null and is_instance_valid(_browser_pane):
		return
	var library := _resource_library
	_browser_pane = ResourceBrowserPane.new()
	_browser_pane.name = "ResourceBrowserPane"
	# Capabilities only - the pane's double-click rides the same cross-jump
	# spine as the link widgets (open_in_workspace), never a private open path.
	_browser_pane.setup(
		func() -> RefCounted: return library.get_index(),
		func() -> String: return library.get_root_dir(),
		_scan_resource_root,
		_current_resource_path,
		_open_in_workspace
	)
	_browser_pane_mount.add_child(_browser_pane)


func set_browser_pane_visible(active: bool) -> void:
	if _browser_pane_mount == null:
		return
	if active:
		_ensure_browser_pane()
	_browser_pane_mount.visible = active
	if _browser_toggle_button != null:
		_browser_toggle_button.set_pressed_no_signal(active)
	sync_right_split_visibility()
	if active and _browser_pane != null:
		_browser_pane.refresh()
	_save_browser_state()


# Quick-open: reveal the resource browser pane and focus its search box (Ctrl+P,
# and the empty-state "Browse resources" button). The table's focus_search()
# defers the grab, so calling it right after the pane is shown is safe.
func focus_browser_pane() -> void:
	set_browser_pane_visible(true)
	if _browser_pane != null and is_instance_valid(_browser_pane):
		_browser_pane.table.focus_search()


# With both children hidden, RightSplit itself hides so dockless workspaces
# keep the pre-pane behavior: no live divider, and the persisted right offset
# stays inert (CenterRightSplit sees one visible child).
func sync_right_split_visibility() -> void:
	if _right_split == null:
		return
	_right_split.visible = (_asset_dock != null and _asset_dock.visible) \
			or (_browser_pane_mount != null and _browser_pane_mount.visible)


func _save_browser_state() -> void:
	if _browser_pane_mount == null or _right_split == null:
		return
	_resource_library.save_browser_state(_browser_pane_mount.visible, _right_split.split_offset)


# Keep a visible pane truthful after the root changes or a rescan.
func refresh_browser_pane() -> void:
	if _browser_pane != null and is_instance_valid(_browser_pane) \
			and _browser_pane_mount != null and _browser_pane_mount.visible:
		_browser_pane.refresh()
