class_name MnsEditor
extends Control

# The Styles tab body inside the Menus workspace: a toolbar over a
# Variables/Source view toggle. Binds to an MnsEditorDocument and owns the undo
# stack. The Menus canvas IS the live preview for stylesheet edits (the
# workspace pushes the in-memory MnsStyleSheet into the MnuEditor on every
# document_changed), so this editor no longer carries its own preview pane.
#
# Undo strategy: every mutation funnels through apply_edit(edit), which records
# a whole-file source-text snapshot pair (the document is a few KB and
# get_source_text() is byte-faithful, so undo restores comments and alignment
# exactly); undo/redo replay via set_source_text. One mechanism covers typed
# rows, structural ops, and Source-view applies alike.

const MnsVariableTableScript = preload("res://modtools/mnu/mns_variable_table.gd")
const MnsSourceViewScript = preload("res://modtools/mnu/mns_source_view.gd")

signal variable_selected(name: String)

var _document   # MnsEditorDocument

var _table: MnsVariableTable
var _table_scroll: ScrollContainer
var _source_view: MnsSourceView
var _btn_variables: Button
var _btn_source: Button
var _btn_add: Button
var _btn_remove: Button

# {before, after, sel_before, sel_after} source-text snapshot pairs.
var _undo_stack: Array[Dictionary] = []
var _redo_stack: Array[Dictionary] = []

# While a self-originated mutation is in flight the document's changed signal
# must not trigger a rebuild (the row that issued the edit keeps focus);
# apply_edit refreshes explicitly afterwards.
var _suppress_refresh := false

# Cached for the shell's per-frame status poll.
var _variable_count := 0
var _diagnostic_count := 0
var _authoring_enabled := true


func _ready() -> void:
	_build_ui()
	_refresh_all()


func _build_ui() -> void:
	if _table != null:
		return
	var root_box := VBoxContainer.new()
	root_box.name = "Root"
	root_box.set_anchors_preset(Control.PRESET_FULL_RECT)
	add_child(root_box)

	_build_toolbar(root_box)

	# Variables and Source share the body; exactly one is visible. There is no
	# separate preview pane: the Menus canvas IS the live stylesheet preview.
	var view_stack := MarginContainer.new()
	view_stack.name = "ViewStack"
	view_stack.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	view_stack.size_flags_vertical = Control.SIZE_EXPAND_FILL
	root_box.add_child(view_stack)

	_table_scroll = ScrollContainer.new()
	_table_scroll.name = "TableScroll"
	_table_scroll.horizontal_scroll_mode = ScrollContainer.SCROLL_MODE_DISABLED
	view_stack.add_child(_table_scroll)
	_table = MnsVariableTableScript.new()
	_table.name = "VariableTable"
	_table.row_selected.connect(_on_row_selected)
	_table.edit_requested.connect(apply_edit)
	_table_scroll.add_child(_table)

	_source_view = MnsSourceViewScript.new()
	_source_view.name = "SourceView"
	_source_view.visible = false
	_source_view.apply_callback = func(text: String) -> void:
		apply_edit({"op": "source", "text": text})
	view_stack.add_child(_source_view)


func _build_toolbar(parent: Control) -> void:
	var bar := HBoxContainer.new()
	bar.name = "Toolbar"
	bar.add_theme_constant_override("separation", 6)
	parent.add_child(bar)

	_btn_add = Button.new()
	_btn_add.text = "Add variable"
	_btn_add.pressed.connect(_on_add_pressed)
	bar.add_child(_btn_add)

	_btn_remove = Button.new()
	_btn_remove.text = "Remove"
	_btn_remove.pressed.connect(_on_remove_pressed)
	bar.add_child(_btn_remove)

	var spacer := Control.new()
	spacer.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	bar.add_child(spacer)

	var view_group := ButtonGroup.new()
	_btn_variables = Button.new()
	_btn_variables.text = "Variables"
	_btn_variables.toggle_mode = true
	_btn_variables.button_group = view_group
	_btn_variables.button_pressed = true
	_btn_variables.toggled.connect(func(on: bool) -> void:
		if on:
			_show_source_view(false))
	bar.add_child(_btn_variables)
	_btn_source = Button.new()
	_btn_source.text = "Source"
	_btn_source.toggle_mode = true
	_btn_source.button_group = view_group
	_btn_source.toggled.connect(func(on: bool) -> void:
		if on:
			_show_source_view(true))
	bar.add_child(_btn_source)


func set_authoring_enabled(enabled: bool) -> void:
	_authoring_enabled = enabled
	if _btn_add != null:
		_btn_add.disabled = not enabled
	if _btn_remove != null:
		_btn_remove.disabled = not enabled
	for root in [_table, _source_view]:
		if root == null:
			continue
		for node in root.find_children("*", "Control", true, false):
			if node is BaseButton:
				(node as BaseButton).disabled = not enabled
			elif node is LineEdit:
				(node as LineEdit).editable = enabled
			elif node is TextEdit:
				(node as TextEdit).editable = enabled


func _show_source_view(on: bool) -> void:
	if _source_view == null or _table_scroll == null:
		return
	# Leaving the source view commits its buffer (same contract as focus-exit).
	if not on:
		_source_view.apply_pending()
	_table_scroll.visible = not on
	_source_view.visible = on
	if on:
		_source_view.refresh_from_resource(true)


# --- Document binding -------------------------------------------------------

func set_document(document) -> void:
	if _document == document:
		return
	if _document != null:
		if _document.resource_loaded.is_connected(_on_resource_loaded):
			_document.resource_loaded.disconnect(_on_resource_loaded)
		if _document.resource_changed.is_connected(_on_resource_changed):
			_document.resource_changed.disconnect(_on_resource_changed)
	_document = document
	if _document != null:
		_document.resource_loaded.connect(_on_resource_loaded)
		_document.resource_changed.connect(_on_resource_changed)
	_undo_stack.clear()
	_redo_stack.clear()
	_refresh_all()


func _sheet() -> MnsStyleSheet:
	return _document.resource if _document != null else null


func _on_resource_loaded(_resource) -> void:
	_undo_stack.clear()
	_redo_stack.clear()
	_refresh_all()


func _on_resource_changed() -> void:
	if _suppress_refresh:
		return
	# An unsuppressed change is an external mutation (rare; everything funnels
	# through apply_edit): rebuild keeping the selection.
	_refresh_views(_table.get_selected_name() if _table != null else "")


func _refresh_all() -> void:
	# Build eagerly so headless callers (tests, the workspace mount path that
	# parks this editor invisible before the right dock is built) get a working
	# table / source view immediately without waiting for the next frame.
	_build_ui()
	var sheet := _sheet()
	if _table != null:
		_table.set_stylesheet(sheet)
	if _source_view != null:
		_source_view.set_stylesheet(sheet)
	_recount(sheet)
	# Default-select the first variable so the inspector is populated.
	var entries: Array = sheet.get_entries() if sheet != null else []
	if entries.size() > 0:
		select_variable(String((entries[0] as Dictionary).get("name", "")))
	else:
		variable_selected.emit("")
	if not _authoring_enabled:
		set_authoring_enabled.call_deferred(false)


func _refresh_views(keep_selection: String) -> void:
	var sheet := _sheet()
	if _table != null:
		_table.refresh()
		if not keep_selection.is_empty():
			_table.select_name(keep_selection, false)
	if _source_view != null:
		_source_view.refresh_from_resource(false)
	_recount(sheet)
	if not _authoring_enabled:
		set_authoring_enabled.call_deferred(false)


func _recount(sheet: MnsStyleSheet) -> void:
	_variable_count = sheet.get_entry_count() if sheet != null else 0
	_diagnostic_count = sheet.get_diagnostics().size() if sheet != null else 0


func get_variable_count() -> int:
	return _variable_count


func get_diagnostic_count() -> int:
	return _diagnostic_count


# --- Selection ---------------------------------------------------------------

func _on_row_selected(name: String) -> void:
	variable_selected.emit(name)


func get_selected_variable() -> String:
	return _table.get_selected_name() if _table != null else ""


# Returns false when no variable with that name exists (case-insensitive).
func select_variable(name: String) -> bool:
	var sheet := _sheet()
	if sheet == null or _table == null:
		return false
	# Resolve to the authored-case name the table keys rows by.
	var target := ""
	for entry_value in sheet.get_entries():
		var entry := entry_value as Dictionary
		if String(entry.get("name", "")).nocasecmp_to(name) == 0:
			target = String(entry.get("name", ""))
	if target.is_empty():
		return false
	_table.select_name(target)
	_table.reveal_selected()
	return true


# --- Edits + undo -------------------------------------------------------------

# The single mutation point. Ops:
#   {op:"set_value", name, value}        {op:"rename", name, new_name}
#   {op:"set_comment", name, comment}    {op:"add", name, value, after_name?}
#   {op:"remove", name}                  {op:"move", name, to_entry_index}
#   {op:"source", text}
func apply_edit(edit: Dictionary) -> void:
	if not _authoring_enabled:
		return
	var sheet := _sheet()
	if sheet == null:
		return
	var op := String(edit.get("op", ""))
	var name := String(edit.get("name", ""))
	var before := String(sheet.get_source_text())
	var sel_before := get_selected_variable()
	var sel_after := sel_before

	_suppress_refresh = true
	match op:
		"set_value":
			sheet.set_variable(name, String(edit.get("value", "")))
		"rename":
			if sheet.rename_variable(name, String(edit.get("new_name", ""))):
				sel_after = String(edit.get("new_name", ""))
		"set_comment":
			sheet.set_inline_comment(name, String(edit.get("comment", "")))
		"add":
			if sheet.add_variable(name, String(edit.get("value", "")), String(edit.get("after_name", ""))):
				sel_after = name
		"remove":
			if sheet.remove_variable(name):
				sel_after = ""
		"move":
			if sheet.move_variable(name, int(edit.get("to_entry_index", -1))):
				sel_after = name
		"source":
			sheet.set_source_text(String(edit.get("text", "")))
			if not sel_before.is_empty() and not sheet.has_variable(sel_before):
				sel_after = ""
		_:
			push_warning("MnsEditor.apply_edit: unknown op '%s'" % op)
	_suppress_refresh = false

	var after := String(sheet.get_source_text())
	if after == before:
		return
	_undo_stack.append({
		"before": before, "after": after,
		"sel_before": sel_before, "sel_after": sel_after,
	})
	_redo_stack.clear()

	# A value edit updates its row in place (the issuing LineEdit keeps focus);
	# structural ops rebuild and re-select, which also repopulates the inspector.
	if op == "set_value":
		if _table != null:
			_table.update_row_value(name, String(sheet.get_variable(name)))
		if _source_view != null:
			_source_view.refresh_from_resource(false)
		_recount(sheet)
	else:
		_refresh_views(sel_after)
		if not sel_after.is_empty():
			select_variable(sel_after)
		else:
			variable_selected.emit("")


func can_undo() -> bool:
	return _authoring_enabled and not _undo_stack.is_empty()


func can_redo() -> bool:
	return _authoring_enabled and not _redo_stack.is_empty()


func undo() -> void:
	if not _authoring_enabled:
		return
	if _undo_stack.is_empty():
		return
	var op: Dictionary = _undo_stack.pop_back()
	_redo_stack.append(op)
	_apply_snapshot(String(op["before"]), String(op["sel_before"]))


func redo() -> void:
	if not _authoring_enabled:
		return
	if _redo_stack.is_empty():
		return
	var op: Dictionary = _redo_stack.pop_back()
	_undo_stack.append(op)
	_apply_snapshot(String(op["after"]), String(op["sel_after"]))


func _apply_snapshot(text: String, selection: String) -> void:
	var sheet := _sheet()
	if sheet == null:
		return
	_suppress_refresh = true
	sheet.set_source_text(text)
	_suppress_refresh = false
	_refresh_views(selection)
	if not selection.is_empty():
		select_variable(selection)
	else:
		variable_selected.emit("")


# Deferred source-view buffer, committed before the shell saves/exports.
# The .mns parse is permissive, so applying always succeeds.
func flush_pending_edits() -> Error:
	if _source_view != null and _source_view.has_pending_edits():
		return _source_view.apply_pending()
	return OK


# --- Toolbar -------------------------------------------------------------------

func _on_add_pressed() -> void:
	var sheet := _sheet()
	if sheet == null:
		return
	var base := "NEW_VARIABLE"
	var name := base
	var n := 2
	while sheet.has_variable(name):
		name = "%s_%d" % [base, n]
		n += 1
	apply_edit({"op": "add", "name": name, "value": "FFFFFFFF", "after_name": get_selected_variable()})


func _on_remove_pressed() -> void:
	var selected := get_selected_variable()
	if not selected.is_empty():
		apply_edit({"op": "remove", "name": selected})


# Undo/redo keyboard shortcuts live in the shell's _shortcut_input (B6).
