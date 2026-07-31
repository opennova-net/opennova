class_name EditorResourceBrowser
extends RefCounted

## The OpenNova Editor's in-editor resource picker: an indexed browser over the
## configured resource directory (EditorResourceLibrary). Extracted from
## editor_workstation.gd (B5-3b); the list itself is the shared ResourceTable
## (A10), which the persistent browser pane reuses.
##
## The dialog is built as a child of the mount shell so the shell's existing
## find_child("ResourceBrowserDialog") lookups (and the named sub-nodes) keep
## resolving. The capabilities the browser can't own itself are injected as
## Callables in setup() so it never reaches into the shell's internals:
##   - open_file_dialog(title, filters, on_pick, current_dir): legacy fallback
##     for workspaces that do not declare a resource kind
##   - open_settings(): raise the settings popup
##   - current_resource_path(kind) -> String: the active workspace's open file
##   - scan_root(): index the configured root (status/popup-sync stay on shell)
##
## Results render in the table's sortable Tree (Name / Type / Size / Modified);
## the search box takes focus on open and Enter opens the selected row, so the
## whole flow is keyboard-driven.

const ResourceTableScript := preload("res://modtools/framework/resource_table.gd")

var _mount: Control
var _library: EditorResourceLibrary
var _open_file_dialog: Callable
var _open_settings: Callable
var _current_resource_path: Callable
var _scan_root: Callable

var _dialog: ConfirmationDialog
var _directory_label: Label
var _settings_button: Button
var _hint: Label
var _table: ResourceTable
var _open_button: Button
var _kind: String = ""
var _title: String = ""
var _filters: PackedStringArray = PackedStringArray()
var _current_dir: String = ""
var _entries: Array = []
var _open_action: Callable = Callable()
# One-shot: set true when a resource is opened, reset each time the dialog opens.
# Stops the Enter key (which emits text_submitted AND the dialog's confirm) from
# opening twice, without gating on dialog visibility (the OK button fires
# confirmed only after AcceptDialog has already hidden itself).
var _picked: bool = false


func setup(mount: Control, library: EditorResourceLibrary, open_file_dialog: Callable, open_settings: Callable, current_resource_path: Callable, scan_root: Callable) -> void:
	_mount = mount
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
	# _filters/_current_dir only feed the no-kind file-dialog fallback above;
	# the indexed path ignores them, so workspace opens keep their behavior.
	_filters = workspace.get_open_dialog_filters()
	_current_dir = workspace.get_open_dialog_dir()
	open_kind(kind, workspace.get_open_dialog_title(), on_pick)


## Open the indexed picker over every resource of `kind`, independent of any
## workspace — the entry point for link widgets ("pick a terrain") and other
## callers that know the kind but have no open-dialog contract.
func open_kind(kind: String, title: String, on_pick: Callable) -> void:
	_ensure_dialog()
	_kind = kind
	_title = title
	_open_action = on_pick
	_picked = false
	_table.clear_search()
	_dialog.title = _title
	_refresh_entries()
	_push_entries()
	_dialog.popup_centered(Vector2i(860, 580))
	# Focus the search box so the user can type immediately. Deferred: the field
	# must be on-screen (post-popup) before it can take focus.
	_table.focus_search()


## Open the picker over an explicit, caller-supplied file list (kind-independent). Unlike open(),
## this does not consult the C++ resource index, so it serves resource types the index does not
## register (e.g. .adm) -- scoped to a single call site rather than registered globally. `files`
## are flat names/paths (display shows the basename); `on_pick` receives the chosen entry's name.
func open_files(title: String, files: PackedStringArray, on_pick: Callable) -> void:
	_ensure_dialog()
	_kind = ""
	_title = title
	_filters = PackedStringArray()
	_current_dir = ""
	_open_action = on_pick
	_picked = false
	_entries = []
	for f in files:
		var name := String(f)
		_entries.append({
			"display_name": name.get_file(),
			"relative_path": name,
			"path": name,
			"logical_name": name,
		})
	_table.clear_search()
	_dialog.title = title
	_push_entries()
	_dialog.popup_centered(Vector2i(760, 520))


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
	if _mount != null and _mount.theme != null:
		_dialog.theme = _mount.theme
	_mount.add_child(_dialog)

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

	_hint = Label.new()
	_hint.name = "ResourceBrowserHint"
	_hint.theme_type_variation = &"Muted"
	_hint.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_hint)

	_table = ResourceTableScript.new()
	_table.name = "ResourceBrowserTable"
	_table.entry_activated.connect(_on_entry_activated)
	_table.selection_changed.connect(func(has_selection: bool) -> void:
		if _open_button != null:
			_open_button.disabled = not has_selection)
	_table.list_changed.connect(func(_visible_count: int) -> void: _refresh_chrome())
	box.add_child(_table)

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


func _push_entries() -> void:
	# The currently-open file is resolved once per push (it cannot change mid
	# search); the table re-applies the tint across its own filter refreshes.
	_table.set_entries(_entries, String(_current_resource_path.call(_kind)))


# Directory label, empty-state hint, and the settings shortcut — the dialog
# chrome around the shared table.
func _refresh_chrome() -> void:
	var root := _library.get_root_dir()
	var has_root := not root.strip_edges().is_empty()
	var has_entries := not _entries.is_empty()
	var has_visible := _table != null and _table.get_visible_count() > 0
	if _directory_label != null:
		_directory_label.text = root if has_root else "No resource directory selected"
	if _hint != null:
		_hint.visible = not has_visible
		if not has_root:
			_hint.text = "No resource directory selected."
		elif not has_entries:
			_hint.text = "No %s resources found in %s." % [ResourceKinds.label(_kind), root]
		else:
			_hint.text = "No matching resources."
	if _settings_button != null:
		_settings_button.visible = not has_root or not has_entries


func _on_entry_activated(entry: Dictionary) -> void:
	_open_entry(entry)


func _on_confirmed() -> void:
	_open_entry(_table.get_selected_entry())


func _open_entry(entry: Dictionary) -> void:
	if _picked or entry.is_empty():
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
