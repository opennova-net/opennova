class_name WorkflowInspector
extends RefCounted

## Base class for per-workflow inspectors, workspace-agnostic (B7): an untyped
## coordinator reference, the build/refresh contract, and thin wrappers over
## InspectorForms. Coordinator callbacks default to no-ops; a workspace whose
## coordinator offers them re-binds in a thin domain base (ObjectInspector /
## ObjectListDetailInspector re-add the object accessors; the terrain
## inspectors wire a TerrainEditor instead of a coordinator).

var _ws  # The owning workspace coordinator, if any.


func _init(workspace = null) -> void:
	_ws = workspace


# --- Build hooks (overridden by subclasses) ---
func build_main(_mount: Control) -> void:
	pass


func build_detail(_box: VBoxContainer) -> void:
	pass


func refresh() -> void:
	pass


# --- Coordinator callbacks (no-op defaults; domain bases re-bind them) ---
func _rebuild_detail_dock() -> void:
	pass


func _notify_shell() -> void:
	pass


# --- UI helper wrappers (delegate to InspectorForms) ---
func _add_spin_row(parent: Control, node_name: String, label_text: String, min_value: float, max_value: float, step: float) -> SpinBox:
	return InspectorForms.add_spin_row(parent, node_name, label_text, min_value, max_value, step)


func _add_color_row(parent: Control, node_name: String, label_text: String) -> ColorPickerButton:
	return InspectorForms.add_color_row(parent, node_name, label_text)


func _add_checkbox(parent: Control, node_name: String, text: String) -> CheckBox:
	return InspectorForms.add_checkbox(parent, node_name, text)


func _add_id_option_row(parent: Control, node_name: String, label_text: String, options: Array) -> OptionButton:
	return InspectorForms.add_id_option_row(parent, node_name, label_text, options)


func _add_detail_field(parent: Control, label_text: String) -> VBoxContainer:
	return InspectorForms.add_detail_field(parent, label_text)


func _add_detail_spin_row(parent: Control, node_name: String, label_text: String, min_value: float, max_value: float, step: float) -> SpinBox:
	return InspectorForms.add_detail_spin_row(parent, node_name, label_text, min_value, max_value, step)


func _add_detail_id_option_row(parent: Control, node_name: String, label_text: String, options: Array) -> OptionButton:
	return InspectorForms.add_detail_id_option_row(parent, node_name, label_text, options)


func _populate_id_option(option: OptionButton, options: Array, current_id: int) -> void:
	InspectorForms.populate_id_option(option, options, current_id)


func _selected_option_id(option: OptionButton) -> int:
	return InspectorForms.selected_option_id(option)


func _set_spin(node, value: float) -> void:
	InspectorForms.set_spin(node, value)


func _build_channel_card(parent: VBoxContainer, node_name: String) -> VBoxContainer:
	return InspectorForms.build_channel_card(parent, node_name)


func _make_inspector_box(mount: Control) -> VBoxContainer:
	return InspectorForms.make_inspector_box(mount)


# --- Capability hooks (read by the workspace coordinator / registry) ---
func has_detail() -> bool:
	return false


# --- Label helpers (delegate to InspectorForms) ---
func _add_section_heading(parent: Control, text: String) -> Label:
	return InspectorForms.add_section_heading(parent, text)


func _add_muted_label(parent: Control, text: String) -> Label:
	return InspectorForms.add_muted_label(parent, text)


func _add_empty_state(parent: Control, text: String, node_name := "") -> Label:
	return InspectorForms.add_empty_state(parent, text, node_name)
