class_name AnimFramesDialog
extends AcceptDialog

const TextureSlotWidgetScript = preload("res://modtools/object/ui/widgets/texture_slot_widget.gd")

signal frame_changed()

var _data: NovaObjectData
var _material_index := -1
var _shader_info := {}
var _content: VBoxContainer


func _init() -> void:
	title = "Animated frames"
	min_size = Vector2i(560, 480)
	get_ok_button().text = "Close"


func setup(data: NovaObjectData, material_index: int, shader_info: Dictionary) -> void:
	_data = data
	_material_index = material_index
	_shader_info = shader_info
	_rebuild()


func _rebuild() -> void:
	if _content == null:
		_content = VBoxContainer.new()
		_content.add_theme_constant_override("separation", 8)
		add_child(_content)
	for child in _content.get_children():
		_content.remove_child(child)
		child.queue_free()

	if _data == null or _material_index < 0:
		_add_message("No material loaded.")
		return
	var info := _data.get_material_info(_material_index)
	var frame_count := int(info.get("anim_frames", 0))
	if frame_count <= 0:
		_add_message("This material has no animated frames.")
		return

	var summary := Label.new()
	summary.text = "%d frames, type %d, frame time/register %d" % [
		frame_count,
		int(info.get("anim_type", 0)),
		int(info.get("anim_frame_time", 0)),
	]
	_content.add_child(summary)

	for slot in _animated_slots():
		_add_slot_section(slot, frame_count)


func _add_message(text: String) -> void:
	var label := Label.new()
	label.text = text
	_content.add_child(label)


func _animated_slots() -> Array:
	var slots: Array = [1]
	if bool(_shader_info.get("has_secondary", false)):
		slots.append(2)
	if bool(_shader_info.get("has_normal_a", false)):
		slots.append(3)
	if bool(_shader_info.get("has_normal_b", false)):
		slots.append(4)
	return slots


func _add_slot_section(slot: int, frame_count: int) -> void:
	var header := Label.new()
	header.text = _slot_label(slot)
	_content.add_child(header)
	var frames := _data.get_material_anim_frames(_material_index, slot)
	for frame_index in range(frame_count):
		var row := HBoxContainer.new()
		row.add_theme_constant_override("separation", 8)
		var label := Label.new()
		label.text = "Frame %d" % frame_index
		label.custom_minimum_size = Vector2(80, 0)
		row.add_child(label)
		var widget = TextureSlotWidgetScript.new()
		widget.configure(slot)
		widget.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		widget.set_value(String(frames[frame_index]) if frame_index < frames.size() else "")
		widget.value_changed.connect(func(_slot_id: int, filename: String, p_slot: int = slot, p_frame: int = frame_index) -> void:
			if _data != null and _data.set_material_anim_frame(_material_index, p_slot, p_frame, filename):
				frame_changed.emit()
		)
		row.add_child(widget)
		_content.add_child(row)


func _slot_label(slot: int) -> String:
	match slot:
		1:
			return "Diffuse"
		2:
			return "Detail"
		3:
			return "Normal A"
		4:
			return "Normal B"
		_:
			return "Slot %d" % slot
