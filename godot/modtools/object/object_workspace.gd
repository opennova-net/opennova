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
const PART_ANIM_MOTION_MODE_OPTIONS := [
	{"id": 0, "label": "None", "mode": "none"},
	{"id": 16, "label": "Slide", "mode": "slide"},
	{"id": 17, "label": "Slide inverse", "mode": "slide_inverse"},
	{"id": 24, "label": "Set", "mode": "set"},
	{"id": 32, "label": "Rotate clockwise", "mode": "rotate_cw"},
	{"id": 33, "label": "Rotate counter-clockwise", "mode": "rotate_ccw"},
	{"id": 50, "label": "Sine wave", "mode": "sine_wave"},
	{"id": 52, "label": "Saw wave", "mode": "saw_wave"},
	{"id": 53, "label": "Inverse saw wave", "mode": "inverse_saw_wave"},
	{"id": 113, "label": "Control register", "mode": "control_register"},
	{"id": 114, "label": "Add control register", "mode": "control_register_add"},
]
const PART_ANIM_SCALE_STYLE_OPTIONS := [
	{"id": 1, "label": "Uniform"},
	{"id": 2, "label": "Per-axis"},
]
const PART_ANIM_TRANSLATION_AXIS_OPTIONS := [
	{"id": 1, "label": "X"},
	{"id": 2, "label": "Y"},
	{"id": 3, "label": "Z"},
]
const PANM_TRANSLATION_VALUE_MIN := -128.0
const PANM_TRANSLATION_VALUE_MAX := 127.99609375

# Motion-mode IDs that bind a control register (see PART_ANIM_MOTION_MODE_OPTIONS).
const CONTROL_REGISTER_MODE_ID := 113
const CONTROL_REGISTER_ADD_MODE_ID := 114
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
var _export_update_mask: int = OED_UPDATE_NONE
var _part_anim_lod_index := 0
var _part_anim_selected_index := 0
var _part_anim_list: ItemList
var _part_anim_lod_spin: SpinBox
var _part_anim_duplicate_button: Button
var _part_anim_delete_button: Button
var _part_anim_inspector_summary: Label
var _part_anim_inspector_context: Label


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
			_build_part_anims_inspector(host)
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
	if _active_workflow_id == Workflow.PARTS and _part_anim_list != null:
		_refresh_part_anim_list(_part_anim_selected_index, false)
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


func _mode_name_for_id(mode_id: int) -> String:
	for option in PART_ANIM_MOTION_MODE_OPTIONS:
		if int(option.get("id", -1)) == mode_id:
			return String(option.get("mode", "none"))
	return "none"


func _mode_id_for_name(mode_name: String) -> int:
	for option in PART_ANIM_MOTION_MODE_OPTIONS:
		if String(option.get("mode", "")) == mode_name:
			return int(option.get("id", 0))
	return 0


func _axis_name_for_id(axis_id: int) -> String:
	if axis_id == 2:
		return "y"
	if axis_id == 3:
		return "z"
	return "x"


func _axis_id_for_name(axis_name: String) -> int:
	if axis_name == "y":
		return 2
	if axis_name == "z":
		return 3
	return 1


func _part_options_for_lod(lod_index: int) -> Array:
	var data: NovaObjectData = object_editor.object_data if object_editor else null
	var options := []
	var lod_info: Dictionary = data.get_render_lod_info(lod_index) if data != null and data.has_method("get_render_lod_info") else {}
	var part_count := maxi(1, int(lod_info.get("part_count", lod_info.get("render_object_count", 1))))
	for part_index in range(part_count):
		options.append({"id": part_index, "label": "Part %02d" % part_index})
	return options


func _part_anim_entries() -> Array:
	var data: NovaObjectData = object_editor.object_data if object_editor else null
	if data == null:
		return []
	if data.has_method("get_part_anim_editor_entries"):
		return data.get_part_anim_editor_entries(_part_anim_lod_index)
	var raw: Array = data.get_part_animations(_part_anim_lod_index)
	for i in range(raw.size()):
		var entry: Dictionary = raw[i]
		entry["summary"] = "Part %02d" % int(entry.get("part_index", i))
	return raw


func _refresh_part_anim_list(preferred_index: int = -1, rebuild_detail: bool = true) -> void:
	var data: NovaObjectData = object_editor.object_data if object_editor else null
	if data == null:
		return
	var summary: Dictionary = data.get_summary()
	var lod_count := maxi(1, int(summary.get("lod_count", 1)))
	_part_anim_lod_index = clampi(_part_anim_lod_index, 0, lod_count - 1)
	if _part_anim_lod_spin != null:
		_part_anim_lod_spin.max_value = lod_count - 1
		_set_spin(_part_anim_lod_spin, _part_anim_lod_index)

	var total_count := 0
	for lod_index in range(lod_count):
		total_count += data.get_part_anim_count(lod_index)
	if _part_anim_inspector_summary != null:
		_part_anim_inspector_summary.text = "%d part animations" % total_count
	var entries := _part_anim_entries()
	if preferred_index >= 0:
		_part_anim_selected_index = preferred_index
	_part_anim_selected_index = clampi(_part_anim_selected_index, 0, maxi(0, entries.size() - 1)) if not entries.is_empty() else -1
	if _part_anim_inspector_context != null:
		_part_anim_inspector_context.text = "LOD %d: %d entries" % [_part_anim_lod_index, entries.size()]
	if _part_anim_duplicate_button != null:
		_part_anim_duplicate_button.disabled = _part_anim_selected_index < 0
	if _part_anim_delete_button != null:
		_part_anim_delete_button.disabled = _part_anim_selected_index < 0
	if _part_anim_list != null:
		_part_anim_list.clear()
		for anim in entries:
			_part_anim_list.add_item(String((anim as Dictionary).get("summary", "Part animation")))
		if _part_anim_selected_index >= 0:
			_part_anim_list.select(_part_anim_selected_index)
	if rebuild_detail:
		_rebuild_object_detail_dock()


func _build_part_anims_inspector(host: Control) -> void:
	var box := _make_inspector_box(host)
	box.name = "PartAnimListPane"
	var data: NovaObjectData = object_editor.object_data if object_editor else null
	var summary: Dictionary = data.get_summary() if data != null else {}
	var lod_count := maxi(1, int(summary.get("lod_count", 1)))
	_part_anim_lod_index = clampi(_part_anim_lod_index, 0, lod_count - 1)

	_part_anim_inspector_summary = Label.new()
	_part_anim_inspector_summary.name = "PartAnimInspectorSummary"
	_part_anim_inspector_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_part_anim_inspector_summary)

	_part_anim_inspector_context = Label.new()
	_part_anim_inspector_context.name = "PartAnimInspectorContext"
	_part_anim_inspector_context.theme_type_variation = &"Muted"
	_part_anim_inspector_context.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	box.add_child(_part_anim_inspector_context)

	_part_anim_lod_spin = _add_spin_row(box, "PartAnimLodIndex", "LOD", 0, lod_count - 1, 1)

	var actions := HBoxContainer.new()
	actions.name = "PartAnimActions"
	actions.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(actions)

	var add_button := Button.new()
	add_button.name = "PartAnimAddButton"
	add_button.text = "Add"
	actions.add_child(add_button)

	_part_anim_duplicate_button = Button.new()
	_part_anim_duplicate_button.name = "PartAnimDuplicateButton"
	_part_anim_duplicate_button.text = "Duplicate"
	actions.add_child(_part_anim_duplicate_button)

	_part_anim_delete_button = Button.new()
	_part_anim_delete_button.name = "PartAnimDeleteButton"
	_part_anim_delete_button.text = "Delete"
	actions.add_child(_part_anim_delete_button)

	_part_anim_list = ItemList.new()
	_part_anim_list.name = "PartAnimList"
	_part_anim_list.custom_minimum_size = Vector2(0, 360)
	_part_anim_list.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	_part_anim_list.size_flags_vertical = Control.SIZE_EXPAND_FILL
	box.add_child(_part_anim_list)

	_part_anim_lod_spin.value_changed.connect(func(value: float) -> void:
		_part_anim_lod_index = int(value)
		_part_anim_selected_index = 0
		_refresh_part_anim_list(0, true)
	)
	_part_anim_list.item_selected.connect(func(index: int) -> void:
		var selection_changed := index != _part_anim_selected_index
		_part_anim_selected_index = index
		_refresh_part_anim_list(index, selection_changed)
	)
	add_button.pressed.connect(func() -> void:
		if data == null or not data.has_method("add_part_anim"):
			return
		var target_index := 0
		var entries := _part_anim_entries()
		if _part_anim_selected_index >= 0 and _part_anim_selected_index < entries.size():
			target_index = int((entries[_part_anim_selected_index] as Dictionary).get("target_part", 0))
		var new_index := int(data.call("add_part_anim", _part_anim_lod_index, target_index))
		if new_index >= 0:
			_refresh_part_anim_list(new_index, true)
	)
	_part_anim_duplicate_button.pressed.connect(func() -> void:
		if data == null or _part_anim_selected_index < 0 or not data.has_method("duplicate_part_anim"):
			return
		var new_index := int(data.call("duplicate_part_anim", _part_anim_lod_index, _part_anim_selected_index))
		if new_index >= 0:
			_refresh_part_anim_list(new_index, true)
	)
	_part_anim_delete_button.pressed.connect(func() -> void:
		if data == null or _part_anim_selected_index < 0 or not data.has_method("delete_part_anim"):
			return
		var removed_index := _part_anim_selected_index
		if bool(data.call("delete_part_anim", _part_anim_lod_index, removed_index)):
			_refresh_part_anim_list(mini(removed_index, maxi(0, data.get_part_anim_count(_part_anim_lod_index) - 1)), true)
	)
	_refresh_part_anim_list(_part_anim_selected_index, true)


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
			_build_part_anim_detail_dock(box)
		Workflow.MATERIALS:
			_materials_inspector.build_detail(box)
		Workflow.LIGHTS:
			_lights_inspector.build_detail(box)


func _build_channel_card(parent: VBoxContainer, node_name: String) -> VBoxContainer:
	return ObjectUiHelpers.build_channel_card(parent, node_name)


func _build_part_anim_detail_dock(box: VBoxContainer) -> void:
	var data: NovaObjectData = object_editor.object_data if object_editor else null
	var heading := Label.new()
	heading.theme_type_variation = &"Heading"
	heading.text = "Part animation"
	box.add_child(heading)

	var entries := _part_anim_entries()
	if data == null or entries.is_empty() or _part_anim_selected_index < 0:
		var empty := Label.new()
		empty.name = "PartAnimDetailsEmpty"
		empty.theme_type_variation = &"Muted"
		empty.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
		empty.text = "Select or add a part animation."
		box.add_child(empty)
		return

	_part_anim_selected_index = clampi(_part_anim_selected_index, 0, entries.size() - 1)
	var info: Dictionary = entries[_part_anim_selected_index]
	var supported := bool(info.get("supported", true))

	var selected_summary := Label.new()
	selected_summary.name = "PartAnimSelectedSummary"
	selected_summary.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	selected_summary.theme_type_variation = &"Muted"
	selected_summary.text = String(info.get("summary", "Part animation"))
	box.add_child(selected_summary)

	var unsupported_notice := Label.new()
	unsupported_notice.name = "PartAnimUnsupportedNotice"
	unsupported_notice.visible = not supported
	unsupported_notice.theme_type_variation = &"Warn"
	unsupported_notice.autowrap_mode = TextServer.AUTOWRAP_WORD_SMART
	unsupported_notice.text = String(info.get("unsupported_reason", "Unsupported PANM mode"))
	box.add_child(unsupported_notice)

	var part_section := VBoxContainer.new()
	part_section.name = "PartAnimTargetSection"
	part_section.add_theme_constant_override("separation", 6)
	part_section.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	box.add_child(part_section)

	var part_options := _part_options_for_lod(_part_anim_lod_index)
	var target_part := _add_detail_id_option_row(part_section, "PartAnimTargetPart", "Animated part", part_options)
	var parent_part := _add_detail_id_option_row(part_section, "PartAnimParentPart", "Moves relative to", part_options)
	_populate_id_option(target_part, part_options, int(info.get("target_part", 0)))
	_populate_id_option(parent_part, part_options, int(info.get("parent_part", 0)))

	var rotation: Dictionary = info.get("rotation", {})
	var rotation_x: Dictionary = rotation.get("x", {})
	var rotation_y: Dictionary = rotation.get("y", {})
	var rotation_z: Dictionary = rotation.get("z", {})
	var rotation_card := _build_channel_card(box, "PartAnimRotationCard")
	var rotation_enabled := CheckBox.new()
	rotation_enabled.name = "PartAnimRotationEnabled"
	rotation_enabled.text = "Rotation"
	rotation_enabled.button_pressed = bool(rotation.get("enabled", false))
	rotation_enabled.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rotation_card.add_child(rotation_enabled)
	var rotation_actions := HBoxContainer.new()
	rotation_actions.name = "PartAnimRotationActions"
	rotation_actions.add_theme_constant_override("separation", 6)
	rotation_actions.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rotation_card.add_child(rotation_actions)
	var add_rotation := Button.new()
	add_rotation.name = "PartAnimAddRotationButton"
	add_rotation.text = "Add"
	add_rotation.custom_minimum_size = Vector2(0, 32)
	add_rotation.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rotation_actions.add_child(add_rotation)
	var remove_rotation := Button.new()
	remove_rotation.name = "PartAnimRemoveRotationButton"
	remove_rotation.text = "Remove"
	remove_rotation.custom_minimum_size = Vector2(0, 32)
	remove_rotation.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rotation_actions.add_child(remove_rotation)
	var rotation_controls := VBoxContainer.new()
	rotation_controls.name = "PartAnimRotationControls"
	rotation_controls.add_theme_constant_override("separation", 4)
	rotation_card.add_child(rotation_controls)
	var rotation_mode := _add_detail_id_option_row(rotation_controls, "PartAnimRotationMode", "Driver", PART_ANIM_MOTION_MODE_OPTIONS)
	var rotation_x_from := _add_detail_spin_row(rotation_controls, "PartAnimRotationXFrom", "Yaw start", -3600, 3600, 0.1)
	var rotation_x_to := _add_detail_spin_row(rotation_controls, "PartAnimRotationXTo", "Yaw end", -3600, 3600, 0.1)
	var rotation_y_from := _add_detail_spin_row(rotation_controls, "PartAnimRotationYFrom", "Pitch start", -3600, 3600, 0.1)
	var rotation_y_to := _add_detail_spin_row(rotation_controls, "PartAnimRotationYTo", "Pitch end", -3600, 3600, 0.1)
	var rotation_z_from := _add_detail_spin_row(rotation_controls, "PartAnimRotationZFrom", "Roll start", -3600, 3600, 0.1)
	var rotation_z_to := _add_detail_spin_row(rotation_controls, "PartAnimRotationZTo", "Roll end", -3600, 3600, 0.1)
	var rotation_speed := _add_detail_spin_row(rotation_controls, "PartAnimRotationSpeed", "Speed", -128, 128, 0.01)
	var reversed := CheckBox.new()
	reversed.name = "PartAnimRotationReversed"
	reversed.text = "Reverse rotation order"
	reversed.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	rotation_controls.add_child(reversed)
	_populate_id_option(rotation_mode, PART_ANIM_MOTION_MODE_OPTIONS, _mode_id_for_name(String(rotation_x.get("mode", "none"))))
	_set_spin(rotation_x_from, float(rotation_x.get("from_value", 0.0)))
	_set_spin(rotation_x_to, float(rotation_x.get("to_value", 0.0)))
	_set_spin(rotation_y_from, float(rotation_y.get("from_value", 0.0)))
	_set_spin(rotation_y_to, float(rotation_y.get("to_value", 0.0)))
	_set_spin(rotation_z_from, float(rotation_z.get("from_value", 0.0)))
	_set_spin(rotation_z_to, float(rotation_z.get("to_value", 0.0)))
	_set_spin(rotation_speed, float(rotation_x.get("speed", 0.0)))
	reversed.button_pressed = bool(rotation.get("reversed", false))

	var scale: Dictionary = info.get("scale", {})
	var scale_x: Dictionary = scale.get("x", {})
	var scale_card := _build_channel_card(box, "PartAnimScaleCard")
	var scale_enabled := CheckBox.new()
	scale_enabled.name = "PartAnimScaleEnabled"
	scale_enabled.text = "Scale"
	scale_enabled.button_pressed = bool(scale.get("enabled", false))
	scale_enabled.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scale_card.add_child(scale_enabled)
	var scale_actions := HBoxContainer.new()
	scale_actions.name = "PartAnimScaleActions"
	scale_actions.add_theme_constant_override("separation", 6)
	scale_actions.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scale_card.add_child(scale_actions)
	var add_scale := Button.new()
	add_scale.name = "PartAnimAddScaleButton"
	add_scale.text = "Add"
	add_scale.custom_minimum_size = Vector2(0, 32)
	add_scale.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scale_actions.add_child(add_scale)
	var remove_scale := Button.new()
	remove_scale.name = "PartAnimRemoveScaleButton"
	remove_scale.text = "Remove"
	remove_scale.custom_minimum_size = Vector2(0, 32)
	remove_scale.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	scale_actions.add_child(remove_scale)
	var scale_controls := VBoxContainer.new()
	scale_controls.name = "PartAnimScaleControls"
	scale_controls.add_theme_constant_override("separation", 4)
	scale_card.add_child(scale_controls)
	var scale_mode := _add_detail_id_option_row(scale_controls, "PartAnimScaleMode", "Style", PART_ANIM_SCALE_STYLE_OPTIONS)
	var scale_motion := _add_detail_id_option_row(scale_controls, "PartAnimScaleMotionMode", "Driver", PART_ANIM_MOTION_MODE_OPTIONS)
	var scale_from := _add_detail_spin_row(scale_controls, "PartAnimScaleUniformFrom", "Start", -128, 128, 0.01)
	var scale_to := _add_detail_spin_row(scale_controls, "PartAnimScaleUniformTo", "End", -128, 128, 0.01)
	var scale_speed := _add_detail_spin_row(scale_controls, "PartAnimScaleSpeed", "Speed", -128, 128, 0.01)
	_populate_id_option(scale_mode, PART_ANIM_SCALE_STYLE_OPTIONS, 2 if String(scale.get("style", "uniform")) == "per_axis" else 1)
	_populate_id_option(scale_motion, PART_ANIM_MOTION_MODE_OPTIONS, _mode_id_for_name(String(scale_x.get("mode", "none"))))
	_set_spin(scale_from, float(scale_x.get("from_value", 1.0)))
	_set_spin(scale_to, float(scale_x.get("to_value", 1.0)))
	_set_spin(scale_speed, float(scale_x.get("speed", 0.0)))

	var translation: Dictionary = info.get("translation", {})
	var translation_track: Dictionary = translation.get("track", {})
	var translation_card := _build_channel_card(box, "PartAnimTranslationCard")
	var translation_enabled := CheckBox.new()
	translation_enabled.name = "PartAnimTranslationEnabled"
	translation_enabled.text = "Translation"
	translation_enabled.button_pressed = bool(translation.get("enabled", false))
	translation_enabled.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	translation_card.add_child(translation_enabled)
	var translation_actions := HBoxContainer.new()
	translation_actions.name = "PartAnimTranslationActions"
	translation_actions.add_theme_constant_override("separation", 6)
	translation_actions.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	translation_card.add_child(translation_actions)
	var add_translation := Button.new()
	add_translation.name = "PartAnimAddTranslationButton"
	add_translation.text = "Add"
	add_translation.custom_minimum_size = Vector2(0, 32)
	add_translation.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	translation_actions.add_child(add_translation)
	var remove_translation := Button.new()
	remove_translation.name = "PartAnimRemoveTranslationButton"
	remove_translation.text = "Remove"
	remove_translation.custom_minimum_size = Vector2(0, 32)
	remove_translation.size_flags_horizontal = Control.SIZE_EXPAND_FILL
	translation_actions.add_child(remove_translation)
	var translation_controls := VBoxContainer.new()
	translation_controls.name = "PartAnimTranslationControls"
	translation_controls.add_theme_constant_override("separation", 4)
	translation_card.add_child(translation_controls)
	var translation_axis := _add_detail_id_option_row(translation_controls, "PartAnimTranslationAxis", "Direction", PART_ANIM_TRANSLATION_AXIS_OPTIONS)
	var translation_mode := _add_detail_id_option_row(translation_controls, "PartAnimTranslationMode", "Driver", PART_ANIM_MOTION_MODE_OPTIONS)
	var translation_register = _add_detail_ctrl_reg_row(translation_controls, "PartAnimTranslationRegister", "Control register")
	var translation_from := _add_detail_spin_row(translation_controls, "PartAnimTranslationFrom", "Start", PANM_TRANSLATION_VALUE_MIN, PANM_TRANSLATION_VALUE_MAX, 0.01)
	var translation_to := _add_detail_spin_row(translation_controls, "PartAnimTranslationTo", "End", PANM_TRANSLATION_VALUE_MIN, PANM_TRANSLATION_VALUE_MAX, 0.01)
	var translation_speed := _add_detail_spin_row(translation_controls, "PartAnimTranslationSpeed", "Speed", -128, 128, 0.01)
	_populate_id_option(translation_axis, PART_ANIM_TRANSLATION_AXIS_OPTIONS, _axis_id_for_name(String(translation.get("axis", "x"))))
	_populate_id_option(translation_mode, PART_ANIM_MOTION_MODE_OPTIONS, _mode_id_for_name(String(translation_track.get("mode", "none"))))
	if translation_register != null:
		translation_register.setup(_control_registers(), int(translation_track.get("control_register", -1)))
	_set_spin(translation_from, float(translation_track.get("from_value", 0.0)))
	_set_spin(translation_to, float(translation_track.get("to_value", 0.0)))
	_set_spin(translation_speed, float(translation_track.get("speed", 0.0)))

	var update_visibility := func() -> void:
		rotation_controls.visible = rotation_enabled.button_pressed and supported
		add_rotation.visible = not rotation_enabled.button_pressed and supported
		remove_rotation.visible = rotation_enabled.button_pressed and supported
		scale_controls.visible = scale_enabled.button_pressed and supported
		add_scale.visible = not scale_enabled.button_pressed and supported
		remove_scale.visible = scale_enabled.button_pressed and supported
		translation_controls.visible = translation_enabled.button_pressed and supported
		add_translation.visible = not translation_enabled.button_pressed and supported
		remove_translation.visible = translation_enabled.button_pressed and supported
		if translation_register != null and translation_register.get_parent() != null:
			translation_register.get_parent().visible = translation_mode.get_selected_id() == CONTROL_REGISTER_MODE_ID or translation_mode.get_selected_id() == CONTROL_REGISTER_ADD_MODE_ID
	update_visibility.call()

	var refresh_after_edit := func() -> void:
		_refresh_part_anim_list(_part_anim_selected_index, false)

	target_part.item_selected.connect(func(_index: int) -> void:
		if data != null and data.has_method("set_part_anim_target"):
			data.set_part_anim_target(_part_anim_lod_index, _part_anim_selected_index, target_part.get_selected_id(), parent_part.get_selected_id())
			refresh_after_edit.call()
	)
	parent_part.item_selected.connect(func(_index: int) -> void:
		if data != null and data.has_method("set_part_anim_target"):
			data.set_part_anim_target(_part_anim_lod_index, _part_anim_selected_index, target_part.get_selected_id(), parent_part.get_selected_id())
			refresh_after_edit.call()
	)
	rotation_enabled.toggled.connect(func(value: bool) -> void:
		if data != null and data.has_method("set_part_anim_channel_enabled"):
			data.set_part_anim_channel_enabled(_part_anim_lod_index, _part_anim_selected_index, "rotation", value)
			refresh_after_edit.call()
		update_visibility.call()
	)
	add_rotation.pressed.connect(func() -> void:
		rotation_enabled.button_pressed = true
		rotation_enabled.toggled.emit(true)
	)
	remove_rotation.pressed.connect(func() -> void:
		rotation_enabled.button_pressed = false
		rotation_enabled.toggled.emit(false)
	)
	rotation_mode.item_selected.connect(func(index: int) -> void:
		if data != null and data.has_method("set_part_anim_channel_mode"):
			data.set_part_anim_channel_mode(_part_anim_lod_index, _part_anim_selected_index, "rotation", "x", _mode_name_for_id(rotation_mode.get_item_id(index)), -1)
			refresh_after_edit.call()
	)
	var set_rotation_values := func(axis: String, from_control: SpinBox, to_control: SpinBox) -> void:
		if data != null and data.has_method("set_part_anim_channel_values"):
			data.set_part_anim_channel_values(_part_anim_lod_index, _part_anim_selected_index, "rotation", axis, float(from_control.value), float(to_control.value), float(rotation_speed.value))
			refresh_after_edit.call()
	rotation_x_from.value_changed.connect(func(_value: float) -> void: set_rotation_values.call("x", rotation_x_from, rotation_x_to))
	rotation_x_to.value_changed.connect(func(_value: float) -> void: set_rotation_values.call("x", rotation_x_from, rotation_x_to))
	rotation_y_from.value_changed.connect(func(_value: float) -> void: set_rotation_values.call("y", rotation_y_from, rotation_y_to))
	rotation_y_to.value_changed.connect(func(_value: float) -> void: set_rotation_values.call("y", rotation_y_from, rotation_y_to))
	rotation_z_from.value_changed.connect(func(_value: float) -> void: set_rotation_values.call("z", rotation_z_from, rotation_z_to))
	rotation_z_to.value_changed.connect(func(_value: float) -> void: set_rotation_values.call("z", rotation_z_from, rotation_z_to))
	rotation_speed.value_changed.connect(func(_value: float) -> void:
		set_rotation_values.call("x", rotation_x_from, rotation_x_to)
		set_rotation_values.call("y", rotation_y_from, rotation_y_to)
		set_rotation_values.call("z", rotation_z_from, rotation_z_to)
	)
	reversed.toggled.connect(func(value: bool) -> void:
		if data != null and data.has_method("set_part_anim_rotation_reversed"):
			data.set_part_anim_rotation_reversed(_part_anim_lod_index, _part_anim_selected_index, value)
			refresh_after_edit.call()
	)
	scale_enabled.toggled.connect(func(value: bool) -> void:
		if data != null and data.has_method("set_part_anim_channel_enabled"):
			data.set_part_anim_channel_enabled(_part_anim_lod_index, _part_anim_selected_index, "scale", value)
			refresh_after_edit.call()
		update_visibility.call()
	)
	add_scale.pressed.connect(func() -> void:
		scale_enabled.button_pressed = true
		scale_enabled.toggled.emit(true)
	)
	remove_scale.pressed.connect(func() -> void:
		scale_enabled.button_pressed = false
		scale_enabled.toggled.emit(false)
	)
	var set_scale_values := func(axis: String) -> void:
		if data != null and data.has_method("set_part_anim_channel_values"):
			data.set_part_anim_channel_values(_part_anim_lod_index, _part_anim_selected_index, "scale", axis, float(scale_from.value), float(scale_to.value), float(scale_speed.value))
			refresh_after_edit.call()
	scale_mode.item_selected.connect(func(index: int) -> void:
		set_scale_values.call("per_axis" if scale_mode.get_item_id(index) == 2 else "uniform")
	)
	scale_motion.item_selected.connect(func(index: int) -> void:
		if data != null and data.has_method("set_part_anim_channel_mode"):
			data.set_part_anim_channel_mode(_part_anim_lod_index, _part_anim_selected_index, "scale", "uniform", _mode_name_for_id(scale_motion.get_item_id(index)), -1)
			refresh_after_edit.call()
	)
	scale_from.value_changed.connect(func(_value: float) -> void: set_scale_values.call("uniform"))
	scale_to.value_changed.connect(func(_value: float) -> void: set_scale_values.call("uniform"))
	scale_speed.value_changed.connect(func(_value: float) -> void: set_scale_values.call("uniform"))
	translation_enabled.toggled.connect(func(value: bool) -> void:
		if data != null and data.has_method("set_part_anim_channel_enabled"):
			data.set_part_anim_channel_enabled(_part_anim_lod_index, _part_anim_selected_index, "translation", value)
			refresh_after_edit.call()
		update_visibility.call()
	)
	add_translation.pressed.connect(func() -> void:
		translation_enabled.button_pressed = true
		translation_enabled.toggled.emit(true)
	)
	remove_translation.pressed.connect(func() -> void:
		translation_enabled.button_pressed = false
		translation_enabled.toggled.emit(false)
	)
	var set_translation_values := func() -> void:
		if data != null and data.has_method("set_part_anim_channel_values"):
			data.set_part_anim_channel_values(_part_anim_lod_index, _part_anim_selected_index, "translation", _axis_name_for_id(translation_axis.get_selected_id()), float(translation_from.value), float(translation_to.value), float(translation_speed.value))
			refresh_after_edit.call()
	translation_axis.item_selected.connect(func(_index: int) -> void: set_translation_values.call())
	translation_mode.item_selected.connect(func(index: int) -> void:
		if data != null and data.has_method("set_part_anim_channel_mode"):
			data.set_part_anim_channel_mode(_part_anim_lod_index, _part_anim_selected_index, "translation", _axis_name_for_id(translation_axis.get_selected_id()), _mode_name_for_id(translation_mode.get_item_id(index)), translation_register.selected if translation_register != null else -1)
			refresh_after_edit.call()
		update_visibility.call()
	)
	if translation_register != null:
		translation_register.register_selected.connect(func(reg: int) -> void:
			if data != null and data.has_method("set_part_anim_channel_mode"):
				data.set_part_anim_channel_mode(_part_anim_lod_index, _part_anim_selected_index, "translation", _axis_name_for_id(translation_axis.get_selected_id()), _mode_name_for_id(translation_mode.get_selected_id()), reg)
				refresh_after_edit.call()
		)
	translation_from.value_changed.connect(func(_value: float) -> void: set_translation_values.call())
	translation_to.value_changed.connect(func(_value: float) -> void: set_translation_values.call())
	translation_speed.value_changed.connect(func(_value: float) -> void: set_translation_values.call())


func _make_inspector_box(host: Control) -> VBoxContainer:
	return ObjectUiHelpers.make_inspector_box(host)
