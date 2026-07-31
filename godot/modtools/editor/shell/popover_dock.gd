class_name ShellPopoverDock
extends RefCounted

## The shell's corner popovers as one surface: camera / environment / settings
## visibility with their mutual exclusion and Escape routing, the detachable
## camera/environment panel mounts with their persisted floating preference
## (restore dicts cache the config so per-toggle decisions never re-read it),
## and the camera/environment popover content builders. The settings popover's
## CONTENT lives in ShellSettingsPanel; only its visibility (it joins the
## exclusion trio) is owned here.

const CameraSettingsPanelScene = preload("res://modtools/terrain/ui/camera_settings_panel.tscn")

var _shell: Control
var _resource_library: EditorResourceLibrary
# The env popover's own action bar (a second ShellActionBar instance).
var _environment_action_bar: ShellActionBar
# func() -> Camera3D: the active workspace's camera capability hook.
var _get_editor_camera: Callable
# func() -> EditorWorkspace: the single popup workspace behind %EnvironmentPopup.
var _popup_workspace: Callable
# func() -> void: refresh the settings popover content before it shows.
var _sync_settings_content: Callable

var _camera_toggle_button: Button
var _camera_popup: PopoverPanel
var _camera_popup_close: Button
var _camera_popup_detach: Button
var _camera_popup_content: Control
var _camera_settings_mount: Control
var _environment_toggle_button: Button
var _environment_popup: PopoverPanel
var _environment_popup_title: Label
var _environment_popup_close: Button
var _environment_popup_detach: Button
var _environment_popup_content: Control
var _environment_actions_mount: VBoxContainer
var _environment_inspector_mount: Control
var _settings_toggle_button: Button
var _settings_popup: PopoverPanel
var _settings_popup_close: Button

var _camera_settings_panel: Control
# Detachable panels (B6): the camera/environment popovers can pop their
# content into floating windows. State machines live in the mounts; the cached
# restore dicts make persisted "open floating" decisions without re-reading
# the config per toggle (the save handlers keep them current).
var _camera_panel_mount: DetachablePanelMount
var _environment_panel_mount: DetachablePanelMount
var _panel_restore: Dictionary = {}


func setup(
	shell: Control,
	resource_library: EditorResourceLibrary,
	environment_action_bar: ShellActionBar,
	get_editor_camera: Callable,
	popup_workspace: Callable,
	sync_settings_content: Callable
) -> void:
	_shell = shell
	_resource_library = resource_library
	_environment_action_bar = environment_action_bar
	_get_editor_camera = get_editor_camera
	_popup_workspace = popup_workspace
	_sync_settings_content = sync_settings_content


func bind_camera_nodes(
	toggle: Button, popup: PopoverPanel, close: Button, detach: Button,
	content: Control, settings_mount: Control
) -> void:
	_camera_toggle_button = toggle
	_camera_popup = popup
	_camera_popup_close = close
	_camera_popup_detach = detach
	_camera_popup_content = content
	_camera_settings_mount = settings_mount


func bind_environment_nodes(
	toggle: Button, popup: PopoverPanel, title: Label, close: Button,
	detach: Button, content: Control, actions_mount: VBoxContainer,
	inspector_mount: Control
) -> void:
	_environment_toggle_button = toggle
	_environment_popup = popup
	_environment_popup_title = title
	_environment_popup_close = close
	_environment_popup_detach = detach
	_environment_popup_content = content
	_environment_actions_mount = actions_mount
	_environment_inspector_mount = inspector_mount


func bind_settings_nodes(toggle: Button, popup: PopoverPanel, close: Button) -> void:
	_settings_toggle_button = toggle
	_settings_popup = popup
	_settings_popup_close = close


# --- Wiring -------------------------------------------------------------------

func wire_camera() -> void:
	if _camera_popup != null:
		_camera_popup.visible = false
		_camera_popup.apply_anchor(320.0)
		_camera_popup.bind_close(_camera_popup_close)
		_camera_popup.bind_detach(_camera_popup_detach)
		if not _camera_popup.close_requested.is_connected(_on_camera_popup_close_pressed):
			_camera_popup.close_requested.connect(_on_camera_popup_close_pressed)
		if not _camera_popup.detach_requested.is_connected(detach_camera_panel):
			_camera_popup.detach_requested.connect(detach_camera_panel)
	if _camera_panel_mount == null and _camera_popup_content != null:
		_camera_panel_mount = DetachablePanelMount.new(&"camera", "Camera", Vector2i(344, 320))
		_camera_panel_mount.setup(_camera_popup_content, _shell, _save_camera_panel_state)
		_camera_panel_mount.floating_changed.connect(_on_camera_floating_changed)
		# Read-only startup apply: remember the preference, never spawn windows
		# at launch (the floating preference applies on the next open).
		_panel_restore["camera"] = _resource_library.load_panel_state("camera")
	if _camera_toggle_button != null and not _camera_toggle_button.toggled.is_connected(_on_camera_toggle_toggled):
		_camera_toggle_button.icon = EditorIconLibrary.resolve(&"camera")
		_camera_toggle_button.toggled.connect(_on_camera_toggle_toggled)
	refresh_camera_state()


func wire_environment() -> void:
	if _environment_popup != null:
		_environment_popup.visible = false
		_environment_popup.apply_anchor(400.0)
		_environment_popup.bind_close(_environment_popup_close)
		_environment_popup.bind_detach(_environment_popup_detach)
		if not _environment_popup.close_requested.is_connected(_on_environment_popup_close_pressed):
			_environment_popup.close_requested.connect(_on_environment_popup_close_pressed)
		if not _environment_popup.detach_requested.is_connected(detach_environment_panel):
			_environment_popup.detach_requested.connect(detach_environment_panel)
	if _environment_panel_mount == null and _environment_popup_content != null:
		_environment_panel_mount = DetachablePanelMount.new(&"environment", "Environment", Vector2i(424, 480))
		_environment_panel_mount.setup(_environment_popup_content, _shell, _save_environment_panel_state)
		_environment_panel_mount.floating_changed.connect(_on_environment_floating_changed)
		_panel_restore["environment"] = _resource_library.load_panel_state("environment")
	if _environment_toggle_button != null and not _environment_toggle_button.toggled.is_connected(_on_environment_toggle_toggled):
		_environment_toggle_button.icon = EditorIconLibrary.resolve(&"environment")
		_environment_toggle_button.toggled.connect(_on_environment_toggle_toggled)
	refresh_environment_state()


func wire_settings() -> void:
	if _settings_popup != null:
		_settings_popup.visible = false
		_settings_popup.apply_anchor(420.0)
		_settings_popup.bind_close(_settings_popup_close)
		if not _settings_popup.close_requested.is_connected(_on_settings_popup_close_pressed):
			_settings_popup.close_requested.connect(_on_settings_popup_close_pressed)
	if _settings_toggle_button != null and not _settings_toggle_button.toggled.is_connected(_on_settings_toggle_toggled):
		_settings_toggle_button.icon = EditorIconLibrary.resolve(&"settings")
		_settings_toggle_button.toggled.connect(_on_settings_toggle_toggled)


# Quit-while-floating remembers the preference + rect for the next session.
func save_floating_states() -> void:
	if _camera_panel_mount != null:
		_camera_panel_mount.save_now()
	if _environment_panel_mount != null:
		_environment_panel_mount.save_now()


# The Escape router: closes the topmost visible popover. The native
# confirm/export dialogs own their own Escape handling (they emit canceled and
# hide), so only the corner popovers route here. Returns true when consumed.
func handle_escape() -> bool:
	if _camera_popup != null and _camera_popup.visible:
		set_camera_visible(false)
		return true
	if _environment_popup != null and _environment_popup.visible:
		set_environment_visible(false)
		return true
	if _settings_popup != null and _settings_popup.visible:
		set_settings_visible(false)
		return true
	return false


# --- Detachable panels (B6) ---

func camera_panel_mount() -> DetachablePanelMount:
	return _camera_panel_mount


func environment_panel_mount() -> DetachablePanelMount:
	return _environment_panel_mount


# The env document counts as open whether docked OR floating - the popover
# hides while the content floats, but its document is still on screen.
func environment_open() -> bool:
	return (_environment_popup != null and _environment_popup.visible) \
			or (_environment_panel_mount != null and _environment_panel_mount.is_floating())


func _save_camera_panel_state(docked: bool, rect: Rect2i) -> void:
	_panel_restore["camera"] = {"docked": docked, "has_rect": true, "rect": rect}
	_resource_library.save_panel_state("camera", docked, rect)


func _save_environment_panel_state(docked: bool, rect: Rect2i) -> void:
	_panel_restore["environment"] = {"docked": docked, "has_rect": true, "rect": rect}
	_resource_library.save_panel_state("environment", docked, rect)


func panel_restore_for(panel_id: String) -> Dictionary:
	return _panel_restore.get(panel_id, {"docked": true, "has_rect": false, "rect": Rect2i()})


# The window opens where it was last seen (clamped to a visible screen at
# apply time); first detach derives a rect from where the popover sits.
func _panel_detach_rect(panel_id: String, popover: PopoverPanel) -> Rect2i:
	var restore := panel_restore_for(panel_id)
	if bool(restore.get("has_rect", false)):
		return restore.get("rect", Rect2i()) as Rect2i
	return DetachablePanelMount.screen_rect_for(popover)


func detach_camera_panel() -> void:
	if _camera_panel_mount == null or _camera_panel_mount.is_floating():
		return
	# Content must exist before it floats (the popover may never have opened).
	_ensure_camera_content()
	_camera_panel_mount.detach(_panel_detach_rect("camera", _camera_popup))
	if _camera_popup != null:
		_camera_popup.close()
	refresh_camera_state()


func detach_environment_panel() -> void:
	if _environment_panel_mount == null or _environment_panel_mount.is_floating():
		return
	ensure_environment_content()
	_environment_panel_mount.detach(_panel_detach_rect("environment", _environment_popup))
	if _environment_popup != null:
		_environment_popup.close()
	refresh_environment_state()


func _on_camera_floating_changed(floating: bool) -> void:
	if _camera_toggle_button != null:
		_camera_toggle_button.set_pressed_no_signal(floating)


func _on_environment_floating_changed(floating: bool) -> void:
	if _environment_toggle_button != null:
		_environment_toggle_button.set_pressed_no_signal(floating)


# --- Camera popover -----------------------------------------------------------

func _on_camera_toggle_toggled(pressed: bool) -> void:
	set_camera_visible(pressed)


func _on_camera_popup_close_pressed() -> void:
	set_camera_visible(false)


func set_camera_visible(active: bool) -> void:
	if _camera_popup == null:
		return
	# A floating panel is not a popover: the toggle raises its window, and the
	# siblings' mutual-exclusion calls (active=false) must leave it alone.
	if _camera_panel_mount != null and _camera_panel_mount.is_floating():
		if active:
			_ensure_camera_content()
			_camera_panel_mount.focus_window()
		if _camera_toggle_button != null:
			_camera_toggle_button.set_pressed_no_signal(true)
		return
	if active and _get_editor_camera.call() == null:
		active = false
	# The remembered floating preference applies on open, never at launch.
	if active and _camera_panel_mount != null \
			and not bool(panel_restore_for("camera").get("docked", true)):
		detach_camera_panel()
		return
	if active:
		set_environment_visible(false)
		set_settings_visible(false)
	_camera_popup.visible = active
	if _camera_toggle_button != null:
		_camera_toggle_button.set_pressed_no_signal(active)
	if active:
		_ensure_camera_content()
	refresh_camera_state()


func _ensure_camera_content() -> void:
	if _camera_settings_mount == null:
		return
	if _camera_settings_panel == null:
		_camera_settings_panel = CameraSettingsPanelScene.instantiate() as Control
		_camera_settings_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_camera_settings_mount.add_child(_camera_settings_panel)
	sync_camera_editor()


# The camera panel's editor API is the shell itself (get_editor_camera etc.).
func sync_camera_editor() -> void:
	if _camera_settings_panel != null and _camera_settings_panel.has_method("set_editor"):
		_camera_settings_panel.set_editor(_shell)


func refresh_camera_state() -> void:
	var has_camera: bool = _get_editor_camera.call() != null
	var floating := _camera_panel_mount != null and _camera_panel_mount.is_floating()
	# A floating camera panel whose camera disappeared re-docks (mirrors the
	# docked popover's force-close below). persist=false: a transient
	# camera-null (editor rebind) must not overwrite the user's floating
	# preference - the next open with a camera floats again.
	if floating and not has_camera:
		_camera_panel_mount.redock(false)
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
		_ensure_camera_content()
		if _camera_settings_panel != null and _camera_settings_panel.has_method("sync_from_editor_state"):
			_camera_settings_panel.sync_from_editor_state()


# --- Environment popover -------------------------------------------------------

func _on_environment_toggle_toggled(pressed: bool) -> void:
	set_environment_visible(pressed)


func _on_environment_popup_close_pressed() -> void:
	set_environment_visible(false)


func set_environment_visible(active: bool) -> void:
	if _environment_popup == null:
		return
	# A floating panel is not a popover: the toggle raises its window, and the
	# siblings' mutual-exclusion calls (active=false) must leave it alone.
	if _environment_panel_mount != null and _environment_panel_mount.is_floating():
		if active:
			ensure_environment_content()
			_environment_panel_mount.focus_window()
		if _environment_toggle_button != null:
			_environment_toggle_button.set_pressed_no_signal(true)
		return
	# The remembered floating preference applies on open, never at launch.
	if active and _environment_panel_mount != null \
			and not bool(panel_restore_for("environment").get("docked", true)):
		detach_environment_panel()
		return
	if active:
		set_camera_visible(false)
		set_settings_visible(false)
	_environment_popup.visible = active
	if _environment_toggle_button != null:
		_environment_toggle_button.set_pressed_no_signal(active)
	if active:
		ensure_environment_content()
	refresh_environment_state()


func reset_environment_content() -> void:
	_environment_action_bar.rebuild(null)
	if _environment_actions_mount != null:
		for child in _environment_actions_mount.get_children():
			_environment_actions_mount.remove_child(child)
			child.free()
	if _environment_inspector_mount != null:
		for child in _environment_inspector_mount.get_children():
			_environment_inspector_mount.remove_child(child)
			child.free()


func ensure_environment_content() -> void:
	var popup_workspace := _popup_workspace.call() as EditorWorkspace
	if popup_workspace == null:
		return
	if _environment_actions_mount != null and _environment_action_bar.buttons().is_empty():
		_environment_action_bar.rebuild(popup_workspace)
	if _environment_inspector_mount != null and _environment_inspector_mount.get_child_count() == 0:
		popup_workspace.build_inspector(_environment_inspector_mount)


func refresh_environment_state() -> void:
	var popup_workspace := _popup_workspace.call() as EditorWorkspace
	var title := popup_workspace.get_project_title() if popup_workspace != null else "Environment"
	if _environment_popup_title != null:
		_environment_popup_title.text = title
	if _environment_panel_mount != null and _environment_panel_mount.is_floating():
		# The dirty "*" reaches the floating window through its OS title.
		_environment_panel_mount.set_window_title("Environment — %s" % title)
	_environment_action_bar.refresh_state(popup_workspace)


# --- Settings popover (visibility only; content = ShellSettingsPanel) ----------

func _on_settings_toggle_toggled(pressed: bool) -> void:
	set_settings_visible(pressed)


func _on_settings_popup_close_pressed() -> void:
	set_settings_visible(false)


func set_settings_visible(active: bool) -> void:
	if _settings_popup == null:
		return
	if active:
		set_camera_visible(false)
		set_environment_visible(false)
	_sync_settings_content.call()
	_settings_popup.visible = active
	if _settings_toggle_button != null:
		_settings_toggle_button.set_pressed_no_signal(active)
