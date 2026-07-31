class_name MnuWidgetTree
extends Tree

# The Menus workspace's structural navigator: a Tree mirroring the document's
# screen -> root window -> widget hierarchy. Items are keyed by the document's
# stable widget id (carried in item metadata) so selection survives a rebuild.
# Selecting an item emits widget_selected(id); the editor maps that to the canvas
# preview + inspector. M8b: dragging a widget item onto/between other items emits
# reparent_requested(id, new_parent, index); the editor routes that through
# NovaMnuDocument.reparent_widget (which is the cycle authority).

signal widget_selected(id: int)
signal reparent_requested(id: int, new_parent: int, index: int)

var _document: NovaMnuDocument
var _item_by_id: Dictionary = {}
var _suppress_selection := false
var _authoring_enabled := true


func _ready() -> void:
	columns = 1
	hide_root = true
	select_mode = Tree.SELECT_SINGLE
	allow_reselect = true
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL
	drop_mode_flags = Tree.DROP_MODE_INBETWEEN | Tree.DROP_MODE_ON_ITEM
	if not item_selected.is_connected(_on_item_selected):
		item_selected.connect(_on_item_selected)
	rebuild()


func set_document(doc: NovaMnuDocument) -> void:
	_document = doc
	if is_node_ready():
		rebuild()


func set_authoring_enabled(enabled: bool) -> void:
	_authoring_enabled = enabled
	drop_mode_flags = (Tree.DROP_MODE_INBETWEEN | Tree.DROP_MODE_ON_ITEM) \
		if enabled else Tree.DROP_MODE_DISABLED


func rebuild() -> void:
	clear()
	_item_by_id.clear()
	if _document == null:
		return
	var root := create_item()  # hidden root
	for screen_id in _document.get_screen_ids():
		var screen_item := create_item(root)
		var screen_name := _document.get_screen_name(screen_id)
		screen_item.set_text(0, "%s  (Screen)" % (screen_name if not screen_name.is_empty() else "<screen>"))
		screen_item.set_metadata(0, screen_id)
		_item_by_id[screen_id] = screen_item
		var root_window := _document.get_screen_root_id(screen_id)
		if root_window > 0:
			_add_widget_subtree(screen_item, root_window)


func _add_widget_subtree(parent_item: TreeItem, widget_id: int) -> void:
	var item := create_item(parent_item)
	item.set_text(0, _widget_label(widget_id))
	item.set_metadata(0, widget_id)
	_item_by_id[widget_id] = item
	for child_id in _document.get_child_ids(widget_id):
		_add_widget_subtree(item, child_id)


func _widget_label(widget_id: int) -> String:
	var name := _document.get_widget_name(widget_id)
	var type_name := _document.get_widget_type_name(_document.get_widget_type(widget_id))
	if name.is_empty():
		return "(%s)" % type_name
	return "%s  (%s)" % [name, type_name]


# Programmatic selection (from the canvas pick or external select); guarded so it
# does not re-emit widget_selected back through the editor.
func select_id(id: int) -> void:
	var item := _item_by_id.get(id) as TreeItem
	if item == null:
		return
	_suppress_selection = true
	# Expand any collapsed ancestors first: Godot keeps items under a collapsed
	# parent hidden, so a deep canvas pick would otherwise select a row the user
	# cannot see. Walking up to (but not past) the hidden root reveals it.
	var ancestor := item.get_parent()
	while ancestor != null:
		ancestor.collapsed = false
		ancestor = ancestor.get_parent()
	item.select(0)
	scroll_to_item(item)
	_suppress_selection = false


func get_selected_id() -> int:
	var item := get_selected()
	if item == null:
		return -1
	return int(item.get_metadata(0))


func _on_item_selected() -> void:
	if _suppress_selection:
		return
	var item := get_selected()
	if item == null:
		return
	widget_selected.emit(int(item.get_metadata(0)))


# --- Drag-to-reparent (M8b) -----------------------------------------------------

# A non-screen, non-root widget can be dragged. The drag payload carries its id.
func _get_drag_data(at_position: Vector2) -> Variant:
	if not _authoring_enabled:
		return null
	var item := get_item_at_position(at_position)
	if item == null or _document == null:
		return null
	var id := int(item.get_metadata(0))
	if not _document.widget_exists(id) or _document.is_screen(id) or _is_root_window(id):
		return null
	var preview := Label.new()
	preview.text = _widget_label(id)
	set_drag_preview(preview)
	return {"mnu_widget_id": id}


func _can_drop_data(at_position: Vector2, data: Variant) -> bool:
	if not _authoring_enabled:
		return false
	if typeof(data) != TYPE_DICTIONARY or not data.has("mnu_widget_id") or _document == null:
		return false
	var src := int(data["mnu_widget_id"])
	var target := _drop_target(at_position)
	if target.is_empty():
		return false
	var parent := int(target["parent"])
	if parent == src:
		return false
	# Refuse dropping a node into itself or one of its descendants (the engine
	# refuses too, but this suppresses the drop cursor for an invalid target).
	var cur := parent
	while cur > 0 and _document.widget_exists(cur):
		if cur == src:
			return false
		if _document.is_screen(cur):
			break
		cur = _document.get_parent_id(cur)
	return true


func _drop_data(at_position: Vector2, data: Variant) -> void:
	if not _authoring_enabled:
		return
	if typeof(data) != TYPE_DICTIONARY or not data.has("mnu_widget_id"):
		return
	var target := _drop_target(at_position)
	if target.is_empty():
		return
	reparent_requested.emit(int(data["mnu_widget_id"]), int(target["parent"]), int(target["index"]))


# Resolve a drop position to {parent, index}. ON an item nests as its last child
# (a screen targets its root window, append); BETWEEN items inserts as a sibling
# of the target within its parent. Empty dict when the position has no item.
func _drop_target(at_position: Vector2) -> Dictionary:
	var item := get_item_at_position(at_position)
	if item == null:
		return {}
	var target_id := int(item.get_metadata(0))
	if not _document.widget_exists(target_id):
		return {}
	var section := get_drop_section_at_position(at_position)
	if section == 0:
		return {"parent": target_id, "index": _effective_child_count(target_id)}
	var parent_id := _document.get_parent_id(target_id)
	# A root window / screen has no sibling slot; fall back to nesting in the target.
	if parent_id <= 0 or _document.is_screen(parent_id):
		return {"parent": target_id, "index": _effective_child_count(target_id)}
	var idx := _document.get_child_ids(parent_id).find(target_id)
	if idx < 0:
		idx = 0
	if section == 1:
		idx += 1
	return {"parent": parent_id, "index": idx}


# Child count of the container an id represents: a screen's container is its root
# window, so a drop ONTO a screen appends after the root window's existing children.
func _effective_child_count(id: int) -> int:
	if _document.is_screen(id):
		var root := _document.get_screen_root_id(id)
		return _document.get_child_ids(root).size() if root > 0 else 0
	return _document.get_child_ids(id).size()


func _is_root_window(id: int) -> bool:
	if not _document.widget_exists(id) or _document.is_screen(id):
		return false
	var p := _document.get_parent_id(id)
	return p > 0 and _document.is_screen(p)
