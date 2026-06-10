extends Control

## Entry table for the Strings workspace: a searchable, sortable Tree. Emits
## entry_selected when the user picks a row and delete_requested when Delete is
## pressed on the selection. Rows that fail validation are tinted and tooltipped.
## Lives inside the self-contained StringsEditorView (center surface).

signal entry_selected(index: int)
signal delete_requested(index: int)

const COL_KEY := 0
const COL_TEXT := 1
const COL_SECTION := 2
## Sentinel sort column: file order — rows appear exactly as they sit in the
## .bin, which is meaningful data here (the game reads entries by contiguous
## section runs). Clicking a column header cycles ascending -> descending ->
## back to file order.
const SORT_FILE_ORDER := -1
const TEXT_PREVIEW_LIMIT := 90
const ISSUE_COLOR := Color(1.0, 0.55, 0.55)

var _doc: StringsEditor
var _tree: Tree
var _search: String = ""
var _section_filter: int = -1
var _sort_column: int = SORT_FILE_ORDER
var _sort_ascending: bool = true
var _issues: Dictionary = {}
var _selecting: bool = false


func _ready() -> void:
	set_anchors_preset(Control.PRESET_FULL_RECT)
	_tree = Tree.new()
	_tree.name = "StringsTree"
	_tree.set_anchors_preset(Control.PRESET_FULL_RECT)
	_tree.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_tree.columns = 3
	_tree.select_mode = Tree.SELECT_ROW
	_tree.hide_root = true
	_tree.column_titles_visible = true
	_tree.set_column_title(COL_KEY, "Key")
	_tree.set_column_title(COL_TEXT, "Text")
	_tree.set_column_title(COL_SECTION, "Section")
	_tree.set_column_expand_ratio(COL_KEY, 2)
	_tree.set_column_expand_ratio(COL_TEXT, 4)
	_tree.set_column_expand_ratio(COL_SECTION, 1)
	_tree.item_selected.connect(_on_item_selected)
	_tree.column_title_clicked.connect(_on_column_title_clicked)
	_tree.gui_input.connect(_on_tree_gui_input)
	add_child(_tree)
	rebuild()


func set_document(doc: StringsEditor) -> void:
	_doc = doc
	rebuild()


func set_filter(search: String, section_filter: int) -> void:
	_search = search.strip_edges().to_lower()
	_section_filter = section_filter
	rebuild()


func rebuild() -> void:
	if _tree == null or _doc == null:
		return
	_tree.clear()
	_issues = _doc.validate().get("issues_by_index", {})
	var root := _tree.create_item()

	var indices := _filtered_indices()
	_sort_indices(indices)

	var table := _doc.string_table
	for i in indices:
		var item := _tree.create_item(root)
		item.set_metadata(COL_KEY, i)
		_fill_item(item, i, table)


func update_row(index: int) -> void:
	# Patch a single row in place (live text/key/position edits) without a full
	# rebuild, so scroll and caret are preserved. No-op if the row is filtered out.
	if _tree == null or _doc == null:
		return
	var root := _tree.get_root()
	if root == null:
		return
	var child := root.get_first_child()
	while child != null:
		if int(child.get_metadata(COL_KEY)) == index:
			# A live edit can move the row out of the active filter (e.g. changing
			# its section under a section filter, or editing text out of a search).
			# Patch it in place while it still matches; rebuild to drop it otherwise.
			if _passes_filter(index):
				_fill_item(child, index, _doc.string_table)
			else:
				rebuild()
			return
		child = child.get_next()


func _passes_filter(index: int) -> bool:
	var table := _doc.string_table
	if _section_filter >= 0 and table.get_entry_section_index(index) != _section_filter:
		return false
	if not _search.is_empty():
		var key := table.get_entry_key(index).to_lower()
		var text := table.get_entry_text(index).to_lower()
		if not key.contains(_search) and not text.contains(_search):
			return false
	return true


func select_index(index: int) -> void:
	if _tree == null or index < 0:
		return
	var root := _tree.get_root()
	if root == null:
		return
	var child := root.get_first_child()
	while child != null:
		if int(child.get_metadata(COL_KEY)) == index:
			_selecting = true
			_tree.set_selected(child, COL_KEY)
			_tree.scroll_to_item(child)
			_selecting = false
			return
		child = child.get_next()


func get_selected_index() -> int:
	if _tree == null:
		return -1
	var item := _tree.get_selected()
	return int(item.get_metadata(COL_KEY)) if item != null else -1


func _fill_item(item: TreeItem, index: int, table: RtxtStringFile) -> void:
	var section := table.get_entry_section_index(index)
	var section_count := table.get_section_count()
	var section_label := table.get_section_name(section) if section >= 0 and section < section_count else "#%d" % section
	item.set_text(COL_KEY, table.get_entry_key(index))
	item.set_text(COL_TEXT, _preview(table.get_entry_text(index)))
	item.set_text(COL_SECTION, section_label)

	var issue_list := _issues.get(index, []) as Array
	var color := ISSUE_COLOR if not issue_list.is_empty() else Color(1, 1, 1)
	var tooltip := "\n".join(PackedStringArray(issue_list)) if not issue_list.is_empty() else ""
	for col in 3:
		if issue_list.is_empty():
			item.clear_custom_color(col)
		else:
			item.set_custom_color(col, color)
		item.set_tooltip_text(col, tooltip)


func _filtered_indices() -> Array:
	var out: Array = []
	for i in _doc.string_table.get_entry_count():
		if _passes_filter(i):
			out.append(i)
	return out


func _sort_indices(indices: Array) -> void:
	if _sort_column == SORT_FILE_ORDER:
		return  # _filtered_indices already walks entries in file order
	var table = _doc.string_table
	var ascending := _sort_ascending
	var column := _sort_column
	indices.sort_custom(func(a, b):
		var av: Variant
		var bv: Variant
		match column:
			COL_TEXT:
				av = table.get_entry_text(a).to_lower()
				bv = table.get_entry_text(b).to_lower()
			COL_SECTION:
				av = table.get_entry_section_index(a)
				bv = table.get_entry_section_index(b)
			_:
				av = table.get_entry_key(a).to_lower()
				bv = table.get_entry_key(b).to_lower()
		if av == bv:
			return a < b if ascending else a > b
		return av < bv if ascending else av > bv
	)


func _preview(text: String) -> String:
	var first_line := text.replace("\r", "").split("\n")[0]
	if first_line.length() > TEXT_PREVIEW_LIMIT:
		first_line = first_line.substr(0, TEXT_PREVIEW_LIMIT) + "…"
	if text.contains("\n"):
		first_line += " ⏎"
	return first_line


func _on_item_selected() -> void:
	if _selecting:
		return
	var item := _tree.get_selected()
	if item != null:
		entry_selected.emit(int(item.get_metadata(COL_KEY)))


func _on_column_title_clicked(column: int, _mouse_button_index: int) -> void:
	if column == _sort_column:
		if _sort_ascending:
			_sort_ascending = false
		else:
			_sort_column = SORT_FILE_ORDER  # third click returns to file order
			_sort_ascending = true
	else:
		_sort_column = column
		_sort_ascending = true
	rebuild()
	select_index(_doc.selected_index if _doc != null else -1)


func _on_tree_gui_input(event: InputEvent) -> void:
	if event is InputEventKey and event.pressed and not event.echo and event.keycode == KEY_DELETE:
		var item := _tree.get_selected()
		if item != null:
			delete_requested.emit(int(item.get_metadata(COL_KEY)))
			accept_event()
