class_name StringsEditorWorkspace
extends EditorWorkspace

## ONED workspace for editing RTXT localized string tables (strings/*.bin).
##
## A non-3D data editor. The center viewport mount holds one self-contained view
## (StringsEditorView = table + per-entry detail in an HSplit); the left inspector
## holds search / section management / validation / CSV. There is no right asset
## dock, which keeps the detail editor always visible and avoids the shell's
## split-persistence pitfalls. This adapter owns the StringsEditor document and
## fans its two change channels out: `structure_changed` -> rebuild, `edited` ->
## refresh the title only.

const StringsEditorScript = preload("res://modtools/strings/strings_editor.gd")
const StringsEditorViewScript = preload("res://modtools/strings/ui/strings_editor_view.gd")
const StringsInspectorScript = preload("res://modtools/strings/ui/strings_inspector.gd")

## Session state (open tabs + active), restored on activate like the music workspace.
const STATE_PATH := "user://strings_editor_state.cfg"

## The ACTIVE document. Multi-document state lives in _tabs; this alias always
## points at the active tab's document, so get_editor_document() (and every
## test/caller that reaches for `strings_editor`) keeps working untouched.
var strings_editor: StringsEditor

var _tabs := DocumentTabSet.new()
var _mount: ViewportMount
var _view: Control
var _inspector: Control
var _state_restored: bool = false

var _search: String = ""
var _section_filter: int = -1  # -1 = all sections


func _init() -> void:
	# A named method, not a lambda: a lambda touching a member signal captures
	# self strongly and would cycle workspace <-> tab set (both RefCounted).
	_tabs.changed.connect(_on_tabs_changed)


func _on_tabs_changed() -> void:
	documents_changed.emit()


func set_editor_shell(value: Node) -> void:
	super.set_editor_shell(value)
	_ensure_editor()


# --- Identity ---

func get_workspace_id() -> String:
	return "strings"


func get_workspace_label() -> String:
	return "Strings"


func get_workspace_tooltip() -> String:
	return "Edit localized game string tables."


func get_project_title() -> String:
	return strings_editor.get_project_title() if strings_editor else "Strings"


func get_status_tool() -> String:
	return "Strings"


func get_status_context() -> String:
	return strings_editor.get_status_context() if strings_editor else "No string table open."


# --- Lifecycle ---

func activate() -> void:
	_ensure_editor()
	_restore_state()


func deactivate() -> void:
	_save_state()


func _restore_state() -> void:
	# Reopen the last session's tabs once per session, and only while the
	# workspace is still pristine (one tab, no path, no edits) so it never
	# clobbers user work. Falls back to the pre-tabs `last_path` key.
	if _state_restored:
		return
	_state_restored = true
	if strings_editor == null or _tabs.count() != 1 \
			or strings_editor.is_dirty or not strings_editor.current_path.is_empty():
		return
	var cfg := ConfigFile.new()
	if cfg.load(STATE_PATH) != OK:
		return
	var paths: PackedStringArray = cfg.get_value("session", "open_paths", PackedStringArray())
	if paths.is_empty():
		var last: String = cfg.get_value("session", "last_path", "")
		if not last.is_empty():
			paths.append(last)
	for path in paths:
		if not path.is_empty() and FileAccess.file_exists(path):
			open_file(path)
	var active := int(cfg.get_value("session", "active_index", _tabs.count() - 1))
	if active >= 0 and active < _tabs.count():
		activate_document(active)


func _save_state() -> void:
	if strings_editor == null:
		return
	var cfg := ConfigFile.new()
	cfg.load(STATE_PATH)  # keep unrelated values if the file exists
	# last_path stays for back-compat with pre-tab session files.
	cfg.set_value("session", "last_path", strings_editor.current_path)
	var open := _tabs.open_paths()
	cfg.set_value("session", "open_paths", open)
	# active_index is stored in open_paths space: pathless (Untitled) tabs are
	# not persisted, so a full-list index would drift past them on restore.
	var active := -1
	if not strings_editor.current_path.is_empty():
		active = open.find(strings_editor.current_path)
	cfg.set_value("session", "active_index", active)
	cfg.save(STATE_PATH)


func _ensure_editor() -> void:
	if strings_editor != null:
		return
	var doc := _create_document()
	_tabs.add(doc)
	strings_editor = doc


func _create_document() -> StringsEditor:
	var doc: StringsEditor = StringsEditorScript.new()
	doc.name = "StringsEditor"
	_mount_under_shell(doc)
	doc.new_table(false)
	doc.structure_changed.connect(_on_doc_structure_changed.bind(doc))
	doc.edited.connect(_on_doc_edited.bind(doc))
	return doc


## Repoint the alias + the view/inspector at the active tab's document.
func _bind_active_document() -> void:
	var active := _tabs.get_active() as StringsEditor
	if active == null or active == strings_editor:
		strings_editor = active
		return
	strings_editor = active
	if _view != null:
		_view.set_document(strings_editor)
		_view.set_filter(_search, _section_filter)
	if _inspector != null:
		_inspector.refresh()
	_sync_shell()


# --- Document tabs (EditorWorkspace tier) ---

func supports_document_tabs() -> bool:
	return true


func get_document_tabs() -> Array[DocumentTabRow]:
	return _tabs.tabs()


func get_active_document_index() -> int:
	return _tabs.get_active_index()


func activate_document(index: int) -> Error:
	_ensure_editor()
	var err := _tabs.set_active(index)
	if err != OK:
		return err
	_bind_active_document()
	_save_state()
	return OK


func close_document(index: int) -> Error:
	_ensure_editor()
	var doc := _tabs.get_at(index)
	if doc == null:
		return ERR_INVALID_PARAMETER
	var was_active := index == _tabs.get_active_index()
	_tabs.remove_at(index)
	(doc as Node).queue_free()
	if _tabs.count() == 0:
		# The workspace never holds zero documents: seed a fresh pristine table.
		_tabs.add(_create_document())
		was_active = true
	if was_active:
		_bind_active_document()
	_save_state()
	return OK


# --- Center: self-contained table + detail view ---

func _ensure_mount() -> ViewportMount:
	if _mount == null:
		_mount = ViewportMount.new(&"StringsEditorView", _create_view)
	return _mount


func _create_view() -> Control:
	_view = StringsEditorViewScript.new()
	return _view


func mount_viewport(mount: Control) -> void:
	if mount == null:
		return
	_ensure_editor()
	_ensure_mount().mount(mount)
	if _view != null:
		_view.set_document(strings_editor)
		_view.set_filter(_search, _section_filter)
		_view.set_font_service(_preview_font_service())


func unmount_viewport(_released: Control) -> void:
	if _mount != null:
		_mount.unmount()


func release_viewport() -> void:
	if _mount != null:
		_mount.release()
	_view = null


# --- Left: inspector controls ---

func build_inspector(mount: Control) -> void:
	_ensure_editor()
	_inspector = StringsInspectorScript.new()
	# setup() precedes the tree entry: _ready builds shell-dependent sections
	# (the Used-by strip), so the workspace ref must already be there.
	_inspector.setup(self)
	mount.add_child(_inspector)
	_inspector.refresh()


# --- Game preview fonts (STR-1) ---
# The detail panel renders the selected entry through the engine draw with a
# chooseable font. These are the workspace-owned seams the picker consumes;
# the panel itself is the framework EngineTextPreview (F2).

## .fnt names available in the shell's mounted game folder, sorted; empty when
## headless or nothing is mounted (strings stays shell-only on purpose — see
## _resource_root_or_settings's do-not-widen note).
func get_preview_font_names() -> PackedStringArray:
	var root := _resource_root()
	if root == null:
		return PackedStringArray()
	var names := PackedStringArray()
	for path in root.list_files(".fnt"):
		var file := String(path).get_file()
		if names.has(file):
			continue
		# Sniff the header 4CC (single-sourced from libs/fnt) so a stray
		# non-font .fnt never reaches the parser — which error-logs — from
		# plain picker enumeration; the picker then eager-previews only fonts
		# that can actually load.
		var bytes := root.read_file(file)
		if bytes.size() < 4 or bytes.decode_u32(0) != NovaFntResource.MAGIC:
			continue
		names.append(file)
	names.sort()
	return names


## Load one of those fonts the way the runtime does (VFS read ->
## NovaFntResource -> FontFile). Null when unresolvable.
func load_preview_font(font_name: String) -> FontFile:
	return HudText.load_font(_resource_root(), font_name)


func _preview_font_service() -> Dictionary:
	return {
		"list": get_preview_font_names,
		"load": load_preview_font,
	}


# --- Coordinator state shared with the views ---

func get_document() -> StringsEditor:
	_ensure_editor()
	return strings_editor


func set_search(text: String) -> void:
	_search = text
	if _view != null:
		_view.set_filter(_search, _section_filter)


func set_section_filter(section_index: int) -> void:
	# Skip redundant re-filters: a structural rebuild already refreshes the table,
	# and the inspector re-applies the same value during its refresh.
	if section_index == _section_filter:
		return
	_section_filter = section_index
	if _view != null:
		_view.set_filter(_search, _section_filter)


func get_section_filter() -> int:
	return _section_filter


func add_entry_default() -> void:
	_ensure_editor()
	var section := _section_filter if _section_filter >= 0 else 0
	if strings_editor.string_table.get_section_count() == 0:
		section = strings_editor.add_section("default")
	strings_editor.add_entry("NEW_KEY", "", section, Vector2i())


func remove_selected_entry() -> void:
	_ensure_editor()
	if strings_editor.selected_index >= 0:
		strings_editor.remove_entry(strings_editor.selected_index)


func _on_doc_structure_changed(doc: StringsEditor) -> void:
	if doc == strings_editor:
		if _view != null:
			_view.rebuild()
		if _inspector != null:
			_inspector.refresh()
		_sync_shell()
	# Background tabs still refresh their strip row (label after save-as, dirty).
	_tabs.notify_changed()


func _on_doc_edited(doc: StringsEditor) -> void:
	if doc == strings_editor:
		_sync_shell()
	_tabs.notify_changed()


# --- Document actions ---

# The domain document the EditorWorkspace base derives undo/redo + dirty from.
func get_editor_document() -> Object:
	return strings_editor


func can_new() -> bool:
	return true


func get_new_action_label() -> String:
	return "New Strings"


func new_current() -> Error:
	_ensure_editor()
	# Reuse a pristine active tab; otherwise the new table gets its own tab and
	# the current one (with its unsaved work) stays open beside it.
	if strings_editor.is_dirty or not strings_editor.current_path.is_empty():
		_tabs.add(_create_document())
		_bind_active_document()
	strings_editor.new_table(true)
	return OK


func can_open() -> bool:
	return true


func get_open_action_label() -> String:
	return "Open Strings..."


func get_open_dialog_title() -> String:
	return "Open strings .bin"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.bin,*.BIN ; Strings"])


func get_open_dialog_dir() -> String:
	return strings_editor.get_last_open_dir() if strings_editor else ""


func get_open_resource_kind() -> String:
	return "strings"


func get_current_resource_path() -> String:
	return strings_editor.current_path if strings_editor else ""


func open_file(path: String) -> Error:
	_ensure_editor()
	# Tab-aware: a table that is already open (matched on the stored document
	# path; VFS entries store their display path, which can differ from the raw
	# archive path) activates its tab — unsaved edits intact — instead of
	# reopening.
	var existing := _tabs.index_of_path(path)
	if existing >= 0:
		return activate_document(existing)
	var vfs := _vfs_root_for_open(path)
	# A pristine active document (fresh workspace, just-seeded tab) is reused so
	# the first open does not leave a stray Untitled tab; otherwise the table
	# opens in its own new tab.
	var reuse := not strings_editor.is_dirty and strings_editor.current_path.is_empty()
	var target: StringsEditor = strings_editor if reuse else _create_document()
	var err: Error
	if vfs != null:
		err = target.open_strings_bytes(vfs.read_file(path), _vfs_display_path(vfs, path))
	else:
		err = target.open_strings(path)
	if err != OK:
		if not reuse:
			(target as Node).queue_free()
		return err
	if reuse:
		_tabs.notify_changed()
	else:
		_tabs.add(target)
		_bind_active_document()
	_save_state()
	return err


# Cross-jump target for the Menus workspace's "Edit in Strings": open the given table
# (activating its tab when already open) and focus a key. Returns the open Error (OK
# when only focusing).
func open_strings_table(path: String, key: String) -> Error:
	_ensure_editor()
	if not path.is_empty() and path != strings_editor.current_path:
		var err := open_file(path)
		if err != OK:
			return err
	return focus_reference(FocusPayload.for_key(key))


# Cross-jump focus hook (EditorWorkspace.focus_reference): `key` filters the
# table to the key and selects its entry. Runs before the shell mounts this
# workspace's viewport, so the search is stashed and applied when mount_viewport
# builds the view.
func focus_reference(focus: FocusPayload) -> Error:
	var key := focus.key
	if key.is_empty():
		return OK
	_ensure_editor()
	set_search(key)
	var idx := strings_editor.string_table.find_entry_by_key(key)
	if idx >= 0:
		strings_editor.selected_index = idx
	return OK


func can_save() -> bool:
	return strings_editor != null and strings_editor.is_dirty and not strings_editor.current_path.is_empty()


func get_save_action_label() -> String:
	return "Save Strings"


func can_save_as() -> bool:
	return strings_editor != null


func get_save_as_action_label() -> String:
	return "Save Strings As..."


func save_current() -> Error:
	var err := strings_editor.save_current() if strings_editor else ERR_UNAVAILABLE
	if err == OK:
		_tabs.notify_changed()  # dirty badge clears
		_save_state()
	return err


func save_as(dir_path: String) -> Error:
	var err := strings_editor.save_as(dir_path) if strings_editor else ERR_UNAVAILABLE
	if err == OK:
		_tabs.notify_changed()  # tab label follows the new filename
		_save_state()
	return err


func get_save_dialog_title() -> String:
	return "Choose where to save the strings .bin"


func get_save_dialog_dir() -> String:
	return strings_editor.get_last_save_dir() if strings_editor else ""

