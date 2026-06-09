class_name MnuEditorWorkspace
extends EditorWorkspace

# Single-pane adapter for the Menus workspace (mirrors fonts_workspace.gd). The
# main viewport hosts the MnuEditor (widget tree + WYSIWYG preview); the right
# dock hosts the read-only property inspector. The adapter forwards the editor's
# selection to the inspector so the two shell regions stay in sync. M6 is
# read-only browse; M7 adds property edit + undo, M8 adds canvas gestures.

const MnuEditorDocumentScript = preload("res://modtools/mnu/mnu_editor_document.gd")
const MnuEditorScript = preload("res://modtools/mnu/mnu_editor.gd")
const MnuPropertyInspectorScript = preload("res://modtools/mnu/mnu_property_inspector.gd")
const SoundPreviewPlayerScript = preload("res://modtools/sound/sound_preview_player.gd")

# The widgets' <SOUND> file (universal across shipped JO menus); its sets are the
# valid triggers and the source for the inspector's trigger dropdown + preview.
const MENU_SOUND_PROFILE := "menu.lwf"

var _document   # MnuEditorDocument
var _editor: Control
var _inspector: Control
var _selected_id := -1
# Editor-local sound audition (reused from the Sound workspace) + a cache of loaded
# .lwf profiles keyed by file name, so the inspector can preview a widget's sound.
var _preview              # SoundPreviewPlayer
var _profiles: Dictionary = {}   # lower-case .lwf name -> NovaLwfData (or null if absent)


func _init() -> void:
	_document = MnuEditorDocumentScript.new()


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
	if _editor == null:
		_editor = MnuEditorScript.new()
		_editor.set_anchors_preset(Control.PRESET_FULL_RECT)
		_editor.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_editor.size_flags_vertical = Control.SIZE_EXPAND_FILL
		_editor.widget_selected.connect(_on_widget_selected)
		_editor.selection_changed.connect(_on_selection_changed)
	if _editor.get_parent() == null:
		host.add_child(_editor)
		_editor.set_anchors_preset(Control.PRESET_FULL_RECT)
	_editor.set_resource_root(_resource_root())
	_editor.set_document(_document)


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
	# Preview a widget's sound through the menu .lwf profile (reuses the Sound
	# workspace's audition player); the inspector lists triggers from the same profile.
	_inspector.sound_preview_requested.connect(_on_sound_preview)
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
	_inspector.show_widget(_document.resource, _selected_id, _text_resource())
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
		_inspector.show_widget(_document.resource, id, _text_resource())


# A multi-selection (>1 widget) drives the inspector's read-only summary view; the
# active id stays tracked for single-widget operations. Mirrors _on_widget_selected.
func _on_selection_changed(ids: PackedInt32Array) -> void:
	if ids.size() > 0:
		_selected_id = ids[ids.size() - 1]
	if _inspector != null and is_instance_valid(_inspector):
		_inspector.show_selection(_document.resource, ids, _text_resource())


# The string table the editor resolved for the open menu (null when none loaded), so
# the inspector can show resolved text + drive the picker.
func _text_resource() -> RtxtStringFile:
	return _editor.get_text_resource() if _editor != null and is_instance_valid(_editor) else null


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
			_ensure_preview().preview_set(profile, _resource_root(), si)
			return
	_status("Trigger '%s' is not a set in %s." % [trigger, name])


# Load (and cache) a .lwf profile by name through the resource root. Caches misses
# as null so a missing profile is not retried every preview/selection.
func _profile_for(name: String):
	var key := name.to_lower()
	if _profiles.has(key):
		return _profiles[key]
	var profile = null
	var root := _resource_root()
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
	if inspector.sound_preview_requested.is_connected(_on_sound_preview):
		inspector.sound_preview_requested.disconnect(_on_sound_preview)


func _resource_root() -> NovaResourceRoot:
	if editor_shell != null and editor_shell.has_method("get_resource_root"):
		return editor_shell.get_resource_root()
	var dir := NovaResourceDirSettings.get_resource_dir()
	if dir.is_empty():
		return null
	var resources := NovaResourceRoot.new()
	return resources if resources.set_root_dir(dir) == OK else null


func has_unsaved_changes() -> bool:
	return _document.is_dirty


func can_new() -> bool:
	return true


func get_new_action_label() -> String:
	return "New Menu"


func new_current() -> Error:
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
	return _document.open_mnu(path)


func can_save() -> bool:
	return _document.is_dirty and not _document.current_path.is_empty()


func get_save_action_label() -> String:
	return "Save Menu"


func can_save_as() -> bool:
	return _document.resource != null


func get_save_as_action_label() -> String:
	return "Save Menu As..."


func save_current() -> Error:
	return _document.save_current()


func save_as(dir_path: String) -> Error:
	return _document.save_as(dir_path)


func get_save_dialog_title() -> String:
	return "Choose where to save the menu"


func get_save_dialog_dir() -> String:
	return _document.get_last_save_dir()


# Undo lives in the editor (shared by inspector commits + future canvas gestures);
# the shell drives it through these hooks. Delegated defensively so it is safe
# before mount / after release.
func can_undo() -> bool:
	return _editor != null and _editor.has_method("can_undo") and _editor.can_undo()


func can_redo() -> bool:
	return _editor != null and _editor.has_method("can_redo") and _editor.can_redo()


func undo() -> void:
	if _editor != null and _editor.has_method("undo"):
		_editor.undo()


func redo() -> void:
	if _editor != null and _editor.has_method("redo"):
		_editor.redo()
