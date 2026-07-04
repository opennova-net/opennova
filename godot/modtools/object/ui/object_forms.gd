class_name ObjectForms
extends RefCounted

## Object-workspace-only form builders (B7): the ctrl-reg picker rows depend
## on the object widget set, so they stay in object/ui/ while the generic
## builders live in framework/inspector_forms.gd (InspectorForms).

const CtrlRegPickerScript = preload("res://modtools/object/ui/widgets/ctrl_reg_picker.gd")


static func add_ctrl_reg_row(parent: Control, node_name: String, label_text: String, control_registers: Array):
	var row := HBoxContainer.new()
	row.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	parent.add_child(row)
	var label := Label.new()
	label.text = label_text
	label.tooltip_text = label_text
	label.clip_text = true
	label.custom_minimum_size = Vector2(InspectorForms.LABEL_COL_WIDTH, 0)
	row.add_child(label)
	var picker = CtrlRegPickerScript.new()
	picker.name = node_name
	picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(picker)
	picker.setup(control_registers, -1)
	return picker


static func add_detail_ctrl_reg_row(parent: Control, node_name: String, label_text: String, control_registers: Array):
	var row := InspectorForms.add_detail_field(parent, label_text)
	var picker = CtrlRegPickerScript.new()
	picker.name = node_name
	picker.fit_to_longest_item = false
	picker.custom_minimum_size = Vector2(0, 32)
	picker.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	row.add_child(picker)
	picker.setup(control_registers, -1)
	return picker
