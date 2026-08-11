extends RefCounted

# MenuDriver's doc-id-keyed TABLE runtime state (rows seeded by companions +
# the multi-select set), and the push of both onto the compiled frame.
# Extracted from menu_driver.gd unchanged; the driver keeps the public
# delegates companions call.


static func add_row(state: Dictionary, cells: PackedStringArray) -> void:
	var rows: Array = state.get("table_rows", [])
	rows.append(cells)
	state["table_rows"] = rows


static func remove_row(state: Dictionary, row: int) -> bool:
	var rows: Array = state.get("table_rows", [])
	if row < 0 or row >= rows.size():
		return false
	rows.remove_at(row)
	state["table_rows"] = rows
	var selected: PackedInt32Array = state.get("table_selected", PackedInt32Array())
	var reindexed := PackedInt32Array()
	for r in selected:
		if r < row:
			reindexed.append(r)
		elif r > row:
			reindexed.append(r - 1)
	state["table_selected"] = reindexed
	return true


static func clear_rows(state: Dictionary) -> void:
	state["table_rows"] = []
	state["table_selected"] = PackedInt32Array()


static func row_count(state: Dictionary) -> int:
	return (state.get("table_rows", []) as Array).size()


static func cell_text(state: Dictionary, row: int, col: int) -> String:
	var rows: Array = state.get("table_rows", [])
	if row < 0 or row >= rows.size():
		return ""
	var cells: PackedStringArray = rows[row]
	return cells[col] if col >= 0 and col < cells.size() else ""


static func select_row(state: Dictionary, row: int, additive: bool) -> void:
	var selected: PackedInt32Array = state.get("table_selected", PackedInt32Array()) \
			if additive else PackedInt32Array()
	if selected.has(row):
		var kept := PackedInt32Array()
		for r in selected:
			if r != row:
				kept.append(r)
		selected = kept
	else:
		selected.append(row)
	state["table_selected"] = selected


static func push_rows(frame: MenuFrame, index: int, state: Dictionary) -> void:
	if index < 0:
		return
	var rows: Array = state.get("table_rows", [])
	var typed: Array[PackedStringArray] = []
	for row in rows:
		typed.append(row as PackedStringArray)
	frame.set_widget_table_rows(index, typed)
	push_selection(frame, index, state)


static func push_selection(frame: MenuFrame, index: int, state: Dictionary) -> void:
	if index < 0:
		return
	var selected: PackedInt32Array = state.get("table_selected",
			PackedInt32Array())
	frame.set_widget_selected_set(index, selected)
	var first := selected[0] if selected.size() > 0 else -1
	frame.set_widget_selection(index, first, -1,
			int(state.get("scroll_row", 0)))
