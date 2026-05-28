extends RefCounted

## Base class for the object editor's per-workflow inspectors.
##
## Holds a reference to the ObjectEditorWorkspace coordinator and exposes the
## shared editor state (object_editor, _preview), small coordinator callbacks,
## and thin wrappers over ObjectUiHelpers. A build method moved out of the
## workspace into a subclass reads the same as it did before: shared object
## references resolve through the coordinator, and the _add_* helpers are
## inherited from here.

var _ws  # ObjectEditorWorkspace coordinator

var object_editor: ObjectEditor:
	get:
		return _ws.object_editor

var _preview: ObjectPreview:
	get:
		return _ws._preview


func _init(workspace) -> void:
	_ws = workspace


# --- Build hooks (overridden by subclasses) ---
func build_main(_host: Control) -> void:
	pass


func build_detail(_box: VBoxContainer) -> void:
	pass


func refresh() -> void:
	pass


# --- Shared editor accessors / coordinator callbacks ---
func _object_data():
	return object_editor.object_data if object_editor != null else null


func _control_registers() -> Array:
	var data = _object_data()
	if data != null and data.has_method("get_control_registers"):
		return data.get_control_registers()
	return []


func _rebuild_detail_dock() -> void:
	_ws._rebuild_object_detail_dock()


func _notify_shell() -> void:
	_ws._sync_shell()


# --- UI helper wrappers (delegate to ObjectUiHelpers) ---
func _add_spin_row(parent: Control, node_name: String, label_text: String, min_value: float, max_value: float, step: float) -> SpinBox:
	return ObjectUiHelpers.add_spin_row(parent, node_name, label_text, min_value, max_value, step)


func _add_color_row(parent: Control, node_name: String, label_text: String) -> ColorPickerButton:
	return ObjectUiHelpers.add_color_row(parent, node_name, label_text)


func _add_id_option_row(parent: Control, node_name: String, label_text: String, options: Array) -> OptionButton:
	return ObjectUiHelpers.add_id_option_row(parent, node_name, label_text, options)


func _add_ctrl_reg_row(parent: Control, node_name: String, label_text: String):
	return ObjectUiHelpers.add_ctrl_reg_row(parent, node_name, label_text, _control_registers())


func _add_detail_field(parent: Control, label_text: String) -> VBoxContainer:
	return ObjectUiHelpers.add_detail_field(parent, label_text)


func _add_detail_spin_row(parent: Control, node_name: String, label_text: String, min_value: float, max_value: float, step: float) -> SpinBox:
	return ObjectUiHelpers.add_detail_spin_row(parent, node_name, label_text, min_value, max_value, step)


func _add_detail_id_option_row(parent: Control, node_name: String, label_text: String, options: Array) -> OptionButton:
	return ObjectUiHelpers.add_detail_id_option_row(parent, node_name, label_text, options)


func _add_detail_ctrl_reg_row(parent: Control, node_name: String, label_text: String):
	return ObjectUiHelpers.add_detail_ctrl_reg_row(parent, node_name, label_text, _control_registers())


func _populate_id_option(option: OptionButton, options: Array, current_id: int) -> void:
	ObjectUiHelpers.populate_id_option(option, options, current_id)


func _selected_option_id(option: OptionButton) -> int:
	return ObjectUiHelpers.selected_option_id(option)


func _set_spin(node, value: float) -> void:
	ObjectUiHelpers.set_spin(node, value)


func _build_channel_card(parent: VBoxContainer, node_name: String) -> VBoxContainer:
	return ObjectUiHelpers.build_channel_card(parent, node_name)


func _make_inspector_box(host: Control) -> VBoxContainer:
	return ObjectUiHelpers.make_inspector_box(host)
