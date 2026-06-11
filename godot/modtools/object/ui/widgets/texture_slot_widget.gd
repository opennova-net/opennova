class_name TextureSlotWidget
extends HBoxContainer

signal value_changed(slot_id: int, filename: String)
signal browse_requested(slot_id: int)

var slot_id := 0
var thumb: TexturePreviewBox
var line_edit: LineEdit
var browse_button: Button
var clear_button: Button
var _current := ""


func _init() -> void:
	add_theme_constant_override("separation", 4)
	thumb = TexturePreviewBox.new()
	thumb.set_box_size(Vector2(40, 40))
	add_child(thumb)
	line_edit = LineEdit.new()
	line_edit.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	line_edit.placeholder_text = "Filename in object folder"
	line_edit.text_submitted.connect(_commit)
	add_child(line_edit)

	browse_button = Button.new()
	browse_button.text = "..."
	browse_button.tooltip_text = "Choose a texture from the object folder."
	browse_button.custom_minimum_size = Vector2(34, 30)
	browse_button.pressed.connect(func() -> void: browse_requested.emit(slot_id))
	add_child(browse_button)

	clear_button = Button.new()
	clear_button.text = "X"
	clear_button.tooltip_text = "Clear texture"
	clear_button.custom_minimum_size = Vector2(30, 30)
	clear_button.pressed.connect(func() -> void: _commit(""))
	add_child(clear_button)


func configure(p_slot_id: int) -> void:
	slot_id = p_slot_id
	thumb.name = "TextureSlot%dThumb" % slot_id
	line_edit.name = "TextureSlot%dName" % slot_id
	browse_button.name = "TextureSlot%dBrowse" % slot_id
	clear_button.name = "TextureSlot%dClear" % slot_id


## loader(name) -> Texture2D feeds the thumbnail (memoized by name inside the
## preview box — texture decode is not cached upstream).
func set_preview_loader(loader: Callable) -> void:
	thumb.set_loader(loader)


func set_value(filename: String) -> void:
	_current = filename
	line_edit.text = filename
	thumb.show_name(filename)


func set_slot_enabled(enabled: bool) -> void:
	line_edit.editable = enabled
	browse_button.disabled = not enabled
	clear_button.disabled = not enabled or _current.is_empty()
	thumb.modulate = Color(1, 1, 1, 1.0 if enabled else 0.45)


func set_placeholder(text: String) -> void:
	line_edit.placeholder_text = text


func _commit(filename: String) -> void:
	if filename == _current:
		return
	_current = filename
	line_edit.text = filename
	clear_button.disabled = filename.is_empty()
	thumb.show_name(filename)
	value_changed.emit(slot_id, filename)
