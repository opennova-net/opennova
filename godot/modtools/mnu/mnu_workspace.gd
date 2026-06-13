class_name MnuEditorWorkspace
extends EditorWorkspace

# Adapter for the Menus workspace. The main viewport hosts one shared MnuEditor
# (widget tree + WYSIWYG preview) rebound across document tabs; the right dock
# hosts the property inspector. The adapter forwards the editor's selection to
# the inspector so the two shell regions stay in sync, and owns the
# DocumentTabSet (multi-menu tabs, Strings-pilot pattern).

const MnuEditorDocumentScript = preload("res://modtools/mnu/mnu_editor_document.gd")
const MnuEditorScript = preload("res://modtools/mnu/mnu_editor.gd")
const MnuPropertyInspectorScript = preload("res://modtools/mnu/mnu_property_inspector.gd")
const SoundPreviewPlayerScript = preload("res://modtools/sound/sound_preview_player.gd")

# The widgets' <SOUND> file (universal across shipped JO menus); its sets are the
# valid triggers and the source for the inspector's trigger dropdown + preview.
const MENU_SOUND_PROFILE := "menu.lwf"

## Session state (open tabs + active), restored on activate like Strings.
const STATE_PATH := "user://mnu_editor_state.cfg"

# The ACTIVE document. Multi-document state lives in _tabs; this alias always
# points at the active tab's document (its name is load-bearing for tests).
var _document   # MnuEditorDocument
var _tabs := DocumentTabSet.new()
# Per-tab undo history, keyed by document. The shared MnuEditor owns the live
# stacks and clears them on set_document, so tab switches stash/restore here.
var _histories: Dictionary = {}
var _state_restored: bool = false
var _editor: Control
var _inspector: Control
var _selected_id := -1
# Editor-local sound audition (reused from the Sound workspace) + a cache of loaded
# .lwf profiles keyed by file name, so the inspector can preview a widget's sound.
var _preview              # SoundPreviewPlayer
var _profiles: Dictionary = {}   # lower-case .lwf name -> NovaLwfData (or null if absent)


func _init() -> void:
	# A named method, not a lambda: a lambda touching a member signal captures
	# self strongly and would cycle workspace <-> tab set (both RefCounted).
	_tabs.changed.connect(_on_tabs_changed)
	_document = _create_document()
	_tabs.add(_document)


func _on_tabs_changed() -> void:
	documents_changed.emit()


func _create_document():
	var doc = MnuEditorDocumentScript.new()
	# One channel covers dirty flips, label changes after save-as, and
	# save-clears: EditorResourceDocument emits state_changed for all of them.
	# No .bind(doc): binding the doc into its own signal's callable would make
	# the RefCounted document reference itself and leak on close/failed open.
	doc.state_changed.connect(_on_doc_state_changed)
	return doc


func _on_doc_state_changed() -> void:
	_tabs.notify_changed()


## Repoint the alias + the shared editor/inspector at the active tab's document,
## stashing the outgoing document's undo history (the editor clears its stacks
## on set_document) and restoring the incoming one's.
func _bind_active_document() -> void:
	var active = _tabs.get_active()
	if active == null or active == _document:
		return
	if _editor != null and is_instance_valid(_editor) and _document != null \
			and _tabs.index_of(_document) >= 0:
		# Only stash documents still open: a just-closed tab's history dies with it.
		_histories[_document] = _editor.take_history()
	_document = active
	if _editor != null and is_instance_valid(_editor):
		_editor.set_document(_document)
		_editor.restore_history(_histories.get(_document, {}))
	_populate_inspector()


# --- Session persistence ---

func activate() -> void:
	_restore_state()


func deactivate() -> void:
	_save_state()


func _restore_state() -> void:
	# Reopen the last session's tabs once per session, and only while the
	# workspace is still pristine (one tab, no path, no edits) so it never
	# clobbers user work. A cross-jump open lands BEFORE activate and both skips
	# the restore and (via its _save_state) replaces the saved session with the
	# jumped-to menu - the session always reflects the tabs actually open.
	if _state_restored:
		return
	_state_restored = true
	if _tabs.count() != 1 or _document.is_dirty or not _document.current_path.is_empty():
		return
	var cfg := ConfigFile.new()
	if cfg.load(STATE_PATH) != OK:
		return
	var paths: PackedStringArray = cfg.get_value("session", "open_paths", PackedStringArray())
	for path in paths:
		if not path.is_empty() and FileAccess.file_exists(path):
			open_file(path)
	var active := int(cfg.get_value("session", "active_index", _tabs.count() - 1))
	if active >= 0 and active < _tabs.count():
		activate_document(active)


func _save_state() -> void:
	var cfg := ConfigFile.new()
	cfg.load(STATE_PATH)  # keep unrelated values if the file exists
	var open := _tabs.open_paths()
	cfg.set_value("session", "open_paths", open)
	# active_index is stored in open_paths space: pathless (Untitled) tabs are
	# not persisted, so a full-list index would drift past them on restore.
	var active := -1
	var active_doc = _tabs.get_active()
	if active_doc != null:
		var path := String(active_doc.get("current_path"))
		if not path.is_empty():
			active = open.find(path)
	cfg.set_value("session", "active_index", active)
	cfg.save(STATE_PATH)


# --- Document tabs (EditorWorkspace tier) ---

func supports_document_tabs() -> bool:
	return true


func get_document_tabs() -> Array:
	return _tabs.tabs()


func get_active_document_index() -> int:
	return _tabs.get_active_index()


func activate_document(index: int) -> Error:
	var err := _tabs.set_active(index)
	if err != OK:
		return err
	_bind_active_document()
	_save_state()
	return OK


func close_document(index: int) -> Error:
	var doc = _tabs.get_at(index)
	if doc == null:
		return ERR_INVALID_PARAMETER
	var was_active := index == _tabs.get_active_index()
	_tabs.remove_at(index)
	_histories.erase(doc)
	if _tabs.count() == 0:
		# The workspace never holds zero documents: seed a fresh pristine menu.
		_tabs.add(_create_document())
		was_active = true
	if was_active:
		_bind_active_document()
	_save_state()
	return OK


func get_workspace_id() -> String:
	return "mnu"


func get_workspace_label() -> String:
	return "Menus"


func get_workspace_tooltip() -> String:
	return "Open and preview Nova *.mnu menu screens: widget tree and layout."


func get_project_title() -> String:
	var name := "untitled"
	if not _document.current_path.is_empty():
		name = _document.current_path.get_file().get_basename()
	var dirty := "*" if _document.is_dirty else ""
	return "%s%s" % [name, dirty]


func get_status_tool() -> String:
	return "Menus"


func get_status_context() -> String:
	if _document.resource == null:
		return ""
	var screens: int = _document.resource.get_screen_count()
	var context := "%d screen(s)" % screens
	if _editor != null:
		var unresolved: int = _editor.get_unresolved_asset_count()
		if unresolved > 0:
			context += ", %d unresolved asset(s)" % unresolved
		if _editor.is_selection_off_board():
			context += ", selection off-board"
	return context


func mount_viewport(host: Control) -> void:
	if host == null:
		return
	var created := false
	if _editor == null:
		_editor = MnuEditorScript.new()
		_editor.set_anchors_preset(Control.PRESET_FULL_RECT)
		_editor.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_editor.size_flags_vertical = Control.SIZE_EXPAND_FILL
		_editor.widget_selected.connect(_on_widget_selected)
		_editor.selection_changed.connect(_on_selection_changed)
		created = true
	if _editor.get_parent() == null:
		host.add_child(_editor)
		_editor.set_anchors_preset(Control.PRESET_FULL_RECT)
	_editor.set_resource_root(_resource_root_or_settings())
	_editor.set_document(_document)
	if created:
		# A fresh editor starts with empty stacks; pick up any history parked
		# for the active tab. (A REMOUNT must not touch the live stacks -
		# set_document early-returns for the same document.)
		_editor.restore_history(_histories.get(_document, {}))


func unmount_viewport(_host: Control) -> void:
	if _editor != null and _editor.get_parent() != null:
		_editor.get_parent().remove_child(_editor)


func release_viewport() -> void:
	if _preview != null and is_instance_valid(_preview):
		_preview.queue_free()
	_preview = null
	_profiles.clear()
	if _inspector != null and is_instance_valid(_inspector):
		_disconnect_inspector(_inspector)
		_inspector.queue_free()
		_inspector = null
	if _editor != null and _editor.get_parent() != null:
		_editor.get_parent().remove_child(_editor)
	if _editor != null:
		_editor.free()
		_editor = null


func build_inspector(host: Control) -> void:
	if _inspector != null and is_instance_valid(_inspector):
		_disconnect_inspector(_inspector)
		_inspector.queue_free()
	_inspector = MnuPropertyInspectorScript.new()
	_inspector.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_inspector.size_flags_vertical = Control.SIZE_EXPAND_FILL
	# Row commits funnel through the editor so the mutation + undo stay centralized
	# (the inspector never touches the document directly).
	_inspector.edit_requested.connect(_on_inspector_edit)
	# Cross-workspace jumps: the inspector asks to edit a widget's string / font in the
	# Strings / Fonts workspace; the adapter resolves the target and drives the shell.
	_inspector.string_jump_requested.connect(_on_string_jump)
	_inspector.font_jump_requested.connect(_on_font_jump)
	_inspector.menu_jump_requested.connect(_on_menu_jump)
	# Preview a widget's sound through the menu .lwf profile (reuses the Sound
	# workspace's audition player); the inspector lists triggers from the same profile.
	_inspector.sound_preview_requested.connect(_on_sound_preview)
	# Shell link-widget services for FILE references (the screen's text_rsrc);
	# string KEYS resolve through the loaded table instead.
	if editor_shell != null and _inspector.has_method("set_reference_services"):
		_inspector.set_reference_services(ResourceRefWidget.services_from_shell(editor_shell))
	host.add_child(_inspector)
	# Populate from the editor's current selection. Subsequent selection changes
	# (user + document reloads, which re-select the first screen) reach the
	# inspector through the editor's widget_selected push (_on_widget_selected), so
	# no document-signal subscription is needed here.
	_populate_inspector()


func _populate_inspector() -> void:
	if _inspector == null or not is_instance_valid(_inspector):
		return
	if _editor != null:
		_selected_id = _editor.get_selected_id()
	_inspector.show_widget(_document.resource, _selected_id, _text_resource(), _text_resource_path())
	_apply_sound_sets()


# Feed the inspector the menu profile's set names so its Sounds trigger field
# becomes a dropdown over the real triggers (MOUSE_OVER/CLICK_SELECT/...). Empty
# when no menu.lwf resolves (the inspector then falls back to free-text entry).
func _apply_sound_sets() -> void:
	if _inspector == null or not is_instance_valid(_inspector):
		return
	var profile = _profile_for(MENU_SOUND_PROFILE)
	var names := PackedStringArray()
	if profile != null:
		for si in range(profile.get_set_count()):
			names.append(String(profile.get_set(si).get("name", "")))
	_inspector.set_sound_sets(names)


func _on_widget_selected(id: int) -> void:
	_selected_id = id
	if _inspector != null and is_instance_valid(_inspector):
		_inspector.show_widget(_document.resource, id, _text_resource(), _text_resource_path())


# A multi-selection (>1 widget) drives the inspector's read-only summary view; the
# active id stays tracked for single-widget operations. Mirrors _on_widget_selected.
func _on_selection_changed(ids: PackedInt32Array) -> void:
	if ids.size() > 0:
		_selected_id = ids[ids.size() - 1]
	if _inspector != null and is_instance_valid(_inspector):
		_inspector.show_selection(_document.resource, ids, _text_resource(), _text_resource_path())


# The string table the editor resolved for the open menu (null when none loaded), so
# the inspector can show resolved text + drive the picker.
func _text_resource() -> RtxtStringFile:
	return _editor.get_text_resource() if _editor != null and is_instance_valid(_editor) else null


# Its resolved path - the string widgets' badge tooltip and Strings jump target.
func _text_resource_path() -> String:
	return _editor.get_text_resource_path() if _editor != null and is_instance_valid(_editor) else ""


func _on_inspector_edit(edit: Dictionary) -> void:
	if _editor != null and is_instance_valid(_editor):
		_editor.apply_edit(edit)


# Jump to the Strings workspace focused on this widget's string id, opening the menu's
# resolved text table. No-ops with a hint when no table is loaded (no resource root).
func _on_string_jump(key: String) -> void:
	if _editor == null or not is_instance_valid(_editor):
		return
	var path: String = _editor.get_text_resource_path()
	if path.is_empty():
		if editor_shell != null and editor_shell.has_method("show_status_message"):
			editor_shell.show_status_message("No string table is loaded for this menu.", 4.0)
		return
	if editor_shell != null and editor_shell.has_method("open_strings_workspace"):
		editor_shell.open_strings_workspace(path, key)


# Jump to the Fonts workspace for this widget's font (reuses the shell's existing
# open_font_workspace cross-jump).
func _on_font_jump(font: String) -> void:
	if editor_shell != null and editor_shell.has_method("open_font_workspace"):
		editor_shell.open_font_workspace(font)


# Jump to the Menus workspace for a cross-file screen action. The shell resolves
# the file against the configured resource root, then opens/focuses the target menu.
func _on_menu_jump(file: String, screen: String) -> void:
	if editor_shell != null and editor_shell.has_method("open_menu_workspace"):
		editor_shell.open_menu_workspace(file, screen)


# Audition a widget's sound: resolve the trigger to a set in its .lwf profile and
# play a member through the shared preview player (the same set->member->.wav path
# the runtime uses). No-ops with a status hint when the profile/set can't resolve.
func _on_sound_preview(trigger: String, file: String) -> void:
	var name := file if not file.is_empty() else MENU_SOUND_PROFILE
	var profile = _profile_for(name)
	if profile == null:
		_status("Sound profile '%s' not found in the resource dir." % name)
		return
	var want := trigger.to_upper()
	for si in range(profile.get_set_count()):
		if String(profile.get_set(si).get("name", "")).to_upper() == want:
			_ensure_preview().preview_set(profile, _resource_root_or_settings(), si)
			return
	_status("Trigger '%s' is not a set in %s." % [trigger, name])


# Load (and cache) a .lwf profile by name through the resource root. Caches misses
# as null so a missing profile is not retried every preview/selection.
func _profile_for(name: String):
	var key := name.to_lower()
	if _profiles.has(key):
		return _profiles[key]
	var profile = null
	var root := _resource_root_or_settings()
	if root != null:
		var d := NovaLwfData.new()
		if d.open_from_resource_root(root, name) == OK and d.is_loaded() and d.get_set_count() > 0:
			profile = d
	_profiles[key] = profile
	return profile


func _ensure_preview():
	# The workspace is a RefCounted adapter (not a Node), so the audition player
	# lives under the shell. Without a shell (headless), preview is a no-op.
	if _preview == null or not is_instance_valid(_preview):
		_preview = SoundPreviewPlayerScript.new()
		_preview.name = "MnuSoundPreview"
		if editor_shell != null:
			editor_shell.add_child(_preview)
	return _preview


func _status(message: String) -> void:
	if editor_shell != null and editor_shell.has_method("show_status_message"):
		editor_shell.show_status_message(message, 4.0)


# Sever the edit channel before an inspector is freed/replaced, so a deferred
# signal from the outgoing inspector cannot reach a stale editor (mirrors the
# defensive disconnect in fonts_workspace.gd).
func _disconnect_inspector(inspector: Control) -> void:
	if inspector.edit_requested.is_connected(_on_inspector_edit):
		inspector.edit_requested.disconnect(_on_inspector_edit)
	if inspector.string_jump_requested.is_connected(_on_string_jump):
		inspector.string_jump_requested.disconnect(_on_string_jump)
	if inspector.font_jump_requested.is_connected(_on_font_jump):
		inspector.font_jump_requested.disconnect(_on_font_jump)
	if inspector.menu_jump_requested.is_connected(_on_menu_jump):
		inspector.menu_jump_requested.disconnect(_on_menu_jump)
	if inspector.sound_preview_requested.is_connected(_on_sound_preview):
		inspector.sound_preview_requested.disconnect(_on_sound_preview)


## Any open tab with unsaved work counts, not just the active one.
func has_unsaved_changes() -> bool:
	for row in _tabs.tabs():
		if bool((row as Dictionary).get("dirty", false)):
			return true
	return false


func can_new() -> bool:
	return true


func get_new_action_label() -> String:
	return "New Menu"


func new_current() -> Error:
	# Reuse a pristine active tab; otherwise the new menu gets its own tab and
	# the current one (with its unsaved work) stays open beside it.
	if _document.is_dirty or not _document.current_path.is_empty():
		_tabs.add(_create_document())
		_bind_active_document()
	return _document.create_new()


func can_open() -> bool:
	return true


func get_open_action_label() -> String:
	return "Open Menu..."


func get_open_dialog_title() -> String:
	return "Open .mnu"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray(["*.mnu,*.MNU ; Nova menus"])


func get_open_dialog_dir() -> String:
	return _document.get_last_open_dir()


# "menu" is the resource-index kind for *.mnu (see resource_index kind_for_path), so
# Open uses the shared indexed quick-open browser like the other workspaces. When no
# resource directory is configured the browser falls back to a native *.mnu dialog.
func get_open_resource_kind() -> String:
	return "menu"


func get_current_resource_path() -> String:
	return _document.current_path


func open_file(path: String) -> Error:
	# Tab-aware: a menu that is already open activates its tab. A CLEAN tab
	# reloads from disk first (the file may have changed underneath — externally
	# edited or rewritten by another tool); unsaved edits stay intact and win.
	# (Menus open from disk only; no VFS branch.)
	var existing := _tabs.index_of_path(path)
	if existing >= 0:
		var open_doc: Variant = _tabs.get_at(existing)
		if open_doc != null and not open_doc.is_dirty:
			var reload_err: Error = open_doc.open_mnu(path)
			if reload_err != OK:
				return reload_err
		return activate_document(existing)
	# A pristine active document (fresh workspace, just-seeded tab) is reused so
	# the first open does not leave a stray Untitled tab.
	var reuse: bool = not _document.is_dirty and _document.current_path.is_empty()
	var target = _document if reuse else _create_document()
	var err: Error = target.open_mnu(path)
	if err != OK:
		# open_mnu validates before adopting, so a failed open on a new document
		# just drops it (RefCounted) with the current tab untouched.
		return err
	if reuse:
		_tabs.notify_changed()
	else:
		_tabs.add(target)
		_bind_active_document()
	_save_state()
	return err


# Cross-jump focus hook (EditorWorkspace.focus_reference): {"screen": String}
# selects the named screen.
func focus_reference(focus: Dictionary) -> Error:
	return focus_screen_named(String(focus.get("screen", "")))


func focus_screen_named(screen_name: String) -> Error:
	var name := screen_name.strip_edges()
	if name.is_empty():
		return OK
	if _document.resource == null:
		return ERR_UNAVAILABLE
	for screen_id in _document.resource.get_screen_ids():
		if _document.resource.get_screen_name(screen_id) == name:
			_selected_id = screen_id
			if _editor != null and is_instance_valid(_editor):
				_editor.select_widget(screen_id)
			_populate_inspector()
			return OK
	return ERR_DOES_NOT_EXIST


func can_save() -> bool:
	return _document.is_dirty and not _document.current_path.is_empty()


func get_save_action_label() -> String:
	return "Save Menu"


func can_save_as() -> bool:
	return _document.resource != null


func get_save_as_action_label() -> String:
	return "Save Menu As..."


func save_current() -> Error:
	# The shell's dirty-close Save flow routes to Save As only on
	# ERR_INVALID_PARAMETER; the document base returns ERR_UNAVAILABLE for a
	# pathless save, so translate at the workspace boundary (the document-level
	# contract stays pinned for fonts/credits).
	if _document.current_path.is_empty():
		return ERR_INVALID_PARAMETER
	var err: Error = _document.save_current()
	if err == OK:
		_save_state()
	return err


func save_as(dir_path: String) -> Error:
	var err: Error = _document.save_as(dir_path)
	if err == OK:
		_save_state()
	return err


func get_save_dialog_title() -> String:
	return "Choose where to save the menu"


func get_save_dialog_dir() -> String:
	return _document.get_last_save_dir()


# Undo lives in the editor (shared by inspector commits + canvas gestures) while
# dirty stays on the document, so has_unsaved_changes keeps its override and the
# base derives undo/redo from the editor control.
func get_editor_document() -> Object:
	return _editor
