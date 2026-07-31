extends WorkflowInspector

# Parts workflow: edit the head / body / arms parts of the Avatars.def. A kind
# selector picks the slot; a list shows that kind's parts; a detail form edits the
# selected part's fields (name, display name, the three .3di graphics, camo, voice,
# sex). Commits read the whole model via db().get_model(), mutate the matching
# part entry, and apply it back through the workspace (set_model -> dirty).
#
# Part fields mirror the witnessed AvatarPart struct (docs/playerinfo/avatars-re.md):
# +0 name, +36 display key, +48 graphic, +64 graphic_j, +80 graphic_s, +68 camo,
# +71 voice, +72 sex. `graphic_d` aliases `graphic` (D-PLAYERINFO-3), so it is not
# a separate field here.

const KINDS := ["head", "body", "arms"]

var _kind_option: OptionButton
var _list: ItemList
var _detail_box: VBoxContainer
var _selected_kind := 0  # NovaAvatarDatabase.PART_HEAD
var _selected_index := -1

# Detail fields (rebuilt per selection).
var _name_edit: LineEdit
var _display_edit: StringRefWidget
var _graphic_edit: ResourceRefWidget
var _graphic_j_edit: ResourceRefWidget
var _graphic_s_edit: ResourceRefWidget
var _camo_r: SpinBox
var _camo_g: SpinBox
var _camo_b: SpinBox
var _voice_spin: SpinBox
var _sex_option: OptionButton


func build_main(mount: Control) -> void:
	var box := _make_inspector_box(mount)

	var heading := Label.new()
	heading.text = "Parts"
	heading.theme_type_variation = &"Heading"
	box.add_child(heading)

	_kind_option = OptionButton.new()
	_kind_option.name = "PartKindOption"
	_kind_option.add_item("Head")
	_kind_option.add_item("Body")
	_kind_option.add_item("Arms")
	_kind_option.select(_selected_kind)
	box.add_child(_kind_option)
	_kind_option.item_selected.connect(func(idx: int) -> void:
		_selected_kind = idx
		_selected_index = -1
		_refresh_list()
		_rebuild_detail())

	_list = ItemList.new()
	_list.name = "PartList"
	_list.custom_minimum_size = Vector2(0, 160)
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(_list)
	_list.item_selected.connect(func(idx: int) -> void:
		_selected_index = idx
		_rebuild_detail())

	_detail_box = VBoxContainer.new()
	_detail_box.name = "PartDetail"
	_detail_box.add_theme_constant_override("separation", 6)
	_detail_box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(_detail_box)

	_refresh_list()
	_rebuild_detail()


func refresh() -> void:
	_refresh_list()
	_rebuild_detail()


func _db():
	return _ws.db() if _ws != null else null


func _kind_const() -> int:
	# Maps the option index to NovaAvatarDatabase.PART_* (head=0, body=1, arms=2),
	# which match KINDS order.
	return _selected_kind


func _part_names() -> PackedStringArray:
	var database = _db()
	if database == null or not database.is_loaded():
		return PackedStringArray()
	return database.get_part_names(_kind_const())


func _refresh_list() -> void:
	if _list == null or not is_instance_valid(_list):
		return
	_list.clear()
	var names := _part_names()
	for n in names:
		_list.add_item(String(n))
	if names.is_empty():
		_selected_index = -1
	else:
		_selected_index = clampi(_selected_index, 0, names.size() - 1)
		_list.select(_selected_index)


func _selected_part() -> Dictionary:
	var database = _db()
	var names := _part_names()
	if database == null or _selected_index < 0 or _selected_index >= names.size():
		return {}
	return database.get_part(_kind_const(), String(names[_selected_index]))


func _rebuild_detail() -> void:
	if _detail_box == null or not is_instance_valid(_detail_box):
		return
	for child in _detail_box.get_children():
		_detail_box.remove_child(child)
		child.free()
	_name_edit = null
	var part := _selected_part()
	if part.is_empty():
		var empty := Label.new()
		empty.text = "Select a part to edit."
		empty.theme_type_variation = &"Muted"
		_detail_box.add_child(empty)
		return

	_name_edit = _add_text_field("Name", String(part.get("name", "")))
	_display_edit = _add_string_ref_field("Display name", String(part.get("display_name", "")))
	_graphic_edit = _add_graphic_field("Graphic", String(part.get("graphic", "")))
	_graphic_j_edit = _add_graphic_field("Graphic (J)", String(part.get("graphic_j", "")), "PartGraphicJRef")
	_graphic_s_edit = _add_graphic_field("Graphic (S)", String(part.get("graphic_s", "")), "PartGraphicSRef")

	var camo: Array = part.get("camo", [0, 0, 0])
	var camo_row := HBoxContainer.new()
	camo_row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var camo_label := Label.new()
	camo_label.text = "Camo"
	camo_label.custom_minimum_size = Vector2(96, 0)
	camo_row.add_child(camo_label)
	_camo_r = _make_byte_spin(int(camo[0]) if camo.size() > 0 else 0)
	_camo_g = _make_byte_spin(int(camo[1]) if camo.size() > 1 else 0)
	_camo_b = _make_byte_spin(int(camo[2]) if camo.size() > 2 else 0)
	camo_row.add_child(_camo_r)
	camo_row.add_child(_camo_g)
	camo_row.add_child(_camo_b)
	_detail_box.add_child(camo_row)

	var voice_row := HBoxContainer.new()
	var voice_label := Label.new()
	voice_label.text = "Voice"
	voice_label.custom_minimum_size = Vector2(96, 0)
	voice_row.add_child(voice_label)
	_voice_spin = SpinBox.new()
	_voice_spin.name = "PartVoiceSpin"
	_voice_spin.min_value = 0
	_voice_spin.max_value = 255
	_voice_spin.step = 1
	_voice_spin.value = float(int(part.get("voice", 0)))
	_voice_spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	voice_row.add_child(_voice_spin)
	_detail_box.add_child(voice_row)

	var sex_row := HBoxContainer.new()
	var sex_label := Label.new()
	sex_label.text = "Sex"
	sex_label.custom_minimum_size = Vector2(96, 0)
	sex_row.add_child(sex_label)
	_sex_option = OptionButton.new()
	_sex_option.name = "PartSexOption"
	_sex_option.add_item("Male")
	_sex_option.add_item("Female")
	_sex_option.select(clampi(int(part.get("sex", 0)), 0, 1))
	_sex_option.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	sex_row.add_child(_sex_option)
	_detail_box.add_child(sex_row)

	var commit := Button.new()
	commit.name = "PartCommitButton"
	commit.text = "Apply Part"
	_detail_box.add_child(commit)
	commit.pressed.connect(_commit_part)


func _add_text_field(label_text: String, value: String) -> LineEdit:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size = Vector2(96, 0)
	row.add_child(label)
	var edit := LineEdit.new()
	edit.text = value
	edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(edit)
	_detail_box.add_child(row)
	return edit


# An Avatars display-name key. The key resolves through the "Avatars" string
# table at runtime; the widget still degrades to a plain key field when no string
# table service is available in headless tests.
func _add_string_ref_field(label_text: String, value: String) -> StringRefWidget:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size = Vector2(96, 0)
	row.add_child(label)
	var widget := StringRefWidget.new()
	widget.name = "PartDisplayRef"
	widget.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	widget.configure("Avatar string")
	widget.set_value(value)
	row.add_child(widget)
	_detail_box.add_child(row)
	return widget


func _add_graphic_field(label_text: String, value: String, node_name: String = "PartGraphicRef") -> ResourceRefWidget:
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	var label := Label.new()
	label.text = label_text
	label.custom_minimum_size = Vector2(96, 0)
	row.add_child(label)
	var widget := ResourceRefWidget.new()
	widget.name = node_name
	widget.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	widget.set_value_from_path(func(path: String) -> String: return path.get_file())
	widget.configure("object_model", label_text, _resource_ref_services())
	widget.set_value(value)
	row.add_child(widget)
	_detail_box.add_child(row)
	return widget


func _make_byte_spin(value: int) -> SpinBox:
	var spin := SpinBox.new()
	spin.min_value = 0
	spin.max_value = 255
	spin.step = 1
	spin.value = float(clampi(value, 0, 255))
	spin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	return spin


# Read the whole model, replace the matching part entry's fields, and apply it
# through the workspace. The match is by (kind, original name) — editing the name
# field renames in place, since the entry is found by its pre-edit identity.
func _commit_part() -> void:
	var database = _db()
	if database == null or _ws == null or _name_edit == null:
		return
	var names := _part_names()
	if _selected_index < 0 or _selected_index >= names.size():
		return
	var original_name := String(names[_selected_index])
	var model: Dictionary = database.get_model()
	var parts: Array = model.get("parts", [])
	for entry_v in parts:
		var entry: Dictionary = entry_v
		if int(entry.get("kind", -1)) == _kind_const() and String(entry.get("name", "")) == original_name:
			entry["name"] = _name_edit.text.strip_edges()
			entry["display_name"] = _display_edit.get_value().strip_edges()
			entry["graphic"] = _graphic_edit.get_value().strip_edges()
			entry["graphic_j"] = _graphic_j_edit.get_value().strip_edges()
			entry["graphic_s"] = _graphic_s_edit.get_value().strip_edges()
			entry["camo"] = [int(_camo_r.value), int(_camo_g.value), int(_camo_b.value)]
			entry["voice"] = int(_voice_spin.value)
			entry["sex"] = _sex_option.get_selected_id() if _sex_option.get_selected_id() >= 0 else _sex_option.selected
			break
	_ws.apply_model(model)
	_refresh_list()


func focus_part(kind: int, name: String) -> Error:
	_selected_kind = clampi(kind, 0, KINDS.size() - 1)
	var names := _part_names()
	var found := -1
	for i in range(names.size()):
		if String(names[i]).nocasecmp_to(name) == 0:
			found = i
			break
	if found < 0:
		return ERR_DOES_NOT_EXIST
	_selected_index = found
	if _kind_option != null and is_instance_valid(_kind_option):
		_kind_option.select(_selected_kind)
	_refresh_list()
	_rebuild_detail()
	return OK


func _resource_ref_services() -> Dictionary:
	var shell: Variant = _ws.editor_shell if _ws != null else null
	if shell != null and shell.has_method("get_reference_index") \
			and shell.has_method("open_kind_picker") \
			and shell.has_method("open_in_workspace"):
		return ResourceRefWidget.services_from_shell(shell)
	return {}
