class_name MnuEditor
extends Control

# The Menus workspace editor surface: a toolbar (add/delete widget + screen) above
# a widget tree alongside a live WYSIWYG canvas. Binds to an MnuEditorDocument,
# drives the canvas preview, and tracks selection by the document's stable widget
# id. Selection is re-emitted as widget_selected(id) so the workspace adapter can
# populate the right-dock property inspector.
#
# M7: this editor owns the undo stack. apply_edit(edit) is the single fine-grained
# property mutation; each edit reads before/after and pushes one undo op.
#
# M8: the canvas reports drag/resize/nudge gestures as rect_committed(id, rect),
# which route through apply_edit (reusing the rect-undo op). Structural ops
# (add/delete/reparent widget, add/delete screen) push a different kind of undo
# entry: a full-document snapshot pair (NovaMnuDocument.capture_state/apply_state),
# which restores byte-identical widget ids so the fine-grained property ops below
# them on the stack stay valid. Ctrl+Z / Ctrl+Y drive undo()/redo() while no text
# field has focus.

const MnuWidgetTreeScript = preload("res://modtools/mnu/mnu_widget_tree.gd")
const MnuCanvasScript = preload("res://modtools/mnu/mnu_canvas.gd")
const MnuAuthoringCommandsScript = preload("res://modtools/mnu/mnu_authoring_commands.gd")

const MENU_STYLESHEET_FILE := "menu_style.mns"

signal widget_selected(id: int)
# Multi-select: the full selection changed to >1 widget (or back). The single-widget
# channel stays widget_selected(id); the workspace routes this to a summary view.
signal selection_changed(ids: PackedInt32Array)
signal interactive_changed(on: bool)

var _document   # MnuEditorDocument
var _resource_root: NovaResourceRoot
# The resolved RTXT string table for the open menu (first screen's text_rsrc) and
# its absolute path, cached on each preview refresh. The inspector reuses these to
# show resolved text, drive the string picker, and jump to the Strings workspace.
var _text_resource: RtxtStringFile
var _text_resource_path: String = ""
var _stylesheet: MnsStyleSheet
# When the workspace owns the .mns document directly (the merged Menus
# workspace does), it pushes its in-memory MnsStyleSheet here so canvas refreshes
# read live edits without a disk round-trip. Null means fall back to the disk
# resolution from the resource root.
var _stylesheet_override: MnsStyleSheet
# Distinct %VAR% tokens in the open menu that the loaded stylesheet does not
# define (all of them, when none loads). Recomputed per preview refresh and
# cached: the shell polls status per frame. The original engine FAILS on an
# unknown variable at expansion, so these surface in the status bar.
var _unresolved_var_count := 0
var _tree        # MnuWidgetTree
var _canvas      # MnuCanvas
var _authoring_commands = MnuAuthoringCommandsScript.new()
var _selected_id := -1
# The full selection set (parallel to the active _selected_id). Size <= 1 mirrors the
# single-select behavior exactly; >1 is a multi-selection driven by canvas gestures.
var _selection: PackedInt32Array = PackedInt32Array()
var _interactive := false

# Toolbar controls.
var _type_picker: OptionButton
var _btn_add: Button
var _btn_delete: Button
var _btn_add_screen: Button
var _btn_delete_screen: Button
var _btn_interactive: Button
var _authoring_controls: Array[Control] = []

# Undo entries are tagged by "kind":
#   "prop"   {kind,target,id,prop,slot,before,after}  fine-grained, id-stable
#   "struct" {kind,op,before,after,sel_before,sel_after}  before/after are full
#            capture_state() snapshots; sel_* are the ids to select on undo/redo.
var _undo_stack: Array[Dictionary] = []
var _redo_stack: Array[Dictionary] = []

# While a self-originated mutation (apply_edit / structural op / undo / redo) is in
# flight, the document's change refreshes the tree + preview but does NOT re-emit
# widget_selected, so the inspector that issued the edit is not torn down (a
# typed-in LineEdit keeps focus). undo/redo + canvas commits re-select explicitly.
var _suppress_select_emit := false


func _ready() -> void:
	# The editor inherits the shell's theme when mounted; no explicit theme load
	# (keeps it decoupled and avoids touching theme assets in headless tests).
	_build_ui()
	_refresh_all()


func _build_ui() -> void:
	if _tree != null:
		return
	var root_box := VBoxContainer.new()
	root_box.name = "Root"
	root_box.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(root_box)

	_build_toolbar(root_box)

	var split := HSplitContainer.new()
	split.name = "RootSplit"
	split.split_offset = 200
	split.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	split.size_flags_vertical = Control.SIZE_EXPAND_FILL
	root_box.add_child(split)

	_tree = MnuWidgetTreeScript.new()
	_tree.name = "WidgetTree"
	_tree.custom_minimum_size = Vector2(170, 0)
	_tree.widget_selected.connect(_on_tree_selected)
	_tree.reparent_requested.connect(_on_tree_reparent_requested)
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
	_canvas.widget_picked.connect(_on_canvas_picked)
	_canvas.selection_cleared.connect(_on_canvas_cleared)
	_canvas.rect_committed.connect(_on_canvas_rect_committed)
	_canvas.selection_set.connect(_on_canvas_selection_set)
	_canvas.rect_committed_batch.connect(_on_canvas_rect_committed_batch)
	canvas_panel.add_child(_canvas)


func _build_toolbar(parent: Control) -> void:
	var bar := HBoxContainer.new()
	bar.name = "Toolbar"
	bar.add_theme_constant_override("separation", 4)
	parent.add_child(bar)

	_type_picker = OptionButton.new()
	_type_picker.name = "TypePicker"
	_type_picker.tooltip_text = "Widget type to add"
	_populate_type_picker()
	bar.add_child(_type_picker)
	_authoring_controls.append(_type_picker)

	_btn_add = _add_toolbar_button(bar, "Add", "Add a widget of the chosen type under the selection", _on_add_pressed)
	_btn_delete = _add_toolbar_button(bar, "Delete", "Delete the selected widget", _on_delete_pressed)
	bar.add_child(VSeparator.new())
	_btn_add_screen = _add_toolbar_button(bar, "Add Screen", "Add a new screen", _on_add_screen_pressed)
	_btn_delete_screen = _add_toolbar_button(bar, "Delete Screen", "Delete the visible screen", _on_delete_screen_pressed)
	_authoring_controls.append_array([
		_btn_add, _btn_delete, _btn_add_screen, _btn_delete_screen])
	var screen_menu := MenuButton.new()
	screen_menu.text = "Screen"
	screen_menu.tooltip_text = "Duplicate or reorder the visible screen"
	screen_menu.get_popup().add_item("Duplicate", 0)
	screen_menu.get_popup().add_item("Move earlier", 1)
	screen_menu.get_popup().add_item("Move later", 2)
	screen_menu.get_popup().id_pressed.connect(func(id: int) -> void:
		match id:
			0: duplicate_screen_action()
			1: move_screen_action(-1)
			2: move_screen_action(1)
	)
	bar.add_child(screen_menu)
	_authoring_controls.append(screen_menu)

	bar.add_child(VSeparator.new())
	var copy_btn := _add_toolbar_button(bar, "Copy",
		"Copy the selected widget subtree (Ctrl+C)", copy_selection_action)
	var paste_btn := _add_toolbar_button(bar, "Paste",
		"Paste beside the selection (Ctrl+V)", paste_selection_action)
	var duplicate_btn := _add_toolbar_button(bar, "Duplicate",
		"Duplicate the selected subtree (Ctrl+D)", duplicate_selection_action)
	_authoring_controls.append_array([copy_btn, paste_btn, duplicate_btn])
	var arrange := MenuButton.new()
	arrange.text = "Arrange"
	arrange.tooltip_text = "Align, distribute, or change draw order"
	for item in [
		["Align left", 0], ["Align right", 1], ["Align top", 2], ["Align bottom", 3],
		["Align centers horizontally", 4], ["Align centers vertically", 5],
		["Distribute horizontally", 6], ["Distribute vertically", 7],
		["Bring to front", 8], ["Bring forward", 9], ["Send backward", 10], ["Send to back", 11],
	]:
		arrange.get_popup().add_item(String(item[0]), int(item[1]))
	arrange.get_popup().id_pressed.connect(_on_arrange_menu)
	bar.add_child(arrange)
	_authoring_controls.append(arrange)

	bar.add_child(VSeparator.new())
	# View aids. "Bounds" mirrors the canvas default (on): faint outlines for every
	# widget so tiny / empty / overlapping ones are visible. The canvas is built after
	# the toolbar, so the handler defers to it at toggle time (no startup sync needed).
	var bounds_btn := Button.new()
	bounds_btn.text = "Bounds"
	bounds_btn.tooltip_text = "Show a faint outline around every widget"
	bounds_btn.toggle_mode = true
	bounds_btn.set_pressed_no_signal(true)
	bounds_btn.toggled.connect(func(on: bool) -> void:
		if _canvas != null:
			_canvas.set_show_all_bounds(on))
	bar.add_child(bounds_btn)
	_add_toolbar_button(bar, "Fit", "Reset zoom and pan to fit the board", _on_fit_pressed)

	bar.add_child(VSeparator.new())
	# Interactive "play" preview: click tabs/buttons in the preview to run their window
	# show/hide + screen navigation, like the running game. No edits are made.
	_btn_interactive = Button.new()
	_btn_interactive.text = "Interactive"
	_btn_interactive.tooltip_text = "Play the menu: click tabs/buttons to show/hide windows and change screens. No edits are made."
	_btn_interactive.toggle_mode = true
	_btn_interactive.toggled.connect(_on_interactive_toggled)
	bar.add_child(_btn_interactive)


func _add_toolbar_button(parent: Control, text: String, tip: String, handler: Callable) -> Button:
	var btn := Button.new()
	btn.text = text
	btn.tooltip_text = tip
	btn.pressed.connect(handler)
	parent.add_child(btn)
	return btn


func _on_interactive_toggled(on: bool) -> void:
	set_interactive(on)


func set_interactive(on: bool) -> void:
	if _interactive == on:
		return
	_interactive = on
	if _btn_interactive != null:
		_btn_interactive.set_pressed_no_signal(on)
	if _canvas != null:
		_canvas.set_interactive(on)
	# Authoring is unavailable while playing; restore the per-button state on exit.
	_set_authoring_enabled(not on)
	interactive_changed.emit(on)


func is_interactive() -> bool:
	return _interactive


# Enable/disable the structural authoring controls, used to lock them during the
# interactive preview. Re-derives the add/delete states via _refresh_toolbar_state.
func _set_authoring_enabled(enabled: bool) -> void:
	for c in _authoring_controls:
		if c != null:
			c.disabled = not enabled
	if _tree != null:
		_tree.set_authoring_enabled(enabled)
	if enabled:
		_refresh_toolbar_state()


func _populate_type_picker() -> void:
	# WidgetType enum names (minus the UNKNOWN sentinel) from the engine, so the
	# labels stay in lockstep with the document's type table.
	var probe := NovaMnuDocument.new()
	for t in range(NovaMnuDocument.TYPE_UNKNOWN):
		_type_picker.add_item(probe.get_widget_type_name(t), t)
	var idx := _type_picker.get_item_index(NovaMnuDocument.TYPE_BUTTON)
	if idx >= 0:
		_type_picker.select(idx)


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
		# resource_changed == an in-place edit: rebuild, keep selection.
		# state_changed (also fired on load + save) is intentionally NOT used here,
		# so an open does not rebuild twice (it fires state_changed then
		# resource_loaded); this mirrors fonts/fnt_editor.gd.
		_document.resource_loaded.connect(_on_resource_loaded)
		_document.resource_changed.connect(_on_resource_changed)
	# A new document invalidates the undo history.
	_undo_stack.clear()
	_redo_stack.clear()
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


func is_selection_off_board() -> bool:
	return _canvas.is_selection_off_board() if _canvas != null else false


func get_visible_screen_name() -> String:
	return _canvas.get_visible_screen_name() if _canvas != null else ""


## Rendered authoring-board extent, including live auto-sized width/height.
func get_rendered_widget_rect(id: int) -> Rect2:
	return _canvas.get_rendered_widget_rect(id) if _canvas != null else Rect2()


## Observable state of a widget in the live preview without exposing canvas maps.
func get_preview_widget_state(id: int) -> MnuPreviewWidgetState:
	return _canvas.get_preview_widget_state(id) \
		if _canvas != null else MnuPreviewWidgetState.new()


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
	_resolve_text_resource(doc)  # refresh the cached table + path
	_resolve_stylesheet_resource()
	_recount_unresolved_vars(doc)
	var menu_file := ""
	if _document != null:
		menu_file = String(_document.get("current_path")).get_file()
	_canvas.set_menu(doc, _resource_root, _text_resource, _stylesheet, menu_file)


# Best-effort: resolve the document's first non-empty screen text resource through
# the shared resource root so the preview renders real strings, caching both the
# loaded table and its absolute path (the inspector reuses them for resolved-text
# display, the string picker, and the "Edit in Strings" jump). Silent on failure
# (the builder then shows string ids / stripped hotkeys).
func _resolve_text_resource(doc: NovaMnuDocument) -> void:
	_text_resource = null
	_text_resource_path = ""
	if doc == null or _resource_root == null or _resource_root.get_root_dir().is_empty():
		return
	for screen_id in doc.get_screen_ids():
		var rsrc := doc.get_screen_text_rsrc(screen_id)
		if rsrc.is_empty():
			continue
		var path := _resource_root.resolve_file(rsrc)
		if path.is_empty():
			continue
		var rtxt := RtxtStringFile.new()
		if rtxt.load_from_path(path) == OK:
			_text_resource = rtxt
			_text_resource_path = path
			return
		# A resolvable-but-unreadable table should not abort resolution; a later
		# screen may carry a loadable one.
		continue


# With an override pushed by the workspace (the merged Menus workspace owns the
# .mns document directly), uses that live document so unsaved Styles edits hit
# the canvas immediately. Without one, re-reads the canonical stylesheet from
# the resource root on every preview refresh (also runs on every viewport mount
# via set_resource_root), so external edits appear on the next refresh with no
# extra wiring. Tests that drive MnuEditor without a workspace still work
# through the disk path.
func _resolve_stylesheet_resource() -> void:
	if _stylesheet_override != null:
		_stylesheet = _stylesheet_override
		return
	_stylesheet = null
	if _resource_root == null or _resource_root.get_root_dir().is_empty():
		return
	var bytes := _resource_root.read_file(MENU_STYLESHEET_FILE)
	if bytes.is_empty():
		return
	var sheet := MnsStyleSheet.new()
	if sheet.load_from_bytes(bytes) == OK:
		_stylesheet = sheet


# The workspace pushes its in-memory stylesheet here so canvas refreshes pick
# up unsaved Styles edits live. Pass null to fall back to disk resolution.
func set_stylesheet_resource(sheet: MnsStyleSheet) -> void:
	_stylesheet_override = sheet
	_refresh_preview()


# The loaded stylesheet (null when the root carries none); the inspector uses
# it to resolve %VAR% swatches and offer variable dropdowns.
func get_stylesheet() -> MnsStyleSheet:
	return _stylesheet


func get_unresolved_var_count() -> int:
	return _unresolved_var_count


# Walk every widget's color/texture/font fields for whole-field %VAR% tokens
# and count the DISTINCT names the stylesheet cannot resolve.
func _recount_unresolved_vars(doc: NovaMnuDocument) -> void:
	_unresolved_var_count = 0
	if doc == null:
		return
	var missing: Dictionary = {}
	var pending: Array[int] = []
	for screen_id in doc.get_screen_ids():
		pending.append(screen_id)
	while not pending.is_empty():
		var id: int = pending.pop_back()
		for child_id in doc.get_child_ids(id):
			pending.append(child_id)
		if doc.is_screen(id):
			continue # screens are containers; the styled fields live on widgets
		for slot in range(8):
			_note_unresolved_token(doc.get_widget_color(id, slot), missing)
		for slot in range(4):
			_note_unresolved_token(doc.get_widget_texture(id, slot), missing)
		_note_unresolved_token(doc.get_widget_font(id), missing)
	_unresolved_var_count = missing.size()


func _note_unresolved_token(raw: String, missing: Dictionary) -> void:
	var token := raw.strip_edges()
	if token.length() < 3 or not token.begins_with("%") or not token.ends_with("%"):
		return
	var name := token.substr(1, token.length() - 2)
	if _stylesheet == null or not _stylesheet.has_variable(name):
		missing[name.to_upper()] = true


func get_text_resource() -> RtxtStringFile:
	return _text_resource


func get_text_resource_path() -> String:
	return _text_resource_path


func _on_tree_selected(id: int) -> void:
	_apply_selection(id)


# Drive the canvas (visible screen + selection) from a selected id, then notify
# listeners. This is the single emit point for widget_selected, so user (tree /
# canvas) and programmatic (select_widget / default) selection both keep the
# right-dock inspector in sync through the adapter. emit is false for
# self-originated mutations, which refresh the canvas without rebuilding the
# inspector.
func _apply_selection(id: int, emit := true) -> void:
	_selected_id = id
	# Collapse the multi-selection to this single id (a screen / invalid id clears it),
	# mirroring the canvas so the two selection models stay in lockstep.
	var sel_doc := _document_resource()
	_selection = PackedInt32Array([id]) if (sel_doc != null and id >= 0 and sel_doc.widget_exists(id) and not sel_doc.is_screen(id)) else PackedInt32Array()
	if _canvas != null:
		var doc := _document_resource()
		if doc != null and id >= 0 and doc.widget_exists(id):
			var screen_id := _screen_of(id)
			if screen_id >= 0:
				_canvas.show_screen_named(doc.get_screen_name(screen_id))
		# The canvas computes the absolute rect itself (a screen / invalid id draws
		# no outline), so deeply nested widgets highlight correctly.
		_canvas.set_selected(id)
	_refresh_toolbar_state()
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


# The screen id matching the canvas's visible screen, else the first screen.
func _screen_id_for_visible() -> int:
	var doc := _document_resource()
	if doc == null:
		return -1
	if _canvas != null:
		var name: String = _canvas.get_visible_screen_name()
		for sid in doc.get_screen_ids():
			if doc.get_screen_name(sid) == name:
				return sid
	return doc.get_screen_ids()[0] if doc.get_screen_count() > 0 else -1


# A non-screen widget whose parent is a screen (a screen's root window). Root
# windows are not deletable / not draggable out of their screen.
func _is_root_window(id: int) -> bool:
	var doc := _document_resource()
	if doc == null or not doc.widget_exists(id) or doc.is_screen(id):
		return false
	var p := doc.get_parent_id(id)
	return p > 0 and doc.is_screen(p)


# --- Canvas gestures (M8a) ------------------------------------------------------

func _on_canvas_picked(id: int) -> void:
	# A canvas pick mirrors a tree click: sync the tree + right-dock inspector.
	if _tree != null:
		_tree.select_id(id)
	_apply_selection(id)


func _on_canvas_cleared() -> void:
	# Clicking empty board clears to the visible screen so the inspector stays
	# populated (matches the editor's default-to-first-screen behavior).
	var sid := _screen_id_for_visible()
	if sid >= 0:
		select_widget(sid)


func _on_canvas_rect_committed(id: int, local_rect: Rect2) -> void:
	if _interactive:
		return
	apply_edit({"target": "widget", "id": id, "prop": "rect", "value": local_rect})
	# The gesture (not the inspector) is the input source, so a rebuild is safe and
	# keeps the inspector's position/size spinboxes in sync with the new rect.
	select_widget(id)


# --- Multi-select (Phase 4) -----------------------------------------------------

# The canvas changed the multi-selection (shift/ctrl toggle or marquee). Route by
# size: a single id collapses to the normal single-select path (so the inspector
# shows that widget); empty selects the visible screen; >1 drives the summary view.
func _on_canvas_selection_set(ids: PackedInt32Array) -> void:
	if ids.size() == 1:
		_on_canvas_picked(ids[0])
		return
	if ids.is_empty():
		_on_canvas_cleared()
		return
	_selection = ids
	_selected_id = ids[ids.size() - 1]
	if _tree != null:
		_tree.select_id(_selected_id)  # the tree tracks only the active member
	_refresh_toolbar_state()
	selection_changed.emit(ids)


# Programmatic multi-select (batch-move re-select; reused by later phases). Pushes the
# set to the canvas + tree and announces it. Size <= 1 delegates to the single path.
func select_widgets(ids: PackedInt32Array) -> void:
	if ids.size() <= 1:
		select_widget(ids[0] if ids.size() == 1 else _screen_id_for_visible())
		return
	_selection = ids
	_selected_id = ids[ids.size() - 1]
	if _canvas != null:
		_canvas.set_selection(ids)
	if _tree != null:
		_tree.select_id(_selected_id)
	_refresh_toolbar_state()
	selection_changed.emit(ids)


func _on_canvas_rect_committed_batch(edits: Array) -> void:
	if _interactive:
		return
	apply_rect_batch(edits)


# Apply N rect edits as ONE undo step (rigid group-move; reused later by align /
# distribute / duplicate). edits = [{id, rect(local)}, ...]. Reuses the snapshot-undo
# path (capture_state + _push_struct), which no-ops when nothing actually moved.
func apply_rect_batch(edits: Array) -> void:
	_run_authoring_command(&"move_rects", {"edits": edits})


# One narrow seam into structural authoring. The command module owns validation,
# mutation order, clipboard state, and layout math; this facade owns preview
# locking, document-change suppression, selection, and snapshot history.
func _run_authoring_command(command: StringName,
		extra_context: Dictionary = {}) -> Dictionary:
	if _interactive:
		return {}
	var doc := _document_resource()
	if doc == null:
		return {}
	var context := extra_context.duplicate()
	context["selected_id"] = _selected_id
	context["selection"] = _selection.duplicate()
	context["visible_screen_id"] = _screen_id_for_visible()
	_suppress_select_emit = true
	var result: Dictionary = _authoring_commands.execute(doc, command, context)
	_suppress_select_emit = false
	if not result.has("before"):
		return result
	var op := String(result.get("op", command))
	var sel_before := int(result.get("sel_before", _selected_id))
	var sel_after := int(result.get("sel_after", sel_before))
	if result.has("selection"):
		if _push_struct(op, result["before"], sel_before, sel_after):
			select_widgets(result["selection"])
	else:
		_commit_struct(op, result["before"], sel_before, sel_after)
	return result


func _rendered_rects_for_selection() -> Dictionary:
	var ids := _selection.duplicate()
	if ids.size() <= 1 and _selected_id >= 0:
		ids = PackedInt32Array([_selected_id])
	var result := {}
	for id in ids:
		result[id] = get_rendered_widget_rect(id)
	return result


# --- Fine-grained property editing + undo (M7) ----------------------------------

# The single fine-grained mutation entry point. edit = {target, id, prop, slot?,
# value}. Reads the current value, no-ops when unchanged, applies the setter (which
# refreshes the tree + preview silently), then records one undo op.
func apply_edit(edit: Dictionary) -> void:
	if _interactive:
		return
	var doc := _document_resource()
	if doc == null:
		return
	# M10: nested list/table mutations carry an "op" and route through the
	# snapshot-undo path (a collection edit's natural undo unit is the whole list).
	if edit.has("op"):
		apply_list_edit(edit)
		return
	var target := String(edit.get("target", "widget"))
	if target == "widgets":
		var ids: PackedInt32Array = edit.get("ids", PackedInt32Array())
		if ids.is_empty() or not edit.has("value"):
			return
		var prop := String(edit.get("prop", ""))
		var slot := int(edit.get("slot", -1))
		var before_state := doc.capture_state()
		_suppress_select_emit = true
		for batch_id in ids:
			if not doc.widget_exists(batch_id) or doc.is_screen(batch_id):
				continue
			if prop == "patch" and edit["value"] is Dictionary:
				doc.apply_widget_patch(batch_id, edit["value"])
			else:
				_write_prop(doc, "widget", batch_id, prop, slot, edit["value"])
		_suppress_select_emit = false
		if _push_struct("multi_" + prop, before_state, _selected_id, _selected_id):
			select_widgets(ids)
		return
	var id := int(edit.get("id", -1))
	if id < 0 or not doc.widget_exists(id):
		return
	if not edit.has("value"):
		return
	var prop := String(edit.get("prop", ""))
	var slot := int(edit.get("slot", -1))
	var after = edit.get("value")
	if target == "widget" and prop == "patch" and after is Dictionary:
		var before_state := doc.capture_state()
		_suppress_select_emit = true
		var applied := bool(doc.apply_widget_patch(id, after))
		_suppress_select_emit = false
		if applied:
			_push_struct("widget_patch", before_state, _selected_id, id)
		return
	var before = _read_prop(doc, target, id, prop, slot)
	if before == null or before == after:
		return
	_suppress_select_emit = true
	_write_prop(doc, target, id, prop, slot, after)
	_suppress_select_emit = false
	_undo_stack.append({"kind": "prop", "target": target, "id": id, "prop": prop, "slot": slot, "before": before, "after": after})
	_redo_stack.clear()


# --- Structural editing (M8b) ---------------------------------------------------

func add_widget_action(type: int) -> int:
	return int(_run_authoring_command(&"add_widget", {"type": type}).get("value", -1))


# Add N widgets (each with optional initial properties) as ONE undo step — the
# programmatic batch counterpart of add_widget_action, for callers that know
# parent/rect up front (the MCP add_menu_widgets tool, future duplicate/paste).
# rows = [{parent: int, type: int, rect: Rect2, props: {prop -> value}}]; props
# use the slot-less _write_prop vocabulary (name/text/string_type/font/flags/
# group/...). Returns one {ok, id?} per row, in order. Mirrors apply_rect_batch's
# snapshot-undo shape; a row whose add is rejected reports ok=false and the
# batch continues (validation belongs to the caller).
func add_widgets_batch(rows: Array) -> Array:
	return _run_authoring_command(&"add_widgets", {"rows": rows}).get("value", [])


func delete_selection_action() -> void:
	_run_authoring_command(&"delete_widget")


func reparent_action(id: int, new_parent: int, index: int) -> void:
	_run_authoring_command(&"reparent_widget", {
		"id": id, "new_parent": new_parent, "index": index})


func copy_selection_action() -> bool:
	return bool(_run_authoring_command(&"copy_widget").get("value", false))


func cut_selection_action() -> void:
	if copy_selection_action():
		delete_selection_action()


func paste_selection_action() -> int:
	return int(_run_authoring_command(&"paste_widget").get("value", -1))


func duplicate_selection_action() -> int:
	return int(_run_authoring_command(&"duplicate_widget").get("value", -1))


func align_selection(mode: String) -> void:
	_run_authoring_command(&"align", {
		"mode": mode, "rendered_rects": _rendered_rects_for_selection()})


func distribute_selection(horizontal: bool) -> void:
	_run_authoring_command(&"distribute", {
		"horizontal": horizontal,
		"rendered_rects": _rendered_rects_for_selection(),
	})


func change_z_order(mode: String) -> void:
	_run_authoring_command(&"z_order", {"mode": mode})


func duplicate_screen_action() -> int:
	return int(_run_authoring_command(&"duplicate_screen").get("value", -1))


func move_screen_action(delta: int) -> void:
	_run_authoring_command(&"move_screen", {"delta": delta})


func _on_arrange_menu(id: int) -> void:
	match id:
		0: align_selection("left")
		1: align_selection("right")
		2: align_selection("top")
		3: align_selection("bottom")
		4: align_selection("hcenter")
		5: align_selection("vcenter")
		6: distribute_selection(true)
		7: distribute_selection(false)
		8: change_z_order("front")
		9: change_z_order("forward")
		10: change_z_order("backward")
		11: change_z_order("back")


# custom_name lets programmatic callers (the MCP edit_menu_screen tool) name the
# screen up front; they own uniqueness (duplicates alias show/delete-by-name).
# The toolbar passes nothing and keeps the unique default.
func add_screen_action(custom_name := "") -> int:
	return int(_run_authoring_command(&"add_screen", {"name": custom_name}).get(
		"value", -1))


func delete_screen_action() -> void:
	_run_authoring_command(&"delete_screen")


# Record one structural undo entry (the mutation has already run) and select the
# resulting node. before is the pre-mutation snapshot; after is captured now.
func _commit_struct(op_name: String, before_state: Dictionary, sel_before: int, sel_after: int) -> void:
	if _push_struct(op_name, before_state, sel_before, sel_after):
		select_widget(sel_after)


# Record a snapshot undo entry without re-selecting (so the inspector is not torn
# down). Returns false when the mutation was a no-op (identical bytes): no entry
# is recorded. Used by field edits, which keep the same row set + selection.
func _push_struct(op_name: String, before_state: Dictionary, sel_before: int, sel_after: int) -> bool:
	var doc := _document_resource()
	if doc == null:
		return false
	var after_state := doc.capture_state()
	if before_state.get("mnu", PackedByteArray()) == after_state.get("mnu", PackedByteArray()):
		return false
	_undo_stack.append({"kind": "struct", "op": op_name, "before": before_state,
		"after": after_state, "sel_before": sel_before, "sel_after": sel_after})
	_redo_stack.clear()
	return true


# --- M10: nested list/table edits (routed from apply_edit when "op" is set) -----
#
# Field edits (item_field / header_field / body_field) keep the row set + the
# selection, so they apply silently and do NOT rebuild the inspector (focus
# survives, like scalar rows). Structural edits (add / remove / move) change the
# row set, so they re-select the widget, which rebuilds the inspector to show it.
# Both record a single snapshot undo entry; both no-op when nothing changed.
func apply_list_edit(edit: Dictionary) -> void:
	if _interactive:
		return
	var doc := _document_resource()
	if doc == null:
		return
	var id := int(edit.get("id", -1))
	if id < 0 or not doc.widget_exists(id):
		return
	var op := String(edit.get("op", ""))

	if op == "item_field" or op == "header_field" or op == "body_field" or op == "subst_field":
		var index := int(edit.get("index", -1))
		var key := String(edit.get("key", ""))
		var row := _read_list_row(doc, op, id, index)
		if row.is_empty() or not row.has(key) or row[key] == edit.get("value"):
			return # unknown row/key, or unchanged (commit fires on blur + Enter)
		row[key] = edit.get("value")
		var before := doc.capture_state()
		var sel_before := _selected_id
		_suppress_select_emit = true
		_write_list_row(doc, op, id, index, row)
		_suppress_select_emit = false
		_push_struct(op, before, sel_before, id)
		return

	# Structural row-set change.
	var before := doc.capture_state()
	var sel_before := _selected_id
	_suppress_select_emit = true
	match op:
		"item_add": doc.add_item(id, edit.get("row", {}))
		"item_remove": doc.remove_item(id, int(edit.get("index", -1)))
		"item_move": doc.move_item(id, int(edit.get("from", -1)), int(edit.get("to", -1)))
		"header_add": doc.add_table_header(id, edit.get("row", {}))
		"header_remove": doc.remove_table_header(id, int(edit.get("index", -1)))
		"body_add": doc.add_table_body(id, edit.get("row", {}))
		"body_remove": doc.remove_table_body(id, int(edit.get("index", -1)))
		"subst_add": doc.add_table_subst(id, edit.get("row", {}))
		"subst_remove": doc.remove_table_subst(id, int(edit.get("index", -1)))
		_:
			_suppress_select_emit = false
			return
	_suppress_select_emit = false
	_commit_struct(op, before, sel_before, id)


func _read_list_row(doc: NovaMnuDocument, op: String, id: int, index: int) -> Dictionary:
	match op:
		"item_field":
			return doc.get_item(id, index)
		"header_field":
			var headers := doc.get_table_headers(id)
			return headers[index] if index >= 0 and index < headers.size() else {}
		"body_field":
			var bodies := doc.get_table_bodies(id)
			return bodies[index] if index >= 0 and index < bodies.size() else {}
		"subst_field":
			var substs := doc.get_table_substs(id)
			return substs[index] if index >= 0 and index < substs.size() else {}
	return {}


func _write_list_row(doc: NovaMnuDocument, op: String, id: int, index: int, row: Dictionary) -> void:
	match op:
		"item_field": doc.set_item(id, index, row)
		"header_field": doc.set_table_header(id, index, row)
		"body_field": doc.set_table_body(id, index, row)
		"subst_field": doc.set_table_subst(id, index, row)


func _resolve_existing_selection(id: int) -> int:
	var doc := _document_resource()
	if doc == null:
		return -1
	if id >= 0 and doc.widget_exists(id):
		return id
	return doc.get_screen_ids()[0] if doc.get_screen_count() > 0 else -1


# --- Undo / redo ----------------------------------------------------------------

## Detach the current undo/redo history so the workspace can stash it per tab
## across document rebinds (set_document clears the live stacks). The op dicts
## are pure data, so they survive being parked.
func take_history() -> Dictionary:
	var history := {"undo": _undo_stack.duplicate(), "redo": _redo_stack.duplicate()}
	_undo_stack.clear()
	_redo_stack.clear()
	return history


## Restore a take_history() stash for the (just-bound) document; an empty
## dictionary leaves the cleared stacks as-is.
func restore_history(history: Dictionary) -> void:
	_undo_stack.clear()
	_redo_stack.clear()
	if history.has("undo"):
		_undo_stack.assign(history["undo"])
	if history.has("redo"):
		_redo_stack.assign(history["redo"])


func can_undo() -> bool:
	return not _interactive and not _undo_stack.is_empty()


func can_redo() -> bool:
	return not _interactive and not _redo_stack.is_empty()


func undo() -> void:
	if _interactive:
		return
	if _undo_stack.is_empty():
		return
	var op := _undo_stack.pop_back() as Dictionary
	if _apply_undo_side(op, true):
		_redo_stack.append(op)


func redo() -> void:
	if _interactive:
		return
	if _redo_stack.is_empty():
		return
	var op := _redo_stack.pop_back() as Dictionary
	if _apply_undo_side(op, false):
		_undo_stack.append(op)


# Apply the "before" (is_undo) or "after" side of an op. Returns false when the op
# could not be applied (the entry is then dropped rather than re-queued).
func _apply_undo_side(op: Dictionary, is_undo: bool) -> bool:
	if String(op.get("kind", "prop")) == "struct":
		return _apply_struct_side(op, is_undo)
	return _apply_op_side(op, "before" if is_undo else "after")


# Fine-grained prop op: write the other side, then re-select the affected node so
# the user sees what changed and the inspector shows the reverted value.
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


# Structural op: restore the full-document snapshot (byte-identical ids), then
# re-select the side's stored id (falling back if it no longer exists).
func _apply_struct_side(op: Dictionary, is_undo: bool) -> bool:
	var doc := _document_resource()
	if doc == null:
		return false
	var state: Dictionary = op["before"] if is_undo else op["after"]
	var target_sel := int(op["sel_before"]) if is_undo else int(op["sel_after"])
	_suppress_select_emit = true
	doc.apply_state(state)
	_suppress_select_emit = false
	select_widget(_resolve_existing_selection(target_sel))
	return true


func _read_prop(doc: NovaMnuDocument, target: String, id: int, prop: String, slot: int):
	if target == "screen":
		match prop:
			"name": return doc.get_screen_name(id)
			"has_music_var": return doc.get_screen_has_music_var(id)
			"music_var": return doc.get_screen_music_var(id)
			"text_rsrc": return doc.get_screen_text_rsrc(id)
			"cursor": return doc.get_screen_cursor_file(id)
			"cursor_flags": return doc.get_screen_cursor_flags(id)
		return null
	match prop:
		"name": return doc.get_widget_name(id)
		"rect": return doc.get_window_rect(id)
		"text": return doc.get_widget_text(id)
		"string_type": return doc.get_widget_string_type(id)
		"font": return doc.get_widget_font(id)
		"datasource": return doc.get_widget_datasource(id)
		"orientation": return doc.get_widget_orientation(id)
		"group": return doc.get_widget_group(id)
		"color": return doc.get_widget_color(id, slot)
		"texture": return doc.get_widget_texture(id, slot)
		"flags": return doc.get_widget_flags(id)
		"sounds": return doc.get_widget_sounds(id)
		"actions": return doc.get_widget_actions(id)
		"appearances": return doc.get_widget_appearances(id)
		"frame": return doc.get_window_frame(id)
		"table_count": return doc.get_table_column_count(id)
		"table_spacing": return doc.get_table_column_spacing(id)
	return null


func _write_prop(doc: NovaMnuDocument, target: String, id: int, prop: String, slot: int, value) -> void:
	if target == "screen":
		match prop:
			"name": doc.set_screen_property(id, "name", value)
			"has_music_var": doc.set_screen_property(id, "has_music_var", bool(value))
			"music_var": doc.set_screen_property(id, "music_var", int(value))
			"text_rsrc": doc.set_screen_property(id, "text_rsrc", value)
			"cursor": doc.set_screen_property(id, "cursor_file", value)
			"cursor_flags": doc.set_screen_property(id, "cursor_flags", value)
		return
	match prop:
		"name": doc.set_widget_name(id, value)
		"rect": doc.set_window_rect(id, value)
		"text": doc.set_widget_text(id, value)
		"string_type": doc.set_widget_string_type(id, value)
		"font": doc.set_widget_font(id, value)
		"datasource": doc.set_widget_datasource(id, value)
		"orientation": doc.set_widget_orientation(id, value)
		"group": doc.set_widget_group(id, int(value))
		"color": doc.set_widget_color(id, slot, value)
		"texture": doc.set_widget_texture(id, slot, value)
		"flags": doc.set_widget_flags(id, int(value))
		"sounds": doc.set_widget_sounds(id, value)
		"actions": doc.set_widget_actions(id, value)
		"appearances": doc.set_widget_appearances(id, value)
		"frame": doc.set_window_frame(id, value)
		"table_count": doc.set_table_column_count(id, int(value))
		"table_spacing": doc.set_table_column_spacing(id, int(value))


# Undo/redo live in the shell. Clipboard shortcuts are local because their
# payload is the typed MNU subtree owned by this editor instance.
func _unhandled_key_input(event: InputEvent) -> void:
	if _interactive:
		return
	if not (event is InputEventKey):
		return
	var key := event as InputEventKey
	if not key.pressed or key.echo or not (key.ctrl_pressed or key.meta_pressed):
		return
	var focus := get_viewport().gui_get_focus_owner()
	if focus is LineEdit or focus is TextEdit or focus is SpinBox:
		return
	match key.keycode:
		KEY_C: copy_selection_action()
		KEY_X: cut_selection_action()
		KEY_V: paste_selection_action()
		KEY_D: duplicate_selection_action()
		_: return
	get_viewport().set_input_as_handled()


# --- Toolbar handlers + state ---------------------------------------------------

func _selected_type() -> int:
	return _type_picker.get_selected_id() if _type_picker != null else NovaMnuDocument.TYPE_BUTTON


func _on_add_pressed() -> void:
	add_widget_action(_selected_type())


func _on_delete_pressed() -> void:
	delete_selection_action()


func _on_add_screen_pressed() -> void:
	add_screen_action()


func _on_delete_screen_pressed() -> void:
	delete_screen_action()


func _on_fit_pressed() -> void:
	if _canvas != null:
		_canvas.reset_view()


func _on_tree_reparent_requested(id: int, new_parent: int, index: int) -> void:
	if _interactive:
		return
	reparent_action(id, new_parent, index)


func _refresh_toolbar_state() -> void:
	if _btn_delete == null:
		return
	if _interactive:
		for control in _authoring_controls:
			if control != null:
				control.disabled = true
		return
	var doc := _document_resource()
	var has_doc := doc != null
	_btn_add.disabled = not has_doc
	_btn_add_screen.disabled = not has_doc
	_btn_delete.disabled = not (has_doc and _selected_id >= 0 and doc.widget_exists(_selected_id) \
		and not doc.is_screen(_selected_id) and not _is_root_window(_selected_id))
	_btn_delete_screen.disabled = not (has_doc and doc.get_screen_count() > 1 and _screen_id_for_visible() >= 0)


# --- Document signals -----------------------------------------------------------

func _on_resource_loaded(_resource) -> void:
	_selected_id = -1
	_undo_stack.clear()
	_redo_stack.clear()
	_refresh_all()
	# A fresh document (open / new) re-centers the view; in-place edits keep the
	# user's zoom + pan (those route through _on_resource_changed, not here).
	if _canvas != null:
		_canvas.reset_view()


func _on_resource_changed() -> void:
	# In-place document mutation: rebuild the tree + preview, keeping the current
	# selection if its id still exists, else falling back to the first screen.
	# Document loads route through _on_resource_loaded instead (full reset). While a
	# self-originated edit is in flight, refresh silently (no widget_selected) so
	# the issuing inspector is not rebuilt mid-edit.
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
