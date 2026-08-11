extends RefCounted

const MenuScrollRange := preload("res://game/menu_scroll_range.gd")

# Replays MenuDriver's document-id state onto the current MenuFrame index map.
# Keeping the frame-setter knowledge here makes screen recompilation a single
# operation for the driver and keeps every saved-state default in one place.


static func apply(frame: MenuFrame, index_of_id: Dictionary,
		id_state: Dictionary) -> void:
	for id in index_of_id:
		var state: Dictionary = id_state.get(id, {})
		if state.is_empty():
			continue
		var index := int(index_of_id[id])
		if state.has("shown"):
			frame.set_widget_shown_override(index, bool(state["shown"]))
		if state.has("disabled"):
			frame.set_widget_disabled(index, bool(state["disabled"]))
		if state.has("checked"):
			frame.set_widget_checked(index, bool(state["checked"]))
		if state.has("text"):
			frame.set_widget_text(index, String(state["text"]))
		if state.has("items"):
			frame.set_widget_items(index, state["items"])
		if state.has("selected_item") or state.has("scroll_row"):
			frame.set_widget_selection(index, int(state.get("selected_item", 0)),
					-1, int(state.get("scroll_row", 0)))
		if state.has("scroll_range"):
			var scroll := state["scroll_range"] as MenuScrollRange
			if scroll != null:
				frame.set_widget_scroll_range(index, scroll.minimum, scroll.maximum,
						scroll.page, scroll.value)
		if state.has("selected_set"):
			frame.set_widget_selected_set(index, state["selected_set"])
		if state.has("table_rows"):
			frame.set_widget_table_rows(index, state["table_rows"])
		if state.has("marquee_lines"):
			frame.set_widget_marquee_lines(index, state["marquee_lines"])
