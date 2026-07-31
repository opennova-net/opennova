extends RefCounted

# Deep authoring module for the Menus workspace's structural and professional
# layout commands. The editor facade supplies the live document plus a compact
# context (selection, visible screen, and rendered extents) through execute();
# this module owns validation, mutation ordering, clipboard payloads, layout
# math, and the normalized result consumed by snapshot history.
#
# Result interface:
#   value       command-specific return value (optional)
#   op          snapshot-history label (present only for a mutation)
#   before      pre-mutation NovaMnuDocument state
#   sel_before  selection restored by undo
#   sel_after   selection restored by redo
#   selection   full multi-selection to restore after the command (optional)

const DEFAULT_WIDGET_RECT := Rect2(20, 20, 100, 30)

var _clipboard_payload: PackedByteArray = PackedByteArray()


func execute(document: NovaMnuDocument, command: StringName,
		context: Dictionary = {}) -> Dictionary:
	if document == null:
		return {}
	match command:
		&"move_rects":
			return _move_rects(document, context)
		&"add_widget":
			return _add_widget(document, context)
		&"add_widgets":
			return _add_widgets(document, context)
		&"delete_widget":
			return _delete_widget(document, context)
		&"reparent_widget":
			return _reparent_widget(document, context)
		&"copy_widget":
			return _copy_widget(document, context)
		&"paste_widget":
			return _paste_widget(document, context)
		&"duplicate_widget":
			return _duplicate_widget(document, context)
		&"align":
			return _align(document, context)
		&"distribute":
			return _distribute(document, context)
		&"z_order":
			return _change_z_order(document, context)
		&"duplicate_screen":
			return _duplicate_screen(document, context)
		&"move_screen":
			return _move_screen(document, context)
		&"add_screen":
			return _add_screen(document, context)
		&"delete_screen":
			return _delete_screen(document, context)
	return {}


func _history_result(op: String, before: Dictionary, sel_before: int,
		sel_after: int) -> Dictionary:
	return {
		"op": op,
		"before": before,
		"sel_before": sel_before,
		"sel_after": sel_after,
	}


func _selected_id(context: Dictionary) -> int:
	return int(context.get("selected_id", -1))


func _visible_screen_id(context: Dictionary) -> int:
	return int(context.get("visible_screen_id", -1))


func _move_rects(document: NovaMnuDocument, context: Dictionary) -> Dictionary:
	var edits: Array = context.get("edits", [])
	if edits.is_empty():
		return {}
	var selected := _selected_id(context)
	var before := document.capture_state()
	for edit_value in edits:
		var edit: Dictionary = edit_value
		var id := int(edit.get("id", -1))
		if id >= 0 and document.widget_exists(id):
			document.set_window_rect(id, edit["rect"])
	var result := _history_result("move_batch", before, selected, selected)
	result["selection"] = _selection(context)
	return result


func _parent_for_add(document: NovaMnuDocument, selected: int) -> int:
	if selected >= 0 and document.widget_exists(selected):
		return selected
	return document.get_screen_ids()[0] if document.get_screen_count() > 0 else -1


func _add_widget(document: NovaMnuDocument, context: Dictionary) -> Dictionary:
	var selected := _selected_id(context)
	var parent := _parent_for_add(document, selected)
	if parent < 0:
		return {"value": -1}
	var before := document.capture_state()
	var new_id := int(document.add_widget(parent, int(context.get("type", -1)),
		context.get("rect", DEFAULT_WIDGET_RECT)))
	if new_id < 0:
		return {"value": -1}
	var result := _history_result("add_widget", before, selected, new_id)
	result["value"] = new_id
	return result


func _add_widgets(document: NovaMnuDocument, context: Dictionary) -> Dictionary:
	var rows: Array = context.get("rows", [])
	var results: Array = []
	if rows.is_empty():
		return {"value": results}
	var selected := _selected_id(context)
	var before := document.capture_state()
	var last_id := -1
	for row_value in rows:
		var row: Dictionary = row_value
		var new_id := int(document.add_widget(int(row.get("parent", -1)),
			int(row.get("type", -1)), row.get("rect", DEFAULT_WIDGET_RECT)))
		if new_id < 0:
			results.append({"ok": false})
			continue
		var props: Dictionary = row.get("props", {})
		for prop in props:
			_write_widget_prop(document, new_id, String(prop), props[prop])
		results.append({"ok": true, "id": new_id})
		last_id = new_id
	var result := _history_result("add_widgets_batch", before, selected,
		last_id if last_id >= 0 else selected)
	result["value"] = results
	return result


func _delete_widget(document: NovaMnuDocument, context: Dictionary) -> Dictionary:
	var selected := _selected_id(context)
	if selected < 0 or not document.widget_exists(selected) \
			or document.is_screen(selected) or _is_root_window(document, selected):
		return {}
	var parent := int(document.get_parent_id(selected))
	var before := document.capture_state()
	document.delete_widget(selected)
	return _history_result("delete_widget", before, selected,
		_resolve_existing_selection(document, parent))


func _reparent_widget(document: NovaMnuDocument, context: Dictionary) -> Dictionary:
	var selected := _selected_id(context)
	var id := int(context.get("id", -1))
	var before := document.capture_state()
	if not bool(document.reparent_widget(id, int(context.get("new_parent", -1)),
			int(context.get("index", -1)))):
		return {}
	return _history_result("reparent_widget", before, selected, id)


func _copy_widget(document: NovaMnuDocument, context: Dictionary) -> Dictionary:
	var selected := _selected_id(context)
	if selected < 0 or not document.widget_exists(selected) \
			or document.is_screen(selected) or _is_root_window(document, selected):
		return {"value": false}
	_clipboard_payload = document.capture_widget_subtree(selected)
	return {"value": not _clipboard_payload.is_empty()}


func _sibling_target(document: NovaMnuDocument, id: int) -> Dictionary:
	if id < 0 or not document.widget_exists(id):
		return {}
	if document.is_screen(id):
		return {"parent": document.get_screen_root_id(id), "index": -1}
	var parent := int(document.get_parent_id(id))
	if parent < 0 or document.is_screen(parent):
		return {}
	var children: PackedInt32Array = document.get_child_ids(parent)
	return {"parent": parent, "index": children.find(id) + 1}


func _paste_widget(document: NovaMnuDocument, context: Dictionary) -> Dictionary:
	if _clipboard_payload.is_empty():
		return {"value": -1}
	var selected := _selected_id(context)
	var target := _sibling_target(document, selected)
	if target.is_empty():
		var screen_id := _visible_screen_id(context)
		if screen_id < 0:
			return {"value": -1}
		target = {"parent": document.get_screen_root_id(screen_id), "index": -1}
	var before := document.capture_state()
	var id := int(document.insert_widget_subtree(int(target["parent"]),
		_clipboard_payload, int(target["index"]), Vector2i(10, 10)))
	var result := {"value": id}
	if id >= 0:
		result.merge(_history_result("paste_widget", before, selected, id))
	return result


func _duplicate_widget(document: NovaMnuDocument, context: Dictionary) -> Dictionary:
	var selected := _selected_id(context)
	if selected < 0 or not document.widget_exists(selected) \
			or document.is_screen(selected) or _is_root_window(document, selected):
		return {"value": -1}
	var payload: PackedByteArray = document.capture_widget_subtree(selected)
	var target := _sibling_target(document, selected)
	if payload.is_empty() or target.is_empty():
		return {"value": -1}
	var before := document.capture_state()
	var id := int(document.insert_widget_subtree(int(target["parent"]), payload,
		int(target["index"]), Vector2i(10, 10)))
	var result := {"value": id}
	if id >= 0:
		result.merge(_history_result("duplicate_widget", before, selected, id))
	return result


func _selection(context: Dictionary) -> PackedInt32Array:
	var ids: PackedInt32Array = context.get("selection", PackedInt32Array()).duplicate()
	var selected := _selected_id(context)
	if ids.size() <= 1 and selected >= 0:
		ids = PackedInt32Array([selected])
	return ids


func _selected_widgets_same_parent(document: NovaMnuDocument,
		context: Dictionary, minimum: int) -> PackedInt32Array:
	var ids := _selection(context)
	if ids.size() < minimum:
		return PackedInt32Array()
	var parent := -1
	for id in ids:
		if not document.widget_exists(id) or document.is_screen(id) \
				or _is_root_window(document, id):
			return PackedInt32Array()
		var this_parent := int(document.get_parent_id(id))
		if parent < 0:
			parent = this_parent
		elif this_parent != parent:
			return PackedInt32Array()
	return ids


func _layout_rect(document: NovaMnuDocument, context: Dictionary, id: int) -> Rect2:
	var rendered_rects: Dictionary = context.get("rendered_rects", {})
	var rendered: Rect2 = rendered_rects.get(id, Rect2())
	if rendered.size.x > 0.0 or rendered.size.y > 0.0:
		return rendered
	return document.get_window_rect(id)


func _rect_preserving_auto(document: NovaMnuDocument, id: int,
		position: Vector2) -> Rect2:
	var rect := document.get_window_rect(id)
	var flags := int(document.get_window_rect_flags(id))
	var size := rect.size
	if (flags & NovaMnuDocument.RECT_HAS_RIGHT) == 0:
		size.x = -1
	if (flags & NovaMnuDocument.RECT_HAS_BOTTOM) == 0:
		size.y = -1
	return Rect2(position, size)


func _layout_to_local_position(document: NovaMnuDocument, context: Dictionary,
		id: int, board_position: Vector2) -> Vector2:
	var local := document.get_window_rect(id)
	var rendered := _layout_rect(document, context, id)
	return board_position - (rendered.position - local.position)


func _align(document: NovaMnuDocument, context: Dictionary) -> Dictionary:
	var ids := _selected_widgets_same_parent(document, context, 2)
	if ids.is_empty():
		return {}
	var mode := String(context.get("mode", ""))
	var bounds := _layout_rect(document, context, ids[0])
	for index in range(1, ids.size()):
		bounds = bounds.merge(_layout_rect(document, context, ids[index]))
	var edits: Array = []
	for id in ids:
		var rect := _layout_rect(document, context, id)
		var position := rect.position
		match mode:
			"left": position.x = bounds.position.x
			"right": position.x = bounds.end.x - rect.size.x
			"top": position.y = bounds.position.y
			"bottom": position.y = bounds.end.y - rect.size.y
			"hcenter": position.x = bounds.get_center().x - rect.size.x * 0.5
			"vcenter": position.y = bounds.get_center().y - rect.size.y * 0.5
			_: return {}
		edits.append({"id": id, "rect": _rect_preserving_auto(document, id,
			_layout_to_local_position(document, context, id, position))})
	var move_context := context.duplicate()
	move_context["edits"] = edits
	return _move_rects(document, move_context)


func _distribute(document: NovaMnuDocument, context: Dictionary) -> Dictionary:
	var ids := _selected_widgets_same_parent(document, context, 3)
	if ids.is_empty():
		return {}
	var horizontal := bool(context.get("horizontal", true))
	var rows: Array = []
	for id in ids:
		rows.append({"id": id, "rect": _layout_rect(document, context, id)})
	rows.sort_custom(func(a: Dictionary, b: Dictionary) -> bool:
		return a["rect"].position.x < b["rect"].position.x if horizontal \
			else a["rect"].position.y < b["rect"].position.y)
	var first: Rect2 = rows[0]["rect"]
	var last: Rect2 = rows[-1]["rect"]
	var start := first.position.x if horizontal else first.position.y
	var finish := last.end.x if horizontal else last.end.y
	var occupied := 0.0
	for row: Dictionary in rows:
		var rect: Rect2 = row["rect"]
		occupied += rect.size.x if horizontal else rect.size.y
	var gap := (finish - start - occupied) / float(rows.size() - 1)
	var cursor := start
	var edits: Array = []
	for row: Dictionary in rows:
		var id := int(row["id"])
		var rect: Rect2 = row["rect"]
		var position := rect.position
		if horizontal:
			position.x = cursor
			cursor += rect.size.x + gap
		else:
			position.y = cursor
			cursor += rect.size.y + gap
		edits.append({"id": id, "rect": _rect_preserving_auto(document, id,
			_layout_to_local_position(document, context, id, position))})
	var move_context := context.duplicate()
	move_context["edits"] = edits
	return _move_rects(document, move_context)


func _change_z_order(document: NovaMnuDocument, context: Dictionary) -> Dictionary:
	var ids := _selected_widgets_same_parent(document, context, 1)
	if ids.is_empty():
		return {}
	var parent := int(document.get_parent_id(ids[0]))
	var children: PackedInt32Array = document.get_child_ids(parent)
	var desired: Array[int] = []
	for child_id in children:
		desired.append(child_id)
	var mode := String(context.get("mode", ""))
	match mode:
		"front":
			desired = desired.filter(func(id: int) -> bool: return not ids.has(id))
			for id in children:
				if ids.has(id):
					desired.append(id)
		"back":
			var reordered: Array[int] = []
			for id in children:
				if ids.has(id):
					reordered.append(id)
			for id in children:
				if not ids.has(id):
					reordered.append(id)
			desired = reordered
		"forward":
			for index in range(desired.size() - 2, -1, -1):
				if ids.has(desired[index]) and not ids.has(desired[index + 1]):
					var swap := desired[index]
					desired[index] = desired[index + 1]
					desired[index + 1] = swap
		"backward":
			for index in range(1, desired.size()):
				if ids.has(desired[index]) and not ids.has(desired[index - 1]):
					var swap := desired[index]
					desired[index] = desired[index - 1]
					desired[index - 1] = swap
		_:
			return {}
	var unchanged := true
	for index in range(desired.size()):
		if desired[index] != children[index]:
			unchanged = false
			break
	if unchanged:
		return {}
	var before := document.capture_state()
	for target_index in range(desired.size()):
		var current: PackedInt32Array = document.get_child_ids(parent)
		var from := current.find(desired[target_index])
		if from != target_index:
			document.move_widget_to_index(desired[target_index], target_index)
	var selected := _selected_id(context)
	var result := _history_result("z_order_" + mode, before, selected, selected)
	result["selection"] = ids
	return result


func _duplicate_screen(document: NovaMnuDocument, context: Dictionary) -> Dictionary:
	var screen_id := _visible_screen_id(context)
	if screen_id < 0:
		return {"value": -1}
	var base := String(document.get_screen_name(screen_id)) + "_COPY"
	var name := base
	var suffix := 2
	while _screen_id_named(document, name) >= 0:
		name = "%s_%d" % [base, suffix]
		suffix += 1
	var before := document.capture_state()
	var copied := int(document.duplicate_screen(screen_id, name))
	var result := {"value": copied}
	if copied >= 0:
		result.merge(_history_result("duplicate_screen", before, screen_id, copied))
	return result


func _move_screen(document: NovaMnuDocument, context: Dictionary) -> Dictionary:
	var screen_id := _visible_screen_id(context)
	if screen_id < 0:
		return {}
	var screens: PackedInt32Array = document.get_screen_ids()
	var from := screens.find(screen_id)
	var to := clampi(from + int(context.get("delta", 0)), 0, screens.size() - 1)
	if from == to:
		return {}
	var before := document.capture_state()
	if not bool(document.move_screen_to_index(screen_id, to)):
		return {}
	return _history_result("move_screen", before, screen_id, screen_id)


func _unique_screen_name(document: NovaMnuDocument) -> String:
	var existing := {}
	for screen_id in document.get_screen_ids():
		existing[document.get_screen_name(screen_id)] = true
	if not existing.has("SCREEN"):
		return "SCREEN"
	var suffix := 2
	while existing.has("SCREEN_%d" % suffix):
		suffix += 1
	return "SCREEN_%d" % suffix


func _add_screen(document: NovaMnuDocument, context: Dictionary) -> Dictionary:
	var custom_name := String(context.get("name", ""))
	var selected := _selected_id(context)
	var before := document.capture_state()
	var screen_id := int(document.add_screen(custom_name if not custom_name.is_empty()
		else _unique_screen_name(document)))
	var result := {"value": screen_id}
	if screen_id >= 0:
		result.merge(_history_result("add_screen", before, selected, screen_id))
	return result


func _delete_screen(document: NovaMnuDocument, context: Dictionary) -> Dictionary:
	if document.get_screen_count() <= 1:
		return {}
	var screen_id := _visible_screen_id(context)
	if screen_id < 0:
		return {}
	var selected := _selected_id(context)
	var before := document.capture_state()
	document.delete_screen(screen_id)
	var first := int(document.get_screen_ids()[0]) \
		if document.get_screen_count() > 0 else -1
	return _history_result("delete_screen", before, selected, first)


func _screen_id_named(document: NovaMnuDocument, name: String) -> int:
	for screen_id in document.get_screen_ids():
		if String(document.get_screen_name(screen_id)).nocasecmp_to(name) == 0:
			return screen_id
	return -1


func _is_root_window(document: NovaMnuDocument, id: int) -> bool:
	if not document.widget_exists(id) or document.is_screen(id):
		return false
	var parent := int(document.get_parent_id(id))
	return parent > 0 and document.is_screen(parent)


func _resolve_existing_selection(document: NovaMnuDocument, id: int) -> int:
	if id >= 0 and document.widget_exists(id):
		return id
	return int(document.get_screen_ids()[0]) \
		if document.get_screen_count() > 0 else -1


func _write_widget_prop(document: NovaMnuDocument, id: int,
		prop: String, value) -> void:
	match prop:
		"name": document.set_widget_name(id, value)
		"rect": document.set_window_rect(id, value)
		"text": document.set_widget_text(id, value)
		"string_type": document.set_widget_string_type(id, value)
		"font": document.set_widget_font(id, value)
		"datasource": document.set_widget_datasource(id, value)
		"orientation": document.set_widget_orientation(id, value)
		"group": document.set_widget_group(id, int(value))
		"color": document.set_widget_color(id, -1, value)
		"texture": document.set_widget_texture(id, -1, value)
		"flags": document.set_widget_flags(id, int(value))
		"sounds": document.set_widget_sounds(id, value)
		"actions": document.set_widget_actions(id, value)
		"appearances": document.set_widget_appearances(id, value)
		"frame": document.set_window_frame(id, value)
		"table_count": document.set_table_column_count(id, int(value))
		"table_spacing": document.set_table_column_spacing(id, int(value))
