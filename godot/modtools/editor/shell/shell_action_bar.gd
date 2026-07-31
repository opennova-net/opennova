class_name ShellActionBar
extends RefCounted

## One document-action rail (New/Open/Save/Save As/Export) over a mount
## container, driven by the workspace's document-action capability hooks.
## Horizontal mounts (the top bar) fold the secondary actions into a "More"
## menu; vertical mounts (the environment popup) list every action inline.
## The shell owns one instance per mount and routes presses back through the
## Callable it injected.

## Stable ids for the document-action cluster. PopupMenu item ids reuse them,
## so the More menu routes through the same pressed handler as the buttons.
enum Action { NEW, OPEN, SAVE, SAVE_AS, EXPORT }

var _mount: BoxContainer
var _on_pressed: Callable
# func() -> bool: any workspace busy (disables the whole rail).
var _busy: Callable
# func() -> EditorWorkspace: what the overflow menu re-gates against on open.
var _active_workspace: Callable
# func() -> Theme: the shell theme for the native popup window (a popup is a
# separate Window and does not inherit the in-tree theme).
var _popup_theme: Callable
var _name_prefix := ""
var _min_height := 34.0
var _buttons: Dictionary = {}
var _overflow_button: MenuButton


func setup(
	mount: BoxContainer,
	on_pressed: Callable,
	busy: Callable,
	active_workspace: Callable,
	popup_theme: Callable,
	name_prefix := "",
	min_height := 34.0
) -> void:
	_mount = mount
	_on_pressed = on_pressed
	_busy = busy
	_active_workspace = active_workspace
	_popup_theme = popup_theme
	_name_prefix = name_prefix
	_min_height = min_height


## The live action-id -> Button map (tests assert over it).
func buttons() -> Dictionary:
	return _buttons


static func action_defs_for(workspace: EditorWorkspace) -> Array:
	# "overflow" marks the secondary actions the horizontal top bar folds into
	# the More menu; vertical mounts (environment popup) ignore it.
	var action_defs := [
		{"id": Action.NEW, "visible": workspace.has_new_action(), "label": workspace.get_new_action_label(), "overflow": false},
		{"id": Action.OPEN, "visible": workspace.has_open_action(), "label": workspace.get_open_action_label(), "overflow": false},
		{"id": Action.SAVE, "visible": workspace.has_save_action(), "label": workspace.get_save_action_label(), "overflow": false},
		{"id": Action.SAVE_AS, "visible": workspace.has_save_as_action(), "label": workspace.get_save_as_action_label(), "overflow": true},
		{"id": Action.EXPORT, "visible": workspace.has_export_action(), "label": workspace.get_export_action_label(), "overflow": true},
	]
	return action_defs


static func button_name_for(action_id: int) -> String:
	match action_id:
		Action.NEW:
			return "NewActionButton"
		Action.OPEN:
			return "OpenActionButton"
		Action.SAVE:
			return "SaveActionButton"
		Action.SAVE_AS:
			return "SaveAsActionButton"
		Action.EXPORT:
			return "ExportActionButton"
		_:
			return "WorkspaceActionButton"


static func icon_id_for(action_id: int) -> StringName:
	match action_id:
		Action.NEW:
			return &"action_new"
		Action.OPEN:
			return &"action_open"
		Action.SAVE:
			return &"action_save"
		Action.SAVE_AS:
			return &"action_save_as"
		Action.EXPORT:
			return &"action_export"
		_:
			return &""


## True when the workspace can run the action right now (busy always gates).
static func action_enabled(workspace: EditorWorkspace, action_id: int, busy: bool) -> bool:
	if busy or workspace == null:
		return false
	match action_id:
		Action.NEW:
			return workspace.can_new()
		Action.OPEN:
			return workspace.can_open()
		Action.SAVE:
			return workspace.can_save()
		Action.SAVE_AS:
			return workspace.can_save_as()
		Action.EXPORT:
			return workspace.can_export()
	return false


func rebuild(workspace: EditorWorkspace) -> void:
	for child in _mount.get_children():
		_mount.remove_child(child)
		child.free()
	_buttons.clear()
	# The freed children included the previous More menu (top-bar mount only).
	if _mount is HBoxContainer:
		_overflow_button = null

	if workspace == null:
		_mount.visible = false
		return

	var action_defs := action_defs_for(workspace)
	var overflow_defs: Array = []
	for action_def in action_defs:
		if not bool(action_def["visible"]):
			continue
		if _mount is HBoxContainer and bool(action_def.get("overflow", false)):
			overflow_defs.append(action_def)
			continue
		var btn := Button.new()
		var action_id := int(action_def["id"])
		btn.name = _name_prefix + button_name_for(action_id)
		btn.text = String(action_def["label"])
		btn.icon = EditorIconLibrary.resolve(icon_id_for(action_id))
		btn.focus_mode = Control.FOCUS_NONE
		if _mount is HBoxContainer:
			btn.custom_minimum_size = Vector2(112, _min_height)
			btn.size_flags_horizontal = Control.SIZE_SHRINK_CENTER
		else:
			btn.custom_minimum_size = Vector2(0, _min_height)
			btn.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		btn.pressed.connect(_on_pressed.bind(action_id))
		_mount.add_child(btn)
		_buttons[action_id] = btn

	if not overflow_defs.is_empty():
		_overflow_button = _make_overflow_button(overflow_defs)
		_mount.add_child(_overflow_button)

	_mount.visible = not _buttons.is_empty() or (_mount is HBoxContainer and _overflow_button != null)
	refresh_state(workspace)


func refresh_state(workspace: EditorWorkspace) -> void:
	var busy := bool(_busy.call())
	for action_id in _buttons:
		var btn := _buttons[action_id] as Button
		if btn == null:
			continue
		btn.disabled = not action_enabled(workspace, int(action_id), busy)


func _make_overflow_button(overflow_defs: Array) -> MenuButton:
	var more := MenuButton.new()
	more.name = _name_prefix + "MoreActionsButton"
	more.text = "More"
	more.tooltip_text = "More document actions"
	more.icon = EditorIconLibrary.resolve(&"action_more")
	more.focus_mode = Control.FOCUS_NONE
	more.flat = false
	more.custom_minimum_size = Vector2(0, _min_height)
	more.size_flags_horizontal = Control.SIZE_SHRINK_CENTER
	var popup := more.get_popup()
	# The popup is a native Window; it does not inherit the shell theme.
	var theme: Theme = _popup_theme.call()
	if theme != null:
		popup.theme = theme
	for action_def in overflow_defs:
		var action_id := int(action_def["id"])
		popup.add_icon_item(EditorIconLibrary.resolve(icon_id_for(action_id)), String(action_def["label"]), action_id)
	# PopupMenu.id_pressed hands over the item id, which is the Action value,
	# so the menu routes through the same handler as the buttons.
	popup.id_pressed.connect(_on_pressed)
	# Items are only actionable while the popup is open; refreshing their
	# disabled state on about_to_popup keeps gating out of the per-frame poll.
	popup.about_to_popup.connect(_refresh_overflow_state)
	return more


func _refresh_overflow_state() -> void:
	if _overflow_button == null or not is_instance_valid(_overflow_button):
		return
	var workspace: EditorWorkspace = _active_workspace.call()
	var busy := bool(_busy.call())
	var popup := _overflow_button.get_popup()
	for i in popup.item_count:
		popup.set_item_disabled(i, not action_enabled(workspace, popup.get_item_id(i), busy))
