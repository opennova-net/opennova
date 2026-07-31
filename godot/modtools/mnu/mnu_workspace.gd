class_name MnuEditorWorkspace
extends EditorWorkspace

# Adapter for the Menus workspace. The main viewport owns one shared MnuEditor
# (widget tree + WYSIWYG preview) rebound across document tabs; the right dock
# owns a [Properties | Styles] tab strip. Properties is the per-widget inspector;
# Styles is the menu stylesheet editor (variables/source) plus its per-variable
# inspector. Both tabs operate on shared workspace state — the canvas IS the live
# stylesheet preview, so unsaved Styles edits show on every Menus canvas refresh.
# The adapter forwards the editor's selection to the inspector so the two shell
# regions stay in sync, owns the DocumentTabSet (multi-menu tabs, Strings-pilot
# pattern), AND owns the single shared MnsEditorDocument for the stylesheet.

const MnuEditorDocumentScript = preload("res://modtools/mnu/mnu_editor_document.gd")
const MnuEditorScript = preload("res://modtools/mnu/mnu_editor.gd")
const MnuPropertyInspectorScript = preload("res://modtools/mnu/mnu_property_inspector.gd")
const MnsEditorDocumentScript = preload("res://modtools/mnu/mns_editor_document.gd")
const MnsEditorScript = preload("res://modtools/mnu/mns_editor.gd")
const MnsInspectorScript = preload("res://modtools/mnu/mns_inspector.gd")
const SoundPreviewPlayerScript = preload("res://modtools/sound/sound_preview_player.gd")

# The widgets' <SOUND> file (universal across shipped JO menus); its sets are the
# valid triggers and the source for the inspector's trigger dropdown + preview.
const MENU_SOUND_PROFILE := "menu.lwf"
# The fixed name the original engine looks for ("named menu_style.mns for the
# game to find it"); also the auto-open and Save-As default for the Styles tab.
const MNS_CANONICAL_FILE := "menu_style.mns"

## Session state (open tabs + active), restored on activate like Strings.
const STATE_PATH := "user://mnu_editor_state.cfg"

# Right-dock tabs: Properties (per-widget) and Styles (per-variable).
enum DockTab { PROPERTIES = 0, STYLES = 1 }

# The ACTIVE menu document. Multi-document state lives in _tabs; this alias
# always points at the active tab's document (its name is load-bearing for tests).
var _document   # MnuEditorDocument
var _tabs := DocumentTabSet.new()
# Per-tab undo history, keyed by document. The shared MnuEditor owns the live
# stacks and clears them on set_document, so tab switches stash/restore here.
var _histories: Dictionary = {}
var _state_restored: bool = false
var _editor: Control
var _inspector: Control
var _selected_id := -1
# Single shared stylesheet document — canonically menu_style.mns. Auto-opened
# once per session, like the old Menu Styles workspace did.
var _mns_document   # MnsEditorDocument
var _mns_editor: Control
var _mns_inspector: Control
var _mns_auto_opened: bool = false
# Right-dock tab strip + current selection. Style jumps switch this to STYLES.
var _dock_tab: int = DockTab.PROPERTIES
var _tab_container: TabContainer
var _properties_page: Control
var _styles_page: Control
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
	_mns_document = MnsEditorDocumentScript.new()
	_mns_document.state_changed.connect(_on_mns_state_changed)


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
	_auto_open_stylesheet()


func deactivate() -> void:
	_save_state()


# First activation auto-opens the canonical stylesheet when the resource root
# carries one and the document is still pristine (never clobbers user work).
# Inherited from the pre-merge Menu Styles workspace.
func _auto_open_stylesheet() -> void:
	if _mns_auto_opened:
		return
	_mns_auto_opened = true
	if _mns_document.is_dirty or not _mns_document.current_path.is_empty():
		return
	var root := _resource_root_or_settings()
	if root == null or root.get_root_dir().is_empty():
		return
	var path := String(root.resolve_file(MNS_CANONICAL_FILE))
	if not path.is_empty():
		_open_mns_path(path)
	elif root.has_file(MNS_CANONICAL_FILE):
		_open_mns_path(MNS_CANONICAL_FILE)


# Stylesheet document state (load/dirty/save) changes — refresh the dock
# inspector's name/dirty markers and push the live sheet at the menu canvas so
# unsaved edits hit the preview immediately.
func _on_mns_state_changed() -> void:
	_push_stylesheet_to_editor()
	_populate_styles_inspector()


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


func get_document_tabs() -> Array[DocumentTabRow]:
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
	if _dock_tab == DockTab.STYLES:
		var sheet_name := "untitled"
		if not _mns_document.current_path.is_empty():
			sheet_name = _mns_document.current_path.get_file().get_basename()
		var sheet_dirty := "*" if _mns_document.is_dirty else ""
		return "%s%s" % [sheet_name, sheet_dirty]
	var name := "untitled"
	if not _document.current_path.is_empty():
		name = _document.current_path.get_file().get_basename()
	var dirty := "*" if _document.is_dirty else ""
	return "%s%s" % [name, dirty]


func get_status_tool() -> String:
	return "Menus"


func get_status_context() -> String:
	if _dock_tab == DockTab.STYLES:
		if _mns_document.resource == null:
			return ""
		var count: int = _mns_editor.get_variable_count() if _mns_editor != null and is_instance_valid(_mns_editor) else 0
		var ctx := "%d variable(s)" % count
		var issues: int = _mns_editor.get_diagnostic_count() if _mns_editor != null and is_instance_valid(_mns_editor) else 0
		if issues > 0:
			ctx += ", %d issue(s)" % issues
		return ctx
	if _document.resource == null:
		return ""
	var screens: int = _document.resource.get_screen_count()
	var context := "%d screen(s)" % screens
	if _editor != null:
		var unresolved: int = _editor.get_unresolved_asset_count()
		if unresolved > 0:
			context += ", %d unresolved asset(s)" % unresolved
		var unresolved_vars: int = _editor.get_unresolved_var_count()
		if unresolved_vars > 0:
			context += ", %d style variable(s) unresolved" % unresolved_vars
		if _editor.is_selection_off_board():
			context += ", selection off-board"
	return context


func mount_viewport(mount: Control) -> void:
	if mount == null:
		return
	var created := false
	if _editor == null:
		_editor = MnuEditorScript.new()
		_editor.set_anchors_preset(Control.PRESET_FULL_RECT)
		_editor.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_editor.size_flags_vertical = Control.SIZE_EXPAND_FILL
		_editor.widget_selected.connect(_on_widget_selected)
		_editor.selection_changed.connect(_on_selection_changed)
		_editor.interactive_changed.connect(_on_interactive_changed)
		created = true
	if _editor.get_parent() == null:
		mount.add_child(_editor)
		_editor.set_anchors_preset(Control.PRESET_FULL_RECT)
	_editor.set_resource_root(_resource_root_or_settings())
	_editor.set_document(_document)
	if created:
		# A fresh editor starts with empty stacks; pick up any history parked
		# for the active tab. (A REMOUNT must not touch the live stacks -
		# set_document early-returns for the same document.)
		_editor.restore_history(_histories.get(_document, {}))
	# The Mns editor owns the .mns undo stack and document binding; it needs to
	# be in the tree (for _ready / _build_ui) before build_inspector reparents it
	# into the Styles tab. Park it under the viewport mount (invisible) until then.
	if _mns_editor == null:
		_mns_editor = MnsEditorScript.new()
		_mns_editor.variable_selected.connect(_on_mns_variable_selected)
	if _mns_editor.get_parent() == null:
		mount.add_child(_mns_editor)
		_mns_editor.visible = false
	_mns_editor.set_document(_mns_document)
	_push_stylesheet_to_editor()


func unmount_viewport(_released: Control) -> void:
	if _editor != null and _editor.get_parent() != null:
		_editor.get_parent().remove_child(_editor)
	# When the inspector hasn't been built yet (or has been torn down), the Mns
	# editor lives under the viewport mount. Pull it out so the mount can be reused.
	if _mns_editor != null and _mns_editor.get_parent() != null \
			and _mns_editor.get_parent() != _styles_page:
		_mns_editor.get_parent().remove_child(_mns_editor)


func release_viewport() -> void:
	if _preview != null and is_instance_valid(_preview):
		_preview.queue_free()
	_preview = null
	_profiles.clear()
	if _tab_container != null and is_instance_valid(_tab_container):
		_disconnect_inspector(_inspector)
		_disconnect_mns_inspector(_mns_inspector)
		_tab_container.queue_free()
		_tab_container = null
		_inspector = null
		_mns_inspector = null
		_properties_page = null
		_styles_page = null
	if _editor != null and _editor.get_parent() != null:
		_editor.get_parent().remove_child(_editor)
	if _editor != null:
		_editor.free()
		_editor = null
	# The Mns editor lives in the styles tab body, so build_inspector's tab tear-
	# down already removed it from the tree above. release_viewport just frees it.
	if _mns_editor != null:
		_mns_editor.free()
		_mns_editor = null


func build_inspector(mount: Control) -> void:
	if _tab_container != null and is_instance_valid(_tab_container):
		_disconnect_inspector(_inspector)
		_disconnect_mns_inspector(_mns_inspector)
		_tab_container.queue_free()
		_tab_container = null
		_inspector = null
		_mns_inspector = null
		_properties_page = null
		_styles_page = null

	_tab_container = TabContainer.new()
	_tab_container.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_tab_container.size_flags_vertical = Control.SIZE_EXPAND_FILL
	# Style jumps and dock activity drive the inspector; this signal carries the
	# user's manual switch back the other way.
	_tab_container.tab_changed.connect(_on_dock_tab_changed)
	mount.add_child(_tab_container)

	# --- Properties page ----------------------------------------------------
	_properties_page = MarginContainer.new()
	_properties_page.name = "Properties"
	_properties_page.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_properties_page.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_tab_container.add_child(_properties_page)

	_inspector = MnuPropertyInspectorScript.new()
	_inspector.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_inspector.size_flags_vertical = Control.SIZE_EXPAND_FILL
	# Row commits funnel through the editor so the mutation + undo stay centralized
	# (the inspector never touches the document directly).
	_inspector.edit_requested.connect(_on_inspector_edit)
	# Cross-workspace jumps: the inspector asks to edit a widget's string / font in
	# the Strings / Fonts workspace; the adapter resolves the target and drives
	# the shell. The style jump now stays IN-WORKSPACE (activates the Styles tab).
	_inspector.string_jump_requested.connect(_on_string_jump)
	_inspector.font_jump_requested.connect(_on_font_jump)
	_inspector.menu_jump_requested.connect(_on_menu_jump)
	_inspector.style_jump_requested.connect(_on_style_jump)
	# Preview a widget's sound through the menu .lwf profile (reuses the Sound
	# workspace's audition player); the inspector lists triggers from the same profile.
	_inspector.sound_preview_requested.connect(_on_sound_preview)
	if _editor != null and is_instance_valid(_editor):
		_inspector.set_authoring_enabled(not _editor.is_interactive())
	# Shell link-widget services for FILE references (the screen's text_rsrc);
	# string KEYS resolve through the loaded table instead.
	if editor_shell != null and _inspector.has_method("set_reference_services"):
		_inspector.set_reference_services(ResourceRefWidget.services_from_shell(editor_shell))
	_properties_page.add_child(_inspector)

	# --- Styles page --------------------------------------------------------
	# Styles stacks the .mns editor (toolbar + variables/source) over the per-
	# variable inspector. The canvas is the live preview, so this pane is the
	# whole stylesheet authoring surface.
	_styles_page = VBoxContainer.new()
	_styles_page.name = "Styles"
	_styles_page.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_styles_page.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_styles_page.add_theme_constant_override("separation", 6)
	_tab_container.add_child(_styles_page)

	# The Mns editor was parked under the viewport mount by mount_viewport so its
	# _ready ran; reparent it into the Styles tab body now and make it visible.
	if _mns_editor == null:
		_mns_editor = MnsEditorScript.new()
		_mns_editor.variable_selected.connect(_on_mns_variable_selected)
	if _mns_editor.get_parent() != null:
		_mns_editor.get_parent().remove_child(_mns_editor)
	_mns_editor.visible = true
	_mns_editor.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_mns_editor.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_styles_page.add_child(_mns_editor)
	_mns_editor.set_document(_mns_document)

	_mns_inspector = MnsInspectorScript.new()
	_mns_inspector.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_mns_inspector.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_mns_inspector.edit_requested.connect(_on_mns_inspector_edit)
	_mns_inspector.font_jump_requested.connect(_on_font_jump)
	_mns_inspector.add_requested.connect(_on_mns_add_variable)
	_mns_inspector.set_shell(editor_shell)
	_mns_inspector.set_font_names(_list_font_names())
	_styles_page.add_child(_mns_inspector)
	if _editor != null and is_instance_valid(_editor):
		_mns_editor.set_authoring_enabled(not _editor.is_interactive())
		_mns_inspector.set_authoring_enabled(not _editor.is_interactive())

	_tab_container.set_tab_title(DockTab.PROPERTIES, "Properties")
	_tab_container.set_tab_title(DockTab.STYLES, "Styles")
	_tab_container.current_tab = _dock_tab

	# Populate from the editor's current selection. Subsequent selection changes
	# (user + document reloads, which re-select the first screen) reach the
	# inspector through the editor's widget_selected push (_on_widget_selected),
	# so no document-signal subscription is needed here.
	_populate_inspector()
	_populate_styles_inspector()


func _populate_inspector() -> void:
	if _inspector == null or not is_instance_valid(_inspector):
		return
	if _editor != null:
		_selected_id = _editor.get_selected_id()
	_apply_stylesheet()
	_inspector.show_widget(_document.resource, _selected_id, _text_resource(), _text_resource_path())
	_apply_sound_sets()


# Hand the inspector the editor's resolved stylesheet so %VAR% swatches render
# their themed colors and the rows offer the variable dropdowns.
func _apply_stylesheet() -> void:
	if _inspector == null or not is_instance_valid(_inspector):
		return
	_inspector.set_stylesheet(_editor.get_stylesheet() if _editor != null and is_instance_valid(_editor) else null)


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
		_apply_stylesheet()
		_inspector.show_widget(_document.resource, id, _text_resource(), _text_resource_path())


# A multi-selection (>1 widget) drives the inspector's read-only summary view; the
# active id stays tracked for single-widget operations. Mirrors _on_widget_selected.
func _on_selection_changed(ids: PackedInt32Array) -> void:
	if ids.size() > 0:
		_selected_id = ids[ids.size() - 1]
	if _inspector != null and is_instance_valid(_inspector):
		_apply_stylesheet()
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


func _on_interactive_changed(on: bool) -> void:
	if _inspector != null and is_instance_valid(_inspector):
		_inspector.set_authoring_enabled(not on)
	if _mns_editor != null and is_instance_valid(_mns_editor):
		_mns_editor.set_authoring_enabled(not on)
	if _mns_inspector != null and is_instance_valid(_mns_inspector):
		_mns_inspector.set_authoring_enabled(not on)


# Jump to the Strings workspace focused on this widget's string id, opening the menu's
# resolved text table. No-ops with a hint when no table is loaded (no resource root).
func _on_string_jump(key: String) -> void:
	if _editor == null or not is_instance_valid(_editor):
		return
	var path: String = _editor.get_text_resource_path()
	if path.is_empty():
		_notify_status("No string table is loaded for this menu.", &"warn")
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


# Local style jump: activate the Styles dock tab and select the named %VAR%.
# The merged workspace owns the .mns document directly, so this never crosses a
# shell boundary anymore.
func _on_style_jump(variable: String) -> void:
	_dock_tab = DockTab.STYLES
	if _tab_container != null and is_instance_valid(_tab_container):
		_tab_container.current_tab = DockTab.STYLES
	var clean := variable.strip_edges()
	if not clean.is_empty() and _mns_editor != null and is_instance_valid(_mns_editor):
		_mns_editor.select_variable(clean)
	_populate_styles_inspector()


# Audition a widget's sound: resolve the trigger to a set in its .lwf profile and
# play a member through the shared preview player (the same set->member->.wav path
# the runtime uses). No-ops with a status hint when the profile/set can't resolve.
func _on_sound_preview(trigger: String, file: String) -> void:
	var name := file if not file.is_empty() else MENU_SOUND_PROFILE
	var profile = _profile_for(name)
	if profile == null:
		_notify_status("Sound profile '%s' not found in the resource dir." % name)
		return
	var want := trigger.to_upper()
	for si in range(profile.get_set_count()):
		if String(profile.get_set(si).get("name", "")).to_upper() == want:
			_ensure_preview().preview_set(profile, _resource_root_or_settings(), si)
			return
	_notify_status("Trigger '%s' is not a set in %s." % [trigger, name])


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
		_mount_under_shell(_preview)
	return _preview


# Sever the edit channel before an inspector is freed/replaced, so a deferred
# signal from the outgoing inspector cannot reach a stale editor (mirrors the
# defensive disconnect in fonts_workspace.gd).
func _disconnect_inspector(inspector: Control) -> void:
	if inspector == null or not is_instance_valid(inspector):
		return
	if inspector.edit_requested.is_connected(_on_inspector_edit):
		inspector.edit_requested.disconnect(_on_inspector_edit)
	if inspector.string_jump_requested.is_connected(_on_string_jump):
		inspector.string_jump_requested.disconnect(_on_string_jump)
	if inspector.font_jump_requested.is_connected(_on_font_jump):
		inspector.font_jump_requested.disconnect(_on_font_jump)
	if inspector.menu_jump_requested.is_connected(_on_menu_jump):
		inspector.menu_jump_requested.disconnect(_on_menu_jump)
	if inspector.style_jump_requested.is_connected(_on_style_jump):
		inspector.style_jump_requested.disconnect(_on_style_jump)
	if inspector.sound_preview_requested.is_connected(_on_sound_preview):
		inspector.sound_preview_requested.disconnect(_on_sound_preview)


func _disconnect_mns_inspector(inspector: Control) -> void:
	if inspector == null or not is_instance_valid(inspector):
		return
	if inspector.edit_requested.is_connected(_on_mns_inspector_edit):
		inspector.edit_requested.disconnect(_on_mns_inspector_edit)
	if inspector.font_jump_requested.is_connected(_on_font_jump):
		inspector.font_jump_requested.disconnect(_on_font_jump)
	if inspector.add_requested.is_connected(_on_mns_add_variable):
		inspector.add_requested.disconnect(_on_mns_add_variable)


# --- Styles tab plumbing ------------------------------------------------------

# Selection in the variables table drives the Mns inspector below it.
func _on_mns_variable_selected(_name: String) -> void:
	_populate_styles_inspector()


# Variable-row commits funnel through the Mns editor so undo/refresh stay
# centralized (same contract as the Mnu inspector → Mnu editor flow).
func _on_mns_inspector_edit(edit: Dictionary) -> void:
	apply_stylesheet_edit(edit)


## Shared stylesheet mutation seam for inspector, automation, and tests. The
## workspace keeps preview locking and undo ownership inside MnsEditor.
func apply_stylesheet_edit(edit: Dictionary) -> void:
	if _mns_editor != null and is_instance_valid(_mns_editor):
		_mns_editor.apply_edit(edit)


# "Add variable" delegates to the editor toolbar's add (keeps the naming policy
# in one place).
func _on_mns_add_variable() -> void:
	if _mns_editor != null and is_instance_valid(_mns_editor):
		_mns_editor._on_add_pressed()


# Push the live MnsStyleSheet at the menu canvas so unsaved Styles edits hit the
# preview immediately (no disk round-trip). The MnuEditor falls back to disk
# resolution when override is null.
func _push_stylesheet_to_editor() -> void:
	if _editor != null and is_instance_valid(_editor):
		_editor.set_stylesheet_resource(_mns_document.resource)
		# A stylesheet swap can resolve previously unresolved %VAR% tokens in the
		# inspector dropdowns: re-populate so swatches update too.
		_apply_stylesheet()


# Rebuild the Mns inspector body from the current variable selection.
func _populate_styles_inspector() -> void:
	if _mns_inspector == null or not is_instance_valid(_mns_inspector):
		return
	var selected: String = _mns_editor.get_selected_variable() \
		if _mns_editor != null and is_instance_valid(_mns_editor) else ""
	if selected.is_empty():
		_mns_inspector.show_none(_mns_document.resource)
	else:
		_mns_inspector.show_variable(_mns_document.resource, selected)


# The user manually flipped the dock tab — track it so save/new/open actions
# (and undo routing via get_editor_document) target the right document.
func _on_dock_tab_changed(tab_index: int) -> void:
	_dock_tab = tab_index


# Common open path used by activate's auto-open AND open_file's .mns branch.
func _open_mns_path(path: String) -> Error:
	var vfs := _vfs_root_for_open(path)
	if vfs != null:
		return _mns_document.open_mns_bytes(vfs.read_file(path), _vfs_display_path(vfs, path))
	return _mns_document.open_mns(path)


# A save that CREATES the stylesheet inside the resource root must rescan it:
# the VFS name index is built once at mount, so the new file would otherwise
# stay invisible to by-name reads (the Menus canvas, the quick-open browser,
# runtime parity) until the next mount.
func _after_mns_save(err: Error) -> Error:
	if err != OK:
		return err
	var root := _resource_root_or_settings()
	if root != null and not _mns_document.current_path.is_empty():
		var name := String(_mns_document.current_path).get_file()
		if not root.has_file(name) and editor_shell != null \
				and editor_shell.has_method("rescan_resource_root"):
			editor_shell.rescan_resource_root()
	return err


func _is_stylesheet_path(path: String) -> bool:
	return path.get_extension().to_lower() == "mns"


# Font basenames (with .fnt suffix) for the Mns inspector's font pick menu —
# also feeds the Mnu inspector's font row dropdown via its own discovery path.
func _list_font_names() -> PackedStringArray:
	var root := _resource_root_or_settings()
	if root == null:
		return PackedStringArray()
	var out := PackedStringArray()
	for path_value in root.list_files(".fnt"):
		out.append(String(path_value).get_file())
	return out


## The shared stylesheet document rides alongside the base's tab fold.
func has_unsaved_changes() -> bool:
	return (_mns_document != null and _mns_document.is_dirty) or super.has_unsaved_changes()


func can_new() -> bool:
	return true


func get_new_action_label() -> String:
	return "New Stylesheet" if _dock_tab == DockTab.STYLES else "New Menu"


func new_current() -> Error:
	if _dock_tab == DockTab.STYLES:
		return _mns_document.create_new()
	# Reuse a pristine active tab; otherwise the new menu gets its own tab and
	# the current one (with its unsaved work) stays open beside it.
	if _document.is_dirty or not _document.current_path.is_empty():
		_tabs.add(_create_document())
		_bind_active_document()
	return _document.create_new()


func can_open() -> bool:
	return true


func get_open_action_label() -> String:
	return "Open Stylesheet..." if _dock_tab == DockTab.STYLES else "Open Menu..."


func get_open_dialog_title() -> String:
	return "Open .mns" if _dock_tab == DockTab.STYLES else "Open .mnu"


func get_open_dialog_filters() -> PackedStringArray:
	if _dock_tab == DockTab.STYLES:
		return PackedStringArray(["*.mns,*.MNS ; Nova menu styles"])
	return PackedStringArray(["*.mnu,*.MNU ; Nova menus"])


func get_open_dialog_dir() -> String:
	if _dock_tab == DockTab.STYLES:
		return _mns_document.get_last_open_dir()
	return _document.get_last_open_dir()


# "menu" is the resource-index kind for *.mnu (see resource_index kind_for_path), so
# Open uses the shared indexed quick-open browser like the other workspaces. When no
# resource directory is configured the browser falls back to a native *.mnu dialog.
# The merged workspace ALSO claims "menu_style" (see get_open_resource_kinds) so a
# quick-open on a .mns lands here with the Styles tab pre-activated.
func get_open_resource_kind() -> String:
	return "menu"


func get_open_resource_kinds() -> PackedStringArray:
	return PackedStringArray(["menu", "menu_style"])


func get_current_resource_path() -> String:
	# The shell uses this to skip a reopen when the path matches. Route by the
	# dock tab so jumps to a .mns the workspace already owns don't re-open it.
	if _dock_tab == DockTab.STYLES:
		return _mns_document.current_path
	return _document.current_path


func open_file(path: String) -> Error:
	# .mns paths land in the stylesheet document (single doc; the Styles tab is
	# activated below). .mnu paths land in the multi-document menu tab set.
	if _is_stylesheet_path(path):
		var err := _open_mns_path(path)
		_dock_tab = DockTab.STYLES
		if _tab_container != null and is_instance_valid(_tab_container):
			_tab_container.current_tab = DockTab.STYLES
		return err
	# Tab-aware: a menu that is already open activates its tab — unsaved edits
	# intact — instead of reopening. (Menus open from disk only; no VFS branch.)
	var existing := _tabs.index_of_path(path)
	if existing >= 0:
		var open_doc: Variant = _tabs.get_at(existing)
		if open_doc != null and not open_doc.is_dirty:
			var reload_err: Error = open_doc.open_mnu(path)
			if reload_err != OK:
				return reload_err
		var activate_err := activate_document(existing)
		if activate_err == OK:
			_show_properties_dock()
		return activate_err
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
	_show_properties_dock()
	return err


func _show_properties_dock() -> void:
	_dock_tab = DockTab.PROPERTIES
	if _tab_container != null and is_instance_valid(_tab_container):
		_tab_container.current_tab = DockTab.PROPERTIES


# Cross-jump focus hook (EditorWorkspace.focus_reference):
#   screen   — select the named screen in the active menu tab.
#   variable — activate the Styles tab and select the variable (the Mns
#              "Used by" strip stays the same wiring; it jumps here on a
#              referrer click).
func focus_reference(focus: FocusPayload) -> Error:
	var variable := focus.variable.strip_edges()
	if not variable.is_empty():
		_on_style_jump(variable)
		return OK if _mns_editor != null and is_instance_valid(_mns_editor) \
				and _mns_editor.get_selected_variable() == variable \
			else ERR_DOES_NOT_EXIST
	return focus_screen_named(focus.screen)


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
	if _dock_tab == DockTab.STYLES:
		return _mns_document.is_dirty and not _mns_document.current_path.is_empty()
	return _document.is_dirty and not _document.current_path.is_empty()


func get_save_action_label() -> String:
	return "Save Stylesheet" if _dock_tab == DockTab.STYLES else "Save Menu"


func can_save_as() -> bool:
	if _dock_tab == DockTab.STYLES:
		return _mns_document.resource != null
	return _document.resource != null


func get_save_as_action_label() -> String:
	return "Save Stylesheet As..." if _dock_tab == DockTab.STYLES else "Save Menu As..."


func save_current() -> Error:
	# The shell's dirty-close Save flow routes to Save As only on
	# ERR_INVALID_PARAMETER; the document base returns ERR_UNAVAILABLE for a
	# pathless save, so translate at the workspace boundary (the document-level
	# contract stays pinned for fonts/credits).
	if _dock_tab == DockTab.STYLES:
		if _mns_document.current_path.is_empty():
			return ERR_INVALID_PARAMETER
		return _after_mns_save(_mns_document.save_current())
	if _document.current_path.is_empty():
		return ERR_INVALID_PARAMETER
	var err: Error = _document.save_current()
	if err == OK:
		_save_state()
	return err


func save_as(dir_path: String) -> Error:
	if _dock_tab == DockTab.STYLES:
		return _after_mns_save(_mns_document.save_as(dir_path))
	var err: Error = _document.save_as(dir_path)
	if err == OK:
		_save_state()
	return err


func get_save_dialog_title() -> String:
	return "Choose where to save the stylesheet" if _dock_tab == DockTab.STYLES \
		else "Choose where to save the menu"


func get_save_dialog_dir() -> String:
	if _dock_tab == DockTab.STYLES:
		var last := String(_mns_document.get_last_save_dir())
		if not last.is_empty():
			return last
		# Default to the resource root: a loose menu_style.mns there shadows any
		# PFF-archived one at runtime, which is the canonical modder flow.
		var root := _resource_root_or_settings()
		return root.get_root_dir() if root != null else ""
	return _document.get_last_save_dir()


# Undo lives in whichever editor is active for the user's current tab — the
# canvas/property edits run through MnuEditor, stylesheet edits through
# MnsEditor. The shell derives undo/redo from this returned editor.
func get_editor_document() -> Object:
	if _dock_tab == DockTab.STYLES and _mns_editor != null and is_instance_valid(_mns_editor):
		return _mns_editor
	return _editor


## The menu tool surface always targets the canvas editor, independent of which
## inspector dock currently owns shell undo/redo.
func get_menu_editor() -> Object:
	return _editor


## Current typed authoring resources. Consumers cross the workspace interface
## instead of depending on its document-wrapper layout.
func get_menu_document() -> MnuEditorDocument:
	return _document


func get_menu_resource() -> NovaMnuDocument:
	return _document.resource if _document != null else null


func get_stylesheet_resource() -> MnsStyleSheet:
	return _mns_document.resource if _mns_document != null else null


# Commit BOTH editors' deferred buffers (only the Mns source view has one
# today, but a uniform flush keeps the shell's save/export contract simple).
func flush_pending_edits() -> Error:
	if _mns_editor != null and is_instance_valid(_mns_editor):
		var err: Error = _mns_editor.flush_pending_edits()
		if err != OK:
			return err
	return OK
