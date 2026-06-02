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
##
## Results render in a sortable, multi-column Tree (Name / Type / Size /
## Modified). Size and modified-time are read off the resource-index entry,
## which the native scan fills from the filesystem (opennova::ResourceFileEntry);
## the browser never stats files itself. Click a column header to sort. The
## search box takes focus on open and Enter opens the selected row, so the whole
## flow is keyboard-driven.

# Tint applied to the row whose resource is the workspace's currently-open file,
# in place of the old "(open)" text suffix.
const OPEN_ROW_COLOR := Color(0.49, 0.73, 1.0)
const COLUMN_TITLES: PackedStringArray = ["Name", "Type", "Size", "Modified"]
const COLUMN_NAME := 0
const COLUMN_TYPE := 1
const COLUMN_SIZE := 2
const COLUMN_MODIFIED := 3

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
var _tree: Tree
var _open_button: Button
var _kind: String = ""
var _title: String = ""
var _filters: PackedStringArray = PackedStringArray()
var _current_dir: String = ""
var _entries: Array = []
var _visible_entries: Array = []
var _open_action: Callable = Callable()
var _sort_column: int = COLUMN_NAME
var _sort_ascending: bool = true
# One-shot: set true when a resource is opened, reset each time the dialog opens.
# Stops the Enter key (which emits text_submitted AND the dialog's confirm) from
# opening twice, without gating on dialog visibility (the OK button fires
# confirmed only after AcceptDialog has already hidden itself).
var _picked: bool = false


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
	_picked = false
	_search.text = ""
	_dialog.title = _title
	_refresh_entries()
	_refresh()
	_dialog.popup_centered(Vector2i(860, 580))
	# Focus the search box so the user can type immediately. Deferred: the field
	# must be on-screen (post-popup) before it can take focus.
	_search.call_deferred("grab_focus")


func _ensure_dialog() -> void:
	if _dialog != null and is_instance_valid(_dialog):
		return
	_dialog = ConfirmationDialog.new()
	_dialog.name = "ResourceBrowserDialog"
	# Resizable so long resource names/paths are not clipped; min_size keeps it
	# usable when shrunk.
	_dialog.min_size = Vector2i(560, 380)
	_dialog.unresizable = false
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
	_search.clear_button_enabled = true
	_search.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_search.text_changed.connect(func(_text: String) -> void:
		_refresh()
	)
	# Enter opens the selected row; Down arrow drops focus into the list.
	_search.text_submitted.connect(_on_search_submitted)
	_search.gui_input.connect(_on_search_gui_input)
	box.add_child(_search)

	_hint = Label.new()
	_hint.name = "ResourceBrowserHint"
	_hint.theme_type_variation = &"Muted"
	_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_hint)

	_tree = Tree.new()
	_tree.name = "ResourceBrowserList"
	_tree.columns = COLUMN_TITLES.size()
	_tree.column_titles_visible = true
	_tree.hide_root = true
	_tree.select_mode = Tree.SELECT_ROW
	# Our LineEdit owns search; the Tree's own incremental type-search would only
	# fight it for keystrokes once the list has focus.
	_tree.allow_search = false
	_tree.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	# Name takes the slack; the metadata columns size to their content.
	_tree.set_column_expand(COLUMN_NAME, true)
	_tree.set_column_clip_content(COLUMN_NAME, true)
	_tree.set_column_expand(COLUMN_TYPE, false)
	_tree.set_column_expand(COLUMN_SIZE, false)
	_tree.set_column_expand(COLUMN_MODIFIED, false)
	_tree.set_column_custom_minimum_width(COLUMN_TYPE, 64)
	_tree.set_column_custom_minimum_width(COLUMN_SIZE, 96)
	_tree.set_column_custom_minimum_width(COLUMN_MODIFIED, 156)
	_tree.set_column_title_alignment(COLUMN_SIZE, HORIZONTAL_ALIGNMENT_RIGHT)
	_tree.item_selected.connect(_on_item_selected)
	_tree.item_activated.connect(_on_item_activated)
	_tree.column_title_clicked.connect(_on_column_title_clicked)
	box.add_child(_tree)

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
	if _tree == null:
		return
	_tree.clear()
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
	_sort_visible_entries()
	_update_column_titles()

	# The currently-open file is resolved once per refresh (it cannot change mid
	# search), not once per row as the old ItemList build did.
	var current_path := String(_current_resource_path.call(_kind))
	var root_item := _tree.create_item()
	var first_item: TreeItem = null
	var open_item: TreeItem = null
	for entry_value in _visible_entries:
		var entry := entry_value as Dictionary
		var item := _tree.create_item(root_item)
		_populate_row(item, entry)
		item.set_metadata(COLUMN_NAME, entry)
		if first_item == null:
			first_item = item
		if open_item == null and not current_path.is_empty() and _same_filesystem_path(String(entry.get("path", "")), current_path):
			_mark_open_row(item)
			open_item = item

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

	# Default selection so the keyboard flow works without a click: prefer the
	# currently-open file, else the first row. Selecting emits item_selected,
	# which enables Open.
	var to_select := open_item if open_item != null else first_item
	if to_select != null:
		to_select.select(COLUMN_NAME)
		_tree.scroll_to_item(to_select)
		if _open_button != null:
			_open_button.disabled = false
	elif _open_button != null:
		_open_button.disabled = true


func _populate_row(item: TreeItem, entry: Dictionary) -> void:
	var display_name := String(entry.get("display_name", ""))
	var relative_path := String(entry.get("relative_path", ""))
	item.set_text(COLUMN_NAME, display_name)
	item.set_tooltip_text(COLUMN_NAME, relative_path)
	item.set_text(COLUMN_TYPE, relative_path.get_extension().to_upper())
	item.set_text(COLUMN_SIZE, _format_size(int(entry.get("size_bytes", 0))))
	item.set_text_alignment(COLUMN_SIZE, HORIZONTAL_ALIGNMENT_RIGHT)
	item.set_text(COLUMN_MODIFIED, _format_modified(int(entry.get("modified_time", 0))))


# The open file's row is tinted and tooltipped rather than carrying a text
# suffix, so the name column reads cleanly.
func _mark_open_row(item: TreeItem) -> void:
	for column in _tree.columns:
		item.set_custom_color(column, OPEN_ROW_COLOR)
	item.set_tooltip_text(COLUMN_NAME, "%s  (currently open)" % item.get_tooltip_text(COLUMN_NAME))


func _format_size(bytes: int) -> String:
	if bytes <= 0:
		return ""
	var unit := 1024.0
	if bytes < 1024:
		return "%d B" % bytes
	if bytes < 1024 * 1024:
		return "%0.1f KB" % (bytes / unit)
	if bytes < 1024 * 1024 * 1024:
		return "%0.1f MB" % (bytes / (unit * unit))
	return "%0.1f GB" % (bytes / (unit * unit * unit))


func _format_modified(unix_seconds: int) -> String:
	if unix_seconds <= 0:
		return ""
	var dt := Time.get_datetime_dict_from_unix_time(unix_seconds)
	return "%04d-%02d-%02d %02d:%02d" % [dt["year"], dt["month"], dt["day"], dt["hour"], dt["minute"]]


func _sort_visible_entries() -> void:
	var column := _sort_column
	var ascending := _sort_ascending
	_visible_entries.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		var a_key: Variant = _sort_key(a, column)
		var b_key: Variant = _sort_key(b, column)
		if a_key == b_key:
			# Stable tiebreak on name so equal sizes/types keep a predictable order.
			return String(a.get("display_name", "")).to_lower() < String(b.get("display_name", "")).to_lower()
		if ascending:
			return a_key < b_key
		return a_key > b_key
	)


func _sort_key(entry: Dictionary, column: int) -> Variant:
	match column:
		COLUMN_TYPE:
			return String(entry.get("relative_path", "")).get_extension().to_lower()
		COLUMN_SIZE:
			return int(entry.get("size_bytes", 0))
		COLUMN_MODIFIED:
			return int(entry.get("modified_time", 0))
		_:
			return String(entry.get("display_name", "")).to_lower()


func _update_column_titles() -> void:
	if _tree == null:
		return
	for i in COLUMN_TITLES.size():
		var title := COLUMN_TITLES[i]
		if i == _sort_column:
			title += "  ▲" if _sort_ascending else "  ▼"
		_tree.set_column_title(i, title)


func _on_column_title_clicked(column: int, mouse_button_index: int) -> void:
	if mouse_button_index != MOUSE_BUTTON_LEFT:
		return
	if _sort_column == column:
		_sort_ascending = not _sort_ascending
	else:
		_sort_column = column
		_sort_ascending = true
	_refresh()


func _on_search_submitted(_text: String) -> void:
	_open_selected_entry()


func _on_search_gui_input(event: InputEvent) -> void:
	if not (event is InputEventKey):
		return
	var key := event as InputEventKey
	if not key.pressed or key.is_echo() or key.keycode != KEY_DOWN:
		return
	if _tree == null or _visible_entries.is_empty():
		return
	# Down arrow drops focus from the search box into the result list.
	_tree.grab_focus()
	if _tree.get_selected() == null:
		var root_item := _tree.get_root()
		var first := root_item.get_first_child() if root_item != null else null
		if first != null:
			first.select(COLUMN_NAME)
	if _search != null:
		_search.accept_event()


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
		"font":
			return "font"
		"credits":
			return "credits"
		"menu":
			return "menu"
		"object", "object_project", "object_model", "object_scene":
			return "object"
		"music", "sbf", "music_script":
			return "music"
		_:
			return "resource"


func _on_item_selected() -> void:
	if _open_button != null:
		_open_button.disabled = _tree == null or _tree.get_selected() == null


func _on_item_activated() -> void:
	_open_selected_entry()


func _on_confirmed() -> void:
	_open_selected_entry()


func _open_selected_entry() -> void:
	if _picked or _tree == null:
		return
	var item := _tree.get_selected()
	if item == null:
		return
	var entry := item.get_metadata(COLUMN_NAME) as Dictionary
	if entry == null:
		return
	var path := String(entry.get("path", ""))
	if path.is_empty():
		path = String(entry.get("logical_name", ""))
	if path.is_empty():
		return
	_picked = true
	if _open_action.is_valid():
		_open_action.call(path)
	if _dialog != null:
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
