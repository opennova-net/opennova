class_name MnuStringPicker
extends PopupPanel

# A searchable picker over an RTXT string table: type to filter by key or resolved
# text, then double-click / Enter / "Use selected" to choose. Emits picked(key) with
# the chosen entry's key and hides. Self-contained (depends only on an RtxtStringFile)
# so it can be reused wherever a widget references a string id. The mount adds it as a
# child once, then calls open_for(text_resource, current_key) and listens for picked.
#
# Populating (set_table / filter) is split from showing (open_for) so the filtering
# and choose logic is unit-testable without spawning a window.

signal picked(key: String)

const PICKER_SIZE := Vector2i(380, 440)

var _text: RtxtStringFile
var _keys: PackedStringArray = PackedStringArray()  # all keys, sorted
var _filter_edit: LineEdit
var _list: ItemList


func _ready() -> void:
	var box := VBoxContainer.new()
	box.set_anchors_preset(Control.PRESET_FULL_RECT)
	box.add_theme_constant_override("separation", 6)
	add_child(box)

	_filter_edit = SearchField.new("Filter strings (key or text)...")
	_filter_edit.search_changed.connect(func(_t: String) -> void: _rebuild_list())
	_filter_edit.text_submitted.connect(func(_t: String) -> void: _confirm_first_or_selected())
	box.add_child(_filter_edit)

	_list = ItemList.new()
	_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_list.item_activated.connect(_emit_index)
	box.add_child(_list)

	var use_btn := Button.new()
	use_btn.text = "Use selected"
	use_btn.pressed.connect(_confirm_first_or_selected)
	box.add_child(use_btn)


# Populate from a string table and (optionally) pre-select current_key. No window.
func set_table(text_resource: RtxtStringFile, current_key: String = "") -> void:
	_text = text_resource
	_keys = _text.get_keys() if _text != null else PackedStringArray()
	_keys.sort()
	if _filter_edit != null:
		_filter_edit.text = ""
	_rebuild_list()
	_preselect(current_key)


# Populate, show centered, and focus the filter for immediate typing.
func open_for(text_resource: RtxtStringFile, current_key: String = "") -> void:
	set_table(text_resource, current_key)
	popup_centered(PICKER_SIZE)
	if _filter_edit != null:
		_filter_edit.grab_focus()


# --- Test / programmatic seams (no window) --------------------------------------

func filter(text: String) -> void:
	if _filter_edit != null:
		_filter_edit.text = text
	_rebuild_list()


func row_count() -> int:
	return _list.item_count if _list != null else 0


func key_at(index: int) -> String:
	if _list == null or index < 0 or index >= _list.item_count:
		return ""
	return String(_list.get_item_metadata(index))


func choose(index: int) -> void:
	_emit_index(index)


# --- Internal -------------------------------------------------------------------

func _rebuild_list() -> void:
	if _list == null:
		return
	_list.clear()
	var needle := _filter_edit.text.strip_edges().to_lower() if _filter_edit != null else ""
	for key in _keys:
		var text := _text.get_string(key) if _text != null else ""
		if not needle.is_empty() and not (key.to_lower().contains(needle) or text.to_lower().contains(needle)):
			continue
		var label := key if text.is_empty() else "%s    %s" % [key, text]
		var idx := _list.add_item(label)
		_list.set_item_metadata(idx, key)


func _preselect(current_key: String) -> void:
	if _list == null or current_key.is_empty():
		return
	for i in range(_list.item_count):
		if String(_list.get_item_metadata(i)) == current_key:
			_list.select(i)
			_list.ensure_current_is_visible()
			return


func _confirm_first_or_selected() -> void:
	if _list == null:
		return
	var sel := _list.get_selected_items()
	if not sel.is_empty():
		_emit_index(sel[0])
	elif _list.item_count > 0:
		_emit_index(0)


func _emit_index(index: int) -> void:
	if _list == null or index < 0 or index >= _list.item_count:
		return
	picked.emit(String(_list.get_item_metadata(index)))
	hide()
