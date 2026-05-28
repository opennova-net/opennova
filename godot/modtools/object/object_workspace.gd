class_name ObjectEditorWorkspace
extends "res://modtools/editor/editor_workspace.gd"

const ObjectEditorScript = preload("res://modtools/object/object_editor.gd")
const ObjectPreviewScript = preload("res://modtools/object/object_preview.gd")
const ShaderTagPickerScript = preload("res://modtools/object/ui/widgets/shader_tag_picker.gd")
const TextureSlotWidgetScript = preload("res://modtools/object/ui/widgets/texture_slot_widget.gd")
const CtrlRegPickerScript = preload("res://modtools/object/ui/widgets/ctrl_reg_picker.gd")
const AnimFramesDialogScript = preload("res://modtools/object/ui/dialogs/anim_frames_dialog.gd")
const PreviewInspectorScript = preload("res://modtools/object/ui/inspectors/preview_inspector.gd")
const LodsInspectorScript = preload("res://modtools/object/ui/inspectors/lods_inspector.gd")
const LightsInspectorScript = preload("res://modtools/object/ui/inspectors/lights_inspector.gd")
const MaterialsInspectorScript = preload("res://modtools/object/ui/inspectors/materials_inspector.gd")
const PartAnimsInspectorScript = preload("res://modtools/object/ui/inspectors/part_anims_inspector.gd")

enum Workflow { PREVIEW, MATERIALS, PARTS, LIGHTS, LODS }

const WORKFLOW_DEFS := [
	{"id": Workflow.PREVIEW, "label": "Preview", "tooltip": "View the object with fixed editor lighting."},
	{"id": Workflow.MATERIALS, "label": "Materials", "tooltip": "Edit material shader tags, textures, and alpha."},
	{"id": Workflow.PARTS, "label": "Part Anims", "tooltip": "Inspect and edit PANM part animation entries."},
	{"id": Workflow.LIGHTS, "label": "Lights", "tooltip": "Inspect and edit object light colors."},
	{"id": Workflow.LODS, "label": "LODs", "tooltip": "Bind ASE scenes and edit project LOD settings."},
]

const OED_UPDATE_NONE := 0
const OED_UPDATE_MTRL := 1
const OED_UPDATE_LGHT := 2
const OED_UPDATE_PANM := 4
const OED_UPDATE_ALL := OED_UPDATE_MTRL | OED_UPDATE_LGHT | OED_UPDATE_PANM
# Maximum value of a 16-bit unsigned field (control registers, light rate).
const U16_VALUE_MAX := 65535

var object_editor: ObjectEditor
var environment_editor
var _active_workflow_id: int = Workflow.PREVIEW
var _preview: ObjectPreview
var _asset_dock_host: Control
var _object_detail_dock: Control
var _preview_inspector
var _lods_inspector
var _lights_inspector
var _materials_inspector
var _parts_inspector
var _export_update_mask: int = OED_UPDATE_NONE


func set_editor_shell(value: Node) -> void:
	super.set_editor_shell(value)
	_ensure_object_editor()


func set_environment_editor(value) -> void:
	if environment_editor != null and environment_editor.has_signal("environment_changed") and environment_editor.environment_changed.is_connected(_on_environment_editor_changed):
		environment_editor.environment_changed.disconnect(_on_environment_editor_changed)
	environment_editor = value
	if environment_editor != null and environment_editor.has_signal("environment_changed") and not environment_editor.environment_changed.is_connected(_on_environment_editor_changed):
		environment_editor.environment_changed.connect(_on_environment_editor_changed)
	_apply_environment_to_preview()


func activate() -> void:
	_ensure_object_editor()


func deactivate() -> void:
	pass


func mount_viewport(host: Control) -> void:
	if host == null:
		return
	_ensure_object_editor()
	if _preview == null:
		_preview = ObjectPreviewScript.new()
		_preview.name = "ObjectPreview"
		_preview.set_anchors_preset(Control.PRESET_FULL_RECT)
		_preview.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_preview.size_flags_vertical = Control.SIZE_EXPAND_FILL
		_preview.set_object_data(object_editor.object_data if object_editor else null)
		_apply_environment_to_preview()
	if _preview.get_parent() == null:
		host.add_child(_preview)
		_preview.set_anchors_preset(Control.PRESET_FULL_RECT)


func unmount_viewport(_host: Control) -> void:
	if _preview != null and _preview.get_parent() != null:
		_preview.get_parent().remove_child(_preview)


func release_viewport() -> void:
	if _preview == null:
		return
	if _preview.get_parent() != null:
		_preview.get_parent().remove_child(_preview)
	_preview.free()
	_preview = null


func get_viewport_camera() -> Camera3D:
	if _preview != null and _preview.has_method("get_editor_camera"):
		return _preview.get_editor_camera()
	return null


func get_workspace_id() -> String:
	return "object"


func get_workspace_label() -> String:
	return "Object"


func get_project_title() -> String:
	return object_editor.get_project_title() if object_editor else "Object"


func get_status_tool() -> String:
	match _active_workflow_id:
		Workflow.MATERIALS:
			return "Materials"
		Workflow.PARTS:
			return "Part anims"
		Workflow.LIGHTS:
			return "Lights"
		Workflow.LODS:
			return "LODs"
		_:
			return "Object"


func get_status_context() -> String:
	return object_editor.get_status_context() if object_editor else "No object loaded"


func uses_asset_dock() -> bool:
	return _active_workflow_uses_detail_dock()


func set_asset_dock(dock: Control) -> void:
	_asset_dock_host = dock
	if dock == null:
		_free_object_detail_dock()
		return
	_sync_object_detail_dock_mount()


func sync_asset_dock() -> void:
	_sync_object_detail_dock_mount()


func get_workflows() -> Array:
	return WORKFLOW_DEFS


func get_active_workflow_id() -> int:
	return _active_workflow_id


func activate_workflow(workflow_id: int) -> void:
	_active_workflow_id = workflow_id
	_sync_object_detail_dock_mount()


func build_workflow_inspector(workflow_id: int, host: Control) -> void:
	_ensure_object_editor()
	_active_workflow_id = workflow_id
	match workflow_id:
		Workflow.MATERIALS:
			_materials_inspector.build_main(host)
		Workflow.PARTS:
			_parts_inspector.build_main(host)
		Workflow.LIGHTS:
			_lights_inspector.build_main(host)
		Workflow.LODS:
			_lods_inspector.build_main(host)
		_:
			_preview_inspector.build_main(host)
	_sync_object_detail_dock_mount()


func has_unsaved_changes() -> bool:
	return object_editor != null and object_editor.is_dirty


func can_new() -> bool:
	return true


func get_new_action_label() -> String:
	return "New Object"


func new_current() -> Error:
	_ensure_object_editor()
	object_editor.create_empty_object(true)
	_sync_shell()
	return OK


func can_open() -> bool:
	return true


func get_open_action_label() -> String:
	return "Open Object..."


func get_open_dialog_title() -> String:
	return "Open object source"


func get_open_dialog_filters() -> PackedStringArray:
	return PackedStringArray([
		"*.3di,*.3DI ; 3DI object",
		"*.3dp,*.3DP ; 3DP object project",
		"*.ase,*.ASE ; ASE scene",
	])


func get_open_dialog_dir() -> String:
	return object_editor.get_last_open_dir() if object_editor else ""


func get_open_resource_kind() -> String:
	return "object"


func get_current_resource_path() -> String:
	return object_editor.current_path if object_editor else ""


func open_file(path: String) -> Error:
	_ensure_object_editor()
	var err := object_editor.open_object(path)
	_sync_shell()
	return err


func add_lod_scene(path: String, lod_index: int = -1) -> Error:
	_ensure_object_editor()
	var err := object_editor.add_lod_scene(path, lod_index)
	_sync_shell()
	return err


func can_save() -> bool:
	return object_editor != null and object_editor.is_dirty and object_editor.object_data != null and object_editor.object_data.can_save_project()


func get_save_action_label() -> String:
	return "Save Object Project"


func can_save_as() -> bool:
	return object_editor != null and object_editor.object_data != null and object_editor.object_data.has_document()


func get_save_as_action_label() -> String:
	return "Save Object Project As..."


func save_current() -> Error:
	return object_editor.save_current() if object_editor else ERR_UNAVAILABLE


func save_as(dir_path: String) -> Error:
	return object_editor.save_as(dir_path) if object_editor else ERR_UNAVAILABLE


func can_export() -> bool:
	return object_editor != null and object_editor.object_data != null and object_editor.object_data.can_export_3di()


func has_export_action() -> bool:
	return true


func get_export_action_label() -> String:
	return "Export 3DI..."


func begin_export(dir_path: String, _flavor: int) -> Error:
	if object_editor == null:
		return ERR_UNAVAILABLE
	var err := object_editor.export_to_dir(dir_path, _selected_export_update_mask())
	if err == OK:
		_sync_export_update_mask_from_dirty()
	return err


func get_save_dialog_title() -> String:
	return "Choose where to save the object project"


func get_save_dialog_dir() -> String:
	return object_editor.get_last_save_dir() if object_editor else ""


func get_export_dialog_title() -> String:
	return "Choose where to export the 3DI"


func get_export_dialog_dir() -> String:
	return object_editor.get_last_export_dir() if object_editor else ""


func build_inspector(host: Control) -> void:
	_ensure_inspectors()
	_preview_inspector.build_main(host)


func _ensure_inspectors() -> void:
	if _preview_inspector == null:
		_preview_inspector = PreviewInspectorScript.new(self)
	if _lods_inspector == null:
		_lods_inspector = LodsInspectorScript.new(self)
	if _lights_inspector == null:
		_lights_inspector = LightsInspectorScript.new(self)
	if _materials_inspector == null:
		_materials_inspector = MaterialsInspectorScript.new(self)
	if _parts_inspector == null:
		_parts_inspector = PartAnimsInspectorScript.new(self)


func _ensure_object_editor() -> void:
	_ensure_inspectors()
	if object_editor != null:
		return
	object_editor = ObjectEditorScript.new()
	object_editor.name = "ObjectEditor"
	if editor_shell != null:
		editor_shell.add_child(object_editor)
	object_editor.create_empty_object(false)
	object_editor.state_changed.connect(_sync_shell)
	_sync_export_update_mask_from_dirty()


func _sync_shell() -> void:
	_sync_export_update_mask_from_dirty()
	if _preview != null and object_editor != null:
		if _preview.object_data != object_editor.object_data:
			_preview.set_object_data(object_editor.object_data)
		_apply_environment_to_preview()
	if _active_workflow_id == Workflow.PARTS:
		_parts_inspector.refresh()
	elif _active_workflow_id == Workflow.MATERIALS:
		_materials_inspector.refresh()
	elif _active_workflow_id == Workflow.LIGHTS:
		_lights_inspector.refresh()
	elif _active_workflow_id != Workflow.PARTS:
		_rebuild_object_detail_dock()
	if editor_shell != null and editor_shell.has_method("sync_from_editor_state"):
		editor_shell.sync_from_editor_state()


func _on_environment_editor_changed(_env_file: EnvFile, _time_of_day: float) -> void:
	_apply_environment_to_preview()


func _apply_environment_to_preview() -> void:
	if _preview == null:
		return
	var env_file: EnvFile = null
	var preview_time := 1200.0
	if environment_editor != null:
		env_file = environment_editor.get("env_file") as EnvFile
		preview_time = float(environment_editor.get("time_of_day"))
	_preview.set_environment(env_file, preview_time)


func _sync_export_update_mask_from_dirty() -> void:
	var dirty_mask := _get_oed_dirty_mask()
	_export_update_mask = dirty_mask if dirty_mask != OED_UPDATE_NONE else OED_UPDATE_ALL


func _selected_export_update_mask() -> int:
	var mask := _export_update_mask & OED_UPDATE_ALL
	return OED_UPDATE_ALL if mask == OED_UPDATE_NONE else mask


func _get_oed_dirty_mask() -> int:
	if object_editor == null or object_editor.object_data == null:
		return OED_UPDATE_NONE
	if not object_editor.object_data.has_method("get_oed_dirty_mask"):
		return OED_UPDATE_NONE
	return int(object_editor.object_data.get_oed_dirty_mask()) & OED_UPDATE_ALL


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


func _control_registers() -> Array:
	if object_editor != null and object_editor.object_data != null and object_editor.object_data.has_method("get_control_registers"):
		return object_editor.object_data.get_control_registers()
	return []


func _set_spin(node, value: float) -> void:
	ObjectUiHelpers.set_spin(node, value)


func _active_workflow_uses_detail_dock() -> bool:
	return _active_workflow_id == Workflow.MATERIALS or _active_workflow_id == Workflow.PARTS or _active_workflow_id == Workflow.LIGHTS


func _free_object_detail_dock() -> void:
	if _object_detail_dock == null:
		return
	if is_instance_valid(_object_detail_dock):
		var parent := _object_detail_dock.get_parent()
		if parent != null:
			parent.remove_child(_object_detail_dock)
		_object_detail_dock.free()
	_object_detail_dock = null


func _ensure_object_detail_dock() -> void:
	if _asset_dock_host == null or not _active_workflow_uses_detail_dock():
		return
	if _object_detail_dock == null or not is_instance_valid(_object_detail_dock):
		_object_detail_dock = PanelContainer.new()
		_object_detail_dock.name = "ObjectDetailDock"
		_object_detail_dock.size_flags_horizontal = Control.SIZE_EXPAND_FILL
		_object_detail_dock.size_flags_vertical = Control.SIZE_EXPAND_FILL
	if _object_detail_dock.get_parent() != _asset_dock_host:
		var old_parent := _object_detail_dock.get_parent()
		if old_parent != null:
			old_parent.remove_child(_object_detail_dock)
		_asset_dock_host.add_child(_object_detail_dock)


func _sync_object_detail_dock_mount() -> void:
	if not _active_workflow_uses_detail_dock():
		_free_object_detail_dock()
		return
	_ensure_object_detail_dock()
	_rebuild_object_detail_dock()


func _rebuild_object_detail_dock() -> void:
	if not _active_workflow_uses_detail_dock():
		_free_object_detail_dock()
		return
	if _object_detail_dock == null or not is_instance_valid(_object_detail_dock):
		return
	for child in _object_detail_dock.get_children():
		_object_detail_dock.remove_child(child)
		child.free()

	var margin := MarginContainer.new()
	margin.add_theme_constant_override("margin_left", ObjectUiHelpers.PANEL_MARGIN)
	margin.add_theme_constant_override("margin_top", ObjectUiHelpers.PANEL_MARGIN)
	margin.add_theme_constant_override("margin_right", ObjectUiHelpers.PANEL_MARGIN)
	margin.add_theme_constant_override("margin_bottom", ObjectUiHelpers.PANEL_MARGIN)
	margin.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	margin.size_flags_vertical = Control.SIZE_EXPAND_FILL
	_object_detail_dock.add_child(margin)

	var scroll := ScrollContainer.new()
	scroll.horizontal_scroll_mode = 0
	scroll.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.size_flags_vertical = Control.SIZE_EXPAND_FILL
	margin.add_child(scroll)

	var box := VBoxContainer.new()
	box.name = "ObjectDetailDockBox"
	box.add_theme_constant_override("separation", 10)
	box.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scroll.add_child(box)

	match _active_workflow_id:
		Workflow.PARTS:
			_parts_inspector.build_detail(box)
		Workflow.MATERIALS:
			_materials_inspector.build_detail(box)
		Workflow.LIGHTS:
			_lights_inspector.build_detail(box)


func _make_inspector_box(host: Control) -> VBoxContainer:
	return ObjectUiHelpers.make_inspector_box(host)
