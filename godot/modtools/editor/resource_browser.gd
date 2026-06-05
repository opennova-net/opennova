class_name EditorResourceBrowser
extends RefCounted

## The OpenNova Editor's in-editor resource picker: an indexed browser over the
## configured resource directory (EditorResourceLibrary). Extracted from
## editor_workstation.gd (B5-3b).
##
## The dialog is built as a child of the host shell so the shell's existing
## find_child("ResourceBrowserDialog") lookups (and the named sub-nodes) keep
## resolving. The capabilities the browser can't own itself are injected as
## Callables in setup() so it never reaches into the shell's internals:
##   - open_file_dialog(title, filters, on_pick, current_dir): legacy fallback
##     for workspaces that do not declare a resource kind
##   - open_settings(): raise the settings popup
##   - current_resource_path(kind) -> String: the active workspace's open file
##   - scan_root(): index the configured root (status/popup-sync stay on shell)

var _host: Control
var _library: EditorResourceLibrary
var _open_file_dialog: Callable
var _open_settings: Callable
var _current_resource_path: Callable
var _scan_root: Callable

var _dialog: ConfirmationDialog
var _directory_label: Label
var _settings_button: Button
var _search: LineEdit
var _hint: Label
var _list: ItemList
var _open_button: Button
var _kind: String = ""
var _title: String = ""
var _filters: PackedStringArray = PackedStringArray()
var _current_dir: String = ""
var _entries: Array = []
var _visible_entries: Array = []
var _open_action: Callable = Callable()


func setup(host: Control, library: EditorResourceLibrary, open_file_dialog: Callable, open_settings: Callable, current_resource_path: Callable, scan_root: Callable) -> void:
	_host = host
	_library = library
	_open_file_dialog = open_file_dialog
	_open_settings = open_settings
	_current_resource_path = current_resource_path
	_scan_root = scan_root


func open(workspace: EditorWorkspace, on_pick: Callable) -> void:
	if workspace == null:
		return
	var kind := String(workspace.get_open_resource_kind()).strip_edges()
	if kind.is_empty():
		_open_file_dialog.call(
			workspace.get_open_dialog_title(),
			workspace.get_open_dialog_filters(),
			on_pick,
			workspace.get_open_dialog_dir()
		)
		return
	_ensure_dialog()
	_kind = kind
	_title = workspace.get_open_dialog_title()
	_filters = workspace.get_open_dialog_filters()
	_current_dir = workspace.get_open_dialog_dir()
	_open_action = on_pick
	_search.text = ""
	_dialog.title = _title
	_refresh_entries()
	_refresh()
	_dialog.popup_centered(Vector2i(760, 520))


func _ensure_dialog() -> void:
	if _dialog != null and is_instance_valid(_dialog):
		return
	_dialog = ConfirmationDialog.new()
	_dialog.name = "ResourceBrowserDialog"
	_dialog.min_size = Vector2i(760, 520)
	_dialog.exclusive = true
	# Use the native themed window frame (border + title bar come from the editor
	# theme's Window/AcceptDialog styles). The embedded window is given the shell's
	# theme explicitly so it resolves the dark styling even though it is a separate
	# Window, replacing the old borderless + hand-coded-stylebox workaround.
	if _host != null and _host.theme != null:
		_dialog.theme = _host.theme
	_host.add_child(_dialog)

	# The dialog panel owns the body inset, so the content VBox attaches directly.
	var box := VBoxContainer.new()
	box.name = "ResourceBrowserBox"
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_theme_constant_override("separation", 10)
	_dialog.add_child(box)

	var header := HBoxContainer.new()
	header.name = "ResourceBrowserHeader"
	header.add_theme_constant_override("separation", 8)
	box.add_child(header)

	_directory_label = Label.new()
	_directory_label.name = "ResourceBrowserDirectoryLabel"
	_directory_label.theme_type_variation = &"Muted"
	_directory_label.clip_text = true
	_directory_label.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	header.add_child(_directory_label)

	_settings_button = Button.new()
	_settings_button.name = "ResourceBrowserSettingsButton"
	_settings_button.text = "Settings"
	_settings_button.focus_mode = Control.FOCUS_NONE
	_settings_button.pressed.connect(_on_settings_pressed)
	header.add_child(_settings_button)

	_search = LineEdit.new()
	_search.name = "ResourceBrowserSearch"
	_search.placeholder_text = "Search resources"
	_search.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_search.text_changed.connect(func(_text: String) -> void:
		_refresh()
	)
	box.add_child(_search)

	_hint = Label.new()
	_hint.name = "ResourceBrowserHint"
	_hint.theme_type_variation = &"Muted"
	_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_hint)

	_list = ItemList.new()
	_list.name = "ResourceBrowserList"
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_list.item_selected.connect(_on_item_selected)
	_list.item_activated.connect(_on_item_activated)
	box.add_child(_list)

	_dialog.confirmed.connect(_on_confirmed)
	_open_button = _dialog.get_ok_button()
	_open_button.text = "Open"
	_dialog.get_cancel_button().text = "Cancel"


func _refresh_entries() -> void:
	_entries = []
	var index := _library.get_index()
	if _library.get_root_dir().is_empty():
		index.clear()
		return
	if index.get_root_dir().is_empty():
		_scan_root.call()
	if not index.get_root_dir().is_empty():
		_entries = index.get_resource_files(_kind)


func _refresh() -> void:
	if _list == null:
		return
	_list.clear()
	_visible_entries = []
	var search := _search.text.strip_edges().to_lower() if _search != null else ""
	for entry_value in _entries:
		var entry := entry_value as Dictionary
		var display_name := String(entry.get("display_name", ""))
		var relative_path := String(entry.get("relative_path", ""))
		var haystack := ("%s %s" % [display_name, relative_path]).to_lower()
		if not search.is_empty() and not haystack.contains(search):
			continue
		_visible_entries.append(entry)
		var text := "%s  %s" % [display_name, relative_path]
		var current_path := String(_current_resource_path.call(_kind))
		if not current_path.is_empty() and _same_filesystem_path(String(entry.get("path", "")), current_path):
			text += "  (open)"
		var index := _list.add_item(text)
		_list.set_item_metadata(index, entry)

	var root := _library.get_root_dir()
	var has_root := not root.strip_edges().is_empty()
	var has_entries := not _entries.is_empty()
	var has_visible := not _visible_entries.is_empty()
	if _directory_label != null:
		_directory_label.text = root if has_root else "No resource directory selected"
	if _hint != null:
		_hint.visible = not has_visible
		if not has_root:
			_hint.text = "No resource directory selected."
		elif not has_entries:
			_hint.text = "No %s resources found in %s." % [_kind_label(), root]
		else:
			_hint.text = "No matching resources."
	if _settings_button != null:
		_settings_button.visible = not has_root or not has_entries
	if _open_button != null:
		_open_button.disabled = true


func _kind_label() -> String:
	match _kind:
		"terrain":
			return "terrain"
		"environment":
			return "environment"
		"mission":
			return "mission"
		"strings":
			return "strings"
		"object", "object_project", "object_model", "object_scene":
			return "object"
		_:
			return "resource"


func _on_item_selected(_index: int) -> void:
	if _open_button != null:
		_open_button.disabled = false


func _on_item_activated(index: int) -> void:
	_list.select(index)
	_open_selected_entry()


func _on_confirmed() -> void:
	_open_selected_entry()


func _open_selected_entry() -> void:
	if _list == null:
		return
	var selected := _list.get_selected_items()
	if selected.size() == 0:
		return
	var entry := _list.get_item_metadata(selected[0]) as Dictionary
	var path := String(entry.get("path", ""))
	if path.is_empty():
		path = String(entry.get("logical_name", ""))
	if path.is_empty():
		return
	if _open_action.is_valid():
		_open_action.call(path)
	_dialog.hide()


func _on_settings_pressed() -> void:
	if _dialog != null:
		_dialog.hide()
	_open_settings.call()


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
