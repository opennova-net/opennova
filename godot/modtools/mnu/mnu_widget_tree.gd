class_name MnuWidgetTree
extends Tree

# The Menus workspace's structural navigator: a Tree mirroring the document's
# screen -> root window -> widget hierarchy. Items are keyed by the document's
# stable widget id (carried in item metadata) so selection survives a rebuild.
# Selecting an item emits widget_selected(id); the editor maps that to the canvas
# preview + read-only inspector. M6 is browse-only (no reparent/add/delete yet).

signal widget_selected(id: int)

var _document: NovaMnuDocument
var _item_by_id: Dictionary = {}
var _suppress_selection := false


func _ready() -> void:
	columns = 1
	hide_root = true
	select_mode = Tree.SELECT_SINGLE
	allow_reselect = true
	size_flags_horizontal = Control.SIZE_EXPAND_FILL
	size_flags_vertical = Control.SIZE_EXPAND_FILL
	if not item_selected.is_connected(_on_item_selected):
		item_selected.connect(_on_item_selected)
	rebuild()


func set_document(doc: NovaMnuDocument) -> void:
	_document = doc
	if is_node_ready():
		rebuild()


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
