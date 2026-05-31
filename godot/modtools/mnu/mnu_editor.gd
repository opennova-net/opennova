class_name MnuEditor
extends Control

# The Menus workspace editor surface: a widget tree alongside a live WYSIWYG
# preview. Binds to an MnuEditorDocument, drives the canvas preview, and tracks
# selection by the document's stable widget id. Selection is re-emitted as
# widget_selected(id) so the workspace adapter can populate the right-dock
# property inspector.
#
# M7: this editor owns the undo stack. apply_edit(edit) is the single mutation
# entry point (the right-dock inspector funnels its row commits here through the
# adapter); each edit reads before/after, applies the document setter, and pushes
# one fine-grained undo op. Ctrl+Z / Ctrl+Y drive undo()/redo() while no text
# field has focus. M8 adds canvas drag/resize gestures that share this stack.

const MnuWidgetTreeScript = preload("res://modtools/mnu/mnu_widget_tree.gd")
const MnuCanvasScript = preload("res://modtools/mnu/mnu_canvas.gd")

signal widget_selected(id: int)

var _document   # MnuEditorDocument
var _resource_root: NovaResourceRoot
var _tree        # MnuWidgetTree
var _canvas      # MnuCanvas
var _selected_id := -1

# Fine-grained property undo ops: {target, id, prop, slot, before, after}.
# Property/rect edits never change the tree shape, so ids stay stable across
# undo/redo (structural ops + byte-snapshot undo arrive in M8).
var _undo_stack: Array[Dictionary] = []
var _redo_stack: Array[Dictionary] = []

# While a self-originated mutation (apply_edit / undo / redo) is in flight, the
# document's change refreshes the tree + preview but does NOT re-emit
# widget_selected, so the inspector that issued the edit is not torn down (a
# typed-in LineEdit keeps focus). undo/redo re-select explicitly afterwards.
var _suppress_select_emit := false


func _ready() -> void:
	# The editor inherits the shell's theme when mounted; no explicit theme load
	# (keeps it decoupled and avoids touching theme assets in headless tests).
	_build_ui()
	_refresh_all()


func _build_ui() -> void:
	if _tree != null:
		return
	var split := HSplitContainer.new()
	split.name = "RootSplit"
	split.set_anchors_preset(Control.PRESET_FULL_RECT)
	split.split_offset = 200
	add_child(split)

	_tree = MnuWidgetTreeScript.new()
	_tree.name = "WidgetTree"
	_tree.custom_minimum_size = Vector2(170, 0)
	_tree.widget_selected.connect(_on_tree_selected)
	split.add_child(_tree)

	var canvas_panel := PanelContainer.new()
	canvas_panel.theme_type_variation = &"FlatPanel"
	canvas_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	canvas_panel.size_flags_vertical = Control.SIZE_EXPAND_FILL
	split.add_child(canvas_panel)

	_canvas = MnuCanvasScript.new()
	_canvas.name = "Canvas"
	_canvas.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_canvas.size_flags_vertical = Control.SIZE_EXPAND_FILL
	canvas_panel.add_child(_canvas)


func set_document(value) -> void:
	if _document == value:
		return
	if _document != null:
		if _document.resource_loaded.is_connected(_on_resource_loaded):
			_document.resource_loaded.disconnect(_on_resource_loaded)
		if _document.resource_changed.is_connected(_on_resource_changed):
			_document.resource_changed.disconnect(_on_resource_changed)
	_document = value
	if _document != null:
		# resource_loaded == a full load (open/new): reset selection + rebuild.
		# resource_changed == an in-place edit (M7): rebuild, keep selection.
		# state_changed (also fired on load + save) is intentionally NOT used here,
		# so an open does not rebuild twice (it fires state_changed then
		# resource_loaded); this mirrors fonts/fnt_editor.gd.
		_document.resource_loaded.connect(_on_resource_loaded)
		_document.resource_changed.connect(_on_resource_changed)
	_refresh_all()


func set_resource_root(root: NovaResourceRoot) -> void:
	_resource_root = root
	_refresh_preview()


func get_selected_id() -> int:
	return _selected_id


func select_widget(id: int) -> void:
	if _tree != null:
		_tree.select_id(id)
	_apply_selection(id)


func get_unresolved_asset_count() -> int:
	return _canvas.get_unresolved_asset_count() if _canvas != null else 0


func get_visible_screen_name() -> String:
	return _canvas.get_visible_screen_name() if _canvas != null else ""


func _document_resource() -> NovaMnuDocument:
	return _document.resource if _document != null else null


func _refresh_all() -> void:
	if not is_node_ready():
		return
	var doc := _document_resource()
	if _tree != null:
		_tree.set_document(doc)
	_refresh_preview()
	# Default selection to the first screen so the inspector + preview are populated.
	if doc != null and doc.get_screen_count() > 0:
		select_widget(doc.get_screen_ids()[0])
	else:
		_apply_selection(-1)


func _refresh_preview() -> void:
	if _canvas == null:
		return
	var doc := _document_resource()
	_canvas.set_menu(doc, _resource_root, _resolve_text_resource(doc))


# Best-effort: resolve the document's first non-empty screen text resource through
# the shared resource root so the preview renders real strings. Silent on failure
# (the builder then shows string ids / stripped hotkeys).
func _resolve_text_resource(doc: NovaMnuDocument) -> RtxtStringFile:
	if doc == null or _resource_root == null or _resource_root.get_root_dir().is_empty():
		return null
	for screen_id in doc.get_screen_ids():
		var rsrc := doc.get_screen_text_rsrc(screen_id)
		if rsrc.is_empty():
			continue
		var path := _resource_root.resolve_file(rsrc)
		if path.is_empty():
			continue
		var rtxt := RtxtStringFile.new()
		if rtxt.load_from_path(path) == OK:
			return rtxt
		# A resolvable-but-unreadable table should not abort resolution; a later
		# screen may carry a loadable one.
		continue
	return null


func _on_tree_selected(id: int) -> void:
	_apply_selection(id)


# Drive the canvas (visible screen + highlight) from a selected id, then notify
# listeners. This is the single emit point for widget_selected, so user (tree)
# and programmatic (select_widget / default) selection both keep the right-dock
# inspector in sync through the adapter. emit is false for self-originated
# mutations, which refresh the canvas without rebuilding the inspector.
func _apply_selection(id: int, emit := true) -> void:
	_selected_id = id
	if _canvas != null:
		var doc := _document_resource()
		if doc == null or id < 0 or not doc.widget_exists(id):
			_canvas.set_highlight(Rect2())
		else:
			var screen_id := _screen_of(id)
			if screen_id >= 0:
				_canvas.show_screen_named(doc.get_screen_name(screen_id))
			if doc.is_screen(id):
				_canvas.set_highlight(Rect2())
			else:
				_canvas.set_highlight(doc.get_window_rect(id))
	if emit:
		widget_selected.emit(id)


# Walk parents until the screen container; -1 if none.
func _screen_of(id: int) -> int:
	var doc := _document_resource()
	if doc == null:
		return -1
	var current := id
	while current > 0 and doc.widget_exists(current):
		if doc.is_screen(current):
			return current
		current = doc.get_parent_id(current)
	return -1


# --- editing + undo -------------------------------------------------------------

# The single mutation entry point. edit = {target, id, prop, slot?, value}. Reads
# the current value, no-ops when unchanged, applies the setter (which refreshes
# the tree + preview silently), then records one undo op. Called by the adapter
# when the right-dock inspector commits a row.
func apply_edit(edit: Dictionary) -> void:
	var doc := _document_resource()
	if doc == null:
		return
	var target := String(edit.get("target", "widget"))
	var id := int(edit.get("id", -1))
	if id < 0 or not doc.widget_exists(id):
		return
	if not edit.has("value"):
		return
	var prop := String(edit.get("prop", ""))
	var slot := int(edit.get("slot", -1))
	var after = edit.get("value")
	var before = _read_prop(doc, target, id, prop, slot)
	if before == null or before == after:
		return
	_suppress_select_emit = true
	_write_prop(doc, target, id, prop, slot, after)
	_suppress_select_emit = false
	_undo_stack.append({"target": target, "id": id, "prop": prop, "slot": slot, "before": before, "after": after})
	_redo_stack.clear()


func can_undo() -> bool:
	return not _undo_stack.is_empty()


func can_redo() -> bool:
	return not _redo_stack.is_empty()


func undo() -> void:
	if _undo_stack.is_empty():
		return
	var op := _undo_stack.pop_back() as Dictionary
	if _apply_op_side(op, "before"):
		_redo_stack.append(op)
	else:
		# Target vanished (only possible once structural ops land); drop the op.
		pass


func redo() -> void:
	if _redo_stack.is_empty():
		return
	var op := _redo_stack.pop_back() as Dictionary
	if _apply_op_side(op, "after"):
		_undo_stack.append(op)


# Apply one side of an op (the "before" or "after" value), then re-select the
# affected node so the user sees what changed and the inspector shows the
# reverted value. Returns false when the target no longer exists.
func _apply_op_side(op: Dictionary, key: String) -> bool:
	var doc := _document_resource()
	if doc == null:
		return false
	var id := int(op["id"])
	if not doc.widget_exists(id):
		return false
	_suppress_select_emit = true
	_write_prop(doc, String(op["target"]), id, String(op["prop"]), int(op.get("slot", -1)), op[key])
	_suppress_select_emit = false
	select_widget(id)
	return true


func _read_prop(doc: NovaMnuDocument, target: String, id: int, prop: String, slot: int):
	if target == "screen":
		match prop:
			"name": return doc.get_screen_name(id)
			"music_var": return doc.get_screen_music_var(id)
			"text_rsrc": return doc.get_screen_text_rsrc(id)
			"cursor": return doc.get_screen_cursor_file(id)
		return null
	match prop:
		"name": return doc.get_widget_name(id)
		"rect": return doc.get_window_rect(id)
		"text": return doc.get_widget_text(id)
		"string_type": return doc.get_widget_string_type(id)
		"font": return doc.get_widget_font(id)
		"color": return doc.get_widget_color(id, slot)
		"texture": return doc.get_widget_texture(id, slot)
		"flags": return doc.get_widget_flags(id)
	return null


func _write_prop(doc: NovaMnuDocument, target: String, id: int, prop: String, slot: int, value) -> void:
	if target == "screen":
		match prop:
			"name": doc.set_screen_property(id, "name", value)
			"music_var": doc.set_screen_property(id, "music_var", int(value))
			"text_rsrc": doc.set_screen_property(id, "text_rsrc", value)
			"cursor": doc.set_screen_property(id, "cursor_file", value)
		return
	match prop:
		"name": doc.set_widget_name(id, value)
		"rect": doc.set_window_rect(id, value)
		"text": doc.set_widget_text(id, value)
		"string_type": doc.set_widget_string_type(id, value)
		"font": doc.set_widget_font(id, value)
		"color": doc.set_widget_color(id, slot, value)
		"texture": doc.set_widget_texture(id, slot, value)
		"flags": doc.set_widget_flags(id, int(value))


# Ctrl+Z / Ctrl+Shift+Z / Ctrl+Y, but never while a text field or spinner has
# focus (those own their native edit-undo), mirroring fnt_editor.gd.
func _shortcut_input(event: InputEvent) -> void:
	if not (event is InputEventKey):
		return
	var key := event as InputEventKey
	if not key.pressed or key.echo or not key.ctrl_pressed:
		return
	var focus := get_viewport().gui_get_focus_owner()
	if focus is LineEdit or focus is TextEdit or focus is SpinBox:
		return
	var handled := true
	match key.keycode:
		KEY_Z:
			if key.shift_pressed:
				redo()
			else:
				undo()
		KEY_Y:
			redo()
		_:
			handled = false
	if handled:
		get_viewport().set_input_as_handled()


func _on_resource_loaded(_resource) -> void:
	_selected_id = -1
	_refresh_all()


func _on_resource_changed() -> void:
	# In-place document mutation (M7 edits): rebuild the tree + preview, keeping the
	# current selection if its id still exists, else falling back to the first
	# screen. Document loads route through _on_resource_loaded instead (full reset).
	# While a self-originated edit is in flight, refresh silently (no
	# widget_selected) so the issuing inspector is not rebuilt mid-edit.
	if not is_node_ready():
		return
	var doc := _document_resource()
	if _tree != null:
		_tree.set_document(doc)
	_refresh_preview()
	var emit := not _suppress_select_emit
	if doc != null and doc.widget_exists(_selected_id):
		if _tree != null:
			_tree.select_id(_selected_id)
		_apply_selection(_selected_id, emit)
	elif doc != null and doc.get_screen_count() > 0:
		var first := doc.get_screen_ids()[0]
		if _tree != null:
			_tree.select_id(first)
		_apply_selection(first, emit)
	else:
		_apply_selection(-1, emit)
