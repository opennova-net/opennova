class_name ListDetailInspector
extends WorkflowInspector

## Base for inspectors that show a list of items on the left and a detail dock
## for the selected item on the right. The list panel, summary, selection
## tracking, empty-state, and refresh are shared; subclasses provide the item
## collection, each row's label, and the detail fields.
##
## Simple inspectors (e.g. lights) use the default build_main/build_detail flow
## and only override the data/label/name hooks plus _build_detail_fields. More
## involved inspectors (e.g. materials) override build_main/build_detail and
## reuse the protected helpers (_build_list_panel, _populate_list, _refresh_list).

var _selected_index := 0
var _list: ItemList
var _list_panel: VBoxContainer
var _list_summary: Label


func has_detail() -> bool:
	return true


func _resync_detail(_index: int) -> bool:
	# Override to re-sync the already-built detail controls in place for the new
	# selection (cheap), instead of tearing down and rebuilding the whole detail
	# dock. Returning false (the default) falls back to a full rebuild.
	return false


# --- Overridable data / label hooks ---
func _list_items() -> Array:
	return []


func _list_item_text(_item, index: int) -> String:
	return str(index)


func _list_summary_text(count: int) -> String:
	return "%d items" % count


func _empty_detail_text() -> String:
	return "Select an item."


func _empty_detail_node_name() -> StringName:
	return &""


func _build_detail_fields(_detail_box: VBoxContainer, _index: int) -> void:
	pass


# --- Overridable node names (kept stable so external lookups / tests resolve) ---
func _list_node_name() -> StringName:
	return &"DetailList"


func _list_panel_node_name() -> StringName:
	return &"DetailListPanel"


func _detail_panel_node_name() -> StringName:
	return &"DetailPanel"


# --- Default build flow (subclasses may override and reuse the helpers below) ---
func build_main(mount: Control) -> void:
	var box := _make_inspector_box(mount)
	_build_list_panel(box)


func build_detail(box: VBoxContainer) -> void:
	var detail_box := VBoxContainer.new()
	detail_box.name = _detail_panel_node_name()
	detail_box.add_theme_constant_override("separation", 8)
	detail_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(detail_box)
	var items := _list_items()
	if items.is_empty():
		_add_empty_state(detail_box, _empty_detail_text(), String(_empty_detail_node_name()))
		return
	_selected_index = clampi(_selected_index, 0, items.size() - 1)
	_build_detail_fields(detail_box, _selected_index)


func refresh() -> void:
	_refresh_list()


# --- Shared helpers (usable by overriding subclasses) ---
func _build_list_panel(box: VBoxContainer) -> VBoxContainer:
	var items := _list_items()
	_list_panel = VBoxContainer.new()
	_list_panel.name = _list_panel_node_name()
	_list_panel.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(_list_panel)

	_list_summary = Label.new()
	_list_summary.name = String(_list_panel_node_name()) + "Summary"
	_list_summary.theme_type_variation = &"Muted"
	_list_summary.text = _list_summary_text(items.size())
	_list_panel.add_child(_list_summary)

	_list = ItemList.new()
	_list.name = _list_node_name()
	_list.custom_minimum_size = Vector2(0, 200)
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_populate_list(_list, items)
	_list_panel.add_child(_list)

	if items.is_empty():
		_selected_index = -1
	else:
		_selected_index = clampi(_selected_index, 0, items.size() - 1)
		_list.select(_selected_index)

	_list.item_selected.connect(func(index: int) -> void:
		_selected_index = index
		if not _resync_detail(index):
			_rebuild_detail_dock()
	)
	return _list_panel


func _populate_list(list: ItemList, items: Array) -> void:
	if list == null or not is_instance_valid(list):
		return
	list.clear()
	for i in range(items.size()):
		list.add_item(_list_item_text(items[i], i))


func _refresh_list() -> void:
	if _list == null or not is_instance_valid(_list):
		return
	var items := _list_items()
	if items.is_empty():
		_selected_index = -1
	else:
		_selected_index = clampi(_selected_index, 0, items.size() - 1)
	_populate_list(_list, items)
	if _list_summary != null and is_instance_valid(_list_summary):
		_list_summary.text = _list_summary_text(items.size())
	if _selected_index >= 0 and _selected_index < items.size():
		_list.select(_selected_index)
