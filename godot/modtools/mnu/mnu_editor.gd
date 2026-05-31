class_name MnuEditor
extends Control

# The Menus workspace editor surface: a widget tree alongside a live WYSIWYG
# preview. Binds to an MnuEditorDocument, drives the canvas preview, and tracks
# selection by the document's stable widget id. Selection is re-emitted as
# widget_selected(id) so the workspace adapter can populate the right-dock
# property inspector. M6 is read-only; M7/M8 add the undo stack + canvas
# gestures here.

const MnuWidgetTreeScript = preload("res://modtools/mnu/mnu_widget_tree.gd")
const MnuCanvasScript = preload("res://modtools/mnu/mnu_canvas.gd")

signal widget_selected(id: int)

var _document   # MnuEditorDocument
var _resource_root: NovaResourceRoot
var _tree        # MnuWidgetTree
var _canvas      # MnuCanvas
var _selected_id := -1


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
# inspector in sync through the adapter.
func _apply_selection(id: int) -> void:
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


func _on_resource_loaded(_resource) -> void:
	_selected_id = -1
	_refresh_all()


func _on_resource_changed() -> void:
	# In-place document mutation (M7 edits): rebuild the tree + preview, keeping the
	# current selection if its id still exists, else falling back to the first
	# screen. Document loads route through _on_resource_loaded instead (full reset).
	if not is_node_ready():
		return
	var doc := _document_resource()
	if _tree != null:
		_tree.set_document(doc)
	_refresh_preview()
	if doc != null and doc.widget_exists(_selected_id):
		select_widget(_selected_id)
	elif doc != null and doc.get_screen_count() > 0:
		select_widget(doc.get_screen_ids()[0])
	else:
		_apply_selection(-1)
