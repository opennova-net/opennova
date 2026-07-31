extends WorkflowInspector

# Tree workflow: a read-only nationality -> division -> character (combo) tree
# mirroring the Avatars.def hierarchy. Rows show the RTXT name key the .def
# references ([orig: "Avatars" string table, PlayerInfo_Populate* @ 0x55d8c0]);
# a `(skipdemo)`-style flag tag is appended when a nationality/division carries
# one. Selecting a character row shows that combo in the 3D preview.
#
# This inspector reads the workspace coordinator directly through _ws; it does not
# use the object-editor accessors on the WorkflowInspector base.

var _tree: Tree
var _issue_label: Label


func build_main(mount: Control) -> void:
	var box := _make_inspector_box(mount)

	var heading := Label.new()
	heading.text = "Characters"
	heading.theme_type_variation = &"Heading"
	box.add_child(heading)

	_issue_label = Label.new()
	_issue_label.name = "AvatarIssueSummary"
	_issue_label.theme_type_variation = &"Muted"
	_issue_label.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	_issue_label.visible = false
	box.add_child(_issue_label)

	_tree = Tree.new()
	_tree.name = "AvatarTree"
	_tree.hide_root = true
	_tree.select_mode = Tree.SELECT_SINGLE
	_tree.custom_minimum_size = Vector2(0, 280)
	_tree.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_tree.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_child(_tree)
	_tree.item_selected.connect(_on_item_selected)
	_populate()


func refresh() -> void:
	_populate()


func _db():
	return _ws.db() if _ws != null else null


func _populate() -> void:
	if _tree == null or not is_instance_valid(_tree):
		return
	_tree.clear()
	var database = _db()
	var root := _tree.create_item()
	if database == null or not database.is_loaded():
		if _issue_label != null:
			_issue_label.visible = false
		var empty := _tree.create_item(root)
		empty.set_text(0, "No Avatars.def loaded")
		empty.set_selectable(0, false)
		return
	_update_issue_summary(database)
	for n in range(database.get_nationality_count()):
		var nat: Dictionary = database.get_nationality(n)
		var nat_item := _tree.create_item(root)
		nat_item.set_text(0, _row_label(String(nat.get("name_key", "")), String(nat.get("flags", ""))))
		nat_item.set_selectable(0, false)
		# Carry no combo meta on nationality/division rows (not previewable).
		nat_item.set_metadata(0, {})
		for d in range(database.get_division_count(n)):
			var div: Dictionary = database.get_division(n, d)
			var div_item := _tree.create_item(nat_item)
			div_item.set_text(0, _row_label(String(div.get("name_key", "")), String(div.get("flags", ""))))
			div_item.set_selectable(0, false)
			div_item.set_metadata(0, {})
			for c in range(database.get_combo_count(n, d)):
				var combo: Dictionary = database.get_combo(n, d, c)
				var combo_item := _tree.create_item(div_item)
				combo_item.set_text(0, _combo_label(combo))
				combo_item.set_metadata(0, {"nat": n, "div": d, "combo": c})
				combo_item.set_tooltip_text(0, _combo_tooltip(combo))


# A nationality/division row label: its RTXT key, with a flag tag appended when
# the entry carries flags (e.g. `(skipdemo)`).
func _row_label(name_key: String, flags: String) -> String:
	var label := name_key if not name_key.is_empty() else "(unnamed)"
	if not flags.strip_edges().is_empty():
		label += "  (%s)" % flags.strip_edges()
	return label


# A combo row label: the head/body part names that form the character
# ("last - first" in the original menu, here the two part identifiers).
func _combo_label(combo: Dictionary) -> String:
	var head := String(combo.get("head_name", ""))
	var body := String(combo.get("body_name", ""))
	if head.is_empty() and body.is_empty():
		return "Character %d" % int(combo.get("id", 0))
	return "%s — %s" % [head, body]


func _combo_tooltip(combo: Dictionary) -> String:
	var issues := PackedStringArray()
	if not combo.has("head") or String((combo.get("head", {}) as Dictionary).get("name", "")).is_empty():
		issues.append("Head is unresolved")
	if not combo.has("body") or String((combo.get("body", {}) as Dictionary).get("name", "")).is_empty():
		issues.append("Body is unresolved")
	if not String(combo.get("arms_name", "")).is_empty() and not bool(combo.get("has_arms", false)):
		issues.append("Arms are unresolved")
	return "\n".join(issues)


func _update_issue_summary(database) -> void:
	if _issue_label == null or not is_instance_valid(_issue_label):
		return
	var diagnostics: Array = database.get_diagnostics() if database.has_method("get_diagnostics") else []
	if diagnostics.is_empty():
		_issue_label.visible = false
		return
	var first: Dictionary = diagnostics[0]
	_issue_label.text = "%d issue%s. First: %s" % [
		diagnostics.size(),
		"" if diagnostics.size() == 1 else "s",
		String(first.get("message", first.get("code", ""))),
	]
	_issue_label.visible = true


func _on_item_selected() -> void:
	if _tree == null or not is_instance_valid(_tree):
		return
	var item := _tree.get_selected()
	if item == null:
		return
	var meta: Variant = item.get_metadata(0)
	if not (meta is Dictionary) or not (meta as Dictionary).has("combo"):
		return
	var m: Dictionary = meta
	if _ws != null:
		_ws.show_combo(int(m["nat"]), int(m["div"]), int(m["combo"]))


func focus_combo(nat_index: int, div_index: int, combo_index: int) -> Error:
	if _tree == null or not is_instance_valid(_tree):
		return OK
	var root := _tree.get_root()
	if root == null:
		return OK
	var item := _find_combo_item(root.get_first_child(), nat_index, div_index, combo_index)
	if item == null:
		return ERR_DOES_NOT_EXIST
	item.select(0)
	_tree.scroll_to_item(item)
	if _ws != null:
		_ws.show_combo(nat_index, div_index, combo_index)
	return OK


func _find_combo_item(item: TreeItem, nat_index: int, div_index: int, combo_index: int) -> TreeItem:
	var cursor := item
	while cursor != null:
		var meta: Variant = cursor.get_metadata(0)
		if meta is Dictionary:
			var d: Dictionary = meta
			if int(d.get("nat", -1)) == nat_index and int(d.get("div", -1)) == div_index \
					and int(d.get("combo", -1)) == combo_index:
				return cursor
		var child := _find_combo_item(cursor.get_first_child(), nat_index, div_index, combo_index)
		if child != null:
			return child
		cursor = cursor.get_next()
	return null
