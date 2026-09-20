extends RefCounted

# Replays MenuDriver's document-id state onto the current MenuFrame index map.
# Keeping the frame-setter knowledge here makes screen recompilation a single
# operation for the driver and keeps every saved-state default in one place.


static func apply(frame: MenuFrame, index_of_id: Dictionary,
		id_state: Dictionary) -> void:
	for id in index_of_id:
		var state: MenuWidgetState = id_state.get(id)
		if state == null or state.is_empty():
			continue
		var index := int(index_of_id[id])
		if state.has_shown:
			frame.set_widget_shown_override(index, state.shown)
		if state.has_disabled:
			frame.set_widget_disabled(index, state.disabled)
		if state.has_checked:
			frame.set_widget_checked(index, state.checked)
		if state.has_text:
			frame.set_widget_text(index, state.text)
		if state.has_items:
			frame.set_widget_items(index, state.items)
		if state.has_selected_item or state.has_scroll_row:
			frame.set_widget_selection(index, state.selected_item, -1, state.scroll_row)
		var scroll := state.scroll_range
		if scroll != null:
			frame.set_widget_scroll_range(index, scroll.minimum, scroll.maximum,
					scroll.page, scroll.value)
		if state.has_selected_set:
			frame.set_widget_selected_set(index, state.selected_set)
		if state.has_table_rows:
			frame.set_widget_table_rows(index, state.table_rows)
