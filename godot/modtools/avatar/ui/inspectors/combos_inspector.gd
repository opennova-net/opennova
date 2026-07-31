extends WorkflowInspector

# Characters (combos) workflow: pick a nationality + division, list its combos,
# and edit each combo's id and its head/body/arms part references via dropdowns
# populated from the part names of each kind. Add / remove combo buttons mutate
# the division's combo list. Every commit reads db().get_model(), edits the
# matching division's combos array, and applies it through the workspace.
#
# A combo is `combo <id> <head> <body> <arms>` (docs/playerinfo/avatars-re.md):
# head + body are required, arms optional. The model keeps the three reference
# names so the writer can round-trip the line (D-PLAYERINFO-4).

const NONE_LABEL := "(none)"

var _nat_option: OptionButton
var _div_option: OptionButton
var _list_box: VBoxContainer
var _selected_nat := 0
var _selected_div := 0


func build_main(mount: Control) -> void:
	var box := _make_inspector_box(mount)

	var heading := Label.new()
	heading.text = "Characters"
	heading.theme_type_variation = &"Heading"
	box.add_child(heading)

	_nat_option = OptionButton.new()
	_nat_option.name = "ComboNationalityOption"
	_nat_option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(_nat_option)
	_nat_option.item_selected.connect(func(idx: int) -> void:
		_selected_nat = idx
		_selected_div = 0
		_populate_divisions()
		_rebuild_list())

	_div_option = OptionButton.new()
	_div_option.name = "ComboDivisionOption"
	_div_option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(_div_option)
	_div_option.item_selected.connect(func(idx: int) -> void:
		_selected_div = idx
		_rebuild_list())

	var add_button := Button.new()
	add_button.name = "ComboAddButton"
	add_button.text = "Add Character"
	box.add_child(add_button)
	add_button.pressed.connect(_add_combo)

	_list_box = VBoxContainer.new()
	_list_box.name = "ComboList"
	_list_box.add_theme_constant_override("separation", 10)
	_list_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(_list_box)

	_populate_nationalities()
	_populate_divisions()
	_rebuild_list()


func refresh() -> void:
	_populate_nationalities()
	_populate_divisions()
	_rebuild_list()


func _db():
	return _ws.db() if _ws != null else null


func _populate_nationalities() -> void:
	if _nat_option == null or not is_instance_valid(_nat_option):
		return
	_nat_option.clear()
	var database = _db()
	if database == null or not database.is_loaded():
		return
	for n in range(database.get_nationality_count()):
		var nat: Dictionary = database.get_nationality(n)
		_nat_option.add_item("%d  %s" % [n, String(nat.get("name_key", ""))])
	_selected_nat = clampi(_selected_nat, 0, maxi(database.get_nationality_count() - 1, 0))
	if _nat_option.item_count > 0:
		_nat_option.select(_selected_nat)


func _populate_divisions() -> void:
	if _div_option == null or not is_instance_valid(_div_option):
		return
	_div_option.clear()
	var database = _db()
	if database == null or not database.is_loaded():
		return
	for d in range(database.get_division_count(_selected_nat)):
		var div: Dictionary = database.get_division(_selected_nat, d)
		_div_option.add_item("%d  %s" % [d, String(div.get("name_key", ""))])
	_selected_div = clampi(_selected_div, 0, maxi(database.get_division_count(_selected_nat) - 1, 0))
	if _div_option.item_count > 0:
		_div_option.select(_selected_div)


func _part_names(kind: int) -> PackedStringArray:
	var database = _db()
	if database == null or not database.is_loaded():
		return PackedStringArray()
	return database.get_part_names(kind)


func _rebuild_list() -> void:
	if _list_box == null or not is_instance_valid(_list_box):
		return
	for child in _list_box.get_children():
		_list_box.remove_child(child)
		child.free()
	var database = _db()
	if database == null or not database.is_loaded():
		return
	if _div_option.item_count == 0:
		var empty := Label.new()
		empty.text = "This nationality has no divisions."
		empty.theme_type_variation = &"Muted"
		_list_box.add_child(empty)
		return
	var count: int = database.get_combo_count(_selected_nat, _selected_div)
	for c in range(count):
		_build_combo_card(database.get_combo(_selected_nat, _selected_div, c), c)


# One editable card per combo: id spin + head/body/arms dropdowns + remove button.
func _build_combo_card(combo: Dictionary, combo_index: int) -> void:
	var card := VBoxContainer.new()
	card.name = "ComboCard%d" % combo_index
	card.add_theme_constant_override("separation", 4)
	card.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_list_box.add_child(card)

	var id_row := HBoxContainer.new()
	var id_label := Label.new()
	id_label.text = "Id"
	id_label.custom_minimum_size = Vector2(64, 0)
	id_row.add_child(id_label)
	var id_spin := SpinBox.new()
	id_spin.name = "ComboIdSpin"
	id_spin.min_value = 0
	id_spin.max_value = 63
	id_spin.step = 1
	id_spin.value = float(int(combo.get("id", 0)))
	id_spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	id_row.add_child(id_spin)
	var remove := Button.new()
	remove.name = "ComboRemoveButton"
	remove.text = "Remove"
	id_row.add_child(remove)
	card.add_child(id_row)

	var head_opt := _build_part_dropdown(card, "Head", NovaAvatarDatabase.PART_HEAD, String(combo.get("head_name", "")), false)
	var body_opt := _build_part_dropdown(card, "Body", NovaAvatarDatabase.PART_BODY, String(combo.get("body_name", "")), false)
	var arms_opt := _build_part_dropdown(card, "Arms", NovaAvatarDatabase.PART_ARMS, String(combo.get("arms_name", "")), true)
	var issues := _combo_issues(combo)
	if not issues.is_empty():
		var issue := Label.new()
		issue.name = "ComboIssueLabel"
		issue.theme_type_variation = &"Muted"
		issue.text = "Issue: %s" % "; ".join(issues)
		issue.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		card.add_child(issue)

	var apply := Button.new()
	apply.name = "ComboApplyButton"
	apply.text = "Apply"
	card.add_child(apply)

	apply.pressed.connect(func() -> void:
		_commit_combo(combo_index, int(id_spin.value),
			_selected_part_name(head_opt), _selected_part_name(body_opt), _selected_part_name(arms_opt)))
	remove.pressed.connect(func() -> void: _remove_combo(combo_index))

	var sep := HSeparator.new()
	card.add_child(sep)


func _build_part_dropdown(card: VBoxContainer, label_text: String, kind: int, current: String, allow_none: bool) -> OptionButton:
	var row := HBoxContainer.new()
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size = Vector2(64, 0)
	row.add_child(label)
	var opt := OptionButton.new()
	opt.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var selected_index := 0
	var idx := 0
	if allow_none:
		opt.add_item(NONE_LABEL)
		if current.strip_edges().is_empty():
			selected_index = 0
		idx = 1
	for name in _part_names(kind):
		opt.add_item(String(name))
		if String(name) == current:
			selected_index = idx
		idx += 1
	if opt.item_count > 0:
		opt.select(clampi(selected_index, 0, opt.item_count - 1))
	row.add_child(opt)
	card.add_child(row)
	return opt


func _combo_issues(combo: Dictionary) -> PackedStringArray:
	var out := PackedStringArray()
	if not combo.has("head") or String((combo.get("head", {}) as Dictionary).get("name", "")).is_empty():
		out.append("head unresolved")
	if not combo.has("body") or String((combo.get("body", {}) as Dictionary).get("name", "")).is_empty():
		out.append("body unresolved")
	if not String(combo.get("arms_name", "")).is_empty() and not bool(combo.get("has_arms", false)):
		out.append("arms unresolved")
	return out


func _selected_part_name(opt: OptionButton) -> String:
	if opt == null or opt.selected < 0:
		return ""
	var text := opt.get_item_text(opt.selected)
	return "" if text == NONE_LABEL else text


# --- Mutations (read model, edit division combos, apply) ----------------------

# The selected division's Dictionary within `model`, or {} if the path is out of
# range. Returns the live reference, so mutating its "combos" array (ensured to
# exist) reflects back into `model` before apply_model.
func _division_dict(model: Dictionary) -> Dictionary:
	var nats: Array = model.get("nationalities", [])
	if _selected_nat < 0 or _selected_nat >= nats.size():
		return {}
	var divs: Array = (nats[_selected_nat] as Dictionary).get("divisions", [])
	if _selected_div < 0 or _selected_div >= divs.size():
		return {}
	var div: Dictionary = divs[_selected_div]
	if not div.has("combos"):
		div["combos"] = []
	return div


func _commit_combo(combo_index: int, id: int, head: String, body: String, arms: String) -> void:
	var database = _db()
	if database == null or _ws == null:
		return
	var model: Dictionary = database.get_model()
	var div := _division_dict(model)
	if div.is_empty():
		return
	var combos: Array = div["combos"]
	if combo_index < 0 or combo_index >= combos.size():
		return
	var entry: Dictionary = combos[combo_index]
	entry["id"] = id
	entry["head_name"] = head
	entry["body_name"] = body
	entry["arms_name"] = arms
	_ws.apply_model(model)
	_rebuild_list()


func _add_combo() -> void:
	var database = _db()
	if database == null or _ws == null or not database.is_loaded():
		return
	var model: Dictionary = database.get_model()
	var div := _division_dict(model)
	if div.is_empty():
		return
	var combos: Array = div["combos"]
	# Seed with the first head + body (required); arms optional/empty. The next
	# free id is one past the current max so it does not collide.
	var heads := _part_names(NovaAvatarDatabase.PART_HEAD)
	var bodies := _part_names(NovaAvatarDatabase.PART_BODY)
	var next_id := 0
	for c in combos:
		next_id = maxi(next_id, int((c as Dictionary).get("id", 0)) + 1)
	combos.append({
		"id": next_id,
		"head_name": String(heads[0]) if heads.size() > 0 else "",
		"body_name": String(bodies[0]) if bodies.size() > 0 else "",
		"arms_name": "",
	})
	_ws.apply_model(model)
	_rebuild_list()


func _remove_combo(combo_index: int) -> void:
	var database = _db()
	if database == null or _ws == null:
		return
	var model: Dictionary = database.get_model()
	var div := _division_dict(model)
	if div.is_empty():
		return
	var combos: Array = div["combos"]
	if combo_index < 0 or combo_index >= combos.size():
		return
	combos.remove_at(combo_index)
	_ws.apply_model(model)
	_rebuild_list()
