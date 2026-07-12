class_name ShellDocumentTabs
extends RefCounted

## The document-tab strip above the viewport, shown only while the active
## workspace opts into the document-tab tier (supports_document_tabs).
## Rebuilds are signal-driven: the shell forwards documents_changed for the
## active workspace plus its workspace-switch surface refresh. Dirty closes
## route through the shell's shared unsaved-changes prompt + save_then.

var _strip: PanelContainer
var _row: HBoxContainer
# func() -> EditorWorkspace: the active workspace.
var _active_workspace: Callable
# func() -> StyleBoxFlat: the shell's active-row stylebox (shared with the rail).
var _active_stylebox: Callable
# func(on_save: Callable, on_discard: Callable): the shared unsaved prompt.
var _prompt_unsaved_for: Callable
# func(workspace, on_done: Callable, failure_message: String): shell save_then.
var _save_then: Callable
# func(): shell sync_from_editor_state (post close/activate refresh).
var _sync: Callable


func setup(
	strip: PanelContainer,
	row: HBoxContainer,
	active_workspace: Callable,
	active_stylebox: Callable,
	prompt_unsaved_for: Callable,
	save_then: Callable,
	sync: Callable
) -> void:
	_strip = strip
	_row = row
	_active_workspace = active_workspace
	_active_stylebox = active_stylebox
	_prompt_unsaved_for = prompt_unsaved_for
	_save_then = save_then
	_sync = sync


func rebuild() -> void:
	if _row == null:
		return
	# queue_free, not free: a rebuild is usually triggered FROM a tab/close
	# button's own pressed emission, and freeing the emitting button is an error
	# (locked object). Detach immediately so the new row builds clean.
	for child in _row.get_children():
		_row.remove_child(child)
		child.queue_free()
	var workspace: EditorWorkspace = _active_workspace.call()
	if workspace == null or not workspace.supports_document_tabs():
		_strip.visible = false
		return
	var tabs: Array[DocumentTabRow] = workspace.get_document_tabs()
	var active := workspace.get_active_document_index()
	var active_style: StyleBoxFlat = _active_stylebox.call()
	for i in tabs.size():
		var tab: DocumentTabRow = tabs[i]
		var label := tab.label
		var btn := Button.new()
		btn.name = "DocumentTab%d" % i
		btn.toggle_mode = true
		btn.text = label + ("*" if tab.dirty else "")
		btn.tooltip_text = tab.tooltip
		btn.clip_text = true
		btn.custom_minimum_size = Vector2(96, 28)
		btn.focus_mode = Control.FOCUS_NONE
		if i == active:
			btn.add_theme_stylebox_override("normal", active_style)
			btn.set_pressed_no_signal(true)
		btn.pressed.connect(_on_tab_pressed.bind(i))
		_row.add_child(btn)
		var close := Button.new()
		close.name = "DocumentTabClose%d" % i
		close.text = PopoverPanel.CLOSE_GLYPH
		close.tooltip_text = "Close %s" % label
		close.custom_minimum_size = Vector2(24, 28)
		close.focus_mode = Control.FOCUS_NONE
		close.pressed.connect(_on_tab_close_pressed.bind(i))
		_row.add_child(close)
	_strip.visible = not tabs.is_empty()


func _on_tab_pressed(index: int) -> void:
	var workspace: EditorWorkspace = _active_workspace.call()
	if workspace == null:
		return
	if index == workspace.get_active_document_index():
		# Re-pressing the active tab must not untoggle it visually.
		rebuild()
		return
	workspace.activate_document(index)
	_sync.call()


func _on_tab_close_pressed(index: int) -> void:
	var workspace: EditorWorkspace = _active_workspace.call()
	if workspace == null:
		return
	var tabs: Array[DocumentTabRow] = workspace.get_document_tabs()
	if index < 0 or index >= tabs.size():
		return
	if not tabs[index].dirty:
		workspace.close_document(index)
		_sync.call()
		return
	# Activate first so the user sees what is at stake — and so the close target
	# stays well-defined across the async prompt/save-as: every follow-up acts on
	# the ACTIVE document, immune to index shifts.
	workspace.activate_document(index)
	rebuild()
	_prompt_unsaved_for.call(
		func() -> void: _save_then_close_active(workspace),
		func() -> void: _close_active(workspace))


func _close_active(workspace: EditorWorkspace) -> void:
	var idx := workspace.get_active_document_index()
	if idx >= 0:
		workspace.close_document(idx)
	_sync.call()


func _save_then_close_active(workspace: EditorWorkspace) -> void:
	var close_after_save := func() -> void:
		_close_active(workspace)
	_save_then.call(workspace, close_after_save, "Save failed - keeping the tab open.")
