extends RefCounted

# MenuDriver's doc-id-keyed TABLE runtime state (rows seeded by companions +
# the multi-select set), and the push of both onto the compiled frame.
# Extracted from menu_driver.gd unchanged; the driver keeps the public
# delegates companions call. The state is the widget's MenuWidgetState.


static func add_row(state: MenuWidgetState, cells: PackedStringArray) -> void:
	state.table_rows.append(cells)
	state.has_table_rows = true


static func remove_row(state: MenuWidgetState, row: int) -> bool:
	if row < 0 or row >= state.table_rows.size():
		return false
	state.table_rows.remove_at(row)
	var reindexed := PackedInt32Array()
	for r in state.table_selected:
		if r < row:
			reindexed.append(r)
		elif r > row:
			reindexed.append(r - 1)
	state.table_selected = reindexed
	return true


static func clear_rows(state: MenuWidgetState) -> void:
	state.table_rows = []
	state.has_table_rows = true
	state.table_selected = PackedInt32Array()


static func row_count(state: MenuWidgetState) -> int:
	return state.table_rows.size() if state != null else 0


static func cell_text(state: MenuWidgetState, row: int, col: int) -> String:
	if state == null or row < 0 or row >= state.table_rows.size():
		return ""
	var cells := state.table_rows[row]
	return cells[col] if col >= 0 and col < cells.size() else ""


static func select_row(state: MenuWidgetState, row: int, additive: bool) -> void:
	var selected := state.table_selected if additive else PackedInt32Array()
	if selected.has(row):
		var kept := PackedInt32Array()
		for r in selected:
			if r != row:
				kept.append(r)
		selected = kept
	else:
		selected.append(row)
	state.table_selected = selected


static func push_rows(frame: MenuFrame, index: int, state: MenuWidgetState) -> void:
	if index < 0 or state == null:
		return
	frame.set_widget_table_rows(index, state.table_rows)
	push_selection(frame, index, state)


static func push_selection(frame: MenuFrame, index: int, state: MenuWidgetState) -> void:
	if index < 0 or state == null:
		return
	var selected := state.table_selected
	frame.set_widget_selected_set(index, selected)
	var first := selected[0] if selected.size() > 0 else -1
	frame.set_widget_selection(index, first, -1, state.scroll_row)
